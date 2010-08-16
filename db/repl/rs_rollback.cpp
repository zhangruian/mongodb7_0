/* @file rs_rollback.cpp
* 
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../repl.h"
#include "../query.h"

/* Scenarios

   We went offline with ops not replicated out.
 
       F = node that failed and coming back.
       P = node that took over, new primary

   #1:
       F : a b c d e f g
       P : a b c d q

   The design is "keep P".  One could argue here that "keep F" has some merits, however, in most cases P 
   will have significantly more data.  Also note that P may have a proper subset of F's stream if there were 
   no subsequent writes.

   For now the model is simply : get F back in sync with P.  If P was really behind or something, we should have 
   just chosen not to fail over anyway.

   #2:
       F : a b c d e f g                -> a b c d
       P : a b c d

   #3:
       F : a b c d e f g                -> a b c d q r s t u v w x z
       P : a b c d.q r s t u v w x z

   Steps
    find an event in common. 'd'.
    undo our events beyond that by: 
      (1) taking copy from other server of those objects
      (2) do not consider copy valid until we pass reach an optime after when we fetched the new version of object 
          -- i.e., reset minvalid.
      (3) we could skip operations on objects that are previous in time to our capture of the object as an optimization.

*/

namespace mongo {

    using namespace bson;

    struct DocID {
        const char *ns;
        be _id;
        bool operator<(const DocID& d) const { 
            int c = strcmp(ns, d.ns);
            if( c < 0 ) return true;
            if( c > 0 ) return false;
            return _id < d._id;
        }
    };

    struct HowToFixUp {
        /* note this is a set -- if there are many $inc's on a single document we need to rollback, we only 
           need to refetch it once. */
        set<DocID> toRefetch;

        /* collections to drop */
        set<string> toDrop;

        OpTime commonPoint;
        DiskLoc commonPointOurDiskloc;

        int rbid; // remote server's current rollback sequence #
    };

    static void refetch(HowToFixUp& h, const BSONObj& ourObj) { 
        const char *op = ourObj.getStringField("op");
        if( *op == 'n' ) 
            return;

        unsigned long long totSize = 0;
        totSize += ourObj.objsize();
        if( totSize > 512 * 1024 * 1024 )
            throw "rollback too large";

        DocID d;
        d.ns = ourObj.getStringField("ns");
        if( *d.ns == 0 ) { 
            log() << "replSet WARNING ignoring op on rollback no ns TODO : " << ourObj.toString() << rsLog;
            return;
        }

        bo o = ourObj.getObjectField(*op=='u' ? "o2" : "o");
        if( o.isEmpty() ) { 
            log() << "replSet warning ignoring op on rollback : " << ourObj.toString() << rsLog;
            return;
        }

        if( *op == 'c' ) { 
            be first = o.firstElement();
            NamespaceString s(d.ns); // foo.$cmd

            Command *cmd = Command::findCommand( first.fieldName() );
            if( cmd == 0 ) { 
                log() << "replSet warning rollback no suchcommand " << first.fieldName() << " - different mongod versions perhaps?" << rsLog;
                return;
            }
            else {
                /* dropdatabase, drop, reindex, dropindexes, findandmodify, godinsert?,  renamecollection */
                if( string("create") == first.fieldName() ) {
                    /* Create collection operation 
                       { ts: ..., h: ..., op: "c", ns: "foo.$cmd", o: { create: "abc", ... } }
                    */
                    string ns = s.db + '.' + o["create"].String(); // -> foo.abc
                    h.toDrop.insert(ns);
                    return;
                }
                else { 
                    log() << "replSet WARNING can't roll back this command yet: " << o.toString() << rsLog;
                }
            }
        }

        d._id = o["_id"];
        if( d._id.eoo() ) {
            log() << "replSet WARNING ignoring op on rollback no _id TODO : " << d.ns << ' '<< ourObj.toString() << rsLog;
            return;
        }

        h.toRefetch.insert(d);
    }

    int getRBID(DBClientConnection*);

    static void syncRollbackFindCommonPoint(DBClientConnection *them, HowToFixUp& h) { 
        static time_t last;
        if( time(0)-last < 60 ) { 
            // this could put a lot of load on someone else, don't repeat too often
            sleepsecs(10);
            throw "findcommonpoint waiting a while before trying again";
        }
        last = time(0);

        assert( dbMutex.atLeastReadLocked() );
        Client::Context c(rsoplog, dbpath, 0, false);
        NamespaceDetails *nsd = nsdetails(rsoplog);
        assert(nsd);
        ReverseCappedCursor u(nsd);
        if( !u.ok() )
            throw "our oplog empty or unreadable";

        const Query q = Query().sort(reverseNaturalObj);
        const bo fields = BSON( "ts" << 1 << "h" << 1 );

        //auto_ptr<DBClientCursor> u = us->query(rsoplog, q, 0, 0, &fields, 0, 0);

        h.rbid = getRBID(them);
        auto_ptr<DBClientCursor> t = them->query(rsoplog, q, 0, 0, &fields, 0, 0);

        if( t.get() == 0 || !t->more() ) throw "remote oplog empty or unreadable";

        BSONObj ourObj = u.current();
        OpTime ourTime = ourObj["ts"]._opTime();
        BSONObj theirObj = t->nextSafe();
        OpTime theirTime = theirObj["ts"]._opTime();

        {
            long long diff = (long long) ourTime.getSecs() - ((long long) theirTime.getSecs());
            /* diff could be positive, negative, or zero */
            log() << "replSet info syncRollback our last optime:   " << ourTime.toStringPretty() << rsLog;
            log() << "replSet info syncRollback their last optime: " << theirTime.toStringPretty() << rsLog;
            log() << "replSet info syncRollback diff in end of log times: " << diff << " seconds" << rsLog;
            if( diff > 3600 ) { 
                log() << "replSet syncRollback too long a time period for a rollback." << rsLog;
                throw "error not willing to roll back more than one hour of data";
            }
        }

        unsigned long long scanned = 0;
        while( 1 ) {
            scanned++;
            /* todo add code to assure no excessive scanning for too long */
            if( ourTime == theirTime ) { 
                if( ourObj["h"].Long() == theirObj["h"].Long() ) { 
                    // found the point back in time where we match.
                    // todo : check a few more just to be careful about hash collisions.
                    log() << "replSet rollback found matching events at " << ourTime.toStringPretty() << rsLog;
                    log() << "replSet rollback findcommonpoint scanned : " << scanned << rsLog;
                    h.commonPoint = ourTime;
                    h.commonPointOurDiskloc = u.currLoc();
                    return;
                }

                refetch(h, ourObj);

                if( !t->more() ) { 
                    log() << "replSet error during rollback reached beginning of remote oplog? [2]" << rsLog;
                    log() << "replSet  them: " << them->toString() << " scanned: " << scanned << rsLog;
                    log() << "replSet  theirTime: " << theirTime.toStringPretty() << rsLog;
                    log() << "replSet  ourTime: " << ourTime.toStringPretty() << rsLog;
                    throw "reached beginning of remote oplog [2]";
                }
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();

                u.advance();
                if( !u.ok() ) throw "reached beginning of local oplog";
                ourObj = u.current();
                ourTime = ourObj["ts"]._opTime();
            }
            else if( theirTime > ourTime ) { 
                if( !t->more() ) { 
                    log() << "replSet error during rollback reached beginning of remote oplog?" << rsLog;
                    log() << "replSet  them: " << them->toString() << " scanned: " << scanned << rsLog;
                    log() << "replSet  theirTime: " << theirTime.toStringPretty() << rsLog;
                    log() << "replSet  ourTime: " << ourTime.toStringPretty() << rsLog;
                    throw "reached beginning of remote oplog [1]";
                }
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();
            }
            else { 
                // theirTime < ourTime
                refetch(h, ourObj);
                u.advance();
                if( !u.ok() ) throw "reached beginning of local oplog";
                ourObj = u.current();
                ourTime = ourObj["ts"]._opTime();
            }
        }
    }

    struct X { 
        const bson::bo *op;
        bson::bo goodVersionOfObject;
    };

   void ReplSetImpl::syncFixUp(HowToFixUp& h, OplogReader& r) {
       DBClientConnection *them = r.conn();

       // fetch all first so we needn't handle interruption in a fancy way

       unsigned long long totSize = 0;

       list< pair<DocID,bo> > goodVersions;

       bo newMinValid;

       DocID d;
       unsigned long long n = 0;
       try {
           for( set<DocID>::iterator i = h.toRefetch.begin(); i != h.toRefetch.end(); i++ ) { 
               d = *i;

               assert( !d._id.eoo() );

               {
                   /* TODO : slow.  lots of round trips. */
                   n++;
                   bo good= them->findOne(d.ns, d._id.wrap()).getOwned();
                   totSize += good.objsize();
                   uassert( 13410, "replSet too much data to roll back", totSize < 300 * 1024 * 1024 );

                   // note good might be eoo, indicating we should delete it
                   goodVersions.push_back(pair<DocID,bo>(d,good));
               }
           }
           newMinValid = r.getLastOp(rsoplog);
           if( newMinValid.isEmpty() ) { 
               sethbmsg("syncRollback error newMinValid empty?");
               return;
           }
       }
       catch(DBException& e) {
           sethbmsg(str::stream() << "syncRollback re-get objects: " << e.toString(),0);
           log() << "syncRollback couldn't re-get ns:" << d.ns << " _id:" << d._id << ' ' << n << '/' << h.toRefetch.size() << rsLog;
           throw e;
       }

       sethbmsg("syncRollback 3.5");
       if( h.rbid != getRBID(r.conn()) ) { 
           // our source rolled back itself.  so the data we received isn't necessarily consistent.
           sethbmsg("syncRollback rbid on source changed during rollback, cancelling this attempt");
           return;
       }

       // update them
       sethbmsg(str::stream() << "syncRollback 4 n:" << goodVersions.size());

       bool warn = false;

       assert( !h.commonPointOurDiskloc.isNull() );

       MemoryMappedFile::flushAll(true);

       dbMutex.assertWriteLocked();

       /* we have items we are writing that aren't from a point-in-time.  thus best not to come online 
	      until we get to that point in freshness. */
       try {
           log() << "replSet set minvalid=" << newMinValid["ts"]._opTime().toString() << rsLog;
       }
       catch(...){}
       Helpers::putSingleton("local.replset.minvalid", newMinValid);

       /** first drop collections to drop - that might make things faster below actually if there were subsequent inserts */
       for( set<string>::iterator i = h.toDrop.begin(); i != h.toDrop.end(); i++ ) { 
           Client::Context c(*i, dbpath, 0, /*doauth*/false);
           try {
               bob res;
               string errmsg;
               log(1) << "replSet rollback drop: " << *i << rsLog;
               dropCollection(*i, errmsg, res);
           }
           catch(...) { 
               log() << "replset rollback error dropping collection " << *i << rsLog;
           }
       }

       Client::Context c(rsoplog, dbpath, 0, /*doauth*/false);
       NamespaceDetails *oplogDetails = nsdetails(rsoplog);
       uassert(13423, str::stream() << "replSet error in rollback can't find " << rsoplog, oplogDetails);

       map<string,shared_ptr<RemoveSaver> > removeSavers;

       unsigned deletes = 0, updates = 0;
       for( list<pair<DocID,bo> >::iterator i = goodVersions.begin(); i != goodVersions.end(); i++ ) {
           const DocID& d = i->first;
           bo pattern = d._id.wrap(); // { _id : ... }
           try { 
               assert( d.ns && *d.ns );
               
               shared_ptr<RemoveSaver>& rs = removeSavers[d.ns];
               if ( ! rs )
                   rs.reset( new RemoveSaver( "rollback" , "" , d.ns ) );

               // todo: lots of overhead in context, this can be faster
               Client::Context c(d.ns, dbpath, 0, /*doauth*/false);
               if( i->second.isEmpty() ) {
                   // wasn't on the primary; delete.
                   /* TODO1.6 : can't delete from a capped collection.  need to handle that here. */
                   deletes++;

                   NamespaceDetails *nsd = nsdetails(d.ns);
                   if( nsd ) {
                       if( nsd->capped ) { 
                           /* can't delete from a capped collection - so we truncate instead. if this item must go, 
                           so must all successors!!! */
                           try { 
                               /** todo: IIRC cappedTrunateAfter does not handle completely empty.  todo. */
                               // this will crazy slow if no _id index.
                               long long start = Listener::getElapsedTimeMillis();
                               DiskLoc loc = Helpers::findOne(d.ns, pattern, false);
                               if( Listener::getElapsedTimeMillis() - start > 200 ) 
                                   log() << "replSet warning roll back slow no _id index for " << d.ns << rsLog; 
                               //would be faster but requires index: DiskLoc loc = Helpers::findById(nsd, pattern);
                               if( !loc.isNull() ) {
                                   try {
                                       nsd->cappedTruncateAfter(d.ns, loc, true);
                                   }
                                   catch(DBException& e) { 
                                       if( e.getCode() == 13415 ) {
                                           // hack: need to just make cappedTruncate do this...
                                           nsd->emptyCappedCollection(d.ns);
                                       } else {
                                           throw;
                                       }
                                   }
                               }
                           }
                           catch(DBException& e) { 
                               log() << "replSet error rolling back capped collection rec " << d.ns << ' ' << e.toString() << rsLog;
                           }
                       }
                       else {
                           try { 
                               deletes++;
                               deleteObjects(d.ns, pattern, /*justone*/true, /*logop*/false, /*god*/true, rs.get() );
                           }
                           catch(...) { 
                               log() << "replSet error rollback delete failed ns:" << d.ns << rsLog;
                           }
                       }
                       // did we just empty the collection?  if so let's check if it even exists on the source.
                       if( nsd->nrecords == 0 ) {
                           try { 
                               string sys = cc().database()->name + ".system.namespaces";
                               bo o = them->findOne(sys, QUERY("name"<<d.ns));
                               if( o.isEmpty() ) { 
                                   // we should drop
                                   try {
                                       bob res;
                                       string errmsg;
                                       dropCollection(d.ns, errmsg, res);
                                   }
                                   catch(...) { 
                                       log() << "replset error rolling back collection " << d.ns << rsLog;
                                   }
                               }
                           }
                           catch(DBException& ) { 
                               /* this isn't *that* big a deal, but is bad. */
                               log() << "replSet warning rollback error querying for existence of " << d.ns << " at the primary, ignoring" << rsLog;
                           }
                       }
                   }
               }
               else {
                   // todo faster...
                   OpDebug debug;
                   updates++;
                   _updateObjects(/*god*/true, d.ns, i->second, pattern, /*upsert=*/true, /*multi=*/false , /*logtheop=*/false , debug, rs.get() );
               }
           }
           catch(DBException& e) { 
               log() << "replSet exception in rollback ns:" << d.ns << ' ' << pattern.toString() << ' ' << e.toString() << " ndeletes:" << deletes << rsLog;
               warn = true;
           }
       }

       removeSavers.clear(); // this effectively closes all of them

       sethbmsg(str::stream() << "syncRollback 5 d:" << deletes << " u:" << updates);
       MemoryMappedFile::flushAll(true);
       sethbmsg("syncRollback 6");

       // clean up oplog
       log(2) << "replSet rollback truncate oplog after " << h.commonPoint.toStringPretty() << rsLog;
       // todo: fatal error if this throws?
       oplogDetails->cappedTruncateAfter(rsoplog, h.commonPointOurDiskloc, false);

       /* reset cached lastoptimewritten and h value */
       loadLastOpTimeWritten();

       sethbmsg("syncRollback 7");
       MemoryMappedFile::flushAll(true);

       // done
       if( warn ) 
           sethbmsg("issues during syncRollback, see log");
       else
           sethbmsg("syncRollback done");
   }

    void ReplSetImpl::syncRollback(OplogReader&r) { 
        assert( !lockedByMe() );
        assert( !dbMutex.atLeastReadLocked() );

        sethbmsg("syncRollback 0");

        writelocktry lk(rsoplog, 20000);
        if( !lk.got() ) {
            sethbmsg("syncRollback couldn't get write lock in a reasonable time");
            sleepsecs(2);
            return;
        }

        if( box.getState().secondary() ) {
            /* by doing this, we will not service reads (return an error as we aren't in secondary staate.
               that perhaps is moot becasue of the write lock above, but that write lock probably gets deferred 
               or removed or yielded later anyway.

               also, this is better for status reporting - we know what is happening.
               */
            box.change(MemberState::RS_ROLLBACK, _self);
        }

        HowToFixUp how;
        sethbmsg("syncRollback 1");
        {
            r.resetCursor();
            /*DBClientConnection us(false, 0, 0);
            string errmsg;
            if( !us.connect(HostAndPort::me().toString(),errmsg) ) { 
                sethbmsg("syncRollback connect to self failure" + errmsg);
                return;
            }*/

            sethbmsg("syncRollback 2 FindCommonPoint");
            try {
                syncRollbackFindCommonPoint(r.conn(), how);
            }
            catch( const char *p ) { 
                sethbmsg(string("syncRollback 2 error ") + p);
                sleepsecs(10);
                return;
            }
            catch( DBException& e ) { 
                sethbmsg(string("syncRollback 2 exception ") + e.toString() + "; sleeping 1 min");
                sleepsecs(60);
                throw;
            }
        }

        sethbmsg("replSet syncRollback 3 fixup");

        syncFixUp(how, r);
    }

}

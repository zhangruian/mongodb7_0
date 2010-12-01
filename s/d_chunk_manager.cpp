// @file d_chunk_manager.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

#include "../client/connpool.h"
#include "../client/dbclientmockcursor.h"
#include "../db/instance.h"

#include "d_chunk_manager.h"

namespace mongo {

    ShardChunkManager::ShardChunkManager( const string& configServer , const string& ns , const string& shardName ) { 

        // have to get a connection to the config db
        // special case if i'm the configdb since i'm locked and if i connect to myself
        // its a deadlock
        auto_ptr<ScopedDbConnection> scoped;
        auto_ptr<DBDirectClient> direct;
        DBClientBase * conn;
        if ( configServer.empty() ){
            direct.reset( new DBDirectClient() );
            conn = direct.get();
        } else {
            scoped.reset( new ScopedDbConnection( configServer ) );
            conn = scoped->get();
        }

        // get this collection's sharding key 
        BSONObj collectionDoc = conn->findOne( "config.collections", BSON( "_id" << ns ) );
        uassert( 13539 , str::stream() << ns << " does not exist" , !collectionDoc.isEmpty() );
        uassert( 13540 , str::stream() << ns << " collection config entry corrupted" , collectionDoc["dropped"].type() );
        uassert( 13541 , str::stream() << ns << " dropped. Re-shard collection first." , !collectionDoc["dropped"].Bool() );
        _fillCollectionKey( collectionDoc );

        // query for all the chunks for 'ns' that live in this shard, sorting so we can efficiently bucket them
        BSONObj q = BSON( "ns" << ns << "shard" << shardName );
        auto_ptr<DBClientCursor> cursor = conn->query( "config.chunks" , Query(q).sort( "min" ) );
        _fillChunks( cursor.get() );
        _fillRanges();

        if ( scoped.get() )
            scoped->done();

        if ( _chunksMap.empty() )
            log() << "no chunk for collection " << ns << " on shard " << shardName << endl;
    }

    ShardChunkManager::ShardChunkManager( const BSONObj& collectionDoc , const BSONArray& chunksArr ) {
        _fillCollectionKey( collectionDoc );

        scoped_ptr<DBClientMockCursor> c ( new DBClientMockCursor( chunksArr ) );
        _fillChunks( c.get() );
        _fillRanges();
    }

    void ShardChunkManager::_fillCollectionKey( const BSONObj& collectionDoc ) {
        BSONElement e = collectionDoc["key"];
        uassert( 13542 , str::stream() << "collection doesn't have a key: " << collectionDoc , ! e.eoo() && e.isABSONObj() );

        BSONObj keys = e.Obj().getOwned();
        BSONObjBuilder b;
        BSONForEach( key , keys ) {
            b.append( key.fieldName() , 1 );
        }
        _key = b.obj();
    }        

    void ShardChunkManager::_fillChunks( DBClientCursorInterface* cursor ) {
        assert( cursor );

        ShardChunkVersion version;
        while ( cursor->more() ){
            BSONObj d = cursor->next();
            _chunksMap.insert( make_pair( d["min"].Obj().getOwned() , d["max"].Obj().getOwned() ) );

            ShardChunkVersion currVersion( d["lastmod"] );
            if ( currVersion > version ) {
                version = currVersion;
            }
        }
        _version = version;
    }

    void ShardChunkManager::_fillRanges() {
        if ( _chunksMap.empty() )
            return;

        // load the chunk information, coallesceing their ranges
        // the version for this shard would be the highest version for any of the chunks
        RangeMap::const_iterator it = _chunksMap.begin();
        BSONObj min,max;
        while ( it != _chunksMap.end() ){
            BSONObj currMin = it->first;
            BSONObj currMax = it->second;
            ++it;

            // coallesce the chunk's bounds in ranges if they are adjacent chunks 
            if ( min.isEmpty() ){
                min = currMin;
                max = currMax;
                continue;
            }
            if ( max == currMin ) {
                max = currMax;
                continue;
            }

            _rangesMap.insert( make_pair( min , max ) );

            min = currMin;
            max = currMax;
        }
        assert( ! min.isEmpty() );
        
        _rangesMap.insert( make_pair( min , max ) );
    }

    bool ShardChunkManager::belongsToMe( const BSONObj& obj ) const {
        if ( _rangesMap.size() == 0 )
            return false;

        BSONObj x = obj.extractFields(_key);

        RangeMap::const_iterator a = _rangesMap.upper_bound( x );
        if ( a != _rangesMap.begin() )
            a--;
        
        bool good = x.woCompare( a->first ) >= 0 && x.woCompare( a->second ) < 0;

        #if 0
        if ( ! good ){
            log() << "bad: " << x << " " << a->first << " " << x.woCompare( a->first ) << " " << x.woCompare( a->second ) << endl;
            for ( RangeMap::const_iterator i=_rangesMap.begin(); i!=_rangesMap.end(); ++i ){
                log() << "\t" << i->first << "\t" << i->second << "\t" << endl;
            }
        }
        #endif

        return good;
    }

    ShardChunkManager* ShardChunkManager::cloneMinus( const BSONObj& min, const BSONObj& max, const ShardChunkVersion& version ) {

        // can't move version backwards when subtracting chunks
        uassert( 13585 , str::stream() << "version " << version << "not greater than " << _version , version > _version ); 

        // check that we have the exact chunk that'll be subtracted
        RangeMap::const_iterator it = _chunksMap.find( min );
        if ( it == _chunksMap.end() ) {
            uasserted( 13586 , str::stream() << "couldn't find chunk " << min << "->" << max );
        }

        if ( it->second.woCompare( max ) != 0 ) {
            ostringstream os;
            os << "ranges differ, "
               << "requested: "  << min << " -> " << max << " " 
               << "existing: " << (it == _chunksMap.end()) ? "<empty>" : it->first.toString() + " -> " + it->second.toString();
            uasserted( 13587 , os.str() );
        }

        auto_ptr<ShardChunkManager> p( new ShardChunkManager );

        p->_key = this->_key;
        p->_chunksMap = this->_chunksMap;
        p->_chunksMap.erase( min );
        p->_version = version;
        p->_fillRanges();

        // TODO handle empty state, as in when the last chunk was cloned out

        return p.release();
    }
 
    static bool overlap( const BSONObj& l1 , const BSONObj& h1 , const BSONObj& l2 , const BSONObj& h2 ) {
        return ! ( ( h1.woCompare( l2 ) <= 0 ) || ( h2.woCompare( l1 ) <= 0 ) );
    }
    
    ShardChunkManager* ShardChunkManager::clonePlus( const BSONObj& min , const BSONObj& max , const ShardChunkVersion& version ) {

        // TODO handle empty state, as in when the first chunk is cloned in

        // check that there isn't any chunk on the interval to be added
        RangeMap::const_iterator it = _chunksMap.lower_bound( max );
        if ( it != _chunksMap.begin() ) {
            --it;
        }
        if ( overlap( min , max , it->first , it->second ) ) {
            ostringstream os;
            os << "ranges overlap, "
               << "requested: " << min << " -> " << max << " " 
               << "existing: " << it->first.toString() + " -> " + it->second.toString();
            uasserted( 13588 , os.str() );
        }
        
        auto_ptr<ShardChunkManager> p( new ShardChunkManager );

        p->_key = this->_key;
        p->_chunksMap = this->_chunksMap;
        p->_chunksMap.insert( make_pair( min.getOwned() , max.getOwned() ) );
        p->_version = version;
        p->_fillRanges();

        return p.release();
    }

}  // namespace mongo

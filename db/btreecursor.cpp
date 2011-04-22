// btreecursor.cpp

/**
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
#include "btree.h"
#include "pdfile.h"
#include "jsobj.h"
#include "curop-inl.h"

namespace mongo {

    extern int otherTraceLevel;

    template< class V >
    class BtreeCursorImpl : public BtreeCursor { 
    public:
        typename typedef BucketBasics<V>::KeyNode KeyNode;
        typename typedef V::Key Key;

        BtreeCursorImpl(NamespaceDetails *a, int b, const IndexDetails& c, const BSONObj &d, const BSONObj &e, bool f, int g) : 
          BtreeCursor(a,b,c,d,e,f,g) { }
        BtreeCursorImpl(NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction) : 
          BtreeCursor(_d,_idxNo,_id,_bounds,_direction) 
        { 
            pair< DiskLoc, int > noBestParent;
            indexDetails.head.btree<V>()->customLocate( bucket, keyOfs, startKey, 0, false, _boundsIterator->cmp(), _boundsIterator->inc(), _ordering, _direction, noBestParent );
            skipAndCheck();
            dassert( _dups.size() == 0 );
        }

        virtual DiskLoc currLoc() { 
            if( bucket.isNull() ) return DiskLoc();
            return currKeyNode().recordLoc;
        }

        virtual BSONObj currKey() const { 
            assert( !bucket.isNull() );
            return bucket.btree<V>()->keyNode(keyOfs).key.toBson();
        }

    protected:
        virtual void _advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) {
            thisLoc.btree<V>()->advanceTo(thisLoc, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction);
        }
        virtual DiskLoc _advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) {
            return thisLoc.btree<V>()->advance(thisLoc, keyOfs, direction, caller);
        }
        virtual void _audit() {
            if ( otherTraceLevel >= 200 ) {
                out() << "BtreeCursor() qtl>200.  validating entire index." << endl;
                indexDetails.head.btree<V>()->fullValidate(indexDetails.head, _order);
            }
            else {
                out() << "BtreeCursor(). dumping head bucket" << endl;
                indexDetails.head.btree<V>()->dump();
            }
        }
        virtual DiskLoc _locate(const BSONObj& key, const DiskLoc& loc) {
            bool found;
            return indexDetails.head.btree<V0>()->
                     locate(indexDetails, indexDetails.head, key, _ordering, keyOfs, found, loc, _direction);
        }

        const _KeyNode& keyNode(int keyOfs) { 
            return bucket.btree<V>()->k(keyOfs);
        }

    private:
        const KeyNode currKeyNode() const {
            assert( !bucket.isNull() );
            const BtreeBucket<V> *b = bucket.btree<V>();
            return b->keyNode(keyOfs);
        }
    };

    template class BtreeCursorImpl<V0>;
    template class BtreeCursorImpl<V1>;

    /*
    class BtreeCursorV1 : public BtreeCursor { 
    public:
        typedef BucketBasics<V1>::KeyNode KeyNode;
        typedef V1::Key Key;

        BtreeCursorV1(NamespaceDetails *a, int b, const IndexDetails& c, const BSONObj &d, const BSONObj &e, bool f, int g) : 
          BtreeCursor(a,b,c,d,e,f,g) { }
        BtreeCursorV1(NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction) : 
          BtreeCursor(_d,_idxNo,_id,_bounds,_direction) 
        { 
            pair< DiskLoc, int > noBestParent;
            indexDetails.head.btree<V1>()->customLocate( bucket, keyOfs, startKey, 0, false, _boundsIterator->cmp(), _boundsIterator->inc(), _ordering, _direction, noBestParent );
            skipAndCheck();
            dassert( _dups.size() == 0 );
        }

        virtual DiskLoc currLoc() { 
            if( bucket.isNull() ) return DiskLoc();
            return currKeyNode().recordLoc;
        }

        virtual BSONObj currKey() const { 
            assert( !bucket.isNull() );
            return bucket.btree<V1>()->keyNode(keyOfs).key.toBson();
        }

    protected:
        virtual void _advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) {
            thisLoc.btree<V1>()->advanceTo(thisLoc, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction);
        }
        virtual DiskLoc _advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) {
            return thisLoc.btree<V1>()->advance(thisLoc, keyOfs, direction, caller);
        }
        virtual void _audit() {
            if ( otherTraceLevel >= 200 ) {
                out() << "BtreeCursor() qtl>200.  validating entire index." << endl;
                indexDetails.head.btree<V1>()->fullValidate(indexDetails.head, _order);
            }
            else {
                out() << "BtreeCursor(). dumping head bucket" << endl;
                indexDetails.head.btree<V1>()->dump();
            }
        }
        virtual DiskLoc _locate(const BSONObj& key, const DiskLoc& loc);
        virtual const _KeyNode& keyNode(int keyOfs) { 
            return bucket.btree<V1>()->k(keyOfs);
        }

    private:
        const KeyNode currKeyNode() const {
            assert( !bucket.isNull() );
            const BtreeBucket<V1> *b = bucket.btree<V1>();
            return b->keyNode(keyOfs);
        }
    };*/

    BtreeCursor* BtreeCursor::make(
        NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, 
        const BSONObj &startKey, const BSONObj &endKey, bool endKeyInclusive, int direction) 
    { 
        int v = _id.version();
        if( v == 1 )
            return new BtreeCursorImpl<V1>(_d,_idxNo,_id,startKey,endKey,endKeyInclusive,direction);
        if( v == 0 )
            return new BtreeCursorImpl<V0>(_d,_idxNo,_id,startKey,endKey,endKeyInclusive,direction);
        uasserted(14800, str::stream() << "unsupported index version " << v);
        return 0;
    }

    BtreeCursor* BtreeCursor::make(
        NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, 
        const shared_ptr< FieldRangeVector > &_bounds, int _direction )
    {
        int v = _id.version();
        if( v == 1 )
            return new BtreeCursorImpl<V1>(_d,_idxNo,_id,_bounds,_direction);
        if( v == 0 )
            return new BtreeCursorImpl<V0>(_d,_idxNo,_id,_bounds,_direction);
        uasserted(14801, str::stream() << "unsupported index version " << v);
        return 0;
    }

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails &_id,
                              const BSONObj &_startKey, const BSONObj &_endKey, bool endKeyInclusive, int _direction ) :
        d(_d), idxNo(_idxNo),
        startKey( _startKey ),
        endKey( _endKey ),
        _endKeyInclusive( endKeyInclusive ),
        _multikey( d->isMultikey( idxNo ) ),
        indexDetails( _id ),
        _order( _id.keyPattern() ),
        _ordering( Ordering::make( _order ) ),
        _direction( _direction ),
        _spec( _id.getSpec() ),
        _independentFieldRanges( false ),
        _nscanned( 0 ) {
        audit();
        init();
        dassert( _dups.size() == 0 );
    }

    BtreeCursor::BtreeCursor( NamespaceDetails *_d, int _idxNo, const IndexDetails& _id, const shared_ptr< FieldRangeVector > &_bounds, int _direction )
        :
        d(_d), idxNo(_idxNo),
        _endKeyInclusive( true ),
        _multikey( d->isMultikey( idxNo ) ),
        indexDetails( _id ),
        _order( _id.keyPattern() ),
        _ordering( Ordering::make( _order ) ),
        _direction( _direction ),
        _bounds( ( assert( _bounds.get() ), _bounds ) ),
        _boundsIterator( new FieldRangeVector::Iterator( *_bounds  ) ),
        _spec( _id.getSpec() ),
        _independentFieldRanges( true ),
        _nscanned( 0 ) {
        massert( 13384, "BtreeCursor FieldRangeVector constructor doesn't accept special indexes", !_spec.getType() );
        audit();
        startKey = _bounds->startKey();
        _boundsIterator->advance( startKey ); // handles initialization
        _boundsIterator->prepDive();
        bucket = indexDetails.head;
        keyOfs = 0;
    }

    void BtreeCursor::audit() {
        dassert( d->idxNo((IndexDetails&) indexDetails) == idxNo );
        if ( otherTraceLevel >= 12 ) {
            _audit();
        }
    }

    void BtreeCursor::init() {
        if ( _spec.getType() ) {
            startKey = _spec.getType()->fixKey( startKey );
            endKey = _spec.getType()->fixKey( endKey );
        }
        bucket = _locate(startKey, _direction > 0 ? minDiskLoc : maxDiskLoc);
        if ( ok() ) {
            _nscanned = 1;
        }
        skipUnusedKeys( false );
        checkEnd();
    }

    void BtreeCursor::skipAndCheck() {
        skipUnusedKeys( true );
        while( 1 ) {
            if ( !skipOutOfRangeKeysAndCheckEnd() ) {
                break;
            }
            while( skipOutOfRangeKeysAndCheckEnd() );
            if ( !skipUnusedKeys( true ) ) {
                break;
            }
        }
    }

    bool BtreeCursor::skipOutOfRangeKeysAndCheckEnd() {
        if ( !ok() ) {
            return false;
        }
        int ret = _boundsIterator->advance( currKey() );
        if ( ret == -2 ) {
            bucket = DiskLoc();
            return false;
        }
        else if ( ret == -1 ) {
            ++_nscanned;
            return false;
        }
        ++_nscanned;
        advanceTo( currKey(), ret, _boundsIterator->after(), _boundsIterator->cmp(), _boundsIterator->inc() );
        return true;
    }

    /* skip unused keys. */
    bool BtreeCursor::skipUnusedKeys( bool mayJump ) {
        int u = 0;
        while ( 1 ) {
            if ( !ok() )
                break;
            const _KeyNode& kn = keyNode(keyOfs);
            if ( kn.isUsed() )
                break;
            bucket = _advance(bucket, keyOfs, _direction, "skipUnusedKeys");
            u++;
            //don't include unused keys in nscanned
            //++_nscanned;
            if ( mayJump && ( u % 10 == 0 ) ) {
                skipOutOfRangeKeysAndCheckEnd();
            }
        }
        if ( u > 10 )
            OCCASIONALLY log() << "btree unused skipped:" << u << '\n';
        return u;
    }

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
    int sgn( int i ) {
        if ( i == 0 )
            return 0;
        return i > 0 ? 1 : -1;
    }

    // Check if the current key is beyond endKey.
    void BtreeCursor::checkEnd() {
        if ( bucket.isNull() )
            return;
        if ( !endKey.isEmpty() ) {
            int cmp = sgn( endKey.woCompare( currKey(), _order ) );
            if ( ( cmp != 0 && cmp != _direction ) ||
                    ( cmp == 0 && !_endKeyInclusive ) )
                bucket = DiskLoc();
        }
    }

    void BtreeCursor::advanceTo( const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive) {
        _advanceTo( bucket, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, _ordering, _direction );
    }

    bool BtreeCursor::advance() {
        killCurrentOp.checkForInterrupt();
        if ( bucket.isNull() )
            return false;

        bucket = _advance(bucket, keyOfs, _direction, "BtreeCursor::advance");

        if ( !_independentFieldRanges ) {
            skipUnusedKeys( false );
            checkEnd();
            if ( ok() ) {
                ++_nscanned;
            }
        }
        else {
            skipAndCheck();
        }
        return ok();
    }

    void BtreeCursor::noteLocation() {
        if ( !eof() ) {
            BSONObj o = currKey().getOwned();
            keyAtKeyOfs = o;
            locAtKeyOfs = currLoc();
        }
    }

    /* Since the last noteLocation(), our key may have moved around, and that old cached
       information may thus be stale and wrong (although often it is right).  We check
       that here; if we have moved, we have to search back for where we were at.

       i.e., after operations on the index, the BtreeCursor's cached location info may
       be invalid.  This function ensures validity, so you should call it before using
       the cursor if other writers have used the database since the last noteLocation
       call.
    */
    void BtreeCursor::checkLocation() {
        if ( eof() )
            return;

        _multikey = d->isMultikey(idxNo);

        BSONObj _keyAtKeyOfs(keyAtKeyOfs);

        if ( keyOfs >= 0 ) {
            assert( !keyAtKeyOfs.isEmpty() );

            // Note keyAt() returns an empty BSONObj if keyOfs is now out of range,
            // which is possible as keys may have been deleted.
            int x = 0;
            while( 1 ) {
                if( currKey().woEqual(keyAtKeyOfs) && currLoc() == locAtKeyOfs ) {

                    if ( keyNode(keyOfs).isUsed() ) {
                        /* we were deleted but still exist as an unused
                        marker key. advance.
                        */
                        skipUnusedKeys( false );
                    }
                    return;
                }

                /* we check one key earlier too, in case a key was just deleted.  this is
                   important so that multi updates are reasonably fast.
                   */
                if( keyOfs == 0 || x++ )
                    break;
                keyOfs--;
            }
        }

        /* normally we don't get to here.  when we do, old position is no longer
            valid and we must refind where we left off (which is expensive)
        */

        /* TODO: Switch to keep indexdetails and do idx.head! */
        bucket = _locate(_keyAtKeyOfs, locAtKeyOfs);
        RARELY log() << "key seems to have moved in the index, refinding. " << bucket.toString() << endl;
        if ( ! bucket.isNull() )
            skipUnusedKeys( false );

    }

    /* ----------------------------------------------------------------------------- */

    struct BtreeCursorUnitTest {
        BtreeCursorUnitTest() {
            assert( minDiskLoc.compare(maxDiskLoc) < 0 );
        }
    } btut;

} // namespace mongo

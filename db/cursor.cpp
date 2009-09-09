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

#include "stdafx.h"
#include "pdfile.h"

namespace mongo {

    class Forward : public AdvanceStrategy {
        virtual DiskLoc next( const DiskLoc &prev ) const {
            return prev.rec()->getNext( prev );
        }
    } _forward;

    class Reverse : public AdvanceStrategy {
        virtual DiskLoc next( const DiskLoc &prev ) const {
            return prev.rec()->getPrev( prev );
        }
    } _reverse;

    AdvanceStrategy *forward() {
        return &_forward;
    }
    AdvanceStrategy *reverse() {
        return &_reverse;
    }

    DiskLoc nextLoop( NamespaceDetails *nsd, const DiskLoc &prev ) {
        assert( nsd->capLooped() );
        DiskLoc next = forward()->next( prev );
        if ( !next.isNull() )
            return next;
        return nsd->firstRecord();
    }

    DiskLoc prevLoop( NamespaceDetails *nsd, const DiskLoc &curr ) {
        assert( nsd->capLooped() );
        DiskLoc prev = reverse()->next( curr );
        if ( !prev.isNull() )
            return prev;
        return nsd->lastRecord();
    }

    ForwardCappedCursor::ForwardCappedCursor( NamespaceDetails *_nsd, const DiskLoc &startLoc ) :
            nsd( _nsd ) {
        if ( !nsd )
            return;
        DiskLoc start = startLoc;
        if ( start.isNull() ) {
            if ( !nsd->capLooped() )
                start = nsd->firstRecord();
            else {
                start = nsd->capExtent.ext()->firstRecord;
                if ( !start.isNull() && start == nsd->capFirstNewRecord ) {
                    start = nsd->capExtent.ext()->lastRecord;
                    start = nextLoop( nsd, start );
                }
            }
        }
        curr = start;
        s = this;
    }

    DiskLoc ForwardCappedCursor::next( const DiskLoc &prev ) const {
        assert( nsd );
        if ( !nsd->capLooped() )
            return forward()->next( prev );

        DiskLoc i = prev;
        // Last record
        if ( i == nsd->capExtent.ext()->lastRecord )
            return DiskLoc();
        i = nextLoop( nsd, i );
        // If we become capFirstNewRecord from same extent, advance to next extent.
        if ( i == nsd->capFirstNewRecord &&
                i != nsd->capExtent.ext()->firstRecord )
            i = nextLoop( nsd, nsd->capExtent.ext()->lastRecord );
        // If we have just gotten to beginning of capExtent, skip to capFirstNewRecord
        if ( i == nsd->capExtent.ext()->firstRecord )
            i = nsd->capFirstNewRecord;
        return i;
    }

    ReverseCappedCursor::ReverseCappedCursor( NamespaceDetails *_nsd, const DiskLoc &startLoc ) :
            nsd( _nsd ) {
        if ( !nsd )
            return;
        DiskLoc start = startLoc;
        if ( start.isNull() ) {
            if ( !nsd->capLooped() ) {
                start = nsd->lastRecord();
            } else {
                start = nsd->capExtent.ext()->lastRecord;
            }
        }
        curr = start;
        s = this;
    }

    DiskLoc ReverseCappedCursor::next( const DiskLoc &prev ) const {
        assert( nsd );
        if ( !nsd->capLooped() )
            return reverse()->next( prev );

        DiskLoc i = prev;
        // Last record
        if ( nsd->capFirstNewRecord == nsd->capExtent.ext()->firstRecord ) {
            if ( i == nextLoop( nsd, nsd->capExtent.ext()->lastRecord ) ) {
                return DiskLoc();
            }
        } else {
            if ( i == nsd->capExtent.ext()->firstRecord ) {
                return DiskLoc();
            }
        }
        // If we are capFirstNewRecord, advance to prev extent, otherwise just get prev.
        if ( i == nsd->capFirstNewRecord )
            i = prevLoop( nsd, nsd->capExtent.ext()->firstRecord );
        else
            i = prevLoop( nsd, i );
        // If we just became last in cap extent, advance past capFirstNewRecord
        // (We know capExtent.ext()->firstRecord != capFirstNewRecord, since would
        // have returned DiskLoc() earlier otherwise.)
        if ( i == nsd->capExtent.ext()->lastRecord )
            i = reverse()->next( nsd->capFirstNewRecord );

        return i;
    }
} // namespace mongo

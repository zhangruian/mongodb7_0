// queryutil.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "stdafx.h"

#include "btree.h"
#include "matcher.h"
#include "pdfile.h"
#include "queryoptimizer.h"

namespace mongo {
    
    FieldRange::FieldRange( const BSONElement &e, bool optimize ) {
        if ( !e.eoo() && e.type() != RegEx && e.getGtLtOp() == BSONObj::opIN ) {
            set< BSONElement, element_lt > vals;
            BSONObjIterator i( e.embeddedObject() );
            while( i.more() )
                vals.insert( i.next() );

            for( set< BSONElement, element_lt >::const_iterator i = vals.begin(); i != vals.end(); ++i )
                intervals_.push_back( FieldInterval(*i) );

            return;
        }
        
        if ( e.type() == Array && e.getGtLtOp() == BSONObj::Equality ){
            
            intervals_.push_back( FieldInterval(e) );
            
            const BSONElement& temp = e.embeddedObject().firstElement();
            if ( ! temp.eoo() ){
                if ( temp < e )
                    intervals_.insert( intervals_.begin() , temp );
                else
                    intervals_.push_back( FieldInterval(temp) );
            }
            
            return;
        }

        intervals_.push_back( FieldInterval() );
        FieldInterval &initial = intervals_[ 0 ];
        BSONElement &lower = initial.lower_.bound_;
        bool &lowerInclusive = initial.lower_.inclusive_;
        BSONElement &upper = initial.upper_.bound_;
        bool &upperInclusive = initial.upper_.inclusive_;
        lower = minKey.firstElement();
        lowerInclusive = true;
        upper = maxKey.firstElement();
        upperInclusive = true;

        if ( e.eoo() )
            return;
        if ( e.type() == RegEx ) {
            const string r = e.simpleRegex();
            if ( r.size() ) {
                lower = addObj( BSON( "" << r ) ).firstElement();
                upper = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
                upperInclusive = false;
            }            
            return;
        }
        switch( e.getGtLtOp() ) {
        case BSONObj::Equality:
            lower = upper = e;
            break;
        case BSONObj::LT:
            upperInclusive = false;
        case BSONObj::LTE:
            upper = e;
            break;
        case BSONObj::GT:
            lowerInclusive = false;
        case BSONObj::GTE:
            lower = e;
            break;
        case BSONObj::opALL: {
            massert( "$all requires array", e.type() == Array );
            BSONObjIterator i( e.embeddedObject() );
            if ( i.more() )
                lower = upper = i.next();
            break;
        }
        case BSONObj::opMOD: {
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , NumberDouble );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , NumberDouble );
                upper = addObj( b.obj() ).firstElement();
            }            
            break;
        }
        case BSONObj::opTYPE: {
            BSONType t = (BSONType)e.numberInt();
            {
                BSONObjBuilder b;
                b.appendMinForType( "" , t );
                lower = addObj( b.obj() ).firstElement();
            }
            {
                BSONObjBuilder b;
                b.appendMaxForType( "" , t );
                upper = addObj( b.obj() ).firstElement();
            }
            
            break;
        }
        default:
            break;
        }
        
        if ( optimize ){
            if ( lower.type() != MinKey && upper.type() == MaxKey && lower.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMaxForType( lower.fieldName() , lower.type() );
                upper = addObj( b.obj() ).firstElement();
            }
            else if ( lower.type() == MinKey && upper.type() != MaxKey && upper.isSimpleType() ){ // TODO: get rid of isSimpleType
                BSONObjBuilder b;
                b.appendMinForType( upper.fieldName() , upper.type() );
                lower = addObj( b.obj() ).firstElement();
            }
        }

    }

    // as called, these functions find the max/min of a bound in the
    // opposite direction, so inclusive bounds are considered less
    // superlative
    FieldBound maxFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a.bound_.woCompare( b.bound_, false );
        if ( ( cmp == 0 && !b.inclusive_ ) || cmp < 0 )
            return b;
        return a;
    }

    FieldBound minFieldBound( const FieldBound &a, const FieldBound &b ) {
        int cmp = a.bound_.woCompare( b.bound_, false );
        if ( ( cmp == 0 && !b.inclusive_ ) || cmp > 0 )
            return b;
        return a;
    }

    bool fieldIntervalOverlap( const FieldInterval &one, const FieldInterval &two, FieldInterval &result ) {
        result.lower_ = maxFieldBound( one.lower_, two.lower_ );
        result.upper_ = minFieldBound( one.upper_, two.upper_ );
        return result.valid();
    }
    
	// NOTE Not yet tested for complex $or bounds, just for simple bounds generated by $in
    const FieldRange &FieldRange::operator&=( const FieldRange &other ) {
        vector< FieldInterval > newIntervals;
        vector< FieldInterval >::const_iterator i = intervals_.begin();
        vector< FieldInterval >::const_iterator j = other.intervals_.begin();
        while( i != intervals_.end() && j != other.intervals_.end() ) {
            FieldInterval overlap;
            if ( fieldIntervalOverlap( *i, *j, overlap ) )
                newIntervals.push_back( overlap );
            if ( i->upper_ == minFieldBound( i->upper_, j->upper_ ) )
                ++i;
            else
                ++j;      
        }
        intervals_ = newIntervals;
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        return *this;
    }
    
    string FieldRange::simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    BSONObj FieldRange::addObj( const BSONObj &o ) {
        objData_.push_back( o );
        return o;
    }
    
    FieldRangeSet::FieldRangeSet( const char *ns, const BSONObj &query , bool optimize ) :
    ns_( ns ),
    query_( query.getOwned() ) {
        BSONObjIterator i( query_ );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( strcmp( e.fieldName(), "$where" ) == 0 )
                continue;
            if ( getGtLtOp( e ) == BSONObj::Equality ) {
                ranges_[ e.fieldName() ] &= FieldRange( e , optimize );
            }
            else {
                BSONObjIterator i( e.embeddedObject() );
                while( i.moreWithEOO() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    ranges_[ e.fieldName() ] &= FieldRange( f , optimize );
                }                
            }
        }
    }
    
    FieldRange *FieldRangeSet::trivialRange_ = 0;
    FieldRange &FieldRangeSet::trivialRange() {
        if ( trivialRange_ == 0 )
            trivialRange_ = new FieldRange();
        return *trivialRange_;
    }
    
    BSONObj FieldRangeSet::simplifiedQuery( const BSONObj &_fields ) const {
        BSONObj fields = _fields;
        if ( fields.isEmpty() ) {
            BSONObjBuilder b;
            for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i ) {
                b.append( i->first.c_str(), 1 );
            }
            fields = b.obj();
        }
        BSONObjBuilder b;
        BSONObjIterator i( fields );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const char *name = e.fieldName();
            const FieldRange &range = ranges_[ name ];
            assert( !range.empty() );
            if ( range.equality() )
                b.appendAs( range.min(), name );
            else if ( range.nontrivial() ) {
                BSONObjBuilder c;
                if ( range.min().type() != MinKey )
                    c.appendAs( range.min(), range.minInclusive() ? "$gte" : "$gt" );
                if ( range.max().type() != MaxKey )
                    c.appendAs( range.max(), range.maxInclusive() ? "$lte" : "$lt" );
                b.append( name, c.done() );                
            }
        }
        return b.obj();
    }
    
    QueryPattern FieldRangeSet::pattern( const BSONObj &sort ) const {
        QueryPattern qp;
        for( map< string, FieldRange >::const_iterator i = ranges_.begin(); i != ranges_.end(); ++i ) {
            assert( !i->second.empty() );
            if ( i->second.equality() ) {
                qp.fieldTypes_[ i->first ] = QueryPattern::Equality;
            } else if ( i->second.nontrivial() ) {
                bool upper = i->second.max().type() != MaxKey;
                bool lower = i->second.min().type() != MinKey;
                if ( upper && lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperAndLowerBound;
                else if ( upper )
                    qp.fieldTypes_[ i->first ] = QueryPattern::UpperBound;
                else if ( lower )
                    qp.fieldTypes_[ i->first ] = QueryPattern::LowerBound;                    
            }
        }
        qp.setSort( sort );
        return qp;
    }
    
    BoundList FieldRangeSet::indexBounds( const BSONObj &keyPattern, int direction ) const {
        BSONObjBuilder equalityBuilder;
        typedef vector< pair< shared_ptr< BSONObjBuilder >, shared_ptr< BSONObjBuilder > > > BoundBuilders;
        BoundBuilders builders;
        BSONObjIterator i( keyPattern );
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange &fr = range( e.fieldName() );
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( ( number >= 0 ? 1 : -1 ) * ( direction >= 0 ? 1 : -1 ) > 0 );
            if ( builders.empty() ) {
                if ( fr.equality() ) {
                    equalityBuilder.appendAs( fr.min(), "" );
                } else {
                    BSONObj equalityObj = equalityBuilder.done();
                    const vector< FieldInterval > &intervals = fr.intervals();
                    if ( forward ) {
                        for( vector< FieldInterval >::const_iterator j = intervals.begin(); j != intervals.end(); ++j ) {
                            builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                            builders.back().first->appendElements( equalityObj );
                            builders.back().second->appendElements( equalityObj );
                            builders.back().first->appendAs( j->lower_.bound_, "" );
                            builders.back().second->appendAs( j->upper_.bound_, "" );
                        }
                    } else {
                        for( vector< FieldInterval >::const_reverse_iterator j = intervals.rbegin(); j != intervals.rend(); ++j ) {
                            builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
                            builders.back().first->appendElements( equalityObj );
                            builders.back().second->appendElements( equalityObj );
                            builders.back().first->appendAs( j->upper_.bound_, "" );
                            builders.back().second->appendAs( j->lower_.bound_, "" );
                        }                       
                    }
                }
            } else {
                for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                    j->first->appendAs( forward ? fr.min() : fr.max(), "" );
                    j->second->appendAs( forward ? fr.max() : fr.min(), "" );
                }
            }
        }
        if ( builders.empty() ) {
            BSONObj equalityObj = equalityBuilder.done();
            assert( !equalityObj.isEmpty() );
            builders.push_back( make_pair( shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ), shared_ptr< BSONObjBuilder >( new BSONObjBuilder() ) ) );
            builders.back().first->appendElements( equalityObj );
            builders.back().second->appendElements( equalityObj );            
        }
        BoundList ret;
        for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i )
            ret.push_back( make_pair( i->first->obj(), i->second->obj() ) );
        return ret;
    }

    ///////////////////
    // FilterMatcher //
    ///////////////////
    
    void FieldMatcher::add( const BSONObj& o ){
        BSONObjIterator i( o );
        while ( i.more() ){
            string s = i.next().fieldName();
            if ( s.find( "." ) == string::npos ){
                fields.insert( pair<string,string>( s , "" ) );
            }
            else {
                string sub = s.substr( 0 , s.find( "." ) );
                fields.insert(pair<string,string>( sub , s.substr( sub.size() + 1 ) ) );
            }
        }

    }
    
    int FieldMatcher::size() const {
        return fields.size();
    }

    bool FieldMatcher::matches( const string& s ) const {
        return fields.find( s ) != fields.end();
    }
    
    BSONObj FieldMatcher::getSpec() const{
        BSONObjBuilder b;
        for ( multimap<string,string>::const_iterator i=fields.begin(); i!=fields.end(); i++ ) {
            string s = i->first;
            if ( i->second.size() > 0 )
                s += "." + i->second;
            b.append( s.c_str() , 1 );
        }
        return b.obj();
    }

    void FieldMatcher::extractDotted( const string& path , const BSONObj& o , BSONObjBuilder& b ) const {
        string::size_type i = path.find( "." );
        if ( i == string::npos ){
            const BSONElement & e = o.getField( path.c_str() );
            if ( e.eoo() )
                return;
            b.append(e);
            return;
        }
        
        string left = path.substr( 0 , i );
        BSONElement e = o[left];
        if ( e.type() != Object )
            return;

        BSONObj sub = e.embeddedObject();
        if ( sub.isEmpty() )
            return;
        
        BSONObjBuilder sub_b(32);
        extractDotted( path.substr( i + 1 ) , sub , sub_b );
        b.append( left.c_str() , sub_b.obj() );
    }
    
    void FieldMatcher::append( BSONObjBuilder& b , const BSONElement& e ) const {
        pair<multimap<string,string>::const_iterator,multimap<string,string>::const_iterator> p = fields.equal_range( e.fieldName() );
        BSONObjBuilder sub_b(32);

        for( multimap<string,string>::const_iterator i = p.first; i != p.second; ++i ) {
            string next = i->second;

            if ( e.eoo() ){
            }
            else if ( next.size() == 0 || next == "." || e.type() != Object ){
                b.append( e );
                return;
            }
            else {
                extractDotted( next , e.embeddedObject() , sub_b );
            }
        }

        b.append( e.fieldName() , sub_b.obj() );
    }
    
} // namespace mongo

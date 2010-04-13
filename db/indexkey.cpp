// index_key.cpp

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
#include "namespace.h"
#include "index.h"
#include "btree.h"
#include "query.h"
#include "background.h"

namespace mongo {

    map<string,IndexPlugin*> * IndexPlugin::_plugins;

    IndexType::IndexType( const IndexPlugin * plugin , const IndexSpec * spec )
        : _plugin( plugin ) , _spec( spec ){
        
    }

    IndexType::~IndexType(){
    }
    
    const BSONObj& IndexType::keyPattern() const { 
        return _spec->keyPattern; 
    }

    IndexPlugin::IndexPlugin( const string& name )
        : _name( name ){
        if ( ! _plugins )
            _plugins = new map<string,IndexPlugin*>();
        (*_plugins)[name] = this;
    }
    
    int IndexType::compare( const BSONObj& l , const BSONObj& r ) const {
        return l.woCompare( r , _spec->keyPattern );
    }

    void IndexSpec::_init(){
        assert( keyPattern.objsize() );
        
        string pluginName = "";

        BSONObjIterator i( keyPattern );
        BSONObjBuilder nullKeyB;
        while( i.more() ) {
            BSONElement e = i.next();
            _fieldNames.push_back( e.fieldName() );
            _fixed.push_back( BSONElement() );
            nullKeyB.appendNull( "" );
            if ( e.type() == String ){
                uassert( 13007 , "can only have 1 index plugin / bad index key pattern" , pluginName.size() == 0 );
                pluginName = e.valuestr();
            }
                
        }
        
        _nullKey = nullKeyB.obj();

        BSONObjBuilder b;
        b.appendNull( "" );
        _nullObj = b.obj();
        _nullElt = _nullObj.firstElement();
        
        if ( pluginName.size() ){
            IndexPlugin * plugin = IndexPlugin::get( pluginName );
            if ( ! plugin ){
                log() << "warning: can't find plugin [" << pluginName << "]" << endl;
            }
            else {
                _indexType.reset( plugin->generate( this ) );
            }
        }
        _finishedInit = true;
    }

    
    void IndexSpec::getKeys( const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const {
        if ( _indexType.get() ){
            _indexType->getKeys( obj , keys );
            return;
        }
        vector<const char*> fieldNames( _fieldNames );
        vector<BSONElement> fixed( _fixed );
        _getKeys( fieldNames , fixed , obj, keys );
        if ( keys.empty() )
            keys.insert( _nullKey );
    }

    void IndexSpec::_getKeys( vector<const char*> fieldNames , vector<BSONElement> fixed , const BSONObj &obj, BSONObjSetDefaultOrder &keys ) const {
        BSONElement arrElt;
        unsigned arrIdx = ~0;
        for( unsigned i = 0; i < fieldNames.size(); ++i ) {
            if ( *fieldNames[ i ] == '\0' )
                continue;
            BSONElement e = obj.getFieldDottedOrArray( fieldNames[ i ] );
            if ( e.eoo() )
                e = _nullElt; // no matching field
            if ( e.type() != Array )
                fieldNames[ i ] = ""; // no matching field or non-array match
            if ( *fieldNames[ i ] == '\0' )
                fixed[ i ] = e; // no need for further object expansion (though array expansion still possible)
            if ( e.type() == Array && arrElt.eoo() ) { // we only expand arrays on a single path -- track the path here
                arrIdx = i;
                arrElt = e;
            }
            // enforce single array path here
            uassert( 10088 ,  "cannot index parallel arrays", e.type() != Array || e.rawdata() == arrElt.rawdata() );
        }

        bool allFound = true; // have we found elements for all field names in the key spec?
        for( vector<const char*>::const_iterator i = fieldNames.begin(); i != fieldNames.end(); ++i ){
            if ( **i != '\0' ){
                allFound = false;
                break;
            }
        }

        if ( allFound ) {
            if ( arrElt.eoo() ) {
                // no terminal array element to expand
                BSONObjBuilder b(_sizeTracker);
                for( vector< BSONElement >::iterator i = fixed.begin(); i != fixed.end(); ++i )
                    b.appendAs( *i, "" );
                keys.insert( b.obj() );
            } 
            else {
                // terminal array element to expand, so generate all keys
                BSONObjIterator i( arrElt.embeddedObject() );
                if ( i.more() ){
                    while( i.more() ) {
                        BSONObjBuilder b(_sizeTracker);
                        for( unsigned j = 0; j < fixed.size(); ++j ) {
                            if ( j == arrIdx )
                                b.appendAs( i.next(), "" );
                            else
                                b.appendAs( fixed[ j ], "" );
                        }
                        keys.insert( b.obj() );
                    }
                }
                else if ( fixed.size() > 1 ){
                    // x : [] - need to insert undefined
                    BSONObjBuilder b(_sizeTracker);
                    for( unsigned j = 0; j < fixed.size(); ++j ) {
                        if ( j == arrIdx )
                            b.appendUndefined( "" );
                        else
                            b.appendAs( fixed[ j ], "" );
                    }
                    keys.insert( b.obj() );
                }
            }
        } else {
            // nonterminal array element to expand, so recurse
            assert( !arrElt.eoo() );
            BSONObjIterator i( arrElt.embeddedObject() );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( e.type() == Object )
                    _getKeys( fieldNames, fixed, e.embeddedObject(), keys );
            }
        }
    }

    bool anyElementNamesMatch( const BSONObj& a , const BSONObj& b ){
        BSONObjIterator x(a);
        while ( x.more() ){
            BSONElement e = x.next();
            BSONObjIterator y(b);
            while ( y.more() ){
                BSONElement f = y.next();
                FieldCompareResult res = compareDottedFieldNames( e.fieldName() , f.fieldName() );
                if ( res == SAME || res == LEFT_SUBFIELD || res == RIGHT_SUBFIELD )
                    return true;
            }
        }
        return false;
    }
        
    IndexSuitability IndexSpec::suitability( const BSONObj& query , const BSONObj& order ) const {
        if ( _indexType.get() )
            return _indexType->suitability( query , order );
        return _suitability( query , order );
    }
    
    IndexSuitability IndexSpec::_suitability( const BSONObj& query , const BSONObj& order ) const {
        // TODO: optimize
        if ( anyElementNamesMatch( keyPattern , query ) == 0 && anyElementNamesMatch( keyPattern , order ) == 0 )
            return USELESS;
        return HELPFUL;
    }

    IndexSuitability IndexType::suitability( const BSONObj& query , const BSONObj& order ) const {
        return _spec->_suitability( query , order );
    }

    bool IndexType::scanAndOrderRequired( const BSONObj& query , const BSONObj& order ) const {
        return ! order.isEmpty();
    }

}

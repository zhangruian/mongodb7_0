// projection.h

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

#pragma once

#include "pch.h"
#include "jsobj.h"

namespace mongo {

    /**
       used for doing field limiting
     */
    class Projection {
    public:
        Projection()
            : _include(true)
            , _special(false)
            , _includeID(true)
            , _skip(0)
            , _limit(-1)
        {}
        
        /**
         * called once per lifetime
         */
        void init( const BSONObj& spec );
        
        /**
         * @return the spec init was called with
         */
        BSONObj getSpec() const { return _source; }
        
        /**
         * transforms in according to spec
         */
        BSONObj transform( const BSONObj& in ) const;

        
        /**
         * transforms in according to spec
         */
        void transform( const BSONObj& in , BSONObjBuilder& b ) const;
        

        /**
         * @return if the key has all the information needed to return
         *         NOTE: a key may have modified the actual data
         *               which has to be handled above this
         */
        bool keyEnough( const BSONObj& keyPattern ) const;
        
    private:

        /**
         * appends e to b if user wants it
         * will descend into e if needed
         */
        void append( BSONObjBuilder& b , const BSONElement& e ) const;


        void add( const string& field, bool include );
        void add( const string& field, int skip, int limit );
        void appendArray( BSONObjBuilder& b , const BSONObj& a , bool nested=false) const;

        bool _include; // true if default at this level is to include
        bool _special; // true if this level can't be skipped or included without recursing

        //TODO: benchmark vector<pair> vs map
        typedef map<string, boost::shared_ptr<Projection> > FieldMap;
        FieldMap _fields;
        BSONObj _source;
        bool _includeID;

        // used for $slice operator
        int _skip;
        int _limit;
    };


}

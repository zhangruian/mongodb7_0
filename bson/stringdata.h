// stringdata.h

/*    Copyright 2010 10gen Inc.
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

#ifndef UTIL_BSON_STRINGDATA_HEADER
#define UTIL_BSON_STRINGDATA_HEADER

#include <string>
#include <cstring>

namespace mongo {

    using std::string;

    struct StringData {
        const char*    data;
        const unsigned size;

        StringData( const char* c ) 
            : data(c), size(strlen(c)) {}

        StringData( const string& s )
            : data(s.c_str()), size(s.size()) {}
        
        struct literal_tag {};
        template<size_t N>
        StringData( const char (&val)[N], literal_tag )
            : data(&val[0]), size(N) {}
    };

} // namespace mongo

#endif  // UTIL_BSON_STRINGDATA_HEADER

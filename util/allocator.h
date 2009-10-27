// allocator.h

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

namespace mongo {
    
    inline void * ourmalloc(size_t size) {
        void *x = malloc(size);
        if ( x == 0 ) dbexit( EXIT_OOM_MALLOC , "malloc fails");
        return x;
    }
    
    inline void * ourrealloc(void *ptr, size_t size) {
        void *x = realloc(ptr, size);
        if ( x == 0 ) dbexit( EXIT_OOM_REALLOC , "realloc fails");
        return x;
    }
    
#define malloc mongo::ourmalloc
#define realloc mongo::ourrealloc
    
#if defined(_WIN32)
    inline void our_debug_free(void *p) {
#if 0
        // this is not safe if you malloc < 4 bytes so we don't use anymore
        unsigned *u = (unsigned *) p;
        u[0] = 0xEEEEEEEE;
#endif
        free(p);
    }
#define free our_debug_free
#endif
    
} // namespace mongo

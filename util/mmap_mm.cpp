// mmap_mm.cpp

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
#include "mmap.h"

/* in memory (no file) version */

namespace mongo {

    MemoryMappedFile::MemoryMappedFile() {
        fd = 0;
        maphandle = 0;
        view = 0;
        len = 0;
    }

    void MemoryMappedFile::close() {
        if ( view )
            delete( view );
        view = 0;
        len = 0;
    }

    void* MemoryMappedFile::map(const char *filename, size_t length) {
        path p( filename );

        view = malloc( length );
        return view;
    }

    void MemoryMappedFile::flush(bool sync) {
    }
    

} 


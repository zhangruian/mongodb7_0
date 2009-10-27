// debug_util.h

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

// for debugging
    typedef struct _Ints {
        int i[100];
    } *Ints;
    typedef struct _Chars {
        char c[200];
    } *Chars;

    typedef char CHARS[400];

    typedef struct _OWS {
        int size;
        char type;
        char string[400];
    } *OWS;

// for now, running on win32 means development not production --
// use this to log things just there.
#if defined(_WIN32)
#define WIN if( 1 )
#else
#define WIN if( 0 )
#endif

#if defined(_DEBUG)
#define DEV if( 1 )
#else
#define DEV if( 0 )
#endif

#define DEBUGGING if( 0 )

// The following declare one unique counter per enclosing function.
// NOTE The implementation double-increments on a match, but we don't really care.
#define SOMETIMES( occasion, howOften ) for( static unsigned occasion = 0; ++occasion % howOften == 0; )
#define OCCASIONALLY SOMETIMES( occasionally, 16 )
#define RARELY SOMETIMES( rarely, 128 )
#define ONCE for( static bool undone = true; undone; undone = false ) 
    
#if defined(_WIN32)
#define strcasecmp _stricmp
#endif

} // namespace mongo

// processinfo_win32.cpp

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
#include "processinfo.h"

#include <iostream>

#include <windows.h>
#include <psapi.h>

using namespace std;

int getpid(){
    return GetCurrentProcessId();
}

namespace mongo {
    
    int _wconvertmtos( SIZE_T s ){
        return (int)( s / ( 1024 * 1024 ) );
    }
    
    ProcessInfo::ProcessInfo( pid_t pid ){
    }

    ProcessInfo::~ProcessInfo(){
    }

    bool ProcessInfo::supported(){
        return true;
    }
    
    int ProcessInfo::getVirtualMemorySize(){
        PROCESS_MEMORY_COUNTERS pmc;
        assert( GetProcessMemoryInfo( _pid , &pmc, sizeof(pmc) ) );
        return _wconvertmtos( pmc.PagefileUsage );
    }
    
    int ProcessInfo::getResidentSize(){
        PROCESS_MEMORY_COUNTERS pmc;
        assert( GetProcessMemoryInfo( _pid , &pmc, sizeof(pmc) ) );
        return _wconvertmtos( pmc.WorkingSetSize );
    }

}

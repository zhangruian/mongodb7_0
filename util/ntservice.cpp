// ntservice.cpp

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

#include "pch.h"
#include "ntservice.h"
#include "text.h"
#include <direct.h>

#if defined(_WIN32)

namespace mongo {

	void shutdown();

	SERVICE_STATUS_HANDLE ServiceController::_statusHandle = NULL;
	std::wstring ServiceController::_serviceName;
	ServiceCallback ServiceController::_serviceCallback = NULL;

	ServiceController::ServiceController() {
    }
    
    bool ServiceController::installService( const std::wstring& serviceName, const std::wstring& displayName, const std::wstring& serviceDesc, int argc, char* argv[] ) {
        assert(argc >= 1);

        stringstream commandLine;

        if ( strchr(argv[0], ':') ) { // a crude test for fully qualified path
            commandLine << '"' << argv[0] << "\" ";
        } else {
            char buffer[256];
            assert( _getcwd(buffer, 256) );
            commandLine << '"' << buffer << '\\' << argv[0] << "\" ";
        }
        
		for ( int i = 1; i < argc; i++ ) {
			std::string arg( argv[ i ] );
			
			// replace install command to indicate process is being started as a service
			if ( arg == "--install" || arg == "--reinstall" ) {
				arg = "--service";
			}
				
			commandLine << arg << "  ";
		}
		
		SC_HANDLE schSCManager = ::OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS );
		if ( schSCManager == NULL ) {
			return false;
		}

		// Make sure it exists first. TODO: See if GetLastError() is set to something if CreateService fails.
		SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );
		if ( schService != NULL ) {
			log() << "There is already a service named " << toUtf8String(serviceName) << ". Aborting" << endl;
			::CloseServiceHandle( schService );
			::CloseServiceHandle( schSCManager );
			return false;
		}
		std::basic_ostringstream< TCHAR > commandLineWide;
 		commandLineWide << commandLine.str().c_str();

		log() << "Creating service " << toUtf8String(serviceName) << "." << endl;

		// create new service
		schService = ::CreateService( schSCManager, serviceName.c_str(), displayName.c_str(),
												SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
												SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
												commandLineWide.str().c_str(), NULL, NULL, L"\0\0", NULL, NULL );

		if ( schService == NULL ) {
			log() << "Error creating service." << endl;
			::CloseServiceHandle( schSCManager );
			return false;
		}

		SERVICE_DESCRIPTION serviceDescription;
		serviceDescription.lpDescription = (LPTSTR)serviceDesc.c_str();
		
		// set new service description
		bool serviceInstalled = ::ChangeServiceConfig2( schService, SERVICE_CONFIG_DESCRIPTION, &serviceDescription );
		
		if ( serviceInstalled ) {
			SC_ACTION aActions[ 3 ] = { { SC_ACTION_RESTART, 0 }, { SC_ACTION_RESTART, 0 }, { SC_ACTION_RESTART, 0 } };
			
			SERVICE_FAILURE_ACTIONS serviceFailure;
			ZeroMemory( &serviceFailure, sizeof( SERVICE_FAILURE_ACTIONS ) );
			serviceFailure.cActions = 3;
			serviceFailure.lpsaActions = aActions;
			
			// set service recovery options
			serviceInstalled = ::ChangeServiceConfig2( schService, SERVICE_CONFIG_FAILURE_ACTIONS, &serviceFailure );

			log() << "Service creation successful." << endl;
		}
		else {
			log() << "Service creation seems to have partially failed. Check the event log for more details." << endl;
		}

		::CloseServiceHandle( schService );
		::CloseServiceHandle( schSCManager );
		
		return serviceInstalled;
    }
    
    bool ServiceController::removeService( const std::wstring& serviceName ) {
		SC_HANDLE schSCManager = ::OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS );
		if ( schSCManager == NULL ) {
			return false;
		}

		SC_HANDLE schService = ::OpenService( schSCManager, serviceName.c_str(), SERVICE_ALL_ACCESS );
		if ( schService == NULL ) {
			log() << "Could not find a service named " << toUtf8String(serviceName) << " to uninstall." << endl;
			::CloseServiceHandle( schSCManager );
			return false;
		}

		SERVICE_STATUS serviceStatus;
		
		// stop service if its running
		if ( ::ControlService( schService, SERVICE_CONTROL_STOP, &serviceStatus ) ) {
			log() << "Service " << toUtf8String(serviceName) << " is currently running. Stopping service." << endl;
			while ( ::QueryServiceStatus( schService, &serviceStatus ) ) {
				if ( serviceStatus.dwCurrentState == SERVICE_STOP_PENDING )
				{
				  Sleep( 1000 );
				}
				else { break; }
			}
			log() << "Service stopped." << endl;
		}

		log() << "Deleting service " << toUtf8String(serviceName) << "." << endl;
		bool serviceRemoved = ::DeleteService( schService );
		
		::CloseServiceHandle( schService );
		::CloseServiceHandle( schSCManager );

		if (serviceRemoved) {
			log() << "Service deleted successfully." << endl;
		}
		else {
			log() << "Failed to delete service." << endl;
		}

		return serviceRemoved;
    }
    
    bool ServiceController::startService( const std::wstring& serviceName, ServiceCallback startService ) {
        _serviceName = serviceName;
		_serviceCallback = startService;
	
        SERVICE_TABLE_ENTRY dispTable[] = {
			{ (LPTSTR)serviceName.c_str(), (LPSERVICE_MAIN_FUNCTION)ServiceController::initService },
			{ NULL, NULL }
		};

		return StartServiceCtrlDispatcher( dispTable );
    }
    
    bool ServiceController::reportStatus( DWORD reportState, DWORD waitHint ) {
		if ( _statusHandle == NULL )
			return false;

		static DWORD checkPoint = 1;
		
		SERVICE_STATUS ssStatus;

		ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		ssStatus.dwServiceSpecificExitCode = 0;
		ssStatus.dwControlsAccepted = reportState == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP;
		ssStatus.dwCurrentState = reportState;
		ssStatus.dwWin32ExitCode = NO_ERROR;
		ssStatus.dwWaitHint = waitHint;
		ssStatus.dwCheckPoint = ( reportState == SERVICE_RUNNING || reportState == SERVICE_STOPPED ) ? 0 : checkPoint++;

		return SetServiceStatus( _statusHandle, &ssStatus );
	}
    
    void WINAPI ServiceController::initService( DWORD argc, LPTSTR *argv ) {
		_statusHandle = RegisterServiceCtrlHandler( _serviceName.c_str(), serviceCtrl );
		if ( !_statusHandle )
			return;

		reportStatus( SERVICE_START_PENDING, 1000 );
		
		_serviceCallback();
		
		reportStatus( SERVICE_STOPPED );
	}
	
	void WINAPI ServiceController::serviceCtrl( DWORD ctrlCode ) {
		switch ( ctrlCode ) {
			case SERVICE_CONTROL_STOP:
			case SERVICE_CONTROL_SHUTDOWN:
				shutdown();
				reportStatus( SERVICE_STOPPED );
				return;
		}
	}

} // namespace mongo

#endif

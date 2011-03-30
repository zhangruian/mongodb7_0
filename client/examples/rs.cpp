// rs.cpp

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

/**
 * example of using replica sets from c++
 */

#include "client/dbclient.h"
#include <iostream>
#include <vector>

using namespace mongo;
using namespace std;

void workerThread( string collName , bool print , DBClientReplicaSet * conn ) {

    while ( true ) {
        try {
            conn->update( collName , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) , true );
            
            BSONObj x = conn->findOne( collName , BSONObj() );

            if ( print ) {
                cout << x << endl;
            }
            
            BSONObj a = conn->slaveConn().findOne( collName , BSONObj() , 0 , QueryOption_SlaveOk );
            BSONObj b = conn->findOne( collName , BSONObj() , 0 , QueryOption_SlaveOk );
            
            if ( print ) {
                cout << "\t A " << a << endl;
                cout << "\t B " << b << endl;
            }
        }
        catch ( std::exception& e ) {
            cout << "ERROR: " << e.what() << endl;
        }
        sleepmillis( 10 );
    }
}

int main( int argc , const char ** argv ) {
    
    unsigned nThreads = 1;
    bool print = false;

    for ( int i=1; i<argc; i++ ) {
        if ( mongoutils::str::equals( "--threads" , argv[i] ) ) {
            nThreads = atoi( argv[++i] );
        }
        else if ( mongoutils::str::equals( "--print" , argv[1] ) ) {
            print = true;
        }
        else {
            cerr << "unknown option: " << argv[i] << endl;
            return 1;
        }
            
    }

    string errmsg;
    ConnectionString cs = ConnectionString::parse( "foo/127.0.0.1" , errmsg );
    if ( ! cs.isValid() ) {
        cout << "error parsing url: " << errmsg << endl;
        return 1;
    }

    DBClientReplicaSet * conn = (DBClientReplicaSet*)cs.connect( errmsg );
    if ( ! conn ) {
        cout << "error connecting: " << errmsg << endl;
        return 2;
    }

    string collName = "test.rs1";

    conn->dropCollection( collName );
    
    vector<shared_ptr<boost::thread> > threads;
    for ( unsigned i=0; i<nThreads; i++ ) {
        string errmsg;
        threads.push_back( shared_ptr<boost::thread>( new boost::thread( boost::bind( workerThread , collName , print , (DBClientReplicaSet*)cs.connect(errmsg) ) ) ) );
    }
    
    for ( unsigned i=0; i<threads.size(); i++ ) {
        threads[i]->join();
    }

}

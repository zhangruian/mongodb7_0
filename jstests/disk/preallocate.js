// check that there is preallocation on explicit createCollection() and no unncessary preallocation after restart

port = allocatePorts( 1 )[ 0 ];

var baseName = "jstests_preallocate2";

var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName );

assert.eq( 0, m.getDBs().totalSize );

m.getDB( baseName ).createCollection( baseName + "1" );

assert.soon( function() { return m.getDBs().totalSize > 100000000; }, "expected second file to bring total size over 100MB" );

stopMongod( port );

var m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/" + baseName );

size = m.getDBs().totalSize;

m.getDB( baseName ).createCollection( baseName + "2" );

sleep( 2000 ); // give prealloc a chance

assert.eq( size, m.getDBs().totalSize );

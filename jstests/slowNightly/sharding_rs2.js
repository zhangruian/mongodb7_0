// mostly for testing mongos w/replica sets


s = new ShardingTest( "rs2" , 2 , 1 , 1 , { rs : true , chunksize : 1 } )

db = s.getDB( "test" )

// -------------------------------------------------------------------------------------------
// ---------- test that config server updates when replica set config changes ----------------
// -------------------------------------------------------------------------------------------


db.foo.save( { x : 17 } )
assert.eq( 1 , db.foo.count() );

s.config.databases.find().forEach( printjson )
s.config.shards.find().forEach( printjson )

serverName = s.getServerName( "test" ) 

function countNodes(){
    var x = s.config.shards.findOne( { _id : serverName } );
    return x.host.split( "," ).length
}

assert.eq( 3 , countNodes() , "A1" )

rs = s.getRSEntry( serverName );
rs.test.add()
try {
    rs.test.reInitiate();
}
catch ( e ){
    // this os ok as rs's may close connections on a change of master
    print( e );
}

assert.soon( 
    function(){
        try {
            printjson( rs.test.getMaster().getDB("admin").runCommand( "isMaster" ) )
            s.config.shards.find().forEach( printjsononeline );
            return countNodes() == 4;
        }
        catch ( e ){
            print( e );
        }
    } , "waiting for config server to update" , 180 * 1000 , 1000 );


// -------------------------------------------------------------------------------------------
// ---------- test routing to slaves ----------------
// -------------------------------------------------------------------------------------------

m = new Mongo( s.s.name );
t = m.getDB( "test" ).foo

before = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

for ( i=0; i<10; i++ )
    assert.eq( 17 , t.findOne().x , "B1" )

m.setSlaveOk()
for ( i=0; i<10; i++ )
    assert.eq( 17 , t.findOne().x , "B2" )

after = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

printjson( before )
printjson( after )

assert.eq( before.query + 10 , after.query , "B3" )

s.stop()

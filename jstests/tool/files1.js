// files1.js

t = new ToolTest( "files1" )

db = t.startDB();

t.runTool( "files" , "-d" , t.baseName , "put" , 'mongod' );
md5 = md5sumFile('mongod');

file_obj = db.fs.files.findOne()
md5_stored = file_obj.md5;
md5_computed = db.runCommand({filemd5: file_obj._id}).md5;
assert.eq( md5 , md5_stored , "A 1" );
assert.eq( md5 , md5_computed, "A 2" );

try {
    listFiles(t.ext);
} catch (e) {
    runProgram('mkdir', t.ext);
}

t.runTool( "files" , "-d" , t.baseName , "get" , 'mongod' , '-l' , t.extFile );
md5 = md5sumFile(t.extFile);
assert.eq( md5 , md5_stored , "B" );

t.stop()

// config.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* This file is things related to the "grid configuration":
   - what machines make up the db component of our cloud
   - where various ranges of things live
*/

#pragma once

#include "../db/namespace.h"
#include "../client/dbclient.h"
#include "../client/model.h"
#include "shardkey.h"
#include "shard.h"

namespace mongo {

    struct ShardNS {
        static string shard;
        
        static string database;
        static string collection;
        static string chunk;

        static string mongos;
        static string settings;
    };

    /**
     * Field names used in the 'shards' collection.
     */
    struct ShardFields {
        static BSONField<bool> draining;
        static BSONField<long long> maxSize;
        static BSONField<long long> currSize;
    };
        
    class Grid;
    class ConfigServer;

    class DBConfig;
    typedef boost::shared_ptr<DBConfig> DBConfigPtr;

    extern DBConfigPtr configServerPtr;
    extern ConfigServer& configServer;
    extern Grid grid;

    class ChunkManager;
    typedef shared_ptr<ChunkManager> ChunkManagerPtr;
    
    /**
     * top level configuration for a database
     */
    class DBConfig  {

        struct CollectionInfo {
            CollectionInfo(){
                _dirty = false;
                _dropped = false;
            }
            
            CollectionInfo( DBConfig * db , const BSONObj& in );
            
            bool isSharded() const {
                return _cm.get();
            }
            
            ChunkManagerPtr getCM() const {
                return _cm;
            }

            void shard( DBConfig * db , const string& ns , const ShardKeyPattern& key , bool unique );
            void unshard();

            bool isDirty() const { return _dirty; }
            bool wasDropped() const { return _dropped; }
            
            void save( const string& ns , DBClientBase* conn );
            

        private:
            ChunkManagerPtr _cm;
            bool _dirty;
            bool _dropped;
        };
        
        typedef map<string,CollectionInfo> Collections;
        
    public:

        DBConfig( string name ) 
            : _name( name ) , 
              _primary("config","") , 
              _shardingEnabled(false), 
              _lock("DBConfig"){
            assert( name.size() );
        }
        virtual ~DBConfig(){}
        
        string getName(){ return _name; };

        /**
         * @return if anything in this db is partitioned or not
         */
        bool isShardingEnabled(){
            return _shardingEnabled;
        }
        
        void enableSharding();
        ChunkManagerPtr shardCollection( const string& ns , ShardKeyPattern fieldsAndOrder , bool unique );
        
        /**
         * @return whether or not the 'ns' collection is partitioned
         */
        bool isSharded( const string& ns );
        
        ChunkManagerPtr getChunkManager( const string& ns , bool reload = false );
        
        /**
         * @return the correct for shard for the ns
         * if the namespace is sharded, will return NULL
         */
        const Shard& getShard( const string& ns );
        
        const Shard& getPrimary() const {
            uassert( 8041 , (string)"no primary shard configured for db: " + _name , _primary.ok() );
            return _primary;
        }
        
        void setPrimary( string s );

        bool load();
        bool reload();
        
        bool dropDatabase( string& errmsg );

        // model stuff

        // lockless loading
        void serialize(BSONObjBuilder& to);

        /**
         * if i need save in new format
         */
        bool unserialize(const BSONObj& from);

        void getAllShards(set<Shard>& shards) const;

    protected:

        /** 
            lockless
        */
        bool _isSharded( const string& ns );

        bool _dropShardedCollections( int& num, set<Shard>& allServers , string& errmsg );

        bool _load();
        bool _reload();
        void _save();

        
        /**
           @return true if there was sharding info to remove
         */
        bool removeSharding( const string& ns );

        string _name; // e.g. "alleyinsider"
        Shard _primary; // e.g. localhost , mongo.foo.com:9999
        bool _shardingEnabled;
        
        //map<string,CollectionInfo> _sharded; // { "alleyinsider.blog.posts" : { ts : 1 }  , ... ] - all ns that are sharded
        //map<string,ChunkManagerPtr> _shards; // this will only have entries for things that have been looked at

        Collections _collections;

        mongo::mutex _lock; // TODO: change to r/w lock ??

        friend class Grid;
        friend class ChunkManager;
    };

    /**
     * stores meta-information about the grid
     * TODO: used shard_ptr for DBConfig pointers
     */
    class Grid {
    public:
        Grid() : _lock( "Grid" ) , _allowLocalShard( true ) { }

        /**
         * gets the config the db.
         * will return an empty DBConfig if not in db already
         */
        DBConfigPtr getDBConfig( string ns , bool create=true );
        
        /**
         * removes db entry.
         * on next getDBConfig call will fetch from db
         */
        void removeDB( string db );

        /**
         * @return true if shards and config servers are allowed to use 'localhost' in address
         */
        bool allowLocalHost() const;

        /**
         * @param whether to allow shards and config servers to use 'localhost' in address
         */
        void setAllowLocalHost( bool allow );

        /**
         *
         * addShard will create a new shard in the grid. It expects a mongod process to be runing
         * on the provided address.
         * TODO - add the mongod's databases to the grid
         *
         * @param name is an optional string with the name of the shard. if ommited, grid will
         * generate one and update the parameter.
         * @param host is the complete address of the machine where the shard will be
         * @param maxSize is the optional space quota in bytes. Zeros means there's no limitation to
         * space usage
         * @param errMsg is the error description in case the operation failed. 
         * @return true if shard was successfully added.
         */
        bool addShard( string* name , const string& host , long long maxSize , string* errMsg );

        /**
         * @return true if the config database knows about a host 'name'
         */
        bool knowAboutShard( const string& name ) const;
        
        /**
         * @return true if the chunk balancing functionality is enabled
         */
        bool shouldBalance() const;

        unsigned long long getNextOpTime() const;

    private:
        mongo::mutex              _lock;            // protects _databases; TODO: change to r/w lock ??
        map<string, DBConfigPtr > _databases;       // maps ns to DBConfig's
        bool                      _allowLocalShard; // can 'localhost' be used in shard addresses?

        /**
         * @param name is the chose name for the shard. Parameter is mandatory.
         * @return true if it managed to generate a shard name. May return false if (currently)
         * 10000 shard 
         */
        bool _getNewShardName( string* name ) const;

    };

    class ConfigServer : public DBConfig {
    public:

        ConfigServer();
        ~ConfigServer();
        
        bool ok(){
            return _primary.ok();
        }
        
        virtual string modelServer(){
            uassert( 10190 ,  "ConfigServer not setup" , _primary.ok() );
            return _primary.getConnString();
        }
        
        /**
           call at startup, this will initiate connection to the grid db 
        */
        bool init( vector<string> configHosts );
        
        bool init( string s );

        bool allUp();
        bool allUp( string& errmsg );
        
        int dbConfigVersion();
        int dbConfigVersion( DBClientBase& conn );
        
        void reloadSettings();

        /**
         * @return 0 = ok, otherwise error #
         */
        int checkConfigVersion( bool upgrade );
        
        /**
         * log a change to config.changes 
         * @param what e.g. "split" , "migrate"
         * @param msg any more info
         */
        void logChange( const string& what , const string& ns , const BSONObj& detail = BSONObj() );

        ConnectionString getConnectionString() const {
            return ConnectionString( _primary.getConnString() , ConnectionString::SYNC );
        }

        static int VERSION;
        
    private:
        string getHost( string name , bool withPort );
    };

} // namespace mongo

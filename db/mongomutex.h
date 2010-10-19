// @file mongomutex.h

/*
 *    Copyright (C) 2010 10gen Inc.
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

#pragma once

namespace mongo { 

    /* the 'big lock' we use for most operations. a read/write lock.
       there is one of these, dbMutex.  
       generally if you need to declare a mutex use the right primitive class no this.

       use readlock and writelock classes for scoped locks on this rather than direct 
       manipulation.
       */
    class MongoMutex {
    public:
        MongoMutex(const char * name);

        /** @return
         *    > 0  write lock
         *    = 0  no lock
         *    < 0  read lock
         */
        int getState() const { return _state.get(); }

        bool atLeastReadLocked() const { return _state.get() != 0; }
        void assertAtLeastReadLocked() const { assert(atLeastReadLocked()); }
        bool isWriteLocked() const { return getState() > 0; }
        void assertWriteLocked() const { 
            assert( getState() > 0 ); 
            DEV assert( !_releasedEarly.get() );
        }

        // write lock
        void lock() { 
            if ( _writeLockedAlready() )
                return;
            
            _state.set(1);

            curopWaitingForLock( 1 ); // stats
            _m.lock(); 
            curopGotLock();

            _minfo.entered();

            MongoFile::lockAll();
        }

        // try write lock
        bool lock_try( int millis ) { 
            if ( _writeLockedAlready() )
                return true;

            curopWaitingForLock( 1 );
            bool got = _m.lock_try( millis ); 
            curopGotLock();
            
            if ( got ){
                _minfo.entered();
                _state.set(1);
                MongoFile::lockAll();
            }                
            
            return got;
        }

        // un write lock 
        void unlock() { 
            int s = _state.get();
            if( s > 1 ) { 
                _state.set(s-1);
                return;
            }
            if( s != 1 ) { 
                if( _releasedEarly.get() ) { 
                    _releasedEarly.set(false);
                    return;
                }
                massert( 12599, "internal error: attempt to unlock when wasn't in a write lock", false);
            }

            MongoFile::unlockAll();

            _state.set(0);
            _minfo.leaving();
            _m.unlock(); 
        }

        /* unlock (write lock), and when unlock() is called later, 
           be smart then and don't unlock it again.
           */
        void releaseEarly() {
            assert( getState() == 1 ); // must not be recursive
            assert( !_releasedEarly.get() );
            _releasedEarly.set(true);
            unlock();
        }

        // read lock
        void lock_shared() { 
            int s = _state.get();
            if( s ) {
                if( s > 0 ) { 
                    // already in write lock - just be recursive and stay write locked
                    _state.set(s+1);
                    return;
                }
                else { 
                    // already in read lock - recurse
                    _state.set(s-1);
                    return;
                }
            }
            _state.set(-1);
            curopWaitingForLock( -1 );
            _m.lock_shared(); 
            curopGotLock();
        }
        
        // try read lock
        bool lock_shared_try( int millis ) {
            int s = _state.get();
            if ( s ){
                // we already have a lock, so no need to try
                lock_shared();
                return true;
            }

            bool got = _m.lock_shared_try( millis );
            if ( got )
                _state.set(-1);
            return got;
        }
        
        void unlock_shared() { 
            int s = _state.get();
            if( s > 0 ) { 
                assert( s > 1 ); /* we must have done a lock write first to have s > 1 */
                _state.set(s-1);
                return;
            }
            if( s < -1 ) { 
                _state.set(s+1);
                return;
            }
            assert( s == -1 );
            _state.set(0);
            _m.unlock_shared(); 
        }
        
        MutexInfo& info() { return _minfo; }

    private:
        /* @return true if was already write locked.  increments recursive lock count. */
        bool _writeLockedAlready() {
            dassert( haveClient() );                
            int s = _state.get();
            if( s > 0 ) {
                _state.set(s+1);
                return true;
            }
            massert( 10293 , (string)"internal error: locks are not upgradeable: " + sayClientState() , s == 0 );
            return false;
        }

        RWLock _m;

        /* > 0 write lock with recurse count
           < 0 read lock 
        */
        ThreadLocalValue<int> _state;

        MutexInfo _minfo;

        /* See the releaseEarly() method.
           we use a separate TLS value for releasedEarly - that is ok as 
           our normal/common code path, we never even touch it */
        ThreadLocalValue<bool> _releasedEarly;

        /* this is for fsyncAndLock command.  otherwise write lock's greediness will
           make us block on any attempted write lock the the fsync's lock.
           */
        //volatile bool _blockWrites;
    };

    extern MongoMutex &dbMutex;

}

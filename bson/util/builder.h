/* builder.h */

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

#include <string>
#include <string.h>
#include <stdio.h>
#include <boost/shared_ptr.hpp>

namespace mongo {

    class StringBuilder;

    void msgasserted(int msgid, const char *msg);

    class BufBuilder {
    public:
        BufBuilder(int initsize = 512) : size(initsize) {
            if ( size > 0 ) {
                data = (char *) malloc(size);
                if( data == 0 )
                    msgasserted(10000, "out of memory BufBuilder");
            } else {
                data = 0;
            }
            l = 0;
        }
        ~BufBuilder() {
            kill();
        }

        void kill() {
            if ( data ) {
                free(data);
                data = 0;
            }
        }

        void reset( int maxSize = 0 ){
            l = 0;
            if ( maxSize && size > maxSize ){
                free(data);
                data = (char*)malloc(maxSize);
                size = maxSize;
            }            
        }

        /* leave room for some stuff later */
        char* skip(int n) { return grow(n); }

        /* note this may be deallocated (realloced) if you keep writing. */
        char* buf() { return data; }
        const char* buf() const { return data; }

        /* assume ownership of the buffer - you must then free() it */
        void decouple() { data = 0; }

        template<class T> void append(T j) {
            *((T*)grow(sizeof(T))) = j;
        }
        void append(short j) {
            append<short>(j);
        }
        void append(int j) {
            append<int>(j);
        }
        void append(unsigned j) {
            append<unsigned>(j);
        }
        void append(bool j) {
            append<bool>(j);
        }
        void append(double j) {
            append<double>(j);
        }

        void append(const void *src, size_t len) {
            memcpy(grow((int) len), src, len);
        }

        void append(const char *str) {
            append((void*) str, strlen(str)+1);
        }
        
        void append(const std::string &str) {
            append( (void *)str.c_str(), str.length() + 1 );
        }

        int len() const {
            return l;
        }

        void setlen( int newLen ){
            l = newLen;
        }

        /* returns the pre-grow write position */
        char* grow(int by) {
            int oldlen = l;
            l += by;
            if ( l > size ) {
                grow_reallocate();
            }
            return data + oldlen;
        }

        /* "slow" portion of 'grow()'  */
        void grow_reallocate() {
            int a = size * 2;
            if ( a == 0 )
                a = 512;
            if ( l > a )
                a = l + 16 * 1024;
            if( a > 64 * 1024 * 1024 )
                msgasserted(10000, "BufBuilder grow() > 64MB");
            data = (char *) realloc(data, a);
            size= a;
        }

        int getSize() const { return size; }

    private:
        char *data;
        int l;
        int size;

        friend class StringBuilder;
    };

#if defined(_WIN32)
#pragma warning( disable : 4996 )
#endif

    class StringBuilder {
    public:
        explicit StringBuilder( int initsize=256 )
            : _buf( initsize ){
        }

#define SBNUM(val,maxSize,macro) \
            int prev = _buf.l; \
            int z = sprintf( _buf.grow(maxSize) , macro , (val) );  \
            _buf.l = prev + z; \
            return *this; 

        StringBuilder& operator<<( double x ){
            SBNUM( x , 25 , "%g" );
        }
        StringBuilder& operator<<( int x ){
            SBNUM( x , 11 , "%d" );
        }
        StringBuilder& operator<<( unsigned x ){
            SBNUM( x , 11 , "%u" );
        }
        StringBuilder& operator<<( long x ){
            SBNUM( x , 22 , "%ld" );
        }
        StringBuilder& operator<<( unsigned long x ){
            SBNUM( x , 22 , "%lu" );
        }
        StringBuilder& operator<<( long long x ){
            SBNUM( x , 22 , "%lld" );
        }
        StringBuilder& operator<<( unsigned long long x ){
            SBNUM( x , 22 , "%llu" );
        }
        StringBuilder& operator<<( short x ){
            SBNUM( x , 8 , "%hd" );
        }
        StringBuilder& operator<<( char c ){
            _buf.grow( 1 )[0] = c;
            return *this;
        }
#undef SBNUM

        void append( const char * str){
            append(str, strlen(str));
        }
        void append( const char * str, int len){
            memcpy( _buf.grow( len ) , str , len );
        }
        StringBuilder& operator<<( const char * str ){
            append( str, strlen(str) );
            return *this;
        }
        StringBuilder& operator<<( const std::string& s ){
            append( s.c_str(), s.size() );
            return *this;
        }
        
        // access

        void reset( int maxSize = 0 ){
            _buf.reset( maxSize );
        }
        
        std::string str(){
            return std::string(_buf.data, _buf.l);
        }

    private:
        BufBuilder _buf;
    };

} // namespace mongo

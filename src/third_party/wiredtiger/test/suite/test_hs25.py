#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wttest

# test_hs25.py
# Ensure updates structure is correct when processing each key.
class test_hs25(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'
    uri = 'table:test_hs25'

    def test_insert_updates_hs(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=i,value_format=S')
        s = self.conn.open_session()

        # Update the first key.
        cursor1 = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor1[1] = 'a'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Update the second key.
        self.session.begin_transaction()
        cursor1[2] = 'a'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        self.session.begin_transaction()
        cursor1[2] = 'b'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Prepared update on the first key.
        self.session.begin_transaction()
        cursor1[1] = 'b'
        cursor1[1] = 'c'
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(4))

        # Run eviction cursor.
        s.begin_transaction('ignore_prepare=true')
        evict_cursor = s.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.assertEqual(evict_cursor[1], 'a')
        self.assertEqual(evict_cursor[2], 'b')
        s.rollback_transaction()
        self.session.rollback_transaction()

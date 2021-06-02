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
from wiredtiger import WT_NOTFOUND
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

# test_prepare16.py
# Test that the prepare transaction rollback/commit multiple keys
# and each key can occupy a leaf page.
class test_prepare16(wttest.WiredTigerTestCase):
    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer_row', dict(key_format='i')),
    ]

    txn_end_values = [
        ('commit', dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]

    scenarios = make_scenarios(in_memory_values, key_format_values, txn_end_values)

    def conn_config(self):
        config = 'cache_size=250MB'
        if self.in_memory:
            config += ',in_memory=true'
        else:
            config += ',in_memory=false'
        return config

    def test_prepare(self):
        nrows = 1000

        # Prepare transactions for column store table is not yet supported.
        if self.key_format == 'r':
            self.skipTest('Prepare transactions for column store table is not yet supported')

        # Create a table without logging.
        uri = "table:prepare16"
        create_config = 'allocation_size=512,key_format=S,value_format=S,leaf_page_max=512,leaf_value_max=64MB'
        self.session.create(uri, create_config)

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(10) +
            ',stable_timestamp=' + timestamp_str(10))

        valuea = 'a' * 400

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[str(i)] = valuea

        cursor.reset()
        cursor.close()
        self.session.prepare_transaction('prepare_timestamp=' + timestamp_str(10))

        s = self.conn.open_session()
        s.begin_transaction('ignore_prepare = true')
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")

        for i in range(1, nrows + 1):
            evict_cursor.set_key(str(i))
            self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
            evict_cursor.reset()

        if self.commit:
            self.session.timestamp_transaction('commit_timestamp=' + timestamp_str(20))
            self.session.timestamp_transaction('durable_timestamp=' + timestamp_str(30))
            self.session.commit_transaction()
        else:
            self.session.rollback_transaction()

        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(30))
        if not self.in_memory:
            self.session.checkpoint()

        self.session.begin_transaction('read_timestamp=' + timestamp_str(20))
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            cursor.set_key(str(i))
            if self.commit:
                self.assertEquals(cursor.search(), 0)
                self.assertEqual(cursor.get_value(), valuea)
            else:
                self.assertEquals(cursor.search(), WT_NOTFOUND)
        self.session.commit_transaction()

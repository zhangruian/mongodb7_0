/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

/*
 * Issue a warning when there enough consecutive unsuccessful checks for rollback to stable.
 */
#define WARN_RTS_NO_CHECK 5

/*
 * snap_init --
 *     Initialize the repeatable operation tracking.
 */
void
snap_init(TINFO *tinfo)
{
    /*
     * We maintain two snap lists. The current one is indicated by tinfo->s, and keeps the most
     * recent operations. The other one is used when we are running with rollback_to_stable. When
     * each thread notices that the stable timestamp has changed, it stashes the current snap list
     * and starts fresh with the other snap list. After we've completed a rollback_to_stable, we can
     * the secondary snap list to see the state of keys/values seen and updated at the time of the
     * rollback.
     */
    if (g.c_txn_rollback_to_stable) {
        tinfo->s = &tinfo->snap_states[1];
        tinfo->snap_list = dcalloc(SNAP_LIST_SIZE, sizeof(SNAP_OPS));
        tinfo->snap_end = &tinfo->snap_list[SNAP_LIST_SIZE];
    }
    tinfo->s = &tinfo->snap_states[0];
    tinfo->snap_list = dcalloc(SNAP_LIST_SIZE, sizeof(SNAP_OPS));
    tinfo->snap_end = &tinfo->snap_list[SNAP_LIST_SIZE];
    tinfo->snap_current = tinfo->snap_list;
}

/*
 * snap_teardown --
 *     Tear down the repeatable operation tracking structures.
 */
void
snap_teardown(TINFO *tinfo)
{
    SNAP_OPS *snaplist;
    u_int i, snap_index;

    for (snap_index = 0; snap_index < WT_ELEMENTS(tinfo->snap_states); snap_index++)
        if ((snaplist = tinfo->snap_states[snap_index].snap_state_list) != NULL) {
            for (i = 0; i < SNAP_LIST_SIZE; ++i) {
                free(snaplist[i].kdata);
                free(snaplist[i].vdata);
            }
            free(snaplist);
        }
}

/*
 * snap_clear --
 *     Clear a single snap entry.
 */
static void
snap_clear_one(SNAP_OPS *snap)
{
    snap->repeatable = false;
}

/*
 * snap_clear --
 *     Clear the snap list.
 */
static void
snap_clear(TINFO *tinfo)
{
    SNAP_OPS *snap;

    for (snap = tinfo->snap_list; snap < tinfo->snap_end; ++snap)
        snap_clear_one(snap);
}

/*
 * snap_op_init --
 *     Initialize the repeatable operation tracking for each new operation.
 */
void
snap_op_init(TINFO *tinfo, uint64_t read_ts, bool repeatable_reads)
{
    uint64_t stable_ts;

    ++tinfo->opid;

    if (g.c_txn_rollback_to_stable) {
        /*
         * If the stable timestamp has changed and we've advanced beyond it, preserve the current
         * snapshot history up to this point, we'll use it verify rollback_to_stable. Switch our
         * tracking to the other snap list.
         */
        stable_ts = __wt_atomic_addv64(&g.stable_timestamp, 0);
        if (stable_ts != tinfo->stable_ts && read_ts > stable_ts) {
            tinfo->stable_ts = stable_ts;
            if (tinfo->s == &tinfo->snap_states[0])
                tinfo->s = &tinfo->snap_states[1];
            else
                tinfo->s = &tinfo->snap_states[0];
            tinfo->snap_current = tinfo->snap_list;

            /* Clear out older info from the snap list. */
            snap_clear(tinfo);
        }
    }

    tinfo->snap_first = tinfo->snap_current;

    tinfo->read_ts = read_ts;
    tinfo->repeatable_reads = repeatable_reads;
    tinfo->repeatable_wrap = false;
}

/*
 * snap_track --
 *     Add a single snapshot isolation returned value to the list.
 */
void
snap_track(TINFO *tinfo, thread_op op)
{
    WT_ITEM *ip;
    SNAP_OPS *snap;

    snap = tinfo->snap_current;
    snap->op = op;
    snap->opid = tinfo->opid;
    snap->keyno = tinfo->keyno;
    snap->ts = WT_TS_NONE;
    snap->repeatable = false;
    snap->last = op == TRUNCATE ? tinfo->last : 0;
    snap->ksize = snap->vsize = 0;

    if (op == INSERT && g.type == ROW) {
        ip = tinfo->key;
        if (snap->kmemsize < ip->size) {
            snap->kdata = drealloc(snap->kdata, ip->size);
            snap->kmemsize = ip->size;
        }
        memcpy(snap->kdata, ip->data, snap->ksize = ip->size);
    }

    if (op != REMOVE && op != TRUNCATE) {
        ip = tinfo->value;
        if (snap->vmemsize < ip->size) {
            snap->vdata = drealloc(snap->vdata, ip->size);
            snap->vmemsize = ip->size;
        }
        memcpy(snap->vdata, ip->data, snap->vsize = ip->size);
    }

    /* Move to the next slot, wrap at the end of the circular buffer. */
    if (++tinfo->snap_current >= tinfo->snap_end)
        tinfo->snap_current = tinfo->snap_list;

    /*
     * It's possible to pass this transaction's buffer starting point and start replacing our own
     * entries. If that happens, we can't repeat operations because we don't know which ones were
     * previously modified.
     */
    if (tinfo->snap_current->opid == tinfo->opid)
        tinfo->repeatable_wrap = true;
}

/*
 * print_item_data --
 *     Display a single data/size pair, with a tag.
 */
static void
print_item_data(const char *tag, const uint8_t *data, size_t size)
{
    WT_ITEM tmp;

    if (g.type == FIX) {
        fprintf(stderr, "%s {0x%02x}\n", tag, data[0]);
        return;
    }

    memset(&tmp, 0, sizeof(tmp));
    testutil_check(__wt_raw_to_esc_hex(NULL, data, size, &tmp));
    fprintf(stderr, "%s {%s}\n", tag, (char *)tmp.mem);
    __wt_buf_free(NULL, &tmp);
}

/*
 * snap_verify --
 *     Repeat a read and verify the contents.
 */
static int
snap_verify(WT_CURSOR *cursor, TINFO *tinfo, SNAP_OPS *snap)
{
    WT_DECL_RET;
    WT_ITEM *key, *value;
    uint64_t keyno;
    uint8_t bitfield;

    testutil_assert(snap->op != TRUNCATE);

    key = tinfo->key;
    value = tinfo->value;
    keyno = snap->keyno;

    /*
     * Retrieve the key/value pair by key. Row-store inserts have a unique generated key we saved,
     * else generate the key from the key number.
     */
    if (snap->op == INSERT && g.type == ROW) {
        key->data = snap->kdata;
        key->size = snap->ksize;
        cursor->set_key(cursor, key);
    } else {
        switch (g.type) {
        case FIX:
        case VAR:
            cursor->set_key(cursor, keyno);
            break;
        case ROW:
            key_gen(key, keyno);
            cursor->set_key(cursor, key);
            break;
        }
    }

    switch (ret = read_op(cursor, SEARCH, NULL)) {
    case 0:
        if (g.type == FIX) {
            testutil_check(cursor->get_value(cursor, &bitfield));
            *(uint8_t *)(value->data) = bitfield;
            value->size = 1;
        } else
            testutil_check(cursor->get_value(cursor, value));
        break;
    case WT_NOTFOUND:
        break;
    default:
        return (ret);
    }

    /* Check for simple matches. */
    if (ret == 0 && snap->op != REMOVE && value->size == snap->vsize &&
      memcmp(value->data, snap->vdata, value->size) == 0)
        return (0);
    if (ret == WT_NOTFOUND && snap->op == REMOVE)
        return (0);

    /*
     * In fixed length stores, zero values at the end of the key space are returned as not-found,
     * and not-found row reads are saved as zero values. Map back-and-forth for simplicity.
     */
    if (g.type == FIX) {
        if (ret == WT_NOTFOUND && snap->vsize == 1 && *(uint8_t *)snap->vdata == 0)
            return (0);
        if (snap->op == REMOVE && value->size == 1 && *(uint8_t *)value->data == 0)
            return (0);
    }

    /* Things went pear-shaped. */
    switch (g.type) {
    case FIX:
        fprintf(stderr,
          "snapshot-isolation: %" PRIu64 " search: expected {0x%02x}, found {0x%02x}\n", keyno,
          snap->op == REMOVE ? 0U : *(uint8_t *)snap->vdata,
          ret == WT_NOTFOUND ? 0U : *(uint8_t *)value->data);
        break;
    case ROW:
        fprintf(
          stderr, "snapshot-isolation %.*s search mismatch\n", (int)key->size, (char *)key->data);

        if (snap->op == REMOVE)
            fprintf(stderr, "expected {deleted}\n");
        else
            print_item_data("expected", snap->vdata, snap->vsize);
        if (ret == WT_NOTFOUND)
            fprintf(stderr, "   found {deleted}\n");
        else
            print_item_data("   found", value->data, value->size);
        break;
    case VAR:
        fprintf(stderr, "snapshot-isolation %" PRIu64 " search mismatch\n", keyno);

        if (snap->op == REMOVE)
            fprintf(stderr, "expected {deleted}\n");
        else
            print_item_data("expected", snap->vdata, snap->vsize);
        if (ret == WT_NOTFOUND)
            fprintf(stderr, "   found {deleted}\n");
        else
            print_item_data("   found", value->data, value->size);
        break;
    }

    g.page_dump_cursor = cursor;
    testutil_assert(0);

    /* NOTREACHED */
    return (1);
}

/*
 * snap_ts_clear --
 *     Clear snapshots at or before a specified timestamp.
 */
static void
snap_ts_clear(TINFO *tinfo, uint64_t ts)
{
    SNAP_OPS *snap;

    /* Check from the first slot to the last. */
    for (snap = tinfo->snap_list; snap < tinfo->snap_end; ++snap)
        if (snap->repeatable && snap->ts <= ts)
            snap->repeatable = false;
}

/*
 * snap_repeat_ok_match --
 *     Compare two operations and see if they modified the same record.
 */
static bool
snap_repeat_ok_match(SNAP_OPS *current, SNAP_OPS *a)
{
    /* Reads are never a problem, there's no modification. */
    if (a->op == READ)
        return (true);

    /* Check for a matching single record modification. */
    if (a->keyno == current->keyno)
        return (false);

    /* Truncates are slightly harder, make sure the ranges don't overlap. */
    if (a->op == TRUNCATE) {
        if (g.c_reverse && (a->keyno == 0 || a->keyno >= current->keyno) &&
          (a->last == 0 || a->last <= current->keyno))
            return (false);
        if (!g.c_reverse && (a->keyno == 0 || a->keyno <= current->keyno) &&
          (a->last == 0 || a->last >= current->keyno))
            return (false);
    }

    return (true);
}

/*
 * snap_repeat_ok_commit --
 *     Return if an operation in the transaction can be repeated, where the transaction isn't yet
 *     committed (so all locks are in place), or has already committed successfully.
 */
static bool
snap_repeat_ok_commit(TINFO *tinfo, SNAP_OPS *current)
{
    SNAP_OPS *p;

    /*
     * Truncates can't be repeated, we don't know the exact range of records that were removed (if
     * any).
     */
    if (current->op == TRUNCATE)
        return (false);

    /*
     * For updates, check for subsequent changes to the record and don't repeat the read. For reads,
     * check for either subsequent or previous changes to the record and don't repeat the read. (The
     * reads are repeatable, but only at the commit timestamp, and the update will do the repeatable
     * read in that case.)
     */
    for (p = current;;) {
        /* Wrap at the end of the circular buffer. */
        if (++p >= tinfo->snap_end)
            p = tinfo->snap_list;
        if (p->opid != tinfo->opid)
            break;

        if (!snap_repeat_ok_match(current, p))
            return (false);
    }

    if (current->op != READ)
        return (true);
    for (p = current;;) {
        /* Wrap at the beginning of the circular buffer. */
        if (--p < tinfo->snap_list)
            p = &tinfo->snap_list[SNAP_LIST_SIZE - 1];
        if (p->opid != tinfo->opid)
            break;

        if (!snap_repeat_ok_match(current, p))
            return (false);
    }
    return (true);
}

/*
 * snap_repeat_ok_rollback --
 *     Return if an operation in the transaction can be repeated, after a transaction has rolled
 *     back.
 */
static bool
snap_repeat_ok_rollback(TINFO *tinfo, SNAP_OPS *current)
{
    SNAP_OPS *p;

    /* Ignore update operations, they can't be repeated after rollback. */
    if (current->op != READ)
        return (false);

    /*
     * Check for previous changes to the record and don't attempt to repeat the read in that case.
     */
    for (p = current;;) {
        /* Wrap at the beginning of the circular buffer. */
        if (--p < tinfo->snap_list)
            p = &tinfo->snap_list[SNAP_LIST_SIZE - 1];
        if (p->opid != tinfo->opid)
            break;

        if (!snap_repeat_ok_match(current, p))
            return (false);
    }
    return (true);
}

/*
 * snap_repeat_txn --
 *     Repeat each operation done within a snapshot isolation transaction.
 */
int
snap_repeat_txn(WT_CURSOR *cursor, TINFO *tinfo)
{
    SNAP_OPS *current;

    /* If we wrapped the buffer, we can't repeat operations. */
    if (tinfo->repeatable_wrap)
        return (0);

    /* Check from the first operation we saved to the last. */
    for (current = tinfo->snap_first;; ++current) {
        /* Wrap at the end of the circular buffer. */
        if (current >= tinfo->snap_end)
            current = tinfo->snap_list;
        if (current->opid != tinfo->opid)
            break;

        /*
         * The transaction is not yet resolved, so the rules are as if the transaction has
         * committed. Note we are NOT checking if reads are repeatable based on the chosen
         * timestamp. This is because we expect snapshot isolation to work even in the presence of
         * other threads of control committing in our past, until the transaction resolves.
         */
        if (snap_repeat_ok_commit(tinfo, current))
            WT_RET(snap_verify(cursor, tinfo, current));
    }

    return (0);
}

/*
 * snap_repeat_update --
 *     Update the list of snapshot operations based on final transaction resolution.
 */
void
snap_repeat_update(TINFO *tinfo, bool committed)
{
    SNAP_OPS *current;

    /* If we wrapped the buffer, we can't repeat operations. */
    if (tinfo->repeatable_wrap)
        return;

    /* Check from the first operation we saved to the last. */
    for (current = tinfo->snap_first;; ++current) {
        /* Wrap at the end of the circular buffer. */
        if (current >= tinfo->snap_end)
            current = tinfo->snap_list;
        if (current->opid != tinfo->opid)
            break;

        /*
         * First, reads may simply not be repeatable because the read timestamp chosen wasn't older
         * than all concurrently running uncommitted updates.
         */
        if (!tinfo->repeatable_reads && current->op == READ)
            continue;

        /*
         * Second, check based on the transaction resolution (the rules are different if the
         * transaction committed or rolled back).
         */
        current->repeatable = committed ? snap_repeat_ok_commit(tinfo, current) :
                                          snap_repeat_ok_rollback(tinfo, current);

        /*
         * Repeat reads at the transaction's read timestamp and updates at the commit timestamp.
         */
        if (current->repeatable)
            current->ts = current->op == READ ? tinfo->read_ts : tinfo->commit_ts;
    }
}

/*
 * snap_repeat --
 *     Repeat one operation.
 */
static void
snap_repeat(WT_CURSOR *cursor, TINFO *tinfo, SNAP_OPS *snap, bool rollback_allowed)
{
    WT_DECL_RET;
    WT_SESSION *session;
    char buf[64];

    session = cursor->session;

    /*
     * Start a new transaction. Set the read timestamp. Verify the record. Discard the transaction.
     */
    wiredtiger_begin_transaction(session, "isolation=snapshot");

    /*
     * If the timestamp has aged out of the system, we'll get EINVAL when we try and set it.
     */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "read_timestamp=%" PRIx64, snap->ts));

    ret = session->timestamp_transaction(session, buf);
    if (ret == 0) {
        trace_op(tinfo, "repeat %" PRIu64 " ts=%" PRIu64 " {%s}", snap->keyno, snap->ts,
          trace_bytes(tinfo, snap->vdata, snap->vsize));

        /* The only expected error is rollback. */
        ret = snap_verify(cursor, tinfo, snap);

        if (ret != 0 && (!rollback_allowed || (ret != WT_ROLLBACK && ret != WT_CACHE_FULL)))
            testutil_check(ret);
    } else if (ret == EINVAL)
        snap_ts_clear(tinfo, snap->ts);
    else
        testutil_check(ret);

    /* Discard the transaction. */
    testutil_check(session->rollback_transaction(session, NULL));
}

/*
 * snap_repeat_single --
 *     Repeat an historic operation.
 */
void
snap_repeat_single(WT_CURSOR *cursor, TINFO *tinfo)
{
    SNAP_OPS *snap;
    u_int v;
    int count;

    /*
     * Start at a random spot in the list of operations and look for a read to retry. Stop when
     * we've walked the entire list or found one.
     */
    v = mmrand(&tinfo->rnd, 1, SNAP_LIST_SIZE) - 1;
    for (snap = &tinfo->snap_list[v], count = SNAP_LIST_SIZE; count > 0; --count, ++snap) {
        /* Wrap at the end of the circular buffer. */
        if (snap >= tinfo->snap_end)
            snap = tinfo->snap_list;

        if (snap->repeatable)
            break;
    }

    if (count == 0)
        return;

    snap_repeat(cursor, tinfo, snap, true);
}

/*
 * snap_repeat_rollback --
 *     Repeat all known operations after a rollback.
 */
void
snap_repeat_rollback(WT_CURSOR *cursor, TINFO **tinfo_array, size_t tinfo_count)
{
    SNAP_OPS *snap;
    SNAP_STATE *state;
    TINFO *tinfo, **tinfop;
    uint32_t count;
    size_t i, statenum;
    char buf[100];

    count = 0;

    track("rollback_to_stable: checking", 0ULL, NULL);
    for (i = 0, tinfop = tinfo_array; i < tinfo_count; ++i, ++tinfop) {
        tinfo = *tinfop;

        /*
         * For this thread, walk through both sets of snaps ("states"), looking for entries that are
         * repeatable and have relevant timestamps. One set will have the most current operations,
         * meaning they will likely be newer than the stable timestamp, and thus cannot be checked.
         * The other set typically has operations that are just before the stable timestamp, so are
         * candidates for checking.
         */
        for (statenum = 0; statenum < WT_ELEMENTS(tinfo->snap_states); statenum++) {
            state = &tinfo->snap_states[statenum];
            for (snap = state->snap_state_list; snap < state->snap_state_end; ++snap) {
                if (snap->repeatable && snap->ts <= g.stable_timestamp &&
                  snap->ts >= g.oldest_timestamp) {
                    snap_repeat(cursor, tinfo, snap, false);
                    ++count;
                    if (count % 100 == 0) {
                        testutil_check(__wt_snprintf(
                          buf, sizeof(buf), "rollback_to_stable: %" PRIu32 " ops repeated", count));
                        track(buf, 0ULL, NULL);
                    }
                }
                snap_clear_one(snap);
            }
        }
    }

    /* Show the final result and check that we're accomplishing some checking. */
    testutil_check(
      __wt_snprintf(buf, sizeof(buf), "rollback_to_stable: %" PRIu32 " ops repeated", count));
    track(buf, 0ULL, NULL);
    if (count == 0) {
        if (++g.rts_no_check >= WARN_RTS_NO_CHECK)
            fprintf(stderr,
              "Warning: %" PRIu32 " consecutive runs with no rollback_to_stable checking\n", count);
    } else
        g.rts_no_check = 0;
}

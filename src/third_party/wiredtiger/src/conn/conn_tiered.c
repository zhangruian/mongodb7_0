/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 1)
/*
 * !!!
 * GCC with -Wformat-nonliteral complains about calls to strftime in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
#endif

/*
 * __flush_tier_wait --
 *     Wait for all previous work units queued to be processed.
 */
static void
__flush_tier_wait(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    int yield_count;

    conn = S2C(session);
    yield_count = 0;
    /*
     * The internal thread needs the schema lock to perform its operations and flush tier also
     * acquires the schema lock. We cannot be waiting in this function while holding that lock or no
     * work will get done.
     */
    WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA));

    /*
     * It may be worthwhile looking at the add and decrement values and make choices of whether to
     * yield or wait based on how much of the workload has been performed. Flushing operations could
     * take a long time so yielding may not be effective.
     *
     * TODO: We should consider a maximum wait value as a configuration setting. If we add one, then
     * this function returns an int and this loop would check how much time we've waited and break
     * out with EBUSY.
     */
    while (!WT_FLUSH_STATE_DONE(conn->flush_state)) {
        if (++yield_count < WT_THOUSAND)
            __wt_yield();
        else
            __wt_cond_wait(session, conn->flush_cond, 200, NULL);
    }
}

/*
 * __flush_tier_once --
 *     Perform one iteration of tiered storage maintenance.
 */
static int
__flush_tier_once(WT_SESSION_IMPL *session, uint32_t flags)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *key, *value;

    WT_UNUSED(flags);
    __wt_verbose(session, WT_VERB_TIERED, "%s", "FLUSH_TIER_ONCE: Called");

    cursor = NULL;
    /*
     * For supporting splits and merge:
     * - See if there is any merging work to do to prepare and create an object that is
     *   suitable for placing onto tiered storage.
     * - Do the work to create said objects.
     * - Move the objects.
     */
    S2C(session)->flush_state = 0;

    /*
     * XXX: Is it sufficient to walk the metadata cursor? If it is, why doesn't checkpoint do that?
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));
    while (cursor->next(cursor) == 0) {
        cursor->get_key(cursor, &key);
        cursor->get_value(cursor, &value);
        /* For now just switch tiers which just does metadata manipulation. */
        if (WT_PREFIX_MATCH(key, "tiered:")) {
            __wt_verbose(session, WT_VERB_TIERED, "FLUSH_TIER_ONCE: %s %s", key, value);
            /* Is this instantiating every handle even if it is not opened or in use? */
            WT_ERR(__wt_session_get_dhandle(session, key, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
            /*
             * When we call wt_tiered_switch the session->dhandle points to the tiered: entry and
             * the arg is the config string that is currently in the metadata.
             */
            WT_ERR(__wt_tiered_switch(session, value));
            WT_ERR(__wt_session_release_dhandle(session));
        }
    }
    WT_ERR(__wt_metadata_cursor_release(session, &cursor));

    return (0);

err:
    WT_TRET(__wt_session_release_dhandle(session));
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __tier_storage_remove_local --
 *     Perform one iteration of tiered storage local tier removal.
 */
static int
__tier_storage_remove_local(WT_SESSION_IMPL *session, const char *uri, bool force)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    size_t len;
    uint64_t now;
    char *config, *newfile;
    const char *cfg[2], *filename;

    config = newfile = NULL;
    if (uri == NULL)
        return (0);
    __wt_verbose(session, WT_VERB_TIERED, "Removing tree %s", uri);
    filename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, filename, "tiered:");
    len = strlen("file:") + strlen(filename) + 1;
    WT_ERR(__wt_calloc_def(session, len, &newfile));
    WT_ERR(__wt_snprintf(newfile, len, "file:%s", filename));

    /*
     * If the file:URI of the tiered object does not exist, there is nothing to do.
     */
    ret = __wt_metadata_search(session, newfile, &config);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto err;
    }
    WT_ERR(ret);

    /*
     * We have a local version of this tiered data. Check its metadata for when it expires and
     * remove if necessary.
     */
    cfg[0] = config;
    cfg[1] = NULL;
    WT_ERR(__wt_config_gets(session, cfg, "local_retention", &cval));
    __wt_seconds(session, &now);
    if (force || (uint64_t)cval.val + S2C(session)->bstorage->retain_secs >= now)
        /*
         * We want to remove the entry and the file. Probably do a schema_drop on the file:uri.
         */
        __wt_verbose(session, WT_VERB_TIERED, "Would remove %s. Local retention expired", newfile);

err:
    __wt_free(session, config);
    __wt_free(session, newfile);
    return (ret);
}

/*
 * __tier_flush_meta --
 *     Perform one iteration of altering the metadata after a flush. This is in its own function so
 *     that we can hold the schema lock while doing the metadata tracking.
 */
static int
__tier_flush_meta(
  WT_SESSION_IMPL *session, WT_TIERED *tiered, const char *local_uri, const char *obj_uri)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    uint64_t now;
    char *newconfig, *obj_value;
    const char *cfg[3] = {NULL, NULL, NULL};
    bool release, tracking;

    release = tracking = false;
    WT_RET(__wt_scr_alloc(session, 512, &buf));
    dhandle = &tiered->iface;

    newconfig = NULL;
    WT_ERR(__wt_meta_track_on(session));
    tracking = true;

    WT_ERR(__wt_session_get_dhandle(session, dhandle->name, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
    release = true;
    /*
     * Once the flush call succeeds we want to first remove the file: entry from the metadata and
     * then update the object: metadata to indicate the flush is complete.
     */
    WT_ERR(__wt_metadata_remove(session, local_uri));
    WT_ERR(__wt_metadata_search(session, obj_uri, &obj_value));
    __wt_seconds(session, &now);
    WT_ERR(__wt_buf_fmt(session, buf, "flush=%" PRIu64, now));
    cfg[0] = obj_value;
    cfg[1] = buf->mem;
    WT_ERR(__wt_config_collapse(session, cfg, &newconfig));
    WT_ERR(__wt_metadata_update(session, obj_uri, newconfig));
    WT_ERR(__wt_meta_track_off(session, true, ret != 0));
    tracking = false;

err:
    __wt_free(session, newconfig);
    if (release)
        WT_TRET(__wt_session_release_dhandle(session));
    __wt_scr_free(session, &buf);
    if (tracking)
        WT_TRET(__wt_meta_track_off(session, true, ret != 0));
    return (ret);
}

/*
 * __wt_tier_do_flush --
 *     Perform one iteration of copying newly flushed objects to the shared storage.
 */
int
__wt_tier_do_flush(
  WT_SESSION_IMPL *session, WT_TIERED *tiered, const char *local_uri, const char *obj_uri)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *bucket_fs;
    WT_STORAGE_SOURCE *storage_source;
    uint32_t msec, retry;
    const char *local_name, *obj_name;

    storage_source = tiered->bstorage->storage_source;
    bucket_fs = tiered->bstorage->file_system;

    local_name = local_uri;
    WT_PREFIX_SKIP_REQUIRED(session, local_name, "file:");
    obj_name = obj_uri;
    WT_PREFIX_SKIP_REQUIRED(session, obj_name, "object:");

    /* This call make take a while, and may fail due to network timeout. */
    WT_RET(storage_source->ss_flush(
      storage_source, &session->iface, bucket_fs, local_name, obj_name, NULL));

    /*
     * Flushing the metadata grabs the data handle with exclusive access, and the data handle may be
     * held by the thread that queues the flush tier work item. As a result, the handle may be busy,
     * so retry as needed, up to a few seconds.
     */
    for (msec = 10, retry = 0; msec < 3000; msec *= 2, retry++) {
        if (retry != 0)
            __wt_sleep(0, msec * WT_THOUSAND);
        WT_WITH_CHECKPOINT_LOCK(session,
          WT_WITH_SCHEMA_LOCK(
            session, ret = __tier_flush_meta(session, tiered, local_uri, obj_uri)));
        if (ret != EBUSY)
            break;
        WT_STAT_CONN_INCR(session, flush_tier_busy);
    }
    WT_RET(ret);

    /*
     * We may need a way to cleanup flushes for those not completed (after a crash), or failed (due
     * to previous network outage).
     */
    WT_RET(storage_source->ss_flush_finish(
      storage_source, &session->iface, bucket_fs, local_name, obj_name, NULL));
    return (0);
}

/*
 * __wt_tier_flush --
 *     Given an ID generate the URI names and call the flush code.
 */
int
__wt_tier_flush(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
{
    WT_DECL_RET;
    const char *local_uri, *obj_uri;

    local_uri = obj_uri = NULL;
    WT_ERR(__wt_tiered_name(session, &tiered->iface, id, WT_TIERED_NAME_LOCAL, &local_uri));
    WT_ERR(__wt_tiered_name(session, &tiered->iface, id, WT_TIERED_NAME_OBJECT, &obj_uri));
    WT_ERR(__wt_tier_do_flush(session, tiered, local_uri, obj_uri));

err:
    __wt_free(session, local_uri);
    __wt_free(session, obj_uri);
    return (ret);
}

/*
 * __tier_storage_copy --
 *     Perform one iteration of copying newly flushed objects to the shared storage.
 */
static int
__tier_storage_copy(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TIERED_WORK_UNIT *entry;

    entry = NULL;
    for (;;) {
        /*
         * We probably need some kind of flush generation so that we don't process flush items for
         * tables that are added during an in-progress flush_tier. This thread could run due to a
         * condition timeout rather than a signal. Checking that generation number would be part of
         * calling __wt_tiered_get_flush so that we don't pull it off the queue until we're sure we
         * want to process it.
         */
        __wt_tiered_get_flush(session, &entry);
        if (entry == NULL)
            break;
        WT_ERR(__wt_tier_flush(session, entry->tiered, entry->id));
        /*
         * We are responsible for freeing the work unit when we're done with it.
         */
        __wt_tiered_work_free(session, entry);
        entry = NULL;
    }

err:
    if (entry != NULL)
        __wt_tiered_work_free(session, entry);
    return (ret);
}

/*
 * __tier_storage_remove --
 *     Perform one iteration of tiered storage local tier removal.
 */
static int
__tier_storage_remove(WT_SESSION_IMPL *session, bool force)
{
    WT_UNUSED(session);
    WT_UNUSED(force);

    /*
     * We want to walk the metadata perhaps and for each tiered URI, call remove on its file:URI
     * version.
     */
    WT_RET(__tier_storage_remove_local(session, NULL, force));
    return (0);
}

/*
 * __wt_flush_tier --
 *     Entry function for flush_tier method.
 */
int
__wt_flush_tier(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint32_t flags;
    const char *cfg[3];
    bool wait;

    conn = S2C(session);
    WT_STAT_CONN_INCR(session, flush_tier);
    if (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_TIERED_MGR))
        WT_RET_MSG(
          session, EINVAL, "Cannot call flush_tier when storage manager thread is configured");

    flags = 0;
    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_flush_tier);
    cfg[1] = (char *)config;
    cfg[2] = NULL;
    WT_RET(__wt_config_gets(session, cfg, "force", &cval));
    if (cval.val)
        LF_SET(WT_FLUSH_TIER_FORCE);
    WT_RET(__wt_config_gets(session, cfg, "sync", &cval));
    if (WT_STRING_MATCH("off", cval.str, cval.len))
        LF_SET(WT_FLUSH_TIER_OFF);
    else if (WT_STRING_MATCH("on", cval.str, cval.len))
        LF_SET(WT_FLUSH_TIER_ON);

    WT_RET(__wt_config_gets(session, cfg, "lock_wait", &cval));
    if (cval.val)
        wait = true;
    else
        wait = false;

    /*
     * We have to hold the lock around both the wait call for a previous flush tier and the
     * execution of the current flush tier call.
     */
    if (wait)
        __wt_spin_lock(session, &conn->flush_tier_lock);
    else
        WT_RET(__wt_spin_trylock(session, &conn->flush_tier_lock));

    /*
     * We cannot perform another flush tier until any earlier ones are done. Often threads will wait
     * after the flush tier based on the sync setting so this check will be fast. But if sync is
     * turned off then any following call must wait and will do so here. We have to wait while not
     * holding the schema lock.
     */
    __flush_tier_wait(session);
    if (wait)
        WT_WITH_SCHEMA_LOCK(session, ret = __flush_tier_once(session, flags));
    else
        WT_WITH_SCHEMA_LOCK_NOWAIT(session, ret, ret = __flush_tier_once(session, flags));
    __wt_spin_unlock(session, &conn->flush_tier_lock);

    if (ret == 0 && LF_ISSET(WT_FLUSH_TIER_ON))
        __flush_tier_wait(session);
    return (ret);
}

/*
 * __tiered_manager_config --
 *     Parse and setup the storage manager options.
 */
static int
__tiered_manager_config(WT_SESSION_IMPL *session, const char **cfg, bool *runp)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_TIERED_MANAGER *mgr;

    conn = S2C(session);
    mgr = &conn->tiered_mgr;

    /* Only start the server if wait time is non-zero */
    WT_RET(__wt_config_gets(session, cfg, "tiered_manager.wait", &cval));
    mgr->wait_usecs = (uint64_t)cval.val * WT_MILLION;
    if (runp != NULL)
        *runp = mgr->wait_usecs != 0;

    WT_RET(__wt_config_gets(session, cfg, "tiered_manager.threads_max", &cval));
    if (cval.val > WT_TIERED_MAX_WORKERS)
        WT_RET_MSG(session, EINVAL, "Maximum storage workers of %" PRIu32 " larger than %d",
          (uint32_t)cval.val, WT_TIERED_MAX_WORKERS);
    mgr->workers_max = (uint32_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "tiered_manager.threads_min", &cval));
    if (cval.val < WT_TIERED_MIN_WORKERS)
        WT_RET_MSG(session, EINVAL, "Minimum storage workers of %" PRIu32 " less than %d",
          (uint32_t)cval.val, WT_TIERED_MIN_WORKERS);
    mgr->workers_min = (uint32_t)cval.val;
    WT_ASSERT(session, mgr->workers_min <= mgr->workers_max);
    return (0);
}

/*
 * __tiered_server_run_chk --
 *     Check to decide if the tiered storage server should continue running.
 */
static bool
__tiered_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_TIERED));
}

/*
 * __tiered_server --
 *     The tiered storage server thread.
 */
static WT_THREAD_RET
__tiered_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;
    uint64_t cond_time, time_start, time_stop, timediff;
    bool signalled;

    session = arg;
    conn = S2C(session);

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    /* Condition timeout is in microseconds. */
    cond_time = WT_MINUTE * WT_MILLION;
    time_start = __wt_clock(session);
    signalled = false;
    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait_signal(
          session, conn->tiered_cond, cond_time, __tiered_server_run_chk, &signalled);

        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_server_run_chk(session))
            break;

        time_stop = __wt_clock(session);
        timediff = WT_CLOCKDIFF_SEC(time_stop, time_start);
        /*
         * Here is where we do work. Work we expect to do:
         *  - Copy any files that need moving from a flush tier call.
         *  - Remove any cached objects that are aged out.
         */
        if (timediff >= WT_MINUTE || signalled) {
            WT_ERR(__tier_storage_copy(session));
            WT_ERR(__tier_storage_remove(session, false));
        }
        time_start = time_stop;
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "storage server error"));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}

/*
 * __tiered_mgr_run_chk --
 *     Check to decide if the tiered storage manager should continue running.
 */
static bool
__tiered_mgr_run_chk(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    return ((FLD_ISSET(conn->server_flags, WT_CONN_SERVER_TIERED_MGR)) &&
      !F_ISSET(&conn->tiered_mgr, WT_TIERED_MANAGER_SHUTDOWN));
}

/*
 * __tiered_mgr_server --
 *     The tiered storage manager thread.
 */
static WT_THREAD_RET
__tiered_mgr_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;
    WT_TIERED_MANAGER *mgr;

    session = arg;
    conn = S2C(session);
    mgr = &conn->tiered_mgr;

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait(session, conn->tiered_mgr_cond, mgr->wait_usecs, __tiered_mgr_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_mgr_run_chk(session))
            break;

        /*
         * Here is where we do work. Work we expect to do:
         */
        WT_WITH_SCHEMA_LOCK(session, ret = __flush_tier_once(session, 0));
        WT_ERR(ret);
        if (ret == 0)
            __flush_tier_wait(session);
        WT_ERR(__tier_storage_remove(session, false));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "storage server error"));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}
/*
 * __tiered_mgr_start --
 *     Start the tiered manager flush thread.
 */
static int
__tiered_mgr_start(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;

    FLD_SET(conn->server_flags, WT_CONN_SERVER_TIERED_MGR);
    WT_RET(__wt_open_internal_session(
      conn, "storage-mgr-server", false, 0, 0, &conn->tiered_mgr_session));
    session = conn->tiered_mgr_session;

    WT_RET(__wt_cond_alloc(session, "storage server", &conn->tiered_mgr_cond));

    /* Start the thread. */
    WT_RET(__wt_thread_create(session, &conn->tiered_mgr_tid, __tiered_mgr_server, session));
    conn->tiered_mgr_tid_set = true;
    return (0);
}

/*
 * __wt_tiered_storage_create --
 *     Start the tiered storage subsystem.
 */
int
__wt_tiered_storage_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    bool start;

    conn = S2C(session);
    start = false;

    WT_RET(__tiered_manager_config(session, cfg, &start));

    /* Start the internal thread. */
    WT_ERR(__wt_cond_alloc(session, "flush tier", &conn->flush_cond));
    WT_ERR(__wt_cond_alloc(session, "storage server", &conn->tiered_cond));
    FLD_SET(conn->server_flags, WT_CONN_SERVER_TIERED);

    WT_ERR(__wt_open_internal_session(conn, "storage-server", true, 0, 0, &conn->tiered_session));
    session = conn->tiered_session;

    /* Start the thread. */
    WT_ERR(__wt_thread_create(session, &conn->tiered_tid, __tiered_server, session));
    conn->tiered_tid_set = true;

    /* After starting non-configurable threads, start the tiered manager if needed. */
    if (start)
        WT_ERR(__tiered_mgr_start(conn));

    if (0) {
err:
        FLD_CLR(conn->server_flags, WT_CONN_SERVER_TIERED);
        WT_TRET(__wt_tiered_storage_destroy(session));
    }
    return (ret);
}

/*
 * __wt_tiered_storage_destroy --
 *     Destroy the tiered storage server thread.
 */
int
__wt_tiered_storage_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TIERED_WORK_UNIT *entry;

    conn = S2C(session);

    /* Stop the internal server thread. */
    if (conn->flush_cond != NULL)
        __wt_cond_signal(session, conn->flush_cond);
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_TIERED | WT_CONN_SERVER_TIERED_MGR);
    if (conn->tiered_tid_set) {
        WT_ASSERT(session, conn->tiered_cond != NULL);
        __wt_cond_signal(session, conn->tiered_cond);
        WT_TRET(__wt_thread_join(session, &conn->tiered_tid));
        conn->tiered_tid_set = false;
        while ((entry = TAILQ_FIRST(&conn->tieredqh)) != NULL) {
            TAILQ_REMOVE(&conn->tieredqh, entry, q);
            __wt_tiered_work_free(session, entry);
        }
    }
    if (conn->tiered_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->tiered_session));
        conn->tiered_session = NULL;
    }

    /* Stop the storage manager thread. */
    if (conn->tiered_mgr_tid_set) {
        WT_ASSERT(session, conn->tiered_mgr_cond != NULL);
        __wt_cond_signal(session, conn->tiered_mgr_cond);
        WT_TRET(__wt_thread_join(session, &conn->tiered_mgr_tid));
        conn->tiered_mgr_tid_set = false;
    }
    /* Destroy all condition variables after threads have stopped. */
    __wt_cond_destroy(session, &conn->tiered_cond);
    __wt_cond_destroy(session, &conn->tiered_mgr_cond);
    /* The flush condition variable must be last because any internal thread could be using it.  */
    __wt_cond_destroy(session, &conn->flush_cond);

    if (conn->tiered_mgr_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->tiered_mgr_session));
        conn->tiered_mgr_session = NULL;
    }

    return (ret);
}

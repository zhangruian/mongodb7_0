/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_direct_io_size_check --
 *     Return a size from the configuration, complaining if it's insufficient for direct I/O.
 */
int
__wt_direct_io_size_check(
  WT_SESSION_IMPL *session, const char **cfg, const char *config_name, uint32_t *allocsizep)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint32_t allocsize;

    *allocsizep = 0;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, config_name, &cval));
    allocsize = (uint32_t)cval.val;

    /*
     * This function exists as a place to hang this comment: if direct I/O is configured, page sizes
     * must be at least as large as any buffer alignment as well as a multiple of the alignment.
     * Linux gets unhappy if you configure direct I/O and then don't do I/O in alignments and units
     * of its happy place. Ideally, we'd fail if an application set an allocation size incompatible
     * with the direct I/O size, while silently adjusting internal files using a default allocation
     * size, but this function is too far down in the call stack to distinguish between the two. We
     * document that setting a larger buffer alignment than the allocation size silently increases
     * the allocation size: direct I/O isn't a heavily used feature, that should be sufficient.
     */
    if (conn->buffer_alignment != 0 &&
      FLD_ISSET(conn->direct_io, WT_DIRECT_IO_CHECKPOINT | WT_DIRECT_IO_DATA)) {

        if (allocsize < conn->buffer_alignment)
            allocsize = (uint32_t)conn->buffer_alignment;
        if (allocsize % conn->buffer_alignment != 0)
            WT_RET_MSG(session, EINVAL,
              "when direct I/O is configured for data files, the %s size must be at least as large "
              "as the buffer alignment, as well as a multiple of the buffer alignment",
              config_name);
    }
    *allocsizep = allocsize;
    return (0);
}

/*
 * __check_imported_ts --
 *     Check the aggregated timestamps for each checkpoint in a file that we've imported. By
 *     default, we're not allowed to import files with timestamps ahead of the oldest timestamp
 *     since a subsequent rollback to stable could result in data loss and historical reads could
 *     yield unexpected values. Therefore, this function should return non-zero to callers to
 *     signify that this is the case. If configured, it is possible to import files with timestamps
 *     smaller than or equal to the stable timestamp. However, there is no history migrated with the
 *     files and thus reading historical versions will not work.
 */
static int
__check_imported_ts(
  WT_SESSION_IMPL *session, const char *uri, const char *config, bool against_stable)
{
    WT_CKPT *ckptbase, *ckpt;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t ts;
    const char *ts_name;

    ckptbase = NULL;
    txn_global = &S2C(session)->txn_global;
    ts = against_stable ? txn_global->stable_timestamp : txn_global->oldest_timestamp;
    ts_name = against_stable ? "stable" : "oldest";

    WT_ERR_NOTFOUND_OK(
      __wt_meta_ckptlist_get_from_config(session, false, &ckptbase, NULL, config), true);
    if (ret == WT_NOTFOUND)
        WT_ERR_MSG(session, EINVAL,
          "%s: import could not find any checkpoint information in supplied metadata", uri);

    /* Now iterate over each checkpoint and compare the aggregate timestamps with our oldest. */
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        if (ckpt->ta.newest_start_durable_ts > ts)
            WT_ERR_MSG(session, WT_ROLLBACK,
              "%s: import found aggregated newest start durable timestamp newer than the current "
              "%s timestamp, newest_start_durable_ts=%" PRIu64 ", %s_ts=%" PRIu64,
              uri, ts_name, ckpt->ta.newest_start_durable_ts, ts_name, ts);

        /*
         * No need to check "newest stop" here as "newest stop durable" serves that purpose. When a
         * file has at least one record without a stop timestamp, "newest stop" will be set to max
         * whereas "newest stop durable" refers to the newest non-max timestamp which is more useful
         * to us in terms of comparing with oldest.
         */
        if (ckpt->ta.newest_stop_durable_ts > ts) {
            WT_ASSERT(session, ckpt->ta.newest_stop_durable_ts != WT_TS_MAX);
            WT_ERR_MSG(session, WT_ROLLBACK,
              "%s: import found aggregated newest stop durable timestamp newer than the current "
              "%s timestamp, newest_stop_durable_ts=%" PRIu64 ", %s_ts=%" PRIu64,
              uri, ts_name, ckpt->ta.newest_stop_durable_ts, ts_name, ts);
        }
    }

err:
    if (ckptbase != NULL)
        __wt_meta_ckptlist_free(session, &ckptbase);
    return (ret);
}

/*
 * __create_file_block_manager --
 *     Create a new file in the block manager, and track it.
 */
static int
__create_file_block_manager(
  WT_SESSION_IMPL *session, const char *uri, const char *filename, uint32_t allocsize)
{
    WT_RET(__wt_block_manager_create(session, filename, allocsize));

    /*
     * Track the creation of this file.
     *
     * If something down the line fails, we're going to need to roll this back. Specifically do NOT
     * track the op in the import case since we do not want to wipe a data file just because we fail
     * to import it.
     */
    if (WT_META_TRACKING(session))
        WT_RET(__wt_meta_track_fileop(session, NULL, uri));

    return (0);
}

/*
 * __create_file --
 *     Create a new 'file:' object.
 */
static int
__create_file(WT_SESSION_IMPL *session, const char *uri, bool exclusive, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(val);
    WT_DECL_RET;
    const char *filename, **p,
      *filecfg[] = {WT_CONFIG_BASE(session, file_meta), config, NULL, NULL, NULL, NULL};
    char *fileconf, *filemeta;
    uint32_t allocsize;
    bool against_stable, exists, import, import_repair, is_metadata;

    fileconf = filemeta = NULL;
    import = F_ISSET(session, WT_SESSION_IMPORT);

    import_repair = false;
    is_metadata = strcmp(uri, WT_METAFILE_URI) == 0;
    WT_ERR(__wt_scr_alloc(session, 1024, &buf));

    filename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, filename, "file:");

    /* Check if the file already exists. */
    if (!is_metadata && (ret = __wt_metadata_search(session, uri, &fileconf)) != WT_NOTFOUND) {
        /*
         * Regardless of the 'exclusive' flag, we should raise an error if we try to import an
         * existing URI rather than just silently returning.
         */
        if (exclusive || import)
            WT_TRET(EEXIST);
        goto err;
    }

    exists = false;
    /*
     * At this moment the uri doesn't exist in the metadata. In scenarios like, the database folder
     * is copied without a checkpoint into another location and trying to recover from it leads to
     * that history store file exists on disk but not as part of metadata. As we recreate the
     * history store file on every restart to ensure that history store file is present. Make sure
     * to remove the already exist history store file in the directory.
     */
    if (strcmp(uri, WT_HS_URI) == 0) {
        WT_IGNORE_RET(__wt_fs_exist(session, filename, &exists));
        if (exists)
            WT_IGNORE_RET(__wt_fs_remove(session, filename, true));
    }

    /* Sanity check the allocation size. */
    WT_ERR(__wt_direct_io_size_check(session, filecfg, "allocation_size", &allocsize));

    /*
     * If we are importing an existing object rather than creating a new one, there are two possible
     * scenarios. Either (1) the file configuration string from the source database metadata is
     * specified in the input config string, or (2) the import.repair option is set and we need to
     * reconstruct the configuration metadata from the file.
     */
    if (import) {
        /*
         * Create the file for tiered storage. It is required because we switched to a new file
         * during the import process.
         */
        if (WT_SUFFIX_MATCH(filename, ".wtobj")) {
            if (session->import_list != NULL)
                WT_ERR(__create_file_block_manager(session, uri, filename, allocsize));
            else
                WT_ERR_MSG(session, ENOTSUP,
                  "%s: import without metadata_file not supported on tiered files", uri);
        }

        /* First verify that the data to import exists on disk. */
        WT_IGNORE_RET(__wt_fs_exist(session, filename, &exists));
        if (!exists)
            WT_ERR_MSG(session, ENOENT, "%s", uri);

        import_repair =
          __wt_config_getones(session, config, "import.repair", &cval) == 0 && cval.val != 0;
        if (!import_repair) {
            if (__wt_config_getones(session, config, "import.file_metadata", &cval) == 0 &&
              cval.len != 0) {
                /*
                 * The string may be enclosed by delimiters (e.g. braces, quotes, parentheses) to
                 * avoid configuration string characters acting as separators. Discard the first and
                 * last characters in this case.
                 */
                if (cval.type == WT_CONFIG_ITEM_STRUCT) {
                    cval.str++;
                    cval.len -= 2;
                }
                WT_ERR(__wt_strndup(session, cval.str, cval.len, &filemeta));
                /*
                 * FIXME-WT-7735: Importing a tiered table is not yet allowed.
                 */
                if (__wt_config_getones(session, filemeta, "tiered_object", &cval) == 0 &&
                  cval.val != 0)
                    WT_ERR_MSG(session, ENOTSUP, "%s: import not supported on tiered files", uri);
                filecfg[2] = filemeta;
                /*
                 * If there is a file metadata provided, reconstruct the incremental backup
                 * information as the imported file was not part of any backup.
                 */
                WT_ERR(__wt_reset_blkmod(session, config, buf));
                filecfg[3] = buf->mem;
            } else if (session->import_list == NULL) {
                /*
                 * If there is no file metadata provided, the user should be specifying a "repair".
                 * To prevent mistakes with API usage, we should return an error here rather than
                 * inferring a repair.
                 */
                WT_ERR_MSG(session, EINVAL,
                  "%s: import requires that 'file_metadata' or 'metadata_file' is specified or the "
                  "'repair' option is provided",
                  uri);
            }
        }
    } else
        /* Create the file. */
        WT_ERR(__create_file_block_manager(session, uri, filename, allocsize));

    /*
     * If creating an ordinary file, update the file ID and current version numbers and strip
     * checkpoint LSN from the extracted metadata. If importing an existing file, incremental backup
     * information is reconstructed inside import repair or when grabbing file metadata.
     */
    if (!is_metadata) {
        if (!import_repair) {
            WT_ERR(__wt_scr_alloc(session, 0, &val));
            WT_ERR(__wt_buf_fmt(session, val,
              "id=%" PRIu32 ",version=(major=%" PRIu16 ",minor=%" PRIu16 "),checkpoint_lsn=",
              ++S2C(session)->next_file_id, WT_BTREE_VERSION_MAX.major,
              WT_BTREE_VERSION_MAX.minor));
            for (p = filecfg; *p != NULL; ++p)
                ;
            *p = val->data;
            WT_ERR(__wt_config_collapse(session, filecfg, &fileconf));
        } else {
            /* Try to recreate the associated metadata from the imported data source. */
            WT_ERR(__wt_import_repair(session, uri, &fileconf));
        }
        WT_ERR(__wt_metadata_insert(session, uri, fileconf));

        /*
         * Ensure that the timestamps in the imported data file are not in the future relative to
         * the configured global timestamp.
         */
        if (session->import_list == NULL && import) {
            against_stable =
              __wt_config_getones(session, config, "import.compare_timestamp", &cval) == 0 &&
              (WT_STRING_MATCH("stable", cval.str, cval.len) ||
                WT_STRING_MATCH("stable_timestamp", cval.str, cval.len));
            WT_ERR(__check_imported_ts(session, uri, fileconf, against_stable));
        }
    }

    /*
     * Open the file to check that it was setup correctly. We don't need to pass the configuration,
     * we just wrote the collapsed configuration into the metadata file, and it's going to be
     * read/used by underlying functions.
     *
     * Turn off bulk-load for imported files.
     */
    WT_ERR(__wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));

    if (session->import_list == NULL && import)
        __wt_btree_disable_bulk(session);

    if (WT_META_TRACKING(session))
        WT_ERR(__wt_meta_track_handle_lock(session, true));
    else
        WT_ERR(__wt_session_release_dhandle(session));

err:
    __wt_scr_free(session, &buf);
    __wt_scr_free(session, &val);
    __wt_free(session, fileconf);
    __wt_free(session, filemeta);
    return (ret);
}

/*
 * __wt_schema_colgroup_source --
 *     Get the URI of the data source for a column group.
 */
int
__wt_schema_colgroup_source(
  WT_SESSION_IMPL *session, WT_TABLE *table, const char *cgname, const char *config, WT_ITEM *buf)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    size_t len;
    const char *prefix, *suffix, *tablename;

    tablename = table->iface.name + strlen("table:");
    if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
      !WT_STRING_MATCH("file", cval.str, cval.len)) {
        prefix = cval.str;
        len = cval.len;
        suffix = "";
    } else if ((S2C(session)->bstorage == NULL) ||
      ((ret = __wt_config_getones(session, config, "tiered_storage.name", &cval)) == 0 &&
        cval.len != 0 && WT_STRING_MATCH("none", cval.str, cval.len))) {
        /*
         * If we're using tiered storage, the default is not file unless the user explicitly turns
         * off using tiered storage for this create. Otherwise the default prefix is tiered.
         */
        prefix = "file";
        len = strlen(prefix);
        suffix = ".wt";
    } else {
        prefix = "tiered";
        len = strlen(prefix);
        suffix = "";
    }
    WT_RET_NOTFOUND_OK(ret);

    if (cgname == NULL)
        WT_RET(__wt_buf_fmt(session, buf, "%.*s:%s%s", (int)len, prefix, tablename, suffix));
    else
        WT_RET(
          __wt_buf_fmt(session, buf, "%.*s:%s_%s%s", (int)len, prefix, tablename, cgname, suffix));

    return (0);
}

/*
 * __create_import_cmp_uri --
 *     Qsort function: sort the import entries array by uri.
 */
static int WT_CDECL
__create_import_cmp_uri(const void *a, const void *b)
{
    WT_IMPORT_ENTRY *ae, *be;

    ae = (WT_IMPORT_ENTRY *)a;
    be = (WT_IMPORT_ENTRY *)b;

    return (strcmp(ae->uri, be->uri));
}

/*
 * __create_import_cmp_id --
 *     Qsort function: sort the import entries array by file id.
 */
static int WT_CDECL
__create_import_cmp_id(const void *a, const void *b)
{
    int64_t res;

    WT_IMPORT_ENTRY *ae, *be;

    ae = (WT_IMPORT_ENTRY *)a;
    be = (WT_IMPORT_ENTRY *)b;

    res = ae->file_id - be->file_id;
    if (res < 0)
        return (-1);
    else if (res > 0)
        return (1);
    else
        return (0);
}

/*
 * __wt_find_import_metadata --
 *     Find metadata entry by URI in session's import list. The list must already be sorted by uri.
 */
int
__wt_find_import_metadata(WT_SESSION_IMPL *session, const char *uri, const char **config)
{
    WT_IMPORT_ENTRY entry, *result;

    WT_ASSERT(session, session->import_list != NULL);

    entry.uri = uri;
    entry.config = NULL;
    result = bsearch(&entry, session->import_list->entries, session->import_list->entries_next,
      sizeof(WT_IMPORT_ENTRY), __create_import_cmp_uri);

    if (result == NULL)
        WT_RET_MSG(session, WT_NOTFOUND, "failed to find metadata for %s", uri);

    *config = result->config;

    return (0);
}

/*
 * __create_colgroup --
 *     Create a column group.
 */
static int
__create_colgroup(WT_SESSION_IMPL *session, const char *name, bool exclusive, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_ITEM confbuf, fmt, namebuf;
    WT_TABLE *table;
    size_t tlen;
    char *cgconf, *origconf;
    const char **cfgp, *cfg[4] = {WT_CONFIG_BASE(session, colgroup_meta), config, NULL, NULL};
    const char *cgname, *source, *sourceconf, *tablename;
    const char *sourcecfg[] = {config, NULL, NULL};
    bool exists, tracked;

    sourceconf = NULL;
    cgconf = origconf = NULL;
    WT_CLEAR(fmt);
    WT_CLEAR(confbuf);
    WT_CLEAR(namebuf);
    exists = tracked = false;

    if (session->import_list != NULL)
        WT_RET(__wt_find_import_metadata(session, name, &cfg[1]));

    tablename = name;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "colgroup:");
    cgname = strchr(tablename, ':');
    if (cgname != NULL) {
        tlen = (size_t)(cgname - tablename);
        ++cgname;
    } else
        tlen = strlen(tablename);

    if ((ret = __wt_schema_get_table(
           session, tablename, tlen, true, WT_DHANDLE_EXCLUSIVE, &table)) != 0)
        WT_RET_MSG(session, (ret == WT_NOTFOUND) ? ENOENT : ret,
          "Can't create '%s' for non-existent table '%.*s'", name, (int)tlen, tablename);

    if (WT_META_TRACKING(session)) {
        WT_WITH_DHANDLE(session, &table->iface, ret = __wt_meta_track_handle_lock(session, false));
        WT_ERR(ret);
        tracked = true;
    }

    /*
     * Make sure the column group is referenced from the table, converting not-found errors to
     * EINVAL for the application.
     */
    if (cgname != NULL && (ret = __wt_config_subgets(session, &table->cgconf, cgname, &cval)) != 0)
        WT_ERR_MSG(session, ret == WT_NOTFOUND ? EINVAL : ret,
          "Column group '%s' not found in table '%.*s'", cgname, (int)tlen, tablename);

    /* Check if the column group already exists. */
    if ((ret = __wt_metadata_search(session, name, &origconf)) == 0) {
        if (exclusive)
            WT_ERR(EEXIST);
        exists = true;
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /* Find the first NULL entry in the cfg stack. */
    for (cfgp = &cfg[1]; *cfgp; cfgp++)
        ;

    /* Add the source to the colgroup config before collapsing. */
    if (__wt_config_getones(session, config, "source", &cval) == 0 && cval.len != 0) {
        WT_ERR(__wt_buf_fmt(session, &namebuf, "%.*s", (int)cval.len, cval.str));
        source = namebuf.data;
    } else {
        WT_ERR(__wt_schema_colgroup_source(session, table, cgname, config, &namebuf));
        source = namebuf.data;
        WT_ERR(__wt_buf_fmt(session, &confbuf, "source=\"%s\"", source));
        *cfgp++ = confbuf.data;
    }

    if (session->import_list != NULL)
        /* Use the import configuration, it should have key and value format configurations. */
        WT_ERR(__wt_find_import_metadata(session, source, &sourcecfg[0]));
    else {
        /* Calculate the key/value formats: these go into the source config. */
        WT_ERR(__wt_buf_fmt(session, &fmt, "key_format=%s", table->key_format));
        if (cgname == NULL)
            WT_ERR(__wt_buf_catfmt(session, &fmt, ",value_format=%s", table->value_format));
        else {
            if (__wt_config_getones(session, config, "columns", &cval) != 0)
                WT_ERR_MSG(session, EINVAL, "No 'columns' configuration for '%s'", name);
            WT_ERR(__wt_buf_catfmt(session, &fmt, ",value_format="));
            WT_ERR(__wt_struct_reformat(session, table, cval.str, cval.len, NULL, true, &fmt));
        }

        sourcecfg[1] = fmt.data;
    }

    WT_ERR(__wt_config_merge(session, sourcecfg, NULL, &sourceconf));
    WT_ERR(__wt_schema_create(session, source, sourceconf));

    WT_ERR(__wt_config_collapse(session, cfg, &cgconf));

    if (!exists) {
        WT_ERR(__wt_metadata_insert(session, name, cgconf));
        WT_ERR(__wt_schema_open_colgroups(session, table));
    }

err:
    __wt_free(session, cgconf);
    __wt_free(session, sourceconf);
    __wt_free(session, origconf);
    __wt_buf_free(session, &confbuf);
    __wt_buf_free(session, &fmt);
    __wt_buf_free(session, &namebuf);

    if (!tracked)
        WT_TRET(__wt_schema_release_table(session, &table));
    return (ret);
}

/*
 * __wt_schema_index_source --
 *     Get the URI of the data source for an index.
 */
int
__wt_schema_index_source(
  WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname, const char *config, WT_ITEM *buf)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    size_t len;
    const char *prefix, *suffix, *tablename;

    tablename = table->iface.name + strlen("table:");
    if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
      !WT_STRING_MATCH("file", cval.str, cval.len)) {
        prefix = cval.str;
        len = cval.len;
        suffix = "_idx";
    } else {
        prefix = "file";
        len = strlen(prefix);
        suffix = ".wti";
    }
    WT_RET_NOTFOUND_OK(ret);

    WT_RET(
      __wt_buf_fmt(session, buf, "%.*s:%s_%s%s", (int)len, prefix, tablename, idxname, suffix));

    return (0);
}

/*
 * __fill_index --
 *     Fill the index from the current contents of the table.
 */
static int
__fill_index(WT_SESSION_IMPL *session, WT_TABLE *table, WT_INDEX *idx)
{
    WT_CURSOR *tcur, *icur;
    WT_DECL_RET;
    WT_SESSION *wt_session;

    wt_session = &session->iface;
    tcur = NULL;
    icur = NULL;
    WT_RET(__wt_schema_open_colgroups(session, table));

    /*
     * If the column groups have not been completely created, there cannot be data inserted yet, and
     * we're done.
     */
    if (!table->cg_complete)
        return (0);

    WT_ERR(wt_session->open_cursor(wt_session, idx->source, NULL, "bulk=unordered", &icur));
    WT_ERR(wt_session->open_cursor(wt_session, table->iface.name, NULL, "readonly", &tcur));

    while ((ret = tcur->next(tcur)) == 0)
        WT_ERR(__wt_apply_single_idx(session, idx, icur, (WT_CURSOR_TABLE *)tcur, icur->insert));

    WT_ERR_NOTFOUND_OK(ret, false);
err:
    if (icur)
        WT_TRET(icur->close(icur));
    if (tcur)
        WT_TRET(tcur->close(tcur));
    return (ret);
}

/*
 * __create_index --
 *     Create an index.
 */
static int
__create_index(WT_SESSION_IMPL *session, const char *name, bool exclusive, const char *config)
{
    WT_CONFIG kcols, pkcols;
    WT_CONFIG_ITEM ckey, cval, icols, kval;
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_ITEM confbuf, extra_cols, fmt, namebuf;
    WT_PACK pack;
    WT_TABLE *table;
    size_t tlen;
    u_int i, npublic_cols;
    char *idxconf, *origconf;
    const char *cfg[4] = {WT_CONFIG_BASE(session, index_meta), NULL, NULL, NULL};
    const char *source, *sourceconf, *idxname, *tablename;
    const char *sourcecfg[] = {config, NULL, NULL};
    bool exists, have_extractor;

    sourceconf = NULL;
    idxconf = origconf = NULL;
    WT_CLEAR(confbuf);
    WT_CLEAR(fmt);
    WT_CLEAR(extra_cols);
    WT_CLEAR(namebuf);
    exists = have_extractor = false;

    tablename = name;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "index:");
    idxname = strchr(tablename, ':');
    if (idxname == NULL)
        WT_RET_MSG(
          session, EINVAL, "Invalid index name, should be <table name>:<index name>: %s", name);

    /*
     * Note: it would be better to keep the table exclusive here, while changing its indexes. We
     * don't because some operation we perform below reacquire the table handle (such as opening a
     * cursor on the table in order to fill the index). If we keep the handle exclusive here, those
     * operations wanting ordinary access will conflict, leading to errors. At the same time, we
     * don't want to allow table cursors that have already been fully opened to remain open across
     * this call.
     *
     * Temporarily getting the table exclusively serves the purpose of ensuring that cursors on the
     * table that are already open must at least be closed before this call proceeds.
     */
    tlen = (size_t)(idxname++ - tablename);
    if ((ret = __wt_schema_get_table(
           session, tablename, tlen, true, WT_DHANDLE_EXCLUSIVE, &table)) != 0)
        WT_RET_MSG(session, ret, "Can't create an index for table: %.*s", (int)tlen, tablename);
    WT_RET(__wt_schema_release_table(session, &table));

    if ((ret = __wt_schema_get_table(session, tablename, tlen, true, 0, &table)) != 0)
        WT_RET_MSG(session, ret, "Can't create an index for a non-existent table: %.*s", (int)tlen,
          tablename);

    if (table->is_simple)
        WT_ERR_MSG(session, EINVAL, "%s requires a table with named columns", name);

    /* Check if the index already exists. */
    if ((ret = __wt_metadata_search(session, name, &origconf)) == 0) {
        if (exclusive)
            WT_ERR(EEXIST);
        exists = true;
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    if (__wt_config_getones(session, config, "source", &cval) == 0) {
        WT_ERR(__wt_buf_fmt(session, &namebuf, "%.*s", (int)cval.len, cval.str));
        source = namebuf.data;
    } else {
        WT_ERR(__wt_schema_index_source(session, table, idxname, config, &namebuf));
        source = namebuf.data;

        /* Add the source name to the index config before collapsing. */
        WT_ERR(__wt_buf_catfmt(session, &confbuf, ",source=\"%s\"", source));
    }

    if (__wt_config_getones_none(session, config, "extractor", &cval) == 0 && cval.len != 0) {
        have_extractor = true;
        /*
         * Custom extractors must supply a key format; convert not-found errors to EINVAL for the
         * application.
         */
        if ((ret = __wt_config_getones(session, config, "key_format", &kval)) != 0)
            WT_ERR_MSG(session, ret == WT_NOTFOUND ? EINVAL : 0,
              "%s: custom extractors require a key_format", name);
    }

    /* Calculate the key/value formats. */
    WT_CLEAR(icols);
    if (__wt_config_getones(session, config, "columns", &icols) != 0 && !have_extractor)
        WT_ERR_MSG(session, EINVAL, "%s: requires 'columns' configuration", name);

    /*
     * Count the public columns using the declared columns for normal indices or the key format for
     * custom extractors.
     */
    npublic_cols = 0;
    if (!have_extractor) {
        __wt_config_subinit(session, &kcols, &icols);
        while ((ret = __wt_config_next(&kcols, &ckey, &cval)) == 0)
            ++npublic_cols;
        WT_ERR_NOTFOUND_OK(ret, false);
    } else {
        WT_ERR(__pack_initn(session, &pack, kval.str, kval.len));
        while ((ret = __pack_next(&pack, &pv)) == 0)
            ++npublic_cols;
        WT_ERR_NOTFOUND_OK(ret, false);
    }

    /*
     * The key format for an index is somewhat subtle: the application specifies a set of columns
     * that it will use for the key, but the engine usually adds some hidden columns in order to
     * derive the primary key. These hidden columns are part of the source's key_format, which we
     * are calculating now, but not part of an index cursor's key_format.
     */
    __wt_config_subinit(session, &pkcols, &table->colconf);
    for (i = 0; i < table->nkey_columns && (ret = __wt_config_next(&pkcols, &ckey, &cval)) == 0;
         i++) {
        /*
         * If the primary key column is already in the secondary key, don't add it again.
         */
        if (__wt_config_subgetraw(session, &icols, &ckey, &cval) == 0) {
            if (have_extractor)
                WT_ERR_MSG(session, EINVAL,
                  "an index with a custom extractor may not include primary key columns");
            continue;
        }
        WT_ERR(__wt_buf_catfmt(session, &extra_cols, "%.*s,", (int)ckey.len, ckey.str));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /* Index values are empty: all columns are packed into the index key. */
    WT_ERR(__wt_buf_fmt(session, &fmt, "value_format=,key_format="));

    if (have_extractor) {
        WT_ERR(__wt_buf_catfmt(session, &fmt, "%.*s", (int)kval.len, kval.str));
        WT_CLEAR(icols);
    }

    /*
     * Construct the index key format, or append the primary key columns for custom extractors.
     */
    WT_ERR(__wt_struct_reformat(
      session, table, icols.str, icols.len, (const char *)extra_cols.data, false, &fmt));

    /* Check for a record number index key, which makes no sense. */
    WT_ERR(__wt_config_getones(session, fmt.data, "key_format", &cval));
    if (cval.len == 1 && cval.str[0] == 'r')
        WT_ERR_MSG(
          session, EINVAL, "column-store index may not use the record number as its index key");

    WT_ERR(__wt_buf_catfmt(session, &fmt, ",index_key_columns=%u", npublic_cols));

    sourcecfg[1] = fmt.data;
    WT_ERR(__wt_config_merge(session, sourcecfg, NULL, &sourceconf));

    WT_ERR(__wt_schema_create(session, source, sourceconf));

    cfg[1] = sourceconf;
    cfg[2] = confbuf.data;
    WT_ERR(__wt_config_collapse(session, cfg, &idxconf));

    if (!exists) {
        WT_ERR(__wt_metadata_insert(session, name, idxconf));

        /* Make sure that the configuration is valid. */
        WT_ERR(__wt_schema_open_index(session, table, idxname, strlen(idxname), &idx));

        /* If there is data in the table, fill the index. */
        WT_ERR(__fill_index(session, table, idx));
    }

err:
    __wt_free(session, idxconf);
    __wt_free(session, origconf);
    __wt_free(session, sourceconf);
    __wt_buf_free(session, &confbuf);
    __wt_buf_free(session, &extra_cols);
    __wt_buf_free(session, &fmt);
    __wt_buf_free(session, &namebuf);

    WT_TRET(__wt_schema_release_table(session, &table));
    return (ret);
}

/*
 * __create_table --
 *     Create a table.
 */
static int
__create_table(WT_SESSION_IMPL *session, const char *uri, bool exclusive, const char *config)
{
    WT_CONFIG conf;
    WT_CONFIG_ITEM cgkey, cgval, ckey, cval;
    WT_DECL_RET;
    WT_TABLE *table;
    size_t len;
    int ncolgroups, nkeys;
    char *cgcfg, *cgname, *filecfg, *filename, *importcfg, *tablecfg;
    const char *cfg[4] = {WT_CONFIG_BASE(session, table_meta), config, NULL, NULL};
    const char *tablename;
    bool import, import_repair;

    import = F_ISSET(session, WT_SESSION_IMPORT);
    import_repair = false;

    cgcfg = filecfg = importcfg = tablecfg = NULL;
    cgname = filename = NULL;
    table = NULL;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE));

    tablename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "table:");

    /* Check if the table already exists. */
    if ((ret = __wt_metadata_search(session, uri, &tablecfg)) != WT_NOTFOUND) {
        /*
         * Regardless of the 'exclusive' flag, we should raise an error if we try to import an
         * existing URI rather than just silently returning.
         */
        if (exclusive || import)
            WT_TRET(EEXIST);
        goto err;
    }

    if (import) {
        import_repair =
          __wt_config_getones(session, config, "import.repair", &cval) == 0 && cval.val != 0;
        /*
         * If this is an import but not a repair, check that the exported table metadata is provided
         * in the config.
         */
        if (session->import_list != NULL)
            WT_ERR(__wt_find_import_metadata(session, uri, &cfg[1]));
        else if (!import_repair) {
            __wt_config_init(session, &conf, config);
            for (nkeys = 0; (ret = __wt_config_next(&conf, &ckey, &cval)) == 0; nkeys++)
                ;
            if (nkeys == 1)
                WT_ERR_MSG(session, EINVAL,
                  "%s: import requires that the table configuration is specified or the "
                  "'repair' option is provided",
                  uri);
            WT_ERR_NOTFOUND_OK(ret, false);
        } else {
            /* Try to recreate the associated metadata from the imported data source. */
            len = strlen("file:") + strlen(tablename) + strlen(".wt") + 1;
            WT_ERR(__wt_calloc_def(session, len, &filename));
            WT_ERR(__wt_snprintf(filename, len, "file:%s.wt", tablename));
            WT_ERR(__wt_import_repair(session, filename, &filecfg));
            cfg[2] = filecfg;
        }
    }

    WT_ERR(__wt_config_gets(session, cfg, "colgroups", &cval));
    __wt_config_subinit(session, &conf, &cval);
    for (ncolgroups = 0; (ret = __wt_config_next(&conf, &cgkey, &cgval)) == 0; ncolgroups++)
        ;
    WT_ERR_NOTFOUND_OK(ret, false);

    WT_ERR(__wt_config_collapse(session, cfg, &tablecfg));
    WT_ERR(__wt_metadata_insert(session, uri, tablecfg));

    if (ncolgroups == 0) {
        len = strlen("colgroup:") + strlen(tablename) + 1;
        WT_ERR(__wt_calloc_def(session, len, &cgname));
        WT_ERR(__wt_snprintf(cgname, len, "colgroup:%s", tablename));
        if (import_repair) {
            len =
              strlen(tablecfg) + strlen(",import=(enabled,file_metadata=())") + strlen(filecfg) + 1;
            WT_ERR(__wt_calloc_def(session, len, &importcfg));
            WT_ERR(__wt_snprintf(
              importcfg, len, "%s,import=(enabled,file_metadata=(%s))", tablecfg, filecfg));
            cfg[2] = importcfg;
            WT_ERR(__wt_config_collapse(session, &cfg[1], &cgcfg));
            WT_ERR(__create_colgroup(session, cgname, exclusive, cgcfg));
        } else
            WT_ERR(__create_colgroup(session, cgname, exclusive, config));
    }

    /*
     * Open the table to check that it was setup correctly. Keep the handle exclusive until it is
     * released at the end of the call.
     */
    WT_ERR(__wt_schema_get_table_uri(session, uri, true, WT_DHANDLE_EXCLUSIVE, &table));
    if (WT_META_TRACKING(session)) {
        WT_WITH_DHANDLE(session, &table->iface, ret = __wt_meta_track_handle_lock(session, true));
        WT_ERR(ret);
        table = NULL;
    }

err:
    WT_TRET(__wt_schema_release_table(session, &table));
    __wt_free(session, cgcfg);
    __wt_free(session, cgname);
    __wt_free(session, filecfg);
    __wt_free(session, filename);
    __wt_free(session, importcfg);
    __wt_free(session, tablecfg);
    return (ret);
}

/*
 * __create_object --
 *     Create a tiered object for the given name.
 */
static int
__create_object(WT_SESSION_IMPL *session, const char *uri, bool exclusive, const char *config)
{
    WT_UNUSED(exclusive);
    WT_RET(__wt_metadata_insert(session, uri, config));
    return (0);
}

/*
 * __wt_tiered_tree_create --
 *     Create a tiered tree structure for the given name.
 */
int
__wt_tiered_tree_create(
  WT_SESSION_IMPL *session, const char *uri, bool exclusive, const char *config)
{
    WT_UNUSED(exclusive);
    WT_RET(__wt_metadata_insert(session, uri, config));
    return (0);
}

/*
 * __create_tiered --
 *     Create a tiered tree structure for the given name.
 */
static int
__create_tiered(WT_SESSION_IMPL *session, const char *uri, bool exclusive, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TIERED *tiered;
    char *meta_value;
    const char *cfg[5] = {WT_CONFIG_BASE(session, tiered_meta), NULL, NULL, NULL, NULL};
    const char *metadata;
    bool free_metadata;

    conn = S2C(session);
    metadata = NULL;
    tiered = NULL;
    free_metadata = true;

    /* Check if the tiered table already exists. */
    if ((ret = __wt_metadata_search(session, uri, &meta_value)) != WT_NOTFOUND) {
        if (exclusive)
            WT_TRET(EEXIST);
        goto err;
    }
    WT_RET_NOTFOUND_OK(ret);

    /*
     * We're creating a tiered table. Set the initial tiers list to empty. Opening the table will
     * cause us to create our first file or tiered object.
     */
    if (!F_ISSET(conn, WT_CONN_READONLY)) {
        if (session->import_list != NULL) {
            WT_RET(__wt_find_import_metadata(session, uri, &metadata));
            free_metadata = false;
        } else {
            WT_RET(__wt_scr_alloc(session, 0, &tmp));
            /*
             * By default use the connection level bucket and prefix. Then we add in any user
             * configuration that may override the system one.
             */
            WT_ERR(__wt_buf_fmt(session, tmp,
              ",tiered_storage=(bucket=%s,bucket_prefix=%s)"
              ",id=%" PRIu32 ",version=(major=%" PRIu16 ",minor=%" PRIu16 "),checkpoint_lsn=",
              conn->bstorage->bucket, conn->bstorage->bucket_prefix, ++conn->next_file_id,
              WT_BTREE_VERSION_MAX.major, WT_BTREE_VERSION_MAX.minor));
            cfg[1] = tmp->data;
            cfg[2] = config;
            cfg[3] = "tiers=()";
            WT_ERR(__wt_config_merge(session, cfg, NULL, &metadata));
        }

        WT_ERR(__wt_metadata_insert(session, uri, metadata));
    }
    WT_ERR(__wt_schema_get_tiered_uri(session, uri, WT_DHANDLE_EXCLUSIVE, &tiered));
    if (WT_META_TRACKING(session)) {
        WT_WITH_DHANDLE(session, &tiered->iface, ret = __wt_meta_track_handle_lock(session, true));
        WT_ERR(ret);
        tiered = NULL;
    }

err:
    WT_TRET(__wt_schema_release_tiered(session, &tiered));
    __wt_scr_free(session, &tmp);
    __wt_free(session, meta_value);
    if (free_metadata)
        __wt_free(session, metadata);

    return (ret);
}

/*
 * __create_data_source --
 *     Create a custom data source.
 */
static int
__create_data_source(
  WT_SESSION_IMPL *session, const char *uri, const char *config, WT_DATA_SOURCE *dsrc)
{
    WT_CONFIG_ITEM cval;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_create), config, NULL};

    /*
     * Check to be sure the key/value formats are legal: the underlying data source doesn't have
     * access to the functions that check.
     */
    WT_RET(__wt_config_gets(session, cfg, "key_format", &cval));
    WT_RET(__wt_struct_confchk(session, &cval));
    WT_RET(__wt_config_gets(session, cfg, "value_format", &cval));
    WT_RET(__wt_struct_confchk(session, &cval));

    /*
     * User-specified collators aren't supported for data-source objects.
     */
    if (__wt_config_getones_none(session, config, "collator", &cval) != WT_NOTFOUND &&
      cval.len != 0)
        WT_RET_MSG(session, EINVAL, "WT_DATA_SOURCE objects do not support WT_COLLATOR ordering");

    return (dsrc->create(dsrc, &session->iface, uri, (WT_CONFIG_ARG *)cfg));
}

/*
 * __create_meta_entry_worker --
 *     Worker function for metadata file reader procedure. The function populates the import list
 *     with entries related to the import URI.
 */
static int
__create_meta_entry_worker(WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value, void *state)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_IMPORT_LIST *import_list;
    const char *meta_key, *meta_key_suffix, *meta_value;

    import_list = (WT_IMPORT_LIST *)state;
    meta_key = (const char *)key->data;
    meta_value = (const char *)value->data;

    /* Get suffix of the key. */
    meta_key_suffix = strchr(meta_key, ':');
    WT_ASSERT(session, meta_key_suffix != NULL && meta_key_suffix[1] != '\0');
    ++meta_key_suffix;

    /*
     * We want to skip unrelated entries. We have stripped out the URI prefixes and want to get all
     * the entries that match the URI. This check will match overlapping entries (i.e. if we're
     * importing table:name but name123 also exists) but should reduce the resources needed for the
     * list of possible entries.
     */
    if (!WT_PREFIX_MATCH(meta_key_suffix, import_list->uri_suffix))
        return (0);

    /*
     * We are not checking if the entry already exists in the metadata. It will be handled later in
     * the appropriate create call.
     */

    /* Grow the entries array if needed. */
    WT_RET(__wt_realloc_def(session, &import_list->entries_allocated, import_list->entries_next + 1,
      &import_list->entries));

    /* Populate the next entry. */
    WT_RET(__wt_strndup(
      session, meta_key, key->size, &import_list->entries[import_list->entries_next].uri));
    WT_RET(__wt_strndup(
      session, meta_value, value->size, &import_list->entries[import_list->entries_next].config));

    ret = __wt_config_getones(
      session, import_list->entries[import_list->entries_next].config, "id", &cval);
    WT_RET_NOTFOUND_OK(ret);
    if (ret == WT_NOTFOUND || cval.len == 0)
        import_list->entries[import_list->entries_next].file_id = WT_IMPORT_INVALID_FILE_ID;
    else
        import_list->entries[import_list->entries_next].file_id = cval.val;

    import_list->entries_next++;

    return (0);
}

/*
 * __create_fix_file_ids --
 *     Update file IDs in the import list according to the session's next file ID field. Certain
 *     entries in the import list have same file ID and we need to preserve this relationships.
 */
static int
__create_fix_file_ids(WT_SESSION_IMPL *session, WT_IMPORT_LIST *import_list)
{
    WT_CONNECTION_IMPL *conn;
    size_t i;
    int64_t new_file_id, prev_file_id;
    char fileid_cfg[64], *config_tmp;
    const char *cfg[3] = {NULL, NULL, NULL};

    config_tmp = NULL;
    new_file_id = prev_file_id = -1;
    conn = S2C(session);

    /* Sort the array of entries by file ID. */
    __wt_qsort(import_list->entries, import_list->entries_next, sizeof(WT_IMPORT_ENTRY),
      __create_import_cmp_id);

    /* Iterate over the array and assign a new ID to each entry. */
    for (i = 0; i < import_list->entries_next; ++i) {
        /* Skip entries without file id. */
        if (import_list->entries[i].file_id == WT_IMPORT_INVALID_FILE_ID)
            continue;

        /* Generate a new file ID. */
        if (import_list->entries[i].file_id != prev_file_id) {
            prev_file_id = import_list->entries[i].file_id;
            new_file_id = ++conn->next_file_id;
        }

        /* Update config with the new file ID. */
        WT_RET(__wt_snprintf(fileid_cfg, sizeof(fileid_cfg), "id=%" PRIu32, (uint32_t)new_file_id));
        cfg[0] = import_list->entries[i].config;
        cfg[1] = fileid_cfg;
        WT_RET(__wt_config_collapse(session, cfg, &config_tmp));
        __wt_free(session, import_list->entries[i].config);
        import_list->entries[i].config = config_tmp;
        import_list->entries[i].file_id = new_file_id;
    }

    return (0);
}

/*
 * __create_parse_export --
 *     Parse export metadata file and populate array of name/config entries related to uri. The
 *     array is sorted by entry name. Caller is responsible to free any memory allocated for the
 *     import list.
 */
static int
__create_parse_export(
  WT_SESSION_IMPL *session, const char *export_file, WT_IMPORT_LIST *import_list)
{
    bool exist;

    exist = false;

    /* Open the specified metadata file and iterate over the key value pairs. */
    WT_RET(__wt_read_metadata_file(
      session, export_file, __create_meta_entry_worker, import_list, &exist));
    if (!exist)
        return (0);

    /* Fix file IDs so that they fit into the recipient system. */
    WT_RET(__create_fix_file_ids(session, import_list));

    /* Sort the array by name. We will use binary search later to get config string. */
    __wt_qsort(import_list->entries, import_list->entries_next, sizeof(WT_IMPORT_ENTRY),
      __create_import_cmp_uri);

    return (0);
}

/*
 * __schema_create_config_check --
 *     Detects any invalid config combinations for schema create.
 */
static int
__schema_create_config_check(
  WT_SESSION_IMPL *session, const char *uri, const char *config, bool import)
{
    WT_CONFIG_ITEM cval;
    bool file_metadata, is_tiered, tiered_name_set;

    file_metadata =
      __wt_config_getones(session, config, "import.file_metadata", &cval) == 0 && cval.val != 0;

    if (import && session->import_list == NULL && !WT_PREFIX_MATCH(uri, "file:") &&
      !WT_PREFIX_MATCH(uri, "table:"))
        WT_RET_MSG(session, ENOTSUP,
          "%s: import is only supported for 'file' and 'table' data sources", uri);

    /*
     * If tiered storage is configured at the connection level and the user has not configured
     * tiered_storage.name to be none, then the object being created is a tiered object.
     */
    tiered_name_set =
      __wt_config_getones(session, config, "tiered_storage.name", &cval) == 0 && cval.len != 0;
    is_tiered = S2C(session)->bstorage != NULL &&
      (!tiered_name_set || !WT_STRING_MATCH("none", cval.str, cval.len));

    /* The import.file_metadata configuration is incompatible with tiered storage. */
    if (is_tiered && file_metadata)
        WT_RET_MSG(session, EINVAL,
          "import for tiered storage is incompatible with the 'file_metadata' setting");

    /*
     * If the type configuration is set to anything but "file" while using tiered storage we must
     * fail the operation.
     */
    if (is_tiered && __wt_config_getones(session, config, "type", &cval) == 0 &&
      !WT_STRING_MATCH("file", cval.str, cval.len))
        WT_RET_MSG(session, ENOTSUP,
          "unsupported type configuration: %.*s: type must be file for tiered storage",
          (int)cval.len, cval.str);

    return (0);
}

/*
 * __schema_create --
 *     Process a WT_SESSION::create operation for all supported types.
 */
static int
__schema_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;
    WT_IMPORT_LIST import_list;
    size_t i;
    char *export_file;
    bool clear_import_flag, exclusive, import;

    WT_CLEAR(import_list);
    export_file = NULL;
    clear_import_flag = false;

    exclusive = __wt_config_getones(session, config, "exclusive", &cval) == 0 && cval.val != 0;
    import = session->import_list != NULL ||
      (__wt_config_getones(session, config, "import.enabled", &cval) == 0 && cval.val != 0);

    WT_RET(__schema_create_config_check(session, uri, config, import));

    /*
     * We track create operations: if we fail in the middle of creating a complex object, we want to
     * back it all out.
     */
    WT_RET(__wt_meta_track_on(session));
    if (import) {
        if (!F_ISSET(session, WT_SESSION_IMPORT)) {
            F_SET(session, WT_SESSION_IMPORT);
            /* This method is called recursively. Clear the flag only in the call that set it. */
            clear_import_flag = true;
        }

        if (session->import_list == NULL &&
          __wt_config_getones(session, config, "import.metadata_file", &cval) == 0 &&
          cval.len != 0 && (cval.type == WT_CONFIG_ITEM_STRING || cval.type == WT_CONFIG_ITEM_ID)) {
            WT_ERR(__wt_strndup(session, cval.str, cval.len, &export_file));
            import_list.uri = uri;

            /* Get suffix of the URI. */
            import_list.uri_suffix = strchr(uri, ':');
            WT_ASSERT(session, import_list.uri_suffix != NULL && import_list.uri_suffix[1] != '\0');
            ++import_list.uri_suffix;

            WT_ERR(__create_parse_export(session, export_file, &import_list));

            WT_ASSERT(session, session->import_list == NULL);
            session->import_list = &import_list;
        }
    }

    if (WT_PREFIX_MATCH(uri, "colgroup:"))
        ret = __create_colgroup(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "file:"))
        ret = __create_file(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "lsm:"))
        ret = __wt_lsm_tree_create(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "index:"))
        ret = __create_index(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "object:"))
        ret = __create_object(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "table:"))
        ret = __create_table(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "tier:"))
        ret = __wt_tiered_tree_create(session, uri, exclusive, config);
    else if (WT_PREFIX_MATCH(uri, "tiered:"))
        ret = __create_tiered(session, uri, exclusive, config);
    else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
        ret = dsrc->create == NULL ? __wt_object_unsupported(session, uri) :
                                     __create_data_source(session, uri, config, dsrc);
    else
        ret = __wt_bad_object_type(session, uri);

err:
    session->dhandle = NULL;
    if (clear_import_flag)
        F_CLR(session, WT_SESSION_IMPORT);

    WT_TRET(__wt_meta_track_off(session, true, ret != 0));

    if (import_list.entries_allocated > 0)
        session->import_list = NULL;

    for (i = 0; i < import_list.entries_next; ++i) {
        __wt_free(session, import_list.entries[i].uri);
        __wt_free(session, import_list.entries[i].config);
    }

    __wt_free(session, import_list.entries);
    __wt_free(session, export_file);

    return (ret);
}

/*
 * __wt_schema_create --
 *     Process a WT_SESSION::create operation for all supported types.
 */
int
__wt_schema_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *int_session;

    WT_RET(__wt_schema_internal_session(session, &int_session));
    ret = __schema_create(int_session, uri, config);
    WT_TRET(__wt_schema_session_release(session, int_session));
    return (ret);
}

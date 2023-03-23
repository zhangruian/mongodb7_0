# Create entries used by our doxygen filter to expand the arch_page
# macros in the documentation.

class ArchDocPage:
    def __init__(self, doxygen_name, data_structures, files):
        self.doxygen_name = doxygen_name
        self.data_structures = data_structures
        self.files = files

##########################################
# List of all architecture subsections
##########################################
arch_doc_pages = [
    ArchDocPage('arch-backup',
        ['WT_CURSOR_BACKUP'],
        ['src/cursor/cur_backup.c', 'src/cursor/cur_backup_incr.c']),
    ArchDocPage('arch-block',
        ['WT_BLOCK', 'WT_BLOCK_CKPT', 'WT_BLOCK_DESC', 'WT_BLOCK_HEADER',
         'WT_BM', 'WT_EXTLIST'],
        ['src/include/block.h', 'src/include/block_inline.h',
         'src/block/']),
    ArchDocPage('arch-btree',
        ['WT_BTREE', 'WT_PAGE'],
        ['src/include/btmem.h', 'src/include/btree.h',
         'src/btree/bt_cursor.c', 'src/btree/bt_delete.c',
         'src/btree/bt_page.c', 'src/btree/bt_read.c']),
    ArchDocPage('arch-cache',
        ['WT_CACHE', 'WT_CACHE_POOL', 'WT_COL', 'WT_COL_RLE', 'WT_INSERT', 'WT_PAGE',
         'WT_PAGE_MODIFY', 'WT_REF', 'WT_ROW', 'WT_UPDATE'],
        ['src/include/btmem.h', 'src/include/cache.h', 'src/include/cache_inline.h',
         'src/conn/conn_cache.c', 'src/conn/conn_cache_pool.c']),
    ArchDocPage('arch-checkpoint',
        ['WT_CONNECTION'],
        ['src/block/block_ckpt.c', 'src/block/block_ckpt_scan.c',
         'src/conn/conn_ckpt.c', 'src/meta/meta_ckpt.c',
         'src/txn/txn_ckpt.c']),
    ArchDocPage('arch-compact',
        ['WT_BLOCK'],
        ['src/block/block_compact.c', 'src/btree/bt_compact.c']),
    ArchDocPage('arch-connection',
        ['WT_CONNECTION'],
        ['src/include/connection.h']),
    ArchDocPage('arch-cursor',
        ['WT_CURSOR', 'WT_CURSOR_BACKUP', 'WT_CURSOR_BTREE', 'WT_CURSOR_BULK',
         'WT_CURSOR_DATA_SOURCE', 'WT_CURSOR_DUMP', 'WT_CURSOR_INDEX',
         'WT_CURSOR_LOG', 'WT_CURSOR_METADATA', 'WT_CURSOR_STAT'],
        ['src/include/cursor.h', 'src/include/cursor_inline.h',
         'src/cursor/']),
    ArchDocPage('arch-data-file',
        ['WT_CELL'],
        ['src/include/block.h', 'src/include/btmem.h',
         'src/include/cell.h', 'src/include/cell_inline.h',
         'src/reconcile/rec_col.c', 'src/reconcile/rec_row.c']),
    ArchDocPage('arch-dhandle',
        ['WT_DHANDLE'],
        ['src/include/dhandle.h', 'src/conn/conn_dhandle.c',
         'src/session/session_dhandle.c']),
    ArchDocPage('arch-eviction',
        ['WT_EVICT_ENTRY', 'WT_EVICT_QUEUE'],
        ['src/include/cache.h',
         'src/evict/']),
    ArchDocPage('arch-fast-truncate',
        ['WT_PAGE_DELETED'],
        # It would be nice to have this link to the list of places at the bottom of the page
        # (since there are a _lot_ of places in the tree that truncate support appears) but
        # s_docs only accepts source files here. The choices seem to be listing them all
        # (which loses the fact that bt_delete.c is the main place because it is required to
        # be sorted into the middle of the list) or just listing bt_delete.c, and the latter
        # seems like the better choice given the constraints.
        ['src/btree/bt_delete.c']),
    ArchDocPage('arch-fs-os',
        ['WT_FILE_HANDLE', 'WT_FILE_SYSTEM'],
        ['src/include/os.h', 'src/include/os_fhandle_inline.h',
         'src/include/os_fs_inline.h', 'src/include/os_fstream_inline.h',
         'src/include/os_windows.h', 'src/include/posix.h',
         'src/os_common/', 'src/os_posix/', 'src/os_win/',
         ]),
    ArchDocPage('arch-hs',
        ['WT_CURSOR_HS'],
        ['src/history/']),
    ArchDocPage('arch-log-file',
        ['WT_LOGSLOT', 'WT_LOG_RECORD', 'WT_LSN'],
        ['src/include/log.h', 'src/log/']),
    ArchDocPage('arch-logging',
        ['WT_CURSOR_LOG', 'WT_LOG', 'WT_LOGSLOT', 'WT_LOG_RECORD', 'WT_LSN'],
        ['src/include/log.h', 'src/include/log_inline.h', 'src/log/']),
    ArchDocPage('arch-metadata',
        [],
        ['src/include/meta.h', 'src/meta/']),
    ArchDocPage('arch-python',
        [],
        ['lang/python/']),
    ArchDocPage('arch-row-column',
        ['WT_BTREE'],
        ['src/include/btree.h']),
    ArchDocPage('arch-rts',
        [''],
        ['src/rollback_to_stable/']),
    ArchDocPage('arch-s3-extension',
        ['S3_FILE_HANDLE', 'S3_FILE_SYSTEM', 'S3_STORAGE'],
        ['ext/storage_sources/s3_store/']),
    ArchDocPage('arch-schema',
        ['WT_COLGROUP', 'WT_INDEX', 'WT_LSM_TREE', 'WT_TABLE'],
        ['src/include/intpack_inline.h', 'src/include/packing_inline.h',
         'src/include/schema.h',
         'src/lsm/', 'src/packing/', 'src/schema/']),
    ArchDocPage('arch-session',
        ['WT_SESSION'],
        ['src/include/session.h']),
    ArchDocPage('arch-snapshot',
        ['WT_TXN'],
        ['src/include/txn.h']),
    ArchDocPage('arch-timestamp',
        ['WT_TIME_AGGREGATE', 'WT_TIME_WINDOW'],
        ['src/include/timestamp.h', 'src/include/timestamp_inline.h']),
    ArchDocPage('arch-transaction',
        ['WT_TXN', 'WT_TXN_GLOBAL', 'WT_TXN_OP', 'WT_TXN_SHARED'],
        ['src/include/txn.h', 'src/include/txn_inline.h', 'src/txn/']),
]

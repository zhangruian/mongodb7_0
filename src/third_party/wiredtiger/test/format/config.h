/* DO NOT EDIT: automatically built by format/config.sh. */

#define C_TYPE_MATCH(cp, type)                                                                    \
    (!F_ISSET(cp, (C_TYPE_FIX | C_TYPE_ROW | C_TYPE_VAR)) ||                                      \
      ((type) == FIX && F_ISSET(cp, C_TYPE_FIX)) || ((type) == ROW && F_ISSET(cp, C_TYPE_ROW)) || \
      ((type) == VAR && F_ISSET(cp, C_TYPE_VAR)))

typedef struct {
    const char *name; /* Configuration item */
    const char *desc; /* Configuration description */

#define C_BOOL 0x001u        /* Boolean (true if roll of 1-to-100 is <= CONFIG->min) */
#define C_IGNORE 0x002u      /* Not a simple randomization, configured specially */
#define C_STRING 0x004u      /* String (rather than integral) */
#define C_TABLE 0x008u       /* Value is per table, not global */
#define C_TYPE_FIX 0x010u    /* Value is only relevant to FLCS */
#define C_TYPE_LSM 0x020u    /* Value is only relevant to LSM */
#define C_TYPE_ROW 0x040u    /* Value is only relevant to RS */
#define C_TYPE_VAR 0x080u    /* Value is only relevant to VLCS */
#define C_ZERO_NOTSET 0x100u /* Ignore zero values */
    uint32_t flags;

    uint32_t min;     /* Minimum value */
    uint32_t maxrand; /* Maximum value randomly chosen */
    uint32_t maxset;  /* Maximum value explicitly set */

    u_int off; /* Value offset */
} CONFIG;

#define V_MAX_TABLES_CONFIG 1000

#define V_GLOBAL_ASSERT_READ_TIMESTAMP 0
#define V_GLOBAL_BACKUP 1
#define V_GLOBAL_BACKUP_INCREMENTAL 2
#define V_GLOBAL_BACKUP_INCR_GRANULARITY 3
#define V_GLOBAL_BLOCK_CACHE 4
#define V_GLOBAL_BLOCK_CACHE_CACHE_ON_CHECKPOINT 5
#define V_GLOBAL_BLOCK_CACHE_CACHE_ON_WRITES 6
#define V_GLOBAL_BLOCK_CACHE_SIZE 7
#define V_TABLE_BTREE_BITCNT 8
#define V_TABLE_BTREE_COMPRESSION 9
#define V_TABLE_BTREE_DICTIONARY 10
#define V_TABLE_BTREE_HUFFMAN_VALUE 11
#define V_TABLE_BTREE_INTERNAL_KEY_TRUNCATION 12
#define V_TABLE_BTREE_INTERNAL_PAGE_MAX 13
#define V_TABLE_BTREE_KEY_MAX 14
#define V_TABLE_BTREE_KEY_MIN 15
#define V_TABLE_BTREE_LEAF_PAGE_MAX 16
#define V_TABLE_BTREE_MEMORY_PAGE_MAX 17
#define V_TABLE_BTREE_PREFIX_LEN 18
#define V_TABLE_BTREE_PREFIX_COMPRESSION 19
#define V_TABLE_BTREE_PREFIX_COMPRESSION_MIN 20
#define V_TABLE_BTREE_REPEAT_DATA_PCT 21
#define V_TABLE_BTREE_REVERSE 22
#define V_TABLE_BTREE_SPLIT_PCT 23
#define V_TABLE_BTREE_VALUE_MAX 24
#define V_TABLE_BTREE_VALUE_MIN 25
#define V_GLOBAL_CACHE 26
#define V_GLOBAL_CACHE_EVICT_MAX 27
#define V_GLOBAL_CACHE_MINIMUM 28
#define V_GLOBAL_CHECKPOINT 29
#define V_GLOBAL_CHECKPOINT_LOG_SIZE 30
#define V_GLOBAL_CHECKPOINT_WAIT 31
#define V_TABLE_DISK_CHECKSUM 32
#define V_GLOBAL_DISK_DATA_EXTEND 33
#define V_GLOBAL_DISK_DIRECT_IO 34
#define V_GLOBAL_DISK_ENCRYPTION 35
#define V_TABLE_DISK_FIRSTFIT 36
#define V_GLOBAL_DISK_MMAP 37
#define V_GLOBAL_DISK_MMAP_ALL 38
#define V_GLOBAL_FORMAT_ABORT 39
#define V_GLOBAL_FORMAT_INDEPENDENT_THREAD_RNG 40
#define V_GLOBAL_FORMAT_MAJOR_TIMEOUT 41
#define V_GLOBAL_IMPORT 42
#define V_GLOBAL_LOGGING 43
#define V_GLOBAL_LOGGING_COMPRESSION 44
#define V_GLOBAL_LOGGING_FILE_MAX 45
#define V_GLOBAL_LOGGING_PREALLOC 46
#define V_GLOBAL_LOGGING_REMOVE 47
#define V_TABLE_LSM_AUTO_THROTTLE 48
#define V_TABLE_LSM_BLOOM 49
#define V_TABLE_LSM_BLOOM_BIT_COUNT 50
#define V_TABLE_LSM_BLOOM_HASH_COUNT 51
#define V_TABLE_LSM_BLOOM_OLDEST 52
#define V_TABLE_LSM_CHUNK_SIZE 53
#define V_TABLE_LSM_MERGE_MAX 54
#define V_GLOBAL_LSM_WORKER_THREADS 55
#define V_GLOBAL_OPS_ALTER 56
#define V_GLOBAL_OPS_COMPACTION 57
#define V_GLOBAL_OPS_HS_CURSOR 58
#define V_TABLE_OPS_PCT_DELETE 59
#define V_TABLE_OPS_PCT_INSERT 60
#define V_TABLE_OPS_PCT_MODIFY 61
#define V_TABLE_OPS_PCT_READ 62
#define V_TABLE_OPS_PCT_WRITE 63
#define V_GLOBAL_OPS_PREPARE 64
#define V_GLOBAL_OPS_RANDOM_CURSOR 65
#define V_GLOBAL_OPS_SALVAGE 66
#define V_TABLE_OPS_TRUNCATE 67
#define V_GLOBAL_OPS_VERIFY 68
#define V_GLOBAL_QUIET 69
#define V_GLOBAL_RUNS_IN_MEMORY 70
#define V_GLOBAL_RUNS_OPS 71
#define V_TABLE_RUNS_MIRROR 72
#define V_TABLE_RUNS_ROWS 73
#define V_TABLE_RUNS_SOURCE 74
#define V_GLOBAL_RUNS_TABLES 75
#define V_GLOBAL_RUNS_THREADS 76
#define V_GLOBAL_RUNS_TIMER 77
#define V_TABLE_RUNS_TYPE 78
#define V_GLOBAL_RUNS_VERIFY_FAILURE_DUMP 79
#define V_GLOBAL_STATISTICS 80
#define V_GLOBAL_STATISTICS_SERVER 81
#define V_GLOBAL_STRESS_AGGRESSIVE_SWEEP 82
#define V_GLOBAL_STRESS_CHECKPOINT 83
#define V_GLOBAL_STRESS_CHECKPOINT_EVICT_PAGE 84
#define V_GLOBAL_STRESS_CHECKPOINT_RESERVED_TXNID_DELAY 85
#define V_GLOBAL_STRESS_CHECKPOINT_PREPARE 86
#define V_GLOBAL_STRESS_EVICT_REPOSITION 87
#define V_GLOBAL_STRESS_FAILPOINT_EVICTION_FAIL_AFTER_RECONCILIATION 88
#define V_GLOBAL_STRESS_FAILPOINT_HS_DELETE_KEY_FROM_TS 89
#define V_GLOBAL_STRESS_HS_CHECKPOINT_DELAY 90
#define V_GLOBAL_STRESS_HS_SEARCH 91
#define V_GLOBAL_STRESS_HS_SWEEP 92
#define V_GLOBAL_STRESS_SPLIT_1 93
#define V_GLOBAL_STRESS_SPLIT_2 94
#define V_GLOBAL_STRESS_SPLIT_3 95
#define V_GLOBAL_STRESS_SPLIT_4 96
#define V_GLOBAL_STRESS_SPLIT_5 97
#define V_GLOBAL_STRESS_SPLIT_6 98
#define V_GLOBAL_STRESS_SPLIT_7 99
#define V_GLOBAL_TRANSACTION_IMPLICIT 100
#define V_GLOBAL_TRANSACTION_TIMESTAMPS 101
#define V_GLOBAL_WIREDTIGER_CONFIG 102
#define V_GLOBAL_WIREDTIGER_RWLOCK 103
#define V_GLOBAL_WIREDTIGER_LEAK_MEMORY 104

#define V_ELEMENT_COUNT 105

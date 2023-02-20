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
#include <map>
#include <memory>
#include <shared_mutex>
#include <ostream>
#include <set>
#include <string>
#include <vector>
extern "C" {
#include <unistd.h>
#include "workgen_func.h"
#include <math.h>
}
#include "workgen_time.h"

namespace workgen {

/*
 * A 'tint' or ('table integer') is a unique small value integer
 * assigned to each table URI in use. Currently, we assign it once,
 * and its value persists through the lifetime of the Context.
 */
typedef uint32_t tint_t;

struct ThreadRunner;
struct WorkloadRunner;

struct WorkgenTimeStamp {
    WorkgenTimeStamp() {}

    static uint64_t get_timestamp_lag(double seconds) {
        uint64_t start_time;
        workgen_clock(&start_time);

        return (ns_to_us(start_time) - secs_us(seconds));
    }

    static void sleep(double seconds) {
        usleep(ceil(secs_us(seconds)));
    }

    static uint64_t get_timestamp() {
        uint64_t start_time;
        workgen_clock(&start_time);
        return (ns_to_us(start_time));
    }
};

/*
 * A exception generated by the workgen classes. Methods generally return an
 * int errno, so this is useful primarily for notifying the caller about
 * failures in constructors.
 */
struct WorkgenException {
    WorkgenException() = default;
    WorkgenException(int err, const std::string& msg) {
	if (err != 0)
	    _str += wiredtiger_strerror(err);
	if (!msg.empty()) {
	    if (!_str.empty())
                _str += ": ";
            _str += msg;
        }
    }
    ~WorkgenException() = default;
    std::string _str;
};

struct Throttle {
    ThreadRunner &_runner;
    double _throttle;                          // operations per second
    double _burst;
    timespec _next_div;
    int64_t _ops_delta;
    uint64_t _ops_prev;                        // previously returned value
    uint64_t _ops_per_div;                     // statically calculated.
    uint64_t _ms_per_div;                      // statically calculated.
    double _ops_left_this_second;              // ops left to go this second
    uint_t _div_pos;                           // count within THROTTLE_PER_SEC
    bool _started;

    Throttle(ThreadRunner &runner, double throttle, double burst);
    ~Throttle() = default;

    /*
     * Called with the number of operations since the last throttle.
     * Sleeps for any needed amount and returns the number operations the
     * caller should perform before the next call to throttle.
     */
    int throttle(uint64_t op_count, uint64_t *op_limit);
};

// There is one of these per Thread object. It exists for the duration of a
// call to Workload::run() method.
struct ThreadRunner {
    int _errno;
    WorkgenException _exception;
    Thread *_thread;
    Context *_context;
    ContextInternal *_icontext;
    Workload *_workload;
    WorkloadRunner *_wrunner;
    workgen_random_state *_rand_state;
    Throttle *_throttle;
    uint64_t _throttle_ops;
    uint64_t _throttle_limit;
    uint64_t _start_time_us;
    uint64_t _op_time_us;   // time that current operation starts
    bool _in_transaction;
    uint32_t _number;
    Stats _stats;

    typedef enum {
	USAGE_READ = 0x1, USAGE_WRITE = 0x2, USAGE_MIXED = 0x4 } Usage;
    std::map<tint_t, uint32_t> _table_usage;       // value is Usage
    WT_CURSOR **_cursors;                          // indexed by tint_t
    volatile bool _stop;
    WT_SESSION *_session;
    char *_keybuf;
    char *_valuebuf;
    bool _repeat;

    ThreadRunner();
    ~ThreadRunner();

    void free_all();
    static int cross_check(std::vector<ThreadRunner> &runners);

    int close_all();
    int create_all(WT_CONNECTION *conn);
    void get_static_counts(Stats &);
    int open_all();
    int run();

    void op_clear_table(Operation *op);
    void op_create_all(Operation *, size_t &keysize, size_t &valuesize);
    uint64_t op_get_key_recno(Operation *, uint64_t range, tint_t tint);
    void op_get_static_counts(Operation *, Stats &, int);
    void op_kv_gen(Operation *op, const tint_t tint);
    int op_run(Operation *);
    int op_run_setup(Operation *);
    void op_set_table(Operation *op, const std::string &uri, const tint_t tint);
    float random_signed();
    uint32_t random_value();

#ifdef _DEBUG
    std::stringstream _debug_messages;
    std::string get_debug();
#define	DEBUG_CAPTURE(runner, expr)	runner._debug_messages << expr
#else
#define	DEBUG_CAPTURE(runner, expr)
#endif
};

struct Monitor {
    int _errno;
    WorkgenException _exception;
    WorkloadRunner &_wrunner;
    volatile bool _stop;
    pthread_t _handle;
    std::ostream *_out;
    std::ostream *_json;

    Monitor(WorkloadRunner &wrunner);
    ~Monitor() = default;
    int run();

private:
    void _format_out_header();
    void _format_out_entry(const Stats &interval, double interval_secs, const timespec &timespec,
        bool checkpointing, const tm &tm);
    void _format_json_prefix(const std::string &version);
    void _format_json_entry(const tm &tm, const timespec &timespec, bool first_iteration,
    const Stats &interval, bool checkpointing, double interval_secs);
    void _format_json_suffix();
    void _check_latency_threshold(const Stats &interval, uint64_t latency_max);
};

struct TableRuntime {
    uint64_t _max_recno;              // highest recno allocated
    bool _disjoint;                   // does key space have holes?

    // Only used for the dynamic table set.
    bool _is_base;                    // true if this is the base table, false if the mirror
    std::string _mirror;              // table uri of mirror, if mirrored
    uint32_t _in_use;                 // How many operations are using this table
    bool _pending_delete;             // Delete this table once not in use

    TableRuntime() : _max_recno(0), _disjoint(0), _is_base(true), _mirror(std::string()),
        _in_use(0), _pending_delete(false) {}
    TableRuntime(const bool is_base, const std::string &mirror) : _max_recno(0), _disjoint(0),
        _is_base(is_base), _mirror(mirror), _in_use(0), _pending_delete(false) {}
    bool is_base_table() { return _is_base == true; }
    bool has_mirror() { return !_mirror.empty(); }
};

struct ContextInternal {
    // Dedicated to tables that are alive until the workload ends.
    std::map<std::string, tint_t> _tint;           // maps uri -> tint_t
    std::map<tint_t, std::string> _table_names;    // reverse mapping
    std::vector<TableRuntime> _table_runtime;      // # entries per tint_t
    tint_t _tint_last;                             // last tint allocated

    // Dedicated to tables that can be created or removed during the workload.
    std::map<std::string, tint_t> _dyn_tint;
    std::map<tint_t, std::string> _dyn_table_names;
    std::map<tint_t, TableRuntime> _dyn_table_runtime;
    tint_t _dyn_tint_last;
    // This mutex should be used to protect the access to the dynamic tables set.
    std::shared_mutex* _dyn_mutex;
    // unique id per context, to work with multiple contexts, starts at 1.
    uint32_t _context_count;

    ContextInternal();
    ~ContextInternal();
    int create_all(WT_CONNECTION *conn);
};

struct OperationInternal {
#define	WORKGEN_OP_REOPEN		0x0001 // reopen cursor for each op
    uint32_t _flags;

    OperationInternal() : _flags(0) {}
    OperationInternal(const OperationInternal &other) : _flags(other._flags) {}
    virtual ~OperationInternal() {}
    virtual void parse_config(const std::string &config) { (void)config; }
    virtual int run(ThreadRunner *runner, WT_SESSION *session) {
	(void)runner; (void)session; return (0); }
    virtual uint64_t sync_time_us() const { return (0); }
};

struct CheckpointOperationInternal : OperationInternal {
    std::string ckpt_config;
    CheckpointOperationInternal() : OperationInternal(), ckpt_config() {}
    CheckpointOperationInternal(const CheckpointOperationInternal &other) :
	OperationInternal(other), ckpt_config(other.ckpt_config)  {}
    virtual void parse_config(const std::string &config);
    virtual int run(ThreadRunner *runner, WT_SESSION *session);
};

struct LogFlushOperationInternal : OperationInternal {
    LogFlushOperationInternal() : OperationInternal() {}
    LogFlushOperationInternal(const LogFlushOperationInternal &other) :
	OperationInternal(other) {}
    virtual int run(ThreadRunner *runner, WT_SESSION *session);
};

struct TableOperationInternal : OperationInternal {
    uint_t _keysize;    // derived from Key._size and Table.options.key_size
    uint_t _valuesize;
    uint_t _keymax;
    uint_t _valuemax;

    TableOperationInternal() : OperationInternal(), _keysize(0), _valuesize(0),
			       _keymax(0), _valuemax(0) {}
    TableOperationInternal(const TableOperationInternal &other) :
	OperationInternal(other),
	_keysize(other._keysize), _valuesize(other._valuesize),
	_keymax(other._keymax), _valuemax(other._valuemax) {}
    virtual void parse_config(const std::string &config);
};

struct SleepOperationInternal : OperationInternal {
    float _sleepvalue;

    SleepOperationInternal() : OperationInternal(), _sleepvalue(0) {}
    SleepOperationInternal(const SleepOperationInternal &other) :
	OperationInternal(other),_sleepvalue(other._sleepvalue) {}
    virtual void parse_config(const std::string &config);
    virtual int run(ThreadRunner *runner, WT_SESSION *session);
    virtual uint64_t sync_time_us() const;
};

struct TableInternal {
    tint_t _tint;
    uint32_t _context_count;

    TableInternal();
    TableInternal(const TableInternal &other);
    ~TableInternal() = default;
};

// An instance of this class only exists for the duration of one call to a
// Workload::run() method.
struct WorkloadRunner {
    Workload *_workload;
    workgen_random_state *_rand_state;
    std::vector<ThreadRunner> _trunners;
    std::ostream *_report_out;
    std::string _wt_home;
    timespec _start;
    bool stopping;

    WorkloadRunner(Workload *);
    ~WorkloadRunner();
    int run(WT_CONNECTION *conn);
    int increment_timestamp(WT_CONNECTION *conn);
    int start_table_idle_cycle(WT_CONNECTION *conn);
    int start_tables_create(WT_CONNECTION *conn);
    int start_tables_drop(WT_CONNECTION *conn);
    int check_timing(const std::string& name, uint64_t last_interval);

private:
    int close_all();
    int create_all(WT_CONNECTION *conn, Context *context);
    int create_table(WT_SESSION *session, const std::string &config, const std::string &uri,
      const std::string &mirror_uri, const bool is_base);
    void final_report(timespec &);
    void schedule_table_for_drop(const std::map<std::string, tint_t>::iterator &itr,
      std::vector<std::string> &pending_delete);
    void get_stats(Stats *stats);
    int open_all();
    void open_report_file(std::ofstream &, const std::string&, const std::string&);
    void report(time_t, time_t, Stats *stats);
    int run_all(WT_CONNECTION *conn);
    int select_table_for_drop(std::vector<std::string> &pending_delete);

    WorkloadRunner(const WorkloadRunner &);                 // disallowed
    WorkloadRunner& operator=(const WorkloadRunner &other); // disallowed
};

}

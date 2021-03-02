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
#include <ostream>
#include <string>
#include <vector>
#include <map>
#include <set>
extern "C" {
#include <unistd.h>
#include "workgen_func.h"
#include <math.h>
}
#include "workgen_time.h"

namespace workgen {

// A 'tint' or ('table integer') is a unique small value integer
// assigned to each table URI in use.  Currently, we assign it once,
// and its value persists through the lifetime of the Context.
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

// A exception generated by the workgen classes.  Methods generally return an
// int errno, so this is useful primarily for notifying the caller about
// failures in constructors.
struct WorkgenException {
    std::string _str;
    WorkgenException() : _str() {}
    WorkgenException(int err, const char *msg = NULL) : _str() {
	if (err != 0)
	    _str += wiredtiger_strerror(err);
	if (msg != NULL) {
	    if (!_str.empty())
		_str += ": ";
	    _str += msg;
	}
    }
    WorkgenException(const WorkgenException &other) : _str(other._str) {}
    ~WorkgenException() {}
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
    ~Throttle();

    // Called with the number of operations since the last throttle.
    // Sleeps for any needed amount and returns the number operations the
    // caller should perform before the next call to throttle.
    int throttle(uint64_t op_count, uint64_t *op_limit);
};

// There is one of these per Thread object.  It exists for the duration of a
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

    void op_create_all(Operation *, size_t &keysize, size_t &valuesize);
    uint64_t op_get_key_recno(Operation *, uint64_t range, tint_t tint);
    void op_get_static_counts(Operation *, Stats &, int);
    int op_run(Operation *);
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
    ~Monitor();
    int run();
};

struct TableRuntime {
    uint64_t _max_recno;                           // highest recno allocated
    bool _disjoint;                                // does key space have holes?

    TableRuntime() : _max_recno(0), _disjoint(0) {}
};

struct ContextInternal {
    std::map<std::string, tint_t> _tint;           // maps uri -> tint_t
    std::map<tint_t, std::string> _table_names;    // reverse mapping
    TableRuntime *_table_runtime;                  // # entries per tint_t
    uint32_t _runtime_alloced;                     // length of _table_runtime
    tint_t _tint_last;                             // last tint allocated
    // unique id per context, to work with multiple contexts, starts at 1.
    uint32_t _context_count;

    ContextInternal();
    ~ContextInternal();
    int create_all();
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
    CheckpointOperationInternal() : OperationInternal() {}
    CheckpointOperationInternal(const CheckpointOperationInternal &other) :
	OperationInternal(other) {}
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
			       _keymax(0),_valuemax(0) {}
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
    ~TableInternal();
};

// An instance of this class only exists for the duration of one call to a
// Workload::run() method.
struct WorkloadRunner {
    Workload *_workload;
    std::vector<ThreadRunner> _trunners;
    std::ostream *_report_out;
    std::string _wt_home;
    timespec _start;
    bool stopping;

    WorkloadRunner(Workload *);
    ~WorkloadRunner();
    int run(WT_CONNECTION *conn);
    int increment_timestamp(WT_CONNECTION *conn);

private:
    int close_all();
    int create_all(WT_CONNECTION *conn, Context *context);
    void final_report(timespec &);
    void get_stats(Stats *stats);
    int open_all();
    void open_report_file(std::ofstream &, const char *, const char *);
    void report(time_t, time_t, Stats *stats);
    int run_all(WT_CONNECTION *conn);

    WorkloadRunner(const WorkloadRunner &);                 // disallowed
    WorkloadRunner& operator=(const WorkloadRunner &other); // disallowed
};

}

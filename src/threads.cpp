// threads.cpp - client queue, worker, logger, cleanup and delivery threads, metrics, thread-safe logging.
// Owner: Shoyam20
#include <winsock2.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <mutex>
#include <semaphore>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include "common.h"
using namespace std;

// Monotonic clock: not affected by system time changes (used to measure latency).
long long now_us() {
    return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- client queue: a circular buffer; each entry holds the socket and its accept time ----
static ClientJob q[QUEUE_SIZE];
static int head = 0;    // Next client is taken from here.
static int tail = 0;    // Next client is put here.
static int queue_count = 0;     // Real clients only (not stop signals); shown in /metrics.

// Mutex: only one thread may change the array, head, tail and queue_count at a time.
static mutex qlock;

// items = clients waiting (starts at 0): a worker sleeps when it is 0.
// slots = free places (starts at QUEUE_SIZE): when it is 0 the accept loop is refused.
static counting_semaphore<> items(0);
static counting_semaphore<> slots(QUEUE_SIZE);

// ---- worker threads ----
static thread workers[WORKERS];

// Thread number (0 main, -1 cleanup, -2 logger, -3 delivery); thread_local = one copy per thread.
static thread_local int my_id = 0;

// Without this lock, lines from different threads would mix.
static mutex log_lock;

int thread_id() {
    return my_id;
}

void log_tag(const string& tag, const string& msg) {
    time_t now = time(nullptr);
    tm t;
    localtime_s(&t, &now);
    char tm_buf[16];
    strftime(tm_buf, sizeof(tm_buf), "%H:%M:%S", &t);

    log_lock.lock();
    cout << "[" << tm_buf << "][" << tag << "] " << msg << endl;
    log_lock.unlock();
}

// Name for log lines: THREAD n, CLEANUP, LOGGER, DELIVERY, or SERVER (main).
string thread_name() {
    if (my_id > 0) return "THREAD " + to_string(my_id);
    if (my_id == -1) return "CLEANUP";
    if (my_id == -2) return "LOGGER";
    if (my_id == -3) return "DELIVERY";
    return "SERVER";
}

void log_msg(const string& msg) {
    log_tag(thread_name(), msg);
}

// Very frequent lines (per request, "conn taken") print only when VERBOSE_LOG = 1.
// The same information is already in /metrics and the database; the console would only hide real errors.
void log_verbose(const string& tag, const string& msg) {
    if (VERBOSE_LOG) log_tag(tag, msg);
}

void log_msg_verbose(const string& msg) {
    if (VERBOSE_LOG) log_msg(msg);
}

// ---- metrics: all behind one mutex (many threads add, /metrics reads) ----
static mutex metrics_lock;
static time_t started_at = time(nullptr);
static long long m_total = 0;
static long long m_class[6] = {0, 0, 0, 0, 0, 0};   // m_class[2] = 2xx ... m_class[5] = 5xx.
static long long m_rejected_503 = 0;
static long long m_timeouts_408 = 0;
static long long m_commit = 0, m_rollback = 0;
static long long m_dropped = 0;
static int m_busy = 0;

// Latency of the last LATENCY_WINDOW requests: a ring buffer, old values are overwritten.
static long long lat_ring[LATENCY_WINDOW];
static int lat_pos = 0;
static int lat_count = 0;

// One bucket per second for the last RPS_SECONDS seconds (bucket_sec = which second, bucket_cnt = count).
static long long bucket_sec[RPS_SECONDS];
static long long bucket_cnt[RPS_SECONDS];

// The caller must already hold metrics_lock.
static void count_response(int status) {
    m_total++;
    int cls = status / 100;
    if (cls >= 2 && cls <= 5) m_class[cls]++;

    long long sec = (long long)time(nullptr);
    int i = (int)(sec % RPS_SECONDS);
    if (bucket_sec[i] != sec) {         // The bucket held an old second: reset it for the new second.
        bucket_sec[i] = sec;
        bucket_cnt[i] = 0;
    }
    bucket_cnt[i]++;
}

static void metric_request(int status, long long latency_us) {
    metrics_lock.lock();
    count_response(status);
    if (status == 408) m_timeouts_408++;
    lat_ring[lat_pos] = latency_us;
    lat_pos = (lat_pos + 1) % LATENCY_WINDOW;
    if (lat_count < LATENCY_WINDOW) lat_count++;
    metrics_lock.unlock();
}

// Queue full: the accept thread sent 503. Not added to the latency ring (no real work was done).
long long metric_rejected() {
    metrics_lock.lock();
    count_response(503);
    m_rejected_503++;
    long long n = m_rejected_503;
    metrics_lock.unlock();
    return n;
}

void metric_checkout(bool commit) {
    metrics_lock.lock();
    if (commit) m_commit++; else m_rollback++;
    metrics_lock.unlock();
}

static void metric_busy(int delta) {
    metrics_lock.lock();
    m_busy += delta;
    metrics_lock.unlock();
}

static void metric_dropped(long long n) {
    metrics_lock.lock();
    m_dropped += n;
    metrics_lock.unlock();
}

Metrics metrics_snapshot() {
    Metrics m;
    vector<long long> lat;

    metrics_lock.lock();
    time_t now = time(nullptr);
    m.uptime_s = (long long)(now - started_at);
    m.total = m_total;
    m.c2xx = m_class[2]; m.c3xx = m_class[3]; m.c4xx = m_class[4]; m.c5xx = m_class[5];
    m.rejected_503 = m_rejected_503;
    m.timeouts_408 = m_timeouts_408;
    m.busy_workers = m_busy;
    m.checkout_commit = m_commit;
    m.checkout_rollback = m_rollback;
    m.dropped_logs = m_dropped;
    lat.assign(lat_ring, lat_ring + lat_count);     // Copy, so the sorting happens outside the lock.

    long long recent = 0;                           // Last RPS_SECONDS seconds, including the current partial second.
    for (int i = 0; i < RPS_SECONDS; i++)
        if (bucket_sec[i] > (long long)now - RPS_SECONDS) recent += bucket_cnt[i];
    long long window = m.uptime_s < RPS_SECONDS ? (m.uptime_s < 1 ? 1 : m.uptime_s) : RPS_SECONDS;
    m.rps = (double)recent / (double)window;
    metrics_lock.unlock();

    // Average and p95 (the value at the 95% position after sorting).
    m.latency_samples = (int)lat.size();
    if (!lat.empty()) {
        sort(lat.begin(), lat.end());
        long long sum = 0;
        for (long long v : lat) sum += v;
        m.avg_ms = (double)sum / (double)lat.size() / 1000.0;
        size_t idx = (size_t)ceil(0.95 * (double)lat.size());
        m.p95_ms = (double)lat[(idx < 1 ? 1 : idx) - 1] / 1000.0;
    }

    qlock.lock();
    m.queue_length = queue_count;
    qlock.unlock();
    m.free_db_conns = db_free_connections();
    return m;
}


// A place is already reserved; store the client (used by push_client and stop_workers).
static void put_client(const ClientJob& job) {
    qlock.lock();           // Hold the lock only for this short step, never while waiting.
    q[tail] = job;
    tail = (tail + 1) % QUEUE_SIZE;
    if (job.sock != INVALID_SOCKET) queue_count++;
    qlock.unlock();
    items.release();        // Wake a worker: a client is waiting.
}

bool push_client(const ClientJob& job) {
    if (!slots.try_acquire()) return false;   // No room: do not wait, tell the caller.
    put_client(job);
    return true;
}

ClientJob pop_client() {
    items.acquire();        // An empty queue puts the worker to sleep here.
    qlock.lock();
    ClientJob job = q[head];
    q[head] = ClientJob();
    head = (head + 1) % QUEUE_SIZE;
    if (job.sock != INVALID_SOCKET) queue_count--;
    qlock.unlock();
    slots.release();        // Tell the accept loop that a place is free.
    return job;
}

// ---- request_logs: workers add to memory, one logger thread writes to the database ----
// Workers never wait for the database; the logger does one multi-row INSERT every LOG_FLUSH_SECONDS.
static vector<LogEntry> rlog_pending;       // Bounded: never more than LOG_QUEUE_SIZE.
static mutex rlog_lock;                     // Protects rlog_pending and rlog_stop.
static condition_variable rlog_cv;
static bool rlog_stop = false;
static thread logger_thread;

// Plain text for the table: printable ASCII only (other bytes become "?"), limited length; a bad byte can make the INSERT fail.
static string clean_for_log(const string& s, size_t max_len) {
    string out;
    for (char c : s) {
        if (out.size() >= max_len) break;
        out += (c >= 32 && c <= 126) ? c : '?';
    }
    return out;
}

void push_request_log(const LogEntry& e) {
    rlog_lock.lock();
    if (rlog_pending.size() >= (size_t)LOG_QUEUE_SIZE) {    // Full (slow database): drop the entry rather than slow the request.
        rlog_lock.unlock();
        metric_dropped(1);
        return;
    }
    rlog_pending.push_back(e);
    rlog_lock.unlock();
}

// Writes entries in chunks of LOG_BATCH_MAX. A failed chunk is dropped (and counted); requests are not affected.
static void flush_logs(const vector<LogEntry>& batch) {
    static long long last_error_us = 0;             // Only the logger thread uses this.
    for (size_t from = 0; from < batch.size(); from += LOG_BATCH_MAX) {
        size_t to = min(batch.size(), from + (size_t)LOG_BATCH_MAX);
        vector<LogEntry> chunk(batch.begin() + from, batch.begin() + to);
        if (db_insert_request_logs(chunk) < 0) {
            metric_dropped((long long)chunk.size());
            // Do not repeat the same error every second (e.g. a missing table): at most once per 30 s.
            if (last_error_us == 0 || now_us() - last_error_us > 30LL * 1000000LL) {
                last_error_us = now_us();
                log_tag("LOGGER", "could not save " + to_string(chunk.size()) +
                                  " request log rows (dropped). Is the request_logs table created?");
            }
        }
    }
}

static void logger_loop() {
    my_id = -2;
    db_thread_start();
    log_tag("LOGGER", "started (batch insert every " + to_string(LOG_FLUSH_SECONDS) + " s, queue " +
                      to_string(LOG_QUEUE_SIZE) + ")");

    vector<LogEntry> batch;                 // Reused buffer: after the swap, rlog_pending also gets a big buffer.
    batch.reserve(LOG_QUEUE_SIZE);

    unique_lock<mutex> lk(rlog_lock);
    while (true) {
        // Not sleep(): a condition_variable wait lets stop_logger wake us at once.
        auto deadline = chrono::steady_clock::now() + chrono::seconds(LOG_FLUSH_SECONDS);
        while (!rlog_stop && rlog_cv.wait_until(lk, deadline) == cv_status::no_timeout) {
        }

        batch.clear();
        batch.swap(rlog_pending);           // The whole queue in O(1); the mutex is released quickly.
        bool stopping = rlog_stop;
        lk.unlock();

        flush_logs(batch);                  // Database work is outside the mutex, so workers can keep adding.

        lk.lock();
        // If we are stopping and nothing is left (the workers have already stopped), exit;
        // otherwise run once more to flush what arrived after the swap.
        if (stopping && rlog_pending.empty()) break;
    }
    lk.unlock();

    db_thread_end();
    log_tag("LOGGER", "stopped (all pending logs flushed)");
}

void start_logger() {
    rlog_stop = false;
    rlog_pending.reserve(LOG_QUEUE_SIZE);
    logger_thread = thread(logger_loop);
}

void stop_logger() {
    rlog_lock.lock();
    rlog_stop = true;
    rlog_lock.unlock();
    rlog_cv.notify_all();                   // Wake the sleeping logger so it flushes the rest now.
    if (logger_thread.joinable()) logger_thread.join();
}


// Count the request in metrics and queue a log entry (both in memory, no database wait).
static void finish_request(const RequestInfo& info, long long accept_us, long long picked_us, long long done_us) {
    metric_request(info.status, done_us - accept_us);

    LogEntry e;
    e.method = clean_for_log(info.method, 10);          // Column is VARCHAR(10).
    e.path = clean_for_log(info.path, 255);             // VARCHAR(255), without the query string.
    e.status = info.status;
    e.latency_ms = (double)(done_us - accept_us) / 1000.0;          // accept -> reply sent.
    e.queue_wait_ms = (double)(picked_us - accept_us) / 1000.0;     // accept -> picked up by a worker.
    e.worker_id = my_id;
    e.user_id = info.user_id;
    e.created = (long long)time(nullptr);
    push_request_log(e);
}

static void worker_loop(int id) {
    my_id = id;
    db_thread_start();      // The MySQL library must know about each thread.
    log_msg("worker started");
    while (true) {
        ClientJob job = pop_client();
        if (job.sock == INVALID_SOCKET) break;    // INVALID_SOCKET is the "stop" signal.
        long long picked = now_us();              // queue_wait_ms ends here: accept to now.
        metric_busy(1);
        RequestInfo info = handle_client(job);
        long long done = now_us();
        metric_busy(-1);
        if (info.status > 0) finish_request(info, job.accept_us, picked, done);
    }
    db_thread_end();        // Free this thread's MySQL memory.
    log_msg("worker stopped");
}

void start_workers() {
    for (int i = 0; i < WORKERS; i++)
        workers[i] = thread(worker_loop, i + 1);
}

void stop_workers() {
    // One stop signal per worker; clients already queued are still served first (FIFO).
    // acquire(), not try_acquire: wait for room even if the queue is full.
    ClientJob stop;                         // sock = INVALID_SOCKET.
    for (int i = 0; i < WORKERS; i++) {
        slots.acquire();
        put_client(stop);
    }
    for (int i = 0; i < WORKERS; i++) workers[i].join();
}

// ---- cleanup thread: keeps closing expired sessions ----
static thread cleanup_thread;
static mutex cleanup_lock;                  // Protects cleanup_stop.
static condition_variable cleanup_cv;       // Wakes the thread for "stop" or "time is up".
static bool cleanup_stop = false;

static void cleanup_loop() {
    my_id = -1;
    db_thread_start();
    log_tag("CLEANUP", "started (every " + to_string(CLEANUP_SECONDS) + " seconds)");

    unique_lock<mutex> lk(cleanup_lock);
    while (!cleanup_stop) {
        // Not sleep(60): wait_until can be woken at once by notify (stop).
        // A spurious wakeup (waking for no reason) just waits again until the deadline.
        auto deadline = chrono::steady_clock::now() + chrono::seconds(CLEANUP_SECONDS);
        while (!cleanup_stop && cleanup_cv.wait_until(lk, deadline) == cv_status::no_timeout) {
        }
        if (cleanup_stop) break;

        lk.unlock();                        // Database work without the mutex, or stop_cleanup would block.
        int n = close_expired_sessions();
        if (n < 0) log_tag("CLEANUP", "failed to close expired sessions");
        else if (n > 0) log_tag("CLEANUP", "closed " + to_string(n) + " expired sessions");
        lk.lock();
    }
    lk.unlock();

    db_thread_end();
    log_tag("CLEANUP", "stopped");
}

// ---- delivery thread: moves due orders one step (PLACED -> PACKED -> ... -> DELIVERED) ----
// The due time is stored in the database (orders.next_step_at), so orders continue after a restart.
static thread delivery_thread;
static mutex delivery_lock;                 // Protects delivery_stop.
static condition_variable delivery_cv;      // Wakes the thread for "stop" or "tick is up".
static bool delivery_stop = false;

static void delivery_loop() {
    my_id = -3;
    db_thread_start();
    log_tag("DELIVERY", "started (check every " + to_string(DELIVERY_TICK_SECONDS) + " seconds)");

    unique_lock<mutex> lk(delivery_lock);
    while (!delivery_stop) {
        // Same as the cleanup thread: a condition_variable wait, so shutdown is immediate.
        auto deadline = chrono::steady_clock::now() + chrono::seconds(DELIVERY_TICK_SECONDS);
        while (!delivery_stop && delivery_cv.wait_until(lk, deadline) == cv_status::no_timeout) {
        }
        if (delivery_stop) break;

        lk.unlock();                        // Database work without the mutex, or stop_delivery would block.
        if (deliver_due_orders() < 0) log_tag("DELIVERY", "failed to read due orders");
        lk.lock();
    }
    lk.unlock();

    db_thread_end();
    log_tag("DELIVERY", "stopped");
}

void start_delivery() {
    if (!AUTO_DELIVERY) {
        log_tag("DELIVERY", "off (AUTO_DELIVERY = 0): only the admin changes order status");
        return;
    }
    delivery_stop = false;
    delivery_thread = thread(delivery_loop);
}

void stop_delivery() {
    delivery_lock.lock();
    delivery_stop = true;
    delivery_lock.unlock();
    delivery_cv.notify_all();               // Wakes the sleeping thread at once.
    if (delivery_thread.joinable()) delivery_thread.join();
}

// The delivery thread starts and stops with the cleanup thread, because both use the database (main.cpp is unchanged).
void start_cleanup() {
    cleanup_stop = false;
    cleanup_thread = thread(cleanup_loop);
    start_delivery();
}

void stop_cleanup() {
    stop_delivery();
    cleanup_lock.lock();
    cleanup_stop = true;
    cleanup_lock.unlock();
    cleanup_cv.notify_all();                // Wakes the sleeping thread at once.
    if (cleanup_thread.joinable()) cleanup_thread.join();
}

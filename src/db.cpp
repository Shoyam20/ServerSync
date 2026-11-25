#include "platform.h"
#include <mysql.h>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <semaphore>
#include <thread>
#include "common.h"
using namespace std;

// ---- login details (read from db.conf, never written in the code) ----
static string db_host, db_user, db_pass, db_name;
static string db_ssl_ca;             // CA certificate file (e.g. Aiven's ca.pem). If set: SSL required + server certificate checked
static bool db_ssl_required = false;  // no CA, but SSL still required (encrypted, certificate not checked)
static unsigned int db_port = 3306;

// ---- connection pool ----
static MYSQL* pool[DB_POOL];        // Connection number = index + 1.
static int free_ids[DB_POOL];       // Free list: which connection numbers are free now.
static int free_count = 0;

// Mutex: only one thread may change the free list at a time.
static mutex pool_lock;

// free_sem = number of free connections; get_conn() waits at 0 (init_db releases DB_POOL).
static counting_semaphore<> free_sem(0);

static string who() {
    return thread_name();
}

static string env_or(const char* name, const string& fallback) {
    const char* v = getenv(name);
    return (v != nullptr && *v != '\0') ? string(v) : fallback;
}

// Config from environment variables (hosts like Render): if DB_HOST is set, db.conf is not read.
// DB_HOST, DB_PORT, DB_USER, DB_PASSWORD, DB_NAME; DB_CA_CERT = the whole CA certificate PEM text (optional).
// It is a cloud database, so SSL is always required: VERIFY_CA when a CA is given, else REQUIRED (encrypted, no cert check).
static bool load_env_conf() {
    db_host = env_or("DB_HOST", "");
    db_user = env_or("DB_USER", "");
    db_pass = env_or("DB_PASSWORD", "");
    db_name = env_or("DB_NAME", "ecommerce_db");
    db_port = (unsigned int)atoi(env_or("DB_PORT", "3306").c_str());
    db_ssl_required = true;

    string pem = env_or("DB_CA_CERT", "");
    if (!pem.empty()) {
        // the MySQL library wants a file path, not the text: write the PEM to a file
        const string ca_path = "db_ca_from_env.pem";
        ofstream out(ca_path, ios::binary | ios::trunc);
        out << pem;
        if (pem.back() != '\n') out << '\n';
        if (out) db_ssl_ca = ca_path;
        else log_tag("DB", "cannot write " + ca_path + ": using SSL without certificate check");
    }

    if (db_user.empty() || db_pass.empty()) {
        log_tag("DB", "DB_HOST is set, so DB_USER and DB_PASSWORD env vars are needed too.");
        return false;
    }
    log_tag("DB", string("config: env vars (SSL ") + (db_ssl_ca.empty() ? "required, CA not checked)" : "with CA check)"));
    return true;
}

// Reads db.conf: lines are key=value; a line starting with # is a comment.
// Where: the DB_CONF environment variable if set, else ./db.conf, else /etc/secrets/db.conf (Render Secret Files).
static bool load_conf() {
    if (getenv("DB_HOST") != nullptr) return load_env_conf();
    const char* env_path = getenv("DB_CONF");
    string path = (env_path != nullptr && *env_path != '\0') ? env_path : "db.conf";
    ifstream f(path);
    if (!f && env_path == nullptr) {
        path = "/etc/secrets/db.conf";
        f.open(path);
    }
    if (!f) {
        log_tag("DB", "db.conf not found. Copy db.conf.example to db.conf and fill in your MySQL details.");
        return false;
    }
    log_tag("DB", "config: " + path);

    string line;
    bool has_host = false, has_user = false, has_pass = false, has_name = false;
    while (getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = trim(line.substr(0, eq));
        string val = trim(line.substr(eq + 1));

        if (key == "host") { db_host = val; has_host = true; }
        else if (key == "user") { db_user = val; has_user = true; }
        else if (key == "password") { db_pass = val; has_pass = true; }
        else if (key == "database") { db_name = val; has_name = true; }
        else if (key == "port") db_port = (unsigned int)atoi(val.c_str());
        else if (key == "ssl_ca") db_ssl_ca = val;
    }

    if (!has_host || !has_user || !has_pass || !has_name) {
        log_tag("DB", "db.conf must have host, user, password and database lines.");
        return false;
    }
    return true;
}

// Opens a new MySQL connection (nullptr on failure, the reason is logged).
// Used by init_db and by get_conn when a connection died, so all connection options live in one place.
static MYSQL* open_conn() {
    MYSQL* m = mysql_init(nullptr);
    if (m == nullptr) {
        log_tag("DB", "mysql_init failed (out of memory)");
        return nullptr;
    }
    // CLIENT_FOUND_ROWS: UPDATE reports the rows MATCHED by WHERE, not only the rows whose value changed.
    // Without it, writing the same value gives 0, and 0 could not tell "not found" from "same value".
    unsigned int timeout = 5;                            // Give up after 5 seconds if the server is down.
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    // Cloud MySQL (Aiven) needs SSL. With ssl_ca: VERIFY_CA, the connection is encrypted and the server certificate must be
    // signed by that CA (otherwise someone in the middle could read the password). No ssl_ca: the old behaviour.
    if (!db_ssl_ca.empty()) {
        mysql_options(m, MYSQL_OPT_SSL_CA, db_ssl_ca.c_str());
        unsigned int ssl_mode = SSL_MODE_VERIFY_CA;
        mysql_options(m, MYSQL_OPT_SSL_MODE, &ssl_mode);
    } else if (db_ssl_required) {
        unsigned int ssl_mode = SSL_MODE_REQUIRED;
        mysql_options(m, MYSQL_OPT_SSL_MODE, &ssl_mode);
    }

    if (mysql_real_connect(m, db_host.c_str(), db_user.c_str(), db_pass.c_str(),
                           db_name.c_str(), db_port, nullptr, CLIENT_FOUND_ROWS) == nullptr) {
        log_tag("DB", "cannot connect to MySQL at " + db_host + " as '" + db_user +
                      "' (database " + db_name + "): " + mysql_error(m));
        mysql_close(m);
        return nullptr;
    }
    return m;
}

bool init_db() {
    if (!load_conf()) return false;

    // Initialise the library once, before any thread connects.
    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        log_tag("DB", "mysql_library_init failed");
        return false;
    }

    // A cloud DB (Aiven free plan) that is asleep or still starting makes the first connect fail. Instead of exiting at once,
    // wait and try again a few times, a little longer each time (5 s, 10 s, 15 s ...). Close a half-built pool before retrying.
    bool ready = false;
    for (int attempt = 1; attempt <= DB_CONNECT_TRIES && !ready; attempt++) {
        ready = true;
        for (int i = 0; i < DB_POOL; i++) {
            MYSQL* m = open_conn();
            if (m == nullptr) {
                ready = false;
                break;
            }
            pool[i] = m;
            free_ids[free_count++] = i + 1;
        }
        if (ready) break;
        for (int i = 0; i < DB_POOL; i++) {
            if (pool[i] != nullptr) {
                mysql_close(pool[i]);
                pool[i] = nullptr;
            }
        }
        free_count = 0;
        if (attempt < DB_CONNECT_TRIES) {
            int wait = DB_CONNECT_WAIT_SECONDS * attempt;
            log_tag("DB", "connect attempt " + to_string(attempt) + " of " + to_string(DB_CONNECT_TRIES) + " failed, trying again in " +
                          to_string(wait) + " s");
            this_thread::sleep_for(chrono::seconds(wait));
        }
    }
    if (!ready) {
        log_tag("DB", "gave up after " + to_string(DB_CONNECT_TRIES) + " attempts");
        mysql_library_end();
        return false;
    }

    free_sem.release(DB_POOL);
    log_tag("DB", "pool ready: " + to_string(DB_POOL) + " connections to " + db_name + " on " + db_host);
    return true;
}

void close_db() {
    for (int i = 0; i < DB_POOL; i++) {
        if (pool[i] != nullptr) {
            mysql_close(pool[i]);
            pool[i] = nullptr;
        }
    }
    free_count = 0;
    mysql_library_end();
}

MYSQL* get_conn(int& id) {
    // One connection is used by one thread at a time: wait here if none is free.
    free_sem.acquire();
    pool_lock.lock();
    if (free_count <= 0) {          // Cannot happen (the semaphore guards it); return instead of crashing if it does.
        pool_lock.unlock();
        log_tag("DB", "BUG: no free connection in list");
        id = 0;
        return nullptr;
    }
    id = free_ids[--free_count];
    MYSQL* m = pool[id - 1];
    pool_lock.unlock();

    // MySQL closes idle connections (wait_timeout, default 8 h) and a restart kills all of them.
    // Failed ping = dead connection: open a new one (mysql_ping does not reconnect itself). No lock needed: this slot is ours.
    if (mysql_ping(m) != 0) {
        log_tag("DB", "conn " + to_string(id) + " lost (" + mysql_error(m) + "), reconnecting");
        MYSQL* fresh = open_conn();
        if (fresh != nullptr) {
            mysql_close(m);
            m = fresh;
            pool[id - 1] = m;
        }
        // If a new one cannot be opened (MySQL is down), return the old one: this request gets a clean error (500), no crash.
        // The next get_conn() tries again.
    }
    log_verbose("DB", "conn " + to_string(id) + " taken by " + who());
    return m;
}

void put_conn(int id) {
    // Guards the free list against a bad id or a double put.
    if (id < 1 || id > DB_POOL) return;

    // Log first: once the id is in the free list another thread may take it, and its "taken" line would come first.
    log_verbose("DB", "conn " + to_string(id) + " returned");
    pool_lock.lock();
    if (free_count >= DB_POOL) {
        pool_lock.unlock();
        log_tag("DB", "BUG: put_conn called too many times");
        return;
    }
    free_ids[free_count++] = id;
    pool_lock.unlock();
    free_sem.release();             // Wake a waiting thread.
}

// Number of free connections (for /metrics).
int db_free_connections() {
    pool_lock.lock();
    int n = free_count;
    pool_lock.unlock();
    return n;
}

// ---- prepared statements ----
// SQL (with ?) goes to the server first and the data separately: user data can never become SQL code (no SQL injection).

const int MAX_ARGS = 4000;          // Max ? per query (the multi-row request_logs INSERT uses 200 rows x 8 = 1600).
const int MAX_COLS = 16;            // Max columns per query.
const unsigned long MAX_CELL = 65535;   // Max size of one value.

// Reads all rows (the statement has already been executed).
// err = MySQL error number on failure (lock wait timeout 1205 and deadlock 1213 come out here).
static bool fetch_all(MYSQL_STMT* stmt, MYSQL_RES* meta, vector<vector<string>>& rows, int& err) {
    int cols = (int)mysql_num_fields(meta);
    if (cols > MAX_COLS) {
        log_tag("DB", "too many columns in result");
        return false;
    }
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);

    // One buffer per column; every value is requested as text.
    vector<vector<char>> bufs(cols);
    MYSQL_BIND bind[MAX_COLS];
    unsigned long lens[MAX_COLS];
    bool is_null[MAX_COLS];
    bool is_err[MAX_COLS];
    memset(bind, 0, sizeof(bind));
    for (int i = 0; i < cols; i++) {
        unsigned long size = fields[i].length < MAX_CELL ? fields[i].length : MAX_CELL;
        bufs[i].resize(size + 1);
        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].buffer = bufs[i].data();
        bind[i].buffer_length = size;
        bind[i].length = &lens[i];
        bind[i].is_null = &is_null[i];
        bind[i].error = &is_err[i];
    }

    if (mysql_stmt_bind_result(stmt, bind) != 0 || mysql_stmt_store_result(stmt) != 0) {
        err = (int)mysql_stmt_errno(stmt);
        log_tag("DB", "result failed: " + string(mysql_stmt_error(stmt)));
        return false;
    }

    while (true) {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;             // No more rows.
        if (rc != 0) {                              // Error, or a value did not fit its buffer.
            err = (int)mysql_stmt_errno(stmt);
            log_tag("DB", "fetch failed: " + string(mysql_stmt_error(stmt)));
            return false;
        }
        vector<string> row;
        for (int i = 0; i < cols; i++) {
            // lens[i] cannot exceed the buffer (truncation already failed above); checked anyway.
            if (is_null[i] || lens[i] > bufs[i].size()) row.push_back("");
            else row.push_back(string(bufs[i].data(), lens[i]));
        }
        rows.push_back(row);
    }
    return true;
}

// If there is a result set, read it; always free the metadata.
static bool read_rows(MYSQL_STMT* stmt, vector<vector<string>>& rows, int& err) {
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (meta == nullptr) return true;               // INSERT/UPDATE: no rows.
    bool ok = fetch_all(stmt, meta, rows, err);
    mysql_free_result(meta);
    return ok;
}

// DEBUG_SQL = 1 logs the SQL and each bound value (and its length) before every query.
// Queries with "password" or "user_sessions" in the SQL show '***' instead of values (passwords, session ids).
static void debug_log_sql(const string& sql, const vector<string>& args, const unsigned long* bound_len) {
    if (!DEBUG_SQL) return;
    if (sql.compare(0, 25, "INSERT INTO request_logs ") == 0) return;   // Skip the request_logs INSERT: far too noisy.
    string lower = sql;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    bool secret = lower.find("password") != string::npos || lower.find("user_sessions") != string::npos;

    log_tag("DB", "SQL: " + sql);
    for (size_t i = 0; i < args.size(); i++) {
        string shown = secret ? "***" : args[i].substr(0, 200);     // Long values are cut at 200 characters.
        log_tag("DB", "param " + to_string(i + 1) + " = '" + shown + "' (len " + to_string(args[i].size()) +
                      ", bound length " + to_string(bound_len[i]) + ")");
    }
}

// When a CHECK constraint fails (error 3819), log its real definition from the database.
// That shows at once if code and database disagree (e.g. an ALTER was run on another database).
static void log_check_definition(MYSQL_STMT* stmt) {
    string msg = mysql_stmt_error(stmt);                // Example: "Check constraint 'chk_orders_status' is violated."
    size_t a = msg.find('\'');
    size_t b = (a == string::npos) ? a : msg.find('\'', a + 1);
    if (b == string::npos) return;

    vector<vector<string>> rows;
    if (db_select_on(stmt->mysql, "SELECT DATABASE(), CHECK_CLAUSE FROM information_schema.CHECK_CONSTRAINTS "
                                  "WHERE CONSTRAINT_SCHEMA = DATABASE() AND CONSTRAINT_NAME = ?",
                     {msg.substr(a + 1, b - a - 1)}, rows) && !rows.empty() && rows[0].size() >= 2)
        log_tag("DB", "in database '" + rows[0][0] + "' the rule " + msg.substr(a, b - a + 1) + " is: " + rows[0][1]);
}

// Prepare, bind args, execute. select = true reads rows; false counts affected rows.
// On failure err gets the MySQL error number (e.g. 1062 = duplicate entry).
static bool stmt_steps(MYSQL_STMT* stmt, const string& sql, const vector<string>& args, bool select,
                       vector<vector<string>>& rows, long long& affected, int& err) {
    if (mysql_stmt_prepare(stmt, sql.c_str(), (unsigned long)sql.size()) != 0) {
        err = (int)mysql_stmt_errno(stmt);
        log_tag("DB", "prepare failed: " + string(mysql_stmt_error(stmt)));
        return false;
    }
    if ((int)args.size() > MAX_ARGS || mysql_stmt_param_count(stmt) != args.size()) {
        log_tag("DB", "wrong number of ? for this query");
        return false;
    }

    // A SELECT has a result set, INSERT/UPDATE/DELETE do not: the wrong helper is refused before the query runs.
    bool has_result = mysql_stmt_field_count(stmt) > 0;
    if (has_result != select) {
        log_tag("DB", select ? "db_select used for a statement that returns no rows"
                             : "db_exec used for a statement that returns rows");
        return false;
    }

    // All args are sent as text; MySQL converts to numbers when needed.
    // IMPORTANT: bind and lens must stay alive until mysql_stmt_execute() finishes (it only keeps pointers), so they live outside any `if`.
    vector<MYSQL_BIND> bind(args.size());       // A vector: this many ? should not sit on the stack (~100 bytes each).
    vector<unsigned long> lens(args.size());
    if (!args.empty()) {
        memset(bind.data(), 0, bind.size() * sizeof(MYSQL_BIND));
        for (size_t i = 0; i < args.size(); i++) {
            lens[i] = (unsigned long)args[i].size();
            bind[i].buffer_type = MYSQL_TYPE_STRING;
            bind[i].buffer = (void*)args[i].c_str();
            bind[i].buffer_length = lens[i];
            bind[i].length = &lens[i];
        }
        if (mysql_stmt_bind_param(stmt, bind.data()) != 0) {
            log_tag("DB", "bind failed: " + string(mysql_stmt_error(stmt)));
            return false;
        }
        debug_log_sql(sql, args, lens.data());
    } else {
        debug_log_sql(sql, args, nullptr);
    }

    if (mysql_stmt_execute(stmt) != 0) {
        err = (int)mysql_stmt_errno(stmt);
        // 1062 (e.g. duplicate email) is normal, not an error: the caller handles it.
        if (err != 1062) log_tag("DB", "execute failed: " + string(mysql_stmt_error(stmt)));
        if (err == 3819) log_check_definition(stmt);    // CHECK constraint failed: show the real definition.
        return false;
    }
    if (select) return read_rows(stmt, rows, err);

    // Affected rows = rows matched by WHERE (because of CLIENT_FOUND_ROWS, see open_conn).
    unsigned long long n = mysql_stmt_affected_rows(stmt);
    if (n == (unsigned long long)-1) {
        log_tag("DB", "affected rows unknown");
        return false;
    }
    affected = (long long)n;
    return true;
}

// Build the statement, run the steps, always close the statement.
static bool run_stmt(MYSQL* m, const string& sql, const vector<string>& args, bool select,
                     vector<vector<string>>& rows, long long& affected, int& err) {
    MYSQL_STMT* stmt = mysql_stmt_init(m);
    if (stmt == nullptr) {
        log_tag("DB", "mysql_stmt_init failed");
        return false;
    }
    bool ok = stmt_steps(stmt, sql, args, select, rows, affected, err);
    mysql_stmt_close(stmt);
    return ok;
}

// ---- *_on: query on a connection the caller already holds (the caller does get_conn and put_conn) ----

bool db_select_on(MYSQL* m, const string& sql, const vector<string>& args, vector<vector<string>>& rows,
                  int* err_code) {
    rows.clear();
    int err = 0;
    long long affected = 0;
    bool ok = run_stmt(m, sql, args, true, rows, affected, err);
    if (err_code != nullptr) *err_code = err;
    return ok;
}

int db_exec_on(MYSQL* m, const string& sql, const vector<string>& args, int* err_code) {
    vector<vector<string>> rows;
    int err = 0;
    long long affected = 0;
    bool ok = run_stmt(m, sql, args, false, rows, affected, err);
    if (err_code != nullptr) *err_code = err;
    return ok ? (int)affected : -1;
}

// ---- one query = one connection: get_conn, query, put_conn ----

bool db_select(const string& sql, const vector<string>& args, vector<vector<string>>& rows, int* conn_id) {
    rows.clear();
    int conn = 0;
    MYSQL* m = get_conn(conn);
    if (conn_id != nullptr) *conn_id = conn;
    if (m == nullptr) return false;     // get_conn failed (a bug): nothing to give back.
    bool ok = db_select_on(m, sql, args, rows);
    put_conn(conn);                 // The connection always goes back, even on error.
    return ok;
}

int db_exec(const string& sql, const vector<string>& args, int* err_code) {
    int conn = 0;
    MYSQL* m = get_conn(conn);
    if (m == nullptr) return -1;
    int n = db_exec_on(m, sql, args, err_code);
    put_conn(conn);                 // The connection always goes back, even on error.
    return n;
}

bool db_scalar(const string& sql, string& out, int* conn_id) {
    vector<vector<string>> rows;
    if (!db_select(sql, {}, rows, conn_id)) return false;
    if (rows.empty() || rows[0].empty()) return false;     // No row or column: return false, do not crash.
    out = rows[0][0];
    return true;
}

// ---- request_logs: batch INSERT ----

// Text like "0.512" (3 decimals) for the DECIMAL(10,3) columns.
static string ms_text(double ms) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f", ms < 0 ? 0.0 : ms);
    return buf;
}

// One multi-row INSERT for many rows is much cheaper than one INSERT each (one round trip, one transaction).
// SQL pieces are fixed, values are bound with ?. user_id 0 = NULL; created_at is the request's own time. Returns rows added, -1 on error.
int db_insert_request_logs(const vector<LogEntry>& rows) {
    if (rows.empty()) return 0;

    string sql = "INSERT INTO request_logs (method, path, status, latency_ms, queue_wait_ms, worker_id, user_id, created_at) VALUES ";
    vector<string> args;
    for (size_t i = 0; i < rows.size(); i++) {
        const LogEntry& e = rows[i];
        sql += (i == 0 ? "" : ", ");
        sql += "(?, ?, ?, ?, ?, ?, NULLIF(?, 0), FROM_UNIXTIME(?))";
        args.push_back(e.method);
        args.push_back(e.path);
        args.push_back(to_string(e.status));
        args.push_back(ms_text(e.latency_ms));
        args.push_back(ms_text(e.queue_wait_ms));
        args.push_back(to_string(e.worker_id));
        args.push_back(to_string(e.user_id));
        args.push_back(to_string(e.created));
    }
    return db_exec(sql, args);
}

// ---- cart ----
// Cart functions use a connection the caller holds (get_conn first, put_conn last, in pages.cpp): one request, one connection.

// Finds the user's ACTIVE cart (SELECT only, creates nothing). 0 = none or error.
int find_active_cart(MYSQL* m, int user_id) {
    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT cart_id FROM cart WHERE user_id = ? AND status = 'ACTIVE'",
                      {to_string(user_id)}, rows))
        return 0;
    if (rows.empty() || rows[0].empty()) return 0;
    return atoi(rows[0][0].c_str());
}

// Gets the ACTIVE cart or creates it (0 on error). Race: two requests may both INSERT; UNIQUE(active_user_id) lets one win,
// the loser gets 1062 = fine, the cart exists now: SELECT it again.
int get_active_cart(MYSQL* m, int user_id) {
    int cart_id = find_active_cart(m, user_id);
    if (cart_id > 0) return cart_id;

    int err = 0;
    int n = db_exec_on(m, "INSERT INTO cart (user_id) VALUES (?)", {to_string(user_id)}, &err);
    if (n < 0 && err != 1062) return 0;         // A real error.
    return find_active_cart(m, user_id);        // Our INSERT or someone else's: either way the cart exists now.
}

// Adds a product (or adds to its quantity) in ONE statement: SELECT then UPDATE could lose an update when two requests overlap.
// MySQL locks the row and adds to the CURRENT value; LEAST caps it at MAX_CART_QTY. cart_id always comes from the session.
bool cart_add(MYSQL* m, int cart_id, const string& product_id, const string& qty) {
    string sql = "INSERT INTO cart_items (cart_id, product_id, quantity) VALUES (?, ?, ?) AS new_item "
                 "ON DUPLICATE KEY UPDATE quantity = LEAST(cart_items.quantity + new_item.quantity, " +
                 to_string(MAX_CART_QTY) + ")";
    return db_exec_on(m, sql, {to_string(cart_id), product_id, qty}) >= 0;
}

// Sets the quantity directly (0 = remove): no read-modify-write, so no lost update.
// Returns rows matched (0 = the item was not in the cart), -1 on error.
int cart_set_qty(MYSQL* m, int cart_id, const string& product_id, const string& qty) {
    if (qty == "0") return cart_remove(m, cart_id, product_id);
    return db_exec_on(m, "UPDATE cart_items SET quantity = ? WHERE cart_id = ? AND product_id = ?",
                      {qty, to_string(cart_id), product_id});
}

// Removes an item. Returns rows removed (0 = it was not there), -1 on error.
int cart_remove(MYSQL* m, int cart_id, const string& product_id) {
    return db_exec_on(m, "DELETE FROM cart_items WHERE cart_id = ? AND product_id = ?",
                      {to_string(cart_id), product_id});
}

// Cart items: product_id, name, price, qty, stock, is_active. The price always comes from the products table.
bool cart_items(MYSQL* m, int cart_id, vector<vector<string>>& rows) {
    return db_select_on(m, "SELECT p.product_id, p.name, p.price, ci.quantity, p.stock, p.is_active "
                           "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id "
                           "WHERE ci.cart_id = ? ORDER BY p.name", {to_string(cart_id)}, rows);
}

// Total quantity in the user's ACTIVE cart; 0 if no cart, -1 on error.
int cart_count(MYSQL* m, int user_id) {
    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT COALESCE(SUM(ci.quantity), 0) FROM cart c "
                         "LEFT JOIN cart_items ci ON ci.cart_id = c.cart_id "
                         "WHERE c.user_id = ? AND c.status = 'ACTIVE'", {to_string(user_id)}, rows))
        return -1;
    if (rows.empty() || rows[0].empty()) return 0;
    return atoi(rows[0][0].c_str());
}

// ---- order path: PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED ----
static const char* FLOW[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
const int FLOW_N = 5;
// Delay (seconds) before reaching FLOW[i]; [0] is empty because PLACED is created directly.
static const int FLOW_MIN[] = {0, DELAY_PACKED_MIN, DELAY_SHIPPED_MIN, DELAY_OUT_FOR_DELIVERY_MIN, DELAY_DELIVERED_MIN};
static const int FLOW_MAX[] = {0, DELAY_PACKED_MAX, DELAY_SHIPPED_MAX, DELAY_OUT_FOR_DELIVERY_MAX, DELAY_DELIVERED_MAX};

// Position in the path (PLACED = 0 ... DELIVERED = 4); CANCELLED or unknown = -1.
static int flow_index(const string& status) {
    for (int i = 0; i < FLOW_N; i++)
        if (status == FLOW[i]) return i;
    return -1;
}

// Random delay between min and max before FLOW[i]. thread_local: each thread has its own generator, so no lock.
static int random_delay(int i) {
    static thread_local mt19937 gen(random_device{}());
    return uniform_int_distribution<int>(FLOW_MIN[i], FLOW_MAX[i])(gen);
}

// next_step_at SQL, NULL after the last status. scheduled = true (delivery thread): count from the PREVIOUS due time, not NOW(),
// so shipping continues while the server is down. Otherwise from NOW(). The number is our own int, not user input.
static string next_step_sql(const string& status, bool scheduled = false) {
    int i = flow_index(status);
    if (i < 0 || i >= FLOW_N - 1) return "NULL";
    return string(scheduled ? "DATE_ADD(next_step_at, INTERVAL " : "NOW() + INTERVAL ") + to_string(random_delay(i + 1)) + " SECOND" +
           (scheduled ? ")" : "");
}

string order_next_status(const string& status) {
    int i = flow_index(status);
    if (i < 0 || i >= FLOW_N - 1) return "";
    return FLOW[i + 1];
}

// For "Arriving by": the steps after the next one, using their average delay (min + max) / 2.
int order_eta_seconds(const string& status) {
    int i = flow_index(status);
    if (i < 0 || i >= FLOW_N - 1) return 0;
    int sum = 0;
    for (int k = i + 2; k < FLOW_N; k++) sum += (FLOW_MIN[k] + FLOW_MAX[k]) / 2;
    return sum;
}

// ---- checkout: one DB transaction ----
// A transaction is "all or nothing": any failure = ROLLBACK, success = COMMIT. An order never exists without its stock change and payment.

const int CO_OK = 0;        // All done (COMMIT still to do).
const int CO_PROBLEM = 1;   // User-level problem (empty cart, low stock): ROLLBACK and show the message.
const int CO_RETRY = 2;     // Deadlock or lock wait timeout: ROLLBACK and try again.
const int CO_FAIL = 3;      // Any other error: ROLLBACK, error page.

// Start a transaction: autocommit off (otherwise every query commits by itself) + START TRANSACTION.
static bool db_begin(MYSQL* m) {
    if (mysql_autocommit(m, false) || mysql_query(m, "START TRANSACTION") != 0) {
        log_tag("CHECKOUT", "cannot start transaction: " + string(mysql_error(m)));
        return false;
    }
    return true;
}

// Autocommit back ON: this pooled connection goes to another request, which must not get autocommit off.
// NOTE: turning autocommit ON commits a running transaction, so COMMIT or ROLLBACK must come first.
static void db_end(MYSQL* m) {
    if (mysql_autocommit(m, true) && mysql_query(m, "SET autocommit = 1") != 0)
        log_tag("CHECKOUT", "BUG: could not turn autocommit back on: " + string(mysql_error(m)));
}

// 1213 = deadlock (InnoDB cancelled our transaction), 1205 = lock wait timeout: both are worth retrying.
static int fail_code(int err, CheckoutResult& r, const string& why) {
    r.message = why;
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

// Runs a query that must change exactly one row (n == 1), otherwise error.
static bool exec_one(MYSQL* m, const string& sql, const vector<string>& args, int& err) {
    return db_exec_on(m, sql, args, &err) == 1;
}

// The work inside the transaction (db_begin is done). The caller does COMMIT or ROLLBACK.
static int checkout_steps(MYSQL* m, int user_id, int address_id, const PaymentInfo& pay, CheckoutResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;

    // STEP 1: lock the user's ACTIVE cart FOR UPDATE: a second checkout of the same user waits here (no double order).
    // The cart comes from the session, never from the form.
    if (!db_select_on(m, "SELECT cart_id FROM cart WHERE user_id = ? AND status = 'ACTIVE' FOR UPDATE",
                      {to_string(user_id)}, rows, &err))
        return fail_code(err, r, "Database error");
    if (rows.empty() || rows[0].empty()) { r.message = "Your cart is empty."; return CO_PROBLEM; }
    string cart_id = rows[0][0];

    // STEP 1b: the address must belong to this user and be active. FOR SHARE: nobody can change or delete it until we finish.
    // Not found = 404 (someone else's address also looks like "not found").
    if (!db_select_on(m, "SELECT address_id FROM addresses WHERE address_id = ? AND user_id = ? AND is_active = TRUE FOR SHARE",
                      {to_string(address_id), to_string(user_id)}, rows, &err))
        return fail_code(err, r, "Database error");
    if (rows.empty()) { r.status = 404; r.message = "That shipping address was not found. Please choose another."; return CO_PROBLEM; }

    // STEP 2: lock cart items and products FOR UPDATE, ORDER BY product_id: everybody locks in the same order.
    // A 1->2 and a 2->1 locker would wait for each other (DEADLOCK); a fixed order prevents it. After this the stock cannot change.
    vector<vector<string>> items;       // product_id, name, price, quantity, stock, is_active (ordered by product_id).
    if (!db_select_on(m, "SELECT p.product_id, p.name, p.price, ci.quantity, p.stock, p.is_active "
                         "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id "
                         "WHERE ci.cart_id = ? ORDER BY p.product_id FOR UPDATE",
                      {cart_id}, items, &err))
        return fail_code(err, r, "Database error");
    if (items.empty()) { r.message = "Your cart is empty."; return CO_PROBLEM; }
    for (auto& row : items)
        if (row.size() < 6) return fail_code(0, r, "Database error");

    // (Testing only) hold the locks so two checkouts show a race.
    if (CHECKOUT_DELAY_MS > 0) this_thread::sleep_for(chrono::milliseconds(CHECKOUT_DELAY_MS));

    // STEP 3: check the stock now. The rows are locked, so the stock we see cannot change.
    for (auto& row : items) {
        int qty = atoi(row[3].c_str());
        int stock = atoi(row[4].c_str());
        // The admin hid the product (rows are locked, so it cannot change now): no order, everything is rolled back.
        if (row[5] == "0") { r.message = row[1] + " is no longer available."; return CO_PROBLEM; }
        if (stock == 0) { r.message = row[1] + " is out of stock."; return CO_PROBLEM; }
        if (qty > stock) {
            r.message = "Only " + to_string(stock) + " of " + row[1] + " available, but you have " +
                        to_string(qty) + " in your cart.";
            return CO_PROBLEM;
        }
    }

    // STEP 4: create the order. The total is computed from the DB prices, never from the form or cart.
    // next_step_at = now + random delay: the delivery thread will move the order to PACKED then.
    if (!exec_one(m, "INSERT INTO orders (user_id, cart_id, total, status, next_step_at) "
                     "SELECT ?, ?, SUM(p.price * ci.quantity), 'PLACED', " + next_step_sql("PLACED") + " "
                     "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id WHERE ci.cart_id = ?",
                  {to_string(user_id), cart_id, cart_id}, err))
        return fail_code(err, r, "Database error");
    // The id of the order just created on this connection (each connection has its own LAST_INSERT_ID).
    if (!db_select_on(m, "SELECT LAST_INSERT_ID()", {}, rows, &err) || rows.empty() || rows[0][0] == "0")
        return fail_code(err, r, "Database error");
    string order_id = rows[0][0];
    r.order_id = atoi(order_id.c_str());

    // First history row (PLACED) in the same transaction: both exist or neither does.
    if (!exec_one(m, "INSERT INTO order_status_history (order_id, status, changed_by) VALUES (?, 'PLACED', 'CUSTOMER')",
                  {order_id}, err))
        return fail_code(err, r, "Database error");

    // Address SNAPSHOT: copied as text (state NAME included), like the price in order_items.
    // Later edits or deletes of the address cannot change this order.
    if (!exec_one(m, "INSERT INTO order_addresses (order_id, first_name, last_name, phone, line1, line2, landmark, city, "
                     "state_name, pincode, address_type) "
                     "SELECT ?, a.first_name, a.last_name, a.phone, a.line1, a.line2, a.landmark, a.city, s.state_name, "
                     "a.pincode, a.address_type FROM addresses a JOIN states s ON s.state_id = a.state_id "
                     "WHERE a.address_id = ? AND a.user_id = ?",
                  {order_id, to_string(address_id), to_string(user_id)}, err))
        return fail_code(err, r, "Database error");

    // (Testing only) error here: the order INSERT is done, so ROLLBACK must remove it.
    if (TEST_FAIL_CHECKOUT) return fail_code(0, r, "Test failure after INSERT orders");

    // STEP 5: order_items keep each item's CURRENT price (price at purchase), so later price changes do not change old orders.
    // The number of rows added must equal the number of items.
    int n = db_exec_on(m, "INSERT INTO order_items (order_id, product_id, quantity, price) "
                          "SELECT ?, p.product_id, ci.quantity, p.price "
                          "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id WHERE ci.cart_id = ?",
                       {order_id, cart_id}, &err);
    if (n < 0) return fail_code(err, r, "Database error");
    if (n != (int)items.size()) return fail_code(0, r, "Database error");

    // STEP 6: reduce the stock in product_id order. AND stock >= ? is the last guard: stock never goes negative (the table has CHECK too).
    // Exactly one row must change, otherwise error.
    for (auto& it : items) {
        if (!exec_one(m, "UPDATE products SET stock = stock - ? WHERE product_id = ? AND stock >= ?",
                      {it[3], it[0], it[3]}, err))
            return fail_code(err, r, err == 0 ? "Stock changed, please try again." : "Database error");
    }

    // STEP 7: payment in the same transaction, after order and stock are locked and before COMMIT.
    // If the bank declines: ROLLBACK, so no order and no money taken. A separate transaction could take money without an order (or the reverse).
    if (pay.declined) {
        r.message = "Payment declined by the bank. No money was taken and the order was not placed.";
        return CO_PROBLEM;
    }
    // COD: PENDING, paid_at NULL. Online: PAID now, with paid_at, pay_detail and txn_ref.
    // NULLIF(?, '') stores empty text as NULL. NOW()/NULL are our own fixed text; everything else is bound with ?.
    bool online = pay.method != "COD";
    if (!exec_one(m, "INSERT INTO payments (order_id, amount, method, pay_detail, txn_ref, status, paid_at) "
                     "SELECT order_id, total, ?, NULLIF(?, ''), NULLIF(?, ''), ?, " + string(online ? "NOW()" : "NULL") +
                     " FROM orders WHERE order_id = ?",
                  {pay.method, pay.detail, pay.txn_ref, online ? "PAID" : "PENDING", order_id}, err))
        return fail_code(err, r, "Database error");

    // STEP 8: the cart becomes CHECKED_OUT (the next time a new ACTIVE cart is created).
    if (!exec_one(m, "UPDATE cart SET status = 'CHECKED_OUT' WHERE cart_id = ?", {cart_id}, err))
        return fail_code(err, r, "Database error");

    if (!db_select_on(m, "SELECT total FROM orders WHERE order_id = ?", {order_id}, rows, &err) ||
        rows.empty() || rows[0].empty())
        return fail_code(err, r, "Database error");
    r.total = rows[0][0];
    return CO_OK;
}

// One full attempt: BEGIN, steps, COMMIT (or ROLLBACK). Returns a CO_* code; r holds the status and message.
static int checkout_attempt(MYSQL* m, int user_id, int address_id, const PaymentInfo& pay, CheckoutResult& r) {
    string who = "user " + to_string(user_id);
    if (!db_begin(m)) {
        r.status = 500;
        r.message = "Database error";
        return CO_FAIL;
    }

    int err = 0;
    int code = checkout_steps(m, user_id, address_id, pay, r, err);

    // STEP 9: if all went well, COMMIT: all changes become permanent and visible together.
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = fail_code(err, r, "Could not save the order");
    }

    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        metric_checkout(true);                  // COMMIT count for /metrics.
        log_tag("CHECKOUT", who + " order " + to_string(r.order_id) + " total " + r.total + " COMMIT");
        return code;
    }

    // On any problem ROLLBACK: everything this transaction changed (order, stock, payment...) is undone.
    // (On a deadlock InnoDB has already rolled back; rolling back again is harmless.)
    mysql_rollback(m);
    metric_checkout(false);                     // ROLLBACK count for /metrics.
    r.status = (code == CO_PROBLEM) ? (r.status == 404 ? 404 : 409) : 500;   // 404 = address not found (set by the steps).
    string order = r.order_id > 0 ? " order " + to_string(r.order_id) : "";
    log_tag("CHECKOUT", who + order + " ROLLBACK reason: " + r.message +
                        (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    r.order_id = 0;                     // Rolled back: that order does not exist.
    return code;
}

CheckoutResult do_checkout(int user_id, int address_id, const PaymentInfo& pay) {
    CheckoutResult r;
    int id = 0;
    MYSQL* m = get_conn(id);            // The whole checkout runs on this one connection (a transaction belongs to one connection).
    if (m == nullptr) return r;

    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = CheckoutResult();
        int code = checkout_attempt(m, user_id, address_id, pay, r);
        if (code != CO_RETRY) break;

        // Deadlock/timeout: start the whole transaction again (it was rolled back, nothing is half done).
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
            log_tag("CHECKOUT", "user " + to_string(user_id) + " gave up after " + to_string(attempt) + " attempts");
        } else {
            log_tag("CHECKOUT", "user " + to_string(user_id) + " retry " + to_string(attempt + 1) + "/" +
                                to_string(CHECKOUT_RETRIES));
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));   // Wait a little so the other transaction can finish.
        }
    }

    db_end(m);                          // Autocommit back ON, always.
    put_conn(id);
    return r;
}

// ---- shipping addresses ----

// Counts characters, not bytes (MySQL VARCHAR(n) counts characters); UTF-8 continuation bytes (10xxxxxx) are skipped.
static size_t addr_chars(const string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

static bool all_digits(const string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Rejects control characters: an address is one line of text.
static bool has_control(const string& s) {
    for (unsigned char c : s)
        if (c < 32 || c == 127) return true;
    return false;
}

// Checks one text field: required, max length, no control characters. Returns a message or "".
static string addr_text_check(const string& label, const string& v, size_t max_chars, bool required) {
    if (v.empty()) return required ? label + " is required." : "";
    if (addr_chars(v) > max_chars) return label + " can have at most " + to_string(max_chars) + " characters.";
    if (has_control(v)) return label + " has characters that are not allowed.";
    return "";
}

// Checks the whole form: trim, then field by field; returns the first problem, "" = OK.
// The same rules exist as DB CHECKs (phone 10 digits, pincode 6 digits not starting with 0, type): the last line of defence.
string address_check(Address& a) {
    a.first_name = trim(a.first_name);
    a.last_name = trim(a.last_name);
    a.phone = trim(a.phone);
    a.line1 = trim(a.line1);
    a.line2 = trim(a.line2);
    a.landmark = trim(a.landmark);
    a.city = trim(a.city);
    a.state_id = trim(a.state_id);
    a.pincode = trim(a.pincode);
    a.type = trim(a.type);

    string e;
    if (!(e = addr_text_check("First name", a.first_name, 50, true)).empty()) return e;
    if (!(e = addr_text_check("Last name", a.last_name, 50, true)).empty()) return e;
    if (!all_digits(a.phone, 10)) return "Phone number must be exactly 10 digits (numbers only).";
    if (!(e = addr_text_check("Address line 1", a.line1, 150, true)).empty()) return e;
    if (!(e = addr_text_check("Address line 2", a.line2, 150, false)).empty()) return e;
    if (!(e = addr_text_check("Landmark", a.landmark, 100, false)).empty()) return e;
    if (!(e = addr_text_check("City", a.city, 80, true)).empty()) return e;
    if (a.state_id.empty() || a.state_id.size() > 9 || !all_digits(a.state_id, a.state_id.size())) return "Please choose a state.";
    if (!all_digits(a.pincode, 6) || a.pincode[0] == '0') return "Pincode must be 6 digits and cannot start with 0.";
    if (a.type != "HOME" && a.type != "WORK" && a.type != "OTHER") return "Address type must be Home, Work or Other.";
    return "";
}

// Address work: like checkout, one connection and one transaction. The CO_* codes are defined above.
static int addr_fail(int err, AddressResult& r) {
    r.status = (err == 1213 || err == 1205) ? 503 : 500;
    r.message = r.status == 503 ? "The shop is busy right now. Please try again in a moment." : "Database error";
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

static int addr_problem(AddressResult& r, int status, const string& message) {
    r.status = status;
    r.message = message;
    return CO_PROBLEM;
}

// New address or edit. First lock the user's own row FOR UPDATE: this user's address changes run one at a time,
// so two tabs cannot both pass the limit. Nobody else locks that row (no deadlock). The first address becomes the default.
static int addr_save_steps(MYSQL* m, int user_id, const Address& a, bool is_new, AddressResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;
    const string uid = to_string(user_id);

    if (!db_select_on(m, "SELECT user_id FROM users WHERE user_id = ? FOR UPDATE", {uid}, rows, &err)) return addr_fail(err, r);
    if (rows.empty()) return addr_problem(r, 404, "User not found");

    if (is_new) {
        if (!db_select_on(m, "SELECT COUNT(*), COALESCE(SUM(is_default), 0) FROM addresses WHERE user_id = ? AND is_active = TRUE",
                          {uid}, rows, &err) || rows.empty() || rows[0].size() < 2)
            return addr_fail(err, r);
        if (atoi(rows[0][0].c_str()) >= MAX_ADDRESSES)
            return addr_problem(r, 400, "You can save at most " + to_string(MAX_ADDRESSES) + " addresses. Delete one first.");
        string first = (rows[0][1] == "0") ? "1" : "0";         // No default yet: this one becomes the default.

        int n = db_exec_on(m, "INSERT INTO addresses (user_id, first_name, last_name, phone, line1, line2, landmark, city, "
                              "state_id, pincode, address_type, is_default) VALUES (?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), ?, ?, ?, ?, ?)",
                           {uid, a.first_name, a.last_name, a.phone, a.line1, a.line2, a.landmark, a.city, a.state_id, a.pincode,
                            a.type, first}, &err);
        if (err == 1452) return addr_problem(r, 400, "Please choose a valid state.");   // Foreign key failed: no such state.
        if (n != 1) return addr_fail(err, r);
        if (!db_select_on(m, "SELECT LAST_INSERT_ID()", {}, rows, &err) || rows.empty() || rows[0].empty()) return addr_fail(err, r);
        r.address_id = atoi(rows[0][0].c_str());
        return CO_OK;
    }

    // Edit: AND user_id = ? AND is_active: someone else's or a deleted address matches 0 rows = 404.
    int n = db_exec_on(m, "UPDATE addresses SET first_name = ?, last_name = ?, phone = ?, line1 = ?, line2 = NULLIF(?, ''), "
                          "landmark = NULLIF(?, ''), city = ?, state_id = ?, pincode = ?, address_type = ? "
                          "WHERE address_id = ? AND user_id = ? AND is_active = TRUE",
                       {a.first_name, a.last_name, a.phone, a.line1, a.line2, a.landmark, a.city, a.state_id, a.pincode, a.type,
                        a.id, uid}, &err);
    if (err == 1452) return addr_problem(r, 400, "Please choose a valid state.");
    if (n < 0) return addr_fail(err, r);
    if (n == 0) return addr_problem(r, 404, "Address not found");
    r.address_id = atoi(a.id.c_str());
    return CO_OK;
}

// Set default: lock all the user's active addresses FOR UPDATE (in address_id order), clear the old default, set the new one,
// in one transaction. Clear first: UNIQUE(default_user) would reject two defaults at once.
static int addr_default_steps(MYSQL* m, int user_id, int address_id, AddressResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;
    const string uid = to_string(user_id);

    if (!db_select_on(m, "SELECT address_id FROM addresses WHERE user_id = ? AND is_active = TRUE ORDER BY address_id FOR UPDATE",
                      {uid}, rows, &err))
        return addr_fail(err, r);
    bool found = false;
    for (auto& row : rows)
        if (!row.empty() && atoi(row[0].c_str()) == address_id) found = true;
    if (!found) return addr_problem(r, 404, "Address not found");

    if (db_exec_on(m, "UPDATE addresses SET is_default = FALSE WHERE user_id = ? AND is_default = TRUE", {uid}, &err) < 0)
        return addr_fail(err, r);
    if (db_exec_on(m, "UPDATE addresses SET is_default = TRUE WHERE address_id = ? AND user_id = ?",
                   {to_string(address_id), uid}, &err) != 1)
        return addr_fail(err, r);
    r.address_id = address_id;
    return CO_OK;
}

// Runs one transaction: BEGIN, steps, COMMIT (or ROLLBACK). which: 0 = save, 1 = set default.
static int addr_attempt(MYSQL* m, int which, int user_id, const Address& a, bool is_new, int address_id, AddressResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        return CO_FAIL;
    }
    int err = 0;
    int code = (which == 0) ? addr_save_steps(m, user_id, a, is_new, r, err) : addr_default_steps(m, user_id, address_id, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = addr_fail(err, r);
    }
    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        return code;
    }
    mysql_rollback(m);
    log_tag("DB", "address change of user " + to_string(user_id) + " ROLLBACK reason: " + r.message +
                  (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

static AddressResult addr_run(int which, int user_id, const Address& a, bool is_new, int address_id) {
    AddressResult r;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return r;
    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = AddressResult();
        int code = addr_attempt(m, which, user_id, a, is_new, address_id, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
        } else {
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }
    db_end(m);                          // Autocommit back ON, always.
    put_conn(id);
    return r;
}

AddressResult address_save(int user_id, const Address& a, bool is_new) {
    return addr_run(0, user_id, a, is_new, 0);
}

AddressResult address_set_default(int user_id, int address_id) {
    return addr_run(1, user_id, Address(), false, address_id);
}

// Delete = is_active = 0 (soft delete). Old orders keep their own copy in order_addresses. A deleted default leaves no default.
// A single UPDATE is atomic by itself, so no transaction is needed.
AddressResult address_delete(int user_id, int address_id) {
    AddressResult r;
    int err = 0;
    int n = db_exec("UPDATE addresses SET is_active = FALSE, is_default = FALSE WHERE address_id = ? AND user_id = ? AND is_active = TRUE",
                    {to_string(address_id), to_string(user_id)}, &err);
    if (n < 0) return r;
    if (n == 0) { r.status = 404; r.message = "Address not found"; return r; }
    r.status = 200;
    r.message = "";
    r.address_id = address_id;
    return r;
}

// ---- change password: one DB transaction ----
// The database stores a password hash (password_hash.h). Hashing and verifying the typed password happen in auth.cpp
// (CPU work, done without holding a DB connection); this code only replaces the stored value. The rules live in
// check_password, not in a DB CHECK: they apply to the typed password, and a hash does not look like one.
static int pw_fail(int err, PasswordResult& r) {
    r.status = (err == 1213 || err == 1205) ? 503 : 500;
    r.message = r.status == 503 ? "The shop is busy right now. Please try again in a moment." : "Database error";
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

// 1) One UPDATE checks that the stored value is still the one auth.cpp verified (BINARY = byte-by-byte) and changes it:
//    no gap for someone else to change it in between. 0 rows = it changed or the user is gone -> 403.
// 2) End the user's other sessions (LOGOUT), keep the current one. Both steps commit together or not at all.
static int pw_steps(MYSQL* m, int user_id, const string& current, const string& new_pw, const string& keep_session, PasswordResult& r, int& err) {
    err = 0;
    int n = db_exec_on(m, "UPDATE users SET password = ? WHERE user_id = ? AND BINARY password = ?",
                       {new_pw, to_string(user_id), current}, &err);
    if (n < 0) return pw_fail(err, r);
    if (n == 0) {
        r.status = 403;
        r.message = "Current password is incorrect.";
        return CO_PROBLEM;
    }
    int ended = db_exec_on(m, "UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'LOGOUT' "
                              "WHERE user_id = ? AND is_active = 1 AND session_id <> ?",
                           {to_string(user_id), keep_session}, &err);
    if (ended < 0) return pw_fail(err, r);
    r.ended = ended;
    return CO_OK;
}

static int pw_attempt(MYSQL* m, int user_id, const string& current, const string& new_pw, const string& keep_session, PasswordResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        return CO_FAIL;
    }
    int err = 0;
    int code = pw_steps(m, user_id, current, new_pw, keep_session, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = pw_fail(err, r);
    }
    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        return code;
    }
    mysql_rollback(m);
    // The password is never logged: only which user and why it failed.
    log_tag("DB", "password change of user " + to_string(user_id) + " ROLLBACK reason: " + r.message +
                  (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

PasswordResult password_change(int user_id, const string& current, const string& new_pw, const string& keep_session) {
    PasswordResult r;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return r;
    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = PasswordResult();
        int code = pw_attempt(m, user_id, current, new_pw, keep_session, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
        } else {
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }
    db_end(m);                          // Autocommit back ON, always.
    put_conn(id);
    return r;
}

// ---- order status change (admin: ship/deliver/cancel, customer: cancel): one DB transaction ----
// Allowed: one step forward (PLACED->...->DELIVERED) or CANCELLED before DELIVERED; a customer only from PLACED/PACKED (checked in order_steps).
bool order_transition_ok(const string& from, const string& to) {
    int f = flow_index(from);
    if (f < 0) return false;                    // CANCELLED or unknown: no way out.
    if (to == "CANCELLED") return f < FLOW_N - 1;
    return flow_index(to) == f + 1;
}

// Like checkout: retry on deadlock or lock timeout, other errors give an error page.
static int order_fail(int err, OrderResult& r, const string& why) {
    r.status = 500;
    r.message = why;
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

// User-level problem (order not found, path not allowed): ROLLBACK and show status + message.
static int order_problem(OrderResult& r, int status, const string& message) {
    r.status = status;
    r.message = message;
    return CO_PROBLEM;
}

// The work inside the transaction (db_begin is done); the caller does COMMIT or ROLLBACK.
static int order_steps(MYSQL* m, int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                       const string& by, OrderResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;
    const string oid = to_string(order_id);

    // STEP 1: lock the order FOR UPDATE: another admin or the customer's cancel waits here,
    // and after we get the lock the status we see is the truth.
    if (!db_select_on(m, "SELECT status, user_id, IFNULL(DATE_FORMAT(next_step_at, '%Y-%m-%d %H:%i:%s'), '') FROM orders WHERE order_id = ? FOR UPDATE", {oid}, rows, &err))
        return order_fail(err, r, "Database error");
    if (rows.empty() || rows[0].size() < 3) return order_problem(r, 404, "Order not found");
    const string from = rows[0][0];
    r.from = from;
    // A delivery-thread step counts as happening at its SCHEDULED time (changed_at, paid_at, next step), even if the thread ran late.
    // For admin and customer there is no schedule: "now" is right.
    const string sched = (by == "SYSTEM") ? rows[0][2] : "";

    // STEP 2: checks. A customer can only see or cancel their own order (someone else's looks like "not found"), only PLACED or PACKED.
    // The server decides; the page buttons are only a convenience.
    if (!as_admin && atoi(rows[0][1].c_str()) != user_id) return order_problem(r, 404, "Order not found");
    if (!expected_from.empty() && expected_from != from)
        return order_problem(r, 409, "This order was changed by someone else (now " + from + "). Reload the page and check it.");
    if (!order_transition_ok(from, to) || (!as_admin && !((from == "PLACED" || from == "PACKED") && to == "CANCELLED")))
        return order_problem(r, 400, "Not allowed: an order cannot go from " + from + " to " + to + ".");

    if (to == "CANCELLED") {
        // STEP 3: put each item's quantity back into stock, in product_id order (checkout locks in the same order,
        // so there is no deadlock).
        vector<vector<string>> items;
        if (!db_select_on(m, "SELECT product_id, quantity FROM order_items WHERE order_id = ? ORDER BY product_id",
                          {oid}, items, &err))
            return order_fail(err, r, "Database error");
        for (auto& it : items) {
            if (it.size() < 2) return order_fail(0, r, "Database error");
            if (!exec_one(m, "UPDATE products SET stock = stock + ? WHERE product_id = ?", {it[1], it[0]}, err))
                return order_fail(err, r, "Database error");
        }
    }

    // STEP 4: change the status and the next step's time. AND status = <old>: if it already changed we get 0 rows
    // and ROLLBACK.
    if (!exec_one(m, "UPDATE orders SET status = ?, next_step_at = " + next_step_sql(to, !sched.empty()) + " WHERE order_id = ? AND status = ?",
                  {to, oid, from}, err))
        return order_fail(err, r, err == 0 ? "The order changed, please try again." : "Database error");

    // STEP 4b: history row in the same transaction: status and history change together (changed_by = who did it).
    // changed_at is the scheduled time if there is one, otherwise the DEFAULT (now).
    bool hist_ok = sched.empty()
        ? exec_one(m, "INSERT INTO order_status_history (order_id, status, changed_by) VALUES (?, ?, ?)", {oid, to, by}, err)
        : exec_one(m, "INSERT INTO order_status_history (order_id, status, changed_at, changed_by) VALUES (?, ?, ?, ?)",
                   {oid, to, sched, by}, err);
    if (!hist_ok) return order_fail(err, r, "Database error");

    // STEP 5: payment in the same transaction; middle steps do not touch it. DELIVERED: only a PENDING (COD) payment becomes PAID;
    // an online payment keeps its paid_at (0 rows is fine, so no exec_one). CANCELLED: online PAID -> REFUNDED, COD -> CANCELLED.
    if (to == "DELIVERED") {
        int n = sched.empty()
            ? db_exec_on(m, "UPDATE payments SET status = 'PAID', paid_at = NOW() WHERE order_id = ? AND status = 'PENDING'", {oid}, &err)
            : db_exec_on(m, "UPDATE payments SET status = 'PAID', paid_at = ? WHERE order_id = ? AND status = 'PENDING'", {sched, oid}, &err);
        if (n < 0) return order_fail(err, r, "Database error");
    } else if (to == "CANCELLED") {
        if (db_exec_on(m, "UPDATE payments SET status = IF(status = 'PAID', 'REFUNDED', 'CANCELLED') WHERE order_id = ?",
                       {oid}, &err) < 0)
            return order_fail(err, r, "Database error");
    }

    // STEP 6: an admin change also writes its audit row in this transaction (change and record are saved together).
    if (by == "ADMIN" && !exec_one(m, "INSERT INTO admin_actions (admin_id, action, details) VALUES (?, 'ORDER_STATUS', ?)",
                              {to_string(user_id), "order " + oid + " " + from + "->" + to}, err))
        return order_fail(err, r, "Database error");
    return CO_OK;
}

static int order_attempt(MYSQL* m, int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                         const string& by, OrderResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        r.message = "Database error";
        return CO_FAIL;
    }
    int err = 0;
    int code = order_steps(m, order_id, to, user_id, as_admin, expected_from, by, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = order_fail(err, r, "Could not save the change");
    }
    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        if (to == "CANCELLED")
            log_tag("ORDER", "order " + to_string(order_id) + " CANCELLED by " + by + " " + to_string(user_id) + ", stock restored");
        return code;
    }
    mysql_rollback(m);                  // Nothing is left half done: stock, status and payment are as before.
    log_tag("ORDER", "order " + to_string(order_id) + " -> " + to + " ROLLBACK reason: " + r.message +
                     (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

OrderResult order_change_by(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                            const string& changed_by) {
    OrderResult r;
    int id = 0;
    MYSQL* m = get_conn(id);            // The whole transaction runs on this one connection.
    if (m == nullptr) return r;

    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = OrderResult();
        int code = order_attempt(m, order_id, to, user_id, as_admin, expected_from, changed_by, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
            log_tag("ORDER", "order " + to_string(order_id) + " gave up after " + to_string(attempt) + " attempts");
        } else {
            log_tag("ORDER", "order " + to_string(order_id) + " retry " + to_string(attempt + 1) + "/" + to_string(CHECKOUT_RETRIES));
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }

    db_end(m);                          // Autocommit back ON, always.
    put_conn(id);
    return r;
}

// Entry point for pages.cpp: who changes the order follows from as_admin.
OrderResult order_change(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from) {
    return order_change_by(order_id, to, user_id, as_admin, expected_from, as_admin ? "ADMIN" : "CUSTOMER");
}

// Delivery thread work: move orders whose next_step_at has come (or passed while the server was down) one step. One short transaction
// per order, so one failure does not stop the rest. A customer cancel in between gives 409 (UPDATE ... WHERE status = <old>): we skip it.
int deliver_due_orders() {
    vector<vector<string>> rows;
    if (!db_select("SELECT order_id, status FROM orders WHERE status NOT IN ('DELIVERED', 'CANCELLED') "
                   "AND next_step_at <= NOW() ORDER BY next_step_at LIMIT 50", {}, rows))
        return -1;

    int moved = 0;
    for (auto& row : rows) {
        if (row.size() < 2) continue;
        // Keep moving while the next step is due (next_step_at <= NOW()): several steps in one tick after downtime.
        // Each step is its own short transaction. At most FLOW_N steps (the length of the path).
        string from = row[1], now_at = row[1];
        int steps = 0;
        for (int k = 0; k < FLOW_N; k++) {
            string to = order_next_status(now_at);
            if (to.empty()) break;
            OrderResult r = order_change_by(atoi(row[0].c_str()), to, 0, true, now_at, "SYSTEM");
            if (r.status != 200) {
                if (r.status != 409)        // 409 = someone changed it meanwhile: normal, we look again next tick.
                    log_tag("DELIVERY", "order " + row[0] + " " + now_at + "->" + to + " failed: " + r.message);
                break;
            }
            steps++;
            moved++;
            if (steps == 1) log_tag("DELIVERY", "order " + row[0] + " " + now_at + "->" + to);
            now_at = to;

            vector<vector<string>> again;   // Is the next step due too?
            if (!db_select("SELECT status FROM orders WHERE order_id = ? AND status NOT IN ('DELIVERED', 'CANCELLED') "
                           "AND next_step_at <= NOW()", {row[0]}, again) || again.empty() || again[0].empty())
                break;
        }
        if (steps > 1)
            log_tag("DELIVERY", "order " + row[0] + " caught up " + to_string(steps) + " steps after restart (" + from + "->" + now_at + ")");
    }
    return moved;
}

// ---- admin audit: one row per admin change (admin_actions) ----
// Cut by characters, not bytes, so a UTF-8 character is never split (MySQL would reject it).
static string cut_chars(const string& s, size_t max_chars) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (chars == max_chars) break;
        chars++;
        i += len;
    }
    return s.substr(0, i);
}

// For changes without a transaction (product, category, session end). If the row cannot be written we only log it (the change is done).
// Changes with a transaction (order status, role) write the audit row inside it.
void admin_audit(int admin_id, const string& action, const string& details) {
    int err = 0;
    int n = db_exec("INSERT INTO admin_actions (admin_id, action, details) VALUES (?, ?, ?)",
                    {to_string(admin_id), cut_chars(action, 50), cut_chars(details, 255)}, &err);
    if (n != 1) log_tag("ADMIN", "could not write the audit row (MySQL error " + to_string(err) + "): " + action + " " + details);
}

// ---- change a user's role: one DB transaction. The last ADMIN is never demoted; nobody changes their own role ----
// ADMIN rows are locked FOR UPDATE and counted after the lock, so two admins demoting each other cannot both succeed.
static int role_fail(int err, RoleResult& r) {
    r.status = 500;
    r.code = "db_error";
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

static int role_problem(RoleResult& r, int status, const string& code) {
    r.status = status;
    r.code = code;
    return CO_PROBLEM;
}

static int role_steps(MYSQL* m, int admin_id, int user_id, const string& new_role, RoleResult& r, int& err) {
    vector<vector<string>> admins, target;
    err = 0;

    // STEP 1: lock all ADMIN rows FOR UPDATE, then count them.
    if (!db_select_on(m, "SELECT user_id FROM users WHERE role = 'ADMIN' ORDER BY user_id FOR UPDATE", {}, admins, &err))
        return role_fail(err, r);
    bool actor_is_admin = false;
    for (auto& a : admins) {
        if (a.empty()) return role_fail(0, r);
        if (atoi(a[0].c_str()) == admin_id) actor_is_admin = true;
    }
    // While waiting for the lock this admin may have lost their role: then their change is refused too.
    if (!actor_is_admin) return role_problem(r, 403, "not_admin");

    // STEP 2: lock the target user's row too.
    if (!db_select_on(m, "SELECT role FROM users WHERE user_id = ? FOR UPDATE", {to_string(user_id)}, target, &err))
        return role_fail(err, r);
    if (target.empty() || target[0].empty()) return role_problem(r, 404, "no_user");
    const string current = target[0][0];
    r.from = current;
    if (current == new_role) return role_problem(r, 400, "same_role");
    if (current == "ADMIN" && new_role == "CUSTOMER" && admins.size() <= 1) return role_problem(r, 400, "last_admin");

    // STEP 3: change it (AND role = <old>: exactly 1 row) and write the audit row in the same transaction.
    if (!exec_one(m, "UPDATE users SET role = ? WHERE user_id = ? AND role = ?", {new_role, to_string(user_id), current}, err))
        return role_fail(err, r);
    if (!exec_one(m, "INSERT INTO admin_actions (admin_id, action, details) VALUES (?, 'USER_ROLE', ?)",
                  {to_string(admin_id), "user " + to_string(user_id) + " role " + current + "->" + new_role}, err))
        return role_fail(err, r);
    return CO_OK;
}

static int role_attempt(MYSQL* m, int admin_id, int user_id, const string& new_role, RoleResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        r.code = "db_error";
        return CO_FAIL;
    }
    int err = 0;
    int code = role_steps(m, admin_id, user_id, new_role, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = role_fail(err, r);
    }
    if (code == CO_OK) {
        r.status = 200;
        r.code = "";
        return code;
    }
    mysql_rollback(m);
    log_tag("ADMIN", "role change of user " + to_string(user_id) + " by user " + to_string(admin_id) + " ROLLBACK reason: " + r.code +
                     (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

RoleResult role_change(int admin_id, int user_id, const string& new_role) {
    RoleResult r;
    if (new_role != "ADMIN" && new_role != "CUSTOMER") { r.status = 400; r.code = "bad_request"; return r; }
    if (admin_id == user_id) { r.status = 400; r.code = "own_role"; return r; }     // Not your own role: checked before the transaction.

    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return r;
    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = RoleResult();
        int code = role_attempt(m, admin_id, user_id, new_role, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.code = "busy";
            log_tag("ADMIN", "role change of user " + to_string(user_id) + " gave up after " + to_string(attempt) + " attempts");
        } else {
            log_tag("ADMIN", "role change retry " + to_string(attempt + 1) + "/" + to_string(CHECKOUT_RETRIES));
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }
    db_end(m);
    put_conn(id);
    return r;
}

void db_thread_start() {
    if (mysql_thread_init() != 0) log_tag("DB", "mysql_thread_init failed");
}

void db_thread_end() {
    mysql_thread_end();
}

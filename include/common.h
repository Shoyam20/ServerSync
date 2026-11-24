#pragma once
#include <winsock2.h>   // Must come before windows.h; gives the SOCKET type.
#include <string>
#include <map>
#include <vector>
using namespace std;

struct MYSQL;   // The real mysql.h is only needed in db.cpp.

// ---- settings ----
const int PORT = 8080;
const int WORKERS = 4;                    // Worker threads.
const int QUEUE_SIZE = 16;                // Max clients waiting in the queue.
const int DB_POOL = 4;                    // MySQL connections (pool size).
const int MAX_HEADER = 8 * 1024;          // Max header size (8 KB).
const int MAX_BODY = 1024 * 1024;         // Max body size (1 MB).
const int RECV_TIMEOUT_MS = 5000;         // A slow client waits at most 5 s.
const int SESSION_MINUTES = 60;           // Minutes a login session lasts without activity.
const int CLEANUP_SECONDS = 60;           // Seconds between runs of the session cleanup thread.
const int MAX_CART_QTY = 10;              // Max quantity of one product in the cart.
const int PW_MIN_LEN = 8;                 // Minimum password length (characters).
const int PW_MAX_LEN = 64;                // Maximum password length (characters).
const int MAX_ADDRESSES = 10;            // Max active shipping addresses per user.
const int CHECKOUT_RETRIES = 3;           // Tries for a transaction on deadlock (1213) or lock wait timeout (1205).

// Auto shipment tracking: the delivery thread moves an order PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED.
const int AUTO_DELIVERY = 1;              // 1 = delivery thread on, 0 = off (only the admin changes the status).
const int DELIVERY_TICK_SECONDS = 5;      // Seconds between checks for orders that are due.
// Delay (seconds) before an order enters each status; random between min and max.
// Small demo numbers; a real shop would use hours or days.
const int DELAY_PACKED_MIN = 20,           DELAY_PACKED_MAX = 40;             // PLACED -> PACKED
const int DELAY_SHIPPED_MIN = 30,          DELAY_SHIPPED_MAX = 60;            // PACKED -> SHIPPED
const int DELAY_OUT_FOR_DELIVERY_MIN = 40, DELAY_OUT_FOR_DELIVERY_MAX = 80;   // SHIPPED -> OUT_FOR_DELIVERY
const int DELAY_DELIVERED_MIN = 20,        DELAY_DELIVERED_MAX = 40;          // OUT_FOR_DELIVERY -> DELIVERED

// Folders, relative to where the server runs (the folder with db.conf).
const string STATIC_DIR = "public";       // css/svg/png/js/jpg: loaded into memory at startup, served at /static/<file>.
const string TEMPLATE_DIR = "templates";  // *.html templates: loaded into memory at startup.

// Storefront.
const int BUDGET_MAX = 1000;              // Limit (rupees) for the "Under 1,000" link and the home page budget picks.
const int LOW_STOCK = 5;                  // At or below this stock the card shows an orange "Only N left".

// Logging and metrics.
const int VERBOSE_LOG = 0;                // 0 = no per-request or "conn taken/returned" console lines (startup, errors, CHECKOUT, CLEANUP, LOGGER lines stay);
                                          // 1 = everything.
const int LOG_QUEUE_SIZE = 1000;          // Memory queue for request_logs: entries wait here; when it is full, new ones are DROPPED (and counted).
const int LOG_FLUSH_SECONDS = 1;          // Seconds between the logger thread's batch INSERTs.
const int LOG_BATCH_MAX = 200;            // Max rows in one multi-row INSERT.
const int LATENCY_WINDOW = 1000;          // Average and p95 latency are taken over this many recent requests.
const int RPS_SECONDS = 10;               // Requests per second are taken over this many recent seconds.

// Testing only (keep both at 0 normally):
const int CHECKOUT_DELAY_MS = 0;          // Milliseconds to wait AFTER locking rows in checkout, to show races.
const int TEST_FAIL_CHECKOUT = 0;         // 1 = fail on purpose after INSERT orders, to test ROLLBACK.
const int DEBUG_SQL = 0;                  // 1 = log every query's SQL and each parameter (value + length).
                                          // Values of password and user_sessions queries show as '***'.

// ---- structs ----
// The parts of one HTTP request.
struct Request {
    string method;                  // GET, POST ...
    string target;                  // As received, e.g. /products?cat=3.
    string path;                    // Without the query, e.g. /products.
    map<string, string> params;     // Query parameters, decoded: cat -> 3.
    string version;                 // HTTP/1.1
    map<string, string> headers;    // Names in lower case.
    map<string, string> cookies;    // From the Cookie header: session_id -> abc...
    string body;
    map<string, string> form;       // POST form fields, decoded: email -> a@b.com.

    // The user of this request (route() fills it via load_user); every request has its own copy.
    int user_id = 0;                // 0 = not logged in.
    string user_name;
    string role;                    // "CUSTOMER" or "ADMIN", read from the DB on every request, so a role change applies from the next request.
    string session_id;             // Current session (only when logged in).
    bool clear_cookie = false;      // The session expired: remove the cookie in the reply.
    bool refresh_cookie = false;    // Logged-in user: send the cookie again with a new Max-Age.
    int cart_count = 0;             // Total items in the user's cart (for the nav bar).
    string client_ip;               // Client IPv4, e.g. "127.0.0.1" (/metrics is local only).
};

// One file from public/, fully in memory (type = Content-Type).
struct StaticFile {
    string type;
    string data;
};

// One client in the queue: socket, accept time (queue wait and latency are measured from it), IP.
struct ClientJob {
    SOCKET sock = INVALID_SOCKET;
    long long accept_us = 0;        // now_us() when accept() returned.
    string ip;
};

// What handle_client did: which request got which answer (the worker builds the log and metrics from it).
struct RequestInfo {
    string method;
    string path;                    // Without the query.
    int status = 0;                 // 0 = nothing answered (the client left without sending a request).
    int user_id = 0;                // 0 = not logged in.
};

// One row of the request_logs table (waits in a memory queue; the logger thread writes it to the DB).
struct LogEntry {
    string method;
    string path;
    int status = 0;
    double latency_ms = 0;          // accept -> reply sent.
    double queue_wait_ms = 0;       // accept -> picked up by a worker.
    int worker_id = 0;
    int user_id = 0;                // 0 = NULL.
    long long created = 0;          // Unix seconds (the real request time, not the insert time).
};

// Snapshot for /metrics (filled by metrics_snapshot in threads.cpp).
struct Metrics {
    long long uptime_s = 0;
    long long total = 0;            // Answered requests (including 503 rejects).
    long long c2xx = 0, c3xx = 0, c4xx = 0, c5xx = 0;
    long long rejected_503 = 0;     // Queue was full.
    long long timeouts_408 = 0;
    int busy_workers = 0;           // Workers handling a request now.
    int queue_length = 0;           // Clients waiting in the queue now.
    int free_db_conns = 0;          // Free pool connections now.
    int latency_samples = 0;        // Requests used for avg/p95.
    double avg_ms = 0;
    double p95_ms = 0;
    double rps = 0;                 // Requests per second over the last RPS_SECONDS.
    long long checkout_commit = 0;
    long long checkout_rollback = 0;
    long long dropped_logs = 0;     // Log entries lost (memory queue full or DB insert failed).
};

// Result of checkout (filled by do_checkout in db.cpp).
struct CheckoutResult {
    int status = 500;               // 200 = order created, 409 = empty cart / low stock (see message),
                                    // 503 = deadlock/timeout again and again, 500 = other error, 404 = address not found (someone else's or deleted).
    int order_id = 0;               // Set when status is 200.
    string total;                   // Set when status is 200, e.g. "4497.00".
    string message = "Database error";   // Safe to show to the user (on error).
};

// A shipping address as typed (all text, even state_id); address_check validates it.
struct Address {
    string id;                      // "" = new address.
    string first_name, last_name, phone;
    string line1, line2, landmark;  // line2 and landmark are optional.
    string city, state_id, pincode;
    string type = "HOME";           // HOME, WORK or OTHER.
};

// Result of an address change.
struct AddressResult {
    int status = 500;               // 200 done, 400 bad input or limit (show the message), 404 not found (or someone else's),
                                    // 503 deadlock/timeout, 500 other error.
    string message = "Database error";
    int address_id = 0;             // On 200: the new or changed address.
};

// Result of a password change (filled by password_change in db.cpp).
struct PasswordResult {
    int status = 500;               // 200 changed, 403 wrong old password, 404 no such user, 503 deadlock/timeout, 500 other error.
    string message = "Database error";
    int ended = 0;                  // On 200: how many other sessions were ended.
};

// Payment chosen at checkout (pages.cpp fills it after checking the form). The gateway is simulated: no real money.
struct PaymentInfo {
    string method = "COD";          // COD, UPI, CARD or NETBANKING.
    string detail;                  // Safe to show: UPI ID, "VISA **** 1111" or bank name. Never the full card number or CVV.
    string txn_ref;                 // Transaction id of an online payment ("" for COD).
    bool declined = false;          // The bank refused: checkout rolls back, no order is created.
};

// ---- server.cpp ----
bool start_server();                 // WSAStartup, socket, bind, listen.
void run_server();                   // Accept loop: accepts clients and puts them in the queue.
void stop_server();                  // Close the socket, WSACleanup.
RequestInfo handle_client(const ClientJob& job);   // One client: read, parse, route, send, close. Returns what happened.
string trim(const string& s);
int load_static_files(const string& dir);                 // Loads every file of dir into memory (once at startup); returns how many.
const StaticFile* find_static(const string& name);        // "style.css" -> file, or nullptr (404) for a wrong or unknown path.
// Sends a reply; extra = more header lines, each ending in \r\n (e.g. "Retry-After: 2\r\n").
void send_response(SOCKET client, int status, const string& type, const string& body,
                   const string& extra = "");

// ---- pages.cpp ----
// Password rules: returns the messages of failed rules (empty = OK). No name or email part in it; different from old_pw when changing.
vector<string> check_password(const string& pw, const string& name, const string& email, const string& old_pw);
bool load_templates(const string& dir);                   // Loads all .html templates into memory (at startup); false if a required one is missing.
// Template name + values: {{key}} = HTML-escaped, {{{key}}} = raw (only for HTML we built ourselves).
string render(const string& name, const map<string, string>& values);
string render_error_page(const string& message);          // Full error page (with layout), used by server.cpp for 400/408/503.
int route(SOCKET client, Request& req);   // Loads the user, picks the page by path and sends it; returns the status code.
int close_expired_sessions();             // Closes all expired sessions; returns how many (-1 on error).

// ---- threads.cpp ----
long long now_us();                 // Monotonic clock in microseconds (accept time and latency).
bool push_client(const ClientJob& job);   // Puts a client in the queue; returns false at once if the queue is full (no waiting).
ClientJob pop_client();             // Takes a client from the queue; waits while it is empty.
void start_workers();               // Starts the WORKERS worker threads.
void stop_workers();                // Stops the workers and joins them.
void start_logger();                // Background thread for request_logs (batch INSERT every LOG_FLUSH_SECONDS).
void stop_logger();                 // Writes whatever is left to the DB, then stops the thread.
void push_request_log(const LogEntry& e);   // Adds to the memory queue; never blocks; if full it drops the entry and counts it.
long long metric_rejected();        // Queue full (503): counts in total and 5xx, returns the 503 count.
void metric_checkout(bool commit);  // Counts a checkout COMMIT or ROLLBACK.
Metrics metrics_snapshot();         // All current numbers (thread-safe).
void log_verbose(const string& tag, const string& msg);   // Prints only when VERBOSE_LOG = 1.
void log_msg_verbose(const string& msg);                  // log_msg, but only when VERBOSE_LOG = 1.
void start_cleanup();               // Background thread: closes expired sessions every CLEANUP_SECONDS (the delivery thread starts with it).
void stop_cleanup();                // Wakes the thread at once, stops it and joins it.
void start_delivery();              // Delivery thread: every DELIVERY_TICK_SECONDS moves due orders one step (when AUTO_DELIVERY = 1).
void stop_delivery();               // Wakes the thread at once, stops it and joins it.
void log_msg(const string& msg);    // Prints [time][THREAD n] msg, thread-safe.
void log_tag(const string& tag, const string& msg);   // Prints [time][tag] msg (e.g. tag = "DB").
int thread_id();                    // This thread's number (0 main, -1 cleanup, -2 logger, -3 delivery).
string thread_name();               // "THREAD n", "CLEANUP", "LOGGER", "DELIVERY" or "SERVER" (for logs).

// ---- db.cpp ----
bool init_db();                     // Reads db.conf and opens DB_POOL connections; false on failure.
void close_db();                    // Closes all connections.
MYSQL* get_conn(int& id);           // Takes a free connection (waits if none); id = its number.
void put_conn(int id);              // Gives the connection back to the pool.
// Prepared statements: put ? in the SQL and pass user data in args (never join it into the SQL).
// db_select: SELECT only; values come back as text (NULL = ""); false = error. conn_id (optional): which connection was used (testing only).
bool db_select(const string& sql, const vector<string>& args, vector<vector<string>>& rows,
               int* conn_id = nullptr);
// db_exec: INSERT/UPDATE/DELETE only. Returns the rows matched by WHERE (even if the value did not change: CLIENT_FOUND_ROWS), -1 on error.
// err_code (optional): MySQL error number on failure (e.g. 1062 = duplicate entry).
int db_exec(const string& sql, const vector<string>& args, int* err_code = nullptr);
// A query without user input: the first value of the first row.
bool db_scalar(const string& sql, string& out, int* conn_id = nullptr);
// *_on: use a connection the caller already holds (the caller does get_conn/put_conn), so all queries of one request
// run on one connection. Otherwise like db_select/db_exec.
bool db_select_on(MYSQL* m, const string& sql, const vector<string>& args, vector<vector<string>>& rows,
                  int* err_code = nullptr);   // err_code: MySQL error number on failure (e.g. 1213 deadlock).
int db_exec_on(MYSQL* m, const string& sql, const vector<string>& args, int* err_code = nullptr);
// Cart (all use the given connection m; cart_id always comes from the session user).
int find_active_cart(MYSQL* m, int user_id);        // ACTIVE cart id; 0 if none or error (creates nothing).
int get_active_cart(MYSQL* m, int user_id);         // ACTIVE cart, created if missing (handles a 1062 race); 0 on error.
bool cart_add(MYSQL* m, int cart_id, const string& product_id, const string& qty);   // Atomic add, capped at MAX_CART_QTY.
int cart_set_qty(MYSQL* m, int cart_id, const string& product_id, const string& qty); // qty "0" removes; returns rows matched, -1 on error.
int cart_remove(MYSQL* m, int cart_id, const string& product_id);                     // Rows removed, -1 on error.
bool cart_items(MYSQL* m, int cart_id, vector<vector<string>>& rows);   // product_id, name, price, qty, stock.
int cart_count(MYSQL* m, int user_id);              // Total items in the cart, -1 on error.
// Checkout: one DB transaction on one pooled connection (retried on deadlock); the cart is the session user's.
// address_id is checked inside the transaction (same user, active, else 404). pay: online = PAID in the same transaction; a decline rolls back.
CheckoutResult do_checkout(int user_id, int address_id, const PaymentInfo& pay = PaymentInfo());
// Shipping addresses (always by the session's user_id; someone else's address is never found).
string address_check(Address& a);   // Trims and checks: "" = OK, otherwise a message for the user.
AddressResult address_save(int user_id, const Address& a, bool is_new);   // New (max MAX_ADDRESSES) or edit; one transaction.
AddressResult address_delete(int user_id, int address_id);                // Sets is_active = 0 (never a DELETE).
AddressResult address_set_default(int user_id, int address_id);           // Clears the old default and sets the new one: one transaction.

// Order status change (admin: ship/deliver/cancel; customer: cancel own PLACED/PACKED order). One DB transaction.
struct OrderResult {
    int status = 500;               // 200 done, 400 path not allowed, 404 order not found (or someone else's),
                                    // 409 someone changed it meanwhile, 503 deadlock/timeout again and again, 500 other error.
    string message = "Database error";
    string from;                    // The old status (for logs).
};
// expected_from: the status the page showed ("" = no check). as_admin = false: only the user's own PLACED or PACKED order, only to CANCELLED.
OrderResult order_change(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from);
// Like order_change, but also writes "changed_by" in the history: "SYSTEM" (delivery thread, user_id 0, as_admin true), "ADMIN", "CUSTOMER".
OrderResult order_change_by(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                            const string& changed_by);
// One step forward, or CANCELLED any time before DELIVERED: PLACED->PACKED->SHIPPED->OUT_FOR_DELIVERY->DELIVERED.
bool order_transition_ok(const string& from, const string& to);
string order_next_status(const string& status);                   // Next step ("" = nothing after DELIVERED, CANCELLED or an unknown status).
int order_eta_seconds(const string& status);                      // Average time (seconds) of the steps AFTER the next one (for "Arriving by").
int deliver_due_orders();           // Moves orders whose next_step_at has come one step (max 50). Returns how many moved, -1 on error.

// Role change (admin panel), one DB transaction: no changing your own role, the last ADMIN is never demoted.
struct RoleResult {
    int status = 500;               // 200 done, 400 against the rules, 403 no longer an admin, 404 no such user, 503 busy, 500 error.
    string code = "db_error";       // Flash code on 400/403/404: own_role, last_admin, same_role, no_user, not_admin, bad_request.
    string from;                    // The old role (for logs).
};

RoleResult role_change(int admin_id, int user_id, const string& new_role);
// Changes the password only if the old one is right; also ends all the user's other sessions (end_reason 'LOGOUT'),
// keeping keep_session. All in one transaction.
PasswordResult password_change(int user_id, const string& current, const string& new_pw, const string& keep_session);
// One row in admin_actions per admin change (for changes outside a transaction).
void admin_audit(int admin_id, const string& action, const string& details);
// Many request_logs rows in one multi-row INSERT. Returns rows added, -1 on error.
int db_insert_request_logs(const vector<LogEntry>& rows);
int db_free_connections();          // Free pool connections (for /metrics).
void db_thread_start();             // Call when a worker thread starts (mysql_thread_init).
void db_thread_end();               // Call before a worker thread ends (mysql_thread_end).

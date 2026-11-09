// stale_conn_check.cpp - tests open_conn() and the mysql_ping in get_conn() of src/db.cpp against the throwaway test MySQL
// (127.0.0.1:3399, root, no password). src/db.cpp is #included so the pool arrays and open_conn() are reachable; the login
// settings are filled in here, so init_db() and db.conf are never used.
// usage: stale_conn_check <mode>     healthy | kill | timeout | down      (tests/test_stale_conn.py runs them)
#include "../src/db.cpp"
#include <iostream>

// ---- stubs for what db.cpp expects from other files ----
static mutex out_lock;
static int lost_lines = 0;                  // how many "conn N lost ... reconnecting" lines get_conn() logged
void log_tag(const string& tag, const string& msg) {
    lock_guard<mutex> g(out_lock);
    if (msg.find("lost") != string::npos) lost_lines++;
    cerr << "[" << tag << "] " << msg << endl;
}
int thread_id() { return 0; }
string thread_name() { return "TEST"; }
string trim(const string& s) { return s; }
void log_verbose(const string& tag, const string& msg) { (void)tag; (void)msg; }
void metric_checkout(bool) {}

static string ids() {
    string out;
    for (int i = 0; i < DB_POOL; i++) out += (i ? "," : "") + to_string(pool[i] ? mysql_thread_id(pool[i]) : 0);
    return out;
}

// takes ALL pooled connections at once (so every one of them goes through get_conn's ping), runs a query on each, gives them back.
// Then 4 more single queries. Returns how many queries worked (2 * DB_POOL when everything is fine)
static int run_queries(int n) {
    (void)n;
    int ok = 0;
    MYSQL* held[DB_POOL];
    int hid[DB_POOL];
    for (int i = 0; i < DB_POOL; i++) held[i] = get_conn(hid[i]);
    for (int i = 0; i < DB_POOL; i++) {
        vector<vector<string>> rows;
        if (held[i] != nullptr && db_select_on(held[i], "SELECT 1", {}, rows) && rows.size() == 1 && rows[0][0] == "1") ok++;
    }
    for (int i = 0; i < DB_POOL; i++) put_conn(hid[i]);
    for (int i = 0; i < DB_POOL; i++) {
        vector<vector<string>> rows;
        if (db_select("SELECT 1", {}, rows) && rows.size() == 1 && rows[0][0] == "1") ok++;
    }
    return ok;
}

int main(int argc, char** argv) {
    string mode = argc > 1 ? argv[1] : "healthy";
    db_host = "127.0.0.1"; db_user = "root"; db_pass = ""; db_name = "ecommerce_db"; db_port = 3399;
    mysql_library_init(0, nullptr, nullptr);

    for (int i = 0; i < DB_POOL; i++) {         // same as init_db(): the pool is filled with open_conn()
        pool[i] = open_conn();
        if (pool[i] == nullptr) { cerr << "cannot open the pool" << endl; return 2; }
        free_ids[free_count++] = i + 1;
    }
    free_sem.release(DB_POOL);
    MYSQL* admin = open_conn();                 // a separate connection that plays "MySQL closed your connection"
    string before = ids();

    if (mode == "kill") {                       // MySQL drops every pooled connection (like a restart or an admin KILL)
        for (int i = 0; i < DB_POOL; i++) mysql_query(admin, ("KILL " + to_string(mysql_thread_id(pool[i]))).c_str());
    } else if (mode == "timeout") {             // idle connections time out: new connections get wait_timeout = 2 seconds
        mysql_query(admin, "SET GLOBAL wait_timeout = 2");
        for (int i = 0; i < DB_POOL; i++) {
            mysql_close(pool[i]);
            pool[i] = open_conn();
        }
        before = ids();
        this_thread::sleep_for(chrono::seconds(4));
    } else if (mode == "down") {                // every connection dies AND MySQL cannot be reached for a moment
        for (int i = 0; i < DB_POOL; i++) mysql_query(admin, ("KILL " + to_string(mysql_thread_id(pool[i]))).c_str());
        db_port = 3398;                         // nobody listens here: open_conn() fails
        vector<vector<string>> rows;
        bool worked = db_select("SELECT 1", {}, rows);
        cout << "DOWN_QUERY_WORKED " << (worked ? 1 : 0) << " free_after=" << db_free_connections() << endl;   // must not crash, and must give the connection back
        db_port = 3399;                         // MySQL is back
    }

    int ok = run_queries(2 * DB_POOL);
    cout << "QUERIES_OK " << ok << " of " << 2 * DB_POOL << endl;
    cout << "LOST_LINES " << lost_lines << endl;
    cout << "FREE " << db_free_connections() << " of " << DB_POOL << endl;
    cout << "IDS_CHANGED " << (before != ids() ? 1 : 0) << endl;
    return 0;
}

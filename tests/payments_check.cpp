// payments_check.cpp - runs the REAL do_checkout() and order_change_by() from src/db.cpp against the throwaway test MySQL
// (127.0.0.1:3399, root, no password). src/db.cpp is #included so the pool arrays are reachable: we fill the pool
// with our own connections, so init_db() and db.conf are never used.
// Needs (tests/test_payments.py makes them): user 1, product 1 with stock 50, address 1 of user 1. Prints "KEY value" lines.
#include "../src/db.cpp"
#include <iostream>

// ---- stubs for what db.cpp expects from other files ----
static mutex out_lock;
void log_tag(const string& tag, const string& msg) {
    lock_guard<mutex> g(out_lock);
    cerr << "[" << tag << "] " << msg << endl;
}
int thread_id() { return 0; }
string thread_name() { return "TEST"; }
string trim(const string& s) { return s; }
void log_verbose(const string& tag, const string& msg) { log_tag(tag, msg); }
void metric_checkout(bool) {}

static void show(const string& key, const string& value) { cout << key << " " << value << endl; }

// scalar value of a query (no user input in these queries)
static string one(const string& sql) {
    string out;
    db_scalar(sql, out);
    return out;
}

// a fresh ACTIVE cart for user 1 with 2 x product 1
static void seed_cart() {
    db_exec("INSERT INTO cart (user_id) VALUES (1)", {});
    db_exec("INSERT INTO cart_items (cart_id, product_id, quantity) SELECT cart_id, 1, 2 FROM cart WHERE user_id = 1 AND status = 'ACTIVE'", {});
}

static PaymentInfo online(const string& method, const string& detail, const string& txn) {
    PaymentInfo p;
    p.method = method;
    p.detail = detail;
    p.txn_ref = txn;
    return p;
}

// PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED, one step at a time, like the delivery thread
static void deliver(int order) {
    const char* steps[] = {"PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
    string from = "PLACED";
    for (const char* to : steps) {
        OrderResult r = order_change_by(order, to, 0, true, from, "SYSTEM");
        if (r.status != 200) show("DELIVER_FAIL", to_string(order) + " " + to + " " + r.message);
        from = to;
    }
}

int main() {
    mysql_library_init(0, nullptr, nullptr);
    for (int i = 0; i < DB_POOL; i++) {
        MYSQL* m = mysql_init(nullptr);
        if (!mysql_real_connect(m, "127.0.0.1", "root", "", "ecommerce_db", 3399, nullptr, CLIENT_FOUND_ROWS)) {
            cerr << "connect failed: " << mysql_error(m) << endl;
            return 2;
        }
        pool[i] = m;
        free_ids[free_count++] = i + 1;
    }
    free_sem.release(DB_POOL);

    // 1. the bank declines: ROLLBACK, nothing is left behind (no order, no payment, stock back, cart still ACTIVE)
    seed_cart();
    string stock0 = one("SELECT stock FROM products WHERE product_id = 1");
    PaymentInfo dec = online("UPI", "ann@okbank", "TXN0000");
    dec.declined = true;
    CheckoutResult r = do_checkout(1, 1, dec);
    show("DECLINED", to_string(r.status) + " order=" + to_string(r.order_id) + " " + r.message);
    show("DECLINED_LEFTOVERS", "orders=" + one("SELECT COUNT(*) FROM orders") + " payments=" + one("SELECT COUNT(*) FROM payments") +
                               " order_addresses=" + one("SELECT COUNT(*) FROM order_addresses") + " stock_same=" +
                               (one("SELECT stock FROM products WHERE product_id = 1") == stock0 ? "1" : "0") +
                               " cart=" + one("SELECT status FROM cart WHERE user_id = 1 ORDER BY cart_id DESC LIMIT 1"));

    // 2. COD (default argument = what the checkout page sends for now): PENDING, no paid_at, no detail, no txn
    r = do_checkout(1, 1);
    show("COD", to_string(r.status) + " order=" + to_string(r.order_id));
    int cod = r.order_id;
    show("COD_PAY", one("SELECT CONCAT(status, '/', IFNULL(paid_at, 'NULL')) FROM payments WHERE order_id = " + to_string(cod)));   // before delivery

    // 3. online methods: PAID at once, paid_at, detail and txn_ref, in the same transaction as the order
    seed_cart();
    r = do_checkout(1, 1, online("UPI", "ann@okbank", "TXN0001"));
    show("UPI", to_string(r.status) + " order=" + to_string(r.order_id));
    int upi = r.order_id;
    seed_cart();
    r = do_checkout(1, 1, online("CARD", "VISA **** 1111", "TXN0002"));
    show("CARD", to_string(r.status) + " order=" + to_string(r.order_id));
    int card = r.order_id;
    seed_cart();
    r = do_checkout(1, 1, online("NETBANKING", "HDFC Bank", "TXN0003"));
    show("NB", to_string(r.status) + " order=" + to_string(r.order_id));
    int nb = r.order_id;
    seed_cart();
    r = do_checkout(1, 1);
    show("COD2", to_string(r.status) + " order=" + to_string(r.order_id));
    int cod2 = r.order_id;

    // 4. DELIVERED: only the PENDING (COD) payment becomes PAID. The online payment keeps its real paid_at
    db_exec("UPDATE payments SET paid_at = '2020-01-01 00:00:00' WHERE order_id = ?", {to_string(upi)});
    deliver(cod);
    deliver(upi);

    // 5. CANCELLED: online -> REFUNDED, COD -> CANCELLED (stock goes back, like before)
    OrderResult c1 = order_change(card, "CANCELLED", 1, false, "");
    OrderResult c2 = order_change(cod2, "CANCELLED", 1, false, "");
    show("CANCEL", to_string(c1.status) + " " + to_string(c2.status));

    show("IDS", to_string(cod) + " " + to_string(upi) + " " + to_string(card) + " " + to_string(nb) + " " + to_string(cod2));
    return 0;
}

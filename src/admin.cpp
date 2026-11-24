
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>      // BCryptGenRandom: the OS secure random number generator.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "common.h"
#include "pages.h"
using namespace std;

// ---- /metrics ----

static string num2(double v) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// 3725 -> "1h 2m 5s".
static string uptime_text(long long s) {
    return to_string(s / 3600) + "h " + to_string((s % 3600) / 60) + "m " + to_string(s % 60) + "s";
}

static string metric_row(const string& name, const string& value) {
    return render("metric_row", {{"name", name}, {"value", value}});
}

// /metrics and /metrics.json show internal numbers: only from this computer (127.0.0.1), directly (not through a proxy or tunnel).
// The server listens on every network card (INADDR_ANY), so this check is needed: another computer gets 403.
int metrics_page(SOCKET client, const Request& req, bool json) {
    // X-Forwarded-For / X-Forwarded-Host mean the request came through a tunnel or proxy (e.g. ngrok): the proxy runs on this machine,
    // so the socket IP looks like 127.0.0.1 but the real client is outside. Also 403.
    bool proxied = req.headers.count("x-forwarded-for") > 0 || req.headers.count("x-forwarded-host") > 0;
    if (req.client_ip != "127.0.0.1" || proxied)
        return send_page(client, req, 403, "Forbidden",
                         error_content("403 Forbidden", render("detail_text", {{"text", "Metrics are only available directly from 127.0.0.1 (not through a proxy or tunnel)."}})));
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    Metrics m = metrics_snapshot();

    if (json) {
        string j = "{\"uptime_seconds\":" + to_string(m.uptime_s) +
                   ",\"total_requests\":" + to_string(m.total) +
                   ",\"status\":{\"2xx\":" + to_string(m.c2xx) + ",\"3xx\":" + to_string(m.c3xx) +
                   ",\"4xx\":" + to_string(m.c4xx) + ",\"5xx\":" + to_string(m.c5xx) + "}" +
                   ",\"rejected_503\":" + to_string(m.rejected_503) +
                   ",\"timeouts_408\":" + to_string(m.timeouts_408) +
                   ",\"busy_workers\":" + to_string(m.busy_workers) + ",\"workers\":" + to_string(WORKERS) +
                   ",\"queue_length\":" + to_string(m.queue_length) + ",\"queue_size\":" + to_string(QUEUE_SIZE) +
                   ",\"free_db_connections\":" + to_string(m.free_db_conns) + ",\"db_pool\":" + to_string(DB_POOL) +
                   ",\"latency_ms\":{\"avg\":" + num2(m.avg_ms) + ",\"p95\":" + num2(m.p95_ms) +
                   ",\"samples\":" + to_string(m.latency_samples) + "}" +
                   ",\"requests_per_sec\":" + num2(m.rps) +
                   ",\"checkout\":{\"commit\":" + to_string(m.checkout_commit) +
                   ",\"rollback\":" + to_string(m.checkout_rollback) + "}" +
                   ",\"dropped_logs\":" + to_string(m.dropped_logs) + "}";
        send_response(client, 200, "application/json", j, "Cache-Control: no-store\r\n");
        return 200;
    }

    // Simple table; auto refresh with <meta http-equiv="refresh"> (no JavaScript).
    string h;
    h += metric_row("Uptime", uptime_text(m.uptime_s));
    h += metric_row("Total requests", to_string(m.total));
    h += metric_row("2xx / 3xx / 4xx / 5xx", to_string(m.c2xx) + " / " + to_string(m.c3xx) + " / " + to_string(m.c4xx) + " / " +
                                       to_string(m.c5xx));
    h += metric_row("503 rejected (queue full)", to_string(m.rejected_503));
    h += metric_row("408 timeouts", to_string(m.timeouts_408));
    h += metric_row("Busy workers now", to_string(m.busy_workers) + " / " + to_string(WORKERS));
    h += metric_row("Queue length now", to_string(m.queue_length) + " / " + to_string(QUEUE_SIZE));
    h += metric_row("Free DB connections now", to_string(m.free_db_conns) + " / " + to_string(DB_POOL));
    h += metric_row("Latency avg (last " + to_string(m.latency_samples) + " requests)", num2(m.avg_ms) + " ms");
    h += metric_row("Latency p95 (last " + to_string(m.latency_samples) + " requests)", num2(m.p95_ms) + " ms");
    h += metric_row("Requests/sec (last " + to_string(RPS_SECONDS) + " s)", num2(m.rps));
    h += metric_row("Checkout COMMIT / ROLLBACK", to_string(m.checkout_commit) + " / " + to_string(m.checkout_rollback));
    h += metric_row("Dropped logs", to_string(m.dropped_logs));
    // interactive = false: no JavaScript on this page (the refresh is a meta tag).
    send_response(client, 200, HTML, page(req, "Metrics", render("metrics", {{"rows", h}}),
                                          render("meta_refresh", {{"seconds", "2"}}), "", false, false),
                  "Cache-Control: no-store\r\n");
    return 200;
}

// ---- admin panel (A1: dashboard, A2: products + categories) ----
// Every /admin/* route goes through require_admin first (one place), so no route can skip it. Changes are POST only, then 302 back with ?ok= or ?err=.

// 0 = admin, carry on. Otherwise the reply was already sent and that status is returned:
// not logged in -> 302 /login, logged in but not ADMIN -> 403.
static int require_admin(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.role != "ADMIN") {
        log_tag("PAGE", "user " + to_string(req.user_id) + " tried " + req.path + " (not an admin)");
        return error_page(client, req, 403, "403 Forbidden");
    }
    return 0;
}

// Flash messages: the URL carries only a short code (?ok=restocked); the text is fixed here, so no text from the URL ever reaches the page.
struct AdminMsg {
    const char* code;
    const char* text;
};
static const AdminMsg ADMIN_OK[] = {
    {"product_added", "Product added. Its stock starts at 0: use Restock to add stock."},
    {"product_saved", "Product saved."},
    {"product_hidden", "Product hidden from the store."},
    {"product_shown", "Product is visible in the store again."},
    {"restocked", "Stock updated."},
    {"cat_added", "Category added."},
    {"cat_renamed", "Category renamed."},
    {"cat_deleted", "Category deleted."},
    {"order_packed", "Order marked as packed."},
    {"order_shipped", "Order marked as shipped."},
    {"order_ofd", "Order marked as out for delivery."},
    {"order_delivered", "Order delivered. The payment is now PAID."},
    {"order_cancelled", "Order cancelled. Its items were added back to stock."},
    {"role_changed", "Role changed. It applies from that user's next request."},
    {"session_ended", "Session ended. That user is logged out."},
    {"sessions_ended", "All active sessions of that user were ended."}};
static const AdminMsg ADMIN_ERR[] = {
    {"stock_below", "Stock cannot go below 0."},
    {"bad_delta", "Stock change must be a whole number like +5 or -2 (not 0)."},
    {"no_product", "Product not found."},
    {"bad_request", "Bad request."},
    {"cat_exists", "That category name already exists."},
    {"cat_has_products", "This category still has products. Move its products to another category first (hiding them is not enough)."},
    {"cat_missing", "Category not found."},
    {"bad_name", "Category name must be 1 to 50 characters."},
    {"own_role", "You cannot change your own role."},
    {"last_admin", "The last admin cannot be demoted: make someone else an admin first."},
    {"same_role", "That user already has this role."},
    {"no_user", "User not found."},
    {"not_admin", "You are no longer an admin."},
    {"session_gone", "That session is already ended or does not exist."},
    {"own_session", "That is your current session: use Sign out instead."},
    {"no_sessions", "That user has no active sessions."},
    {"own_user", "Use Sign out to end your own sessions."}};

static string admin_notice(const Request& req) {
    string ok = param(req, "ok");
    string err = param(req, "err");
    for (const AdminMsg& m : ADMIN_ERR)
        if (err == m.code) return flash_text("err", m.text);
    for (const AdminMsg& m : ADMIN_OK)
        if (ok == m.code) return flash_text("ok", m.text);
    return "";
}

// One sidebar link; the active one is indigo.
static string admin_link(const string& active, const string& key, const string& href, const string& icon, const string& label) {
    return render("admin_link", {{"on_class", active == key ? " on" : ""}, {"href", href}, {"icon", icon}, {"label", label}});
}

// Full admin page: its own sidebar, not the store navbar. active = which link is highlighted.
static int send_admin(SOCKET client, const Request& req, int status, const string& title, const string& active,
                      const string& content, const string& head_extra = "") {
    string side = admin_link(active, "dashboard", "/admin", "📊", "Dashboard") +
                  admin_link(active, "products", "/admin/products", "📦", "Products") +
                  admin_link(active, "categories", "/admin/categories", "🗂️", "Categories") +
                  admin_link(active, "orders", "/admin/orders", "🧾", "Orders") +
                  admin_link(active, "users", "/admin/users", "👥", "Users") +
                  admin_link(active, "sessions", "/admin/sessions", "🔐", "Sessions") +
                  admin_link(active, "logs", "/admin/logs", "📜", "Logs");
    send_response(client, status, HTML,
                  render("admin_layout", {{"title", title}, {"sidebar", side}, {"name", req.user_name},
                                          {"flash", admin_notice(req)}, {"content", content}, {"head_extra", head_extra}}),
                  with_cookie_fix(req, "Cache-Control: no-store\r\n"));
    return status;
}

// An admin page that reports "not found / invalid" (with a status).
static int admin_problem(SOCKET client, const Request& req, int status, const string& title, const string& message) {
    return send_admin(client, req, status, title, "", render("admin_message", {{"title", title}, {"message", message}}));
}

// This page's own path + query (without ok/err): forms send it as "return" so we come back here after the action.
static string admin_self(const Request& req) {
    return strip_param(strip_param(req.target, "ok"), "err");
}

// "return" may only be one of our /admin paths (no open redirect), otherwise the fallback.
static string admin_back(const Request& req, const string& fallback) {
    string r = field(req, "return");
    bool in_admin = r == "/admin" || r.rfind("/admin/", 0) == 0 || r.rfind("/admin?", 0) == 0;
    if (!in_admin || safe_return(r) != r) return fallback;
    return strip_param(strip_param(r, "ok"), "err");
}

// 302 back, with a green (ok) or red (err) message.
static int admin_redirect(SOCKET client, const Request& req, const string& fallback, const string& ok, const string& err) {
    string back = admin_back(req, fallback);
    back += (back.find('?') == string::npos ? "?" : "&") + string(err.empty() ? "ok=" : "err=") + (err.empty() ? ok : err);
    return redirect(client, req, back);
}

// Audit row of an admin change (admin_audit in db.cpp), written after the change. Order status and role rows are written inside their transaction.
static void audit(const Request& req, const string& action, const string& details) {
    admin_audit(req.user_id, action, details);
}

// A GET on a POST route (or the reverse): 405.
static int admin_method(SOCKET client, const Request& req, const string& want) {
    if (req.method == want) return 0;
    return method_not_allowed(client, req, want);
}

// ---- admin: small helpers (validation) ----

// Counts characters, not bytes: UTF-8 continuation bytes (10xxxxxx) are skipped. VARCHAR(100) counts characters.
static size_t utf8_len(const string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

// "499", "499.5", "499.50" -> paise. Digits and one '.', at most 2 decimals, 0 < price <= 1,000,000.
static bool parse_price(const string& s, long long& cents) {
    size_t dot = s.find('.');
    string whole = s.substr(0, dot);
    string frac = dot == string::npos ? "" : s.substr(dot + 1);
    if (whole.empty() || whole.size() > 7 || frac.size() > 2) return false;
    if (dot != string::npos && frac.empty()) return false;
    for (char c : whole + frac)
        if (c < '0' || c > '9') return false;
    while (frac.size() < 2) frac += '0';
    cents = atoll(whole.c_str()) * 100 + atoll(frac.c_str());
    return cents > 0 && cents <= 100000000LL;
}

// Paise -> "1234.50" (for the DB DECIMAL).
static string cents_plain(long long c) {
    string p = to_string(c % 100);
    return to_string(c / 100) + "." + (p.size() < 2 ? "0" : "") + p;
}

// "+5", "-2", "5" -> "5" / "-2". A whole number, not 0, at most 100000.
static bool parse_delta(const string& text, string& out) {
    string s = trim(text);
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = (s[0] == '-');
        s.erase(0, 1);
    }
    if (s.empty() || s.size() > 6) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    int v = atoi(s.c_str());
    if (v == 0 || v > 100000) return false;
    out = (neg ? "-" : "") + to_string(v);
    return true;
}

static string admin_card(const string& label, const string& value, const string& tone = "") {
    return render("admin_card", {{"label", label}, {"value", value}, {"tone", tone}});
}

// Small "+5 / -2" restock form (dashboard and products table). back = where to return after the action.
static string restock_form(const string& id, const string& name, const string& back) {
    return render("admin_restock_form", {{"id", id}, {"name", name}, {"back", back}});
}

// ---- admin: dashboard ----

// GET /admin: all queries on ONE pooled connection (get_conn first, put_conn before the page is built).
static int admin_dashboard_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return error_page(client, req, 500, "Database error");

    const string low = to_string(LOW_STOCK);
    vector<vector<string>> stats, recent, by_status, low_rows;
    bool ok =
        db_select_on(m, "SELECT COALESCE((SELECT SUM(total) FROM orders WHERE status <> 'CANCELLED'), 0), "
                        "(SELECT COUNT(*) FROM orders), (SELECT COUNT(*) FROM orders WHERE status = 'PLACED'), "
                        "(SELECT COUNT(*) FROM users), (SELECT COUNT(*) FROM products), "
                        "(SELECT COUNT(*) FROM products WHERE stock <= ? AND is_active = TRUE), "
                        "(SELECT COUNT(*) FROM user_sessions WHERE is_active = 1 AND expires_at > NOW())",
                     {low}, stats) &&
        db_select_on(m, "SELECT o.order_id, u.name, o.created_at, o.total, o.status FROM orders o "
                        "JOIN users u ON u.user_id = o.user_id ORDER BY o.order_id DESC LIMIT 10", {}, recent) &&
        db_select_on(m, "SELECT status, COUNT(*) FROM orders GROUP BY status ORDER BY COUNT(*) DESC, status", {}, by_status) &&
        db_select_on(m, "SELECT product_id, name, stock FROM products WHERE stock <= ? AND is_active = TRUE "
                        "ORDER BY stock, name LIMIT 20", {low}, low_rows);
    put_conn(id);                       // The connection is not needed any more: building the page uses no DB.
    if (!ok || stats.size() != 1 || !cols_ok(stats, 7) || !cols_ok(by_status, 2) || !cols_ok(low_rows, 3) ||
        (!recent.empty() && !cols_ok(recent, 5)))
        return error_page(client, req, 500, "Database error");

    vector<string>& s = stats[0];
    string cards = admin_card("Revenue (excl. cancelled)", rupees_text(s[0])) + admin_card("Total orders", s[1]) +
                   admin_card("Orders waiting (PLACED)", s[2], s[2] != "0" ? "warn" : "") + admin_card("Users", s[3]) +
                   admin_card("Products", s[4]) +
                   admin_card("Low stock (<= " + low + ")", s[5], s[5] != "0" ? "err" : "") + admin_card("Active sessions", s[6]);

    string orders;
    for (auto& r : recent)
        orders += render("admin_order_row", {{"id", r[0]}, {"customer", r[1]}, {"date", r[2]}, {"total", rupees_text(r[3])},
                                             {"status", status_label(r[4])}, {"status_class", status_class(r[4])}});
    if (recent.empty()) orders = render("admin_none_row", {{"message", "No orders yet."}});
    string statuses;
    for (auto& r : by_status)
        statuses += render("admin_status_count", {{"status", status_label(r[0])}, {"status_class", status_class(r[0])}, {"count", r[1]}});

    string low_html;
    if (low_rows.empty()) {
        low_html = render("admin_none", {{"message", "Every product has more than " + low + " in stock."}});
    } else {
        string rows;
        for (auto& r : low_rows)
            rows += render("admin_low_row", {{"name", r[1]}, {"stock", r[2]}, {"tone", r[2] == "0" ? "pill-cancelled" : "pill-placed"},
                                             {"restock", restock_form(r[0], r[1], "/admin")}});
        low_html = render("admin_low", {{"rows", rows}});
    }
    return send_admin(client, req, 200, "Dashboard", "dashboard",
                      render("admin_dashboard", {{"cards", cards}, {"orders", orders}, {"statuses", statuses}, {"low", low_html}}));
}

// ---- admin: products ----

// GET /admin/products?q=&cat=&hidden=1
static int admin_products_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string q = trim(param(req, "q"));
    string cat = param(req, "cat");
    bool show_hidden = param(req, "hidden") == "1";
    if (!cat.empty() && !is_number(cat)) return admin_problem(client, req, 400, "Products", "Bad category filter.");

    // SQL pieces are fixed; user data only in args (?).
    string sql = "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.is_active FROM products p "
                 "JOIN categories c ON c.category_id = p.category_id WHERE 1=1";
    vector<string> args;
    if (!show_hidden) sql += " AND p.is_active = TRUE";
    if (!cat.empty()) {
        sql += " AND p.category_id = ?";
        args.push_back(cat);
    }
    if (!q.empty()) {
        sql += " AND p.name LIKE ? ESCAPE '!'";
        args.push_back("%" + escape_like(q) + "%");
    }
    sql += " ORDER BY p.product_id LIMIT 300";
    vector<vector<string>> prods;
    if (!db_select(sql, args, prods) || !cols_ok(prods, 6)) return error_page(client, req, 500, "Database error");

    string back = admin_self(req);
    string rows;
    for (auto& p : prods) {
        bool active = (p[5] != "0");
        rows += render("admin_product_row", {{"row_class", active ? "" : "dim"}, {"id", p[0]}, {"name", p[1]}, {"category", p[2]},
                                             {"price", rupees_text(p[3])}, {"stock", p[4]},
                                             {"state", active ? "active" : "hidden"}, {"state_label", active ? "Active" : "Hidden"},
                                             {"new_active", active ? "0" : "1"}, {"toggle_label", active ? "Hide" : "Show"},
                                             {"restock", restock_form(p[0], p[1], back)}, {"back", back}});
    }
    if (prods.empty()) rows = render("admin_none_row", {{"message", "No products match."}});

    string options = render("search_option", {{"value", ""}, {"label", "All categories"}, {"selected", cat.empty() ? "selected" : ""}});
    for (auto& c : get_categories(true))
        options += render("search_option", {{"value", c[0]}, {"label", c[1]}, {"selected", c[0] == cat ? "selected" : ""}});
    string count = to_string(prods.size()) + (prods.size() == 1 ? " product" : " products") +
                   (prods.size() == 300 ? " (showing the first 300: use the filters)" : "");
    return send_admin(client, req, 200, "Products", "products",
                      render("admin_products", {{"q", q}, {"options", options}, {"hidden_checked", show_hidden ? "checked" : ""},
                                                {"count", count}, {"rows", rows}}));
}

// Add/edit form fields (as the user typed them).
struct ProductForm {
    string id, name, description, price, category;
};

static ProductForm read_product_form(const Request& req) {
    ProductForm f;
    f.id = trim(field(req, "product_id"));
    f.name = trim(field(req, "name"));
    f.description = trim(field(req, "description"));
    f.price = trim(field(req, "price"));
    f.category = trim(field(req, "category_id"));
    return f;
}

// "" = all fine (the price in cents too), otherwise a message for the user.
static string check_product_form(const ProductForm& f, long long& cents) {
    size_t n = utf8_len(f.name);
    if (n < 1 || n > 100) return "Name must be 1 to 100 characters.";
    if (utf8_len(f.description) > 255) return "Description can be at most 255 characters.";
    if (!parse_price(f.price, cents)) return "Price must be more than 0 and at most 1000000 (at most 2 decimals).";
    if (!is_number(f.category)) return "Please choose a category.";
    vector<vector<string>> rows;
    if (!db_select("SELECT category_id FROM categories WHERE category_id = ?", {f.category}, rows)) return "Database error";
    if (rows.empty()) return "That category does not exist.";
    return "";
}

static int product_form_page(SOCKET client, const Request& req, int status, bool is_new, const ProductForm& f, const string& error) {
    string options;
    for (auto& c : get_categories(true))
        options += render("search_option", {{"value", c[0]}, {"label", c[1]}, {"selected", c[0] == f.category ? "selected" : ""}});
    string heading = is_new ? "New product" : "Edit product";
    string content = render("admin_product_form", {
        {"heading", heading}, {"action", is_new ? "/admin/products/new" : "/admin/products/edit"},
        {"error", error.empty() ? "" : flash_text("err", error)},
        {"id_field", f.id.empty() ? "" : render("hidden_input", {{"name", "product_id"}, {"value", f.id}})},
        {"name", f.name}, {"description", f.description}, {"price", f.price}, {"options", options},
        {"note", is_new ? "Stock starts at 0. Use Restock after saving." : "Stock is not changed here: use Restock on the products list."},
        {"submit", is_new ? "Add product" : "Save changes"}});
    return send_admin(client, req, status, heading, "products", content);
}

// GET /admin/products/new (form) and POST (create).
static int admin_product_new_page(SOCKET client, const Request& req) {
    if (req.method != "POST") {
        if (req.method != "GET") return method_not_allowed(client, req, "GET, POST");
        return product_form_page(client, req, 200, true, ProductForm(), "");
    }
    ProductForm f = read_product_form(req);
    long long cents = 0;
    string err = check_product_form(f, cents);
    if (!err.empty()) return product_form_page(client, req, err == "Database error" ? 500 : 400, true, f, err);

    int n = db_exec("INSERT INTO products (name, description, price, stock, category_id) VALUES (?, ?, ?, 0, ?)",
                    {f.name, f.description, cents_plain(cents), f.category});
    if (n < 0) return product_form_page(client, req, 500, true, f, "Database error");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " added product '" + f.name + "'");
    audit(req, "PRODUCT_ADD", "product '" + f.name + "' price " + cents_plain(cents) + " category " + f.category);
    return admin_redirect(client, req, "/admin/products", "product_added", "");
}

// GET /admin/products/edit?id=N (form) and POST (update: does not change stock).
static int admin_product_edit_page(SOCKET client, const Request& req) {
    if (req.method == "GET") {
        string id = param(req, "id");
        if (!valid_product_id(id)) return admin_problem(client, req, 400, "Edit product", "Bad product id.");
        vector<vector<string>> rows;
        if (!db_select("SELECT product_id, name, description, price, category_id FROM products WHERE product_id = ?", {id}, rows) ||
            !cols_ok(rows, 5))
            return error_page(client, req, 500, "Database error");
        if (rows.empty()) return admin_problem(client, req, 404, "Edit product", "Product not found.");
        ProductForm f;
        f.id = rows[0][0]; f.name = rows[0][1]; f.description = rows[0][2]; f.price = rows[0][3]; f.category = rows[0][4];
        return product_form_page(client, req, 200, false, f, "");
    }
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    ProductForm f = read_product_form(req);
    if (!valid_product_id(f.id)) return admin_redirect(client, req, "/admin/products", "", "bad_request");
    long long cents = 0;
    string err = check_product_form(f, cents);
    if (!err.empty()) return product_form_page(client, req, err == "Database error" ? 500 : 400, false, f, err);

    // The stock is not in this query: editing never changes stock (restock is separate and atomic).
    int n = db_exec("UPDATE products SET name = ?, description = ?, price = ?, category_id = ? WHERE product_id = ?",
                    {f.name, f.description, cents_plain(cents), f.category, f.id});
    if (n < 0) return product_form_page(client, req, 500, false, f, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/products", "", "no_product");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " edited product " + f.id);
    audit(req, "PRODUCT_EDIT", "product " + f.id + " '" + f.name + "' price " + cents_plain(cents) + " category " + f.category);
    return admin_redirect(client, req, "/admin/products", "product_saved", "");
}

// POST /admin/products/restock (product_id, delta "+5" / "-2"): ONE atomic query.
// stock = stock + ? (never = <number>) so a sale at the same moment is not lost; AND stock + ? >= 0 refuses going below zero (0 rows).
static int admin_restock_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string pid = trim(field(req, "product_id"));
    string delta;
    if (!valid_product_id(pid)) return admin_redirect(client, req, "/admin/products", "", "bad_request");
    if (!parse_delta(field(req, "delta"), delta)) return admin_redirect(client, req, "/admin/products", "", "bad_delta");

    int n = db_exec("UPDATE products SET stock = stock + CAST(? AS SIGNED) "
                    "WHERE product_id = ? AND stock + CAST(? AS SIGNED) >= 0", {delta, pid, delta});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) {                       // No such product, or the stock would go below zero.
        vector<vector<string>> rows;
        if (!db_select("SELECT 1 FROM products WHERE product_id = ?", {pid}, rows)) return error_page(client, req, 500, "Database error");
        return admin_redirect(client, req, "/admin/products", "", rows.empty() ? "no_product" : "stock_below");
    }
    log_tag("ADMIN", "user " + to_string(req.user_id) + " restock product " + pid + " by " + delta);
    audit(req, "PRODUCT_RESTOCK", "product " + pid + " by " + delta);
    return admin_redirect(client, req, "/admin/products", "restocked", "");
}

// POST /admin/products/toggle (product_id, active = 0 hide / 1 show). Products are never DELETEd: old orders point to them.
static int admin_toggle_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string pid = trim(field(req, "product_id"));
    string active = field(req, "active");
    if (!valid_product_id(pid) || (active != "0" && active != "1"))
        return admin_redirect(client, req, "/admin/products", "", "bad_request");
    int n = db_exec("UPDATE products SET is_active = ? WHERE product_id = ?", {active, pid});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/products", "", "no_product");
    log_tag("ADMIN", "user " + to_string(req.user_id) + (active == "1" ? " showed" : " hid") + " product " + pid);
    audit(req, active == "1" ? "PRODUCT_SHOW" : "PRODUCT_HIDE", "product " + pid);
    return admin_redirect(client, req, "/admin/products", active == "1" ? "product_shown" : "product_hidden", "");
}

// ---- admin: categories ----

// GET /admin/categories: each category with its product count (LEFT JOIN: an empty category shows 0).
static int admin_categories_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    vector<vector<string>> cats;
    if (!db_select("SELECT c.category_id, c.category_name, COUNT(p.product_id) FROM categories c "
                   "LEFT JOIN products p ON p.category_id = c.category_id "
                   "GROUP BY c.category_id, c.category_name ORDER BY c.category_id", {}, cats) ||
        !cols_ok(cats, 3))
        return error_page(client, req, 500, "Database error");
    string rows;
    for (auto& c : cats)
        rows += render("admin_category_row", {{"id", c[0]}, {"name", c[1]}, {"count", c[2]}});
    if (cats.empty()) rows = render("admin_none_row", {{"message", "No categories yet."}});
    return send_admin(client, req, 200, "Categories", "categories", render("admin_categories", {{"rows", rows}}));
}

// Category name: 1 to 50 characters (VARCHAR(50)); "" = invalid.
static string clean_category_name(const Request& req) {
    string name = trim(field(req, "name"));
    size_t n = utf8_len(name);
    return (n < 1 || n > 50) ? "" : name;
}

// MySQL error number -> admin message code (categories only).
static int category_result(SOCKET client, const Request& req, int n, int code, const string& ok_code,
                           const string& audit_action, const string& details) {
    if (n < 0 && code == 1062) return admin_redirect(client, req, "/admin/categories", "", "cat_exists");
    if (n < 0 && code == 1451) return admin_redirect(client, req, "/admin/categories", "", "cat_has_products");
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/categories", "", "cat_missing");
    clear_category_cache();
    log_tag("ADMIN", "user " + to_string(req.user_id) + " category change: " + ok_code);
    audit(req, audit_action, details);
    return admin_redirect(client, req, "/admin/categories", ok_code, "");
}

static int admin_category_add_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string name = clean_category_name(req);
    if (name.empty()) return admin_redirect(client, req, "/admin/categories", "", "bad_name");
    int code = 0;
    int n = db_exec("INSERT INTO categories (category_name) VALUES (?)", {name}, &code);
    return category_result(client, req, n, code, "cat_added", "CATEGORY_ADD", "category '" + name + "'");
}

static int admin_category_rename_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "category_id"));
    string name = clean_category_name(req);
    if (!is_number(id)) return admin_redirect(client, req, "/admin/categories", "", "bad_request");
    if (name.empty()) return admin_redirect(client, req, "/admin/categories", "", "bad_name");
    int code = 0;
    int n = db_exec("UPDATE categories SET category_name = ? WHERE category_id = ?", {name, id}, &code);
    return category_result(client, req, n, code, "cat_renamed", "CATEGORY_RENAME", "category " + id + " renamed to '" + name + "'");
}

// DELETE works only for an empty category: with products the FK (1451) refuses.
static int admin_category_delete_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "category_id"));
    if (!is_number(id)) return admin_redirect(client, req, "/admin/categories", "", "bad_request");
    int code = 0;
    int n = db_exec("DELETE FROM categories WHERE category_id = ?", {id}, &code);
    return category_result(client, req, n, code, "cat_deleted", "CATEGORY_DELETE", "category " + id);
}

// ---- admin: orders (A3) ----

// For putting query text into a link: only A-Z a-z 0-9 - _ . ~ stay as they are, the rest is %XX.
static string url_encode(const string& s) {
    static const char* HEX = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}

// Newer / Older links (the filters stay in base); empty if there is nothing.
static string admin_pager_html(const string& base, int pg, bool more) {
    string prev = pg > 1 ? render("admin_pager_link", {{"href", base + "page=" + to_string(pg - 1)}, {"label", "&larr; Newer"}}) : "";
    string next = more ? render("admin_pager_link", {{"href", base + "page=" + to_string(pg + 1)}, {"label", "Older &rarr;"}}) : "";
    if (prev.empty() && next.empty()) return "";
    return render("admin_pager", {{"prev", prev}, {"next", next}, {"page", to_string(pg)}});
}

// Order statuses (only those the DB CHECK allows): used to validate the filter and the status form.
static bool known_order_status(const string& s) {
    static const char* ALL[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED"};
    for (const char* a : ALL)
        if (s == a) return true;
    return false;
}

const int ORDERS_PER_PAGE = 20;

// GET /admin/orders?status=&q=&page=
static int admin_orders_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string status = param(req, "status");
    string q = trim(param(req, "q"));
    string page_text = param(req, "page");
    if (!status.empty() && !known_order_status(status)) return admin_problem(client, req, 400, "Orders", "Bad status filter.");
    int pg = 1;
    if (!page_text.empty() && !parse_range(page_text, 1, 100000, pg)) return admin_problem(client, req, 400, "Orders", "Bad page number.");

    // SQL pieces are fixed; filter values only in args. The LIMIT/OFFSET numbers are our own ints (not user text).
    string sql = "SELECT o.order_id, u.name, u.email, o.created_at, "
                 "(SELECT COALESCE(SUM(oi.quantity), 0) FROM order_items oi WHERE oi.order_id = o.order_id), "
                 "o.total, o.status, IFNULL(p.status, '-') "
                 "FROM orders o JOIN users u ON u.user_id = o.user_id LEFT JOIN payments p ON p.order_id = o.order_id WHERE 1=1";
    vector<string> args;
    if (!status.empty()) {
        sql += " AND o.status = ?";
        args.push_back(status);
    }
    if (!q.empty()) {
        sql += " AND (o.order_id = ? OR u.email LIKE ? ESCAPE '!')";
        args.push_back(is_number(q) ? q : "0");              // A number also matches an order id; order id 0 does not exist.
        args.push_back("%" + escape_like(q) + "%");
    }
    sql += " ORDER BY o.order_id DESC LIMIT " + to_string(ORDERS_PER_PAGE + 1) + " OFFSET " + to_string((pg - 1) * ORDERS_PER_PAGE);

    vector<vector<string>> found;
    if (!db_select(sql, args, found) || !cols_ok(found, 8)) return error_page(client, req, 500, "Database error");
    bool more = (int)found.size() > ORDERS_PER_PAGE;         // One extra row means there is a next page.
    if (more) found.resize(ORDERS_PER_PAGE);

    string rows;
    for (auto& r : found)
        rows += render("admin_orders_row", {{"id", r[0]}, {"name", r[1]}, {"email", r[2]}, {"date", r[3]}, {"items", r[4]},
                                            {"total", rupees_text(r[5])}, {"status", status_label(r[6])}, {"status_class", status_class(r[6])},
                                            {"pay", r[7]}, {"pay_class", status_class(r[7])}});
    if (found.empty()) rows = render("admin_none_row", {{"message", "No orders match."}});

    string options = render("search_option", {{"value", ""}, {"label", "All statuses"}, {"selected", status.empty() ? "selected" : ""}});
    for (const char* s : {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED"})
        options += render("search_option", {{"value", s}, {"label", status_label(s)}, {"selected", status == s ? "selected" : ""}});

    // Pager: the filters stay in the links.
    string base = "/admin/orders?";
    if (!status.empty()) base += "status=" + url_encode(status) + "&";
    if (!q.empty()) base += "q=" + url_encode(q) + "&";
    string pager = admin_pager_html(base, pg, more);

    string count = to_string(found.size()) + (found.size() == 1 ? " order" : " orders") + " on this page (newest first, " +
                   to_string(ORDERS_PER_PAGE) + " per page)";
    return send_admin(client, req, 200, "Orders", "orders",
                      render("admin_orders", {{"options", options}, {"q", q}, {"count", count}, {"rows", rows}, {"pager", pager}}));
}

// GET /admin/order?id=N: customer, items (price at purchase), payment, and buttons only for the allowed next statuses.
static int admin_order_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string id = param(req, "id");
    if (!valid_product_id(id)) return admin_problem(client, req, 400, "Order", "Bad order id.");

    vector<vector<string>> head, items, hist;
    int cid = 0;
    MYSQL* m = get_conn(cid);               // All three queries on one connection.
    if (m == nullptr) return error_page(client, req, 500, "Database error");
    bool ok = db_select_on(m, "SELECT o.order_id, o.status, o.total, o.created_at, u.name, u.email, IFNULL(p.method, ''), "
                              "IFNULL(p.status, ''), IFNULL(p.paid_at, ''), IFNULL(p.pay_detail, ''), IFNULL(p.txn_ref, '') "
                              "FROM orders o JOIN users u ON u.user_id = o.user_id "
                              "LEFT JOIN payments p ON p.order_id = o.order_id WHERE o.order_id = ?", {id}, head) &&
              db_select_on(m, "SELECT p.product_id, p.name, oi.quantity, oi.price FROM order_items oi "
                              "JOIN products p ON p.product_id = oi.product_id WHERE oi.order_id = ? ORDER BY p.name", {id}, items) &&
              db_select_on(m, "SELECT status, changed_at, changed_by FROM order_status_history WHERE order_id = ? ORDER BY history_id",
                           {id}, hist);
    put_conn(cid);
    if (!ok) return error_page(client, req, 500, "Database error");
    if (head.empty()) return admin_problem(client, req, 404, "Order", "Order not found.");
    if (!cols_ok(head, 11) || !cols_ok(items, 4) || !cols_ok(hist, 3)) return error_page(client, req, 500, "Database error");

    vector<string>& o = head[0];
    string rows;
    for (auto& r : items)
        rows += render("admin_order_item_row", {{"name", r[1]}, {"qty", r[2]}, {"price", rupees_text(r[3])},
                                                {"subtotal", rupees(to_cents(r[3]) * atoi(r[2].c_str()))}});
    string addr;
    if (!order_address_text(o[0], addr)) return error_page(client, req, 500, "Database error");
    string history;                         // Full history: when, which status, who changed it (SYSTEM / ADMIN / CUSTOMER).
    for (auto& h : hist)
        history += render("admin_history_row", {{"time", h[1]}, {"status_class", status_class(h[0])}, {"label", status_label(h[0])}, {"by", h[2]}});
    if (hist.empty()) history = render("admin_none_row", {{"message", "No history yet (order placed before tracking was added)."}});
    string pay = payment_html(o[6], o[9], o[7], o[8], o[10]);

    // Buttons: for each possible "to", if order_transition_ok in db.cpp says yes (the same rule the server enforces).
    struct Next {
        const char* to;
        const char* label;
        const char* tone;
    };
    static const Next NEXT[] = {{"PACKED", "Mark as packed", "btn-primary"},
                                {"SHIPPED", "Mark as shipped", "btn-primary"},
                                {"OUT_FOR_DELIVERY", "Mark as out for delivery", "btn-primary"},
                                {"DELIVERED", "Mark as delivered (payment becomes PAID)", "btn-primary"},
                                {"CANCELLED", "Cancel order (items go back to stock)", "btn-danger"}};
    string buttons;
    for (const Next& n : NEXT)
        if (order_transition_ok(o[1], n.to))
            buttons += render("admin_order_action", {{"id", o[0]}, {"to", n.to}, {"from", o[1]}, {"label", n.label}, {"tone", n.tone}});
    string actions = buttons.empty() ? render("admin_none", {{"message", "This order is final: " + status_label(o[1]) + " orders cannot be changed."}})
                                     : render("admin_order_buttons", {{"buttons", buttons}});

    return send_admin(client, req, 200, "Order #" + o[0], "orders",
                      render("admin_order", {{"id", o[0]}, {"status", status_label(o[1])}, {"status_class", status_class(o[1])}, {"total", rupees_text(o[2])},
                                             {"placed", o[3]}, {"name", o[4]}, {"email", o[5]}, {"payment", pay}, {"rows", rows},
                                             {"history", history}, {"address", addr}, {"actions", actions}}));
}

// POST /admin/order/status (order_id, to, from = the status the admin saw). The real work is order_change in db.cpp.
static int admin_order_status_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "order_id"));
    string to = trim(field(req, "to"));
    string from = trim(field(req, "from"));
    if (!valid_product_id(id) || !known_order_status(to) || to == "PLACED" || (!from.empty() && !known_order_status(from)))
        return admin_redirect(client, req, "/admin/orders", "", "bad_request");

    OrderResult r = order_change(atoi(id.c_str()), to, req.user_id, true, from);
    string back = "/admin/order?id=" + id;
    if (r.status == 200) {
        log_tag("ADMIN", "user " + to_string(req.user_id) + " order " + id + " " + r.from + "->" + to);
        string code = to == "PACKED" ? "order_packed" : to == "SHIPPED" ? "order_shipped" : to == "OUT_FOR_DELIVERY" ? "order_ofd" :
                      to == "DELIVERED" ? "order_delivered" : "order_cancelled";
        return admin_redirect(client, req, back, code, "");
    }
    if (r.status == 400 || r.status == 404 || r.status == 409 || r.status == 503)
        return admin_problem(client, req, r.status, "Order #" + id, r.message);
    return error_page(client, req, 500, "Database error");
}

// ---- admin: users (A4) ----

// GET /admin/users?q=&page=
static int admin_users_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string q = trim(param(req, "q"));
    string page_text = param(req, "page");
    int pg = 1;
    if (!page_text.empty() && !parse_range(page_text, 1, 100000, pg)) return admin_problem(client, req, 400, "Users", "Bad page number.");

    string sql = "SELECT u.user_id, u.name, u.email, u.role, u.created_at, "
                 "(SELECT COUNT(*) FROM orders o WHERE o.user_id = u.user_id), "
                 "(SELECT COUNT(*) FROM user_sessions s WHERE s.user_id = u.user_id AND s.is_active = 1 AND s.expires_at > NOW()) "
                 "FROM users u WHERE 1=1";
    vector<string> args;
    if (!q.empty()) {
        sql += " AND (u.name LIKE ? ESCAPE '!' OR u.email LIKE ? ESCAPE '!')";
        args.push_back("%" + escape_like(q) + "%");
        args.push_back("%" + escape_like(q) + "%");
    }
    sql += " ORDER BY u.user_id LIMIT " + to_string(ORDERS_PER_PAGE + 1) + " OFFSET " + to_string((pg - 1) * ORDERS_PER_PAGE);

    vector<vector<string>> found;
    if (!db_select(sql, args, found) || !cols_ok(found, 7)) return error_page(client, req, 500, "Database error");
    bool more = (int)found.size() > ORDERS_PER_PAGE;
    if (more) found.resize(ORDERS_PER_PAGE);

    string rows;
    for (auto& u : found) {
        bool is_admin = (u[3] == "ADMIN");
        bool me = (atoi(u[0].c_str()) == req.user_id);
        string actions;
        if (me) actions = render("admin_user_me", {});           // Your own role and sessions are not changed from here.
        else {
            actions = render("admin_role_form", {{"id", u[0]}, {"role", is_admin ? "CUSTOMER" : "ADMIN"},
                                                 {"label", is_admin ? "Make customer" : "Make admin"}, {"back", admin_self(req)}});
            if (u[6] != "0") actions += " " + render("admin_logout_all_form", {{"id", u[0]}, {"back", admin_self(req)}});
        }
        rows += render("admin_user_row", {{"id", u[0]}, {"name", u[1]}, {"email", u[2]}, {"role", u[3]}, {"role_class", status_class(u[3])},
                                          {"joined", u[4]}, {"orders", u[5]}, {"sessions", u[6]}, {"actions", actions}});
    }
    if (found.empty()) rows = render("admin_none_row", {{"message", "No users match."}});

    string base = "/admin/users?";
    if (!q.empty()) base += "q=" + url_encode(q) + "&";
    string count = to_string(found.size()) + (found.size() == 1 ? " user" : " users") + " on this page (" + to_string(ORDERS_PER_PAGE) + " per page)";
    return send_admin(client, req, 200, "Users", "users",
                      render("admin_users", {{"q", q}, {"count", count}, {"rows", rows}, {"pager", admin_pager_html(base, pg, more)}}));
}

// POST /admin/users/role (user_id, role): the rules are in role_change in db.cpp (one transaction).
static int admin_user_role_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "user_id"));
    string role = trim(field(req, "role"));
    if (!valid_product_id(id) || (role != "ADMIN" && role != "CUSTOMER")) return admin_redirect(client, req, "/admin/users", "", "bad_request");

    RoleResult r = role_change(req.user_id, atoi(id.c_str()), role);
    if (r.status == 200) {
        log_tag("ADMIN", "user " + to_string(req.user_id) + " changed role of user " + id + " " + r.from + "->" + role);
        return admin_redirect(client, req, "/admin/users", "role_changed", "");
    }
    if (r.status >= 500 && r.code != "busy") return error_page(client, req, 500, "Database error");
    if (r.code == "busy") return error_page(client, req, 503, "The shop is busy right now. Please try again in a moment.");
    return admin_redirect(client, req, "/admin/users", "", r.code);       // Codes: own_role, last_admin, same_role, no_user, not_admin.
}

// ---- admin: sessions (A4) ----

// Status for admins: Active / Logged out / Expired / Ended by admin (session_status above is shared with the customer page).
static string admin_session_state(const vector<string>& s) {
    return session_status(s[6], s[7], s[8]);       // is_active, expired, end_reason.
}

// GET /admin/sessions?show=active|all&page=
static int admin_sessions_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string show = param(req, "show");
    if (show.empty()) show = "active";
    if (show != "active" && show != "all") return admin_problem(client, req, 400, "Sessions", "Bad filter.");
    string page_text = param(req, "page");
    int pg = 1;
    if (!page_text.empty() && !parse_range(page_text, 1, 100000, pg)) return admin_problem(client, req, 400, "Sessions", "Bad page number.");

    string sql = "SELECT s.session_id, u.name, u.email, s.login_time, s.last_activity, s.expires_at, s.is_active, s.expires_at <= NOW(), "
                 "IFNULL(s.end_reason, '') FROM user_sessions s JOIN users u ON u.user_id = s.user_id";
    if (show == "active") sql += " WHERE s.is_active = 1 AND s.expires_at > NOW()";
    sql += " ORDER BY s.login_time DESC, s.session_id LIMIT " + to_string(ORDERS_PER_PAGE + 1) + " OFFSET " + to_string((pg - 1) * ORDERS_PER_PAGE);

    vector<vector<string>> found;
    if (!db_select(sql, {}, found) || !cols_ok(found, 9)) return error_page(client, req, 500, "Database error");
    bool more = (int)found.size() > ORDERS_PER_PAGE;
    if (more) found.resize(ORDERS_PER_PAGE);

    string rows;
    for (auto& s : found) {
        string state = admin_session_state(s);
        bool mine = (s[0] == req.session_id);
        // The full session id is never on the page: only the first 8 characters (the button finds the session by those 8).
        string short_id = s[0].substr(0, 8);
        string action = state == "Active" && !mine ? render("admin_end_session_form", {{"id", short_id}, {"back", admin_self(req)}}) : (mine ? render("admin_user_me", {}) : "");
        rows += render("admin_session_row", {{"session", short_id}, {"name", s[1]}, {"email", s[2]}, {"login", s[3]}, {"activity", s[4]},
                                             {"expires", s[5]}, {"state", state}, {"state_class", status_class(state)}, {"action", action}});
    }
    if (found.empty()) rows = render("admin_none_row", {{"message", "No sessions to show."}});

    string base = "/admin/sessions?show=" + show + "&";
    string tabs = render("admin_tab", {{"href", "/admin/sessions?show=active"}, {"label", "Active"}, {"on", show == "active" ? " on" : ""}}) +
                  render("admin_tab", {{"href", "/admin/sessions?show=all"}, {"label", "All"}, {"on", show == "all" ? " on" : ""}});
    return send_admin(client, req, 200, "Sessions", "sessions",
                      render("admin_sessions", {{"tabs", tabs}, {"rows", rows}, {"pager", admin_pager_html(base, pg, more)}}));
}

// POST /admin/sessions/end (session_id = first 8 hex characters, or all 64): ends the session, end_reason 'FORCED'.
static int admin_session_end_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string sid = trim(field(req, "session_id"));
    bool hex = true;
    for (char c : sid)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = false;
    if (!hex || (sid.size() != 8 && sid.size() != 64)) return admin_redirect(client, req, "/admin/sessions", "", "bad_request");

    // The 8 characters must match exactly ONE active session (the full id is never shown).
    vector<vector<string>> found;
    if (!db_select("SELECT session_id FROM user_sessions WHERE session_id LIKE ? AND is_active = 1 LIMIT 2", {sid + "%"}, found))
        return error_page(client, req, 500, "Database error");
    if (found.empty()) return admin_redirect(client, req, "/admin/sessions", "", "session_gone");
    if (found.size() > 1) return admin_redirect(client, req, "/admin/sessions", "", "bad_request");
    const string full = found[0][0];
    if (full == req.session_id) return admin_redirect(client, req, "/admin/sessions", "", "own_session");

    int n = db_exec("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED' WHERE session_id = ? AND is_active = 1", {full});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/sessions", "", "session_gone");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " ended session " + full.substr(0, 8));
    audit(req, "SESSION_END", "session " + full.substr(0, 8));
    return admin_redirect(client, req, "/admin/sessions", "session_ended", "");
}

// POST /admin/users/logout-all (user_id): ends all of that user's active sessions.
static int admin_user_logout_all_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "user_id"));
    if (!valid_product_id(id)) return admin_redirect(client, req, "/admin/users", "", "bad_request");
    if (atoi(id.c_str()) == req.user_id) return admin_redirect(client, req, "/admin/users", "", "own_user");

    int n = db_exec("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED' WHERE user_id = ? AND is_active = 1", {id});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/users", "", "no_sessions");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " ended all " + to_string(n) + " sessions of user " + id);
    audit(req, "USER_LOGOUT_ALL", "user " + id + ", " + to_string(n) + " sessions ended");
    return admin_redirect(client, req, "/admin/users", "sessions_ended", "");
}

// ---- admin: logs (A4) ----

// GET /admin/logs?worker=&cls=2xx|3xx|4xx|5xx&path=&refresh=1
static int admin_logs_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string worker = trim(param(req, "worker"));
    string cls = param(req, "cls");
    string path = trim(param(req, "path"));
    bool refresh = param(req, "refresh") == "1";
    if (!worker.empty() && !is_number(worker)) return admin_problem(client, req, 400, "Logs", "Bad worker filter.");
    if (!cls.empty() && cls != "2xx" && cls != "3xx" && cls != "4xx" && cls != "5xx") return admin_problem(client, req, 400, "Logs", "Bad status filter.");

    // SQL pieces are fixed; worker and path only in args. The status class numbers are our own (2xx -> 200..299).
    string sql = "SELECT l.created_at, l.method, l.path, l.status, l.latency_ms, l.queue_wait_ms, l.worker_id, "
                 "IFNULL(CONCAT(u.name, ' (#', l.user_id, ')'), '-') FROM request_logs l LEFT JOIN users u ON u.user_id = l.user_id WHERE 1=1";
    vector<string> args;
    if (!worker.empty()) {
        sql += " AND l.worker_id = ?";
        args.push_back(worker);
    }
    if (!cls.empty()) {
        int lo = (cls[0] - '0') * 100;
        sql += " AND l.status BETWEEN " + to_string(lo) + " AND " + to_string(lo + 99);
    }
    if (!path.empty()) {
        sql += " AND l.path LIKE ? ESCAPE '!'";
        args.push_back("%" + escape_like(path) + "%");
    }
    sql += " ORDER BY l.log_id DESC LIMIT 100";

    vector<vector<string>> logs, workers, actions;
    int cid = 0;
    MYSQL* m = get_conn(cid);               // All three queries on one connection.
    if (m == nullptr) return error_page(client, req, 500, "Database error");
    bool ok = db_select_on(m, sql, args, logs) &&
              db_select_on(m, "SELECT worker_id, COUNT(*), AVG(latency_ms) FROM request_logs "
                              "WHERE created_at >= NOW() - INTERVAL 5 MINUTE GROUP BY worker_id ORDER BY worker_id", {}, workers) &&
              db_select_on(m, "SELECT a.created_at, u.name, a.action, a.details FROM admin_actions a "
                              "JOIN users u ON u.user_id = a.admin_id ORDER BY a.action_id DESC LIMIT 50", {}, actions);
    put_conn(cid);
    if (!ok || !cols_ok(logs, 8) || !cols_ok(workers, 3) || !cols_ok(actions, 4)) return error_page(client, req, 500, "Database error");

    // Worker bars: the busiest one is 100%.
    long long top = 0;
    for (auto& w : workers) top = max(top, atoll(w[1].c_str()));
    string worker_rows;
    for (auto& w : workers) {
        long long count = atoll(w[1].c_str());
        int pct = top > 0 ? (int)(count * 100 / top) : 0;
        char avg[32];
        snprintf(avg, sizeof avg, "%.2f", atof(w[2].c_str()));
        worker_rows += render("admin_worker_row", {{"worker", w[0]}, {"count", w[1]}, {"avg", avg}, {"pct", to_string(pct)}});
    }
    if (workers.empty()) worker_rows = render("admin_none_row", {{"message", "No requests in the last 5 minutes."}});

    string log_rows;
    for (auto& l : logs)
        log_rows += render("admin_log_row", {{"time", l[0]}, {"method", l[1]}, {"path", l[2]}, {"status", l[3]}, {"status_class", "s" + l[3].substr(0, 1)},
                                             {"latency", l[4]}, {"queue", l[5]}, {"worker", l[6]}, {"user", l[7]}});
    if (logs.empty()) log_rows = render("admin_none_row", {{"message", "No log rows match."}});

    string action_rows;
    for (auto& a : actions)
        action_rows += render("admin_activity_row", {{"time", a[0]}, {"admin", a[1]}, {"action", a[2]}, {"details", a[3]}});
    if (actions.empty()) action_rows = render("admin_none_row", {{"message", "No admin changes yet."}});

    string class_options = render("search_option", {{"value", ""}, {"label", "All status classes"}, {"selected", cls.empty() ? "selected" : ""}});
    for (const char* c : {"2xx", "3xx", "4xx", "5xx"})
        class_options += render("search_option", {{"value", c}, {"label", c}, {"selected", cls == c ? "selected" : ""}});

    string head = refresh ? render("meta_refresh", {{"seconds", "3"}}) : "";
    return send_admin(client, req, 200, "Logs", "logs",
                      render("admin_logs", {{"worker", worker}, {"path", path}, {"class_options", class_options},
                                            {"refresh_checked", refresh ? "checked" : ""}, {"worker_rows", worker_rows},
                                            {"log_rows", log_rows}, {"log_count", to_string(logs.size()) + " of the last 100 requests"},
                                            {"action_rows", action_rows}}), head);
}

// /admin ... : guard first, then the page by path.
int admin_route(SOCKET client, const Request& req) {
    int guard = require_admin(client, req);
    if (guard != 0) return guard;

    const string& p = req.path;
    if (p == "/admin") return admin_dashboard_page(client, req);
    if (p == "/admin/products") return admin_products_page(client, req);
    if (p == "/admin/products/new") return admin_product_new_page(client, req);
    if (p == "/admin/products/edit") return admin_product_edit_page(client, req);
    if (p == "/admin/products/restock") return admin_restock_page(client, req);
    if (p == "/admin/products/toggle") return admin_toggle_page(client, req);
    if (p == "/admin/categories") return admin_categories_page(client, req);
    if (p == "/admin/categories/add") return admin_category_add_page(client, req);
    if (p == "/admin/categories/rename") return admin_category_rename_page(client, req);
    if (p == "/admin/categories/delete") return admin_category_delete_page(client, req);
    if (p == "/admin/orders") return admin_orders_page(client, req);
    if (p == "/admin/order") return admin_order_page(client, req);
    if (p == "/admin/order/status") return admin_order_status_page(client, req);
    if (p == "/admin/users") return admin_users_page(client, req);
    if (p == "/admin/users/role") return admin_user_role_page(client, req);
    if (p == "/admin/users/logout-all") return admin_user_logout_all_page(client, req);
    if (p == "/admin/sessions") return admin_sessions_page(client, req);
    if (p == "/admin/sessions/end") return admin_session_end_page(client, req);
    if (p == "/admin/logs") return admin_logs_page(client, req);
    return error_page(client, req, 404, "404 Not Found");
}


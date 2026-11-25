
#include "platform.h"   // secure_random (BCryptGenRandom / getrandom), sleep_ms, local_time
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "common.h"
#include "pages.h"
using namespace std;


const string HTML = "text/html; charset=utf-8";
const string COOKIE_NAME = "session_id";

// SQL piece for a session's new expiry: now + SESSION_MINUTES.
// SESSION_MINUTES is our own constant, not user input, so joining it into the SQL is safe.
const string EXPIRES_SQL = "NOW() + INTERVAL " + to_string(SESSION_MINUTES) + " MINUTE";

// The rupee sign (3 UTF-8 bytes). A separate const because a digit after "\xB9" would become part of the hex escape.
const string RUPEE = "\xE2\x82\xB9";

// Category list for the navbar, home and products pages, ordered by id.
const string CATEGORIES_SQL = "SELECT category_id, category_name FROM categories ORDER BY category_id";

// Home "Budget picks": in-stock products up to BUDGET_MAX (our constant, not user input), cheapest first.
// Columns match product_card: id, name, category, price, stock, description.
const string BUDGET_SQL =
    "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description "
    "FROM products p JOIN categories c ON c.category_id = p.category_id "
    "WHERE p.price <= " + to_string(BUDGET_MAX) + " AND p.stock > 0 AND p.is_active = TRUE ORDER BY p.price, p.product_id LIMIT 10";

// Escape user text before putting it in HTML (otherwise a script could run).
static string html_escape(const string& s) {
    string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else out += c;
    }
    return out;
}

// ---- templates: loaded into memory once at startup, no disk reads per request ----

static map<string, string> templates;           // Name ("layout") -> full .html text.
static mutex render_problem_lock;               // Protects the counters below (workers render at the same time).
static int render_problems = 0;                 // Count of missing keys / templates (the tests check it).
static string render_last_problem;

// Templates the code cannot work without; if one is missing the server does not start.
static const char* REQUIRED_TEMPLATES[] = {
    "layout", "nav", "search_form", "search_option", "account_guest", "account_user", "cart_link", "subnav", "subnav_link",
    "flash", "flash_text", "error", "detail_text", "plain", "heading", "empty_state", "meta_refresh",
    "home", "hero_create", "hero_orders", "home_categories", "category_tile", "home_budget",
    "login", "signup", "products", "side_cat", "hidden_input", "no_products",
    "product_card", "thumb_photo", "thumb_emoji", "stock_in", "stock_low", "soldout", "signin_buy", "add_form_card", "add_form_page", "product",
    "stock_warn", "cart_row", "cart", "checkout_row", "checkout", "checkout_failed",
    "order_row", "orders", "order_item_row", "order_payment", "order", "session_row", "sessions",
    "admin_layout", "admin_link", "admin_nav_link", "admin_card", "admin_order_row", "admin_status_count", "admin_low_row",
    "admin_low", "admin_none", "admin_none_row", "admin_message", "admin_dashboard", "admin_restock_form",
    "admin_users", "admin_user_row", "admin_user_me", "admin_role_form", "admin_logout_all_form", "admin_sessions", "admin_session_row",
    "admin_end_session_form", "admin_tab", "admin_logs", "admin_worker_row", "admin_log_row", "admin_activity_row", "admin_orders", "admin_orders_row", "admin_pager", "admin_pager_link", "admin_order", "admin_order_item_row",
    "admin_order_action", "admin_order_buttons", "order_cancel", "timeline", "timeline_step", "timeline_eta", "admin_history_row",
    "error_list", "error_list_item", "pw_field", "pw_rules", "pw_rule_item", "change_password",
    "drawer", "drawer_user", "drawer_admin", "drawer_guest", "tabbar",
    "checkout_payment", "address_extra", "address_text", "address_none", "address_default_badge", "address_default_form", "address_card", "addresses",
    "addresses_empty", "address_form", "checkout_address", "checkout_no_address", "checkout_place", "order_address", "admin_products", "admin_product_row", "admin_product_form", "admin_categories", "admin_category_row", "metric_row", "metrics", "test_item", "test", "dbtest",
    "flash_added", "flash_max", "flash_out", "flash_stock", "modals", "scripts"};

// Loads every *.html file (name = file name without .html). Returns whether all required ones were found.
bool load_templates(const string& dir) {
    templates.clear();
    error_code ec;
    if (filesystem::is_directory(dir, ec)) {
        for (auto& entry : filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".html") continue;
            ifstream f(entry.path(), ios::binary);
            string text((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
            // Remove trailing blank lines so joined partials leave no extra space.
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            templates[entry.path().stem().string()] = text;
        }
    }

    bool ok = true;
    for (const char* name : REQUIRED_TEMPLATES) {
        if (templates.find(name) == templates.end()) {
            log_tag("PAGE", "missing template: " + dir + "/" + name + ".html");
            ok = false;
        }
    }
    if (ok) log_tag("PAGE", "templates loaded: " + to_string(templates.size()) + " from '" + dir + "'");
    return ok;
}

// Cache busting: /static/style.css?v={{ver}} (server start time: a new start makes browsers fetch new css/js).
// Every template gets it automatically (the static handler ignores the ?query).
static const string STATIC_VER = to_string((long long)time(nullptr));

// Value of a key; if missing it returns "" and records a problem (to catch mistakes).
static string template_value(const string& tname, const map<string, string>& values, const string& key_text) {
    string key = trim(key_text);
    auto it = values.find(key);
    if (it != values.end()) return it->second;
    if (key == "ver") return STATIC_VER;

    render_problem_lock.lock();
    render_problems++;
    render_last_problem = "template '" + tname + "' needs the value '" + key + "'";
    render_problem_lock.unlock();
    log_tag("PAGE", "template '" + tname + "' needs the value '" + key + "'");
    return "";
}

// Placeholders: {{key}} = HTML-escaped (always for user/DB text), {{{key}}} = raw (only for HTML we built ourselves).
// One pass only: a "{{...}}" inside an inserted value is not expanded again.
string render(const string& name, const map<string, string>& values) {
    auto t = templates.find(name);
    if (t == templates.end()) {
        render_problem_lock.lock();
        render_problems++;
        render_last_problem = "template '" + name + "' not found";
        render_problem_lock.unlock();
        log_tag("PAGE", "template '" + name + "' not found");
        return "";
    }

    const string& src = t->second;
    string out;
    size_t i = 0;
    while (i < src.size()) {
        if (src.compare(i, 3, "{{{") == 0) {
            size_t end = src.find("}}}", i + 3);
            if (end != string::npos) {
                out += template_value(name, values, src.substr(i + 3, end - i - 3));    // raw.
                i = end + 3;
                continue;
            }
        } else if (src.compare(i, 2, "{{") == 0) {
            size_t end = src.find("}}", i + 2);
            if (end != string::npos) {
                out += html_escape(template_value(name, values, src.substr(i + 2, end - i - 2)));
                i = end + 2;
                continue;
            }
        }
        out += src[i];
        i++;
    }
    return out;
}

// ---- page helpers ----

// Cookie handling for every reply that sets no Set-Cookie itself (e.g. login/logout): an expired session removes the cookie;
// a logged-in user gets the cookie again with a new Max-Age (see the idle timeout below).
string with_cookie_fix(const Request& req, const string& extra) {
    if (extra.find("Set-Cookie") != string::npos) return extra;
    if (req.clear_cookie) return extra + session_cookie("", 0);
    if (req.refresh_cookie) return extra + session_cookie(req.session_id, SESSION_MINUTES * 60);
    return extra;
}

// Navigation bar: logged in = name, cart badge, Orders, Sessions, Logout; otherwise Login + Signup.
// Logout is a POST, not a link, so another site cannot log you out with <img src=/logout>.
static string nav_bar(const Request& req, bool allow_db) {
    vector<vector<string>> cats = get_categories(allow_db);

    // Search form: category <select> (the chosen one selected) + text box. GET /products?cat=&q=.
    string cur_cat, cur_q;
    auto c = req.params.find("cat");
    if (c != req.params.end()) cur_cat = c->second;
    auto qi = req.params.find("q");
    if (qi != req.params.end()) cur_q = qi->second;

    string options = render("search_option", {{"value", ""}, {"label", "All"}, {"selected", cur_cat.empty() ? "selected" : ""}});
    string links;
    for (auto& cat : cats) {
        options += render("search_option", {{"value", cat[0]}, {"label", cat[1]}, {"selected", cat[0] == cur_cat ? "selected" : ""}});
        links += render("subnav_link", {{"id", cat[0]}, {"emoji", category_emoji(cat[1])}, {"name", cat[1]}});
    }

    // Account area: guests get Sign in / Sign up (they open the popups), logged-in users get the name and links.
    string account = req.user_id > 0 ? render("account_user", {{"name", req.user_name}, {"admin", req.role == "ADMIN" ? render("admin_nav_link", {}) : ""}})
                                     : render("account_guest", {});
    string cart = render("cart_link", {{"cart_count", to_string(req.cart_count)},
                                       {"cart_unit", req.cart_count == 1 ? "item" : "items"}});
    string subnav = render("subnav", {{"links", links}, {"budget", rupees((long long)BUDGET_MAX * 100)},
                                      {"budget_max", to_string(BUDGET_MAX)}});

    // Mobile: the menu drawer and the bottom tab bar; css hides them on desktop (display: none).
    // The drawer also opens with :target, so the links work without JavaScript.
    string items = req.user_id > 0 ? render("drawer_user", {{"name", req.user_name}, {"admin", req.role == "ADMIN" ? render("drawer_admin", {}) : ""}})
                                   : render("drawer_guest", {});
    const string& p = req.path;
    auto on = [&p](bool yes) { return string(yes ? "on" : ""); };
    string tabbar = render("tabbar", {{"cart_count", to_string(req.cart_count)}, {"home_on", on(p == "/")},
                                      {"cats_on", on(p == "/products" || p == "/product")},
                                      {"orders_on", on(p == "/orders" || p == "/order")},
                                      {"acct_on", on(p == "/addresses" || p == "/sessions")},
                                      {"cart_on", on(p == "/cart" || p == "/checkout")}});
    return render("nav", {{"search", render("search_form", {{"options", options}, {"q", cur_q}})},
                          {"account", account}, {"cart", cart}, {"subnav", subnav},
                          {"drawer", render("drawer", {{"items", items}})}, {"tabbar", tabbar}});
}

// Message box (kind = "ok" green, "err" red). message_html must be HTML we built (user text already escaped).
string flash_box(const string& kind, const string& message_html) {
    return render("flash", {{"kind", kind}, {"message", message_html}});
}

// Same, but from text (may be user/DB text): the engine escapes it.
string flash_text(const string& kind, const string& text) {
    return render("flash_text", {{"kind", kind}, {"message", text}});
}

// Builds the full page from layout.html. head_extra = extra <head> tag (e.g. the /metrics refresh); flash = message box on top.
// interactive = popups + app.js (off for /metrics). allow_db = false for error pages and /metrics: they must never wait for the DB.
string page(const Request& req, const string& title, const string& content, const string& head_extra,
                   const string& flash, bool interactive, bool allow_db) {
    string modals = (interactive && req.user_id == 0) ? render("modals", pw_parts(false, nullptr)) : "";
    string scripts = interactive ? render("scripts", {}) : "";
    return render("layout", {{"title", title}, {"head_extra", head_extra}, {"nav", nav_bar(req, allow_db)},
                             {"flash", flash}, {"content", content}, {"modals", modals}, {"scripts", scripts}});
}

int send_page(SOCKET client, const Request& req, int status, const string& title, const string& content,
                     const string& extra, const string& flash) {
    send_response(client, status, HTML, page(req, title, content, "", flash), with_cookie_fix(req, extra));
    return status;
}

string error_content(const string& msg, const string& detail_html) {
    return render("error", {{"message", msg}, {"detail", detail_html}});
}

int error_page(SOCKET client, const Request& req, int status, const string& msg) {
    return send_page(client, req, status, msg, error_content(msg));
}

// For server.cpp's own error replies (400/408/503): a full page without login.
string render_error_page(const string& message) {
    Request anonymous;
    return page(anonymous, message, error_content(message), "", "", true, false);   // No DB: the accept thread must not wait.
}

// 302 redirect (Post/Redirect/Get). location is always one of our fixed paths, never user input.
int redirect(SOCKET client, const Request& req, const string& location, const string& extra) {
    send_response(client, 302, HTML, render("plain", {{"message", "Redirecting..."}}),
                  "Location: " + location + "\r\n" + with_cookie_fix(req, extra));
    return 302;
}

// 405 with an Allow header.
int method_not_allowed(SOCKET client, const Request& req, const string& allow) {
    send_response(client, 405, HTML, page(req, "Method Not Allowed", error_content("Method Not Allowed")),
                  with_cookie_fix(req, "Allow: " + allow + "\r\n"));
    return 405;
}

// "499.00" -> 49900 (paise): money is added as integers, not floats (price is DECIMAL(10,2)).
long long to_cents(const string& price) {
    long long v = 0;
    for (char c : price)
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
    return v;
}

// Formats paise as rupees with a thousands comma: 149900 -> 1,499 and 103350 -> 1,033.50 (paise shown only if not 00).
string rupees(long long cents) {
    string whole = to_string(cents / 100);
    string grouped;
    for (size_t i = 0; i < whole.size(); i++) {
        if (i > 0 && (whole.size() - i) % 3 == 0) grouped += ',';
        grouped += whole[i];
    }
    long long paise = cents % 100;
    string out = RUPEE + grouped;
    if (paise != 0) out += "." + string(paise < 10 ? "0" : "") + to_string(paise);
    return out;
}

// A DB price string ("1499.00") straight to rupee text.
string rupees_text(const string& price) {
    return rupees(to_cents(price));
}

// One emoji per category (card background, navbar, category tiles); a default if the table has none.
struct CategoryEmoji {
    const char* name;
    const char* emoji;
};
static const CategoryEmoji CATEGORY_EMOJIS[] = {
    {"Electronics", "💡"}, {"Accessories", "🔋"}, {"Audio", "🎧"}, {"Mobile Phones", "📱"}, {"Laptops", "💻"},
    {"Computers", "🖥️"}, {"Cameras", "📷"}, {"Gaming", "🎮"}, {"Clothing", "👕"}, {"Shoes", "👟"}, {"Books", "📚"},
    {"Home & Kitchen", "🍳"}, {"Beauty & Personal Care", "💄"}, {"Sports & Fitness", "🏋️"}, {"Toys & Games", "🧸"}};

string category_emoji(const string& name) {
    for (const CategoryEmoji& c : CATEGORY_EMOJIS)
        if (name == c.name) return c.emoji;
    return "🛍️";
}

// A colour (0-359) from the category name: the same category always gets the same colour.
int thumb_hue(const string& category) {
    unsigned int sum = 0;
    for (unsigned char c : category) sum = sum * 31 + c;
    return (int)(sum % 360);
}

// CSS class from a status text: lower-case letters only ("Logged out" -> "loggedout"). The text comes from the DB, so it is cleaned.
string status_class(const string& status) {
    string out;
    for (unsigned char c : status)
        if (isalpha(c)) out += (char)tolower(c);
    return out.empty() ? "other" : out;
}

// Display name of an order status (the DB keeps PLACED, OUT_FOR_DELIVERY ...). Unknown statuses are shown as they are.
string status_label(const string& status) {
    if (status == "PLACED") return "Ordered";
    if (status == "PACKED") return "Packed";
    if (status == "SHIPPED") return "Shipped";
    if (status == "OUT_FOR_DELIVERY") return "Out for delivery";
    if (status == "DELIVERED") return "Delivered";
    if (status == "CANCELLED") return "Cancelled";
    return status;
}

// Stock badge: "In stock" (green) or "Only N left" up to LOW_STOCK (orange). Nothing at 0: the button is disabled there.
string stock_html(const string& stock) {
    int n = atoi(stock.c_str());
    if (n <= 0) return "";
    if (n <= LOW_STOCK) return render("stock_low", {{"stock", to_string(n)}});
    return render("stock_in", {{"stock", to_string(n)}});      // The badge only says "In stock"; the exact count is in the tooltip.
}

// Digits only, 1 to 9 of them.
bool is_number(const string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Stop % _ and ! from having their LIKE meaning (we use ESCAPE '!').
string escape_like(const string& s) {
    string out;
    for (char c : s) {
        if (c == '!' || c == '%' || c == '_') out += '!';
        out += c;
    }
    return out;
}

string param(const Request& req, const string& key) {
    auto it = req.params.find(key);
    return it == req.params.end() ? "" : it->second;
}

string field(const Request& req, const string& key) {
    auto it = req.form.find(key);
    return it == req.form.end() ? "" : it->second;
}

// Does every row have at least n columns? (true for no rows; check rows.empty() separately before rows[0].)
// This keeps rows[i][j] from ever going out of range.
bool cols_ok(const vector<vector<string>>& rows, size_t n) {
    for (auto& r : rows)
        if (r.size() < n) {
            log_tag("PAGE", "unexpected query result shape (expected " + to_string(n) + " columns)");
            return false;
        }
    return true;
}

// ---- categories cache: the navbar needs them on every page and they rarely change, so keep them 60 s in memory ----
// Many workers read it, so a mutex protects it. If the DB fails, the old list stays in use.
static mutex cat_lock;
static vector<vector<string>> cat_cache;        // (id, name).
static long long cat_loaded_us = 0;             // 0 = not loaded yet.
const long long CAT_CACHE_US = 60LL * 1000000LL;

vector<vector<string>> get_categories(bool allow_db) {
    cat_lock.lock();
    vector<vector<string>> copy = cat_cache;
    bool fresh = cat_loaded_us != 0 && now_us() - cat_loaded_us < CAT_CACHE_US;
    cat_lock.unlock();
    if (fresh || !allow_db) return copy;

    vector<vector<string>> rows;                 // DB work outside the mutex.
    if (!db_select(CATEGORIES_SQL, {}, rows) || !cols_ok(rows, 2)) return copy;
    cat_lock.lock();
    cat_cache = rows;
    cat_loaded_us = now_us();
    cat_lock.unlock();
    return rows;
}

// After any category change drop the cached navbar list, so the next store request shows the new name.
void clear_category_cache() {
    cat_lock.lock();
    cat_cache.clear();
    cat_loaded_us = 0;
    cat_lock.unlock();
}

// ---- sessions ----

// 32 random bytes -> 64 hex characters. Not rand(): it is guessable; BCryptGenRandom is not.
bool new_session_id(string& out) {
    unsigned char raw[32];
    if (!secure_random(raw, sizeof(raw))) return false;
    const char* hex = "0123456789abcdef";
    out = "";
    for (int i = 0; i < 32; i++) {
        out += hex[raw[i] >> 4];
        out += hex[raw[i] & 15];
    }
    return true;
}

// Is the cookie really 64 hex characters? (otherwise there is no need to ask the DB)
static bool looks_like_session_id(const string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Cookie header: HttpOnly = JavaScript cannot read it, SameSite=Lax = not sent on cross-site POSTs,
// Path=/ = whole site, Max-Age = seconds until the browser drops it (0 = now).
string session_cookie(const string& value, int max_age) {
    return "Set-Cookie: " + COOKIE_NAME + "=" + value + "; HttpOnly; SameSite=Lax; Path=/; Max-Age=" +
           to_string(max_age) + "\r\n";
}

// Finds the session in the DB and fills req.user_id / user_name / cart_count.
// All its queries use the given connection m (the caller does get_conn / put_conn).
static void load_user_on(MYSQL* m, Request& req, const string& sid) {
    // Only is_active = 1 sessions; whether it expired is asked in SQL (the DB's clock).
    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT s.user_id, u.name, s.expires_at <= NOW(), u.role FROM user_sessions s "
                         "JOIN users u ON u.user_id = s.user_id WHERE s.session_id = ? AND s.is_active = 1",
                      {sid}, rows))
        return;                         // DB error: treat as anonymous.
    if (rows.empty()) return;           // No such session, or already ended (logout or expiry).
    if (!cols_ok(rows, 4)) return;      // Wrong result shape: treat as anonymous.

    if (rows[0][2] == "1") {
        // Session expired: close it in the DB (logout_time = expires_at), treat as logged out, and remove the cookie in the reply.
        // AND is_active = 1: if the cleanup thread closed it first, nothing changes.
        db_exec_on(m, "UPDATE user_sessions SET is_active = 0, logout_time = expires_at, end_reason = 'EXPIRED' "
                      "WHERE session_id = ? AND is_active = 1", {sid});
        req.clear_cookie = true;
        log_tag("PAGE", "session of user " + rows[0][0] + " expired");
        return;
    }

    // IDLE TIMEOUT (sliding expiry): every request moves expires_at to now + SESSION_MINUTES, so a session ends only after that long without activity.
    // One UPDATE sets last_activity and expires_at together. WHERE is_active = 1 AND expires_at > NOW(): a session that just ended is not revived.
    int n = db_exec_on(m, "UPDATE user_sessions SET last_activity = NOW(), expires_at = " + EXPIRES_SQL +
                          " WHERE session_id = ? AND is_active = 1 AND expires_at > NOW()", {sid});
    if (n == 0) {                       // The session just ended: treat as logged out, remove the cookie.
        req.clear_cookie = true;
        return;
    }

    req.user_id = atoi(rows[0][0].c_str());
    req.user_name = rows[0][1];
    req.role = rows[0][3] == "ADMIN" ? "ADMIN" : "CUSTOMER";    // Anything else (or empty) gets the lowest rights.
    req.session_id = sid;

    // The browser cookie and the DB expiry must move together, so the cookie is sent again with every reply (new Max-Age).
    // n < 0 (DB error): the DB expiry did not change, so the cookie stays too (the user is let in this time).
    req.refresh_cookie = (n > 0);

    // For the nav bar "Cart (N items)"; 0 on error.
    int count = cart_count(m, req.user_id);
    req.cart_count = count > 0 ? count : 0;
}

// On every request: find the session from the cookie and fill req.user_id / user_name.
// All session queries of this request use one connection: get_conn first, put_conn last.
static void load_user(Request& req) {
    auto it = req.cookies.find(COOKIE_NAME);
    if (it == req.cookies.end() || !looks_like_session_id(it->second)) return;   // No cookie: no need to ask the DB.

    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return;           // No connection: treat as anonymous.
    load_user_on(m, req, it->second);
    put_conn(id);                       // The connection goes back whether or not load_user_on returned early.
}


// /test: shows the request as received.
static int test_page(SOCKET client, const Request& req) {
    string params, headers;
    for (auto& kv : req.params)
        params += render("test_item", {{"key", kv.first}, {"sep", " = "}, {"value", kv.second}});
    for (auto& kv : req.headers) {
        // The cookie holds the session id: never show it on the page.
        string val = (kv.first == "cookie") ? "(hidden)" : kv.second;
        headers += render("test_item", {{"key", kv.first}, {"sep", ": "}, {"value", val}});
    }
    return send_page(client, req, 200, "Request",
                     render("test", {{"method", req.method}, {"path", req.path}, {"version", req.version},
                                     {"body_size", to_string(req.body.size())},
                                     {"params", params}, {"headers", headers}}));
}

// Runs the SQL on a pool connection and shows the result, connection number and thread number (testing only).
static int db_test_page(SOCKET client, const Request& req, const string& title, const string& sql) {
    string val;
    int conn = 0;
    if (!db_scalar(sql, val, &conn)) return error_page(client, req, 500, "Database error");
    return send_page(client, req, 200, title,
                     render("dbtest", {{"title", title}, {"result", val}, {"conn", to_string(conn)},
                                       {"thread", to_string(thread_id())}}));
}

// ---- going back to the same page after "Add to cart" ----

// Removes one query parameter from a target: strip_param("/products?cat=3&added=5", "added") -> "/products?cat=3".
string strip_param(const string& target, const string& name) {
    size_t q = target.find('?');
    if (q == string::npos) return target;

    string kept;
    size_t pos = q + 1;
    while (pos <= target.size()) {
        size_t amp = target.find('&', pos);
        if (amp == string::npos) amp = target.size();
        string pair = target.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty()) continue;
        if (pair.substr(0, pair.find('=')) == name) continue;   // This is the parameter to remove.
        kept += (kept.empty() ? "" : "&") + pair;
    }
    return kept.empty() ? target.substr(0, q) : target.substr(0, q) + "?" + kept;
}

// This page's path + query without the old "added"/"cart_err"; it goes into the form's hidden "return" field.
string return_target(const Request& req) {
    return strip_param(strip_param(req.target, "added"), "cart_err");
}

// Stops an OPEN REDIRECT: "return" comes from the form, so anyone could send users to another site. Only our own paths pass:
// start with "/" but not "//", no backslash, no space or control characters (\r \n could inject a header); otherwise /products.
string safe_return(const string& r) {
    const string fallback = "/products";
    if (r.empty() || r.size() > 1000 || r[0] != '/') return fallback;
    if (r.size() > 1 && r[1] == '/') return fallback;
    for (char c : r)
        if (c == '\\' || c <= 32 || c == 127) return fallback;   // c <= 32 also catches non-ASCII bytes.
    return r;
}

// With ?added=<product id> (and optional cart_err) show a message: green (added) or red (problem). cart_err=stock -> "Only N left", out -> "out of stock".
// The texts are fixed; the product name comes from the DB and is escaped; added is used only if it is a number.
string cart_notice(const Request& req) {
    string id = param(req, "added");
    if (!is_number(id) || atoi(id.c_str()) < 1) return "";

    vector<vector<string>> rows;
    if (!db_select("SELECT name, stock FROM products WHERE product_id = ?", {id}, rows)) return "";
    if (rows.empty() || !cols_ok(rows, 2)) return "";
    string name = rows[0][0];
    string err = param(req, "cart_err");
    int stock = atoi(rows[0][1].c_str());

    // The message pieces are templates; the engine escapes the name.
    if (err == "max")
        return flash_box("err", render("flash_max", {{"max", to_string(MAX_CART_QTY)}, {"name", name}}));
    if (err == "out" || (err == "stock" && stock == 0))
        return flash_box("err", render("flash_out", {{"name", name}}));
    if (err == "stock")
        return flash_box("err", render("flash_stock", {{"stock", to_string(stock)}, {"name", name}}));
    return flash_box("ok", render("flash_added", {{"name", name}}));
}

// The debug routes (/test, /slow, /dbtest, /dbslow) exist only when the environment variable DEBUG_ROUTES is 1.
static bool debug_routes_on() {
    const char* v = getenv("DEBUG_ROUTES");
    return v != nullptr && string(v) == "1";
}

// Sends the page for the path; returns the status code.
int route(SOCKET client, Request& req) {
    // /metrics needs no login or session (and no DB): handled before load_user.
    if (req.path == "/metrics") return metrics_page(client, req, false);
    if (req.path == "/metrics.json") return metrics_page(client, req, true);

    // /healthz: Render (or any uptime monitor) asks "is the server alive?". It only shows that a worker thread answers:
    // no database, session or template is touched, so it comes before load_user. It reveals no internal numbers (unlike /metrics).
    if (req.path == "/healthz") {
        if (req.method != "GET" && req.method != "HEAD") return method_not_allowed(client, req, "GET, HEAD");
        send_response(client, 200, "text/plain; charset=utf-8", "ok\n", "Cache-Control: no-store\r\n");
        return 200;
    }

    load_user(req);                     // First find out who it is (on every request).
    const string& path = req.path;

    if (path == "/admin" || path.rfind("/admin/", 0) == 0) return admin_route(client, req);   // The guard is inside admin_route.

    if (path == "/") return home_page(client, req);
    if (path == "/products") return products_page(client, req);
    if (path == "/product") return product_page(client, req);
    if (path == "/signup") return signup_page(client, req);
    if (path == "/login") return login_page(client, req);
    if (path == "/logout") return logout_page(client, req);
    if (path == "/account/password") return account_password_page(client, req);
    if (path == "/sessions") return sessions_page(client, req);
    if (path == "/cart") return cart_page(client, req);
    if (path == "/cart/add") return cart_add_page(client, req);
    if (path == "/cart/update") return cart_update_page(client, req);
    if (path == "/cart/remove") return cart_remove_page(client, req);
    if (path == "/addresses") return addresses_page(client, req);
    if (path == "/addresses/new") return address_new_page(client, req);
    if (path == "/addresses/edit") return address_edit_page(client, req);
    if (path == "/addresses/delete") return address_delete_page(client, req);
    if (path == "/addresses/default") return address_default_page(client, req);
    if (path == "/checkout") return checkout_page(client, req);
    if (path == "/orders") return orders_page(client, req);
    if (path == "/order") return order_page(client, req);
    if (path == "/order/cancel") return order_cancel_page(client, req);

    // Debug routes: without DEBUG_ROUTES=1 they are not matched and fall through to 404.
    if (path == "/test" && debug_routes_on()) return test_page(client, req);
    if (path == "/slow" && debug_routes_on()) {
        sleep_ms(3000);
        send_response(client, 200, HTML, render("plain", {{"message", "done"}}), with_cookie_fix(req, ""));
        return 200;
    }
    if (path == "/dbtest" && debug_routes_on()) return db_test_page(client, req, "Products in database", "SELECT COUNT(*) FROM products");
    if (path == "/dbslow" && debug_routes_on()) return db_test_page(client, req, "Slept 2 seconds", "SELECT SLEEP(2)");

    return error_page(client, req, 404, "404 Not Found");
}


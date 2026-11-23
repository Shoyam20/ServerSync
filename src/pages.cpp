// pages.cpp - routes, sessions (signup/login/logout), template render(), and the data for each page.
// Owner: Ashutosh
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>      // BCryptGenRandom: the OS secure random number generator.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "common.h"
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

static string session_cookie(const string& value, int max_age);   // Defined below, in the sessions section.
static vector<vector<string>> get_categories(bool allow_db);        // Defined below, in the helpers.
static string category_emoji(const string& name);
static string rupees(long long cents);
static map<string, string> pw_parts(bool change_form, const vector<string>* failed);   // Defined below, in the password section.

// Cookie handling for every reply that sets no Set-Cookie itself (e.g. login/logout): an expired session removes the cookie;
// a logged-in user gets the cookie again with a new Max-Age (see the idle timeout below).
static string with_cookie_fix(const Request& req, const string& extra) {
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
static string flash_box(const string& kind, const string& message_html) {
    return render("flash", {{"kind", kind}, {"message", message_html}});
}

// Same, but from text (may be user/DB text): the engine escapes it.
static string flash_text(const string& kind, const string& text) {
    return render("flash_text", {{"kind", kind}, {"message", text}});
}

// Builds the full page from layout.html. head_extra = extra <head> tag (e.g. the /metrics refresh); flash = message box on top.
// interactive = popups + app.js (off for /metrics). allow_db = false for error pages and /metrics: they must never wait for the DB.
static string page(const Request& req, const string& title, const string& content, const string& head_extra = "",
                   const string& flash = "", bool interactive = true, bool allow_db = true) {
    string modals = (interactive && req.user_id == 0) ? render("modals", pw_parts(false, nullptr)) : "";
    string scripts = interactive ? render("scripts", {}) : "";
    return render("layout", {{"title", title}, {"head_extra", head_extra}, {"nav", nav_bar(req, allow_db)},
                             {"flash", flash}, {"content", content}, {"modals", modals}, {"scripts", scripts}});
}

static int send_page(SOCKET client, const Request& req, int status, const string& title, const string& content,
                     const string& extra = "", const string& flash = "") {
    send_response(client, status, HTML, page(req, title, content, "", flash), with_cookie_fix(req, extra));
    return status;
}

static string error_content(const string& msg, const string& detail_html = "") {
    return render("error", {{"message", msg}, {"detail", detail_html}});
}

static int error_page(SOCKET client, const Request& req, int status, const string& msg) {
    return send_page(client, req, status, msg, error_content(msg));
}

// For server.cpp's own error replies (400/408/503): a full page without login.
string render_error_page(const string& message) {
    Request anonymous;
    return page(anonymous, message, error_content(message), "", "", true, false);   // No DB: the accept thread must not wait.
}

// 302 redirect (Post/Redirect/Get). location is always one of our fixed paths, never user input.
static int redirect(SOCKET client, const Request& req, const string& location, const string& extra = "") {
    send_response(client, 302, HTML, render("plain", {{"message", "Redirecting..."}}),
                  "Location: " + location + "\r\n" + with_cookie_fix(req, extra));
    return 302;
}

// 405 with an Allow header.
static int method_not_allowed(SOCKET client, const Request& req, const string& allow) {
    send_response(client, 405, HTML, page(req, "Method Not Allowed", error_content("Method Not Allowed")),
                  with_cookie_fix(req, "Allow: " + allow + "\r\n"));
    return 405;
}

// "499.00" -> 49900 (paise): money is added as integers, not floats (price is DECIMAL(10,2)).
static long long to_cents(const string& price) {
    long long v = 0;
    for (char c : price)
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
    return v;
}

// Formats paise as rupees with a thousands comma: 149900 -> 1,499 and 103350 -> 1,033.50 (paise shown only if not 00).
static string rupees(long long cents) {
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
static string rupees_text(const string& price) {
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

static string category_emoji(const string& name) {
    for (const CategoryEmoji& c : CATEGORY_EMOJIS)
        if (name == c.name) return c.emoji;
    return "🛍️";
}

// A colour (0-359) from the category name: the same category always gets the same colour.
static int thumb_hue(const string& category) {
    unsigned int sum = 0;
    for (unsigned char c : category) sum = sum * 31 + c;
    return (int)(sum % 360);
}

// CSS class from a status text: lower-case letters only ("Logged out" -> "loggedout"). The text comes from the DB, so it is cleaned.
static string status_class(const string& status) {
    string out;
    for (unsigned char c : status)
        if (isalpha(c)) out += (char)tolower(c);
    return out.empty() ? "other" : out;
}

// Display name of an order status (the DB keeps PLACED, OUT_FOR_DELIVERY ...). Unknown statuses are shown as they are.
static string status_label(const string& status) {
    if (status == "PLACED") return "Ordered";
    if (status == "PACKED") return "Packed";
    if (status == "SHIPPED") return "Shipped";
    if (status == "OUT_FOR_DELIVERY") return "Out for delivery";
    if (status == "DELIVERED") return "Delivered";
    if (status == "CANCELLED") return "Cancelled";
    return status;
}

// Stock badge: "In stock" (green) or "Only N left" up to LOW_STOCK (orange). Nothing at 0: the button is disabled there.
static string stock_html(const string& stock) {
    int n = atoi(stock.c_str());
    if (n <= 0) return "";
    if (n <= LOW_STOCK) return render("stock_low", {{"stock", to_string(n)}});
    return render("stock_in", {{"stock", to_string(n)}});      // The badge only says "In stock"; the exact count is in the tooltip.
}

// Digits only, 1 to 9 of them.
static bool is_number(const string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Stop % _ and ! from having their LIKE meaning (we use ESCAPE '!').
static string escape_like(const string& s) {
    string out;
    for (char c : s) {
        if (c == '!' || c == '%' || c == '_') out += '!';
        out += c;
    }
    return out;
}

static string param(const Request& req, const string& key) {
    auto it = req.params.find(key);
    return it == req.params.end() ? "" : it->second;
}

static string field(const Request& req, const string& key) {
    auto it = req.form.find(key);
    return it == req.form.end() ? "" : it->second;
}

// Does every row have at least n columns? (true for no rows; check rows.empty() separately before rows[0].)
// This keeps rows[i][j] from ever going out of range.
static bool cols_ok(const vector<vector<string>>& rows, size_t n) {
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

static vector<vector<string>> get_categories(bool allow_db) {
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

// ---- sessions ----

// 32 random bytes -> 64 hex characters. Not rand(): it is guessable; BCryptGenRandom is not.
static bool new_session_id(string& out) {
    unsigned char raw[32];
    if (BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
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
static string session_cookie(const string& value, int max_age) {
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

// ---- forms ----


// ---- password policy ----
// The rules live only in check_password; app.js just shows them live. No DB CHECK: they apply to typed passwords, not hashes.
static const char* COMMON_PW[] = {"password@123", "admin@123", "qwerty@123", "welcome@123", "abcd@1234", "password@1234", "password1!",
                                  "passw0rd!", "p@ssw0rd", "p@ssword1", "admin@1234", "admin@12345", "qwerty@1234", "welcome@1",
                                  "welcome@1234", "test@1234", "india@123", "india@1234", "pass@1234", "abc@12345", "abc@1234",
                                  "letmein@123", "changeme@123", "iloveyou@123", "user@1234", "master@123", "monkey@123",
                                  "dragon@123", "login@1234", "hello@1234"};

static string lower_ascii(string t) {
    for (char& c : t) c = (char)tolower((unsigned char)c);
    return t;
}

// Counts characters, not bytes: UTF-8 continuation bytes (10xxxxxx) are skipped.
static size_t pw_chars(const string& t) {
    size_t n = 0;
    for (unsigned char c : t)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

// Failed rules as (rule key, message), in rule order (the form checklist uses the same order; the key is its data-rule).
// Upper/lower/digit/special count ASCII only (special = printable, not a letter or digit).
static vector<pair<string, string>> pw_failures(const string& pw, const string& name, const string& email, const string& old_pw) {
    vector<pair<string, string>> bad;

    size_t len = pw_chars(pw);
    if (len < (size_t)PW_MIN_LEN || len > (size_t)PW_MAX_LEN)
        bad.push_back({"len", "Password must be " + to_string(PW_MIN_LEN) + " to " + to_string(PW_MAX_LEN) + " characters long."});

    bool space = false, upper = false, lower = false, digit = false, special = false;
    for (unsigned char c : pw) {
        if (c <= 32 || c == 127) space = true;                  // Space, tab, newline and other control characters.
        else if (c >= 'A' && c <= 'Z') upper = true;
        else if (c >= 'a' && c <= 'z') lower = true;
        else if (c >= '0' && c <= '9') digit = true;
        else if (c < 127) special = true;                       // 33..126 that is not a letter or digit.
    }
    if (space) bad.push_back({"space", "Password cannot contain spaces."});
    if (!upper) bad.push_back({"upper", "Password needs at least one uppercase letter (A-Z)."});
    if (!lower) bad.push_back({"lower", "Password needs at least one lowercase letter (a-z)."});
    if (!digit) bad.push_back({"digit", "Password needs at least one number (0-9)."});
    if (!special) bad.push_back({"special", "Password needs at least one special character (like @ # $ ! %)."});

    // Name or email part (case-insensitive), only pieces of 3+ characters (a short name like "al" would block every password):
    // the full name (spaces removed), each word of the name, and the part of the email before @.
    string low = lower_ascii(pw);
    vector<string> bits;
    string joined, word;
    for (char c : lower_ascii(name) + " ") {
        if (c == ' ' || c == '\t') {
            if (word.size() >= 3) bits.push_back(word);
            joined += word;
            word = "";
        } else {
            word += c;
        }
    }
    if (joined.size() >= 3) bits.push_back(joined);
    string local = lower_ascii(email.substr(0, email.find('@')));
    if (local.size() >= 3) bits.push_back(local);
    for (auto& b : bits)
        if (low.find(b) != string::npos) {
            bad.push_back({"personal", "Password cannot contain your name or the first part of your email."});
            break;
        }

    for (const char* c : COMMON_PW)
        if (low == c) {
            bad.push_back({"common", "That password is too common. Please choose a less guessable one."});
            break;
        }

    if (!old_pw.empty() && pw == old_pw) bad.push_back({"different", "New password must be different from your current password."});
    return bad;
}

// Messages of the failed rules (empty = password OK).
vector<string> check_password(const string& pw, const string& name, const string& email, const string& old_pw) {
    vector<string> msgs;
    for (auto& f : pw_failures(pw, name, email, old_pw)) msgs.push_back(f.second);
    return msgs;
}

// Password input (the eye button only shows when JavaScript runs). kind: new / confirm / old (used by app.js).
static string pw_field_html(const string& name, const string& kind, const string& autocomplete, const string& placeholder, int max_len) {
    return render("pw_field", {{"name", name}, {"kind", kind}, {"auto", autocomplete}, {"ph", placeholder}, {"max", to_string(max_len)}});
}

// Rules checklist: hidden by default (css .pw-rules); app.js shows it on focus/typing and marks each rule live. data-common gives app.js the same common-password list.
// failed != nullptr (the server found a problem): the list is open and only the FAILED rules show (the rest are .pw-passed). Without JavaScript it stays so.
struct PwRule {
    const char* key;
    string label;
};

static string pw_rules_html(bool with_different, const vector<string>* failed) {
    string common;
    for (const char* c : COMMON_PW) common += (common.empty() ? "" : "|") + string(c);

    vector<PwRule> rules = {{"len", to_string(PW_MIN_LEN) + " to " + to_string(PW_MAX_LEN) + " characters"},
                            {"space", "No spaces"},
                            {"upper", "One uppercase letter (A-Z)"},
                            {"lower", "One lowercase letter (a-z)"},
                            {"digit", "One number (0-9)"},
                            {"special", "One special character (like @ # $ ! %)"},
                            {"personal", "Does not contain your name or email name"},
                            {"common", "Not a common password"}};
    if (with_different) rules.push_back({"different", "Different from your current password"});
    rules.push_back({"match", "Both passwords match"});

    string items;
    for (auto& r : rules) {
        bool bad = true, passed = false;
        if (failed != nullptr) {
            bad = false;
            for (auto& f : *failed)
                if (f == r.key) bad = true;
            passed = !bad;
        }
        items += render("pw_rule_item", {{"key", r.key}, {"cls", passed ? "pw-passed" : "bad"}, {"mark", passed ? "\u2713" : "\u2717"}, {"label", r.label}});
    }
    return render("pw_rules", {{"min", to_string(PW_MIN_LEN)}, {"max", to_string(PW_MAX_LEN)}, {"common", common},
                               {"state", failed != nullptr ? " server" : ""}, {"items", items}});
}

// Password parts of the signup page and popup (password, confirm, checklist). change_form = /account/password.
// failed = rule keys the server found failing; nullptr = no problem (the list stays hidden).
static map<string, string> pw_parts(bool change_form, const vector<string>* failed) {
    return {{"password_field", pw_field_html(change_form ? "new_password" : "password", "new", "new-password", change_form ? "" : "Password", PW_MAX_LEN)},
            {"confirm_field", pw_field_html("confirm_password", "confirm", "new-password", change_form ? "" : "Confirm password", PW_MAX_LEN)},
            {"rules", pw_rules_html(change_form, failed)}};
}

// Red error box: plain text for one simple error (as before), a list for several.
static string errors_flash(const vector<string>& errs, bool as_list) {
    if (!as_list && errs.size() == 1) return flash_text("err", errs[0]);
    string items;
    for (auto& e : errs) items += render("error_list_item", {{"text", e}});
    return flash_box("err", render("error_list", {{"items", items}}));
}

static string signup_form(const string& name, const string& email, const vector<string>* failed = nullptr) {
    map<string, string> v = pw_parts(false, failed); // Password fields are never refilled: only name and email come back.
    v["name"] = name;
    v["email"] = email;
    return render("signup", v);
}

static string login_form(const string& email) {
    return render("login", {{"email", email}});
}

// Simple email check: one '@', text on both sides, no spaces.
static bool email_ok(const string& e) {
    size_t at = e.find('@');
    if (at == string::npos || at == 0 || at + 1 >= e.size()) return false;
    if (e.find('@', at + 1) != string::npos) return false;
    return e.find_first_of(" \t\r\n") == string::npos;
}

// Checks the signup name/email: returns a message or "" (the password rules are in check_password).
static string signup_error(const string& name, const string& email) {
    if (name.empty() || email.empty()) return "Please fill in all fields.";
    if (name.size() > 100) return "Name is too long (max 100 characters).";
    if (email.size() > 100) return "Email is too long (max 100 characters).";
    if (!email_ok(email)) return "Please enter a valid email address.";
    return "";
}

// ---- auth pages ----

static int signup_page(SOCKET client, const Request& req) {
    if (req.method == "GET") return send_page(client, req, 200, "Sign up", signup_form("", ""));
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    string name = trim(field(req, "name"));
    string email = trim(field(req, "email"));
    string pass = field(req, "password");
    string confirm = field(req, "confirm_password");

    // All errors at once (so the user need not fix them one by one): name/email, passwords match, failed rules.
    vector<string> errs;
    vector<string> failed;                  // Keys of the failed password rules: the form list shows only these.
    bool pw_problem = false;                // Any password-related error is always shown as a list.
    if (pass.empty() || confirm.empty()) {
        string e = signup_error(name, email);
        errs.push_back(e.empty() ? "Please fill in all fields." : e);
    } else {
        string e = signup_error(name, email);
        if (!e.empty()) errs.push_back(e);
        if (pass != confirm) { errs.push_back("The two passwords do not match."); failed.push_back("match"); pw_problem = true; }
        for (auto& r : pw_failures(pass, name, email, "")) { errs.push_back(r.second); failed.push_back(r.first); pw_problem = true; }
    }
    if (!errs.empty())
        return send_page(client, req, 400, "Sign up", signup_form(name, email, pw_problem ? &failed : nullptr), "", errors_flash(errs, pw_problem));

    // The password is plain text for now (known limitation; hashing comes later).
    int code = 0;
    int n = db_exec("INSERT INTO users (name, email, password) VALUES (?, ?, ?)", {name, email, pass}, &code);
    if (n < 0) {
        if (code == 1062)   // Duplicate entry: the email already exists (UNIQUE).
            return send_page(client, req, 409, "Sign up", signup_form(name, email), "",
                             flash_text("err", "Email already registered."));
        return error_page(client, req, 500, "Database error");
    }
    if (n != 1) return error_page(client, req, 500, "Could not create account");   // Exactly one row must have been added.
    log_tag("PAGE", "new user signed up");
    return redirect(client, req, "/login?registered=1");
}

static int login_page(SOCKET client, const Request& req) {
    if (req.method == "GET") {
        string ok = (param(req, "registered") == "1") ? flash_text("ok", "Account created. Please log in.") : "";
        return send_page(client, req, 200, "Login", login_form(""), "", ok);
    }
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    string email = trim(field(req, "email"));
    string pass = field(req, "password");
    const string bad = "Invalid email or password.";   // Same message for a wrong email or a wrong password.

    // BINARY: the password match is case-sensitive (a normal compare is not).
    vector<vector<string>> rows;
    if (!db_select("SELECT user_id FROM users WHERE email = ? AND BINARY password = ?", {email, pass}, rows))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) {
        log_tag("PAGE", "login failed");
        return send_page(client, req, 401, "Login", login_form(email), "", flash_text("err", bad));
    }
    if (!cols_ok(rows, 1)) return error_page(client, req, 500, "Database error");
    string uid = rows[0][0];             // Password rules are never checked at login: old passwords keep working.

    string sid;
    if (!new_session_id(sid)) return error_page(client, req, 500, "Could not create session");

    // expires_at = now + SESSION_MINUTES (it moves on every request: idle timeout, see load_user_on).
    // If the INSERT fails (or adds no row) no cookie is set: the browser would hold a session id that is not in the DB.
    int n = db_exec("INSERT INTO user_sessions (session_id, user_id, expires_at) VALUES (?, ?, " + EXPIRES_SQL + ")",
                    {sid, uid});
    if (n != 1) {
        log_tag("PAGE", "login: could not store session for user " + uid);
        return error_page(client, req, 500, "Could not log you in. Please try again.");
    }

    log_tag("PAGE", "user " + uid + " logged in");
    return redirect(client, req, "/products", session_cookie(sid, SESSION_MINUTES * 60));
}

// ---- /account/password ----

static string change_pw_form(const Request& req, const string& email, const vector<string>* failed = nullptr) {
    map<string, string> v = pw_parts(true, failed);
    v["current_field"] = pw_field_html("current_password", "old", "current-password", "", 255);   // The old password may be longer than 64.
    v["name"] = req.user_name;
    v["email"] = email;
    return render("change_password", v);
}

// GET = form, POST = change. Login required; the old password must be right (password_change in db.cpp checks it).
static int account_password_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET" && req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    vector<vector<string>> rows;
    if (!db_select("SELECT email FROM users WHERE user_id = ?", {to_string(req.user_id)}, rows) || !cols_ok(rows, 1))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) return error_page(client, req, 404, "User not found");
    const string email = rows[0][0];

    if (req.method == "GET") {
        string flash;
        if (param(req, "ok") == "1") flash = flash_text("ok", "Password changed");
        return send_page(client, req, 200, "Change password", change_pw_form(req, email), "", flash);
    }

    string cur = field(req, "current_password");
    string pw = field(req, "new_password");
    string confirm = field(req, "confirm_password");
    vector<string> errs, failed;
    if (cur.empty()) errs.push_back("Please enter your current password.");
    if (pw.empty() || confirm.empty()) errs.push_back("Please enter the new password twice.");
    else {
        if (pw != confirm) { errs.push_back("The two new passwords do not match."); failed.push_back("match"); }
        for (auto& r : pw_failures(pw, req.user_name, email, cur)) { errs.push_back(r.second); failed.push_back(r.first); }
    }
    if (!errs.empty())
        return send_page(client, req, 400, "Change password", change_pw_form(req, email, failed.empty() ? nullptr : &failed), "", errors_flash(errs, true));

    PasswordResult r = password_change(req.user_id, cur, pw, req.session_id);
    if (r.status == 200) {
        log_tag("PAGE", "user " + to_string(req.user_id) + " changed the password, " + to_string(r.ended) + " other sessions ended");
        return redirect(client, req, "/account/password?ok=1");
    }
    if (r.status == 403) return send_page(client, req, 403, "Change password", change_pw_form(req, email), "", flash_text("err", r.message));
    return error_page(client, req, r.status == 503 ? 503 : 500, r.status == 503 ? r.message : "Database error");
}

// /logout is POST only: end the session, remove the cookie, go to /products.
static int logout_page(SOCKET client, const Request& req) {
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    if (req.user_id > 0) {
        // On a DB error keep the cookie: the session is still active in the DB, so showing "logged out" would be wrong.
        if (db_exec("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'LOGOUT' "
                    "WHERE session_id = ?",
                    {req.session_id}) < 0)
            return error_page(client, req, 500, "Could not log you out. Please try again.");
        log_tag("PAGE", "user " + to_string(req.user_id) + " logged out");
    }
    return redirect(client, req, "/products", session_cookie("", 0));
}

// For the cleanup thread: close all expired but still active sessions in one UPDATE.
// logout_time = expires_at (the real expiry time, not the cleanup time). Returns how many were closed.
int close_expired_sessions() {
    return db_exec("UPDATE user_sessions SET is_active = 0, logout_time = expires_at, end_reason = 'EXPIRED' "
                   "WHERE is_active = 1 AND expires_at < NOW()", {});
}

// Status text: Active / Logged out / Expired.
static string session_status(const string& is_active, const string& expired, const string& end_reason) {
    if (is_active == "1") return expired == "1" ? "Expired" : "Active";   // Expired but not closed yet.
    if (end_reason == "FORCED") return "Ended by admin";                  // Ended by an admin (Admin > Sessions).
    return end_reason == "EXPIRED" ? "Expired" : "Logged out";            // LOGOUT, or an old row (NULL).
}

// /sessions: this user's login history, the current session highlighted.
static int sessions_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");

    vector<vector<string>> rows;
    if (!db_select("SELECT session_id, login_time, last_activity, logout_time, is_active, expires_at <= NOW(), "
                   "end_reason FROM user_sessions WHERE user_id = ? ORDER BY login_time DESC LIMIT 50",
                   {to_string(req.user_id)}, rows))
        return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 7)) return error_page(client, req, 500, "Database error");

    string body;
    for (auto& r : rows) {
        bool current = (r[0] == req.session_id);
        // The full session id is never shown, only its first 8 characters.
        string status = session_status(r[4], r[5], r[6]);
        body += render("session_row", {{"row_class", current ? "me" : ""},
                                       {"session", r[0].substr(0, 8)},
                                       {"login", r[1]},
                                       {"activity", r[2]},
                                       {"logout", r[3].empty() ? "-" : r[3]},
                                       {"status", status},
                                       {"status_class", status_class(status)},
                                       {"current", current ? " (this session)" : ""}});
    }
    return send_page(client, req, 200, "My Sessions", render("sessions", {{"rows", body}}));
}

// ---- shop pages ----

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
static string strip_param(const string& target, const string& name) {
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
static string return_target(const Request& req) {
    return strip_param(strip_param(req.target, "added"), "cart_err");
}

// Stops an OPEN REDIRECT: "return" comes from the form, so anyone could send users to another site. Only our own paths pass:
// start with "/" but not "//", no backslash, no space or control characters (\r \n could inject a header); otherwise /products.
static string safe_return(const string& r) {
    const string fallback = "/products";
    if (r.empty() || r.size() > 1000 || r[0] != '/') return fallback;
    if (r.size() > 1 && r[1] == '/') return fallback;
    for (char c : r)
        if (c == '\\' || c <= 32 || c == 127) return fallback;   // c <= 32 also catches non-ASCII bytes.
    return r;
}

// With ?added=<product id> (and optional cart_err) show a message: green (added) or red (problem). cart_err=stock -> "Only N left", out -> "out of stock".
// The texts are fixed; the product name comes from the DB and is escaped; added is used only if it is a number.
static string cart_notice(const Request& req) {
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

// Product photo (public/images/products/<id>.jpg) if it exists, otherwise the category emoji (for products added in admin).
// find_static looks in the in-memory map, so there is no disk read here. The id is a DB number.
static string product_thumb(const string& id, const string& category) {
    string src = "images/products/" + id + ".jpg";
    if (find_static(src) != nullptr) return render("thumb_photo", {{"src", src}});
    return render("thumb_emoji", {{"emoji", category_emoji(category)}});
}

// One product card (used everywhere): photo or emoji, name, description, price, stock badge, button. back = this page's path + query.
// Button: logged in = add to cart (POST), guest = sign in (popup), stock 0 = disabled. The server refuses it too (cart_add_steps); price and cart_id never come from the form.
static string product_card(const vector<string>& p, const string& back, bool logged_in) {
    bool sold_out = (p[4] == "0");
    string action = sold_out ? render("soldout", {})
                  : logged_in ? render("add_form_card", {{"id", p[0]}, {"back", back}})
                              : render("signin_buy", {});
    return render("product_card", {{"hue", to_string(thumb_hue(p[2]))}, {"thumb", product_thumb(p[0], p[2])},
                                   {"id", p[0]}, {"name", p[1]}, {"description", p[5]}, {"price", rupees_text(p[3])},
                                   {"stock", stock_html(p[4])}, {"action", action}});
}

// /products?cat=ID&q=text&min=N&max=N (navbar search, sidebar category and price filter, the "Under 1,000" link).
static int products_page(SOCKET client, const Request& req) {
    string cat = param(req, "cat");
    string q = param(req, "q");
    string min_price = param(req, "min");
    string max_price = param(req, "max");
    if (!cat.empty() && !is_number(cat)) return error_page(client, req, 400, "Bad category id");
    if ((!min_price.empty() && !is_number(min_price)) || (!max_price.empty() && !is_number(max_price)))
        return error_page(client, req, 400, "Bad price filter");

    vector<vector<string>> cats = get_categories(true);

    // SQL pieces are fixed; user data goes only into args (the ?): category, name, min and max price.
    string sql = "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description "
                 "FROM products p JOIN categories c ON c.category_id = p.category_id WHERE p.is_active = TRUE";
    vector<string> args;
    if (!cat.empty()) {
        sql += " AND p.category_id = ?";
        args.push_back(cat);
    }
    if (!q.empty()) {
        sql += " AND p.name LIKE ? ESCAPE '!'";
        args.push_back("%" + escape_like(q) + "%");
    }
    if (!min_price.empty()) {
        sql += " AND p.price >= ?";
        args.push_back(min_price);
    }
    if (!max_price.empty()) {
        sql += " AND p.price <= ?";
        args.push_back(max_price);
    }
    sql += " ORDER BY p.name LIMIT 200";

    vector<vector<string>> prods;
    if (!db_select(sql, args, prods)) return error_page(client, req, 500, "Database error");
    if (!cols_ok(prods, 6)) return error_page(client, req, 500, "Database error");

    string cards;
    string back = return_target(req);
    for (auto& p : prods) cards += product_card(p, back, req.user_id > 0);

    // Sidebar: All + each category (the chosen one "on"), and a price form that keeps cat / q as hidden fields.
    string side = render("side_cat", {{"on_class", cat.empty() ? " on" : ""}, {"href", "/products"},
                                      {"emoji", "\xF0\x9F\x9B\x8D\xEF\xB8\x8F"}, {"label", "All"}});
    string heading = "All products";
    for (auto& c : cats) {
        bool on = (c[0] == cat);
        side += render("side_cat", {{"on_class", on ? " on" : ""}, {"href", "/products?cat=" + c[0]},
                                    {"emoji", category_emoji(c[1])}, {"label", c[1]}});
        if (on) heading = category_emoji(c[1]) + " " + c[1];
    }
    string hidden;
    if (!cat.empty()) hidden += render("hidden_input", {{"name", "cat"}, {"value", cat}});
    if (!q.empty()) hidden += render("hidden_input", {{"name", "q"}, {"value", q}});

    string count = to_string(prods.size()) + (prods.size() == 1 ? " product" : " products");
    string note;
    if (!q.empty()) note += " matching \"" + q + "\"";
    if (!min_price.empty()) note += ", from " + rupees(atoll(min_price.c_str()) * 100);
    if (!max_price.empty()) note += ", up to " + rupees(atoll(max_price.c_str()) * 100);

    string content = render("products", {{"side_cats", side}, {"hidden_fields", hidden}, {"min", min_price},
                                         {"max", max_price}, {"heading", heading}, {"count", count}, {"note", note},
                                         {"empty_message", prods.empty() ? render("no_products", {}) : ""},
                                         {"cards", cards}});
    return send_page(client, req, 200, "Products", content, "", cart_notice(req));
}

// Home page: hero, 4 tiles, "Shop by category" and "Budget picks under 1,000" (same card).
// Hero and tiles show even if the DB is down (only the category and budget parts are missing): home never gives 500.
static int home_page(SOCKET client, const Request& req) {
    vector<vector<string>> cats = get_categories(true);
    vector<vector<string>> budget;
    if (!db_select(BUDGET_SQL, {}, budget) || !cols_ok(budget, 6)) {
        log_tag("PAGE", "home: could not load the budget picks");
        budget.clear();
    }

    string tiles;
    for (auto& c : cats)
        tiles += render("category_tile", {{"id", c[0]}, {"emoji", category_emoji(c[1])}, {"name", c[1]}});

    string cards;
    string back = return_target(req);       // "Add to cart" returns here (to home).
    for (auto& p : budget) cards += product_card(p, back, req.user_id > 0);

    string budget_text = rupees((long long)BUDGET_MAX * 100);
    string content = render("home", {
        {"hero_second", req.user_id > 0 ? render("hero_orders", {}) : render("hero_create", {})},
        {"categories_section", cats.empty() ? "" : render("home_categories", {{"tiles", tiles}})},
        {"budget_section", budget.empty() ? "" : render("home_budget", {{"cards", cards}, {"budget", budget_text},
                                                                         {"budget_max", to_string(BUDGET_MAX)}})}});
    return send_page(client, req, 200, "Home", content, "", cart_notice(req));
}

// /product?id=N
static int product_page(SOCKET client, const Request& req) {
    string id = param(req, "id");
    if (!is_number(id)) return error_page(client, req, 400, "Bad product id");

    vector<vector<string>> rows;
    if (!db_select("SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description, p.category_id "
                   "FROM products p JOIN categories c ON c.category_id = p.category_id "
                   "WHERE p.product_id = ? AND p.is_active = TRUE", {id}, rows))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) return error_page(client, req, 404, "Product not found");
    if (!cols_ok(rows, 7)) return error_page(client, req, 500, "Database error");

    vector<string>& p = rows[0];
    bool sold_out = (p[4] == "0");
    string action = sold_out ? render("soldout", {})
                  : req.user_id > 0 ? render("add_form_page", {{"id", p[0]}, {"back", return_target(req)},
                                                                {"max", to_string(MAX_CART_QTY)}})
                                    : render("signin_buy", {});
    string content = render("product", {{"hue", to_string(thumb_hue(p[2]))}, {"thumb", product_thumb(p[0], p[2])},
                                        {"name", p[1]}, {"category", p[2]}, {"cat_id", p[6]}, {"description", p[5]},
                                        {"price", rupees_text(p[3])}, {"stock", stock_html(p[4])}, {"action", action}});
    return send_page(client, req, 200, p[1], content, "", cart_notice(req));
}

// ---- cart: every cart page needs login; cart_id always comes from the session user, never from the form ----
// The price always comes from the DB. Only product_id and qty are read from the form.

// Is the text a number between lo and hi? (out gets the value)
static bool parse_range(const string& s, int lo, int hi, int& out) {
    if (!is_number(s)) return false;
    int v = atoi(s.c_str());
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

// Valid product id? (a number, 1 or more)
static bool valid_product_id(const string& s) {
    int v = 0;
    return parse_range(s, 1, 999999999, v);
}

// Reads the cart items (empty if there is no cart). One connection, and put_conn always runs (a single return point).
static bool load_cart_rows(int user_id, vector<vector<string>>& rows) {
    rows.clear();
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return false;
    bool ok = true;
    int cart_id = find_active_cart(m, user_id);
    if (cart_id > 0) ok = cart_items(m, cart_id, rows);
    put_conn(id);
    return ok;
}

// One item row: name, price, qty (update form), subtotal, remove button, stock warning.
static string cart_row(const vector<string>& r, long long& total) {
    int qty = atoi(r[3].c_str());
    int stock = atoi(r[4].c_str());
    long long sub = to_cents(r[2]) * qty;
    total += sub;

    string warn;
    if (r.size() > 5 && r[5] == "0") warn = render("stock_warn", {{"message", "No longer available"}});   // Hidden by the admin.
    else if (stock == 0) warn = render("stock_warn", {{"message", "Out of stock"}});
    else if (qty > stock) warn = render("stock_warn", {{"message", "Only " + to_string(stock) + " left"}});

    return render("cart_row", {{"id", r[0]}, {"name", r[1]}, {"warn", warn}, {"price", rupees_text(r[2])},
                               {"qty", to_string(qty)}, {"max", to_string(MAX_CART_QTY)}, {"subtotal", rupees(sub)}});
}

// GET /cart
static int cart_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    vector<vector<string>> rows;
    if (!load_cart_rows(req.user_id, rows)) return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 5)) return error_page(client, req, 500, "Database error");

    if (rows.empty())
        return send_page(client, req, 200, "Cart", render("heading", {{"text", "Your cart"}}) +
                                                    render("empty_state", {{"message", "Your cart is empty."}}));

    long long total = 0;
    string body;
    for (auto& r : rows) body += cart_row(r, total);
    return send_page(client, req, 200, "Cart", render("cart", {{"rows", body}, {"total", rupees(total)}}));
}

// The work inside cart/add (m is given; get/put is outside). Returns the status; a problem goes in msg.
// problem (with 200): "" = fine, "out" = stock was 0 (not added), "stock" = added but the cart quantity is now above stock (red message).
static int cart_add_steps(MYSQL* m, int user_id, const string& pid, const string& qty, string& msg,
                          string& problem) {
    problem = "";
    int cart_id = get_active_cart(m, user_id);
    if (cart_id == 0) { msg = "Database error"; return 500; }

    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT stock FROM products WHERE product_id = ? AND is_active = TRUE", {pid}, rows) || !cols_ok(rows, 1)) {
        msg = "Database error";
        return 500;
    }
    if (rows.empty()) { msg = "Product not found"; return 404; }
    int stock = atoi(rows[0][0].c_str());
    if (stock == 0) { problem = "out"; return 200; }

    // Quantity in the cart before the add (0 = not there): tells whether MAX_CART_QTY cut something.
    // Only for the message; the real add is one atomic query below, so a race cannot hurt here.
    int before = 0;
    if (db_select_on(m, "SELECT quantity FROM cart_items WHERE cart_id = ? AND product_id = ?",
                     {to_string(cart_id), pid}, rows) && !rows.empty() && !rows[0].empty())
        before = atoi(rows[0][0].c_str());

    if (!cart_add(m, cart_id, pid, qty)) { msg = "Database error"; return 500; }

    // Above MAX_CART_QTY the cap applied: show "Maximum N per item", not "added".
    if (before + atoi(qty.c_str()) > MAX_CART_QTY) { problem = "max"; return 200; }

    // The cart may hold more than the stock (stock can change later): just tell the user.
    // (The new quantity comes from a SELECT: after the add, that is the truth the atomic query made.)
    if (db_select_on(m, "SELECT quantity FROM cart_items WHERE cart_id = ? AND product_id = ?",
                     {to_string(cart_id), pid}, rows) &&
        !rows.empty() && !rows[0].empty() && atoi(rows[0][0].c_str()) > stock)
        problem = "stock";
    return 200;
}

// POST /cart/add (product_id, qty: default 1, 1..MAX_CART_QTY)
static int cart_add_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    string pid = field(req, "product_id");
    string qty = field(req, "qty");
    if (qty.empty()) qty = "1";
    int q = 0;
    if (!valid_product_id(pid)) return error_page(client, req, 400, "Invalid product id");
    if (!parse_range(qty, 1, MAX_CART_QTY, q))
        return error_page(client, req, 400, "Quantity must be a number from 1 to " + to_string(MAX_CART_QTY));
    qty = to_string(q);                 // "007" becomes a clean number.

    string msg = "Database error";
    string problem;
    int status = 500;
    int id = 0;
    MYSQL* m = get_conn(id);            // All queries of this request use this one connection.
    if (m != nullptr) {
        status = cart_add_steps(m, req.user_id, pid, qty, msg, problem);
        put_conn(id);                   // The connection goes back whatever path the steps took.
    }
    if (status != 200) return error_page(client, req, status, msg);

    // Post/Redirect/Get: back to the page where the button was pressed, with ?added=<id>.
    // "return" goes through safe_return (no open redirect); old added/cart_err are removed, new ones added.
    string back = strip_param(strip_param(safe_return(field(req, "return")), "added"), "cart_err");
    back += (back.find('?') == string::npos ? "?" : "&") + string("added=") + pid;
    if (!problem.empty()) back += "&cart_err=" + problem;
    return redirect(client, req, back);
}

static int cart_update_steps(MYSQL* m, int user_id, const string& pid, const string& qty, string& msg) {
    int cart_id = get_active_cart(m, user_id);
    if (cart_id == 0) { msg = "Database error"; return 500; }
    int n = cart_set_qty(m, cart_id, pid, qty);     // qty "0" removes the item.
    if (n < 0) { msg = "Database error"; return 500; }
    if (n == 0) { msg = "That item is not in your cart"; return 404; }
    return 200;
}

// POST /cart/update (product_id, qty: 0..MAX_CART_QTY, 0 = remove).
static int cart_update_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    string pid = field(req, "product_id");
    string qty = field(req, "qty");
    int q = 0;
    if (!valid_product_id(pid)) return error_page(client, req, 400, "Invalid product id");
    if (!parse_range(qty, 0, MAX_CART_QTY, q))
        return error_page(client, req, 400, "Quantity must be a number from 0 to " + to_string(MAX_CART_QTY));
    qty = to_string(q);

    string msg = "Database error";
    int status = 500;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m != nullptr) {
        status = cart_update_steps(m, req.user_id, pid, qty, msg);
        put_conn(id);
    }
    if (status != 200) return error_page(client, req, status, msg);
    return redirect(client, req, "/cart");
}

static int cart_remove_steps(MYSQL* m, int user_id, const string& pid, string& msg) {
    int cart_id = find_active_cart(m, user_id);
    if (cart_id == 0) return 200;                   // No cart at all: nothing to remove.
    if (cart_remove(m, cart_id, pid) < 0) { msg = "Database error"; return 500; }
    return 200;                                     // The item was not there (e.g. a double click): fine.
}

// POST /cart/remove (product_id)
static int cart_remove_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    string pid = field(req, "product_id");
    if (!valid_product_id(pid)) return error_page(client, req, 400, "Invalid product id");

    string msg = "Database error";
    int status = 500;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m != nullptr) {
        status = cart_remove_steps(m, req.user_id, pid, msg);
        put_conn(id);
    }
    if (status != 200) return error_page(client, req, status, msg);
    return redirect(client, req, "/cart");
}

// ---- shipping addresses ----

// The 10 address text values r[o..o+9]: first, last, phone, line1, line2, landmark, city, state_name, pincode, type.
// Used by the address card, checkout, order page and admin order page (the order page uses the order_addresses copy).
static string address_html(const vector<string>& r, size_t o) {
    string extra;                       // line2 and landmark: no line at all if empty.
    if (!r[o + 4].empty()) extra += render("address_extra", {{"text", r[o + 4]}});
    if (!r[o + 5].empty()) extra += render("address_extra", {{"text", "Landmark: " + r[o + 5]}});
    string type = r[o + 9];             // "HOME" -> "Home".
    for (size_t i = 0; i < type.size(); i++) type[i] = (i == 0) ? (char)toupper((unsigned char)type[i]) : (char)tolower((unsigned char)type[i]);
    return render("address_text", {{"name", r[o] + " " + r[o + 1]}, {"type", type}, {"type_class", status_class(r[o + 9])},
                                   {"line1", r[o + 3]}, {"extra", extra}, {"city", r[o + 6]}, {"state", r[o + 7]},
                                   {"pincode", r[o + 8]}, {"phone", r[o + 2]}});
}

// The user's active addresses (default first). Columns: address_id, 10 text values (for address_html), is_default.
static bool load_addresses(int user_id, vector<vector<string>>& rows) {
    if (!db_select("SELECT a.address_id, a.first_name, a.last_name, a.phone, a.line1, IFNULL(a.line2, ''), IFNULL(a.landmark, ''), "
                   "a.city, s.state_name, a.pincode, a.address_type, a.is_default FROM addresses a "
                   "JOIN states s ON s.state_id = a.state_id WHERE a.user_id = ? AND a.is_active = TRUE "
                   "ORDER BY a.is_default DESC, a.address_id", {to_string(user_id)}, rows))
        return false;
    return cols_ok(rows, 12);
}

// An order's shipping address (the order_addresses copy) for the order pages. An old order shows "No address".
static bool order_address_text(const string& order_id, string& out) {
    vector<vector<string>> rows;
    if (!db_select("SELECT first_name, last_name, phone, line1, IFNULL(line2, ''), IFNULL(landmark, ''), city, state_name, pincode, "
                   "address_type FROM order_addresses WHERE order_id = ?", {order_id}, rows) || !cols_ok(rows, 10))
        return false;
    out = rows.empty() ? render("address_none", {}) : address_html(rows[0], 0);
    return true;
}

// Message above /addresses?ok=... (fixed text).
static string address_notice(const Request& req) {
    string ok = param(req, "ok");
    if (ok == "added") return flash_text("ok", "Address added.");
    if (ok == "saved") return flash_text("ok", "Address saved.");
    if (ok == "deleted") return flash_text("ok", "Address deleted. Orders you already placed keep their address.");
    if (ok == "default") return flash_text("ok", "Default address changed.");
    return "";
}

// GET /addresses: all addresses as cards (Default badge, Edit, Delete, Set as default).
static int addresses_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    vector<vector<string>> rows;
    if (!load_addresses(req.user_id, rows)) return error_page(client, req, 500, "Database error");
    string content;
    if (rows.empty()) {
        content = render("heading", {{"text", "Your addresses"}}) + render("addresses_empty", {});
    } else {
        string cards;
        for (auto& r : rows) {
            bool def = (r[11] == "1");
            cards += render("address_card", {{"id", r[0]}, {"extra_class", def ? "addr-default" : ""},
                                             {"badge", def ? render("address_default_badge", {}) : ""},
                                             {"text", address_html(r, 1)},
                                             {"default_form", def ? "" : render("address_default_form", {{"id", r[0]}})}});
        }
        content = render("addresses", {{"count", to_string(rows.size())}, {"max", to_string(MAX_ADDRESSES)}, {"cards", cards}});
    }
    return send_page(client, req, 200, "Addresses", content, "", address_notice(req));
}

// Form fields from the request (address_check reports anything wrong or empty).
static Address read_address(const Request& req) {
    Address a;
    a.id = trim(field(req, "address_id"));
    a.first_name = field(req, "first_name");
    a.last_name = field(req, "last_name");
    a.phone = field(req, "phone");
    a.line1 = field(req, "line1");
    a.line2 = field(req, "line2");
    a.landmark = field(req, "landmark");
    a.city = field(req, "city");
    a.state_id = field(req, "state_id");
    a.pincode = field(req, "pincode");
    a.type = field(req, "address_type");
    return a;
}

// Remember "came from checkout": go back to /checkout after add/edit. Only this one fixed value, never a URL.
static bool from_checkout(const Request& req) {
    return param(req, "return") == "checkout" || field(req, "return") == "checkout";
}

// Address form: the states list from the DB (36 rows), the 3 type options, and what the user typed.
static int address_form_page(SOCKET client, const Request& req, int status, const Address& a, bool is_new, const string& error) {
    vector<vector<string>> states;
    if (!db_select("SELECT state_id, state_name FROM states ORDER BY state_name", {}, states) || !cols_ok(states, 2))
        return error_page(client, req, 500, "Database error");
    string state_opts;
    for (auto& st : states)
        state_opts += render("search_option", {{"value", st[0]}, {"label", st[1]}, {"selected", st[0] == a.state_id ? "selected" : ""}});
    string type_opts;
    for (const char* t : {"HOME", "WORK", "OTHER"}) {
        string label = string(1, t[0]) + (string(t + 1)) ;
        for (size_t i = 1; i < label.size(); i++) label[i] = (char)tolower((unsigned char)label[i]);
        type_opts += render("search_option", {{"value", t}, {"label", label}, {"selected", a.type == t ? "selected" : ""}});
    }
    string hidden;
    if (!is_new) hidden += render("hidden_input", {{"name", "address_id"}, {"value", a.id}});
    if (from_checkout(req)) hidden += render("hidden_input", {{"name", "return"}, {"value", "checkout"}});

    string content = render("address_form", {{"title", is_new ? "Add a new address" : "Edit address"},
                                             {"action", is_new ? "/addresses/new" : "/addresses/edit"}, {"hidden", hidden},
                                             {"first_name", a.first_name}, {"last_name", a.last_name}, {"phone", a.phone},
                                             {"types", type_opts}, {"line1", a.line1}, {"line2", a.line2}, {"landmark", a.landmark},
                                             {"city", a.city}, {"states", state_opts}, {"pincode", a.pincode},
                                             {"button", is_new ? "Save address" : "Save changes"},
                                             {"cancel", from_checkout(req) ? "/checkout" : "/addresses"}});
    return send_page(client, req, status, is_new ? "Add address" : "Edit address", content, "", error.empty() ? "" : flash_text("err", error));
}

// After a save: success goes back (checkout or /addresses), bad input shows the form with the message, anything else an error page.
static int address_saved(SOCKET client, const Request& req, const AddressResult& r, const Address& a, bool is_new) {
    if (r.status == 200) {
        if (from_checkout(req)) return redirect(client, req, "/checkout?address_id=" + to_string(r.address_id));
        return redirect(client, req, is_new ? "/addresses?ok=added" : "/addresses?ok=saved");
    }
    if (r.status == 400) return address_form_page(client, req, 400, a, is_new, r.message);
    return error_page(client, req, r.status == 404 || r.status == 503 ? r.status : 500, r.message);
}

// GET /addresses/new = empty form, POST = save. All checks are on the server (the form patterns are only a convenience).
static int address_new_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method == "GET") return address_form_page(client, req, 200, Address(), true, "");
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    Address a = read_address(req);
    a.id = "";
    string e = address_check(a);
    if (!e.empty()) return address_form_page(client, req, 400, a, true, e);
    return address_saved(client, req, address_save(req.user_id, a, true), a, true);
}

// GET /addresses/edit?id=N = filled form, POST = save. Only this user's address (otherwise 404).
static int address_edit_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET" && req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    if (req.method == "POST") {
        Address a = read_address(req);
        if (!valid_product_id(a.id)) return error_page(client, req, 400, "Invalid address id");
        string e = address_check(a);
        if (!e.empty()) return address_form_page(client, req, 400, a, false, e);
        return address_saved(client, req, address_save(req.user_id, a, false), a, false);
    }

    string id = param(req, "id");
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    vector<vector<string>> rows;
    if (!db_select("SELECT first_name, last_name, phone, line1, IFNULL(line2, ''), IFNULL(landmark, ''), city, state_id, pincode, address_type "
                   "FROM addresses WHERE address_id = ? AND user_id = ? AND is_active = TRUE", {id, to_string(req.user_id)}, rows) ||
        !cols_ok(rows, 10))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) return error_page(client, req, 404, "Address not found");
    Address a;
    a.id = id;
    a.first_name = rows[0][0]; a.last_name = rows[0][1]; a.phone = rows[0][2]; a.line1 = rows[0][3]; a.line2 = rows[0][4];
    a.landmark = rows[0][5]; a.city = rows[0][6]; a.state_id = rows[0][7]; a.pincode = rows[0][8]; a.type = rows[0][9];
    return address_form_page(client, req, 200, a, false, "");
}

// POST /addresses/delete (address_id): is_active = 0, never DELETE.
static int address_delete_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "address_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    AddressResult r = address_delete(req.user_id, atoi(id.c_str()));
    if (r.status == 200) return redirect(client, req, "/addresses?ok=deleted");
    return error_page(client, req, r.status == 404 ? 404 : 500, r.message);
}

// POST /addresses/default (address_id)
static int address_default_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "address_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    AddressResult r = address_set_default(req.user_id, atoi(id.c_str()));
    if (r.status == 200) return redirect(client, req, "/addresses?ok=default");
    return error_page(client, req, r.status == 404 || r.status == 503 ? r.status : 500, r.message);
}

// ---- checkout and orders ----

// ---- payment (simulated gateway: no real money, but the checks are real) ----
// Net Banking banks: the code goes in the form, the name is stored in payments.pay_detail.
struct Bank {
    const char* code;
    const char* name;
};
static const Bank BANKS[] = {{"SBI", "State Bank of India"}, {"HDFC", "HDFC Bank"}, {"ICICI", "ICICI Bank"}, {"AXIS", "Axis Bank"},
                             {"KOTAK", "Kotak Mahindra Bank"}, {"PNB", "Punjab National Bank"}, {"BOB", "Bank of Baroda"}};

// Test values (like real gateways' test mode): this card / UPI ID means "declined by the bank", to show the ROLLBACK.
const string DECLINED_CARD = "4000000000000002";
const string DECLINED_UPI = "fail@upi";

// Luhn checksum of a card number (every real number has it): it catches any single mistyped digit.
static bool luhn_ok(const string& digits) {
    int sum = 0;
    bool twice = false;                     // Every second digit from the right is doubled.
    for (size_t i = digits.size(); i-- > 0;) {
        int d = digits[i] - '0';
        if (twice) {
            d *= 2;
            if (d > 9) d -= 9;
        }
        sum += d;
        twice = !twice;
    }
    return sum % 10 == 0;
}

// Card brand from the start of the number (for display only).
static string card_brand(const string& d) {
    int two = atoi(d.substr(0, 2).c_str());
    int four = atoi(d.substr(0, 4).c_str());
    if (d[0] == '4') return "VISA";
    if ((two >= 51 && two <= 55) || (four >= 2221 && four <= 2720)) return "MASTERCARD";
    if (two == 34 || two == 37) return "AMEX";
    if (two == 60 || two == 65 || two == 81 || two == 82) return "RUPAY";
    return "CARD";
}

// Expiry "MM/YY": a valid month, this month or later (a card works until the end of its month).
static bool expiry_ok(const string& e) {
    if (e.size() != 5 || e[2] != '/' || !is_number(e.substr(0, 2)) || !is_number(e.substr(3, 2))) return false;
    int mm = atoi(e.substr(0, 2).c_str());
    int yy = 2000 + atoi(e.substr(3, 2).c_str());
    if (mm < 1 || mm > 12) return false;
    time_t now = time(nullptr);
    tm t;
    localtime_s(&t, &now);
    int cur = (t.tm_year + 1900) * 12 + t.tm_mon + 1;
    return yy * 12 + mm >= cur;
}

// UPI ID: name@bank, e.g. ravi.kumar@okaxis (name: a-z 0-9 . _ -, bank: letters only).
static bool upi_ok(const string& u) {
    size_t at = u.find('@');
    if (at == string::npos || at < 2 || at > 64 || u.size() - at - 1 < 2 || u.size() - at - 1 > 32) return false;
    for (size_t i = 0; i < u.size(); i++) {
        char c = u[i];
        if (i < at && !(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
        if (i > at && !isalpha((unsigned char)c)) return false;
    }
    return true;
}

// Gateway transaction id: "TXN" + 12 random hex (the same secure random as session ids, not rand()).
static string new_txn_ref() {
    string hex;
    if (!new_session_id(hex)) return "";
    string out = "TXN";
    for (size_t i = 0; i < 12; i++) out += (char)toupper((unsigned char)hex[i]);
    return out;
}

// Reads and checks the checkout payment. "" = OK (pay is filled), otherwise a message for the user.
// The card number and CVV are only checked here: never stored, never logged, never sent back to the form.
static string read_payment(const Request& req, PaymentInfo& pay) {
    pay.method = trim(field(req, "pay_method"));
    if (pay.method == "COD") return "";

    if (pay.method == "UPI") {
        string upi = lower_ascii(trim(field(req, "upi_id")));
        if (!upi_ok(upi)) return "Enter a valid UPI ID, like name@okaxis.";
        pay.detail = upi;
        pay.declined = (upi == DECLINED_UPI);
    } else if (pay.method == "CARD") {
        string num;
        for (char c : field(req, "card_number")) {
            if (c == ' ' || c == '-') continue;                 // "4111 1111 ..." is accepted too.
            if (c < '0' || c > '9') return "Card number can only have digits.";
            num += c;
        }
        if (num.size() < 13 || num.size() > 19 || !luhn_ok(num)) return "That card number is not valid. Please check it.";
        string brand = card_brand(num);
        string name = trim(field(req, "card_name"));
        if (name.size() < 2 || name.size() > 60) return "Enter the name printed on the card.";
        if (!expiry_ok(trim(field(req, "card_expiry")))) return "Enter a valid expiry date (MM/YY) that is not in the past.";
        string cvv = trim(field(req, "card_cvv"));
        size_t want = (brand == "AMEX") ? 4 : 3;
        if (cvv.size() != want || !is_number(cvv)) return "CVV must be " + to_string(want) + " digits.";
        pay.detail = brand + " **** " + num.substr(num.size() - 4);    // Only this is stored.
        pay.declined = (num == DECLINED_CARD);
    } else if (pay.method == "NETBANKING") {
        string code = trim(field(req, "bank"));
        for (const Bank& b : BANKS)
            if (code == b.code) pay.detail = b.name;
        if (pay.detail.empty()) return "Please choose your bank.";
    } else {
        return "Please choose a payment method.";
    }

    pay.txn_ref = new_txn_ref();
    if (pay.txn_ref.empty()) return "Could not start the payment. Please try again.";
    return "";
}

// Payment panel of the checkout page. After an error the chosen method, UPI ID, name, expiry and bank are filled in again;
// the card number and CVV never come back (the template has no value for those fields).
static string payment_panel(const Request& req) {
    bool post = req.method == "POST";
    string m = post ? trim(field(req, "pay_method")) : "";
    if (m != "UPI" && m != "CARD" && m != "NETBANKING") m = "COD";
    string bank = post ? field(req, "bank") : "";
    string banks = render("search_option", {{"value", ""}, {"label", "Choose your bank"}, {"selected", ""}});
    for (const Bank& b : BANKS)
        banks += render("search_option", {{"value", b.code}, {"label", b.name}, {"selected", bank == b.code ? "selected" : ""}});
    return render("checkout_payment", {{"cod", m == "COD" ? "checked" : ""}, {"upi", m == "UPI" ? "checked" : ""},
                                       {"card", m == "CARD" ? "checked" : ""}, {"netbanking", m == "NETBANKING" ? "checked" : ""},
                                       {"upi_id", post ? field(req, "upi_id") : ""}, {"card_name", post ? field(req, "card_name") : ""},
                                       {"card_expiry", post ? field(req, "card_expiry") : ""}, {"banks", banks}});
}

// Payment as shown on the order pages: method + detail, status pill, paid time, transaction id.
static string payment_html(const string& method, const string& detail, const string& status, const string& paid, const string& txn) {
    if (method.empty()) return "-";
    string label = method == "COD" ? "Cash on Delivery" : method == "UPI" ? "UPI" : method == "CARD" ? "Card"
                 : method == "NETBANKING" ? "Net Banking" : method;
    return render("order_payment", {{"method", label}, {"detail", detail.empty() ? "" : " (" + detail + ")"},
                                    {"status", status}, {"status_class", status_class(status)},
                                    {"paid", paid.empty() ? "" : ", paid " + paid}, {"txn", txn.empty() ? "" : ", Txn " + txn}});
}

// Checkout confirm page: cart summary and total (read-only, changes nothing).
// status/error: if the payment form was wrong, the same page with 400 and the error (address, method, UPI ID filled in).
static int checkout_confirm_page(SOCKET client, const Request& req, int status = 200, const string& error = "") {
    vector<vector<string>> rows;
    if (!load_cart_rows(req.user_id, rows)) return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 5)) return error_page(client, req, 500, "Database error");
    if (rows.empty())
        return send_page(client, req, 200, "Checkout", render("heading", {{"text", "Checkout"}}) +
                                                        render("empty_state", {{"message", "Your cart is empty."}}));

    long long total = 0;
    string body, warnings;
    for (auto& r : rows) {
        int qty = atoi(r[3].c_str());
        int stock = atoi(r[4].c_str());
        long long sub = to_cents(r[2]) * qty;
        total += sub;
        body += render("checkout_row", {{"name", r[1]}, {"qty", to_string(qty)}, {"price", rupees_text(r[2])},
                                        {"subtotal", rupees(sub)}});
        if (qty > stock)
            warnings += flash_text("err", r[1] + ": only " + to_string(stock) + " left, you have " + to_string(qty) +
                                          " in your cart. Update your cart first.");
    }

    // Shipping address: radio cards. The default comes first (load_addresses order) and is pre-selected.
    // /checkout?address_id=N (a just-added address) selects that one.
    vector<vector<string>> addrs;
    if (!load_addresses(req.user_id, addrs)) return error_page(client, req, 500, "Database error");
    string want = req.method == "POST" ? field(req, "address_id") : param(req, "address_id");
    size_t pick = 0;
    for (size_t i = 0; i < addrs.size(); i++)
        if (addrs[i][0] == want) pick = i;
    string cards;
    for (size_t i = 0; i < addrs.size(); i++)
        cards += render("checkout_address", {{"id", addrs[i][0]}, {"checked", i == pick ? "checked" : ""},
                                             {"badge", addrs[i][11] == "1" ? render("address_default_badge", {}) : ""},
                                             {"text", address_html(addrs[i], 1)}});
    if (addrs.empty()) cards = render("checkout_no_address", {});      // No address, no order: the button is not shown.
    return send_page(client, req, status, "Checkout",
                     render("checkout", {{"warnings", warnings}, {"addresses", cards}, {"payment", payment_panel(req)},
                                         {"rows", body}, {"total", rupees(total)},
                                         {"place", addrs.empty() ? "" : render("checkout_place", {})}}),
                     "", error.empty() ? "" : flash_text("err", error));
}

// GET /checkout = confirm page, POST = place the order (one DB transaction: do_checkout in db.cpp).
static int checkout_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method == "GET") return checkout_confirm_page(client, req);
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    // The cart is the session user's. From the form only address_id: db.cpp checks inside the transaction that it is theirs and active.
    string aid = trim(field(req, "address_id"));
    if (!valid_product_id(aid))
        return send_page(client, req, 400, "Order not placed", render("checkout_failed", {}), "",
                         flash_text("err", "Please choose a shipping address."));
    // A wrong payment form shows the checkout page again with the error (address, method, UPI ID filled in); no order is attempted.
    PaymentInfo pay;
    string bad = read_payment(req, pay);
    if (!bad.empty()) return checkout_confirm_page(client, req, 400, bad);
    CheckoutResult r = do_checkout(req.user_id, atoi(aid.c_str()), pay);
    if (r.status == 200) return redirect(client, req, "/order?id=" + to_string(r.order_id));   // Post/Redirect/Get.

    string retry = (r.status == 503) ? "Retry-After: 2\r\n" : "";
    return send_page(client, req, r.status, "Order not placed", render("checkout_failed", {}), retry,
                     flash_text("err", r.message));
}

// GET /orders: all of this user's orders (newest first).
static int orders_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    vector<vector<string>> rows;
    if (!db_select("SELECT order_id, created_at, total, status FROM orders WHERE user_id = ? "
                   "ORDER BY order_id DESC LIMIT 50", {to_string(req.user_id)}, rows))
        return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 4)) return error_page(client, req, 500, "Database error");

    if (rows.empty())
        return send_page(client, req, 200, "Orders", render("heading", {{"text", "Your orders"}}) +
                                                      render("empty_state", {{"message", "No orders yet."}}));
    string body;
    for (auto& r : rows)
        body += render("order_row", {{"id", r[0]}, {"placed", r[1]}, {"total", rupees_text(r[2])}, {"status", status_label(r[3])},
                                     {"status_class", status_class(r[3])}});
    return send_page(client, req, 200, "Orders", render("orders", {{"rows", body}}));
}

// Order header (with payment) and items on one connection. Only the owner's order is found ("AND o.user_id = ?"):
// someone else's order gives 404, not 403 (we do not even say whether the order exists).
static bool load_order(MYSQL* m, int user_id, const string& id, vector<vector<string>>& head,
                       vector<vector<string>>& items, vector<vector<string>>& hist, string& eta) {
    if (!db_select_on(m, "SELECT o.order_id, o.status, o.total, o.created_at, IFNULL(p.method, ''), "
                         "IFNULL(p.status, ''), IFNULL(p.paid_at, ''), IFNULL(p.pay_detail, ''), IFNULL(p.txn_ref, '') "
                         "FROM orders o LEFT JOIN payments p ON p.order_id = o.order_id "
                         "WHERE o.order_id = ? AND o.user_id = ?", {id, to_string(user_id)}, head))
        return false;
    if (head.empty()) return true;
    if (!db_select_on(m, "SELECT p.product_id, p.name, oi.quantity, oi.price FROM order_items oi "
                         "JOIN products p ON p.product_id = oi.product_id WHERE oi.order_id = ? ORDER BY p.name",
                      {id}, items))
        return false;
    // For tracking: when each status happened (history) and "Arriving by" = next step time + average of the remaining steps.
    // GREATEST(.., NOW()): if the next step is overdue, count from now. The seconds are our own int (order_eta_seconds).
    if (!db_select_on(m, "SELECT status, DATE_FORMAT(changed_at, '%d %b, %H:%i:%s'), changed_by FROM order_status_history "
                         "WHERE order_id = ? ORDER BY history_id", {id}, hist))
        return false;
    const string status = head[0].size() > 1 ? head[0][1] : "";
    if (AUTO_DELIVERY && !order_next_status(status).empty()) {
        vector<vector<string>> e;
        if (!db_select_on(m, "SELECT DATE_FORMAT(DATE_ADD(GREATEST(next_step_at, NOW()), INTERVAL " + to_string(order_eta_seconds(status)) +
                             " SECOND), '%d %b, %H:%i:%s') FROM orders WHERE order_id = ? AND next_step_at IS NOT NULL", {id}, e))
            return false;
        if (!e.empty() && !e[0].empty()) eta = e[0][0];
    }
    return true;
}

// Time of the (first) history row for this status; "" for an old order that has none.
static string history_time(const vector<vector<string>>& hist, const string& status) {
    for (auto& h : hist)
        if (h.size() >= 2 && h[0] == status) return h[1];
    return "";
}

// Order tracking: 5 steps (Ordered, Packed, Shipped, Out for delivery, Delivered): done = tick + history time, current = highlighted, rest dim.
// A CANCELLED order shows the steps that happened, then a red "Cancelled".
static string timeline_html(const string& status, const vector<vector<string>>& hist, const string& eta) {
    static const char* STEPS[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
    bool cancelled = (status == "CANCELLED");
    int cur = -1;
    for (int i = 0; i < 5; i++)
        if (status == STEPS[i]) cur = i;

    string steps;
    for (int i = 0; i < 5; i++) {
        string t = history_time(hist, STEPS[i]);
        string state = "todo";
        if (cancelled) {
            if (t.empty()) continue;            // A cancelled order does not show steps that never happened.
            state = "done";
        }
        else if (status == "DELIVERED" || i < cur) state = "done";
        else if (i == cur) state = "current";
        steps += render("timeline_step", {{"state", state}, {"label", status_label(STEPS[i])}, {"time", state == "todo" ? "" : t}});
    }
    if (cancelled)
        steps += render("timeline_step", {{"state", "cancelled"}, {"label", "Cancelled"},
                                          {"time", history_time(hist, "CANCELLED")}});
    string eta_html = eta.empty() ? "" : render("timeline_eta", {{"time", eta}});
    return render("timeline", {{"steps", steps}, {"eta", eta_html}});
}

// GET /order?id=N
// Message above the order page: /order?id=N&ok=cancelled (fixed text).
static string order_notice(const Request& req) {
    if (param(req, "ok") == "cancelled") return flash_text("ok", "Your order was cancelled. Its items went back to stock.");
    return "";
}

static int order_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    string id = param(req, "id");
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid order id");

    vector<vector<string>> head, items, hist;
    string eta;
    bool ok = false;
    int cid = 0;
    MYSQL* m = get_conn(cid);
    if (m != nullptr) {
        ok = load_order(m, req.user_id, id, head, items, hist, eta);
        put_conn(cid);
    }
    if (!ok) return error_page(client, req, 500, "Database error");
    if (head.empty()) return error_page(client, req, 404, "Order not found");
    if (!cols_ok(head, 9) || !cols_ok(items, 4) || !cols_ok(hist, 3)) return error_page(client, req, 500, "Database error");

    vector<string>& o = head[0];
    // Payment: method + detail, status pill (PENDING yellow, PAID green, REFUNDED purple ...), paid time, Txn id; "-" if none.
    string pay = payment_html(o[4], o[7], o[5], o[6], o[8]);
    string body;
    for (auto& r : items)       // The price at the time of the order (order_items), not today's price.
        body += render("order_item_row", {{"id", r[0]}, {"name", r[1]}, {"qty", r[2]}, {"price", rupees_text(r[3])},
                                          {"subtotal", rupees(to_cents(r[3]) * atoi(r[2].c_str()))}});
    string addr;                // Shipping address (copy from order time); an old order shows "No address (old order)".
    if (!order_address_text(o[0], addr)) return error_page(client, req, 500, "Database error");
    // "Cancel order" only for PLACED or PACKED (the server checks the same in order_change).
    string actions = (o[1] == "PLACED" || o[1] == "PACKED") ? render("order_cancel", {{"id", o[0]}}) : "";
    return send_page(client, req, 200, "Order #" + o[0],
                     render("order", {{"id", o[0]}, {"status", status_label(o[1])}, {"status_class", status_class(o[1])},
                                      {"timeline", timeline_html(o[1], hist, eta)}, {"address", render("order_address", {{"body", addr}})},
                                      {"placed", o[3]}, {"payment", pay}, {"rows", body}, {"total", rupees_text(o[2])},
                                      {"actions", actions}}),
                     "", order_notice(req));
}

// POST /order/cancel (order_id): the customer cancels their own PLACED/PACKED order. The owner check is inside order_change (someone else's = 404).
static int order_cancel_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "order_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid order id");

    OrderResult r = order_change(atoi(id.c_str()), "CANCELLED", req.user_id, false, "");
    if (r.status == 200) return redirect(client, req, "/order?id=" + id + "&ok=cancelled");
    if (r.status == 400) return error_page(client, req, 400, "This order can no longer be cancelled.");
    if (r.status == 404 || r.status == 409 || r.status == 503) return error_page(client, req, r.status, r.message);
    return error_page(client, req, 500, "Database error");
}

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
static int metrics_page(SOCKET client, const Request& req, bool json) {
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

// After any category change drop the cached navbar list, so the next store request shows the new name.
static void clear_category_cache() {
    cat_lock.lock();
    cat_cache.clear();
    cat_loaded_us = 0;
    cat_lock.unlock();
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
static int admin_route(SOCKET client, const Request& req) {
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
        Sleep(3000);
        send_response(client, 200, HTML, render("plain", {{"message", "done"}}), with_cookie_fix(req, ""));
        return 200;
    }
    if (path == "/dbtest" && debug_routes_on()) return db_test_page(client, req, "Products in database", "SELECT COUNT(*) FROM products");
    if (path == "/dbslow" && debug_routes_on()) return db_test_page(client, req, "Slept 2 seconds", "SELECT SLEEP(2)");

    return error_page(client, req, 404, "404 Not Found");
}


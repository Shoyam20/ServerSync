// pages.h - what the page files (pages, auth, shop, cart, checkout, admin) share. Only declarations: every body is in the .cpp named in the section.
// Included by all of src/pages.cpp, auth.cpp, shop.cpp, cart.cpp, checkout.cpp and admin.cpp (after common.h).
#pragma once
#include <map>
#include <string>
#include <vector>
#include "common.h"
using namespace std;

extern const string HTML;           // "text/html; charset=utf-8"
extern const string EXPIRES_SQL;    // "NOW() + INTERVAL <SESSION_MINUTES> MINUTE"
extern const string BUDGET_SQL;     // the home page's "Budget picks" query

// ---- pages.cpp: layout, responses, request fields, small formatting helpers, sessions, categories cache ----
void clear_category_cache();
string session_cookie(const string& value, int max_age);
vector<vector<string>> get_categories(bool allow_db);
string category_emoji(const string& name);
string rupees(long long cents);
string with_cookie_fix(const Request& req, const string& extra);
string flash_box(const string& kind, const string& message_html);
string flash_text(const string& kind, const string& text);
string page(const Request& req, const string& title, const string& content, const string& head_extra = "", const string& flash = "", bool interactive = true, bool allow_db = true);
int send_page(SOCKET client, const Request& req, int status, const string& title, const string& content, const string& extra = "", const string& flash = "");
string error_content(const string& msg, const string& detail_html = "");
int error_page(SOCKET client, const Request& req, int status, const string& msg);
int redirect(SOCKET client, const Request& req, const string& location, const string& extra = "");
int method_not_allowed(SOCKET client, const Request& req, const string& allow);
long long to_cents(const string& price);
string rupees_text(const string& price);
int thumb_hue(const string& category);
string status_class(const string& status);
string status_label(const string& status);
string stock_html(const string& stock);
bool is_number(const string& s);
string escape_like(const string& s);
string param(const Request& req, const string& key);
string field(const Request& req, const string& key);
bool cols_ok(const vector<vector<string>>& rows, size_t n);
bool new_session_id(string& out);
string strip_param(const string& target, const string& name);
string return_target(const Request& req);
string safe_return(const string& r);
string cart_notice(const Request& req);

// ---- auth.cpp: password rules, signup, login, logout, change password, My Sessions ----
map<string, string> pw_parts(bool change_form, const vector<string>* failed);
string lower_ascii(string t);
int signup_page(SOCKET client, const Request& req);
int login_page(SOCKET client, const Request& req);
int account_password_page(SOCKET client, const Request& req);
int logout_page(SOCKET client, const Request& req);
string session_status(const string& is_active, const string& expired, const string& end_reason);
int sessions_page(SOCKET client, const Request& req);

// ---- shop.cpp: home, products, product page ----
int products_page(SOCKET client, const Request& req);
int home_page(SOCKET client, const Request& req);
int product_page(SOCKET client, const Request& req);

// ---- cart.cpp: the shopping cart ----
bool parse_range(const string& s, int lo, int hi, int& out);
bool valid_product_id(const string& s);
bool load_cart_rows(int user_id, vector<vector<string>>& rows);
int cart_page(SOCKET client, const Request& req);
int cart_add_page(SOCKET client, const Request& req);
int cart_update_page(SOCKET client, const Request& req);
int cart_remove_page(SOCKET client, const Request& req);

// ---- checkout.cpp: addresses, payment, checkout, customer orders ----
bool order_address_text(const string& order_id, string& out);
string payment_html(const string& method, const string& detail, const string& status, const string& paid, const string& txn);
int addresses_page(SOCKET client, const Request& req);
int address_new_page(SOCKET client, const Request& req);
int address_edit_page(SOCKET client, const Request& req);
int address_delete_page(SOCKET client, const Request& req);
int address_default_page(SOCKET client, const Request& req);
int checkout_page(SOCKET client, const Request& req);
int orders_page(SOCKET client, const Request& req);
int order_page(SOCKET client, const Request& req);
int order_cancel_page(SOCKET client, const Request& req);

// ---- admin.cpp: /metrics and the admin panel ----
int metrics_page(SOCKET client, const Request& req, bool json);
int admin_route(SOCKET client, const Request& req);


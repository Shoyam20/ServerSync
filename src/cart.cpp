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

// ---- cart: every cart page needs login; cart_id always comes from the session user, never from the form ----
// The price always comes from the DB. Only product_id and qty are read from the form.

// Is the text a number between lo and hi? (out gets the value)
bool parse_range(const string& s, int lo, int hi, int& out) {
    if (!is_number(s)) return false;
    int v = atoi(s.c_str());
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

// Valid product id? (a number, 1 or more)
bool valid_product_id(const string& s) {
    int v = 0;
    return parse_range(s, 1, 999999999, v);
}

// Reads the cart items (empty if there is no cart). One connection, and put_conn always runs (a single return point).
bool load_cart_rows(int user_id, vector<vector<string>>& rows) {
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
int cart_page(SOCKET client, const Request& req) {
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
int cart_add_page(SOCKET client, const Request& req) {
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
int cart_update_page(SOCKET client, const Request& req) {
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
int cart_remove_page(SOCKET client, const Request& req) {
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


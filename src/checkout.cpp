#include "platform.h"   // secure_random, sleep_ms, local_time (Windows / Linux)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "common.h"
#include "pages.h"
using namespace std;

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
bool order_address_text(const string& order_id, string& out) {
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
int addresses_page(SOCKET client, const Request& req) {
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
int address_new_page(SOCKET client, const Request& req) {
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
int address_edit_page(SOCKET client, const Request& req) {
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
int address_delete_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "address_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    AddressResult r = address_delete(req.user_id, atoi(id.c_str()));
    if (r.status == 200) return redirect(client, req, "/addresses?ok=deleted");
    return error_page(client, req, r.status == 404 ? 404 : 500, r.message);
}

// POST /addresses/default (address_id)
int address_default_page(SOCKET client, const Request& req) {
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
    local_time(&now, &t);   // localtime_s on Windows, localtime_r on Linux (platform.h)
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
string payment_html(const string& method, const string& detail, const string& status, const string& paid, const string& txn) {
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
int checkout_page(SOCKET client, const Request& req) {
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
int orders_page(SOCKET client, const Request& req) {
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

int order_page(SOCKET client, const Request& req) {
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
int order_cancel_page(SOCKET client, const Request& req) {
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


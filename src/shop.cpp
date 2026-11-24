
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

// ---- shop pages ----

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
int products_page(SOCKET client, const Request& req) {
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
int home_page(SOCKET client, const Request& req) {
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
int product_page(SOCKET client, const Request& req) {
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


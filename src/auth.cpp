// auth.cpp - password rules, signup, login, logout, change password, My Sessions.
// Owner: Ashutosh
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

// ---- forms ----


// ---- password policy ----
// The rules live only in check_password; app.js just shows them live. No DB CHECK: they apply to typed passwords, not hashes.
static const char* COMMON_PW[] = {"password@123", "admin@123", "qwerty@123", "welcome@123", "abcd@1234", "password@1234", "password1!",
                                  "passw0rd!", "p@ssw0rd", "p@ssword1", "admin@1234", "admin@12345", "qwerty@1234", "welcome@1",
                                  "welcome@1234", "test@1234", "india@123", "india@1234", "pass@1234", "abc@12345", "abc@1234",
                                  "letmein@123", "changeme@123", "iloveyou@123", "user@1234", "master@123", "monkey@123",
                                  "dragon@123", "login@1234", "hello@1234"};

string lower_ascii(string t) {
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
map<string, string> pw_parts(bool change_form, const vector<string>* failed) {
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

int signup_page(SOCKET client, const Request& req) {
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

int login_page(SOCKET client, const Request& req) {
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
int account_password_page(SOCKET client, const Request& req) {
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
int logout_page(SOCKET client, const Request& req) {
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
string session_status(const string& is_active, const string& expired, const string& end_reason) {
    if (is_active == "1") return expired == "1" ? "Expired" : "Active";   // Expired but not closed yet.
    if (end_reason == "FORCED") return "Ended by admin";                  // Ended by an admin (Admin > Sessions).
    return end_reason == "EXPIRED" ? "Expired" : "Logged out";            // LOGOUT, or an old row (NULL).
}

// /sessions: this user's login history, the current session highlighted.
int sessions_page(SOCKET client, const Request& req) {
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


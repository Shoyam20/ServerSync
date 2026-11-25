// password_hash_check.cpp - checks include/password_hash.h (and pbkdf2_sha256 in platform.h) on its own.
// No database, no server. tests/test_password_hash.py builds and runs it; it prints "PASS ..." / "FAIL ..." lines.
#include "../include/password_hash.h"
#include <chrono>
#include <iostream>
using namespace std;

static int fails = 0;
static void check(const string& name, bool ok) {
    cout << (ok ? "PASS " : "FAIL ") << name << endl;
    if (!ok) fails++;
}

static string pbkdf2_hex(const string& pw, const string& salt, unsigned long iters) {
    unsigned char out[32];
    if (!pbkdf2_sha256(pw, (const unsigned char*)salt.data(), (unsigned long)salt.size(), iters, out, sizeof(out))) return "error";
    return pw_to_hex(out, sizeof(out));
}

int main() {
    // known answers for PBKDF2-HMAC-SHA256 (same numbers as Python's hashlib.pbkdf2_hmac / Node's crypto.pbkdf2Sync)
    check("pbkdf2 vector: password/salt/1", pbkdf2_hex("password", "salt", 1) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    check("pbkdf2 vector: password/salt/4096", pbkdf2_hex("password", "salt", 4096) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    check("pbkdf2 vector: passwd/salt/1", pbkdf2_hex("passwd", "salt", 1) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");

    auto t0 = chrono::steady_clock::now();
    string h = hash_password("Zx9#kLm2Qw");
    auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
    cout << "HASH " << h << endl;
    cout << "HASH_MS " << ms << endl;
    check("hash has the right shape", is_password_hash(h) && h.size() == string(PW_HASH_PREFIX).size() + 6 + 1 + 32 + 1 + 64);
    check("hash does not contain the password", h.find("Zx9#kLm2Qw") == string::npos);
    check("right password verifies", verify_password("Zx9#kLm2Qw", h));
    check("wrong password fails", !verify_password("Zx9#kLm2Qx", h));
    check("capital / small letters matter", !verify_password("zx9#klm2qw", h));
    check("empty password fails", !verify_password("", h));
    check("same password twice -> different hashes (random salt)", hash_password("Zx9#kLm2Qw") != h);
    check("new hash does not need a rehash", !password_needs_rehash(h));

    // old users: plain text in the database
    check("old plain text password still verifies", verify_password("Old@pass1", "Old@pass1"));
    check("old plain text: wrong password fails", !verify_password("old@pass1", "Old@pass1"));
    check("old plain text needs a rehash", password_needs_rehash("Old@pass1"));
    check("empty stored value never matches", !verify_password("", ""));

    // a hash made with fewer iterations still works but asks for a rehash
    string weak = hash_password("Zx9#kLm2Qw", 1000);
    check("fewer iterations: still verifies", verify_password("Zx9#kLm2Qw", weak));
    check("fewer iterations: needs a rehash", password_needs_rehash(weak));

    // broken values in the database must never log anyone in
    string p = PW_HASH_PREFIX;
    for (const string& bad : {p, p + "abc$00$00", p + "0$" + string(32, '0') + "$" + string(64, '0'), p + "100$zz$" + string(64, '0'),
                              p + "100$" + string(32, '0') + "$" + string(63, '0'), p + "100$" + string(32, '0') + "$" + string(64, 'A')})
        check("broken stored hash rejected: " + bad.substr(0, 40), !verify_password("anything", bad));

    cout << (fails == 0 ? "ALL OK" : "SOME FAILED") << endl;
    return fails == 0 ? 0 : 1;
}

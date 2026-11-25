// password_hash.h - passwords are no longer stored as plain text; the database keeps a PBKDF2-HMAC-SHA256 hash.
//
// What users.password holds (about 120 characters, fits easily in VARCHAR(255)):
//     pbkdf2_sha256$<iterations>$<salt: 32 hex>$<hash: 64 hex>
// - salt: 16 random bytes per password (secure_random). Two users with the same password still get different hashes,
//   and precomputed "rainbow tables" do not help an attacker.
// - iterations: makes hashing deliberately slow, so every guess is expensive if the database ever leaks.
//   The number is stored inside the hash, so it can be raised later: old hashes keep working and are re-made
//   with the new number at the user's next login (password_needs_rehash).
// - Users created before hashing still have a plain text password. verify_password accepts those too, and auth.cpp
//   replaces them with a hash right after a successful login, so no migration script is needed.
//
// Why PBKDF2: it is already available on Windows (bcrypt.dll) and Linux (OpenSSL), so no new library is needed.
// (Argon2 or bcrypt would be stronger, but they need a new dependency such as libsodium.)
#pragma once
#include "platform.h"   // secure_random, pbkdf2_sha256
#include <string>
#include <vector>

const char* const PW_HASH_PREFIX = "pbkdf2_sha256$";
// OWASP recommends 600,000, but the Render free plan has a very small CPU share (every login does this work, and there are
// only 4 workers). 100,000 keeps a login there under about half a second. The number is stored in each hash, so raising
// it later is easy.
const unsigned long PW_HASH_ITERATIONS = 100000;
const int PW_SALT_BYTES = 16;
const int PW_KEY_BYTES = 32;    // SHA-256 output size

inline std::string pw_to_hex(const unsigned char* b, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += digits[b[i] >> 4];
        s += digits[b[i] & 15];
    }
    return s;
}

inline bool pw_from_hex(const std::string& s, std::vector<unsigned char>& out) {
    if (s.empty() || s.size() % 2 != 0) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;      // lowercase hex only: that is what we write ourselves
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = val(s[i]), lo = val(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((unsigned char)(hi * 16 + lo));
    }
    return true;
}

// Are the two strings equal? Looks at every byte even when the first one already differs, so the response time
// does not reveal how many leading characters were right (timing attack).
inline bool pw_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

inline bool is_password_hash(const std::string& stored) { return stored.rfind(PW_HASH_PREFIX, 0) == 0; }

// New hash (signup, password change, upgrading an old plain text password). Returns "" on error: the caller must show
// an error and never save the plain text instead.
inline std::string hash_password(const std::string& pw, unsigned long iterations = PW_HASH_ITERATIONS) {
    unsigned char salt[PW_SALT_BYTES];
    unsigned char key[PW_KEY_BYTES];
    if (!secure_random(salt, sizeof(salt))) return "";
    if (!pbkdf2_sha256(pw, salt, sizeof(salt), iterations, key, sizeof(key))) return "";
    return std::string(PW_HASH_PREFIX) + std::to_string(iterations) + "$" + pw_to_hex(salt, sizeof(salt)) + "$" +
           pw_to_hex(key, sizeof(key));
}

// Splits a stored hash into its parts. Returns false if it is not in the expected format.
inline bool pw_parse(const std::string& stored, unsigned long& iterations, std::vector<unsigned char>& salt, std::string& key_hex) {
    if (!is_password_hash(stored)) return false;
    size_t a = std::string(PW_HASH_PREFIX).size();
    size_t b = stored.find('$', a);
    if (b == std::string::npos) return false;
    size_t c = stored.find('$', b + 1);
    if (c == std::string::npos) return false;
    std::string iter_s = stored.substr(a, b - a);
    if (iter_s.empty() || iter_s.size() > 9 || iter_s.find_first_not_of("0123456789") != std::string::npos) return false;
    iterations = std::stoul(iter_s);
    if (iterations == 0) return false;
    if (!pw_from_hex(stored.substr(b + 1, c - b - 1), salt)) return false;
    key_hex = stored.substr(c + 1);
    std::vector<unsigned char> key;
    return pw_from_hex(key_hex, key) && key.size() == (size_t)PW_KEY_BYTES;
}

// Does the typed password match the stored value? For a hash: hash again with the same salt and iterations and compare.
// For an old plain text value: compare directly (case-sensitive, like the old SQL BINARY compare).
inline bool verify_password(const std::string& pw, const std::string& stored) {
    if (!is_password_hash(stored)) return !stored.empty() && pw_equal(pw, stored);
    unsigned long iterations = 0;
    std::vector<unsigned char> salt;
    std::string key_hex;
    if (!pw_parse(stored, iterations, salt, key_hex)) return false;
    unsigned char key[PW_KEY_BYTES];
    if (!pbkdf2_sha256(pw, salt.data(), (unsigned long)salt.size(), iterations, key, sizeof(key))) return false;
    return pw_equal(pw_to_hex(key, sizeof(key)), key_hex);
}

// After a successful login: should this user's password be saved again in the current format?
// Yes for plain text, and for a hash made with fewer iterations than PW_HASH_ITERATIONS.
inline bool password_needs_rehash(const std::string& stored) {
    unsigned long iterations = 0;
    std::vector<unsigned char> salt;
    std::string key_hex;
    if (!pw_parse(stored, iterations, salt, key_hex)) return true;
    return iterations < PW_HASH_ITERATIONS;
}

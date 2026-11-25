// password_hash.h - passwords ab plain text me nahi, PBKDF2-HMAC-SHA256 hash ban kar DB me jaate hain.
//
// DB me kya rehta hai (users.password, VARCHAR(255) me aaram se aata hai, ~120 characters):
//     pbkdf2_sha256$<iterations>$<salt: 32 hex>$<hash: 64 hex>
// - salt: har password ka apna 16 random bytes (secure_random). Do users ka same password ho to bhi hash alag,
//   aur pehle se bani "rainbow table" kaam nahi aati.
// - iterations: hash ko jaan-boojh kar dheema banata hai, taaki DB leak hone par har guess mehenga pade.
//   Number hash ke andar likha hai, to baad me badhaya ja sakta hai: purane hash chalte rahenge, aur login par naye number
//   se dobara ban jaayenge (password_needs_rehash).
// - Purane users (jo hashing se pehle bane) ke paas plain text password hai. verify_password unhe bhi maanta hai,
//   aur login hote hi pages.cpp unka password hash me badal deta hai. Is tarah koi migration script nahi chahiye.
//
// Kyun PBKDF2: Windows (bcrypt.dll) aur Linux (OpenSSL) dono me pehle se hai, koi nayi library nahi chahiye.
// (Argon2 / bcrypt behtar hain par unke liye libsodium jaisi nayi dependency lagti.)
#pragma once
#include "platform.h"   // secure_random, pbkdf2_sha256
#include <string>
#include <vector>

const char* const PW_HASH_PREFIX = "pbkdf2_sha256$";
// OWASP 600,000 bolta hai, par Render free plan ka CPU bahut chhota hai (har login itna kaam karta hai, aur sirf 4 workers hain).
// 100,000 = wahan bhi login lagbhag aadhe second ke andar. Hash me number likha hai, to baad me badhana aasaan hai.
const unsigned long PW_HASH_ITERATIONS = 100000;
const int PW_SALT_BYTES = 16;
const int PW_KEY_BYTES = 32;    // SHA-256 ka output size

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
        return -1;      // sirf chhote hex akshar: hum khud aise hi likhte hain
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = val(s[i]), lo = val(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((unsigned char)(hi * 16 + lo));
    }
    return true;
}

// dono string barabar hain? Har byte dekhta hai chahe pehla hi alag ho, taaki "kitni der laga" se pata na chale
// ki kitne shuru ke characters sahi the (timing attack)
inline bool pw_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

inline bool is_password_hash(const std::string& stored) { return stored.rfind(PW_HASH_PREFIX, 0) == 0; }

// naya hash (signup, password badalna, purane plain text ko badalna). Error par "" (caller 500 dikhaye, kabhi plain text save na kare)
inline std::string hash_password(const std::string& pw, unsigned long iterations = PW_HASH_ITERATIONS) {
    unsigned char salt[PW_SALT_BYTES];
    unsigned char key[PW_KEY_BYTES];
    if (!secure_random(salt, sizeof(salt))) return "";
    if (!pbkdf2_sha256(pw, salt, sizeof(salt), iterations, key, sizeof(key))) return "";
    return std::string(PW_HASH_PREFIX) + std::to_string(iterations) + "$" + pw_to_hex(salt, sizeof(salt)) + "$" +
           pw_to_hex(key, sizeof(key));
}

// hash ke hisse alag karo. Galat shakal = false
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

// typed password, DB wali value se milta hai? Hash ho to wahi salt + iterations se dobara hash karke milao.
// Hash nahi (purana user) to plain text compare (capital/small ka farq, pehle ke BINARY compare jaisa).
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

// login sahi hone ke baad: kya is user ka password dobara (naye tareeke se) save karna chahiye?
// Plain text ho, ya kam iterations wala purana hash ho, to haan
inline bool password_needs_rehash(const std::string& stored) {
    unsigned long iterations = 0;
    std::vector<unsigned char> salt;
    std::string key_hex;
    if (!pw_parse(stored, iterations, salt, key_hex)) return true;
    return iterations < PW_HASH_ITERATIONS;
}

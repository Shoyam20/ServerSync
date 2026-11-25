# test_password_hash.py - builds and runs tests/password_hash_check.cpp: PBKDF2 known answers, hash / verify,
# old plain text passwords, broken stored values. No database, no server, no db.conf.
# Also checks the C++ hash against Python's own hashlib, so both sides agree on the format.
#     python tests\test_password_hash.py
import hashlib, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="serversync_pwhash_")
exe = os.path.join(work, "pwhash" + (".exe" if os.name == "nt" else ""))
libs = ["-lbcrypt", "-static-libgcc", "-static-libstdc++"] if os.name == "nt" else ["-lcrypto"]
r = subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-o", exe, os.path.join(ROOT, "tests", "password_hash_check.cpp"),
                    "-I" + os.path.join(ROOT, "include")] + libs + (["-lws2_32"] if os.name == "nt" else []), capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)

out = subprocess.run([exe], capture_output=True, text=True).stdout
print(out)
fails = out.count("FAIL ")

# the stored hash must be exactly PBKDF2-HMAC-SHA256(password, salt, iterations) -> recompute it in Python
line = next((l for l in out.splitlines() if l.startswith("HASH ")), "")
try:
    algo, iters, salt_hex, key_hex = line[5:].split("$")
    same = algo == "pbkdf2_sha256" and hashlib.pbkdf2_hmac("sha256", b"Zx9#kLm2Qw", bytes.fromhex(salt_hex), int(iters)).hex() == key_hex
except ValueError:
    same = False
print(("PASS " if same else "FAIL ") + "python hashlib gives the same hash for the same salt + iterations")
fails += 0 if same else 1

ms = next((int(l.split()[1]) for l in out.splitlines() if l.startswith("HASH_MS ")), -1)
print("one hash took %d ms on this machine" % ms)
if "ALL OK" not in out:
    fails += 1
sys.exit(1 if fails else 0)

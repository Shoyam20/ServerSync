# test_server.py - builds and runs tests/server_check.cpp: the request parsing helpers in src/server.cpp, whole requests
# over a loopback socket (413 / 431 / 501 / 400 / 408), and static files with GET and HEAD.
# No server, no database, no db.conf. Takes a few seconds (the 408 check waits for a 1 s timeout).
#     python tests\test_server.py
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="serversync_server_")
exe = os.path.join(work, "server_check" + (".exe" if os.name == "nt" else ""))
libs = ["-lws2_32", "-lbcrypt", "-static-libgcc", "-static-libstdc++"] if os.name == "nt" else ["-pthread"]
r = subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-o", exe, os.path.join(ROOT, "tests", "server_check.cpp"),
                    "-I" + os.path.join(ROOT, "include")] + libs, capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)

p = subprocess.run([exe], capture_output=True, text=True, timeout=120)
print(p.stdout)
sys.exit(0 if p.returncode == 0 and "ALL OK" in p.stdout and "FAIL " not in p.stdout else 1)

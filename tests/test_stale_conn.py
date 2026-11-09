# test_stale_conn.py - step P4: open_conn() and the mysql_ping in get_conn() (src/db.cpp), on a throwaway MySQL (see throwaway_mysql.py;
# never your real database, never db.conf). MySQL closes idle connections (wait_timeout) and drops them all on a restart; the pool must
# notice, log it and open a fresh connection instead of failing the next request.
#     python tests\test_stale_conn.py
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as t

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MYSQL_DIR = r"C:\Program Files\MySQL\MySQL Server 8.0"
exe = os.path.join(os.environ.get("TEMP", "."), "stale_conn_check.exe")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  | " + str(detail)[:300]))
    if not ok:
        fails += 1


r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "stale_conn_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-I" + MYSQL_DIR + "\\include", "-L" + MYSQL_DIR + "\\lib", "-lmysql", "-lws2_32", "-lbcrypt"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
env = dict(os.environ)
env["PATH"] = MYSQL_DIR + "\\lib;" + env["PATH"]


def run(mode):
    p = subprocess.run([exe, mode], capture_output=True, text=True, env=env, timeout=120)
    res = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition(" ")
        res[k] = v
    return res, p.stderr


t.start()
try:
    t.run("CREATE DATABASE ecommerce_db")

    res, err = run("healthy")
    check("healthy connections: no reconnect, nothing logged, the same 4 connections are reused",
          res["QUERIES_OK"] == "8 of 8" and res["LOST_LINES"] == "0" and res["IDS_CHANGED"] == "0" and res["FREE"] == "4 of 4", (res, err[-300:]))

    res, err = run("kill")
    check("MySQL killed all 4 pooled connections: every query still works, 4 'lost ... reconnecting' lines, 4 new connections",
          res["QUERIES_OK"] == "8 of 8" and res["LOST_LINES"] == "4" and res["IDS_CHANGED"] == "1" and res["FREE"] == "4 of 4", (res, err[-400:]))
    check("the log line says which connection was lost and why", "conn " in err and "lost (" in err and "reconnecting" in err, err[-300:])

    res, err = run("timeout")
    check("idle connections timed out (wait_timeout = 2 s, slept 4 s): all queries still work after reconnecting",
          res["QUERIES_OK"] == "8 of 8" and res["LOST_LINES"] == "4" and res["IDS_CHANGED"] == "1", (res, err[-400:]))

    res, err = run("down")
    check("MySQL unreachable at that moment: no crash, the query fails cleanly and the connection goes back to the pool",
          res["DOWN_QUERY_WORKED"].startswith("0 free_after=4"), res)
    check("...and as soon as MySQL is reachable again the pool recovers on its own (all later queries work)",
          res["QUERIES_OK"] == "8 of 8" and res["FREE"] == "4 of 4", res)
finally:
    t.stop()

SRC = open(os.path.join(ROOT, "src", "db.cpp"), encoding="utf-8").read()
check("open_conn() is the only place that opens a MySQL connection (init_db and get_conn use it)",
      SRC.count("mysql_real_connect(") == 1 and "MYSQL* m = open_conn();" in SRC and "MYSQL* fresh = open_conn();" in SRC)
check("no DB_* environment variables and no Linux-only code were added", "getenv" not in SRC and "#ifdef" not in SRC and "/dev/urandom" not in SRC)
gi = open(os.path.join(ROOT, ".gitignore"), encoding="utf-8").read().split()
check(".gitignore has libmysql.dll, /admin.txt, /c.txt, /race.txt and old/ (db.conf, server.exe stay ignored)",
      all(x in gi for x in ("libmysql.dll", "/admin.txt", "/c.txt", "/race.txt", "old/", "db.conf", "server.exe")))
live = open(os.path.join(ROOT, "tests", "test_live_flow.py"), encoding="utf-8").read()
check("tests/test_live_flow.py takes any base URL from the command line and names no deployed site",
      "BASE = sys.argv[1]" in live and "onrender" not in live and "localhost:8080" in live)

print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)

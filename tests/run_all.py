# run_all.py - runs every offline test (the ones that need NO running server) one after another and prints a summary.
# The tests that talk to a running server (test_auth, test_cart, test_checkout_http, test_live_flow, test_metrics_http,
# test_static_http) are skipped here because they need server.exe + a database started by you.
# Exit code 1 if any test failed, so GitHub Actions marks the run red.
#     python tests\run_all.py              (all offline tests)
#     python tests\run_all.py password cart (only files whose name contains one of these words)
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NEEDS_SERVER = {"test_auth.py", "test_cart.py", "test_checkout_http.py", "test_live_flow.py",
                "test_metrics_http.py", "test_static_http.py"}
TIMEOUT = 900   # seconds per test file; the browser tests are the slow ones

names = sorted(f for f in os.listdir(HERE) if f.startswith("test_") and f.endswith(".py") and f not in NEEDS_SERVER)
if len(sys.argv) > 1:
    names = [n for n in names if any(w in n for w in sys.argv[1:])]

results = []
for name in names:
    print("\n=================== " + name, flush=True)
    start = time.time()
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, name)], cwd=os.path.dirname(HERE), timeout=TIMEOUT)
        ok = r.returncode == 0
    except subprocess.TimeoutExpired:
        print("TIMEOUT after %d s" % TIMEOUT)
        ok = False
    results.append((name, ok, time.time() - start))

print("\n=================== summary")
for name, ok, secs in results:
    print("%-4s %-36s %6.1f s" % ("PASS" if ok else "FAIL", name, secs))
failed = [n for n, ok, _ in results if not ok]
print("%d of %d test files passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

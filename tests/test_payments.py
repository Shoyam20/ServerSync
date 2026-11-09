# test_payments.py - payment methods (step P1): the REAL do_checkout() and order_change_by() from src/db.cpp on a throwaway MySQL
# (see throwaway_mysql.py; never your real database, never db.conf). Checks the new payments columns / CHECKs, that a declined payment
# rolls back the WHOLE checkout, that online payments are PAID inside the checkout transaction, and the DELIVERED / CANCELLED rules.
#     python tests\test_payments.py
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as t

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MYSQL_DIR = r"C:\Program Files\MySQL\MySQL Server 8.0"
exe = os.path.join(os.environ.get("TEMP", "."), "payments_check.exe")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  | " + str(detail)[:300]))
    if not ok:
        fails += 1


r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "payments_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-I" + MYSQL_DIR + "\\include", "-L" + MYSQL_DIR + "\\lib", "-lmysql", "-lws2_32", "-lbcrypt"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)

t.start()
try:
    t.run(open(os.path.join(ROOT, "sql", "ecommerce_db.sql"), encoding="utf-8").read())
    q = lambda sql: t.run(sql, db="ecommerce_db")
    bad = lambda sql: t.run(sql, db="ecommerce_db", expect_error=True)[0]

    # ---- the schema ----
    cols = q("SELECT COLUMN_NAME, IS_NULLABLE, COLUMN_DEFAULT, CHARACTER_MAXIMUM_LENGTH FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='ecommerce_db' "
             "AND TABLE_NAME='payments' AND COLUMN_NAME IN ('method','pay_detail','txn_ref') ORDER BY COLUMN_NAME").split("\n")
    check("payments.method NOT NULL default COD; pay_detail VARCHAR(80) NULL; txn_ref VARCHAR(30) NULL",
          cols == ["method\tNO\tCOD\t30", "pay_detail\tYES\tNULL\t80", "txn_ref\tYES\tNULL\t30"], cols)
    q("INSERT INTO users(email,name,password) VALUES('a@x.in','Ann','Zx9#kLm2Qw')")
    # the seed already has products 1..52: the harness uses product 1, so give it a known stock and price
    q("UPDATE products SET stock = 50, price = 100.00 WHERE product_id = 1")
    q("INSERT INTO addresses(user_id,first_name,last_name,phone,line1,city,state_id,pincode,address_type,is_default) "
      "VALUES(1,'Ann','Lee','9876543210','12 MG Road','Dehradun',27,'248001','HOME',1)")
    q("INSERT INTO cart(user_id,status) VALUES(1,'CHECKED_OUT')")      # an old cart, so the harness's carts are not the first one
    check("method CHECK: UPI / CARD / NETBANKING / COD accepted, other values and NULL refused",
          bad("INSERT INTO orders(user_id,cart_id,total) VALUES(1,1,1); INSERT INTO payments(order_id,amount,method) VALUES(1,1,'BITCOIN')")
          and bad("INSERT INTO payments(order_id,amount,method) VALUES(1,1,NULL)"))
    q("DELETE FROM payments; DELETE FROM orders")

    # ---- the real checkout / order code ----
    env = dict(os.environ)
    env["PATH"] = MYSQL_DIR + "\\lib;" + env["PATH"]
    p = subprocess.run([exe], capture_output=True, text=True, env=env)
    res = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition(" ")
        res[k] = v
    print(p.stderr[-600:] if "DELIVER_FAIL" in p.stdout or "DECLINED" not in res else "")

    check("declined payment -> checkout fails with the bank message (409), no order id", res["DECLINED"].startswith("409 order=0 Payment declined by the bank"), res.get("DECLINED"))
    check("declined payment rolled back EVERYTHING: no order, no payment, no address copy, stock back, cart still ACTIVE",
          res["DECLINED_LEFTOVERS"] == "orders=0 payments=0 order_addresses=0 stock_same=1 cart=ACTIVE", res.get("DECLINED_LEFTOVERS"))
    check("all five checkouts worked (COD, UPI, CARD, NETBANKING, COD)", all(res[k].startswith("200") for k in ("COD", "UPI", "CARD", "NB", "COD2")), res)
    cod, upi, card, nb, cod2 = res["IDS"].split()

    def pay(order):
        return q("SELECT method, IFNULL(pay_detail,'~'), IFNULL(txn_ref,'~'), status, IF(paid_at IS NULL,'no','yes') FROM payments WHERE order_id=%s" % order)

    check("COD (default argument): PENDING with paid_at NULL right after checkout, no detail, no txn_ref",
          res["COD_PAY"] == "PENDING/NULL" and q("SELECT COUNT(*) FROM payments WHERE order_id=%s AND method='COD' AND pay_detail IS NULL AND txn_ref IS NULL" % cod) == "1", res.get("COD_PAY"))
    check("UPI: PAID at checkout with detail + txn_ref + paid_at", pay(upi) == "UPI\tann@okbank\tTXN0001\tPAID\tyes", pay(upi))
    check("CARD: only brand + last 4 digits stored, PAID", pay(card).startswith("CARD\tVISA **** 1111\tTXN0002\t") , pay(card))
    check("NETBANKING: bank name stored, PAID, paid_at set (the order was not touched afterwards)", pay(nb) == "NETBANKING\tHDFC Bank\tTXN0003\tPAID\tyes", pay(nb))
    check("the payment amount equals the order total for every order",
          q("SELECT COUNT(*) FROM payments p JOIN orders o ON o.order_id = p.order_id WHERE p.amount <> o.total") == "0")

    check("DELIVERED: the COD payment becomes PAID with a paid_at", pay(cod) == "COD\t~\t~\tPAID\tyes", pay(cod))
    check("DELIVERED: an online payment is NOT changed - it keeps its original paid_at",
          q("SELECT paid_at, status FROM payments WHERE order_id=%s" % upi) == "2020-01-01 00:00:00\tPAID", q("SELECT paid_at, status FROM payments WHERE order_id=%s" % upi))
    check("CANCELLED: online payment (CARD) -> REFUNDED, its detail and txn_ref are kept", pay(card).split("\t")[3] == "REFUNDED" and "TXN0002" in pay(card), pay(card))
    check("CANCELLED: COD payment -> CANCELLED (no money was ever taken)", pay(cod2).split("\t")[3] == "CANCELLED", pay(cod2))
    check("an order that is still PLACED keeps its PAID online payment", pay(nb).split("\t")[3] == "PAID")
    check("cancelling still gave the stock back (only the CARD and COD2 orders were cancelled: 4 items)",
          q("SELECT stock FROM products WHERE product_id=1") == str(50 - 2 * 5 + 2 * 2), q("SELECT stock FROM products WHERE product_id=1"))
    check("order history rows are still written with every change",
          q("SELECT COUNT(*) FROM order_status_history WHERE order_id=%s" % cod) == "5")
finally:
    t.stop()
print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)

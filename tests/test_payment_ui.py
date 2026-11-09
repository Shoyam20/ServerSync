# test_payment_ui.py - step P2: the payment panel on the checkout page, the server checks (read_payment), and how orders show a payment.
# The real pages (tests/pages_render_check.cpp: real templates, real route(), fake DB answers; do_checkout is a stub that only records what the
# page handed to it). No server, no database, no db.conf.     python tests\test_payment_ui.py
import os, re, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="serversync_payui_")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  | " + str(detail)[:300]))
    if not ok:
        fails += 1


exe = os.path.join(work, "prc.exe")
r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "pages_render_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-lws2_32", "-lbcrypt", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
out = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", cwd=ROOT).stdout

# every page, plus the PAY_CALL line(s) printed while it was being handled (the line comes BEFORE the page's "=== name" header)
pages, calls, pending = {}, {}, []
lines = out.split("\n")
i = 0
while i < len(lines):
    line = lines[i]
    if line.startswith("PAY_CALL "):
        pending.append(line[9:])
    m = re.match(r"=== (\S+)$", line)
    if m and m.group(1) != "END":
        name = m.group(1)
        j = i
        while lines[j] != "BODY":
            j += 1
        k = j + 1
        body = []
        while lines[k] != "=== END":
            body.append(lines[k])
            k += 1
        status = int(re.match(r"STATUS (\d+)", lines[i + 1]).group(1))
        pages[name] = dict(status=status, body="\n".join(body), extra=lines[i + 3])
        calls[name] = pending
        pending = []
        i = k
    i += 1


def pg(n):
    return pages[n]


def flash(n):
    m = re.search(r'<div class="flash err">(.*?)</div>', pg(n)["body"], re.S)
    return m.group(1) if m else ""


def call(n):
    return calls[n][-1].split("|") if calls[n] else None      # [address, method, detail, txn, ok/declined]


expect_msgs = {
    "pay_upi_bad1": "valid UPI ID", "pay_upi_bad2": "valid UPI ID", "pay_upi_bad3": "valid UPI ID", "pay_upi_empty": "valid UPI ID",
    "pay_card_luhn": "card number is not valid", "pay_card_letters": "only have digits", "pay_card_short": "card number is not valid",
    "pay_card_long": "card number is not valid", "pay_card_noname": "name printed on the card", "pay_card_past": "valid expiry",
    "pay_card_month13": "valid expiry", "pay_card_expiry_fmt": "valid expiry", "pay_card_cvv2": "CVV must be 3 digits",
    "pay_card_cvv_letters": "CVV must be 3 digits", "pay_card_amex_cvv3": "CVV must be 4 digits", "pay_card_expiry_last_month": "valid expiry",
    "pay_nb_bad": "choose your bank", "pay_nb_none": "choose your bank", "pay_method_bad": "choose a payment method",
    "pay_method_none": "choose a payment method"}

print("--- the checkout payment panel")
body = pg("pay_checkout_get")["body"]
check("panel lists COD, UPI, Card and Net Banking as radio options; COD is chosen by default",
      all('name="pay_method" value="%s"' % v in body for v in ("COD", "UPI", "CARD", "NETBANKING")) and 'value="COD" checked' in body
      and not re.search(r'value="(UPI|CARD|NETBANKING)" checked', body))
check("test-mode note shows the test card, the declined card and the declined UPI ID",
      "Test mode" in body and "4111 1111 1111 1111" in body and "4000 0000 0000 0002" in body and "fail@upi" in body)
check("each method has its own fields (UPI id; card number, name, expiry, CVV; bank list) and Place order is still there",
      all(x in body for x in ('name="upi_id"', 'name="card_number"', 'name="card_name"', 'name="card_expiry"', 'name="card_cvv"', 'name="bank"', 'class="buy">Place order'))
      and body.count('<option value="') >= 7)
check("the card number and CVV inputs have NO value, and CVV is a password box",
      not re.search(r'name="card_number"[^>]*value=', body) and not re.search(r'name="card_cvv"[^>]*value=', body) and 'name="card_cvv" type="password"' in body)
CSS = open(os.path.join(ROOT, "public", "style.css"), encoding="utf-8").read()
check("works without JavaScript: only the chosen method's fields show, done in CSS (:has) and the page needs no script for it",
      '.pay:has(input[value="CARD"]:checked) .pay-card' in CSS and "@supports selector(:has(a))" in CSS and "app.js" not in open(os.path.join(ROOT, "templates", "checkout_payment.html"), encoding="utf-8").read())

print("--- server checks (read_payment)")
check("COD -> do_checkout gets method COD, no detail, no txn id", call("pay_cod") == ["2", "COD", "", "", "ok"], calls["pay_cod"])
check("UPI: ID is trimmed and lower-cased (Ravi.Kumar@OkAxis -> ravi.kumar@okaxis)", call("pay_upi_ok")[:3] == ["2", "UPI", "ravi.kumar@okaxis"])
check("bad UPI IDs (no @, space, digits in the bank part, empty) -> 400 and NO order attempt",
      all(pg(n)["status"] == 400 and call(n) is None and expect_msgs[n] in flash(n) for n in ("pay_upi_bad1", "pay_upi_bad2", "pay_upi_bad3", "pay_upi_empty")))
check("card with spaces is accepted; only 'VISA **** 1111' goes on (brand + last 4)", call("pay_card_ok")[:3] == ["2", "CARD", "VISA **** 1111"])
check("brands: Visa, Mastercard, Amex, RuPay", call("pay_card_mc")[2] == "MASTERCARD **** 4444" and call("pay_card_amex")[2] == "AMEX **** 0005"
      and call("pay_card_rupay")[2] == "RUPAY **** 0007" and call("pay_card_ok")[2].startswith("VISA"), [call(n) for n in ("pay_card_mc", "pay_card_amex", "pay_card_rupay")])
check("13-digit card is accepted (13 is the minimum); 12 and 20 digits are refused",
      call("pay_card_13") is not None and expect_msgs["pay_card_short"] in flash("pay_card_short") and expect_msgs["pay_card_long"] in flash("pay_card_long"))
check("Luhn: a wrong last digit is refused", pg("pay_card_luhn")["status"] == 400 and "not valid" in flash("pay_card_luhn") and call("pay_card_luhn") is None)
check("card number with letters is refused", pg("pay_card_letters")["status"] == 400 and "only have digits" in flash("pay_card_letters"))
check("empty name / past expiry / month 13 / wrong format / last month are refused; this month and 12/99 are accepted",
      all(pg(n)["status"] == 400 and expect_msgs[n] in flash(n) and call(n) is None for n in ("pay_card_noname", "pay_card_past", "pay_card_month13", "pay_card_expiry_fmt", "pay_card_expiry_last_month"))
      and call("pay_card_expiry_this_month") is not None and call("pay_card_ok") is not None)
check("CVV: 3 digits (not 2, not letters); Amex needs 4 (and 3 is refused)",
      all(pg(n)["status"] == 400 and expect_msgs[n] in flash(n) for n in ("pay_card_cvv2", "pay_card_cvv_letters", "pay_card_amex_cvv3")) and call("pay_card_amex") is not None)
check("Net Banking: bank code -> bank name; unknown / missing bank refused", call("pay_nb_ok")[:3] == ["2", "NETBANKING", "HDFC Bank"]
      and "choose your bank" in flash("pay_nb_bad") and "choose your bank" in flash("pay_nb_none") and call("pay_nb_bad") is None)
check("unknown or missing payment method refused", all("choose a payment method" in flash(n) and call(n) is None for n in ("pay_method_bad", "pay_method_none")))
check("test values: card 4000000000000002 and fail@upi are sent as 'declined'", call("pay_card_declined")[4] == "declined" and call("pay_upi_declined")[4] == "declined"
      and call("pay_upi_ok")[4] == "ok" and call("pay_card_ok")[4] == "ok")
check("a bank decline (do_checkout 409) shows the normal 'order not placed' page with the message",
      pg("pay_declined_page")["status"] == 409 and "Payment declined by the bank" in flash("pay_declined_page") and "Order not placed" in pg("pay_declined_page")["body"])

print("--- errors keep the checkout page filled in (400)")
for n in ("pay_err_keep_upi", "pay_err_keep_card", "pay_err_keep_bank"):
    check("%s: 400, the checkout page again, no order attempted" % n, pg(n)["status"] == 400 and "<h1>Confirm your order</h1>" in pg(n)["body"] and call(n) is None)
b = pg("pay_err_keep_upi")["body"]
check("UPI error: address 2 and UPI are still selected, the typed ID is back (escaped)",
      re.search(r'name="address_id" value="2" checked', b) and 'name="address_id" value="1" checked' not in b and 'value="UPI" checked' in b and 'value="COD" checked' not in b and 'value="bad id"' in b)
b = pg("pay_err_keep_card")["body"]
check("card error: card is still selected; name (escaped) and expiry are back", 'value="CARD" checked' in b and 'value="Ann &lt;b&gt;Lee"' in b and 'name="card_expiry" value="12/99"' in b)
check("card error: the card number and the CVV are NOT sent back", "4111111111111112" not in b and 'value="987"' not in b and ">987<" not in b
      and not re.search(r'name="card_number"[^>]*value=', b) and not re.search(r'name="card_cvv"[^>]*value=', b))
check("bank error: Net Banking still selected", 'value="NETBANKING" checked' in pg("pay_err_keep_bank")["body"])

print("--- card data is never stored, logged or echoed")
NL = chr(10)
SECRETS = ["5555555555554444", "378282246310005", "6521000000000007", "4111111111111112", "4000000000000002", "4111111111111111", "5555-5555-5555-4444"]
check("no unspaced card number appears in any page, log line or recorded payment (the test note only shows the spaced test cards)",
      not any(sct in NL.join(l for l in out.split(NL) if not l.startswith("LUHN ")) for sct in SECRETS))   # (the LUHN lines are the test's own checksum probes)
check("what is handed to do_checkout is only 'BRAND **** last4' (never a full number)",
      all(re.fullmatch(r"\d+\|CARD\|(VISA|MASTERCARD|AMEX|RUPAY) \*\*\*\* \d{4}\|TXN[0-9A-F]{12}\|(ok|declined)", c) for n in calls for c in calls[n] if "|CARD|" in c))
SRC = open(os.path.join(ROOT, "src", "pages.cpp"), encoding="utf-8").read() + open(os.path.join(ROOT, "src", "db.cpp"), encoding="utf-8").read()
log_lines = [l for l in SRC.split("\n") if "log_tag(" in l or "log_msg(" in l or "cout" in l]
check("no log statement in pages.cpp / db.cpp mentions card_number, card_cvv or cvv", not any(re.search(r"card_number|card_cvv|cvv|\bnum\b", l, re.I) for l in log_lines), [l for l in log_lines if re.search(r"card_number|cvv", l, re.I)])
check("the payments INSERT writes only method, pay_detail, txn_ref (no card number / cvv column exists)",
      "pay.detail, pay.txn_ref" in SRC and "card_number" not in open(os.path.join(ROOT, "sql", "ecommerce_db.sql"), encoding="utf-8").read())

print("--- Luhn checksum and transaction ids")
luhn = {m.group(1): m.group(2) for m in re.finditer(r"^LUHN (\d+) (\d)$", out, re.M)}
check("Luhn accepts the known good test cards and the textbook example 79927398713",
      all(luhn[n] == "1" for n in ("4111111111111111", "5555555555554444", "378282246310005", "6521000000000007", "4222222222222", "4000000000000002", "79927398713")), luhn)
check("Luhn refuses 4111111111111112 and 79927398710", luhn["4111111111111112"] == "0" and luhn["79927398710"] == "0")
check("Luhn catches EVERY single wrong digit (144 of 144 changes on a 16-digit number)", "LUHN_SINGLE_DIGIT tried=144 missed=0" in out, re.search(r"LUHN_SINGLE_DIGIT.*", out).group(0))
txns = [c.split("|")[3] for n in calls for c in calls[n] if c.split("|")[1] != "COD"]
check("txn id = 'TXN' + 12 hex characters, and every one is different (%d online payments)" % len(txns),
      len(txns) >= 8 and all(re.fullmatch(r"TXN[0-9A-F]{12}", t) for t in txns) and len(set(txns)) == len(txns), txns)
check("txn ids come from the secure random used for session ids (no rand())", "new_session_id(hex)" in SRC.split("static string new_txn_ref()")[1].split("}")[0] and not re.search(r"(?<![A-Za-z_])s?rand\(", NL.join(l.split("//")[0] for l in SRC.split(NL))))

print("--- order page and admin order page")
b = pg("pay_order_upi")["body"]
check("order page (UPI): method + detail, PAID pill, paid time, Txn id",
      "UPI (ann@okbank)" in b and '<span class="pill pill-paid">PAID</span>' in b and ", paid 2026-10-24 10:00:05" in b and ", Txn TXN1A2B3C4D5E6F" in b)
b = pg("pay_order_refunded")["body"]
check("order page (cancelled online order): Card (VISA **** 1111) with a REFUNDED pill", "Card (VISA **** 1111)" in b and '<span class="pill pill-refunded">REFUNDED</span>' in b and "Txn TXN0000AAAA11" in b)
b = pg("pay_order_cod")["body"]
check("order page (COD): Cash on Delivery, PENDING pill, no paid time, no Txn", "Cash on Delivery" in b and 'pill pill-pending">PENDING' in b and ", paid" not in b and "Txn" not in b)
check("order page: the bank name is escaped", "Net Banking (HDFC Bank &lt;b&gt;)" in pg("pay_order_nb")["body"])
for n, want in (("pay_admin_refunded", "Card (VISA **** 1111)"), ("pay_admin_upi", "UPI (ann@okbank)")):
    b = pg(n)["body"]
    check("admin order page %s: method + detail, status pill, paid time, Txn id" % n, want in b and "Txn TXN" in b and ", paid 2026-10-24 10:00:05" in b)
check("admin order page shows the REFUNDED pill", 'pill pill-refunded">REFUNDED' in pg("pay_admin_refunded")["body"])
ACSS = open(os.path.join(ROOT, "public", "admin.css"), encoding="utf-8").read()
check("pill-refunded has a style in style.css and admin.css", ".pill-refunded {" in CSS and ".pill-refunded {" in ACSS)

print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)

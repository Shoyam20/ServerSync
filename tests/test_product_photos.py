# test_product_photos.py - step P3, product photos:
#   A. the 52 files themselves (public/images/products/1..52.jpg) and docs/image_credits.md
#   B. the REAL static code of src/server.cpp serves them over a socket (.jpg / .jpeg -> image/jpeg, byte-identical)
#   C. the REAL pages (tests/pages_render_check.cpp): a photo when the file exists, the category emoji when it does not
# No server, no database, no db.conf.     python tests\test_product_photos.py
import os, re, shutil, struct, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix="serversync_photos_")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  | " + str(detail)[:300]))
    if not ok:
        fails += 1


IMG = os.path.join(ROOT, "public", "images", "products")


def jpeg_size(path):
    """width, height from the SOF marker (no imaging library needed); None if it is not a JPEG"""
    d = open(path, "rb").read()
    if d[:3] != b"\xff\xd8\xff":
        return None
    i = 2
    while i < len(d) - 9:
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", d[i + 5:i + 9])
            return w, h
        i += 2 + struct.unpack(">H", d[i + 2:i + 4])[0]
    return None


print("--- A. the photo files")
files = sorted(os.listdir(IMG))
check("exactly 1.jpg ... 52.jpg (the 52 seed products)", files == sorted("%d.jpg" % n for n in range(1, 53)), files[:5])
sizes = {f: jpeg_size(os.path.join(IMG, f)) for f in files}
check("every file is a real JPEG", all(sizes.values()), [f for f, v in sizes.items() if not v])
check("every photo is 600x450 (as the credits say)", all(v == (600, 450) for v in sizes.values() if v), {f: v for f, v in sizes.items() if v != (600, 450)})
big = [f for f in files if os.path.getsize(os.path.join(IMG, f)) > 2 * 1024 * 1024]
check("every photo is under the 2 MB limit of the static loader (they are kept in memory)", not big, big)
credits = open(os.path.join(ROOT, "docs", "image_credits.md"), encoding="utf-8").read()
rows = re.findall(r"^\| (\d+) \| ", credits, re.M)
licenses = re.findall(r"^\| \d+ \|.*\| ([^|]+) \|$", credits, re.M)
check("docs/image_credits.md has one row per photo (52), each with an author link and a license (CC, GFDL or public domain)",
      sorted(map(int, rows)) == list(range(1, 53)) and len(licenses) == 52 and all(l.strip() for l in licenses) and credits.count("commons.wikimedia.org") >= 52)
seed = open(os.path.join(ROOT, "sql", "ecommerce_db.sql"), encoding="utf-8").read()
names = re.findall(r"^\('([^']+)', '", seed[seed.index("INSERT INTO products"):], re.M)
cred_names = re.findall(r"^\| \d+ \| ([^|]+) \|", credits, re.M)
check("the credits list the seed products in the same order as the SQL seed", [n.strip() for n in cred_names] == names[:52], (names[:3], cred_names[:3]))

print("--- B. the static server")
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
exe = os.path.join(work, "photos_check.exe")
r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "photos_check.cpp"), "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(BASE, "include"),
                    "-lws2_32", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
# a folder with upper / lower case extensions, an unknown type and a name with a space
tmp = os.path.join(work, "pub")
os.makedirs(os.path.join(tmp, "images", "products"))
shutil.copy(os.path.join(IMG, "1.jpg"), os.path.join(tmp, "images", "products", "a.JPEG"))
shutil.copy(os.path.join(IMG, "2.jpg"), os.path.join(tmp, "images", "products", "b.jpeg"))
shutil.copy(os.path.join(IMG, "3.jpg"), os.path.join(tmp, "images", "products", "c.jpg"))
shutil.copy(os.path.join(IMG, "4.jpg"), os.path.join(tmp, "images", "products", "d.gif"))
shutil.copy(os.path.join(IMG, "5.jpg"), os.path.join(tmp, "images", "products", "bad name.jpg"))
names = ["images/products/%d.jpg" % n for n in range(1, 53)]
p = subprocess.run([exe, os.path.join(ROOT, "public")] + names + ["images/products/53.jpg", "images/products/../../db.conf", "images/products/1.png"], capture_output=True, text=True)
out = p.stdout
http = {}
for m in re.finditer(r"^HTTP (\S+) \| (.*?) \| (.*?) \| len (\S+) \| (.*?) \| body (\d+) sum (\d+) \| disk (\d+) sum (\d+)$", out, re.M):
    http[m.group(1)] = m.groups()[1:]
check("all 52 photos are loaded: 200 OK, Content-Type image/jpeg", all(http[n][0] == "HTTP/1.1 200 OK" and http[n][1] == "image/jpeg" for n in names), [n for n in names if http[n][1] != "image/jpeg"][:3])
check("every photo arrives byte-for-byte identical to the file on disk, with the right Content-Length",
      all(http[n][4] == http[n][6] and http[n][5] == http[n][7] and http[n][2] == http[n][4] and http[n][4] != "0" for n in names))
check("photos can be cached by the browser (Cache-Control max-age)", all("max-age" in http[n][3] for n in names))
check("a photo that does not exist -> 404; ../ tricks -> 404", http["images/products/53.jpg"][0].endswith("404 Not Found") and http["images/products/../../db.conf"][0].endswith("404 Not Found"))
check("a .jpg is NOT served under another name (images/products/1.png -> 404)", http["images/products/1.png"][0].endswith("404 Not Found"))
q = subprocess.run([exe, tmp, "images/products/a.JPEG", "images/products/b.jpeg", "images/products/c.jpg", "images/products/d.gif", "images/products/bad name.jpg"], capture_output=True, text=True).stdout
t = {m.group(1): m.group(2) for m in re.finditer(r"^HTTP (\S+(?: name\.jpg)?) \| HTTP/1.1 (\d+) .*?\| (\S*) \|", q, re.M)}
types = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r"^HTTP (images/products/\S+) \| HTTP/1.1 (\d+) [^|]*\| ([^|]*) \|", q, re.M)}
check(".jpg, .jpeg and upper-case .JPEG all -> image/jpeg", all(types[n] == ("200", "image/jpeg") for n in ("images/products/a.JPEG", "images/products/b.jpeg", "images/products/c.jpg")), types)
check("unknown type (.gif) and unsafe name (space) are not served", types["images/products/d.gif"][0] == "404" and "COUNT 3" in q, q[:200])

print("--- C. the pages")
exe2 = os.path.join(work, "prc.exe")
r = subprocess.run(["g++", "-std=c++20", "-o", exe2, os.path.join(ROOT, "tests", "pages_render_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-lws2_32", "-lbcrypt", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
pout = subprocess.run([exe2], capture_output=True, text=True, encoding="utf-8", cwd=ROOT).stdout
pages = {}
for m in re.finditer(r"^=== (\S+)\nSTATUS (\d+) SENT (\d+)\nTYPE [^\n]*\nEXTRA ([^\n]*)\nPROBLEMS (\d+) ([^\n]*)\nBODY\n(.*?)\n=== END$", pout, re.S | re.M):
    pages[m.group(1)] = dict(status=int(m.group(2)), problems=int(m.group(5)), body=m.group(7))


def cards(body):
    """the thumb inside each product card: {product id: html of its .pimg}"""
    return {m.group(1): m.group(2) for m in re.finditer(r'<a class="pimg"[^>]*href="/product\?id=(\d+)"[^>]*>(.*?)</a>', body, re.S)}


b = pages["products"]["body"]
c = cards(b)
check("product card with a photo file (id 7): <img> from /static/images/products/7.jpg, lazy, no emoji",
      re.fullmatch(r'<img class="pphoto" src="/static/images/products/7\.jpg\?v=\d+" alt="" loading="lazy" decoding="async">', c["7"]), c.get("7"))
check("product card without a photo file (id 8): the category emoji, no <img>", '<span class="emoji">' in c["8"] and "<img" not in c["8"], c.get("8"))
check("no template problems on the product pages", pages["products"]["problems"] == 0 and pages["product"]["problems"] == 0 and pages["product_soldout"]["problems"] == 0)
check("product page (id 7, photo): the photo inside the .pimg box; (id 8): the emoji",
      re.search(r'<div class="pimg detail-img"[^>]*>\s*<img class="pphoto" src="/static/images/products/7\.jpg\?v=\d+"', pages["product"]["body"]) is not None
      and '<span class="emoji">' in pages["product_soldout"]["body"] and "pphoto" not in pages["product_soldout"]["body"])
check("home page budget cards use the same thumbs (photo for id 20, emoji for the others)",
      'src="/static/images/products/20.jpg' in pages["home"]["body"] and pages["home"]["body"].count('<span class="emoji">') >= 7)
check("the card keeps its link and coloured box (--h) behind the photo", 'class="pimg" style="--h:' in b and 'href="/product?id=7"' in b)
CSS = open(os.path.join(ROOT, "public", "style.css"), encoding="utf-8").read()
check("css: the photo fills the card box with object-fit: cover (box clips it, photo is positioned inside)",
      ".pphoto { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: cover; display: block; }" in CSS and ".pimg { position: relative; overflow: hidden; }" in CSS)
check("templates: thumb_photo (lazy <img>) and thumb_emoji exist and the card / product page use {{{thumb}}}",
      all(os.path.exists(os.path.join(ROOT, "templates", f)) for f in ("thumb_photo.html", "thumb_emoji.html"))
      and "{{{thumb}}}" in open(os.path.join(ROOT, "templates", "product_card.html"), encoding="utf-8").read()
      and "{{{thumb}}}" in open(os.path.join(ROOT, "templates", "product.html"), encoding="utf-8").read())

print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)

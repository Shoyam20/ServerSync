// server_check.cpp - tests for src/server.cpp. tests/test_server.py builds and runs it; it prints PASS / FAIL lines.
// server.cpp is #included, so its static functions (url_decode, parse_request, read_request, serve_static ...) can be
// called directly. Real requests go over a loopback socket (127.0.0.1), so no server, database or db.conf is needed.
#include "../src/server.cpp"
#include <chrono>
#include <thread>

// ---- stubs for what server.cpp uses from other files ----
void log_msg(const string& msg) { cerr << "[LOG] " << msg << endl; }
void log_msg_verbose(const string&) {}
string render_error_page(const string& message) { return "<p>" + message + "</p>"; }
int route(SOCKET client, Request& req) {            // every non-static path answers with its own path
    send_response(client, 200, "text/plain; charset=utf-8", "route:" + req.path);
    return 200;
}
long long now_us() { return 0; }
bool push_client(const ClientJob&) { return true; }
long long metric_rejected() { return 0; }

static int fails = 0;
static void check(const string& name, bool ok, const string& detail = "") {
    cout << (ok ? "PASS " : "FAIL ") << name << (ok || detail.empty() ? "" : "  | " + detail.substr(0, 200)) << endl;
    if (!ok) fails++;
}

// ---- loopback socket pair: 'client' is the browser side, 'server_side' is what the server code gets ----
struct Pair { SOCKET client = INVALID_SOCKET, server_side = INVALID_SOCKET; };

static Pair make_pair_sockets() {
    Pair p;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                                  // the OS picks a free port
    ::bind(l, (sockaddr*)&a, sizeof(a));
    listen(l, 1);
    sock_len_t len = sizeof(a);
    getsockname(l, (sockaddr*)&a, &len);
    p.client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(p.client, (sockaddr*)&a, sizeof(a));
    p.server_side = accept(l, nullptr, nullptr);
    close_socket(l);
    set_recv_timeout(p.server_side, 1000);           // so a missing end of headers gives 408 after 1 s
    set_recv_timeout(p.client, 1000);
    return p;
}

static void send_text(SOCKET s, const string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.c_str() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return;
        sent += n;
    }
}

// everything the client receives until the other side closes or 1 s passes without data
static string read_all(SOCKET s) {
    string out;
    char buf[4096];
    while (true) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, n);
    }
    return out;
}

// sends raw bytes as the browser, runs the real handle_client() (which reads, answers and closes), returns the answer
static string full_request(const string& raw) {
    Pair p = make_pair_sockets();
    send_text(p.client, raw);
    ClientJob job;
    job.sock = p.server_side;
    job.ip = "127.0.0.1";
    handle_client(job);                              // closes server_side
    string res = read_all(p.client);
    close_socket(p.client);
    return res;
}

static string first_line(const string& res) { return res.substr(0, res.find("\r\n")); }

int main() {
    if (!net_init()) {
        cout << "FAIL network init" << endl;
        return 1;
    }

    // ---------------- A. small parsing helpers ----------------
    size_t n = 0;
    check("to_number: 1234", to_number("1234", n) && n == 1234);
    check("to_number: refuses empty, -5, 12abc, 10 digits", !to_number("", n) && !to_number("-5", n) && !to_number("12abc", n) &&
                                                              !to_number("1234567890", n));
    string out;
    check("url_decode: + is a space, %41 is A", url_decode("a+b%41", out) && out == "a bA", out);
    check("url_decode: lowercase hex works", url_decode("%2f", out) && out == "/", out);
    check("url_decode: refuses %4 at the end, %zz and %00", !url_decode("%4", out) && !url_decode("%zz", out) && !url_decode("a%00b", out));
    map<string, string> q;
    check("parse_query: a=1&b=two+words&empty=&c", parse_query("a=1&b=two+words&empty=&c", q) && q["a"] == "1" &&
                                                     q["b"] == "two words" && q.count("empty") && q["empty"] == "" && q.count("c"));
    q.clear();
    check("parse_query: a bad %xx makes the whole query invalid", !parse_query("ok=1&bad=%zz", q));

    Request r1;
    check("request line: GET /products?cat=3 HTTP/1.1", parse_request_line("GET /products?cat=3 HTTP/1.1", r1) && r1.method == "GET" &&
                                                         r1.path == "/products" && r1.target == "/products?cat=3" && r1.params["cat"] == "3");
    Request r2, r3, r4, r5;
    check("request line: refuses lowercase method, no leading /, missing version, not HTTP/",
          !parse_request_line("get / HTTP/1.1", r2) && !parse_request_line("GET products HTTP/1.1", r3) &&
          !parse_request_line("GET /", r4) && !parse_request_line("GET / FTP/1.0", r5));

    Request r6;
    check("headers: names lowercased, values trimmed, cookies split",
          parse_request("GET / HTTP/1.1\r\nHost:  example.com \r\nCookie: a=1; session_id=xyz", r6) && r6.headers["host"] == "example.com" &&
              r6.cookies["session_id"] == "xyz" && r6.cookies["a"] == "1");
    Request r7, r8;
    check("headers: refuses a line without colon and a space inside the name",
          !parse_request("GET / HTTP/1.1\r\nNoColonHere", r7) && !parse_request("GET / HTTP/1.1\r\nBad Name: x", r8));

    check("reason: 413 / 431 / 501 have their text, unknown code says Unknown",
          reason(413) == "Payload Too Large" && reason(431) == "Request Header Fields Too Large" && reason(501) == "Not Implemented" &&
              reason(299) == "Unknown");

    // ---------------- B. whole requests over a socket (real read_request + handle_client) ----------------
    string res = full_request("GET /hello?x=1 HTTP/1.1\r\nHost: t\r\n\r\n");
    check("normal GET reaches route()", first_line(res) == "HTTP/1.1 200 OK" && res.find("route:/hello") != string::npos, first_line(res));

    res = full_request("POST /form HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 11\r\n\r\nname=a+b&x=");
    check("POST with a body is read and answered", first_line(res) == "HTTP/1.1 200 OK", first_line(res));

    res = full_request("GET / HTTP/1.1\r\nX-Big: " + string(MAX_HEADER + 100, 'a') + "\r\n\r\n");
    check("headers bigger than MAX_HEADER -> 431", first_line(res) == "HTTP/1.1 431 Request Header Fields Too Large", first_line(res));
    check("431 page says why", res.find("431 Request Header Fields Too Large") != string::npos);

    res = full_request("POST / HTTP/1.1\r\nContent-Length: " + to_string(MAX_BODY + 1) + "\r\n\r\n");
    check("Content-Length bigger than MAX_BODY -> 413", first_line(res) == "HTTP/1.1 413 Payload Too Large", first_line(res));

    res = full_request("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    check("Transfer-Encoding: chunked -> 501", first_line(res) == "HTTP/1.1 501 Not Implemented", first_line(res));

    res = full_request("POST / HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n");
    check("Content-Length that is not a number -> 400", first_line(res) == "HTTP/1.1 400 Bad Request", first_line(res));

    res = full_request("GARBAGE\r\n\r\n");
    check("broken request line -> 400", first_line(res) == "HTTP/1.1 400 Bad Request", first_line(res));

    res = full_request("GET / HTTP/1.1\r\nHost: t\r\n");     // headers never end: the server gives up after its timeout
    check("headers that never end -> 408 after the receive timeout", first_line(res) == "HTTP/1.1 408 Request Timeout", first_line(res));

    // ---------------- C. static files: GET and HEAD ----------------
    error_code ec;
    filesystem::path dir = filesystem::temp_directory_path() / "serversync_server_check";
    filesystem::remove_all(dir, ec);
    filesystem::create_directories(dir / "img", ec);
    { ofstream(dir / "style.css", ios::binary) << "body{color:red}"; }
    { ofstream(dir / "img" / "a.png", ios::binary) << string("\x89PNG\r\n\x1a\n\0\x01", 10); }
    { ofstream(dir / "notes.txt", ios::binary) << "unknown type, must be skipped"; }
    int loaded = load_static_files(dir.string());
    check("static: loads css and png (also in a subfolder), skips .txt", loaded == 2 && find_static("style.css") && find_static("img/a.png") &&
                                                                           !find_static("notes.txt"), to_string(loaded));
    check("static: refuses .., backslash, leading /", !find_static("../style.css") && !find_static("img\\a.png") && !find_static("/style.css"));

    res = full_request("GET /static/style.css HTTP/1.1\r\n\r\n");
    check("GET static: 200, right type and length, body", first_line(res) == "HTTP/1.1 200 OK" &&
                                                           res.find("Content-Type: text/css; charset=utf-8") != string::npos &&
                                                           res.find("Content-Length: 15\r\n") != string::npos &&
                                                           res.substr(res.size() - 15) == "body{color:red}", first_line(res));

    res = full_request("GET /static/img/a.png HTTP/1.1\r\n\r\n");
    check("GET static png: bytes unchanged (binary, with a 0 byte)", res.size() >= 10 && res.substr(res.size() - 10) == string("\x89PNG\r\n\x1a\n\0\x01", 10));

    res = full_request("HEAD /static/style.css HTTP/1.1\r\n\r\n");
    check("HEAD static: 200 with the same Content-Length as GET", first_line(res) == "HTTP/1.1 200 OK" &&
                                                                   res.find("Content-Length: 15\r\n") != string::npos, first_line(res));
    check("HEAD static: no body after the headers", res.size() >= 4 && res.substr(res.size() - 4) == "\r\n\r\n", to_string(res.size()));

    res = full_request("HEAD /static/missing.css HTTP/1.1\r\n\r\n");
    check("HEAD missing file: 404 and no body", first_line(res) == "HTTP/1.1 404 Not Found" && res.substr(res.size() - 4) == "\r\n\r\n");

    res = full_request("GET /static/missing.css HTTP/1.1\r\n\r\n");
    check("GET missing file: 404 with text", first_line(res) == "HTTP/1.1 404 Not Found" && res.find("\r\n\r\nNot Found") != string::npos);

    res = full_request("POST /static/style.css HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    check("POST static: 405 and Allow: GET, HEAD", first_line(res) == "HTTP/1.1 405 Method Not Allowed" &&
                                                    res.find("Allow: GET, HEAD\r\n") != string::npos, first_line(res));

    filesystem::remove_all(dir, ec);
    net_cleanup();
    cout << (fails == 0 ? "ALL OK" : "SOME FAILED") << endl;
    return fails == 0 ? 0 : 1;
}

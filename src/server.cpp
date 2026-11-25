// server.cpp - socket code (Winsock on Windows, POSIX sockets on Linux; the differences are in platform.h):
// accept loop, recv/send, request parsing, static files (/static/...)
//
// How a request flows through this file:
//   run_server()      main thread: accept() a client and put it in the queue (or answer 503 if the queue is full)
//   handle_client()   a worker thread: read_request() -> serve_static() or route() -> close the socket
//   read_request()    reads bytes until the end of the headers, parses them, then reads the body (Content-Length)
//   send_response()   builds the status line + headers + body and sends all of it
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "common.h"   // includes platform.h: Winsock on Windows, POSIX sockets on Linux
#ifndef _WIN32
#include <csignal>
#endif
using namespace std;

// The listening socket. Global so the Ctrl+C / SIGTERM handler can close it, which makes accept() fail and
// lets run_server() leave its loop.
static SOCKET server = INVALID_SOCKET;

// Closes the listening socket. Safe to call twice: the second call sees INVALID_SOCKET and does nothing.
static void close_server_socket() {
    if (server != INVALID_SOCKET) {
        SOCKET s = server;
        server = INVALID_SOCKET;
#ifndef _WIN32
        shutdown(s, SHUT_RDWR);   // Linux: close() alone does not wake an accept() blocked in another thread; shutdown() does.
#endif
        close_socket(s);          // closesocket() on Windows, close() on Linux (platform.h)
    }
}

// Ctrl+C closes the socket, so accept() fails and the loop ends.
#ifdef _WIN32
static BOOL WINAPI on_ctrl_c(DWORD) {
    close_server_socket();
    return TRUE;
}
#else
// Linux: Ctrl+C sends SIGINT, and Render / Docker send SIGTERM when stopping. shutdown() and close() are safe in a handler.
static void on_signal(int) {
    close_server_socket();
}
#endif

// Text -> number for Content-Length. Digits only, at most 9 of them (so it cannot overflow); false if invalid.
// We do not use atoi/stoi: they accept things like "12abc" or "-5", which must be a 400 here.
static bool to_number(const string& s, size_t& num) {
    if (s.empty() || s.size() > 9) return false;
    num = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        num = num * 10 + (c - '0');
    }
    return true;
}

// Header names are case-insensitive in HTTP ("Content-Length" = "content-length"), so we store them in lowercase.
static string to_lower(string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Removes spaces, tabs and line breaks at both ends (header values often have a space after the colon).
string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// One hex digit (0-9, a-f, A-F) -> its value 0..15, or -1 if it is not a hex digit. Used by url_decode for %xx.
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "+" is a space, %xx is a byte; a bad %xx or %00 is rejected.
static bool url_decode(const string& in, string& out) {
    out = "";
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= in.size()) return false;   // % must be followed by two hex digits.
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            char b = (char)(hi * 16 + lo);
            if (b == 0) return false;               // Refuse NUL bytes (they can cut strings short).
            out += b;
            i += 2;
        } else {
            out += c;
        }
    }
    return true;
}

// Splits a=1&b=2 into a map; names and values are URL-decoded. Used for the query string and for POST form bodies.
// A broken %xx anywhere makes the whole request a 400 (better than guessing what the user meant).
static bool parse_query(const string& query, map<string, string>& params) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == string::npos) amp = query.size();
        string pair = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty()) continue;

        size_t eq = pair.find('=');
        string rawName = (eq == string::npos) ? pair : pair.substr(0, eq);
        string rawVal = (eq == string::npos) ? "" : pair.substr(eq + 1);
        string name, val;
        if (!url_decode(rawName, name) || !url_decode(rawVal, val)) return false;
        if (!name.empty()) params[name] = val;
    }
    return true;
}

// Parses the request line, e.g. GET /path HTTP/1.1.
static bool parse_request_line(const string& line, Request& req) {
    size_t a = line.find(' ');
    if (a == string::npos) return false;
    size_t b = line.find(' ', a + 1);
    if (b == string::npos) return false;

    req.method = line.substr(0, a);
    req.target = line.substr(a + 1, b - a - 1);     // Kept exactly as received, e.g. /products?cat=3.
    req.version = line.substr(b + 1);

    size_t q = req.target.find('?');
    req.path = req.target.substr(0, q);
    if (q != string::npos && !parse_query(req.target.substr(q + 1), req.params)) return false;

    if (req.method.empty()) return false;
    for (char c : req.method)
        if (c < 'A' || c > 'Z') return false;        // Method: capital letters only.
    if (req.path.empty() || req.path[0] != '/') return false;
    for (char c : req.target)
        if (c <= 32 || c == 127) return false;       // No control characters or spaces.
    if (req.version.compare(0, 5, "HTTP/") != 0) return false;
    if (req.version.find(' ') != string::npos) return false;
    return true;
}

// Splits the Cookie header "a=1; session_id=xyz" into a map. Cookies without a name are skipped.
// Only session_id is used by the shop (pages.cpp reads it to find the logged-in user).
static void parse_cookies(const string& header, map<string, string>& cookies) {
    size_t pos = 0;
    while (pos < header.size()) {
        size_t semi = header.find(';', pos);
        if (semi == string::npos) semi = header.size();
        string pair = trim(header.substr(pos, semi - pos));
        pos = semi + 1;

        size_t eq = pair.find('=');
        if (eq == string::npos || eq == 0) continue;
        cookies[pair.substr(0, eq)] = pair.substr(eq + 1);
    }
}

// head = request line + headers, without the final blank line. Every header line must be "Name: value";
// anything else (no colon, space inside the name) is a 400.
static bool parse_request(const string& head, Request& req) {
    size_t eol = head.find("\r\n");
    if (eol == string::npos) eol = head.size();
    if (!parse_request_line(head.substr(0, eol), req)) return false;

    size_t pos = eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == string::npos) e = head.size();
        string line = head.substr(pos, e - pos);
        pos = e + 2;

        size_t colon = line.find(':');
        if (colon == string::npos || colon == 0) return false;
        string name = line.substr(0, colon);
        if (name.find_first_of(" \t") != string::npos) return false;
        req.headers[to_lower(name)] = trim(line.substr(colon + 1));
    }

    auto it = req.headers.find("cookie");
    if (it != req.headers.end()) parse_cookies(it->second, req.cookies);
    return true;
}

// read_request results: 0 ok, -1 client left, otherwise an HTTP status.
const int READ_OK = 0;
const int READ_GONE = -1;

// Appends bytes from recv; returns READ_OK, READ_GONE or 408 on timeout.
static int recv_more(SOCKET client, string& data) {
    char buf[4096];
    int n = recv(client, buf, sizeof(buf), 0);   // The receive timeout (SO_RCVTIMEO) stops this waiting forever.
    if (n == SOCKET_ERROR) {
        int err = sock_error();
        if (sock_timed_out(err)) return 408;
        log_msg("recv failed: " + to_string(err));
        return READ_GONE;
    }
    if (n == 0) return READ_GONE;                // recv returning 0 means the client closed the connection.
    data.append(buf, n);
    return READ_OK;
}

// Reads one full request. Returns READ_OK, READ_GONE, or an HTTP error status:
// 400 malformed, 408 timeout, 413 body too big, 431 headers too big, 501 chunked body (not supported).
static int read_request(SOCKET client, Request& req) {
    // TCP is a byte stream: keep reading until the blank line that ends the headers.
    string raw;
    size_t end;
    while ((end = raw.find("\r\n\r\n")) == string::npos) {
        if (raw.size() > MAX_HEADER) return 431;     // Header size limit.
        int r = recv_more(client, raw);
        if (r != READ_OK) return r;
    }
    if (end + 4 > MAX_HEADER) return 431;

    string body = raw.substr(end + 4);               // Body bytes that arrived together with the headers.
    if (!parse_request(raw.substr(0, end), req)) return 400;

    // Body size comes from Content-Length.
    if (req.headers.count("transfer-encoding")) return 501;     // Chunked bodies are not supported.
    size_t len = 0;
    if (req.headers.count("content-length")) {
        if (!to_number(req.headers["content-length"], len)) return 400;   // not a number: malformed
        if (len > MAX_BODY) return 413;
    }

    while (body.size() < len) {
        int r = recv_more(client, body);
        if (r != READ_OK) return r;
    }
    req.body = body.substr(0, len);

    // POST forms are decoded like a query string.
    if (req.method == "POST" &&
        to_lower(req.headers["content-type"]).compare(0, 33, "application/x-www-form-urlencoded") == 0) {
        if (!parse_query(req.body, req.form)) return 400;
    }
    return READ_OK;
}

// The text after the status code in the first response line, e.g. "404 Not Found".
static string reason(int status) {
    if (status == 200) return "OK";
    if (status == 302) return "Found";
    if (status == 400) return "Bad Request";
    if (status == 401) return "Unauthorized";
    if (status == 403) return "Forbidden";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    if (status == 408) return "Request Timeout";
    if (status == 409) return "Conflict";
    if (status == 413) return "Payload Too Large";                 // body bigger than MAX_BODY
    if (status == 431) return "Request Header Fields Too Large";   // headers bigger than MAX_HEADER
    if (status == 500) return "Internal Server Error";
    if (status == 501) return "Not Implemented";                   // Transfer-Encoding (chunked) is not supported
    if (status == 503) return "Service Unavailable";
    return "Unknown";
}

// Status line + headers + the blank line that ends them, without the body.
// Content-Length must equal the body size in bytes (also for HEAD, where the body itself is not sent).
static string response_head(int status, const string& type, size_t length, const string& extra) {
    return "HTTP/1.1 " + to_string(status) + " " + reason(status) + "\r\n"
           "Content-Type: " + type + "\r\n"
           "Content-Length: " + to_string(length) + "\r\n"
           "Connection: close\r\n" + extra +
           "\r\n";
}

static void send_all(SOCKET client, const string& res) {
    // send() may send fewer bytes than asked, so loop.
    size_t sent = 0;
    while (sent < res.size()) {
        int n = send(client, res.c_str() + sent, (int)(res.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            log_msg("send failed: " + to_string(sock_error()));
            break;
        }
        sent += n;
    }
}

// Builds and sends: status line, headers, blank line, body.
// extra = more header lines, each ending in \r\n (e.g. "Retry-After: 2\r\n").
void send_response(SOCKET client, int status, const string& type, const string& body,
                   const string& extra) {
    send_all(client, response_head(status, type, body.size(), extra) + body);
}

// ---- static files: /static/<name>, loaded into memory once at startup (no disk reads) ----
// Why memory: the files are small (CSS, JS, icons, product photos), reading them once is faster than opening a file
// per request, and it is safer, because a request can only ask for a name that is already in the map.
// The URL is never used as a disk path, only looked up in this map, so ../ tricks cannot work.
static map<string, StaticFile> static_files;

// Content-Type from the extension; "" = unknown type (not loaded, so 404).
static string static_type(const string& name) {
    size_t dot = name.rfind('.');
    if (dot == string::npos) return "";
    string ext = name.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";     // Product photos (public/images/products/<id>.jpg).
    return "";
}

// Only a-z A-Z 0-9 . _ - and / (for folders).
static bool safe_name_chars(const string& name) {
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) return false;
    }
    return true;
}

// Loads every file under dir (subfolders too) into memory once at startup; returns how many.
int load_static_files(const string& dir) {
    static_files.clear();
    error_code ec;
    if (!filesystem::is_directory(dir, ec)) {
        log_msg("static folder '" + dir + "' not found: /static/... will return 404");
        return 0;
    }

    for (auto& entry : filesystem::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        string name = filesystem::relative(entry.path(), dir).generic_string();     // Always use "/" as the separator.
        string type = static_type(name);
        if (type.empty() || !safe_name_chars(name) || name.find("..") != string::npos) {
            log_msg("static: skipped '" + name + "' (unknown type or unsafe name)");
            continue;
        }
        if (entry.file_size() > 2 * 1024 * 1024) {              // Files stay in memory: 2 MB maximum each.
            log_msg("static: skipped '" + name + "' (bigger than 2 MB)");
            continue;
        }

        ifstream f(entry.path(), ios::binary);                  // Binary mode: bytes are read unchanged (images).
        StaticFile sf;
        sf.type = type;
        sf.data.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
        static_files[name] = sf;
    }
    log_msg("static files loaded: " + to_string(static_files.size()) + " from '" + dir + "'");
    return (int)static_files.size();
}

// Name after /static/ -> file, or nullptr (404).
// Refused: empty name, "..", backslash, and names that start with "/".
const StaticFile* find_static(const string& name) {
    if (name.empty()) return nullptr;
    if (name.find("..") != string::npos) return nullptr;
    if (name.find('\\') != string::npos) return nullptr;
    if (name[0] == '/') return nullptr;
    auto it = static_files.find(name);
    return it == static_files.end() ? nullptr : &it->second;
}

// GET or HEAD /static/<name>, served from memory (no disk read). Returns the status.
// HEAD gets exactly the same headers as GET (Content-Length included) but no body; browsers, curl -I and
// link checkers use it to see if a file exists and how big it is without downloading it.
static int serve_static(SOCKET client, const Request& req) {
    const bool head = req.method == "HEAD";
    if (req.method != "GET" && !head) {
        send_response(client, 405, "text/plain; charset=utf-8", "Method Not Allowed", "Allow: GET, HEAD\r\n");
        return 405;
    }
    const StaticFile* f = find_static(req.path.substr(8));      // Skip the 8 characters of "/static/".
    if (f == nullptr) {
        const string text = "Not Found";
        if (head) send_all(client, response_head(404, "text/plain; charset=utf-8", text.size(), ""));   // HEAD: never a body
        else send_response(client, 404, "text/plain; charset=utf-8", text);
        return 404;
    }
    // The browser may cache it for one hour.
    const string cache = "Cache-Control: max-age=3600\r\n";
    if (head) send_all(client, response_head(200, f->type, f->data.size(), cache));
    else send_response(client, 200, f->type, f->data, cache);
    return 200;
}

// Closes a client socket without losing the answer we just sent.
// Closing a socket that still has unread data makes the OS (Windows and Linux) send RST instead of FIN, and the client
// can lose the response. So: send FIN first, read and throw away what the client still sends (100 ms timeout, at most
// 16 reads so a client that keeps sending cannot hold the thread), then close.
static void close_gracefully(SOCKET client) {
    shutdown(client, SHUT_SEND);
    set_recv_timeout(client, 100);
    char buf[4096];
    for (int i = 0; i < 16 && recv(client, buf, sizeof(buf), 0) > 0; i++) {}
    close_socket(client);
}

// Handles one client: read, route, close. Runs on a worker thread, never on the main thread.
// Returns what was answered (for the worker's log and metrics).
// Per-request console lines only print when VERBOSE_LOG = 1.
RequestInfo handle_client(const ClientJob& job) {
    SOCKET client = job.sock;
    RequestInfo info;
    Request req;
    req.client_ip = job.ip;
    int r = read_request(client, req);

    if (r == READ_OK) {
        // Static files need no login or database: served before route().
        bool is_static = req.path.compare(0, 8, "/static/") == 0;
        info.status = is_static ? serve_static(client, req) : route(client, req);
        info.method = req.method;
        info.path = req.path;
        info.user_id = req.user_id;
        log_msg_verbose(req.method + " " + req.target + " -> " + to_string(info.status));
    } else if (r == 408) {
        send_response(client, 408, "text/html; charset=utf-8", render_error_page("408 Request Timeout"));
        info.status = 408;
        info.method = "-";              // The request was never fully read.
        info.path = "-";
        log_msg_verbose("timeout -> 408");
    } else if (r > 0) {
        // Any other error status from read_request (400 bad request, 413 / 431 too big, 501 chunked body):
        // send the error page with that status and its text, e.g. "413 Payload Too Large".
        send_response(client, r, "text/html; charset=utf-8", render_error_page(to_string(r) + " " + reason(r)));
        info.status = r;
        info.method = req.method.empty() ? "-" : req.method;   // If the request line was valid, method and path are known.
        info.path = req.path.empty() ? "-" : req.path;
        log_msg_verbose("bad request -> " + to_string(r));
    } else {
        log_msg_verbose("client left early");   // Client left: nothing to send (status 0 = not logged).
    }
    // After an error answer part of the request may still be unread (e.g. a 431 for huge headers): close gracefully.
    if (r > 0) close_gracefully(client);
    else close_socket(client);
    return info;
}

// Sets up shutdown handling, loads the static files and opens the listening socket:
// socket() -> setsockopt(SO_REUSEADDR) -> bind() -> listen(). Returns false (and logs why) if any step fails.
bool start_server() {
#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl_c, TRUE);
#else
    // SIGPIPE: on Linux, send() to a client that already left would kill the process. Ignore it; send() just returns an error.
    signal(SIGPIPE, SIG_IGN);
    // sa_flags = 0 (no SA_RESTART): a blocked accept() returns with EINTR when the signal arrives.
    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
    load_static_files(STATIC_DIR);      // Loaded before the workers start, so the map is only read later (no mutex needed).

    // Windows: WSAStartup must run before any socket call. Linux: nothing to do.
    if (!net_init()) {
        log_msg("network init (WSAStartup) failed");
        return false;
    }

    // socket(): IPv4 (AF_INET), TCP (SOCK_STREAM).
    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        log_msg("socket failed: " + to_string(sock_error()));
        net_cleanup();
        return false;
    }

    // Lets the port be reused right after a restart.
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // INADDR_ANY = all network cards; htons = network byte order.
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int port = listen_port(PORT);       // PORT environment variable (Render sets it), else PORT from common.h
    addr.sin_port = htons(port);
    if (::bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg("bind failed on port " + to_string(port) + ": " + to_string(sock_error()));
        close_server_socket();
        net_cleanup();
        return false;
    }

    // Start waiting for connections; the OS keeps the backlog.
    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        log_msg("listen failed: " + to_string(sock_error()));
        close_server_socket();
        net_cleanup();
        return false;
    }

    log_msg("Listening on http://localhost:" + to_string(port));
    return true;
}

// Queue full: do not wait, answer 503 at once.
static void reject_client(SOCKET client) {
    send_response(client, 503, "text/html; charset=utf-8", render_error_page("Server busy, try again"),
                  "Retry-After: 2\r\n");

    // The request was never read, so close gracefully (see close_gracefully) or the client may lose the 503.
    close_gracefully(client);
}

// The accept loop, on the main thread. It only accepts and queues; the worker threads do the reading and answering,
// so one slow client cannot stop new clients from being accepted. Ends when the listening socket is closed.
void run_server() {
    while (true) {
        // accept() sleeps until a client connects and returns a new socket for it.
        sockaddr_in from = {};
        sock_len_t from_len = sizeof(from);
        SOCKET client = accept(server, (sockaddr*)&from, &from_len);
        long long accepted = now_us();             // Note the time now: queue wait and latency are measured from here.
        if (client == INVALID_SOCKET) {
            if (server == INVALID_SOCKET) break;   // Ctrl+C closed the socket: stop.
            log_msg("accept failed: " + to_string(sock_error()));
            continue;
        }
        // A slow client must not block a worker forever: receive timeout (platform.h: DWORD ms on Windows, timeval on Linux).
        set_recv_timeout(client, RECV_TIMEOUT_MS);

        // IPv4 as a.b.c.d; /metrics is only allowed from 127.0.0.1.
        const unsigned char* b = (const unsigned char*)&from.sin_addr;
        ClientJob job;
        job.sock = client;
        job.accept_us = accepted;
        job.ip = to_string(b[0]) + "." + to_string(b[1]) + "." + to_string(b[2]) + "." + to_string(b[3]);

        // Workers do the handling; we only queue.
        if (!push_client(job)) {
            reject_client(client);
            log_msg("queue full -> 503 (rejected so far: " + to_string(metric_rejected()) + ")");
        }
    }
}

// Called once at shutdown, after the worker threads have stopped.
void stop_server() {
    close_server_socket();
    // Windows: WSACleanup, the pair of WSAStartup. Linux: nothing.
    net_cleanup();
}

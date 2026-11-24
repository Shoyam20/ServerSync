#include <winsock2.h>   // Must come before windows.h.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "common.h"
using namespace std;

// Global so the Ctrl+C handler can close it.
static SOCKET server = INVALID_SOCKET;

// Safe to call twice.
static void close_server_socket() {
    if (server != INVALID_SOCKET) {
        SOCKET s = server;
        server = INVALID_SOCKET;
        closesocket(s);   // Windows uses closesocket(), not close().
    }
}

// Ctrl+C closes the socket, so accept() fails and the loop ends.
static BOOL WINAPI on_ctrl_c(DWORD) {
    close_server_socket();
    return TRUE;
}

// Digits only, at most 9 (so it cannot overflow); false if invalid.
static bool to_number(const string& s, size_t& num) {
    if (s.empty() || s.size() > 9) return false;
    num = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        num = num * 10 + (c - '0');
    }
    return true;
}

static string to_lower(string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

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
        } else 
        {
            out += c;
        }
    }
    return true;
}

// Splits a=1&b=2 into a map; names and values are URL-decoded.
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
static bool parse_request_line(const string& line, Request& req) 
{
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

// Splits "a=1; session_id=xyz" into a map.
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

// head = request line + headers, without the final blank line.
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
        if (WSAGetLastError() == WSAETIMEDOUT) return 408;
        log_msg("recv failed: " + to_string(WSAGetLastError()));
        return READ_GONE;
    }
    if (n == 0) return READ_GONE;                // recv returning 0 means the client closed the connection.
    data.append(buf, n);
    return READ_OK;
}

// Reads one full request: READ_OK, READ_GONE, 400 (bad or too big) or 408 (timeout).
static int read_request(SOCKET client, Request& req) {
    // TCP is a byte stream: keep reading until the blank line that ends the headers.
    string raw;
    size_t end;
    while ((end = raw.find("\r\n\r\n")) == string::npos) {
        if (raw.size() > MAX_HEADER) return 400;     // Header size limit.
        int r = recv_more(client, raw);
        if (r != READ_OK) return r;
    }
    if (end + 4 > MAX_HEADER) return 400;

    string body = raw.substr(end + 4);               // Body bytes that arrived together with the headers.
    if (!parse_request(raw.substr(0, end), req)) return 400;

    // Body size comes from Content-Length.
    if (req.headers.count("transfer-encoding")) return 400;     // Chunked bodies are not supported.
    size_t len = 0;
    if (req.headers.count("content-length")) {
        if (!to_number(req.headers["content-length"], len)) return 400;
        if (len > MAX_BODY) return 400;
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

static string reason(int status) {
    if (status == 200) return "OK";
    if (status == 302) return "Found";
    if (status == 400) return "Bad Request";
    if (status == 401) return "Unauthorized";
    if (status == 403) return "Forbidden";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    if (status == 409) return "Conflict";
    if (status == 408) return "Request Timeout";
    if (status == 500) return "Internal Server Error";
    if (status == 503) return "Service Unavailable";
    return "Unknown";
}

static void send_all(SOCKET client, const string& res) {
    // send() may send fewer bytes than asked, so loop.
    size_t sent = 0;
    while (sent < res.size()) {
        int n = send(client, res.c_str() + sent, (int)(res.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            log_msg("send failed: " + to_string(WSAGetLastError()));
            break;
        }
        sent += n;
    }
}

// Builds and sends: status line, headers, blank line, body.
// extra = more header lines, each ending in \r\n (e.g. "Retry-After: 2\r\n").
void send_response(SOCKET client, int status, const string& type, const string& body,
                   const string& extra) {
    // Content-Length must equal the body size in bytes.
    string res = "HTTP/1.1 " + to_string(status) + " " + reason(status) + "\r\n"
                 "Content-Type: " + type + "\r\n"
                 "Content-Length: " + to_string(body.size()) + "\r\n"
                 "Connection: close\r\n" + extra +
                 "\r\n" + body;
    send_all(client, res);
}

// ---- static files: /static/<name>, loaded into memory once at startup (no disk reads) ----
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

// GET /static/<name>, served from memory (no disk read). Returns the status.
static int serve_static(SOCKET client, const Request& req) {
    if (req.method != "GET") {
        send_response(client, 405, "text/plain; charset=utf-8", "Method Not Allowed", "Allow: GET\r\n");
        return 405;
    }
    const StaticFile* f = find_static(req.path.substr(8));      // Skip the 8 characters of "/static/".
    if (f == nullptr) {
        send_response(client, 404, "text/plain; charset=utf-8", "Not Found");
        return 404;
    }
    // The browser may cache it for one hour.
    send_response(client, 200, f->type, f->data, "Cache-Control: max-age=3600\r\n");
    return 200;
}

// Handles one client: read, route, close. Returns what was answered (for the worker's log and metrics).
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
    } else if (r == 400) {
        send_response(client, 400, "text/html; charset=utf-8", render_error_page("400 Bad Request"));
        info.status = 400;
        info.method = req.method.empty() ? "-" : req.method;   // If the request line was valid, method and path are known.
        info.path = req.path.empty() ? "-" : req.path;
        log_msg_verbose("bad request -> 400");
    } else {
        log_msg_verbose("client left early");   // Client left: nothing to send (status 0 = not logged).
    }
    closesocket(client);
    return info;
}

bool start_server() {
    SetConsoleCtrlHandler(on_ctrl_c, TRUE);
    load_static_files(STATIC_DIR);      // Loaded before the workers start, so the map is only read later (no mutex needed).

    // WSAStartup must run before any socket call (version 2.2).
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        log_msg("WSAStartup failed: " + to_string(r));
        return false;
    }

    // socket(): IPv4 (AF_INET), TCP (SOCK_STREAM).
    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        log_msg("socket failed: " + to_string(WSAGetLastError()));
        WSACleanup();
        return false;
    }

    // Lets the port be reused right after a restart.
    BOOL yes = TRUE;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // INADDR_ANY = all network cards; htons = network byte order.
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg("bind failed: " + to_string(WSAGetLastError()));
        close_server_socket();
        WSACleanup();
        return false;
    }

    // Start waiting for connections; the OS keeps the backlog.
    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        log_msg("listen failed: " + to_string(WSAGetLastError()));
        close_server_socket();
        WSACleanup();
        return false;
    }

    log_msg("Listening on http://localhost:" + to_string(PORT));
    return true;
}

// Queue full: do not wait, answer 503 at once.
static void reject_client(SOCKET client) {
    send_response(client, 503, "text/html; charset=utf-8", render_error_page("Server busy, try again"),
                  "Retry-After: 2\r\n");

    // Closing with unread data makes Windows send RST, and the client may lose the 503.
    // So send FIN first, read the request briefly (max 100 ms), then close.
    shutdown(client, SD_SEND);
    DWORD ms = 100;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
    char buf[4096];
    recv(client, buf, sizeof(buf), 0);
    closesocket(client);
}

void run_server() {
    while (true) {
        // accept() sleeps until a client connects and returns a new socket for it.
        sockaddr_in from = {};
        int from_len = sizeof(from);
        SOCKET client = accept(server, (sockaddr*)&from, &from_len);
        long long accepted = now_us();             // Note the time now: queue wait and latency are measured from here.
        if (client == INVALID_SOCKET) {
            if (server == INVALID_SOCKET) break;   // Ctrl+C closed the socket: stop.
            log_msg("accept failed: " + to_string(WSAGetLastError()));
            continue;
        }
        // A slow client must not block a worker forever: receive timeout (ms).
        DWORD ms = RECV_TIMEOUT_MS;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));

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

void stop_server() {
    close_server_socket();
    // Pairs with WSAStartup.
    WSACleanup();
}

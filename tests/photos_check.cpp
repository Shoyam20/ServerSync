// photos_check.cpp - serves the product photos with the REAL static code of src/server.cpp (load_static_files, find_static, serve_static)
// over a loopback socket, without a database, a worker pool or db.conf. server.cpp is #included so its static functions are reachable.
// usage: photos_check <folder> <name> [<name> ...]      names are relative to the folder, like images/products/7.jpg
#include "../src/server.cpp"
#include <iostream>

// ---- stubs for what server.cpp expects from threads.cpp / pages.cpp ----
void log_tag(const string& tag, const string& msg) { cerr << "[" << tag << "] " << msg << endl; }
void log_msg(const string& msg) { log_tag("TEST", msg); }
void log_msg_verbose(const string&) {}
long long now_us() { return 0; }
bool push_client(const ClientJob&) { return true; }
long long metric_rejected() { return 0; }
int route(SOCKET, Request&) { return 200; }
string render_error_page(const string& message) { return message; }

static bool make_pair(SOCKET& a, SOCKET& c) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) return false;
    int len = sizeof(addr);
    getsockname(ls, (sockaddr*)&addr, &len);
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(c, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    a = accept(ls, nullptr, nullptr);
    closesocket(ls);
    return a != INVALID_SOCKET;
}

// one GET /static/<name> through serve_static; returns the raw bytes the "browser" received
static string fetch(const string& path) {
    SOCKET a, c;
    if (!make_pair(a, c)) return "";
    Request req;
    req.method = "GET";
    req.path = path;
    serve_static(a, req);
    closesocket(a);
    string raw;
    char buf[8192];
    while (true) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);
    }
    closesocket(c);
    return raw;
}

// value of one response header ("-" if it is not there)
static string header(const string& head, const string& name) {
    string crlf = "\r\n";
    size_t p = head.find(crlf + name + ": ");
    if (p == string::npos) return "-";
    size_t start = p + 2 + name.size() + 2;
    return head.substr(start, head.find(crlf, start) - start);
}

static unsigned long long checksum(const string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    string dir = argc > 1 ? argv[1] : "public";
    cout << "COUNT " << load_static_files(dir) << endl;
    for (int i = 2; i < argc; i++) {
        string name = argv[i];
        string raw = fetch("/static/" + name);
        size_t split = raw.find("\r\n\r\n");
        string head = split == string::npos ? raw : raw.substr(0, split);
        string body = split == string::npos ? "" : raw.substr(split + 4);
        string type = header(head, "Content-Type"), len = header(head, "Content-Length"), cache = header(head, "Cache-Control");
        ifstream f(dir + "/" + name, ios::binary);
        string disk((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        cout << "HTTP " << name << " | " << head.substr(0, head.find("\r\n")) << " | " << type << " | len " << len << " | " << cache << " | body " << body.size()
             << " sum " << checksum(body) << " | disk " << disk.size() << " sum " << checksum(disk) << endl;
    }
    WSACleanup();
    return 0;
}

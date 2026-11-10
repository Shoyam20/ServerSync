// platform.h - Windows (Winsock) aur Linux (POSIX sockets) ka farq sirf yahin. Baaki code dono par same chalta hai.
// Windows: build.bat (g++ / MinGW).  Linux: Makefile / Dockerfile (Render par deploy ke liye).
#pragma once

#include <string>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
// ---------------- Windows ----------------
#include <winsock2.h>   // windows.h se pehle aana chahiye
#include <windows.h>
#include <bcrypt.h>     // BCryptGenRandom: OS ka secure random number generator

typedef int sock_len_t;                     // accept() ka address length type
const int SHUT_SEND = SD_SEND;              // shutdown(): sirf bhejna band (FIN)

inline bool net_init() {                    // WSAStartup: koi bhi socket call se pehle zaroori, version 2.2
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
inline void net_cleanup() { WSACleanup(); }
inline void close_socket(SOCKET s) { closesocket(s); }   // Windows me close() nahi, closesocket()
inline int sock_error() { return WSAGetLastError(); }
inline bool sock_timed_out(int err) { return err == WSAETIMEDOUT; }

// recv par timeout: Windows me DWORD, milliseconds
inline void set_recv_timeout(SOCKET s, int ms) {
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
}

inline bool secure_random(unsigned char* buf, unsigned long n) {
    return BCryptGenRandom(nullptr, buf, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

inline void local_time(const time_t* now, tm* out) { localtime_s(out, now); }   // Windows: (tm*, time_t*)
inline void sleep_ms(int ms) { Sleep((DWORD)ms); }

#else
// ---------------- Linux / POSIX ----------------
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/random.h>   // getrandom()
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

typedef int SOCKET;                         // Linux me socket bas ek file descriptor (int) hai
const SOCKET INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1;
typedef socklen_t sock_len_t;
const int SHUT_SEND = SHUT_WR;

inline bool net_init() { return true; }     // Linux me WSAStartup jaisa kuch nahi chahiye
inline void net_cleanup() {}
inline void close_socket(SOCKET s) { close(s); }
inline int sock_error() { return errno; }
// SO_RCVTIMEO khatam hone par recv() -1 deta hai aur errno = EAGAIN / EWOULDBLOCK
inline bool sock_timed_out(int err) { return err == EAGAIN || err == EWOULDBLOCK; }

// recv par timeout: Linux me struct timeval (seconds + microseconds)
inline void set_recv_timeout(SOCKET s, int ms) {
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// getrandom: kernel ka secure RNG (BCryptGenRandom jaisa). Chhote reads (<= 256 bytes) beech me nahi tootte
inline bool secure_random(unsigned char* buf, unsigned long n) {
    unsigned long got = 0;
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += (unsigned long)r;
    }
    return true;
}

inline void local_time(const time_t* now, tm* out) { localtime_r(now, out); }   // POSIX: (time_t*, tm*) - ulta order
inline void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

// kis port par sunna hai: env var PORT (Render, Railway jaise hosts yahi set karte hain), warna common.h ka default
inline int listen_port(int fallback) {
    const char* p = std::getenv("PORT");
    if (p == nullptr || *p == '\0') return fallback;
    int v = std::atoi(p);
    return (v > 0 && v < 65536) ? v : fallback;
}

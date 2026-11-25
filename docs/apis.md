# APIs used in ServerSync

ServerSync calls **no external web APIs** (no payment gateway, no REST services) and does not use a web framework or an
HTTP / JSON library. Everything below is a library or operating-system API called directly from C++.

## Sockets (`src/server.cpp`, `include/platform.h`)

| Windows (Winsock2, `ws2_32`) | Linux (POSIX) | Used for |
|---|---|---|
| `WSAStartup`, `WSACleanup` | not needed | load / unload the socket library |
| `socket`, `setsockopt(SO_REUSEADDR, SO_RCVTIMEO)`, `bind`, `htons`, `listen` | same | create the listening socket |
| `accept`, `recv`, `send`, `shutdown` | same | serve each client |
| `closesocket`, `WSAGetLastError` | `close`, `errno` | close sockets, read error codes |

## MySQL C API (`libmysqlclient`, `src/db.cpp`)

| Group | Functions |
|---|---|
| library / threads | `mysql_library_init`, `mysql_library_end`, `mysql_thread_init`, `mysql_thread_end` |
| connections | `mysql_init`, `mysql_options` (connect timeout, utf8mb4, SSL mode, SSL CA), `mysql_real_connect`, `mysql_ping`, `mysql_close` |
| prepared statements | `mysql_stmt_init`, `mysql_stmt_prepare`, `mysql_stmt_bind_param`, `mysql_stmt_execute`, `mysql_stmt_bind_result`, `mysql_stmt_store_result`, `mysql_stmt_fetch`, `mysql_stmt_affected_rows`, `mysql_stmt_close` |
| plain queries | `mysql_query`, `mysql_fetch_fields`, `mysql_num_fields`, `mysql_free_result` |
| transactions | `mysql_autocommit`, `mysql_commit`, `mysql_rollback` |
| errors | `mysql_error`, `mysql_errno`, `mysql_stmt_error`, `mysql_stmt_errno` |

SQL features we rely on: `START TRANSACTION`, `SELECT ... FOR UPDATE` / `FOR SHARE`, `COMMIT` / `ROLLBACK`,
`LAST_INSERT_ID()`, `CHECK` constraints, generated columns with `UNIQUE` (one active cart / one default address per user).

## Threads (C++20 standard library, `src/threads.cpp`, `src/db.cpp`)

`std::thread`, `std::mutex`, `std::lock_guard`, `std::unique_lock`, `std::condition_variable`,
`std::counting_semaphore`, `std::atomic`, `std::this_thread::sleep_for`.

## Security

| API | Platform | Used for |
|---|---|---|
| `BCryptGenRandom` | Windows | session ids, password salts, payment reference ids |
| `getrandom()` | Linux | the same |
| `BCryptDeriveKeyPBKDF2` | Windows | password hashing (PBKDF2-HMAC-SHA256) |
| `PKCS5_PBKDF2_HMAC` (OpenSSL libcrypto) | Linux | password hashing |

`std::mt19937` is used only where the number does not need to be secret (random delivery delays).

## Time, signals, files

| API | Used for |
|---|---|
| `std::chrono::steady_clock` | request latency and queue wait for `/metrics` |
| `time`, `strftime`, `localtime_s` / `localtime_r` | log timestamps |
| `SetConsoleCtrlHandler` (Windows), `sigaction` / `signal` with `SIGINT`, `SIGTERM` (Linux) | clean shut-down on Ctrl+C or when Docker stops the container |
| `signal(SIGPIPE, SIG_IGN)` (Linux) | writing to a client that already disconnected must not kill the process |
| `std::getenv` | `PORT`, `DB_*`, `DEBUG_ROUTES` |
| `std::filesystem`, `std::ifstream` / `std::ofstream` | loading templates and static files, reading `db.conf` |

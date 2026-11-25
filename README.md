# ServerSync

An online shop with a web server written from scratch in C++20. There is no web framework: the server opens a TCP socket,
parses HTTP itself, hands each connection to a pool of worker threads and talks to MySQL through the MySQL C API.

It builds on Windows (MinGW, Winsock) and on Linux (g++, POSIX sockets) from the same source. The live copy runs on
Render with an Aiven MySQL database.

## Features

**Shop**
- 15 categories and 52 products with photos, search, category and price filters
- cart with a per-product limit, stock checks and "only N left" badges
- shipping addresses (up to 10 per user, one default)
- checkout as one database transaction with row locks, so two buyers can never take the last item
- payments: cash on delivery, UPI, card (Luhn check) and net banking (simulated, no real gateway)
- orders move PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED on their own (delivery thread); customers can cancel

**Accounts**
- signup / login with sessions stored in MySQL (random 64-character session id, HttpOnly + SameSite=Lax cookie)
- password rules checked on the server, with a live checklist in the browser
- passwords stored as PBKDF2-SHA256 hashes (see `include/password_hash.h`)
- see and end your own sessions

**Admin panel** (`/admin`)
- dashboard, products (add / edit / hide / restock), categories, orders, users and roles, sessions, request logs
- every admin action is written to an audit table

**Server internals**
- fixed pool of worker threads fed by a bounded queue; when the queue is full new clients get 503
- pool of MySQL connections guarded by a semaphore, with reconnect when a connection dies
- background threads: request logger (batched inserts), expired-session cleanup, delivery
- `/metrics` and `/metrics.json` (localhost only) with latency, queue length, status counts
- `/healthz` for uptime checks

## Tech stack

| Part | What we used |
|---|---|
| Language | C++20 (`std::thread`, `std::counting_semaphore`, `std::filesystem`) |
| Networking | Winsock2 on Windows, POSIX sockets on Linux (differences only in `include/platform.h`) |
| Database | MySQL 8.0 through the MySQL C API, prepared statements everywhere user input is used |
| Pages | HTML templates in `templates/`, rendered on the server; plain CSS and a little JavaScript in `public/` |
| Deploy | Docker image on Render, MySQL on Aiven |

## Run it on Windows

You need MinGW-w64 g++ with C++20 support and MySQL Server 8.0 (8.0.16 or newer, for CHECK constraints).

1. **Create the database** (this drops and recreates `ecommerce_db`, see `sql/README.md`). In Command Prompt:
   ```
   "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p --default-character-set=utf8mb4 < sql\ecommerce_db.sql
   ```
2. **Configure**: copy `db.conf.example` to `db.conf` and fill in your MySQL user and password. `db.conf` is git-ignored.
3. **Build**: run `build.bat`. If MySQL is not in the default folder, first run `set MYSQL=<your mysql folder>`.
4. **Run**: copy `libmysql.dll` from MySQL's `lib` folder next to `server.exe`, then start `server.exe` from the
   project folder (it loads `templates/` and `public/` from there). Open http://localhost:8080.
5. **Make yourself admin**: sign up on the site, then run
   `UPDATE users SET role = 'ADMIN' WHERE email = '<your email>';` in MySQL.

Stop the server with Ctrl+C: it stops the worker threads and flushes the request log to MySQL before exiting.

## Run it on Linux / Docker

```
make                      # needs g++, libmysqlclient-dev and libssl-dev
./server                  # reads db.conf, or the DB_* environment variables (see DEPLOY.md)
```

or with Docker, exactly like Render builds it:

```
docker build -t serversync .
docker run -p 8080:8080 -e DB_HOST=... -e DB_PORT=... -e DB_USER=... -e DB_PASSWORD=... serversync
```

The Docker image turns the debug routes (`/test`, `/slow`, `/dbtest`, `/dbslow`) off with `DISABLE_TEST_ROUTES=1`.
Deploying to Render + Aiven step by step is in [DEPLOY.md](DEPLOY.md).

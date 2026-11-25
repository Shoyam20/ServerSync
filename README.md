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

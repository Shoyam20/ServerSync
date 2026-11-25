# How ServerSync works

This page follows one request through the server and explains the threads, queues and locks involved.
All numbers below are constants in `include/common.h`.

## Start-up (`src/main.cpp`)

1. Load every HTML template from `templates/` into memory. No templates -> exit.
2. Read the database settings (`db.conf`, `/etc/secrets/db.conf`, or the `DB_*` environment variables) and open the
   pool of `DB_POOL = 4` MySQL connections. A failed connect is retried up to `DB_CONNECT_TRIES = 5` times.
3. Create the listening socket: `socket` -> `setsockopt(SO_REUSEADDR)` -> `bind` -> `listen`. The port is `PORT` from the
   environment (Render sets it) or 8080.
4. Start the background threads: request logger, `WORKERS = 4` worker threads, session cleanup, delivery.
5. Run the accept loop until Ctrl+C / SIGTERM, then stop everything in reverse order.

## One request

```
browser ──TCP──> accept loop (main thread)
                    │  set a 5 s receive timeout, remember the client IP
                    ▼
               client queue (max QUEUE_SIZE = 16)  ── full? -> 503 and close
                    │
                    ▼
               worker thread (one of 4)
                    │  read_request: headers <= 8 KB, body <= 1 MB, else 4xx
                    │  /static/... -> file from memory (public/)
                    │  everything else -> route() in pages.cpp (pages in auth/shop/cart/checkout/admin.cpp)
                    │       load_user: session cookie -> user, and slide the session expiry
                    │       page code: get_conn() from the pool, queries, put_conn()
                    │       render template -> send_response
                    ▼
               close socket, push one row for request_logs into the logger queue
```

The main thread only accepts; it never reads a request. This is the producer / consumer pattern: the accept loop
produces jobs, the workers consume them, and a mutex + condition variable protect the queue. Because the queue has a fixed
size, a flood of clients gets quick 503 answers instead of using up memory.

## Database connection pool (`src/db.cpp`)

- 4 connections are opened at start-up. A counting semaphore starts at 4: `get_conn()` waits on it, `put_conn()` releases it,
  so at most 4 threads use MySQL at the same time and nobody shares a connection.
- `get_conn()` pings the connection first. If MySQL dropped it (idle timeout or restart) a new one is opened right there.
- Every query that contains user input is a prepared statement with `?` placeholders (`mysql_stmt_*`), which is what
  protects against SQL injection.

## Transactions

- **Checkout** (`do_checkout`): one transaction. Cart items and products are locked with `SELECT ... FOR UPDATE` in
  `product_id` order (so two checkouts cannot deadlock on each other), stock is checked and reduced, the order, its items
  and the payment are inserted, and the cart is closed. Any problem -> `ROLLBACK`, nothing is half done. Deadlocks (1213)
  and lock wait timeouts (1205) are retried up to `CHECKOUT_RETRIES = 3` times.
- **Order status changes, role changes, password changes** follow the same pattern: one transaction, and an
  `UPDATE ... WHERE <value is still what we read>` so a change made by someone else in between is never overwritten.

## Background threads (`src/threads.cpp`)

| Thread | What it does |
|---|---|
| logger | collects request log rows in a memory queue (max 1000) and writes them with one multi-row INSERT every second (max 200 rows per INSERT); if the queue is full new rows are dropped and counted |
| cleanup | every 60 s marks sessions past their `expires_at` as ended (`EXPIRED`) |
| delivery | every 5 s moves orders whose next step is due one step along PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED |

## Sessions and passwords

- Login creates a session id from 32 bytes of the OS's secure random generator (`BCryptGenRandom` / `getrandom`),
  stores it in `user_sessions` and sends it as an `HttpOnly; SameSite=Lax` cookie. Sessions expire after 60 minutes
  without activity.
- Passwords are stored as PBKDF2-SHA256 hashes with a random salt (`include/password_hash.h`). Accounts created before
  hashing still work and are upgraded to a hash at their next login.

## Windows and Linux

Only `include/platform.h` knows which system it is on: socket types and close functions, the receive timeout format,
the secure random generator, PBKDF2, local time and sleep. Everything else is the same code on both.

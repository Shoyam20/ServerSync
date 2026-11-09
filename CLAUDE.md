# ServerSync
Reference (read only): D:\serversync_updated is the teammates' version.
Read it to copy logic, never edit it. Never copy deployment code from it
(Dockerfile, render.yaml, vercel, deploy/, DB_* env vars, Linux #ifdef
parts, /dev/urandom, signal()). Our build is Windows only.

## Goal
A multithreaded HTTP web server for an e-commerce site, written from scratch in C++20
with MySQL as the database. University project covering Operating Systems (threads,
mutexes, semaphores, sockets) and DBMS (schema, transactions, concurrency).

## Platform
Windows, Winsock2, g++ (MinGW/MSYS2), MySQL C API.
Threads: `std::thread`, `std::mutex`, `std::counting_semaphore` (C++20).

## Folders
- `src/`     one .cpp file per module (see Code layout)
- `include/` `common.h`, the only shared header
- `sql/`     database schema and sample data
- `tests/`   test programs / scripts
- `public/`  static files (style.css, app.js, favicon.svg, ...): loaded into memory at startup, served at `/static/<file>`.
             `app.js` only opens the login/signup popups; every page and form must also work with JavaScript off.
- `templates/` page HTML (`*.html`): loaded into memory at startup, filled by `render(name, values)`
- `old/`     previous version, reference only. Do not edit or copy blindly.

The server must be started from the project folder: `db.conf`, `public/` and `templates/` are found relative to it.

## Rules
1. Work in small steps. One feature per step; build and run before moving on.
2. Do not write code beyond the current step.
3. No web frameworks or HTTP libraries. Only Winsock2, the standard library
   and the MySQL C API.
4. Always explain new code in simple words after writing it.
5. Comment the OS/DBMS concepts (why a mutex, what a semaphore limits, why a
   transaction), not obvious syntax.
6. Only change the files the current step mentions.
7. `db.conf` holds the user's real MySQL login. NEVER create, overwrite,
   rename, delete or read it. If a test needs DB settings, ask the user first.
8. All test scripts live in `tests/`. Never put test scripts, logs or other
   temporary files in the project root.
9. Never run cleanup (delete/move) commands on files you did not create
   yourself in the current task. Check the path before every delete.

## Code layout
One file per module:
- `src/main.cpp`    startup
- `src/server.cpp`  Winsock, send/recv
- `src/threads.cpp` queue, worker threads, mutex, semaphore
- `src/db.cpp`      MySQL, connection pool, queries
- `src/pages.cpp`   routes, sessions, template `render()`, the data each page needs
- `include/common.h` the only shared header: structs, settings, function declarations

HTML lives in `templates/`, not in .cpp files: the C++ code fetches data and calls
`render("name", {{"key", value}, ...})`. `{{key}}` is HTML-escaped; `{{{key}}}` is raw and only for HTML we built
ourselves (rendered partials). Never put user or database text in a `{{{ }}}` slot unescaped.
CSS lives in `public/style.css` (plain CSS, no frameworks, no CDN).

Create a new file only if one module really needs it.
Settings (PORT, WORKERS, QUEUE_SIZE, DB_POOL) are constants in `common.h`.

## Coding style (follow in ALL steps)
1. Very simple beginner-style code: plain functions and structs.
   No classes, no templates, no fancy C++ features.
2. Use `using namespace std;` so there are no `std::` prefixes.
   No `ClassName::` style functions anywhere.
3. Comments are in Hinglish (Hindi written in English letters), short,
   one line each. Everything else (code, variables, output) is in English.
4. Short, simple variable names: client, port, buf, req, res.
5. Each file starts with a 2-line comment: what the file does + owner name.
6. Log messages start with a tag: [SERVER], [THREAD n], [DB], [PAGE].
7. Small functions, one job each.

## Build
`build.bat` produces `server.exe`.

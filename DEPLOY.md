# Deploying ServerSync (Render + Aiven MySQL)

The server now builds on both Windows (`build.bat`) and Linux (`Makefile` / `Dockerfile`).
All platform differences live in `include/platform.h`.

Never commit `db.conf` or `ca.pem` (both are git-ignored), and never paste passwords or API keys into chats or issues.

## 1. Database on Aiven

1. Create a **MySQL** service (8.0.16+). Pick a region close to the one you will use on Render.
2. From the service **Overview** page note: host, port, user (`avnadmin`), password. Download the **CA certificate** as `ca.pem`.
3. Import the schema from your PC (one line, Command Prompt; `ecommerce_db.sql` already contains all migrations):

   ```
   "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -h <host> -P <port> -u avnadmin -p --ssl-mode=VERIFY_CA --ssl-ca=ca.pem --default-character-set=utf8mb4 < sql\ecommerce_db.sql
   ```

   It starts with `DROP DATABASE IF EXISTS ecommerce_db`: run it only once, on an empty service.
4. Better than using `avnadmin` in the app: create a user that can only touch this database
   (`CREATE USER 'shop'@'%' IDENTIFIED BY '...'; GRANT SELECT, INSERT, UPDATE, DELETE ON ecommerce_db.* TO 'shop'@'%';`).
   Note: the CHECK-constraint error message lookup reads `information_schema`, which is fine with these grants.

## 2. Test locally against Aiven first

Put this in `db.conf` (next to `server.exe`), with `ca.pem` in the same folder:

```
host=<aiven host>
port=<aiven port>
user=shop
password=<password>
database=ecommerce_db
ssl_ca=ca.pem
```

Build with `build.bat`, run `server.exe`, open http://localhost:8080. If this works, only Render is left to debug.

## 3. Web service on Render

1. **New → Web Service** → connect GitHub → choose this repo. Runtime: **Docker** (Render finds the `Dockerfile`).
2. **Environment → Secret Files**: add
   - `db.conf`: same as above but with `ssl_ca=/etc/secrets/ca.pem`
   - `ca.pem`: paste the certificate contents
3. Deploy. In the logs you should see `config: /etc/secrets/db.conf` and `Listening on http://localhost:<PORT>`.
   Render sets `PORT` itself; the server reads it (locally it falls back to 8080).
4. Make an admin: sign up on the site, then run on Aiven
   `UPDATE users SET role = 'ADMIN' WHERE email = '<your email>';`

## Things to know

- **Free tier sleep:** Render's free web services stop after ~15 min without traffic; the first request then takes
  30-60 s. While asleep the delivery thread and session cleanup do not run. Open the site a minute before a demo.
- **Time zone:** the container uses `TZ=Asia/Kolkata`; Aiven MySQL runs in UTC, so times produced by SQL (`NOW()`)
  may show in UTC. Check Aiven's advanced configuration for a default time zone setting if this matters.
- **/metrics** stays blocked on Render (requests come through Render's proxy, not from 127.0.0.1). That is intended.
- **Testing routes:** `/slow`, `/dbslow` and `/dbtest` are public. `/dbslow` holds a DB connection for 2 s and the
  pool has 4, so a few repeated hits can stall the shop. Consider disabling them before sharing the URL.
- **Passwords are stored in plain text** (see `common.h`). Do not let real users reuse real passwords on this site
  until passwords are hashed.

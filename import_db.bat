@echo off
REM Imports sql\ecommerce_db.sql into a cloud MySQL (Aiven). Run from the project folder.
REM Asks for host/port/user; mysql.exe asks for the password itself (it is never stored or printed).
REM WARNING: the SQL starts with DROP DATABASE IF EXISTS ecommerce_db - only run on a fresh database.
setlocal
set MYSQL=C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe
if not exist "%MYSQL%" (
  echo mysql.exe not found at "%MYSQL%". Edit the MYSQL line in this file.
  exit /b 1
)

set /p DBHOST=Aiven host:
set /p DBPORT=Aiven port:
set DBUSER=avnadmin
set /p DBUSER=User [avnadmin]:

set SSL=--ssl-mode=REQUIRED
if exist ca.pem set SSL=--ssl-mode=VERIFY_CA --ssl-ca=ca.pem
if not exist ca.pem echo ca.pem not found here: connecting with SSL but without certificate check.

echo Importing...
"%MYSQL%" -h %DBHOST% -P %DBPORT% -u %DBUSER% -p %SSL% --default-character-set=utf8mb4 < sql\ecommerce_db.sql
if errorlevel 1 (
  echo IMPORT FAILED
  exit /b 1
)

echo Checking tables (password again):
"%MYSQL%" -h %DBHOST% -P %DBPORT% -u %DBUSER% -p %SSL% -e "SELECT COUNT(*) AS tables_created FROM information_schema.tables WHERE table_schema='ecommerce_db'; SELECT COUNT(*) AS products FROM ecommerce_db.products;"
echo Done. Expect 15 tables and 52 products.

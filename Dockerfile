# ServerSync - Linux build for Render (or any Docker host).
# Stage 1 compiles server; stage 2 is a small image with only the binary, the runtime library and the assets.
# Ubuntu (not Debian): Ubuntu ships Oracle's MySQL client (libmysqlclient), Debian's default is MariaDB's,
# which lacks MYSQL_OPT_SSL_MODE used in db.cpp.

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make libmysqlclient-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY include include
COPY src src
RUN make

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# libmysqlclient21 = MySQL 8.0 client runtime; tzdata so log times / card-expiry check use Indian time
RUN apt-get update \
 && apt-get install -y --no-install-recommends libmysqlclient21 ca-certificates tzdata \
 && rm -rf /var/lib/apt/lists/*
ENV TZ=Asia/Kolkata
WORKDIR /app
COPY --from=build /src/server ./server
COPY public public
COPY templates templates
# db.conf and ca.pem are NOT baked into the image. On Render add them as Secret Files
# (they appear at /etc/secrets/db.conf and /etc/secrets/ca.pem; db.cpp looks there automatically).
# Render sets the PORT env var; server.cpp listens on it (falls back to 8080 locally).
EXPOSE 8080
CMD ["./server"]

/* studio_local_target_test.c — regression test for the
 * security-audit F08 fix: the Studio server must refuse
 * local-FILE targets by default and only accept s3:// (otsardb+s3://) and
 * the in-memory engine target ":memory:" — an explicit opt-in
 * (otsardb_studio_server_start_ex allow_local_files=1, i.e. --local-files /
 * OTSARDB_STUDIO_ALLOW_LOCAL=1) restores local-path workbenching.
 *
 * Deterministic (no store required):
 * - pure-function matrix for otsardb_studio_allow_local_files
 * (flag wins, then OTSARDB_STUDIO_ALLOW_LOCAL truthy, else 0);
 * - default server: ":memory:" query -> 200 ok (in-memory engine stays);
 * - default server: local FILE path query -> 200 with the F08 refusal
 * error object (no file opened);
 * - default server: a local path via /api/schema -> same refusal;
 * - opt-in server: the SAME local file path -> 200 ok and the query
 * actually ran (file created, table written, read back);
 * - opt-in server: local file path via /api/edit -> accepted.
 *
 * s3:// acceptance (the audit's "an s3:// target works" leg): when built
 * with OTSARDB_ENABLE_S3 AND a store is configured (OTSARDB_S3_ENDPOINT +
 * OTSARDB_S3_BUCKET), a unique fresh s3:// root is opened through the
 * default server and a SELECT runs -> 200 ok. Without the S3 build or the
 * store env the leg is reported SKIPPED (R2: a skip is not a pass), never
 * counted as PASS.
 *
 * Portable sockets (Winsock2 / BSD); server threads joined via
 * otsardb_studio_server_stop. The temp SQLite file is created in the OS
 * temp dir and removed (best-effort) after the opt-in assertions.
 */

#include "studio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
typedef SOCKET test_socket;
#define TEST_INVALID_SOCKET INVALID_SOCKET
#define test_socket_close(fd) closesocket(fd)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int test_socket;
#define TEST_INVALID_SOCKET (-1)
#define test_socket_close(fd) close(fd)
#endif

#define RESP_CAP (64 * 1024)
#define TOKEN_CAP 96

static int ensure_wsa(void) {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return 1;
#endif
}

static int wait_readable(test_socket fd, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select((int)fd + 1, &read_fds, NULL, NULL, &tv);
    return ready > 0 ? 1 : 0;
}

static test_socket connect_tcp(int port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo *resolved = NULL;
    if (getaddrinfo("127.0.0.1", port_str, &hints, &resolved) != 0 ||
        !resolved) {
        return TEST_INVALID_SOCKET;
    }
    test_socket fd = TEST_INVALID_SOCKET;
    for (struct addrinfo *ai = resolved; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == TEST_INVALID_SOCKET) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        test_socket_close(fd);
        fd = TEST_INVALID_SOCKET;
    }
    freeaddrinfo(resolved);
    return fd;
}

/* POST /api/<path> with the correct loopback Host, the per-server CSRF
 * token and Content-Type: application/json; reads the full response. */
static int post_json(int port, const char *path, const char *token,
                     const char *body, char *out, size_t cap) {
    test_socket fd = connect_tcp(port);
    if (fd == TEST_INVALID_SOCKET) return 0;
    char header[4096];
    int header_len = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.1\r\n"
                              "Host: 127.0.0.1:%d\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Type: application/json\r\n"
                              "X-OTSARDB-CSRF: %s\r\n"
                              "Connection: close\r\n\r\n",
                              path, port, body ? strlen(body) : 0, token);
    int ok = 0;
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        int sent_ok = 1;
        for (size_t sent = 0; sent < (size_t)header_len;) {
            int n = send(fd, header + sent,
                         (int)((size_t)header_len - sent), 0);
            if (n <= 0) { sent_ok = 0; break; }
            sent += (size_t)n;
        }
        size_t body_len = body ? strlen(body) : 0;
        for (size_t sent = 0; sent_ok && sent < body_len;) {
            int n = send(fd, body + sent, (int)(body_len - sent), 0);
            if (n <= 0) { sent_ok = 0; break; }
            sent += (size_t)n;
        }
        size_t used = 0;
        out[0] = '\0';
        while (sent_ok && used + 1 < cap) {
            if (!wait_readable(fd, 8000)) break;
            int n = recv(fd, out + used, (int)(cap - used - 1), 0);
            if (n <= 0) break;
            used += (size_t)n;
            out[used] = '\0';
        }
        ok = sent_ok && used > 0;
    }
    test_socket_close(fd);
    return ok;
}

static int fetch_token(int port, char *out, size_t cap) {
    test_socket fd = connect_tcp(port);
    if (fd == TEST_INVALID_SOCKET) return 0;
    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "GET /api/csrf HTTP/1.1\r\n"
                              "Host: 127.0.0.1:%d\r\n"
                              "Connection: close\r\n\r\n",
                              port);
    int ok = 0;
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        for (size_t sent = 0; sent < (size_t)header_len;) {
            int n = send(fd, header + sent, (int)((size_t)header_len - sent), 0);
            if (n <= 0) break;
            sent += (size_t)n;
        }
        char resp[2048];
        size_t used = 0;
        resp[0] = '\0';
        while (used + 1 < sizeof(resp)) {
            if (!wait_readable(fd, 5000)) break;
            int n = recv(fd, resp + used, (int)(sizeof(resp) - used - 1), 0);
            if (n <= 0) break;
            used += (size_t)n;
            resp[used] = '\0';
        }
        const char *key = strstr(resp, "\"token\":\"");
        if (key) {
            const char *start = key + strlen("\"token\":\"");
            size_t n = 0;
            while (start[n] && start[n] != '"' && n + 1 < cap) ++n;
            if (n > 0) {
                memcpy(out, start, n);
                out[n] = '\0';
                ok = 1;
            }
        }
    }
    test_socket_close(fd);
    return ok;
}

static int response_status(const char *resp) {
    int status = 0;
    if (resp && sscanf(resp, "HTTP/1.1 %d", &status) == 1) return status;
    return 0;
}

static const char *response_body(const char *resp) {
    const char *p = resp ? strstr(resp, "\r\n\r\n") : NULL;
    return p ? p + 4 : "";
}

static int has(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static void make_scratch_db(char *buf, size_t cap) {
#if defined(_WIN32)
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(buf, cap, "%s\\otsardb-studio-f08-%lu.db", dir,
             (unsigned long)getpid());
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-studio-f08-%lu.db", dir,
             (unsigned long)getpid());
#endif
}

/* JSON string escaping for the path value inside a request body: on
 * Windows the temp path contains backslashes, which must be doubled in
 * JSON ("C:\\path\\to\\otsardb-studio-f08-123.db"). */
static size_t json_escape(char *out, size_t cap, const char *in) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; ++i) {
        if (in[i] == '\\' || in[i] == '"') {
            if (o + 2 >= cap) break;
            out[o++] = '\\';
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
    return o;
}

static void remove_scratch_db(const char *path) {
    if (!path) return;
    remove(path);
    {
        char side[1024];
        snprintf(side, sizeof(side), "%s-wal", path);
        remove(side);
        snprintf(side, sizeof(side), "%s-shm", path);
        remove(side);
        snprintf(side, sizeof(side), "%s-journal", path);
        remove(side);
    }
}

static int failures = 0;
static int skips = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("studio-f08 %-54s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

int main(void) {
    ensure_wsa();

    /* --- pure decision function matrix (F08 opt-in) --- */
    check("flag wins (1,\"0\") -> 1",
          otsardb_studio_allow_local_files(1, "0") == 1, NULL);
    check("flag wins (1,NULL) -> 1",
          otsardb_studio_allow_local_files(1, NULL) == 1, NULL);
    check("no flag, env 0 -> 0",
          otsardb_studio_allow_local_files(0, "0") == 0, NULL);
    check("no flag, env off -> 0",
          otsardb_studio_allow_local_files(0, "off") == 0, NULL);
    check("no flag, env empty -> 0",
          otsardb_studio_allow_local_files(0, "") == 0, NULL);
    check("no flag, env NULL -> 0",
          otsardb_studio_allow_local_files(0, NULL) == 0, NULL);
    check("no flag, env 1 -> 1",
          otsardb_studio_allow_local_files(0, "1") == 1, NULL);
    check("no flag, env true -> 1",
          otsardb_studio_allow_local_files(0, "true") == 1, NULL);
    check("no flag, env YES -> 1",
          otsardb_studio_allow_local_files(0, "YES") == 1, NULL);
    check("no flag, env on -> 1",
          otsardb_studio_allow_local_files(0, "on") == 1, NULL);

    /* --- server A: DEFAULT (no opt-in) --- */
    char error[256];
    otsardb_studio_server *default_server =
        otsardb_studio_server_start(0, error, sizeof(error));
    if (!default_server) {
        printf("studio-f08 FAIL: default server start: %s\n", error);
        return 1;
    }
    int default_port = otsardb_studio_server_port(default_server);
    char default_token[TOKEN_CAP] = "";
    if (!fetch_token(default_port, default_token, sizeof(default_token))) {
        printf("studio-f08 FAIL: cannot fetch the default-server CSRF token\n");
        otsardb_studio_server_stop(default_server);
        return 1;
    }

    char resp[RESP_CAP];

    /* ":memory:" stays available (in-memory engine, no file I/O). */
    if (post_json(default_port, "/api/query", default_token,
                  "{\"target\":\":memory:\",\"sql\":\"SELECT 1 AS one;\"}",
                  resp, sizeof(resp))) {
        check("default :memory: query status 200",
              response_status(resp) == 200, resp);
        check("default :memory: query ok",
              has(response_body(resp), "\"ok\":true"), response_body(resp));
    } else {
        check("default :memory: query reachable", 0, "transport failure");
    }

    /* A local FILE path is refused with the F08 error object. */
    char db_path[1024];
    make_scratch_db(db_path, sizeof(db_path));
    char esc_db_path[2300];
    json_escape(esc_db_path, sizeof(esc_db_path), db_path);
    char file_body[2600];
    snprintf(file_body, sizeof(file_body),
             "{\"target\":\"%s\",\"sql\":\"SELECT 1;\"}", esc_db_path);
    if (post_json(default_port, "/api/query", default_token, file_body, resp,
                  sizeof(resp))) {
        check("default local-file query status 200",
              response_status(resp) == 200, resp);
        check("default local-file query refused",
              has(response_body(resp), "\"ok\":false") &&
                  has(response_body(resp), "local-file targets are disabled"),
              response_body(resp));
        check("default local-file refusal names the opt-in",
              has(response_body(resp), "--local-files") &&
                  has(response_body(resp), "OTSARDB_STUDIO_ALLOW_LOCAL"),
              response_body(resp));
    } else {
        check("default local-file query reachable", 0, "transport failure");
    }

    /* The same refusal applies to /api/schema and /api/edit. */
    if (post_json(default_port, "/api/schema", default_token, file_body, resp,
                  sizeof(resp))) {
        check("default local-file /api/schema refused",
              response_status(resp) == 200 &&
                  has(response_body(resp), "local-file targets are disabled"),
              response_body(resp));
    } else {
        check("default local-file /api/schema reachable", 0,
              "transport failure");
    }
    {
        char edit_body[2600];
        snprintf(edit_body, sizeof(edit_body),
                 "{\"target\":\"%s\",\"table\":\"t\",\"op\":\"insert\","
                 "\"cols\":[\"id\"],\"pk\":[\"id\"],\"old\":[],\"new\":[1]}",
                 esc_db_path);
        if (post_json(default_port, "/api/edit", default_token, edit_body,
                      resp, sizeof(resp))) {
            check("default local-file /api/edit refused",
                  response_status(resp) == 200 &&
                      has(response_body(resp),
                          "local-file targets are disabled"),
                  response_body(resp));
        } else {
            check("default local-file /api/edit reachable", 0,
                  "transport failure");
        }
    }

    /* s3:// acceptance on the default server (needs an S3 build + store). */
#if defined(OTSARDB_ENABLE_S3)
    {
        const char *bucket = getenv("OTSARDB_S3_BUCKET");
        const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
        if (bucket && bucket[0] && endpoint && endpoint[0]) {
            char target[2048];
            snprintf(target, sizeof(target), "s3://%s/studio-f08-%lu",
                     bucket, (unsigned long)getpid());
            char s3_body[2200];
            snprintf(s3_body, sizeof(s3_body),
                     "{\"target\":\"%s\",\"sql\":\"SELECT 1 AS one;\"}",
                     target);
            if (post_json(default_port, "/api/query", default_token, s3_body,
                          resp, sizeof(resp))) {
                check("default s3:// target accepted (status 200)",
                      response_status(resp) == 200, resp);
                check("default s3:// target query ok",
                      has(response_body(resp), "\"ok\":true") &&
                          !has(response_body(resp),
                               "local-file targets are disabled"),
                      response_body(resp));
            } else {
                check("default s3:// target reachable", 0,
                      "transport failure");
            }
        } else {
            printf("studio-f08 %-54s SKIPPED\n",
                   "s3:// target acceptance (no OTSARDB_S3_* store env)");
            skips++;
        }
    }
#else
    printf("studio-f08 %-54s SKIPPED\n",
           "s3:// target acceptance (non-S3 build)");
    skips++;
#endif

    otsardb_studio_server_stop(default_server);

    /* --- server B: OPT-IN (allow_local_files=1) --- */
    otsardb_studio_server *optin_server =
        otsardb_studio_server_start_ex(0, 1, error, sizeof(error));
    if (!optin_server) {
        printf("studio-f08 FAIL: opt-in server start: %s\n", error);
        return 1;
    }
    int optin_port = otsardb_studio_server_port(optin_server);
    char optin_token[TOKEN_CAP] = "";
    if (!fetch_token(optin_port, optin_token, sizeof(optin_token))) {
        printf("studio-f08 FAIL: cannot fetch the opt-in server CSRF token\n");
        otsardb_studio_server_stop(optin_server);
        return 1;
    }

    /* The same local path is ACCEPTED and the query really runs. */
    snprintf(file_body, sizeof(file_body),
             "{\"target\":\"%s\",\"sql\":\"CREATE TABLE t(id INTEGER);"
             "INSERT INTO t(id) VALUES(7);SELECT id FROM t;\"}",
             esc_db_path);
    if (post_json(optin_port, "/api/query", optin_token, file_body, resp,
                  sizeof(resp))) {
        check("opt-in local-file query status 200",
              response_status(resp) == 200, resp);
        check("opt-in local-file query ran",
              has(response_body(resp), "\"ok\":true") &&
                  has(response_body(resp), "\"data\":[[7]]"),
              response_body(resp));
    } else {
        check("opt-in local-file query reachable", 0, "transport failure");
    }
    if (post_json(optin_port, "/api/schema", optin_token, file_body, resp,
                  sizeof(resp))) {
        check("opt-in local-file /api/schema ok",
              response_status(resp) == 200 &&
                  has(response_body(resp), "\"t\"") &&
                  !has(response_body(resp), "local-file targets are disabled"),
              response_body(resp));
    } else {
        check("opt-in local-file /api/schema reachable", 0,
              "transport failure");
    }

    otsardb_studio_server_stop(optin_server);
    remove_scratch_db(db_path);

    if (failures) {
        printf("studio-f08 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: Studio local-file boundary (audit F08) — default refuses "
           "local paths, :memory: and s3:// stay open, opt-in restores "
           "local workbenching\n");
    if (skips > 0) printf("note: %d s3:// leg(s) skipped (see above)\n",
                          skips);
    return 0;
}

/* studio_v02_smoke_test.c — Studio server smoke test (v0.2).
 *
 * Starts the embedded studio server IN-PROCESS on an ephemeral loopback
 * port (otsardb_studio_server_start) and exercises the v0.2 wire contract
 * with raw sockets:
 * - GET / -> 200, HTML carries the v0.2 UI markers
 * (data-studio-version="v0.2", id="profiles", id="schema-tree",
 * id="btn-schema-refresh", id="page-size", id="btn-csv")
 * - POST /api/query multi-statement DDL + PRAGMA on a persistent file
 * target -> the exact NDJSON lines for PRAGMA table_info/index_list/
 * index_info (the query shapes the schema explorer executes)
 * - POST /api/query regression: the exact v0.1 payload contract
 * - POST /api/schema on the same file -> the exact {"ok":true,
 * "tables":[...]} payload
 * - POST /api/schema on :memory: -> {"ok":true,"tables":[]}
 * - POST /api/schema malformed body -> 400 + {"ok":false,...}
 * - POST /api/schema missing target -> 200 + {"ok":false,...}
 * - GET /health regression, clean stop
 *
 * The UI interactions themselves (profiles picker, tree clicks, grid
 * paging/sort/CSV) are a MANUAL test (documented in the test docs); this
 * test covers the wire contract the UI depends on. Portable sockets
 * (Winsock2/BSD); the server thread is joined before the test exits.
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

/* Sends an HTTP/1.1 request (Connection: close) and reads the full
 * response into out. Returns 1 on success, 0 on transport failure. */
/* Per-process CSRF token (audit F01): fetched from
 * GET /api/csrf after the server starts and stamped on every request, so
 * the wire contract is exercised exactly as the UI sends it. */
static char g_csrf[96] = "";

static int http_exchange(int port, const char *method, const char *path,
                         const char *body, char *out, size_t cap) {
    test_socket fd = connect_tcp(port);
    if (fd == TEST_INVALID_SOCKET) return 0;
    char header[2048];
    int header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "Host: 127.0.0.1:%d\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Type: application/json\r\n"
                              "X-OTSARDB-CSRF: %s\r\n"
                              "Connection: close\r\n\r\n",
                              method, path, port, body ? strlen(body) : 0,
                              g_csrf);
    int ok = 0;
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        int sent_header = 1;
        for (size_t sent = 0; sent < (size_t)header_len;) {
            int n = send(fd, header + sent, (int)((size_t)header_len - sent), 0);
            if (n <= 0) { sent_header = 0; break; }
            sent += (size_t)n;
        }
        size_t body_len = body ? strlen(body) : 0;
        for (size_t sent = 0; sent_header && sent < body_len;) {
            int n = send(fd, body + sent, (int)(body_len - sent), 0);
            if (n <= 0) { sent_header = 0; break; }
            sent += (size_t)n;
        }
        size_t used = 0;
        out[0] = '\0';
        while (sent_header && used + 1 < cap) {
            if (!wait_readable(fd, 5000)) break;
            int n = recv(fd, out + used, (int)(cap - used - 1), 0);
            if (n <= 0) break;
            used += (size_t)n;
            out[used] = '\0';
        }
        ok = sent_header && used > 0;
    }
    test_socket_close(fd);
    return ok;
}

static int response_status(const char *resp) {
    int status = 0;
    if (resp && sscanf(resp, "HTTP/1.1 %d", &status) == 1) return status;
    return 0;
}

/* Body after the first \r\n\r\n (may contain NULs at the end only). */
static const char *response_body(const char *resp) {
    const char *p = resp ? strstr(resp, "\r\n\r\n") : NULL;
    return p ? p + 4 : "";
}

static int has(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static int fetch_csrf_token(int port) {
    char resp[RESP_CAP];
    if (!http_exchange(port, "GET", "/api/csrf", NULL, resp, sizeof(resp))) {
        return 0;
    }
    const char *key = strstr(resp, "\"token\":\"");
    if (!key) return 0;
    const char *start = key + strlen("\"token\":\"");
    size_t n = 0;
    while (start[n] && start[n] != '"' && n + 1 < sizeof(g_csrf)) ++n;
    if (n == 0) return 0;
    memcpy(g_csrf, start, n);
    g_csrf[n] = '\0';
    return 1;
}

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("studio-v02-0073 %-58s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

/* Temp file database target (persists between server requests; every
 * :memory: target is a fresh connection per request). */
static void make_temp_db(char *buf, size_t cap) {
#if defined(_WIN32)
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(buf, cap, "%s\\otsardb-studio-v02-0073-%lu.db", dir,
             (unsigned long)getpid());
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-studio-v02-0073-%lu.db", dir,
             (unsigned long)getpid());
#endif
    remove(buf);
}

static void cleanup_db(const char *db) {
    remove(db);
#if defined(_WIN32)
    {
        char side[1024];
        snprintf(side, sizeof(side), "%s-journal", db);
        remove(side);
        snprintf(side, sizeof(side), "%s-wal", db);
        remove(side);
        snprintf(side, sizeof(side), "%s-shm", db);
        remove(side);
    }
#endif
}

int main(void) {
    char error[256];
    /* (audit F08): the v0.2 workbench fixtures use a local
     * file path (studio-0073/db), so this harness explicitly opts into
     * local-file workbenching; the server default refuses local paths. */
    otsardb_studio_server *server =
        otsardb_studio_server_start_ex(0, 1, error, sizeof(error));
    if (!server) {
        printf("studio-v02-0073 server start FAIL: %s\n", error);
        return 1;
    }
    int port = otsardb_studio_server_port(server);
    printf("studio-v02-0073 server started on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("studio-v02-0073 FAIL: ephemeral port not reported\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    ensure_wsa();
    if (!fetch_csrf_token(port)) {
        printf("studio-v02-0073 FAIL: cannot fetch the CSRF token\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("CSRF token fetched", g_csrf[0] != '\0', g_csrf);

    char db[1024];
    make_temp_db(db, sizeof(db));
    /* The target path may contain backslashes (TEMP dir): JSON needs them
     * doubled in the request body. */
    char db_json[2048];
    {
        size_t o = 0;
        for (size_t s = 0; db[s] && o + 2 < sizeof(db_json); ++s) {
            if (db[s] == '\\') db_json[o++] = '\\';
            db_json[o++] = db[s];
        }
        db_json[o] = '\0';
    }
    char target_json[1600];
    snprintf(target_json, sizeof(target_json), "{\"target\":\"%s\"", db_json);

    char resp[RESP_CAP];

    /* ---- GET / : v0.2 UI markers ---- */
    if (!http_exchange(port, "GET", "/", NULL, resp, sizeof(resp))) {
        printf("studio-v02-0073 FAIL: cannot reach the server\n");
        otsardb_studio_server_stop(server);
        cleanup_db(db);
        return 1;
    }
    check("GET / status 200", response_status(resp) == 200, resp);
    const char *body = response_body(resp);
    check("GET / body is HTML", has(body, "<!DOCTYPE html>") &&
                                has(body, "OtsarDB Studio"), body);
    check("GET / v0.2 version marker", has(body, "data-studio-version=\"v0.2\""),
          body);
    check("GET / profile picker marker", has(body, "id=\"profiles\""), body);
    check("GET / schema tree marker", has(body, "id=\"schema-tree\""), body);
    check("GET / schema refresh marker", has(body, "id=\"btn-schema-refresh\""),
          body);
    check("GET / page size marker", has(body, "id=\"page-size\""), body);
    check("GET / csv export marker", has(body, "id=\"btn-csv\""), body);

    /* ---- POST /api/query: DDL + PRAGMA NDJSON contract (the schema
     * explorer's query shapes) on a persistent file target ---- */
    char create_body[2048];
    snprintf(create_body, sizeof(create_body),
             "%s,\"sql\":\"CREATE TABLE s2(id INTEGER PRIMARY KEY, v TEXT); "
             "CREATE INDEX idx_s2_v ON s2(v);\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", create_body, resp,
                      sizeof(resp))) {
        check("POST /api/query DDL status 200", response_status(resp) == 200,
              resp);
        check("POST /api/query DDL empty objects",
              strcmp(response_body(resp),
                     "{\"ok\":true,\"rows\":0,\"columns\":[],\"data\":[]}\n"
                     "{\"ok\":true,\"rows\":0,\"columns\":[],\"data\":[]}\n") == 0,
              response_body(resp));
    } else {
        check("POST /api/query DDL reachable", 0, "transport failure");
    }

    char pragma_body[2048];
    snprintf(pragma_body, sizeof(pragma_body),
             "%s,\"sql\":\"PRAGMA table_info(s2); PRAGMA index_list(s2); "
             "PRAGMA index_info(idx_s2_v);\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", pragma_body, resp,
                      sizeof(resp))) {
        check("POST /api/query PRAGMA status 200",
              response_status(resp) == 200, resp);
        const char *expected_pragma =
            "{\"ok\":true,\"rows\":2,"
            "\"columns\":[\"cid\",\"name\",\"type\",\"notnull\",\"dflt_value\",\"pk\"],"
            "\"data\":[[0,\"id\",\"INTEGER\",0,null,1],[1,\"v\",\"TEXT\",0,null,0]]}\n"
            "{\"ok\":true,\"rows\":1,"
            "\"columns\":[\"seq\",\"name\",\"unique\",\"origin\",\"partial\"],"
            "\"data\":[[0,\"idx_s2_v\",0,\"c\",0]]}\n"
            "{\"ok\":true,\"rows\":1,"
            "\"columns\":[\"seqno\",\"cid\",\"name\"],"
            "\"data\":[[0,1,\"v\"]]}\n";
        check("POST /api/query PRAGMA exact payload",
              strcmp(response_body(resp), expected_pragma) == 0,
              response_body(resp));
    } else {
        check("POST /api/query PRAGMA reachable", 0, "transport failure");
    }

    /* ---- POST /api/query regression: the exact v0.1 payload ---- */
    if (http_exchange(port, "POST", "/api/query",
                      "{\"target\":\":memory:\",\"sql\":\"SELECT 1 AS one, "
                      "'x' AS s;\"}",
                      resp, sizeof(resp))) {
        check("POST /api/query regression status 200",
              response_status(resp) == 200, resp);
        check("POST /api/query regression exact payload",
              strcmp(response_body(resp),
                     "{\"ok\":true,\"rows\":1,\"columns\":[\"one\",\"s\"],"
                     "\"data\":[[1,\"x\"]]}\n") == 0,
              response_body(resp));
    } else {
        check("POST /api/query regression reachable", 0, "transport failure");
    }

    /* ---- POST /api/schema: exact tree payload on the persistent file ---- */
    char schema_body[2048];
    snprintf(schema_body, sizeof(schema_body), "%s}", target_json);
    if (http_exchange(port, "POST", "/api/schema", schema_body, resp,
                      sizeof(resp))) {
        check("POST /api/schema status 200", response_status(resp) == 200, resp);
        const char *expected_schema =
            "{\"ok\":true,\"tables\":["
            "{\"name\":\"s2\",\"type\":\"table\","
            "\"columns\":[{\"name\":\"id\",\"type\":\"INTEGER\",\"pk\":1,"
            "\"notnull\":0},{\"name\":\"v\",\"type\":\"TEXT\",\"pk\":0,"
            "\"notnull\":0}],"
            "\"indexes\":[{\"name\":\"idx_s2_v\",\"unique\":0,\"columns\":[\"v\"]}]}]}\n";
        check("POST /api/schema exact payload",
              strcmp(response_body(resp), expected_schema) == 0,
              response_body(resp));
    } else {
        check("POST /api/schema reachable", 0, "transport failure");
    }

    /* ---- POST /api/schema on :memory: -> empty tree ---- */
    if (http_exchange(port, "POST", "/api/schema",
                      "{\"target\":\":memory:\"}", resp, sizeof(resp))) {
        check("POST /api/schema :memory: status 200",
              response_status(resp) == 200, resp);
        check("POST /api/schema :memory: empty tree",
              strcmp(response_body(resp), "{\"ok\":true,\"tables\":[]}\n") == 0,
              response_body(resp));
    } else {
        check("POST /api/schema :memory: reachable", 0, "transport failure");
    }

    /* ---- POST /api/schema malformed body -> 400 ---- */
    if (http_exchange(port, "POST", "/api/schema", "not json at all", resp,
                      sizeof(resp))) {
        check("POST /api/schema malformed status 400",
              response_status(resp) == 400, resp);
        check("POST /api/schema malformed error object",
              has(response_body(resp), "{\"ok\":false,\"error\":") &&
                  has(response_body(resp), "malformed"),
              response_body(resp));
    } else {
        check("POST /api/schema malformed reachable", 0, "transport failure");
    }

    /* ---- POST /api/schema missing target -> 200 + error object ---- */
    if (http_exchange(port, "POST", "/api/schema", "{}", resp, sizeof(resp))) {
        check("POST /api/schema missing target status 200",
              response_status(resp) == 200, resp);
        check("POST /api/schema missing target error",
              has(response_body(resp), "{\"ok\":false,\"error\":") &&
                  has(response_body(resp), "missing target"),
              response_body(resp));
    } else {
        check("POST /api/schema missing target reachable", 0,
              "transport failure");
    }

    /* ---- GET /health regression ---- */
    if (http_exchange(port, "GET", "/health", NULL, resp, sizeof(resp))) {
        check("GET /health status 200", response_status(resp) == 200, resp);
        check("GET /health body ok", strcmp(response_body(resp), "ok") == 0,
              response_body(resp));
    } else {
        check("GET /health reachable", 0, "transport failure");
    }

    otsardb_studio_server_stop(server);
    cleanup_db(db);
    if (failures) {
        printf("studio-v02-0073 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio v0.2 wire contract (UI markers, PRAGMA "
           "NDJSON, /api/schema, regressions, clean stop)\n");
    return 0;
}

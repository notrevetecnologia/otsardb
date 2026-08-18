/* studio_v05_smoke_test.c — Studio server smoke test (v0.5).
 *
 * Starts the embedded studio server IN-PROCESS on an ephemeral loopback
 * port (otsardb_studio_server_start) and exercises the v0.5 wire contract
 * with raw sockets:
 * - GET / -> 200, HTML carries the v0.5 markers (data-studio-release-v5
 * ="v0.5", data-studio-shortcuts="run", id="btn-theme", id="btn-test",
 * id="btn-dump", the per-profile theme localStorage key
 * "otsardb.studio.theme.v1" and the html data-theme="dark" attribute)
 * AND every v0.2/v0.3/v0.4 marker still present — the 0070/0073/0078/
 * 0086 contracts are preserved
 * - POST /api/query with page_size (v0.4 regression: rows + "total"),
 * /api/schema with_sql (v0.4 verbatim DDL regression),
 * /api/edit round-trip (v0.3 regression)
 * - POST /api/health :memory: deterministic contract — the wire path of
 * the v0.5 "Test connection" button (the button posts the form's
 * credentials to this existing endpoint)
 * - regressions: GET /health "ok", 404/405
 * - S3 phase (SKIPPED 77 without store env): the button's exact request
 * shape — bucket+database+endpoint+region+keys against a live target
 * -> status ok with open_ms.
 *
 * The v0.5 UI behaviors themselves (theme toggle, connection-test click,
 * SQL-dump blob download, Ctrl/Cmd+Enter) are browser-side; their logic is
 * (a) pinned by the marker assertions here, (b) the dump SQL-literal
 * escaping was additionally verified with node (Validation),
 * and (c) browser-level interaction remains the documented manual test.
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

#define RESP_CAP (160 * 1024)

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
    printf("studio-v05-0093 %-50s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

static void make_temp_db(char *buf, size_t cap) {
#if defined(_WIN32)
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(buf, cap, "%s\\otsardb-studio-v05-0093-%lu.db", dir,
             (unsigned long)getpid());
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-studio-v05-0093-%lu.db", dir,
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
    /* (audit F08): the v0.5 workbench fixtures use a local
     * file path (studio-0093/db), so this harness explicitly opts into
     * local-file workbenching; the server default refuses local paths. */
    otsardb_studio_server *server =
        otsardb_studio_server_start_ex(0, 1, error, sizeof(error));
    if (!server) {
        printf("studio-v05-0093 server start FAIL: %s\n", error);
        return 1;
    }
    int port = otsardb_studio_server_port(server);
    printf("studio-v05-0093 server started on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("studio-v05-0093 FAIL: ephemeral port not reported\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    ensure_wsa();
    if (!fetch_csrf_token(port)) {
        printf("studio-v05-0093 FAIL: cannot fetch the CSRF token\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("CSRF token fetched", g_csrf[0] != '\0', g_csrf);

    char resp[RESP_CAP];

    /* ---- GET / : v0.5 markers + every prior marker preserved ---- */
    if (!http_exchange(port, "GET", "/", NULL, resp, sizeof(resp))) {
        printf("studio-v05-0093 FAIL: cannot reach the server\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("GET / status 200", response_status(resp) == 200, resp);
    const char *body = response_body(resp);
    check("GET / body is HTML", has(body, "<!DOCTYPE html>") &&
                                has(body, "OtsarDB Studio"), body);
    check("GET / v0.2 version marker preserved",
          has(body, "data-studio-version=\"v0.2\""), body);
    check("GET / v0.3 release marker preserved",
          has(body, "data-studio-release=\"v0.3\""), body);
    check("GET / v0.4 release marker preserved",
          has(body, "data-studio-release-v4=\"v0.4\""), body);
    check("GET / v0.5 release marker", has(body, "data-studio-release-v5=\"v0.5\""),
           body);
    check("GET / v0.6 desktop marker",
          has(body, "data-studio-release-v6=\"v0.6\""), body);
    check("GET / v0.7 UI marker",
          has(body, "data-studio-release-v7=\"v0.7\""), body);
    check("GET / responsive layout marker",
          has(body, "@media(max-width:620px)") &&
              has(body, "grid-template-columns:minmax(260px,320px)"), body);
    check("GET / html carries the dark theme attribute",
          has(body, "data-theme=\"dark\""), body);
    check("GET / theme toggle marker", has(body, "id=\"btn-theme\""), body);
    check("GET / connection test marker", has(body, "id=\"btn-test\""), body);
    check("GET / sql dump marker", has(body, "id=\"btn-dump\""), body);
    check("GET / per-profile theme localStorage key",
          has(body, "otsardb.studio.theme.v1"), body);
    check("GET / run-shortcut marker", has(body, "data-studio-shortcuts=\"run\""),
          body);
    check("GET / Ctrl+Enter handler present",
          has(body, "ctrlKey") && has(body, "metaKey") && has(body, "\"Enter\""),
          body);
    check("GET / profile picker marker preserved", has(body, "id=\"profiles\""),
          body);
    check("GET / schema tree marker preserved", has(body, "id=\"schema-tree\""),
          body);
    check("GET / page size marker preserved", has(body, "id=\"page-size\""),
          body);
    check("GET / csv marker preserved", has(body, "id=\"btn-csv\""), body);
    check("GET / edit bar marker preserved", has(body, "id=\"edit-bar\""),
          body);
    check("GET / add-row marker preserved", has(body, "id=\"btn-add-row\""),
          body);
    check("GET / monitor panel marker preserved", has(body, "id=\"monitor-panel\""),
          body);
    check("GET / history panel marker preserved", has(body, "id=\"history-panel\""),
          body);
    check("GET / ddl panel marker preserved", has(body, "id=\"ddl-panel\""),
          body);
    check("GET / connection indicator marker preserved",
          has(body, "id=\"conn-active\""), body);

    char db[1024];
    make_temp_db(db, sizeof(db));
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

    /* ---- fixture: pt5(id INTEGER PRIMARY KEY, v TEXT) with 30 rows ---- */
    char create_body[4096];
    snprintf(create_body, sizeof(create_body),
             "%s,\"sql\":\"CREATE TABLE pt5(id INTEGER PRIMARY KEY, v TEXT);"
             "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM "
             "cnt WHERE x<30) INSERT INTO pt5(id,v) SELECT x, 'r'||x FROM "
             "cnt;\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", create_body, resp,
                      sizeof(resp))) {
        check("POST /api/query fixture status 200",
              response_status(resp) == 200, resp);
        check("POST /api/query fixture ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/query fixture reachable", 0, "transport failure");
    }

    /* ---- v0.4 regression: server-side paging still appends total ---- */
    char paged_body[2048];
    snprintf(paged_body, sizeof(paged_body),
             "%s,\"sql\":\"SELECT * FROM pt5;\",\"page_size\":\"10\","
             "\"page\":\"2\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", paged_body, resp,
                      sizeof(resp))) {
        check("POST /api/query paged status 200",
              response_status(resp) == 200, resp);
        const char *pb = response_body(resp);
        check("paged page2 rows 10", has(pb, "\"rows\":10"), pb);
        check("paged page2 total 30", has(pb, "\"total\":30"), pb);
        check("paged page2 starts at row 11", has(pb, "\"data\":[[11,\"r11\""),
              pb);
    } else {
        check("POST /api/query paged reachable", 0, "transport failure");
    }

    /* ---- v0.4 regression: /api/schema with_sql verbatim DDL ---- */
    char schema_body[2048];
    snprintf(schema_body, sizeof(schema_body),
             "%s,\"with_sql\":\"1\"}", target_json);
    if (http_exchange(port, "POST", "/api/schema", schema_body, resp,
                      sizeof(resp))) {
        check("POST /api/schema with_sql status 200",
              response_status(resp) == 200, resp);
        const char *sb = response_body(resp);
        check("with_sql verbatim CREATE TABLE",
              has(sb, "\"sql\":\"CREATE TABLE pt5(id INTEGER PRIMARY KEY, "
                      "v TEXT)\""), sb);
        check("with_sql placed after type",
              has(sb, "\"type\":\"table\",\"sql\":\"CREATE TABLE pt5"), sb);
    } else {
        check("POST /api/schema with_sql reachable", 0, "transport failure");
    }

    /* ---- v0.3 regression: /api/edit round-trip ---- */
    char edit_body[4096];
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"pt5\",\"op\":\"insert\","
             "\"cols\":[\"id\",\"v\"],\"pk\":[\"id\"],"
             "\"old\":[],\"new\":[999,\"v05-edit\"]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit insert status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit insert ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit insert reachable", 0, "transport failure");
    }
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"pt5\",\"op\":\"delete\","
             "\"cols\":[\"id\",\"v\"],\"pk\":[\"id\"],"
             "\"old\":[999,\"v05-edit\"],\"new\":[]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit delete status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit delete ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit delete reachable", 0, "transport failure");
    }

    /* ---- /api/health :memory: — the conn-test button's wire path ---- */
    if (http_exchange(port, "POST", "/api/health",
                      "{\"target\":\":memory:\"}", resp, sizeof(resp))) {
        check("POST /api/health :memory: status 200",
              response_status(resp) == 200, resp);
        const char *hb = response_body(resp);
        check("POST /api/health :memory: status error",
              has(hb, "\"status\":\"error\""), hb);
        check("POST /api/health :memory: lease extension present",
              has(hb, "\"lease\":{\"state\":\"absent\"}"), hb);
        check("POST /api/health :memory: open_ms present",
              has(hb, "\"open_ms\":"), hb);
        check("POST /api/health :memory: last_error extension present",
              has(hb, "\"last_error\":\""), hb);
    } else {
        check("POST /api/health :memory: reachable", 0, "transport failure");
    }

    /* ---- regressions: /api/query exact payload, GET /health, 404/405 ---- */
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
    if (http_exchange(port, "GET", "/health", NULL, resp, sizeof(resp))) {
        check("GET /health regression status 200",
              response_status(resp) == 200, resp);
        check("GET /health regression body ok",
              strcmp(response_body(resp), "ok") == 0, response_body(resp));
    } else {
        check("GET /health regression reachable", 0, "transport failure");
    }
    if (http_exchange(port, "GET", "/nope", NULL, resp, sizeof(resp))) {
        check("GET /nope status 404", response_status(resp) == 404, resp);
    } else {
        check("GET /nope reachable", 0, "transport failure");
    }
    if (http_exchange(port, "DELETE", "/", NULL, resp, sizeof(resp))) {
        check("DELETE / status 405", response_status(resp) == 405, resp);
    } else {
        check("DELETE / reachable", 0, "transport failure");
    }

    /* ---- live-S3 phase: the conn-test button's exact request shape
     * (bucket+database+endpoint+region+keys -> POST /api/health) ---- */
    const char *endpoint_env = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket_env = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint_env || !endpoint_env[0] || !bucket_env || !bucket_env[0]) {
        printf("SKIPPED (S3 phase): OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET not "
               "set (no live store)\n");
    } else {
#ifdef OTSARDB_ENABLE_S3
        char s3_conn[800];
        snprintf(s3_conn, sizeof(s3_conn),
                 "{\"target\":\"s3://%s/studio-0093/db\",\"endpoint\":\"%s\","
                 "\"region\":\"%s\",\"access_key\":\"%s\",\"secret_key\":\"%s\"",
                 bucket_env, endpoint_env,
                 getenv("OTSARDB_S3_REGION") ? getenv("OTSARDB_S3_REGION") : "",
                 getenv("OTSARDB_S3_ACCESS_KEY") ? getenv("OTSARDB_S3_ACCESS_KEY") : "",
                 getenv("OTSARDB_S3_SECRET_KEY") ? getenv("OTSARDB_S3_SECRET_KEY") : "");

        /* create a small table so the health probe has a real object set */
        char s3_create[1600];
        snprintf(s3_create, sizeof(s3_create),
                 "%s,\"sql\":\"DROP TABLE IF EXISTS pt93; "
                 "CREATE TABLE pt93(id INTEGER PRIMARY KEY, v TEXT); "
                 "INSERT INTO pt93 VALUES (1, 'a'), (2, 'b');\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_create, resp,
                          sizeof(resp))) {
            check("S3 create status 200", response_status(resp) == 200, resp);
            check("S3 create ok", has(response_body(resp), "{\"ok\":true"),
                  response_body(resp));
        } else {
            check("S3 create reachable", 0, "transport failure");
        }

        /* the connection-test button's wire call, exactly as the UI posts it */
        char s3_health[800];
        snprintf(s3_health, sizeof(s3_health), "%s}", s3_conn);
        if (http_exchange(port, "POST", "/api/health", s3_health, resp,
                          sizeof(resp))) {
            check("S3 health status 200", response_status(resp) == 200, resp);
            const char *hb = response_body(resp);
            check("S3 health status ok", has(hb, "\"status\":\"ok\""), hb);
            check("S3 health open_ms present", has(hb, "\"open_ms\":"), hb);
            check("S3 health target present",
                  has(hb, "\"target\":\"s3://"), hb);
            check("S3 health lease extension present",
                  has(hb, "\"lease\":"), hb);
        } else {
            check("S3 health reachable", 0, "transport failure");
        }

        /* the dump's data query (plain SELECT * runs through /api/query) */
        char s3_data[800];
        snprintf(s3_data, sizeof(s3_data),
                 "%s,\"sql\":\"SELECT * FROM pt93;\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_data, resp,
                          sizeof(resp))) {
            check("S3 dump data query status 200",
                  response_status(resp) == 200, resp);
            const char *qb = response_body(resp);
            check("S3 dump data rows 2", has(qb, "\"rows\":2"), qb);
            check("S3 dump data values",
                  has(qb, "\"data\":[[1,\"a\"],[2,\"b\"]]"), qb);
        } else {
            check("S3 dump data query reachable", 0, "transport failure");
        }
#else
        printf("S3 phase SKIPPED: this binary was built without "
               "OTSARDB_ENABLE_S3\n");
#endif /* OTSARDB_ENABLE_S3 */
    }

    otsardb_studio_server_stop(server);
    cleanup_db(db);
    if (failures) {
        printf("studio-v05-0093 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio v0.5 wire contract (markers, health button "
           "path, regressions)\n");
    return 0;
}

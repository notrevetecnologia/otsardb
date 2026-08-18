/* studio_v04_smoke_test.c — Studio server smoke test (v0.4).
 *
 * Starts the embedded studio server IN-PROCESS on an ephemeral loopback
 * port (otsardb_studio_server_start) and exercises the v0.4 wire contract
 * with raw sockets:
 * - GET / -> 200, HTML carries the v0.4 markers (data-studio-release-v4
 * ="v0.4", id="history-panel", id="btn-history-clear", id="ddl-panel",
 * id="ddl-text", id="conn-active") AND every v0.2/v0.3 marker still
 * present (data-studio-version="v0.2", data-studio-release="v0.3",
 * id="profiles", id="schema-tree", id="page-size", id="btn-csv",
 * monitor/admin markers) — the 0070/0073/0078 contracts are preserved
 * - POST /api/query SERVER-SIDE PAGING on a persistent file target with
 * a 120-row table: page_size=50&page=1 -> rows=50 + "total":120;
 * page=3 -> rows=20 (rows 101..120); page_size=200&page=2 -> rows=0 +
 * total=120; the "total" field equals COUNT(*); plain-shape with an
 * explicit LIMIT is NOT paged (no total); non-plain SQL with page_size
 * is NOT paged (full result, no total, client-side paging preserved);
 * no page_size -> exact v0.3 payload (no total)
 * - POST /api/schema with "with_sql":"1" -> every object carries the
 * verbatim stored CREATE statement ("sql"); without the field the
 * payload is byte-identical to v0.3 (no "sql" key)
 * - regressions: /api/query exact payload, /api/edit round-trip,
 * /api/health :memory: deterministic contract, GET /health "ok",
 * 404/405
 * - S3 phase (SKIPPED 77 without store env): paged query + count on a
 * live target, /api/schema with_sql.
 *
 * History persistence is browser-side (localStorage) -> UI manual test,
 * documented in .
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
    printf("studio-v04-0086 %-52s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

static void make_temp_db(char *buf, size_t cap) {
#if defined(_WIN32)
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(buf, cap, "%s\\otsardb-studio-v04-0086-%lu.db", dir,
             (unsigned long)getpid());
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-studio-v04-0086-%lu.db", dir,
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
    /* (audit F08): the v0.4 workbench fixtures use a local
     * file path (studio-0086/db), so this harness explicitly opts into
     * local-file workbenching; the server default refuses local paths. */
    otsardb_studio_server *server =
        otsardb_studio_server_start_ex(0, 1, error, sizeof(error));
    if (!server) {
        printf("studio-v04-0086 server start FAIL: %s\n", error);
        return 1;
    }
    int port = otsardb_studio_server_port(server);
    printf("studio-v04-0086 server started on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("studio-v04-0086 FAIL: ephemeral port not reported\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    ensure_wsa();
    if (!fetch_csrf_token(port)) {
        printf("studio-v04-0086 FAIL: cannot fetch the CSRF token\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("CSRF token fetched", g_csrf[0] != '\0', g_csrf);

    char resp[RESP_CAP];

    /* ---- GET / : v0.4 markers + every v0.2/v0.3 marker preserved ---- */
    if (!http_exchange(port, "GET", "/", NULL, resp, sizeof(resp))) {
        printf("studio-v04-0086 FAIL: cannot reach the server\n");
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
    check("GET / v0.4 release marker", has(body, "data-studio-release-v4=\"v0.4\""),
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
    check("GET / history panel marker", has(body, "id=\"history-panel\""),
          body);
    check("GET / history list marker", has(body, "id=\"history-list\""),
          body);
    check("GET / history clear marker", has(body, "id=\"btn-history-clear\""),
          body);
    check("GET / ddl panel marker", has(body, "id=\"ddl-panel\""), body);
    check("GET / ddl text marker", has(body, "id=\"ddl-text\""), body);
    check("GET / connection indicator marker", has(body, "id=\"conn-active\""),
          body);

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

    /* ---- fixture: pt(id INTEGER PRIMARY KEY, v TEXT) with 120 rows ---- */
    char create_body[4096];
    snprintf(create_body, sizeof(create_body),
             "%s,\"sql\":\"CREATE TABLE pt(id INTEGER PRIMARY KEY, v TEXT);"
             "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM "
             "cnt WHERE x<120) INSERT INTO pt(id,v) SELECT x, 'r'||x FROM "
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

    /* ---- server-side paging: page 1 of 50 ---- */
    char paged_body[2048];
    snprintf(paged_body, sizeof(paged_body),
             "%s,\"sql\":\"SELECT * FROM pt;\",\"page_size\":\"50\","
             "\"page\":\"1\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", paged_body, resp,
                      sizeof(resp))) {
        check("POST /api/query paged status 200",
              response_status(resp) == 200, resp);
        const char *pb = response_body(resp);
        check("paged page1 ok", has(pb, "{\"ok\":true"), pb);
        check("paged page1 rows 50", has(pb, "\"rows\":50"), pb);
        check("paged page1 total 120", has(pb, "\"total\":120"), pb);
        check("paged page1 starts at row 1", has(pb, "\"data\":[[1,\"r1\""),
              pb);
        check("paged page1 ends at row 50", has(pb, "[50,\"r50\"]"), pb);
        check("paged page1 has no row 51", !has(pb, "[51,\"r51\"]"), pb);
        check("paged page1 total closes the object",
              has(pb, "\"total\":120}\n"), pb);
    } else {
        check("POST /api/query paged reachable", 0, "transport failure");
    }

    /* ---- page 3 of 50 -> rows 101..120 (rows=20) ---- */
    snprintf(paged_body, sizeof(paged_body),
             "%s,\"sql\":\"SELECT * FROM pt;\",\"page_size\":\"50\","
             "\"page\":\"3\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", paged_body, resp,
                      sizeof(resp))) {
        check("POST /api/query paged page3 status 200",
              response_status(resp) == 200, resp);
        const char *pb = response_body(resp);
        check("paged page3 rows 20", has(pb, "\"rows\":20"), pb);
        check("paged page3 total 120", has(pb, "\"total\":120"), pb);
        check("paged page3 starts at row 101", has(pb, "\"data\":[[101,\"r101\""),
              pb);
        check("paged page3 ends at row 120", has(pb, "[120,\"r120\"]"), pb);
    } else {
        check("POST /api/query paged page3 reachable", 0, "transport failure");
    }

    /* ---- page beyond the end: rows=0 but total intact ---- */
    snprintf(paged_body, sizeof(paged_body),
             "%s,\"sql\":\"SELECT * FROM pt;\",\"page_size\":\"200\","
             "\"page\":\"2\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", paged_body, resp,
                      sizeof(resp))) {
        check("POST /api/query paged beyond status 200",
              response_status(resp) == 200, resp);
        const char *pb = response_body(resp);
        check("paged beyond rows 0", has(pb, "\"rows\":0"), pb);
        check("paged beyond total 120", has(pb, "\"total\":120"), pb);
        check("paged beyond data empty", has(pb, "\"data\":[]"), pb);
    } else {
        check("POST /api/query paged beyond reachable", 0,
              "transport failure");
    }

    /* ---- explicit LIMIT in the SQL: NOT paged (guard rejects) ---- */
    snprintf(paged_body, sizeof(paged_body),
             "%s,\"sql\":\"SELECT * FROM pt LIMIT 5;\",\"page_size\":\"50\","
             "\"page\":\"1\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", paged_body, resp,
                      sizeof(resp))) {
        check("POST /api/query explicit-limit status 200",
              response_status(resp) == 200, resp);
        const char *pb = response_body(resp);
        check("explicit-limit rows 5", has(pb, "\"rows\":5"), pb);
        check("explicit-limit no total field", !has(pb, "\"total\":"), pb);
    } else {
        check("POST /api/query explicit-limit reachable", 0,
              "transport failure");
    }

    /* ---- non-plain shape with page_size: client-side paging kept ---- */
    snprintf(paged_body, sizeof(paged_body),
             "%s,\"sql\":\"SELECT id, v FROM pt WHERE id<=3;\","
             "\"page_size\":\"50\",\"page\":\"1\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", paged_body, resp,
                      sizeof(resp))) {
        check("POST /api/query non-plain status 200",
              response_status(resp) == 200, resp);
        const char *pb = response_body(resp);
        check("non-plain full rows 3", has(pb, "\"rows\":3"), pb);
        check("non-plain no total field", !has(pb, "\"total\":"), pb);
        check("non-plain full data", has(pb, "\"data\":[[1,\"r1\"],[2,\"r2\"],[3,\"r3\"]]"),
              pb);
    } else {
        check("POST /api/query non-plain reachable", 0, "transport failure");
    }

    /* ---- no page_size: exact v0.3 payload (no total) ---- */
    char select_body[2048];
    snprintf(select_body, sizeof(select_body),
             "%s,\"sql\":\"SELECT id, v FROM pt WHERE id<=2;\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", select_body, resp,
                      sizeof(resp))) {
        check("POST /api/query no-paging status 200",
              response_status(resp) == 200, resp);
        const char *expected =
            "{\"ok\":true,\"rows\":2,\"columns\":[\"id\",\"v\"],"
            "\"data\":[[1,\"r1\"],[2,\"r2\"]]}\n";
        check("no-paging exact v0.3 payload",
              strcmp(response_body(resp), expected) == 0, response_body(resp));
    } else {
        check("POST /api/query no-paging reachable", 0, "transport failure");
    }

    /* ---- COUNT(*) equivalence: total equals a direct COUNT ---- */
    char count_body[2048];
    snprintf(count_body, sizeof(count_body),
             "%s,\"sql\":\"SELECT COUNT(*) AS n FROM pt;\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", count_body, resp,
                      sizeof(resp))) {
        check("POST /api/query direct count status 200",
              response_status(resp) == 200, resp);
        check("direct count 120",
              has(response_body(resp), "\"data\":[[120]]"),
              response_body(resp));
    } else {
        check("POST /api/query direct count reachable", 0,
              "transport failure");
    }

    /* ---- /api/schema: byte-identical to v0.3 without with_sql ---- */
    char schema_body[2048];
    snprintf(schema_body, sizeof(schema_body), "%s}", target_json);
    if (http_exchange(port, "POST", "/api/schema", schema_body, resp,
                      sizeof(resp))) {
        check("POST /api/schema status 200", response_status(resp) == 200,
              resp);
        const char *sb = response_body(resp);
        check("schema v0.3 shape: no sql key",
              !has(sb, "\"sql\":"), sb);
        check("schema v0.3 shape: table present",
              has(sb, "{\"name\":\"pt\",\"type\":\"table\","
                      "\"columns\":[{\"name\":\"id\",\"type\":\"INTEGER\","
                      "\"pk\":1,\"notnull\":0}"), sb);
        check("schema v0.3 shape: exact object cap",
              has(sb, "\"indexes\":[]}]}\n"), sb);
    } else {
        check("POST /api/schema reachable", 0, "transport failure");
    }

    /* ---- /api/schema with_sql=1: verbatim stored CREATE ---- */
    snprintf(schema_body, sizeof(schema_body),
             "%s,\"with_sql\":\"1\"}", target_json);
    if (http_exchange(port, "POST", "/api/schema", schema_body, resp,
                      sizeof(resp))) {
        check("POST /api/schema with_sql status 200",
              response_status(resp) == 200, resp);
        const char *sb = response_body(resp);
        check("with_sql verbatim CREATE TABLE",
              has(sb, "\"sql\":\"CREATE TABLE pt(id INTEGER PRIMARY KEY, "
                      "v TEXT)\""), sb);
        check("with_sql placed after type",
              has(sb, "\"type\":\"table\",\"sql\":\"CREATE TABLE pt"),
              sb);
        check("with_sql no stray key",
              has(sb, "\"indexes\":[]}]}\n"), sb);
    } else {
        check("POST /api/schema with_sql reachable", 0, "transport failure");
    }

    /* ---- /api/schema with_sql=0: v0.3 shape kept (opt-in only) ---- */
    snprintf(schema_body, sizeof(schema_body),
             "%s,\"with_sql\":\"0\"}", target_json);
    if (http_exchange(port, "POST", "/api/schema", schema_body, resp,
                      sizeof(resp))) {
        check("POST /api/schema with_sql=0 status 200",
              response_status(resp) == 200, resp);
        check("with_sql=0 no sql key",
              !has(response_body(resp), "\"sql\":"), response_body(resp));
    } else {
        check("POST /api/schema with_sql=0 reachable", 0,
              "transport failure");
    }

    /* ---- /api/schema with_sql on :memory: -> empty tree ---- */
    if (http_exchange(port, "POST", "/api/schema",
                      "{\"target\":\":memory:\",\"with_sql\":\"1\"}", resp,
                      sizeof(resp))) {
        check("POST /api/schema :memory: with_sql empty tree",
              strcmp(response_body(resp), "{\"ok\":true,\"tables\":[]}\n") == 0,
              response_body(resp));
    } else {
        check("POST /api/schema :memory: with_sql reachable", 0,
              "transport failure");
    }

    /* ---- regressions: /api/query exact payload ---- */
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

    /* ---- regressions: /api/edit round-trip ---- */
    char edit_body[4096];
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"pt\",\"op\":\"insert\","
             "\"cols\":[\"id\",\"v\"],\"pk\":[\"id\"],"
             "\"old\":[],\"new\":[999,\"edit-row\"]}", target_json);
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
             "%s,\"table\":\"pt\",\"op\":\"update\","
             "\"cols\":[\"id\",\"v\"],\"pk\":[\"id\"],"
             "\"old\":[999,\"edit-row\"],\"new\":[999,\"edited\"]}",
             target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit update status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit update ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit update reachable", 0, "transport failure");
    }
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"pt\",\"op\":\"delete\","
             "\"cols\":[\"id\",\"v\"],\"pk\":[\"id\"],"
             "\"old\":[999,\"edited\"],\"new\":[]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit delete status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit delete ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit delete reachable", 0, "transport failure");
    }

    /* ---- regressions: /api/health :memory: deterministic contract ---- */
    if (http_exchange(port, "POST", "/api/health",
                      "{\"target\":\":memory:\"}", resp, sizeof(resp))) {
        check("POST /api/health :memory: status 200",
              response_status(resp) == 200, resp);
        const char *hb = response_body(resp);
        check("POST /api/health :memory: status error",
              has(hb, "\"status\":\"error\""), hb);
        check("POST /api/health :memory: lease extension present",
              has(hb, "\"lease\":{\"state\":\"absent\"}"), hb);
        check("POST /api/health :memory: last_error extension present",
              has(hb, "\"last_error\":\""), hb);
    } else {
        check("POST /api/health :memory: reachable", 0, "transport failure");
    }

    /* ---- regressions: GET /health, 404, 405 ---- */
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

    /* ---- live-S3 phase (SKIPPED without a store) ---- */
    const char *endpoint_env = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket_env = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint_env || !endpoint_env[0] || !bucket_env || !bucket_env[0]) {
        printf("SKIPPED (S3 phase): OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET not "
               "set (no live store)\n");
    } else {
#ifdef OTSARDB_ENABLE_S3
        char target[512];
        snprintf(target, sizeof(target), "s3://%s/studio-0086/db",
                 bucket_env);
        char s3_conn[800];
        snprintf(s3_conn, sizeof(s3_conn),
                 "{\"target\":\"%s\",\"endpoint\":\"%s\",\"region\":\"%s\","
                 "\"access_key\":\"%s\",\"secret_key\":\"%s\"",
                 target, endpoint_env,
                 getenv("OTSARDB_S3_REGION") ? getenv("OTSARDB_S3_REGION") : "",
                 getenv("OTSARDB_S3_ACCESS_KEY") ? getenv("OTSARDB_S3_ACCESS_KEY") : "",
                 getenv("OTSARDB_S3_SECRET_KEY") ? getenv("OTSARDB_S3_SECRET_KEY") : "");

        /* create a 60-row table on the live target */
        char s3_create[2000];
        snprintf(s3_create, sizeof(s3_create),
                 "%s,\"sql\":\"DROP TABLE IF EXISTS pt86; "
                 "CREATE TABLE pt86(id INTEGER PRIMARY KEY, v TEXT); "
                 "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 "
                 "FROM cnt WHERE x<60) INSERT INTO pt86(id,v) SELECT x, "
                 "'s'||x FROM cnt;\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_create, resp,
                          sizeof(resp))) {
            check("S3 create status 200", response_status(resp) == 200, resp);
            check("S3 create ok", has(response_body(resp), "{\"ok\":true"),
                  response_body(resp));
        } else {
            check("S3 create reachable", 0, "transport failure");
        }

        /* paged query: page 2 of 50 on 60 rows -> rows=10, total=60 */
        char s3_paged[1600];
        snprintf(s3_paged, sizeof(s3_paged),
                 "%s,\"sql\":\"SELECT * FROM pt86;\",\"page_size\":\"50\","
                 "\"page\":\"2\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_paged, resp,
                          sizeof(resp))) {
            check("S3 paged status 200", response_status(resp) == 200, resp);
            const char *pb = response_body(resp);
            check("S3 paged rows 10", has(pb, "\"rows\":10"), pb);
            check("S3 paged total 60", has(pb, "\"total\":60"), pb);
            check("S3 paged starts at row 51", has(pb, "[51,\"s51\""), pb);
        } else {
            check("S3 paged reachable", 0, "transport failure");
        }

        /* /api/schema with_sql on the live target */
        char s3_schema[1200];
        snprintf(s3_schema, sizeof(s3_schema),
                 "%s,\"with_sql\":\"1\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/schema", s3_schema, resp,
                          sizeof(resp))) {
            check("S3 schema with_sql status 200",
                  response_status(resp) == 200, resp);
            check("S3 schema with_sql verbatim",
                  has(response_body(resp),
                      "\"sql\":\"CREATE TABLE pt86(id INTEGER PRIMARY KEY, "
                      "v TEXT)\""),
                  response_body(resp));
        } else {
            check("S3 schema with_sql reachable", 0, "transport failure");
        }

        /* monitor regression on the live target */
        char s3_health[800];
        snprintf(s3_health, sizeof(s3_health), "%s}", s3_conn);
        if (http_exchange(port, "POST", "/api/health", s3_health, resp,
                          sizeof(resp))) {
            check("S3 health status 200", response_status(resp) == 200, resp);
            check("S3 health status ok",
                  has(response_body(resp), "\"status\":\"ok\""),
                  response_body(resp));
        } else {
            check("S3 health reachable", 0, "transport failure");
        }
#else
        printf("S3 phase SKIPPED: this binary was built without "
               "OTSARDB_ENABLE_S3\n");
#endif /* OTSARDB_ENABLE_S3 */
    }

    otsardb_studio_server_stop(server);
    cleanup_db(db);
    if (failures) {
        printf("studio-v04-0086 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio v0.4 wire contract (markers, server paging, "
           "with_sql schema, regressions)\n");
    return 0;
}

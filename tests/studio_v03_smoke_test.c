/* studio_v03_smoke_test.c — Studio server smoke test (v0.3).
 *
 * Starts the embedded studio server IN-PROCESS on an ephemeral loopback
 * port (otsardb_studio_server_start) and exercises the v0.3 wire contract
 * with raw sockets:
 * - GET / -> 200, HTML carries the v0.3 UI markers
 * (data-studio-release="v0.3", id="monitor", id="btn-monitor-refresh",
 * id="monitor-autorefresh", id="btn-admin-checkpoint", id="btn-admin-gc",
 * id="btn-admin-consent", id="btn-add-row", id="edit-bar") AND the v0.2
 * markers still present (data-studio-version="v0.2" — the 0073
 * contract is preserved, the page only adds the release marker)
 * - POST /api/health on :memory: -> 200 with the deterministic 0061
 * error contract (health is only defined for s3:// targets) plus the
 * lease/single_writer/last_error extension fields
 * - POST /api/edit end-to-end on a persistent file target: create the
 * table via /api/query, then insert/update/delete rows via /api/edit
 * and verify the committed rows via /api/query (exact payloads)
 * - POST /api/edit error cases: missing table, update without pk,
 * malformed body -> 400, unknown table -> SQL error object
 * - regressions: /api/query exact payload, /api/schema exact payload,
 * GET /health "ok"
 * - S3 phase (SKIPPED 77 without store env): /api/health on a live
 * target -> status ok + role + lease + single_writer + last_error;
 * admin request (auto_checkpoint + auto_checkpoint_commits + gc) ->
 * CHECKPOINT.json appears under the root (store-level proof the
 * per-request env window reached the closing session).
 *
 * Browser-level UI interactions (tabs, cell editing, monitor refresh)
 * remain a MANUAL test (documented in the test docs).
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

#ifdef OTSARDB_ENABLE_S3
#include "s3_object_store.h"
#include "s3_target.h"
#endif

#define RESP_CAP (96 * 1024)

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
    printf("studio-v03-0078 %-56s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

/* Temp file database target (persists between server requests). */
static void make_temp_db(char *buf, size_t cap) {
#if defined(_WIN32)
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(buf, cap, "%s\\otsardb-studio-v03-0078-%lu.db", dir,
             (unsigned long)getpid());
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-studio-v03-0078-%lu.db", dir,
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

#ifdef OTSARDB_ENABLE_S3
/* Store-level existence check: <root>/<suffix> via the S3 client built
 * from the ambient env (the same config the server's env window uses). */
static int s3_object_exists(const char *target, const char *suffix) {
    otsardb_s3_target parsed;
    if (!otsardb_s3_target_parse(target, &parsed)) return 0;
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *region = getenv("OTSARDB_S3_REGION");
    if (!region || !region[0]) region = "us-east-1";
    const char *access_key = getenv("OTSARDB_S3_ACCESS_KEY");
    const char *secret_key = getenv("OTSARDB_S3_SECRET_KEY");
    otsardb_s3_config config;
    memset(&config, 0, sizeof(config));
    config.endpoint = endpoint && endpoint[0] ? endpoint : "https://s3.amazonaws.com";
    config.region = region;
    config.bucket = parsed.bucket;
    config.access_key = access_key && access_key[0] ? access_key : NULL;
    config.secret_key = secret_key && secret_key[0] ? secret_key : NULL;
    config.use_https = 1;
    otsardb_object_store *store = otsardb_s3_store_create(&config);
    if (!store) return 0;
    char key[2048];
    snprintf(key, sizeof(key), "%s/%s", parsed.database_root, suffix);
    otsardb_store_object object;
    memset(&object, 0, sizeof(object));
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_OK) otsardb_store_release_object(store, &object);
    otsardb_s3_store_destroy(store);
    return rc == OTSARDB_STORE_OK;
}
#endif /* OTSARDB_ENABLE_S3 */

int main(void) {
    char error[256];
    /* (audit F08): the v0.3 workbench fixtures use a local
     * file path (studio-0078/db), so this harness explicitly opts into
     * local-file workbenching; the server default refuses local paths. */
    otsardb_studio_server *server =
        otsardb_studio_server_start_ex(0, 1, error, sizeof(error));
    if (!server) {
        printf("studio-v03-0078 server start FAIL: %s\n", error);
        return 1;
    }
    int port = otsardb_studio_server_port(server);
    printf("studio-v03-0078 server started on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("studio-v03-0078 FAIL: ephemeral port not reported\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    ensure_wsa();
    if (!fetch_csrf_token(port)) {
        printf("studio-v03-0078 FAIL: cannot fetch the CSRF token\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("CSRF token fetched", g_csrf[0] != '\0', g_csrf);

    char resp[RESP_CAP];

    /* ---- GET / : v0.3 UI markers (+ v0.2 markers preserved) ---- */
    if (!http_exchange(port, "GET", "/", NULL, resp, sizeof(resp))) {
        printf("studio-v03-0078 FAIL: cannot reach the server\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("GET / status 200", response_status(resp) == 200, resp);
    const char *body = response_body(resp);
    check("GET / body is HTML", has(body, "<!DOCTYPE html>") &&
                                has(body, "OtsarDB Studio"), body);
    check("GET / v0.2 version marker preserved",
          has(body, "data-studio-version=\"v0.2\""), body);
    check("GET / v0.3 release marker", has(body, "data-studio-release=\"v0.3\""),
          body);
    check("GET / monitor tab marker", has(body, "id=\"monitor-panel\""), body);
    check("GET / monitor refresh marker", has(body, "id=\"btn-monitor-refresh\""),
          body);
    check("GET / monitor autorefresh marker",
          has(body, "id=\"monitor-autorefresh\""), body);
    check("GET / admin checkpoint marker",
          has(body, "id=\"btn-admin-checkpoint\""), body);
    check("GET / admin gc marker", has(body, "id=\"btn-admin-gc\""), body);
    check("GET / admin consent marker", has(body, "id=\"btn-admin-consent\""),
          body);
    check("GET / add-row marker", has(body, "id=\"btn-add-row\""), body);
    check("GET / edit bar marker", has(body, "id=\"edit-bar\""), body);
    check("GET / edit hint marker", has(body, "id=\"edit-hint\""), body);

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

    /* ---- /api/health on :memory: -> deterministic 0061 error contract
     * (no store needed) ---- */
    if (http_exchange(port, "POST", "/api/health",
                      "{\"target\":\":memory:\"}", resp, sizeof(resp))) {
        check("POST /api/health :memory: status 200",
              response_status(resp) == 200, resp);
        const char *hb = response_body(resp);
        check("POST /api/health :memory: status error",
              has(hb, "\"status\":\"error\""), hb);
        check("POST /api/health :memory: role unknown",
              has(hb, "\"role\":\"unknown\""), hb);
        check("POST /api/health :memory: s3-only error message",
              has(hb, "only defined for s3://"), hb);
        check("POST /api/health :memory: lease extension present",
              has(hb, "\"lease\":{\"state\":\"absent\"}"), hb);
        check("POST /api/health :memory: single_writer extension present",
              has(hb, "\"single_writer\":false") || has(hb, "\"single_writer\":true"),
              hb);
        check("POST /api/health :memory: last_error extension present",
              has(hb, "\"last_error\":\""), hb);
    } else {
        check("POST /api/health :memory: reachable", 0, "transport failure");
    }

    /* ---- /api/health malformed body -> 400 ---- */
    if (http_exchange(port, "POST", "/api/health", "not json at all", resp,
                      sizeof(resp))) {
        check("POST /api/health malformed status 400",
              response_status(resp) == 400, resp);
    } else {
        check("POST /api/health malformed reachable", 0, "transport failure");
    }

    /* ---- /api/edit end-to-end on a persistent file target ---- */
    char create_body[2048];
    snprintf(create_body, sizeof(create_body),
             "%s,\"sql\":\"CREATE TABLE t3(id INTEGER PRIMARY KEY, name TEXT, "
             "age REAL);\"}", target_json);
    if (http_exchange(port, "POST", "/api/query", create_body, resp,
                      sizeof(resp))) {
        check("POST /api/query create table status 200",
              response_status(resp) == 200, resp);
        check("POST /api/query create table ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/query create table reachable", 0, "transport failure");
    }

    /* insert: (id=1, name='alice', age=30.5) */
    char edit_body[4096];
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"t3\",\"op\":\"insert\","
             "\"cols\":[\"id\",\"name\",\"age\"],\"pk\":[\"id\"],"
             "\"old\":[],\"new\":[1,\"alice\",30.5]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit insert status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit insert ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit insert reachable", 0, "transport failure");
    }

    /* update: SET name='O''Brien', age=31 WHERE id=1 (string quoting) */
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"t3\",\"op\":\"update\","
             "\"cols\":[\"id\",\"name\",\"age\"],\"pk\":[\"id\"],"
             "\"old\":[1,\"alice\",30.5],\"new\":[1,\"O'Brien\",31]}",
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

    /* insert a second row via NULL cells */
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"t3\",\"op\":\"insert\","
             "\"cols\":[\"id\",\"name\",\"age\"],\"pk\":[\"id\"],"
             "\"old\":[],\"new\":[2,null,1.5]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit insert nulls status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit insert nulls ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit insert nulls reachable", 0, "transport failure");
    }

    /* verify via /api/query: exact SELECT result */
    char select_body[2048];
    snprintf(select_body, sizeof(select_body),
             "%s,\"sql\":\"SELECT id, name, age FROM t3 ORDER BY id;\"}",
             target_json);
    if (http_exchange(port, "POST", "/api/query", select_body, resp,
                      sizeof(resp))) {
        check("POST /api/query after edits status 200",
              response_status(resp) == 200, resp);
        const char *expected_rows =
            "{\"ok\":true,\"rows\":2,\"columns\":[\"id\",\"name\",\"age\"],"
            "\"data\":[[1,\"O'Brien\",31.0],[2,null,1.5]]}\n";
        check("POST /api/query after edits exact payload",
              strcmp(response_body(resp), expected_rows) == 0,
              response_body(resp));
    } else {
        check("POST /api/query after edits reachable", 0, "transport failure");
    }

    /* delete row 2 */
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"t3\",\"op\":\"delete\","
             "\"cols\":[\"id\",\"name\",\"age\"],\"pk\":[\"id\"],"
             "\"old\":[2,null,1.5],\"new\":[]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit delete status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit delete ok",
              has(response_body(resp), "{\"ok\":true"), response_body(resp));
    } else {
        check("POST /api/edit delete reachable", 0, "transport failure");
    }
    if (http_exchange(port, "POST", "/api/query", select_body, resp,
                      sizeof(resp))) {
        const char *expected_after =
            "{\"ok\":true,\"rows\":1,\"columns\":[\"id\",\"name\",\"age\"],"
            "\"data\":[[1,\"O'Brien\",31.0]]}\n";
        check("POST /api/query after delete exact payload",
              strcmp(response_body(resp), expected_after) == 0,
              response_body(resp));
    } else {
        check("POST /api/query after delete reachable", 0, "transport failure");
    }

    /* ---- /api/edit error cases ---- */
    /* missing table */
    if (http_exchange(port, "POST", "/api/edit",
                      "{\"target\":\":memory:\",\"op\":\"update\","
                      "\"cols\":[\"a\"],\"pk\":[\"a\"],\"old\":[1],\"new\":[2]}",
                      resp, sizeof(resp))) {
        check("POST /api/edit missing table status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit missing table error object",
              has(response_body(resp), "\\\"table\\\"") &&
                  has(response_body(resp), "{\"ok\":false"),
              response_body(resp));
    } else {
        check("POST /api/edit missing table reachable", 0, "transport failure");
    }
    /* update without pk columns -> fail-closed generator error */
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"t3\",\"op\":\"update\","
             "\"cols\":[\"id\",\"name\",\"age\"],\"pk\":[],"
             "\"old\":[1,\"O'Brien\",31.0],\"new\":[1,\"x\",31.0]}",
             target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit no pk status 200", response_status(resp) == 200,
              resp);
        check("POST /api/edit no pk error object",
              has(response_body(resp), "primary-key"),
              response_body(resp));
    } else {
        check("POST /api/edit no pk reachable", 0, "transport failure");
    }
    /* malformed array -> 400 */
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"t3\",\"op\":\"update\",\"cols\":[1,2],"
             "\"pk\":[\"id\"],\"old\":[1],\"new\":[1]}", target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit malformed array status 400",
              response_status(resp) == 400, resp);
    } else {
        check("POST /api/edit malformed array reachable", 0,
              "transport failure");
    }
    /* unknown table -> SQL error object (statement executes, engine errs) */
    snprintf(edit_body, sizeof(edit_body),
             "%s,\"table\":\"nope_0078\",\"op\":\"insert\","
             "\"cols\":[\"id\"],\"pk\":[\"id\"],\"old\":[],\"new\":[1]}",
             target_json);
    if (http_exchange(port, "POST", "/api/edit", edit_body, resp,
                      sizeof(resp))) {
        check("POST /api/edit unknown table status 200",
              response_status(resp) == 200, resp);
        check("POST /api/edit unknown table error object",
              has(response_body(resp), "{\"ok\":false") &&
                  has(response_body(resp), "no such table"),
              response_body(resp));
    } else {
        check("POST /api/edit unknown table reachable", 0, "transport failure");
    }

    /* ---- regressions: v0.1/v0.2 exact payloads ---- */
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
    if (http_exchange(port, "POST", "/api/schema", "{\"target\":\":memory:\"}",
                      resp, sizeof(resp))) {
        check("POST /api/schema :memory: exact payload",
              strcmp(response_body(resp), "{\"ok\":true,\"tables\":[]}\n") == 0,
              response_body(resp));
    } else {
        check("POST /api/schema :memory: reachable", 0, "transport failure");
    }
    if (http_exchange(port, "GET", "/health", NULL, resp, sizeof(resp))) {
        check("GET /health regression status 200",
              response_status(resp) == 200, resp);
        check("GET /health regression body ok",
              strcmp(response_body(resp), "ok") == 0, response_body(resp));
    } else {
        check("GET /health regression reachable", 0, "transport failure");
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
        snprintf(target, sizeof(target), "s3://%s/studio-0078/db",
                 bucket_env);
        char s3_conn[800];
        snprintf(s3_conn, sizeof(s3_conn),
                 "{\"target\":\"%s\",\"endpoint\":\"%s\",\"region\":\"%s\","
                 "\"access_key\":\"%s\",\"secret_key\":\"%s\"",
                 target, endpoint_env,
                 getenv("OTSARDB_S3_REGION") ? getenv("OTSARDB_S3_REGION") : "",
                 getenv("OTSARDB_S3_ACCESS_KEY") ? getenv("OTSARDB_S3_ACCESS_KEY") : "",
                 getenv("OTSARDB_S3_SECRET_KEY") ? getenv("OTSARDB_S3_SECRET_KEY") : "");

        /* create the database through the studio server (write open) */
        char s3_create[1600];
        snprintf(s3_create, sizeof(s3_create),
                 "%s,\"sql\":\"DROP TABLE IF EXISTS m3; CREATE TABLE m3("
                 "id INTEGER PRIMARY KEY, v TEXT); INSERT INTO m3(v) "
                 "VALUES('s3v03');\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_create, resp,
                          sizeof(resp))) {
            check("S3 create status 200", response_status(resp) == 200, resp);
            check("S3 create ok", has(response_body(resp), "{\"ok\":true"),
                  response_body(resp));
        } else {
            check("S3 create reachable", 0, "transport failure");
        }

        /* health: monitor data via the health JSON */
        char s3_health[800];
        snprintf(s3_health, sizeof(s3_health), "%s}", s3_conn);
        if (http_exchange(port, "POST", "/api/health", s3_health, resp,
                          sizeof(resp))) {
            check("POST /api/health S3 status 200",
                  response_status(resp) == 200, resp);
            const char *hb = response_body(resp);
            check("POST /api/health S3 status ok", has(hb, "\"status\":\"ok\""),
                  hb);
            check("POST /api/health S3 role field",
                  has(hb, "\"role\":\"") , hb);
            check("POST /api/health S3 lease field",
                  has(hb, "\"lease\":{") && has(hb, "\"state\":\""), hb);
            check("POST /api/health S3 single_writer field",
                  has(hb, "\"single_writer\":"), hb);
            check("POST /api/health S3 last_error field",
                  has(hb, "\"last_error\":\""), hb);
            check("POST /api/health S3 open_ms field", has(hb, "\"open_ms\":"),
                  hb);
            check("POST /api/health S3 target field", has(hb, "\"target\":"),
                  hb);
        } else {
            check("POST /api/health S3 reachable", 0, "transport failure");
        }

        /* admin: checkpoint + GC in the per-request env window. The admin
         * request is a benign idempotent write (a marker table) because
         * the engine's close-path checkpoint only runs for sessions that
         * published a commit (s3_database.cpp destroy_session: head_loaded
         * + current_generation > 0; /0078). */
        char s3_admin[1200];
        snprintf(s3_admin, sizeof(s3_admin),
                 "%s,\"sql\":\"CREATE TABLE IF NOT EXISTS "
                 "__otsardb_studio_admin(id INTEGER PRIMARY KEY);\","
                 "\"auto_checkpoint\":\"1\","
                 "\"auto_checkpoint_commits\":\"1\",\"gc\":\"1\"}",
                 s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_admin, resp,
                          sizeof(resp))) {
            check("S3 admin request status 200",
                  response_status(resp) == 200, resp);
            check("S3 admin request ok", has(response_body(resp), "{\"ok\":true"),
                  response_body(resp));
        } else {
            check("S3 admin request reachable", 0, "transport failure");
        }
        /* store-level proof: the forced checkpoint published
         * CHECKPOINT.json under the root (per-request env window reached
         * the closing session's checkpoint path). */
        check("S3 CHECKPOINT.json exists after admin request",
              s3_object_exists(target, "CHECKPOINT.json"), target);
        /* the live set is intact: data still reads back */
        char s3_read[1200];
        snprintf(s3_read, sizeof(s3_read),
                 "%s,\"sql\":\"SELECT count(*) AS n FROM m3;\"}", s3_conn);
        if (http_exchange(port, "POST", "/api/query", s3_read, resp,
                          sizeof(resp))) {
            check("S3 data intact after admin request",
                  has(response_body(resp), "\"data\":[[1]]"),
                  response_body(resp));
        } else {
            check("S3 data intact reachable", 0, "transport failure");
        }
#else
        printf("S3 phase SKIPPED: this binary was built without "
               "OTSARDB_ENABLE_S3\n");
#endif /* OTSARDB_ENABLE_S3 */
    }

    otsardb_studio_server_stop(server);
    cleanup_db(db);
    if (failures) {
        printf("studio-v03-0078 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio v0.3 wire contract (UI markers, /api/health, "
           "/api/edit round-trip, admin env window, regressions)\n");
    return 0;
}

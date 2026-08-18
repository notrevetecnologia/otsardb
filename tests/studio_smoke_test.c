/* studio_smoke_test.c — OtsarDB Studio server smoke test.
 *
 * Starts the embedded studio server IN-PROCESS on an ephemeral loopback
 * port (otsardb_studio_server_start) and exercises the HTTP contract with
 * raw sockets:
 * - GET / -> 200, body is HTML (embedded single-page UI)
 * - GET /health -> 200, body "ok"
 * - POST /api/query with :memory: SELECT -> 200, exact --json payload
 * - POST /api/query with a failing SELECT -> 200, {"ok":false,...}
 * - POST /api/query with a malformed body -> 400, {"ok":false,...}
 * - POST /api/query missing "sql" -> 200, {"ok":false,...}
 * - GET /nope -> 404; POST /nope -> 404; DELETE / -> 405
 *
 * The UI interaction itself (form, grid, schema list) is a MANUAL test
 * (documented in the test docs); this test covers the wire contract the
 * UI depends on. Portable sockets (Winsock2/BSD); the server thread is
 * joined via otsardb_studio_server_stop before the test exits.
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
    printf("studio-0070 %-52s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

int main(void) {
    char error[256];
    otsardb_studio_server *server =
        otsardb_studio_server_start(0, error, sizeof(error));
    if (!server) {
        printf("studio-0070 server start FAIL: %s\n", error);
        return 1;
    }
    int port = otsardb_studio_server_port(server);
    printf("studio-0070 server started on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("studio-0070 FAIL: ephemeral port not reported\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    ensure_wsa();
    if (!fetch_csrf_token(port)) {
        printf("studio-0070 FAIL: cannot fetch the CSRF token\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("CSRF token fetched", g_csrf[0] != '\0', g_csrf);

    char resp[RESP_CAP];

    if (!http_exchange(port, "GET", "/", NULL, resp, sizeof(resp))) {
        printf("studio-0070 FAIL: cannot reach the server\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    check("GET / status 200", response_status(resp) == 200, resp);
    check("GET / body is HTML",
          has(response_body(resp), "<!DOCTYPE html>") &&
              has(response_body(resp), "OtsarDB Studio"),
          response_body(resp));

    if (http_exchange(port, "GET", "/health", NULL, resp, sizeof(resp))) {
        check("GET /health status 200", response_status(resp) == 200, resp);
        check("GET /health body ok", strcmp(response_body(resp), "ok") == 0,
              response_body(resp));
    } else {
        check("GET /health reachable", 0, "transport failure");
    }

    if (http_exchange(port, "POST", "/api/query",
                      "{\"target\":\":memory:\",\"sql\":\"SELECT 1 AS one, "
                      "'x' AS s;\"}",
                      resp, sizeof(resp))) {
        check("POST /api/query status 200", response_status(resp) == 200, resp);
        check("POST /api/query exact payload",
              strcmp(response_body(resp),
                     "{\"ok\":true,\"rows\":1,\"columns\":[\"one\",\"s\"],"
                     "\"data\":[[1,\"x\"]]}\n") == 0,
              response_body(resp));
    } else {
        check("POST /api/query reachable", 0, "transport failure");
    }

    if (http_exchange(port, "POST", "/api/query",
                      "{\"target\":\":memory:\",\"sql\":\"SELECT * FROM "
                      "nope_xyz;\"}",
                      resp, sizeof(resp))) {
        check("POST /api/query error object status 200",
              response_status(resp) == 200, resp);
        check("POST /api/query error object",
              has(response_body(resp), "{\"ok\":false,\"error\":") &&
                  has(response_body(resp), "no such table"),
              response_body(resp));
    } else {
        check("POST /api/query error reachable", 0, "transport failure");
    }

    if (http_exchange(port, "POST", "/api/query", "not json at all", resp,
                      sizeof(resp))) {
        check("POST /api/query malformed status 400",
              response_status(resp) == 400, resp);
        check("POST /api/query malformed error object",
              has(response_body(resp), "{\"ok\":false,\"error\":") &&
                  has(response_body(resp), "malformed"),
              response_body(resp));
    } else {
        check("POST /api/query malformed reachable", 0, "transport failure");
    }

    if (http_exchange(port, "POST", "/api/query", "{\"target\":\":memory:\"}",
                      resp, sizeof(resp))) {
        check("POST /api/query missing sql status 200",
              response_status(resp) == 200, resp);
        check("POST /api/query missing sql error",
              has(response_body(resp), "missing \\\"sql\\\"") &&
                  has(response_body(resp), "field"),
              response_body(resp));
    } else {
        check("POST /api/query missing sql reachable", 0, "transport failure");
    }

    if (http_exchange(port, "GET", "/nope", NULL, resp, sizeof(resp))) {
        check("GET /nope status 404", response_status(resp) == 404, resp);
    } else {
        check("GET /nope reachable", 0, "transport failure");
    }
    if (http_exchange(port, "POST", "/nope", "{}", resp, sizeof(resp))) {
        check("POST /nope status 404", response_status(resp) == 404, resp);
    } else {
        check("POST /nope reachable", 0, "transport failure");
    }
    if (http_exchange(port, "DELETE", "/", NULL, resp, sizeof(resp))) {
        check("DELETE / status 405", response_status(resp) == 405, resp);
    } else {
        check("DELETE / reachable", 0, "transport failure");
    }

    otsardb_studio_server_stop(server);
    if (failures) {
        printf("studio-0070 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio server wire contract (root, health, query, "
           "errors, 404/405, clean stop)\n");
    return 0;
}

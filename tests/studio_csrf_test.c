/* studio_csrf_test.c — regression test for the Studio CSRF
 * / loopback-origin gate (security audit F01).
 *
 * Starts the embedded studio server IN-PROCESS on an ephemeral loopback
 * port (otsardb_studio_server_start) and asserts the wire contract of the
 * gate, with raw sockets:
 * - GET / carries the injected per-process token
 * (window.OTSARDB_CSRF_TOKEN="<64 hex>") and GET /api/csrf returns it;
 * the two deliveries agree.
 * - POST /api/query without the X-OTSARDB-CSRF header -> 403
 * - POST with a wrong token value -> 403
 * - POST with a cross-origin Origin header -> 403
 * - POST with a wrong Host (rebinding) -> 403
 * - POST with Host missing the port -> 403
 * - POST without a Host header -> 403
 * - POST with a non-JSON Content-Type -> 415
 * - POST with the endpoint/keys override but NO token (the F01
 * signed-request delegation vector) -> 403
 * - POST with a valid token, Origin absent, application/json -> 200
 * (Origin-absent requests from non-browser clients stay allowed)
 * - POST with a valid token AND the endpoint override -> 200
 * (delegation only with a valid token)
 * - GET /health stays open without any token (GETs are not
 * state-changing and are not gated)
 *
 * The test is deterministic without S3 (the :memory: target and the
 * fail-closed core response for s3:// targets) and portable (Winsock2 /
 * BSD sockets), so it can run in both the core and the S3 ctest builds.
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

/* Raw HTTP exchange. host_header is the literal Host line (NULL = build
 * the correct "Host: 127.0.0.1:<port>" from the real port); extra_headers
 * is appended verbatim before the blank line (NULL = none); body may be
 * NULL (then Content-Length: 0). */
static int http_exchange(int port, const char *method, const char *path,
                         const char *host_header, const char *extra_headers,
                         const char *body, char *out, size_t cap) {
    test_socket fd = connect_tcp(port);
    if (fd == TEST_INVALID_SOCKET) return 0;
    char header[4096];
    char host_line[64];
    if (!host_header) {
        snprintf(host_line, sizeof(host_line), "Host: 127.0.0.1:%d", port);
        host_header = host_line;
    }
    int header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "%s\r\n"
                              "Content-Length: %zu\r\n"
                              "%s"
                              "Connection: close\r\n\r\n",
                              method, path, host_header,
                              body ? strlen(body) : 0,
                              extra_headers ? extra_headers : "");
    int ok = 0;
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        int sent_ok = 1;
        for (size_t sent = 0; sent < (size_t)header_len;) {
            int n = send(fd, header + sent, (int)((size_t)header_len - sent), 0);
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
            if (!wait_readable(fd, 5000)) break;
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

/* Copies the value of "key":"<value>" (or key="<value>") into out. */
static int extract_quoted(const char *haystack, const char *key, char *out,
                          size_t cap) {
    out[0] = '\0';
    if (!haystack || !key || !cap) return 0;
    const char *found = strstr(haystack, key);
    if (!found) return 0;
    const char *value_start = found + strlen(key);
    const char *close = strchr(value_start, '"');
    if (!close) return 0;
    size_t n = (size_t)(close - value_start);
    if (n == 0 || n >= cap) return 0;
    memcpy(out, value_start, n);
    out[n] = '\0';
    return 1;
}

static int is_hex64(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (int i = 0; i < 64; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("studio-csrf-0136 %-58s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

/* A fully stamped request: correct Host, valid token, application/json. */
static const char *stamped_headers(const char *token) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
             "X-OTSARDB-CSRF: %s\r\n"
             "Content-Type: application/json\r\n",
             token);
    return buf;
}

int main(void) {
    char error[256];
    otsardb_studio_server *server =
        otsardb_studio_server_start(0, error, sizeof(error));
    if (!server) {
        printf("studio-csrf-0136 server start FAIL: %s\n", error);
        return 1;
    }
    int port = otsardb_studio_server_port(server);
    printf("studio-csrf-0136 server started on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("studio-csrf-0136 FAIL: ephemeral port not reported\n");
        otsardb_studio_server_stop(server);
        return 1;
    }
    ensure_wsa();

    char resp[RESP_CAP];
    char token[TOKEN_CAP] = "";
    char page_token[TOKEN_CAP] = "";

    /* --- token delivery: GET / injection + GET /api/csrf agree --- */
    if (http_exchange(port, "GET", "/", NULL, NULL, NULL, resp, sizeof(resp))) {
        check("GET / status 200", response_status(resp) == 200, resp);
        check("GET / carries the injected token",
              extract_quoted(response_body(resp),
                             "window.OTSARDB_CSRF_TOKEN=\"", page_token,
                             sizeof(page_token)),
              response_body(resp));
        check("GET / token is 64 hex chars", is_hex64(page_token),
              page_token);
    } else {
        check("GET / reachable", 0, "transport failure");
    }

    if (http_exchange(port, "GET", "/api/csrf", NULL, NULL, NULL, resp,
                      sizeof(resp))) {
        check("GET /api/csrf status 200", response_status(resp) == 200, resp);
        check("GET /api/csrf token present",
              extract_quoted(response_body(resp), "\"token\":\"", token,
                             sizeof(token)),
              response_body(resp));
        check("GET /api/csrf token is 64 hex chars", is_hex64(token), token);
        check("page token and /api/csrf token agree",
              token[0] && strcmp(token, page_token) == 0, token);
    } else {
        check("GET /api/csrf reachable", 0, "transport failure");
    }

    const char *query_body =
        "{\"target\":\":memory:\",\"sql\":\"SELECT 1 AS one;\"}";

    /* --- F01 gate: token required --- */
    if (http_exchange(port, "POST", "/api/query", NULL, NULL, query_body, resp,
                      sizeof(resp))) {
        check("POST no token -> 403", response_status(resp) == 403, resp);
    } else {
        check("POST no token reachable", 0, "transport failure");
    }

    /* --- wrong token --- */
    char bad_headers[512];
    snprintf(bad_headers, sizeof(bad_headers),
             "X-OTSARDB-CSRF: %s\r\nContent-Type: application/json\r\n",
             "0000000000000000000000000000000000000000000000000000000000000000");
    if (http_exchange(port, "POST", "/api/query", NULL, bad_headers,
                      query_body, resp, sizeof(resp))) {
        check("POST wrong token -> 403", response_status(resp) == 403, resp);
    } else {
        check("POST wrong token reachable", 0, "transport failure");
    }

    /* --- cross-origin Origin -> 403 (valid token alone is not enough) --- */
    char origin_headers[512];
    snprintf(origin_headers, sizeof(origin_headers),
             "X-OTSARDB-CSRF: %s\r\n"
             "Content-Type: application/json\r\n"
             "Origin: http://evil.example\r\n",
             token);
    if (http_exchange(port, "POST", "/api/query", NULL, origin_headers,
                      query_body, resp, sizeof(resp))) {
        check("POST cross-origin Origin -> 403",
              response_status(resp) == 403, resp);
    } else {
        check("POST cross-origin Origin reachable", 0, "transport failure");
    }

    /* --- DNS-rebinding Host -> 403 --- */
    if (http_exchange(port, "POST", "/api/query", "Host: evil.example",
                      stamped_headers(token), query_body, resp,
                      sizeof(resp))) {
        check("POST rebinding Host -> 403", response_status(resp) == 403,
              resp);
    } else {
        check("POST rebinding Host reachable", 0, "transport failure");
    }

    /* --- Host without the port -> 403 --- */
    if (http_exchange(port, "POST", "/api/query", "Host: 127.0.0.1",
                      stamped_headers(token), query_body, resp,
                      sizeof(resp))) {
        check("POST Host without port -> 403", response_status(resp) == 403,
              resp);
    } else {
        check("POST Host without port reachable", 0, "transport failure");
    }

    /* --- no Host header at all -> 403 --- */
    if (http_exchange(port, "POST", "/api/query", "", stamped_headers(token),
                      query_body, resp, sizeof(resp))) {
        check("POST no Host -> 403", response_status(resp) == 403, resp);
    } else {
        check("POST no Host reachable", 0, "transport failure");
    }

    /* --- wrong Content-Type -> 415 --- */
    char text_headers[512];
    snprintf(text_headers, sizeof(text_headers),
             "X-OTSARDB-CSRF: %s\r\nContent-Type: text/plain\r\n", token);
    if (http_exchange(port, "POST", "/api/query", NULL, text_headers,
                      query_body, resp, sizeof(resp))) {
        check("POST text/plain Content-Type -> 415",
              response_status(resp) == 415, resp);
    } else {
        check("POST text/plain Content-Type reachable", 0,
              "transport failure");
    }

    /* --- endpoint/keys override WITHOUT a token -> 403 (F01 delegation) --- */
    const char *delegate_body =
        "{\"target\":\":memory:\",\"sql\":\"SELECT 1;\","
        "\"endpoint\":\"https://attacker.example\",\"bucket\":\"x\","
        "\"database\":\"y\"}";
    if (http_exchange(port, "POST", "/api/query", NULL, NULL, delegate_body,
                      resp, sizeof(resp))) {
        check("POST endpoint override no token -> 403",
              response_status(resp) == 403, resp);
    } else {
        check("POST endpoint override no token reachable", 0,
              "transport failure");
    }

    /* --- endpoint/keys override WITH a valid token -> processed (200) --- */
    if (http_exchange(port, "POST", "/api/query", NULL,
                      stamped_headers(token), delegate_body, resp,
                      sizeof(resp))) {
        check("POST endpoint override with token -> 200",
              response_status(resp) == 200, resp);
        check("POST endpoint override with token ran the query",
              has(response_body(resp), "{\"ok\":") ||
                  has(response_body(resp), "\"ok\":false"),
              response_body(resp));
    } else {
        check("POST endpoint override with token reachable", 0,
              "transport failure");
    }

    /* --- valid stamped request, Origin absent (CLI/curl clients) -> 200 --- */
    if (http_exchange(port, "POST", "/api/query", NULL,
                      stamped_headers(token), query_body, resp,
                      sizeof(resp))) {
        check("POST valid stamped status 200", response_status(resp) == 200,
              resp);
        check("POST valid stamped exact payload",
              strcmp(response_body(resp),
                     "{\"ok\":true,\"rows\":1,\"columns\":[\"one\"],"
                     "\"data\":[[1]]}\n") == 0,
              response_body(resp));
    } else {
        check("POST valid stamped reachable", 0, "transport failure");
    }

    /* --- valid stamped request with the same-origin Origin -> 200 --- */
    char same_origin_headers[512];
    snprintf(same_origin_headers, sizeof(same_origin_headers),
             "X-OTSARDB-CSRF: %s\r\n"
             "Content-Type: application/json\r\n"
             "Origin: http://127.0.0.1:%d\r\n",
             token, port);
    if (http_exchange(port, "POST", "/api/query", NULL, same_origin_headers,
                      query_body, resp, sizeof(resp))) {
        check("POST same-origin Origin status 200",
              response_status(resp) == 200, resp);
    } else {
        check("POST same-origin Origin reachable", 0, "transport failure");
    }

    /* --- GETs are not state-changing: /health stays open --- */
    if (http_exchange(port, "GET", "/health", NULL, NULL, NULL, resp,
                      sizeof(resp))) {
        check("GET /health open without token", response_status(resp) == 200,
              resp);
        check("GET /health body ok", strcmp(response_body(resp), "ok") == 0,
              response_body(resp));
    } else {
        check("GET /health reachable", 0, "transport failure");
    }

    otsardb_studio_server_stop(server);
    if (failures) {
        printf("studio-csrf-0136 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: Studio CSRF gate (token required, Origin/Host/Content-Type "
           "enforced, endpoint override gated, GETs open)\n");
    return 0;
}

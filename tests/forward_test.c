#include "forward.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

/* Loopback test of the M2 HTTP write-forwarding transport (
 * 0032): starts the embedded server in-process on an ephemeral port and
 * drives it through the forwarding client plus raw sockets.
 *
 * The handler mimics the leader-side contract of s3_database.cpp:
 * 200 with tab-separated rows + "# end N", 421 "not leader", 500 with the
 * engine message. It deliberately does NOT exercise the S3 session (that
 * path is covered by the live MinIO run). */

static int test_handler(void *ctx, const char *sql, size_t sql_len,
                        char *out, size_t cap) {
    (void)ctx;
    out[0] = '\0';
    size_t used = 0;
    if (sql_len >= 6 && memcmp(sql, "SELECT", 6) == 0) {
        if (sql_len > 6 && sql[6] == '!') {
            char *row[] = {"1", "two", NULL};
            otsardb_forward_row_line(out, cap, &used, 3, row);
            otsardb_forward_end_marker(out, cap, &used, 1);
        } else {
            otsardb_forward_end_marker(out, cap, &used, 0);
        }
        return 200;
    }
    if (sql_len >= 4 && memcmp(sql, "FAIL", 4) == 0) {
        snprintf(out, cap, "boom: syntax error");
        return 500;
    }
    snprintf(out, cap, "not leader");
    return 421;
}

static SOCKET raw_connect_port(int port) {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

/* Sends a raw HTTP request and reads the response until the peer closes. */
static void raw_roundtrip(int port, const char *request, char *response,
                          size_t cap) {
    SOCKET fd = raw_connect_port(port);
    assert(fd != INVALID_SOCKET);
    size_t request_len = strlen(request);
    size_t sent = 0;
    while (sent < request_len) {
        int n = send(fd, request + sent, (int)(request_len - sent), 0);
        assert(n > 0);
        sent += (size_t)n;
    }
    size_t used = 0;
    for (;;) {
        if (used + 1 >= cap) break;
        int n = recv(fd, response + used, (int)(cap - used - 1), 0);
        if (n <= 0) break;
        used += (size_t)n;
    }
    response[used] = '\0';
    closesocket(fd);
}

static void assert_contains(const char *haystack, const char *needle) {
    assert(strstr(haystack, needle) != NULL);
}

int main(void) {
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    /* --- start on an ephemeral port --- */
    char error[256] = {0};
    otsardb_forward_server *server =
        otsardb_forward_server_start("127.0.0.1", 0, test_handler, NULL,
                                   error, sizeof(error));
    assert(server != NULL);
    int port = otsardb_forward_server_port(server);
    assert(port > 0 && port <= 65535);
    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%d", port);

    char response[8192];
    int status = 0;

    /* --- 200 with rows: tab-separated, NULL rendered "NULL", marker --- */
    assert(otsardb_forward_client_post(target, "SELECT!", response,
                                     sizeof(response), &status, 5000) == 0);
    assert(status == 200);
    assert(strcmp(response, "1\ttwo\tNULL\n# end 1\n") == 0);

    /* --- 200 with zero rows: just the marker --- */
    assert(otsardb_forward_client_post(target, "SELECT", response,
                                     sizeof(response), &status, 5000) == 0);
    assert(status == 200);
    assert(strcmp(response, "# end 0\n") == 0);

    /* --- 500: engine error message in the body --- */
    assert(otsardb_forward_client_post(target, "FAIL", response,
                                     sizeof(response), &status, 5000) == 0);
    assert(status == 500);
    assert(strcmp(response, "boom: syntax error") == 0);

    /* --- 421: "not leader" --- */
    assert(otsardb_forward_client_post(target, "INSERT INTO t VALUES(1)", response,
                                     sizeof(response), &status, 5000) == 0);
    assert(status == 421);
    assert(strcmp(response, "not leader") == 0);

    /* --- raw-socket protocol checks --- */
    char raw[8192];
    raw_roundtrip(port,
                  "GET /forward HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                  raw, sizeof(raw));
    assert_contains(raw, "HTTP/1.1 405");
    raw_roundtrip(port,
                  "POST /nope HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
                  "Connection: close\r\n\r\nx",
                  raw, sizeof(raw));
    assert_contains(raw, "HTTP/1.1 404");
    raw_roundtrip(port,
                  "POST /forward HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                  raw, sizeof(raw));
    assert_contains(raw, "HTTP/1.1 400");
    raw_roundtrip(port,
                  "POST /forward HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n"
                  "Connection: close\r\n\r\nSELECT!",
                  raw, sizeof(raw));
    assert_contains(raw, "HTTP/1.1 200");
    assert_contains(raw, "1\ttwo\tNULL\n# end 1\n");

    /* --- transport failure: connect to a closed port --- */
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(probe != INVALID_SOCKET);
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(bind(probe, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0);
    struct sockaddr_in bound;
    int bound_len = sizeof(bound);
    assert(getsockname(probe, (struct sockaddr *)&bound, &bound_len) == 0);
    closesocket(probe); /* free the port; a race here is astronomically rare */
    char dead[64];
    snprintf(dead, sizeof(dead), "127.0.0.1:%d", ntohs(bound.sin_port));
    status = -1;
    assert(otsardb_forward_client_post(dead, "SELECT!", response,
                                     sizeof(response), &status, 5000) == -1);
    assert(status == 0);

    /* --- host:port parsing --- */
    char host[128];
    int parsed_port = 0;
    assert(otsardb_forward_split_host_port("127.0.0.1:18080", host, sizeof(host),
                                         &parsed_port) == 1);
    assert(strcmp(host, "127.0.0.1") == 0);
    assert(parsed_port == 18080);
    assert(otsardb_forward_split_host_port("no-port", host, sizeof(host),
                                         &parsed_port) == 0);
    assert(otsardb_forward_split_host_port(":5", host, sizeof(host),
                                         &parsed_port) == 0);
    assert(otsardb_forward_split_host_port("h:99999", host, sizeof(host),
                                         &parsed_port) == 0);
    assert(otsardb_forward_split_host_port("h:abc", host, sizeof(host),
                                         &parsed_port) == 0);

    /* --- stop and join cleanly --- */
    otsardb_forward_server_stop(server);

    WSACleanup();
    puts("forward server/client loopback semantics passed");
    return 0;
}

#else /* !_WIN32 */

int main(void) {
    /* The loopback test needs Winsock; on POSIX it is a no-op so CTest
     * passes everywhere. The transport code itself is POSIX-ported. */
    puts("forward loopback test skipped (Windows-only Winsock test)");
    return 0;
}

#endif

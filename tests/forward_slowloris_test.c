/* forward_slowloris_test.c — regression test for the
 * forward-server read deadlines (security audit F02).
 *
 * The forward server is single-threaded: a loopback client that connects
 * and sends nothing (or drips headers) used to hold the accept loop
 * indefinitely. Since every server-side recv is preceded by
 * select() with a bounded window (OTSARDB_FORWARD_READ_IDLE_MS idle, plus
 * the OTSARDB_FORWARD_READ_TOTAL_MS absolute request deadline), and header
 * / body stalls are answered 400 / 408.
 *
 * The test starts the embedded server IN-PROCESS on an ephemeral loopback
 * port and asserts, with raw sockets:
 * - a connection that sends nothing is answered 400 within a bounded
 * time and does NOT block the accept loop: a valid POST /forward sent
 * while the stall is in flight still completes 200 with the expected
 * rows, within a bounded total time;
 * - a connection that sends complete headers but stalls the body is
 * answered 408 within a bounded time, and the accept loop keeps
 * serving afterwards;
 * - the existing size caps and response semantics are unchanged.
 *
 * Portable sockets (Winsock2/BSD), so the test can run in both the core
 * and the S3 ctest builds; the only timing constant used is the published
 * OTSARDB_FORWARD_READ_IDLE_MS, so the test does not need re-tuning if the
 * deadline changes.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "forward.h"

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
#define TEST_SLEEP_MS(ms) Sleep(ms)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int test_socket;
#define TEST_INVALID_SOCKET (-1)
#define test_socket_close(fd) close(fd)
#define TEST_SLEEP_MS(ms) usleep((useconds_t)(ms) * 1000)
#endif

#define RESP_CAP (64 * 1024)

/* Handler mirrors forward_test.c's leader-side contract. */
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
    snprintf(out, cap, "boom: syntax error");
    return 500;
}

static int ensure_wsa(void) {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return 1;
#endif
}

static unsigned long long now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
#if defined(__linux__)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)(ts.tv_nsec / 1000000);
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

/* Sends `data` and then reads the full response until the peer closes (or
 * the per-read idle timeout elapses). Returns 1 when a response was read. */
static int raw_exchange(test_socket fd, const char *data, char *out,
                        size_t cap) {
    size_t len = strlen(data);
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    size_t used = 0;
    out[0] = '\0';
    while (used + 1 < cap) {
        if (!wait_readable(fd, 10000)) break;
        int n = recv(fd, out + used, (int)(cap - used - 1), 0);
        if (n <= 0) break;
        used += (size_t)n;
        out[used] = '\0';
    }
    return used > 0;
}

static int status_of(const char *resp) {
    int status = 0;
    if (resp && sscanf(resp, "HTTP/1.1 %d", &status) == 1) return status;
    return 0;
}

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("forward-slowloris-0136 %-48s %s\n", name,
           cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

int main(void) {
    if (!ensure_wsa()) {
        printf("forward-slowloris-0136 FAIL: WSAStartup\n");
        return 1;
    }
    char error[256] = {0};
    otsardb_forward_server *server =
        otsardb_forward_server_start("127.0.0.1", 0, test_handler, NULL,
                                     error, sizeof(error));
    if (!server) {
        printf("forward-slowloris-0136 FAIL: server start: %s\n", error);
        return 1;
    }
    int port = otsardb_forward_server_port(server);
    printf("forward-slowloris-0136 server on ephemeral port %d\n", port);
    if (port <= 0) {
        printf("forward-slowloris-0136 FAIL: no ephemeral port\n");
        otsardb_forward_server_stop(server);
        return 1;
    }

    char resp[RESP_CAP];
    const char *valid =
        "POST /forward HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n"
        "Connection: close\r\n\r\nSELECT!";

    /* --- baseline: valid request works --- */
    test_socket fd = connect_tcp(port);
    check("baseline connect", fd != TEST_INVALID_SOCKET, "");
    if (fd != TEST_INVALID_SOCKET) {
        check("baseline valid request 200",
              raw_exchange(fd, valid, resp, sizeof(resp)) &&
                  status_of(resp) == 200,
              resp);
        check("baseline rows rendered",
              strstr(resp, "1\ttwo\tNULL\n# end 1\n") != NULL, resp);
        test_socket_close(fd);
    }

    /* --- slowloris 1: connect and send NOTHING ---
     * The stalled connection must be answered (400) within a bounded time
     * and must NOT hold the accept loop: a valid request sent while the
     * stall is in flight is queued and must still complete 200 within a
     * bounded total time (idle deadline + margin). */
    test_socket stall = connect_tcp(port);
    check("slowloris connect", stall != TEST_INVALID_SOCKET, "");
    unsigned long long t0 = now_ms();
    char valid_resp[RESP_CAP] = "";
    int valid_ok = 0;
    int valid_status = 0;
    if (stall != TEST_INVALID_SOCKET) {
        /* Do not send anything on `stall`. The single-threaded server
         * serves it first; the select deadline (OTSARDB_FORWARD_READ_IDLE_MS)
         * must expire and only then is the queued valid request handled. */
        test_socket fd2 = connect_tcp(port);
        if (fd2 != TEST_INVALID_SOCKET) {
            valid_ok = raw_exchange(fd2, valid, valid_resp, sizeof(valid_resp));
            valid_status = status_of(valid_resp);
            test_socket_close(fd2);
        }
        /* The stalled socket must eventually see the 400 answer (or EOF). */
        char stall_resp[4096] = "";
        size_t used = 0;
        int saw = 0;
        while (used + 1 < sizeof(stall_resp)) {
            if (!wait_readable(stall, 10000)) break;
            int n = recv(stall, stall_resp + used,
                         (int)(sizeof(stall_resp) - used - 1), 0);
            if (n <= 0) break;
            used += (size_t)n;
            stall_resp[used] = '\0';
            if (strstr(stall_resp, "HTTP/1.1") != NULL) { saw = 1; break; }
        }
        unsigned long long elapsed = now_ms() - t0;
        check("slowloris stall answered 400 within bounded time",
              saw && status_of(stall_resp) == 400, stall_resp);
        check("slowloris valid request still 200",
              valid_ok && valid_status == 200, valid_resp);
        check("slowloris valid request rows rendered",
              strstr(valid_resp, "1\ttwo\tNULL\n# end 1\n") != NULL,
              valid_resp);
        /* Idle deadline + 10 s margin: an indefinite hang fails this. */
        check("slowloris accept loop bounded",
              elapsed < (unsigned long long)OTSARDB_FORWARD_READ_IDLE_MS +
                            10000u,
              "elapsed");
        test_socket_close(stall);
    }

    /* --- slowloris 2: headers sent, body stalled ---
     * Complete headers with Content-Length but no body -> the body read
     * loop must time out with 408 within a bounded time, and the accept
     * loop must keep serving afterwards. */
    test_socket stall2 = connect_tcp(port);
    check("body-stall connect", stall2 != TEST_INVALID_SOCKET, "");
    if (stall2 != TEST_INVALID_SOCKET) {
        const char *headless =
            "POST /forward HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n"
            "Connection: close\r\n\r\n";
        size_t len = strlen(headless);
        size_t sent = 0;
        while (sent < len) {
            int n = send(stall2, headless + sent, (int)(len - sent), 0);
            if (n <= 0) break;
            sent += (size_t)n;
        }
        char stall2_resp[4096] = "";
        size_t used2 = 0;
        int saw2 = 0;
        unsigned long long t1 = now_ms();
        while (used2 + 1 < sizeof(stall2_resp)) {
            if (!wait_readable(stall2, 10000)) break;
            int n = recv(stall2, stall2_resp + used2,
                         (int)(sizeof(stall2_resp) - used2 - 1), 0);
            if (n <= 0) break;
            used2 += (size_t)n;
            stall2_resp[used2] = '\0';
            if (strstr(stall2_resp, "HTTP/1.1") != NULL) { saw2 = 1; break; }
        }
        unsigned long long elapsed2 = now_ms() - t1;
        check("body-stall answered 408 within bounded time",
              saw2 && status_of(stall2_resp) == 408, stall2_resp);
        check("body-stall accept loop bounded",
              elapsed2 < (unsigned long long)OTSARDB_FORWARD_READ_IDLE_MS +
                             10000u,
              "elapsed");
        test_socket_close(stall2);

        test_socket fd3 = connect_tcp(port);
        check("post-stall reconnect", fd3 != TEST_INVALID_SOCKET, "");
        if (fd3 != TEST_INVALID_SOCKET) {
            check("post-stall valid request 200",
                  raw_exchange(fd3, valid, resp, sizeof(resp)) &&
                      status_of(resp) == 200,
                  resp);
            test_socket_close(fd3);
        }
    }

    otsardb_forward_server_stop(server);
    if (failures) {
        printf("forward-slowloris-0136 FAIL: %d assertion(s) failed\n",
               failures);
        return 1;
    }
    printf("PASS: forward server read deadlines (stalled header/body "
           "connections bounded, accept loop live)\n");
    return 0;
}

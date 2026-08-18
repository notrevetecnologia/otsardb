#include "forward.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
typedef SOCKET otsardb_socket;
#define OTSARDB_INVALID_SOCKET INVALID_SOCKET
#define otsardb_socket_close(fd) closesocket(fd)
#define OTSARDB_LAST_SOCKET_ERROR() ((int)WSAGetLastError())
typedef int otsardb_socklen;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int otsardb_socket;
#define OTSARDB_INVALID_SOCKET (-1)
#define otsardb_socket_close(fd) close(fd)
#define OTSARDB_LAST_SOCKET_ERROR() (errno)
typedef socklen_t otsardb_socklen;
#endif

/* minimal embedded HTTP/1.1 server + forwarding client.
 * Protocol spec is in forward.h. One connection at a time, Content-Length
 * bodies, no keep-alive, no TLS; loopback-bound by the leader session.
 *
 * Threading: the server owns one accept-loop thread per instance. The
 * stop flag is a plain volatile int written by otsardb_forward_server_stop
 * and read by the loop between select() ticks (same pattern as the CLI
 * interrupt flag); the loop also joins the thread, so no torn reads
 * survive the join. */

static int ensure_wsa(void) {
#if defined(_WIN32)
    static volatile long started = 0;
    if (!started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
        started = 1;
    }
#endif
    return 1;
}

struct otsardb_forward_server {
    otsardb_socket listen_fd;
    int port;
    volatile int stop;
    void *ctx;
    otsardb_forward_handler handler;
#if defined(_WIN32)
    uintptr_t thread;
#else
    pthread_t thread;
    int thread_running;
#endif
};

/* ------------------------------------------------------------------ util */

/* Portable memmem: locate `needle` inside `haystack`. */
static const char *find_bytes(const char *haystack,
                              size_t haystack_len,
                              const char *needle,
                              size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (needle_len > haystack_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

/* Wait up to timeout_ms for `fd` to become readable. 1 = readable,
 * 0 = timeout/error. */
static int wait_readable(otsardb_socket fd, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select((int)fd + 1, &read_fds, NULL, NULL, &tv);
    return ready > 0 ? 1 : 0;
}

/* Monotonic wall-clock ms (same pattern as studio.c's studio_now_ms). */
static unsigned long long forward_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

/* Server-side read deadline (audit F02): the per-call idle
 * window bounds a connection that sends nothing; the absolute total
 * deadline (measured from the connection start) bounds a connection that
 * drips bytes indefinitely, so a slowloris client can never hold the
 * single-threaded accept loop. Returns the ms budget for the next
 * wait_readable call (0 = the deadline already elapsed). */
static int read_deadline_remaining(unsigned long long start_ms) {
    unsigned long long elapsed = forward_now_ms() - start_ms;
    if (elapsed >= OTSARDB_FORWARD_READ_TOTAL_MS) return 0;
    unsigned long long left = OTSARDB_FORWARD_READ_TOTAL_MS - elapsed;
    if (left >= (unsigned long long)OTSARDB_FORWARD_READ_IDLE_MS) {
        return OTSARDB_FORWARD_READ_IDLE_MS;
    }
    return (int)left;
}

static int send_all(otsardb_socket fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int set_socket_timeouts(otsardb_socket fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    /* SO_SNDTIMEO bounds connect()/send(); the READ path uses select()
     * with a deadline instead of SO_RCVTIMEO, which proved unreliable
     * with a blocking recv on loopback (spurious WSAETIMEDOUT even with
     * data already buffered — observed repeatedly in live forwarding). */
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv)) != 0) {
        return 0;
    }
#else
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return 0;
    }
#endif
    return 1;
}

/* ----------------------------------------------------------------- server */

static void send_simple_response(otsardb_socket fd,
                                 int status,
                                 const char *status_text,
                                 const char *body) {
    char header[512];
    int header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              status,
                              status_text,
                              body ? strlen(body) : 0);
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        send_all(fd, header, (size_t)header_len);
    }
    if (body) send_all(fd, body, strlen(body));
}

/* Reads until the header terminator "\r\n\r\n", then returns the body
 * offset. 0 = failed (timeout, closed, oversize). Every recv is preceded
 * by a select() with the read deadline (audit F02). */
static int read_request_headers(otsardb_socket fd,
                                char *buffer,
                                size_t cap,
                                size_t *body_offset,
                                size_t *used_out,
                                unsigned long long start_ms) {
    size_t used = 0;
    for (;;) {
        if (used + 1 >= cap) return 0; /* keep one byte for the NUL */
        int ms = read_deadline_remaining(start_ms);
        if (ms <= 0 || !wait_readable(fd, ms)) return 0;
        int n = recv(fd, buffer + used, (int)(cap - used - 1), 0);
        if (n <= 0) return 0;
        used += (size_t)n;
        buffer[used] = '\0';
        if (used >= 4 && find_bytes(buffer, used, "\r\n\r\n", 4)) {
            size_t header_end = 0;
            for (size_t i = 0; i + 4 <= used; ++i) {
                if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
                    header_end = i + 4;
                    break;
                }
            }
            *body_offset = header_end;
            *used_out = used;
            return 1;
        }
    }
}

static int parse_request_line(char *line, char *method, size_t method_cap,
                              char *path, size_t path_cap) {
    char *sp1 = strchr(line, ' ');
    if (!sp1) return 0;
    *sp1 = '\0';
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return 0;
    *sp2 = '\0';
    if (strlen(line) >= method_cap || strlen(sp1 + 1) >= path_cap) return 0;
    snprintf(method, method_cap, "%s", line);
    snprintf(path, path_cap, "%s", sp1 + 1);
    return 1;
}

static long parse_content_length(const char *headers) {
    const char *found = strstr(headers, "Content-Length:");
    if (!found) return -1; /* absent */
    found += strlen("Content-Length:");
    while (*found == ' ' || *found == '\t') ++found;
    char *end = NULL;
    long parsed = strtol(found, &end, 10);
    if (end == found || parsed < 0) return -1;
    return parsed;
}

/* One request on one connection. The connection is always closed. */
static void handle_connection(otsardb_forward_server *server, otsardb_socket fd) {
    /* Request deadline (audit F02): measured from the
     * moment the connection is accepted, covering headers AND body, so a
     * connection that connects and sends nothing (or drips bytes) cannot
     * hold the single-threaded accept loop indefinitely. */
    unsigned long long start_ms = forward_now_ms();
    /* Buffers are heap-allocated: together they exceed the default 1 MiB
     * Windows thread stack. */
    size_t request_cap = OTSARDB_FORWARD_MAX_HEADERS + OTSARDB_FORWARD_MAX_BODY + 1;
    char *request = (char *)malloc(request_cap);
    char *response = (char *)malloc(OTSARDB_FORWARD_MAX_BODY + 1);
    if (!request || !response) {
        send_simple_response(fd, 500, "Internal Server Error", "out of memory");
        free(request);
        free(response);
        return;
    }
    size_t body_offset = 0;
    size_t used = 0;
    if (!read_request_headers(fd, request, OTSARDB_FORWARD_MAX_HEADERS, &body_offset, &used, start_ms)) {
        send_simple_response(fd, 400, "Bad Request", "bad request: malformed headers (read deadline exceeded)");
        free(request);
        free(response);
        return;
    }

    /* Make the header zone a clean string; body bytes (pipelined) follow
     * at body_offset and are re-read below. The byte at body_offset may
     * already be the first body byte — save and restore it. */
    char saved_body_byte = request[body_offset];
    request[body_offset] = '\0';

    char method[16] = {0};
    char path[128] = {0};
    if (!parse_request_line(request, method, sizeof(method), path, sizeof(path))) {
        send_simple_response(fd, 400, "Bad Request", "bad request: unparsable request line");
        free(request);
        free(response);
        return;
    }

    /* Search the headers AFTER the request line: parse_request_line
     * NUL-terminates the request line in place, so strstr over the whole
     * buffer would stop at the first embedded NUL. */
    const char *header_zone = find_bytes(request, body_offset, "\r\n", 2);
    header_zone = header_zone ? header_zone + 2 : request;

    if (strcmp(method, "POST") != 0) {
        send_simple_response(fd, 405, "Method Not Allowed", "method not allowed");
        free(request);
        free(response);
        return;
    }
    if (strcmp(path, "/forward") != 0) {
        send_simple_response(fd, 404, "Not Found", "not found");
        free(request);
        free(response);
        return;
    }

    long content_length = parse_content_length(header_zone);
    if (content_length < 0) {
        send_simple_response(fd, 400, "Bad Request", "bad request: missing or invalid Content-Length");
        free(request);
        free(response);
        return;
    }
    if ((unsigned long)content_length > OTSARDB_FORWARD_MAX_BODY) {
        send_simple_response(fd, 400, "Bad Request", "bad request: body exceeds the 1 MiB cap");
        free(request);
        free(response);
        return;
    }
    request[body_offset] = saved_body_byte;

    /* Read the body (may already be partially in the buffer). Every recv
     * is bounded by the read deadline (audit F02): a
     * client that sends headers but stalls the body gets 408 instead of
     * holding the accept loop. */
    size_t have = used > body_offset ? used - body_offset : 0;
    if (have > (size_t)content_length) {
        /* pipelined extra bytes are ignored (no keep-alive) */
        have = (size_t)content_length;
    }
    size_t remaining = (size_t)content_length - have;
    while (remaining > 0) {
        int ms = read_deadline_remaining(start_ms);
        if (ms <= 0 || !wait_readable(fd, ms)) {
            send_simple_response(fd, 408, "Request Timeout",
                                 "request timeout: body was not received");
            free(request);
            free(response);
            return;
        }
        int n = recv(fd, request + body_offset + have, (int)remaining, 0);
        if (n <= 0) {
            send_simple_response(fd, 400, "Bad Request", "bad request: truncated body");
            free(request);
            free(response);
            return;
        }
        remaining -= (size_t)n;
        have += (size_t)n;
    }
    request[body_offset + (size_t)content_length] = '\0';

    response[0] = '\0';
    int status = server->handler(server->ctx,
                                 request + body_offset,
                                 (size_t)content_length,
                                 response,
                                 OTSARDB_FORWARD_MAX_BODY + 1);
    if (status == 200) {
        send_simple_response(fd, 200, "OK", response);
    } else if (status == 421) {
        send_simple_response(fd, 421, "Not Leader", "not leader");
    } else {
        send_simple_response(fd, 500, "Internal Server Error", response);
    }
    free(request);
    free(response);
}

#if defined(_WIN32)
static unsigned int __stdcall forward_server_main(void *arg) {
#else
static void *forward_server_main(void *arg) {
#endif
    otsardb_forward_server *server = (otsardb_forward_server *)arg;
    while (!server->stop) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server->listen_fd, &read_fds);
        struct timeval poll_timeout;
        poll_timeout.tv_sec = 0;
        poll_timeout.tv_usec = 200000; /* 200 ms: bounds the stop latency */
        int ready = select((int)server->listen_fd + 1, &read_fds, NULL, NULL, &poll_timeout);
        if (ready <= 0 || server->stop) continue;
        otsardb_socket conn = accept(server->listen_fd, NULL, NULL);
        if (conn == OTSARDB_INVALID_SOCKET) {
            continue;
        }
        handle_connection(server, conn);
        otsardb_socket_close(conn);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

otsardb_forward_server *otsardb_forward_server_start(const char *bind_host,
                                                 int port,
                                                 otsardb_forward_handler handler,
                                                 void *ctx,
                                                 char *error,
                                                 size_t error_cap) {
    if (error && error_cap) error[0] = '\0';
    if (!handler || port < 0 || port > 65535) {
        if (error && error_cap) snprintf(error, error_cap, "invalid arguments");
        return NULL;
    }
    if (!ensure_wsa()) {
        if (error && error_cap) snprintf(error, error_cap, "socket initialization failed");
        return NULL;
    }

    otsardb_forward_server *server = (otsardb_forward_server *)calloc(1, sizeof(*server));
    if (!server) {
        if (error && error_cap) snprintf(error, error_cap, "out of memory");
        return NULL;
    }
    server->handler = handler;
    server->ctx = ctx;
    server->listen_fd = OTSARDB_INVALID_SOCKET;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(bind_host, port_str, &hints, &resolved) != 0 || !resolved) {
        if (error && error_cap) snprintf(error, error_cap, "cannot resolve bind address %s", bind_host ? bind_host : "*");
        free(server);
        return NULL;
    }

    otsardb_socket listen_fd = OTSARDB_INVALID_SOCKET;
    for (struct addrinfo *ai = resolved; ai; ai = ai->ai_next) {
        listen_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd == OTSARDB_INVALID_SOCKET) continue;
#if !defined(_WIN32)
        int reuse = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (bind(listen_fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        otsardb_socket_close(listen_fd);
        listen_fd = OTSARDB_INVALID_SOCKET;
    }
    freeaddrinfo(resolved);
    if (listen_fd == OTSARDB_INVALID_SOCKET) {
        if (error && error_cap) snprintf(error, error_cap, "cannot bind %s:%d (in use?)", bind_host ? bind_host : "*", port);
        free(server);
        return NULL;
    }
    if (listen(listen_fd, 4) != 0) {
        if (error && error_cap) snprintf(error, error_cap, "listen failed (socket error %d)", OTSARDB_LAST_SOCKET_ERROR());
        otsardb_socket_close(listen_fd);
        free(server);
        return NULL;
    }

    server->listen_fd = listen_fd;
    server->port = port;
    if (port == 0) {
        struct sockaddr_storage bound;
        otsardb_socklen bound_len = (otsardb_socklen)sizeof(bound);
        if (getsockname(listen_fd, (struct sockaddr *)&bound, &bound_len) == 0) {
            server->port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
        }
    }

#if defined(_WIN32)
    server->thread = _beginthreadex(NULL, 0, forward_server_main, server, 0, NULL);
    if (!server->thread) {
        if (error && error_cap) snprintf(error, error_cap, "cannot start the server thread");
        otsardb_socket_close(listen_fd);
        free(server);
        return NULL;
    }
#else
    server->thread_running = pthread_create(&server->thread, NULL, forward_server_main, server) == 0;
    if (!server->thread_running) {
        if (error && error_cap) snprintf(error, error_cap, "cannot start the server thread");
        otsardb_socket_close(listen_fd);
        free(server);
        return NULL;
    }
#endif
    return server;
}

int otsardb_forward_server_port(const otsardb_forward_server *server) {
    return server ? server->port : 0;
}

void otsardb_forward_server_stop(otsardb_forward_server *server) {
    if (!server) return;
    server->stop = 1;
    /* The loop is in select() with a 200 ms tick; shutdown() unblocks it
     * immediately when it is parked on accept. */
    if (server->listen_fd != OTSARDB_INVALID_SOCKET) {
        otsardb_socket_close(server->listen_fd);
        server->listen_fd = OTSARDB_INVALID_SOCKET;
    }
#if defined(_WIN32)
    if (server->thread) {
        WaitForSingleObject((HANDLE)server->thread, INFINITE);
        CloseHandle((HANDLE)server->thread);
        server->thread = 0;
    }
#else
    if (server->thread_running) {
        pthread_join(server->thread, NULL);
        server->thread_running = 0;
    }
#endif
    free(server);
}

/* ----------------------------------------------------------------- client */

int otsardb_forward_split_host_port(const char *host_port,
                                  char *host,
                                  size_t host_cap,
                                  int *port_out) {
    if (!host_port || !host_port[0] || !host || !host_cap || !port_out) return 0;
    /* F09 (security audit 0129): reject CR/LF and
     * whitespace in the whole host:port string. The client builds the
     * single-line "Host: <host>:<port>" header from host_port verbatim, so
     * a \r or \n in either component is a header injection; whitespace is
     * refused too (an operator-controlled value such as "1.2.3.4\r\nX:
     * y:80" must fail the split instead of injecting a header). strtol
     * would otherwise skip leading whitespace in the port component. */
    for (const char *p = host_port; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0; /* controls incl. \r \n \t */
        if (c == ' ') return 0;
    }
    const char *colon = strrchr(host_port, ':');
    if (!colon || colon == host_port) return 0;
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || port < 1 || port > 65535) return 0;
    size_t host_len = (size_t)(colon - host_port);
    if (host_len == 0 || host_len >= host_cap) return 0;
    memcpy(host, host_port, host_len);
    host[host_len] = '\0';
    *port_out = (int)port;
    return 1;
}

int otsardb_forward_client_post(const char *host_port,
                              const char *sql,
                              char *response,
                              size_t response_cap,
                              int *status_out,
                              int timeout_ms) {
    if (!host_port || !host_port[0] || !sql || !response || !response_cap ||
        !status_out) {
        return -1;
    }
    *status_out = 0;
    response[0] = '\0';
    if (!ensure_wsa()) return -1;

    char host[256];
    int port = 0;
    if (!otsardb_forward_split_host_port(host_port, host, sizeof(host), &port)) {
        return -1;
    }
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(host, port_str, &hints, &resolved) != 0 || !resolved) {
        return -1;
    }

    otsardb_socket fd = OTSARDB_INVALID_SOCKET;
    for (struct addrinfo *ai = resolved; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == OTSARDB_INVALID_SOCKET) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        otsardb_socket_close(fd);
        fd = OTSARDB_INVALID_SOCKET;
    }
    freeaddrinfo(resolved);
    if (fd == OTSARDB_INVALID_SOCKET) return -1;

    int ms = timeout_ms > 0 ? timeout_ms : OTSARDB_FORWARD_TIMEOUT_MS;
    set_socket_timeouts(fd, ms);

    size_t sql_len = strlen(sql);
    char header[512];
    int header_len = snprintf(header,
                              sizeof(header),
                              "POST /forward HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              host_port,
                              sql_len);
    int ok = 0;
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        if (send_all(fd, header, (size_t)header_len) && send_all(fd, sql, sql_len)) {
            /* Read status line + headers + body (Connection: close).
             * The deadline is enforced with select() before each recv:
             * SO_RCVTIMEO proved unreliable for a blocking recv on
             * loopback (spurious WSAETIMEDOUT with data already buffered,
             * observed repeatedly in live forwarding). */
            size_t used = 0;
            size_t body_len = 0;
            int status = 0;
            size_t header_end = 0;
            int got_headers = 0;
            while (used < response_cap) {
                if (!wait_readable(fd, ms)) break;
                int n = recv(fd, response + used, (int)(response_cap - used - 1), 0);
                if (n <= 0) break;
                used += (size_t)n;
                response[used] = '\0';
                if (!got_headers) {
                    for (size_t i = 0; i + 4 <= used; ++i) {
                        if (memcmp(response + i, "\r\n\r\n", 4) == 0) {
                            header_end = i + 4;
                            got_headers = 1;
                            break;
                        }
                    }
                    if (got_headers) {
                        char status_line[64];
                        size_t line_len = 0;
                        const char *crlf = find_bytes(response, header_end, "\r\n", 2);
                        if (crlf) line_len = (size_t)(crlf - response);
                        if (line_len == 0 || line_len >= sizeof(status_line)) {
                            break;
                        }
                        memcpy(status_line, response, line_len);
                        status_line[line_len] = '\0';
                        char *token = strchr(status_line, ' ');
                        if (token) status = (int)strtol(token + 1, NULL, 10);
                        const char *cl = strstr(response, "Content-Length:");
                        if (cl) {
                            char *endp = NULL;
                            long parsed = strtol(cl + strlen("Content-Length:"), &endp, 10);
                            if (parsed >= 0 && parsed <= (long)response_cap) {
                                body_len = (size_t)parsed;
                            }
                        }
                    }
                }
                if (got_headers && body_len > 0 && used >= header_end + body_len) {
                    break;
                }
            }
            if (got_headers && status > 0) {
                *status_out = status;
                /* Move the body to the front (or truncate). */
                size_t available = used > header_end ? used - header_end : 0;
                if (body_len > 0 && body_len < available) available = body_len;
                memmove(response, response + header_end, available);
                response[available] = '\0';
                ok = 1;
            }
        }
    }
    otsardb_socket_close(fd);
    return ok ? 0 : -1;
}

/* ----------------------------------------------------------- body builders */

void otsardb_forward_row_line(char *out, size_t cap, size_t *used, int argc, char **argv) {
    if (!out || !cap || !used) return;
    if (*used >= cap) return;
    size_t pos = *used;
    for (int i = 0; i < argc; ++i) {
        if (i) {
            if (pos + 1 >= cap) break;
            out[pos++] = '\t';
        }
        const char *value = argv[i] ? argv[i] : "NULL";
        size_t len = strlen(value);
        if (pos + len >= cap) len = cap - pos - 1;
        memcpy(out + pos, value, len);
        pos += len;
    }
    if (pos + 1 < cap) out[pos++] = '\n';
    out[pos] = '\0';
    *used = pos;
}

void otsardb_forward_end_marker(char *out, size_t cap, size_t *used, size_t row_count) {
    if (!out || !cap || !used) return;
    if (*used >= cap) return;
    size_t room = cap - *used;
    int n = snprintf(out + *used, room, "# end %zu\n", row_count);
    if (n > 0) {
        if ((size_t)n >= room) *used = cap - 1;
        else *used += (size_t)n;
    }
    out[*used] = '\0';
}

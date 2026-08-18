#ifndef OTSARDB_HA_FORWARD_H
#define OTSARDB_HA_FORWARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M2 HTTP write forwarding (design: 
 * "Next";). A non-leader CLI session that is asked to
 * execute a write SQL statement forwards it over HTTP to the lease
 * leader's embedded server (POST /forward) instead of failing with
 * SQLITE_BUSY. Read-only SQL is never forwarded (the CLI decides via a
 * whitespace-insensitive SELECT/PRAGMA/EXPLAIN prefix check).
 *
 * Minimal embedded HTTP/1.1 server + client, single-threaded, no TLS, no
 * keep-alive, Content-Length-based bodies. Windows uses Winsock2
 * (ws2_32); POSIX uses <sys/socket.h>. The server is loopback-bound
 * (127.0.0.1) by callers; the published lease address is therefore only
 * reachable by processes on the leader's own host (v1 safety boundary:
 * the endpoint executes arbitrary SQL with no authentication).
 *
 * Protocol (v1, documented in the forward design):
 *
 * POST /forward HTTP/1.1
 * Host: <host>:<port>
 * Content-Length: <N>
 * Connection: close
 *
 * <N raw bytes of SQL text>
 *
 * Response 200 OK:
 * body = one line per result row, columns tab-separated, NULL
 * rendered as "NULL" (CLI parity); the final line is the end marker
 * "# end <row_count>". An SQL statement that returns no rows yields
 * exactly the marker line.
 * Response 400 Bad Request: malformed request (missing/invalid
 * Content-Length, headers or body over the 1 MiB cap).
 * Response 404 Not Found: unknown path (convenience, not core).
 * Response 405 Method Not Allowed: non-POST method (convenience).
 * Response 421 Not Leader: the serving session no longer holds a live
 * writer lease (it demoted or a takeover raced the request); body
 * "not leader".
 * Response 500 Internal Server Error: the SQL failed on the leader's
 * own connection; body = the engine error message (raw).
 *
 * Server-side read deadlines (security audit F02): the
 * server is single-threaded, so every recv on the accepted socket is
 * preceded by select() with a bounded window. A connection that sends
 * nothing times out after OTSARDB_FORWARD_READ_IDLE_MS of idle; a
 * connection that drips bytes (slowloris) is additionally capped by the
 * absolute OTSARDB_FORWARD_READ_TOTAL_MS request deadline. A header
 * timeout is answered 400, a body timeout 408 — a stalled client can
 * never hold the accept loop indefinitely.
 *
 * The marker line can collide with a user row whose first field is
 * "# end <n>" — accepted v1 limitation (the protocol is text-only).
 * Response bodies are capped at OTSARDB_FORWARD_MAX_BODY bytes: rows past
 * the cap are truncated (success path) and a request body larger than the
 * cap is rejected with 400. */

#define OTSARDB_FORWARD_MAX_BODY (1u << 20) /* 1 MiB request/response cap */
#define OTSARDB_FORWARD_MAX_HEADERS (16u << 10)
#define OTSARDB_FORWARD_TIMEOUT_MS 10000
/* Server-side read deadlines (audit F02): the idle window
 * bounds a connection that sends nothing; the total deadline bounds a
 * connection that drips bytes indefinitely. */
#define OTSARDB_FORWARD_READ_IDLE_MS 5000
#define OTSARDB_FORWARD_READ_TOTAL_MS 30000

typedef struct otsardb_forward_server otsardb_forward_server;

/* Executes the forwarded SQL on the server's own connection and renders
 * the response body into out/out_cap. Returns the HTTP status to send:
 * 200 (rows rendered, must end with otsardb_forward_end_marker), 421 (this
 * session no longer holds the lease -> "not leader"), or 500 (engine
 * error; body = error message). out must be NUL-terminated on return. */
typedef int (*otsardb_forward_handler)(void *ctx,
                                     const char *sql,
                                     size_t sql_len,
                                     char *out,
                                     size_t out_cap);

/* Starts the server thread and begins accepting on bind_host:port.
 * bind_host "127.0.0.1" loops back only (recommended; no auth). A port
 * of 0 asks the OS for an ephemeral port (query with
 * otsardb_forward_server_port). Returns NULL on failure with error/error_cap
 * filled. The server serves one connection at a time, sequentially. */
otsardb_forward_server *otsardb_forward_server_start(const char *bind_host,
                                                 int port,
                                                 otsardb_forward_handler handler,
                                                 void *ctx,
                                                 char *error,
                                                 size_t error_cap);

/* Actual bound port (usable when the server was started with port 0). */
int otsardb_forward_server_port(const otsardb_forward_server *server);

/* Signals stop, unblocks the accept loop and joins the server thread.
 * Safe to call once; the server must not be used afterwards. */
void otsardb_forward_server_stop(otsardb_forward_server *server);

/* Sends POST /forward with `sql` as the body to host_port ("host:port").
 * On success (0) the HTTP status is in *status_out, the response body is
 * NUL-terminated in response/response_cap (truncated if it exceeds the
 * cap, still returning 0) and the response includes the trailing "# end"
 * marker for 200 responses. Returns -1 on transport/parse/timeout errors;
 * *status_out is then 0 and the caller must treat the leader as
 * unreachable (the forwarding rule degrades to the local BUSY error). */
int otsardb_forward_client_post(const char *host_port,
                              const char *sql,
                              char *response,
                              size_t response_cap,
                              int *status_out,
                              int timeout_ms);

/* Parses "host:port" (IPv6 literals not supported in v1) into host and
 * port. 1 = valid, 0 = invalid. Since (security audit F09)
 * the whole input is rejected when it contains CR/LF or whitespace: the
 * client builds the single-line "Host:" header from the input verbatim,
 * so any control byte would be a header-injection vector. */
int otsardb_forward_split_host_port(const char *host_port,
                                  char *host,
                                  size_t host_cap,
                                  int *port_out);

/* Shared response-body builders used by the leader handler and tests.
 * Both append at *used and keep out NUL-terminated; truncation is
 * silent (caller controls the cap). */
void otsardb_forward_row_line(char *out, size_t cap, size_t *used, int argc, char **argv);
void otsardb_forward_end_marker(char *out, size_t cap, size_t *used, size_t row_count);

#ifdef __cplusplus
}
#endif

#endif

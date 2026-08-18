/* studio.c — OtsarDB Studio loopback web server (MVP v0.1).
 *
 * Socket/thread conventions deliberately mirror src/ha/forward.c (
 * 0032): Winsock2 on Windows, BSD sockets on POSIX, one accept-loop thread,
 * a volatile stop flag with a 200 ms select() tick, one connection at a
 * time, Content-Length bodies, no keep-alive, no TLS, no authentication,
 * loopback-only bind. The security boundary is identical to the forward
 * server and is documented in studio.h.
 *
 * Execution model: the server
 * executes SQL IN-PROCESS through the same engine path as the CLI — open
 * (otsardb_open / otsardb_s3_open_target_ex), run the --json executor
 * (src/cli/json_mode.c) on the session, close. Per-request open/close adds
 * the documented single-shot session latency (~0.22 s local, 
 * 0036); accepted for v0.1. The server is single-threaded, so the
 * per-request mutation of the process environment (S3 connection settings)
 * is not racy.
 *
 * The single-page UI is embedded at build time: studio/index.html (the
 * canonical source) is converted to a C string literal by CMake into
 * studio_html.inc in the build directory; this file includes it.
 */

#include "studio.h"
#include "json_mode.h"
#include "otsardb.h"
#include "otsardb_internal.h"
#include "sqlite3.h"

#ifdef OTSARDB_ENABLE_S3
#include "s3_database.h"
#include "s3_object_store.h"
#include "s3_target.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* OpenSSL is linked PUBLICly through otsardb_core (CMakeLists.txt:
 * target_link_libraries(otsardb_core PUBLIC ... OpenSSL::Crypto)), so the
 * CSRF token generator (audit F01) can use RAND_bytes
 * without adding a dependency. */
#include <openssl/rand.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <process.h>
typedef SOCKET studio_socket;
#define STUDIO_INVALID_SOCKET INVALID_SOCKET
#define studio_socket_close(fd) closesocket(fd)
#define STUDIO_LAST_SOCKET_ERROR() ((int)WSAGetLastError())
typedef int studio_socklen;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int studio_socket;
#define STUDIO_INVALID_SOCKET (-1)
#define studio_socket_close(fd) close(fd)
#define STUDIO_LAST_SOCKET_ERROR() (errno)
typedef socklen_t studio_socklen;
#endif

#include "studio_html.inc"

#define OTSARDB_STUDIO_MAX_HEADERS (16u << 10)
#define OTSARDB_STUDIO_MAX_BODY (1u << 20)
#define OTSARDB_STUDIO_MAX_SQL (64u << 10)
#define OTSARDB_STUDIO_PORT_DEFAULT 8717
/* Every accepted connection is handled by the single Studio worker. A
 * client that connects and then sends nothing must therefore have a bounded
 * idle window, otherwise it can starve the whole admin UI indefinitely. */
#define OTSARDB_STUDIO_READ_TIMEOUT_MS 5000

/* ------------------------------------------------------ CSRF gate (0136)
 * Per-process CSRF token + loopback Origin/Host validation for every
 * state-changing request (POST /api/*), per security audit finding F01
 * : any web page the operator visits could otherwise POST
 * blind SQL to the loopback server with the ambient process credentials,
 * and delegate signed S3 requests to an attacker-controlled endpoint via
 * the per-request endpoint/keys override. The token is 32 random bytes
 * hex-encoded, generated at server start, delivered to the UI in the
 * GET / page (window.OTSARDB_CSRF_TOKEN) and via GET /api/csrf, and
 * required in the X-OTSARDB-CSRF header on every POST. */
#define OTSARDB_STUDIO_CSRF_BYTES 32
#define OTSARDB_STUDIO_CSRF_HEX (OTSARDB_STUDIO_CSRF_BYTES * 2)

/* -------------------------------------------------------- port (0093) v0.5
 * Studio port resolution for `otsardb studio [--port <port>]`: the --port
 * flag wins, then OTSARDB_STUDIO_PORT, then the default 8717. Pure function
 * (unit-tested in tests/studio_port_test.c). -1 = the provided value is
 * invalid (out of 1..65535); the caller reports the usage error. */
int otsardb_studio_resolve_port(int flag_port, const char *env_value) {
    if (flag_port != 0) {
        return (flag_port >= 1 && flag_port <= 65535) ? flag_port : -1;
    }
    if (env_value && env_value[0]) {
        char *end = NULL;
        long v = strtol(env_value, &end, 10);
        if (!end || *end != '\0' || v < 1 || v > 65535) return -1;
        return (int)v;
    }
    return OTSARDB_STUDIO_PORT_DEFAULT;
}

/* -------------------------------------------------------- window mode v0.6
 * Studio desktop-window mode: --window wins, then
 * OTSARDB_STUDIO_WINDOW (truthy 1/true/yes/on, case-insensitive — the same
 * parsing rule as env_truthy_ambient below), else 0. Pure function
 * (unit-tested in tests/studio_window_test.c). */
int otsardb_studio_window_mode(int flag_window, const char *env_value) {
    if (flag_window) return 1;
    if (env_value && env_value[0]) {
        char low[16] = {0};
        for (size_t i = 0; env_value[i] && i + 1 < sizeof(low); ++i) {
            char c = env_value[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            low[i] = c;
        }
        return strcmp(low, "1") == 0 || strcmp(low, "true") == 0 ||
               strcmp(low, "yes") == 0 || strcmp(low, "on") == 0;
    }
    return 0;
}

/* Fallback decision: the window was requested and the
 * WebView2 loader DLL is (not) available on this machine -> desktop window
 * or the v0.5 browser path. Pure function; loader_available is the runtime
 * probe result from studio_window.c. */
int otsardb_studio_window_plan(int window_requested, int loader_available) {
    if (!window_requested) return OTSARDB_STUDIO_WINDOW_PLAN_NONE;
    return loader_available ? OTSARDB_STUDIO_WINDOW_PLAN_WINDOW
                            : OTSARDB_STUDIO_WINDOW_PLAN_BROWSER;
}

/* ------------------------------------------------------------------ F08
 * Local-file workbenching opt-in (security audit F08):
 * the --local-files flag wins, then OTSARDB_STUDIO_ALLOW_LOCAL (truthy
 * 1/true/yes/on, case-insensitive — the same parsing rule as the ambient
 * env_truthy checks), else 0. Pure function (unit-tested in
 * tests/studio_local_target_test.c). 0 keeps run_request closed to
 * non-s3:// targets (except ":memory:"); 1 restores the pre-0147 behavior
 * where a loopback client could open any local path. */
int otsardb_studio_allow_local_files(int flag_local_files,
                                     const char *env_value) {
    if (flag_local_files) return 1;
    if (env_value && env_value[0]) {
        char low[16] = {0};
        for (size_t i = 0; env_value[i] && i + 1 < sizeof(low); ++i) {
            char c = env_value[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            low[i] = c;
        }
        return strcmp(low, "1") == 0 || strcmp(low, "true") == 0 ||
               strcmp(low, "yes") == 0 || strcmp(low, "on") == 0;
    }
    return 0;
}

/* Best-effort default-browser open (v0.5): Windows uses
 * ShellExecuteA (start), POSIX spawns xdg-open detached. Both are guarded
 * and failures are silent — a headless run must never fail the server. */
static void studio_open_browser(const char *url) {
#if defined(_WIN32)
    (void)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    char cmd[768];
    int n = snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
    if (n > 0 && n < (int)sizeof(cmd)) (void)system(cmd);
#endif
}

struct otsardb_studio_server {
    studio_socket listen_fd;
    int port;
    /* Per-process CSRF token (audit F01): 64 hex chars +
     * NUL, generated at start; required in X-OTSARDB-CSRF on every
     * POST /api/* request. Delivered to the UI via the GET / page and
     * GET /api/csrf. */
    char csrf_token[OTSARDB_STUDIO_CSRF_HEX + 1];
    /* Local-file workbenching opt-in (security audit F08):
     * when 0 (the default) run_request refuses every non-s3:// target
     * that is not the in-memory engine target ":memory:" — a loopback
     * client can no longer open arbitrary local SQLite paths. Only an
     * explicit start-time flag (--local-files / OTSARDB_STUDIO_ALLOW_LOCAL=1)
     * re-enables local-file targets. */
    int allow_local_files;
    volatile int stop;
#if defined(_WIN32)
    uintptr_t thread;
#else
    pthread_t thread;
    int thread_running;
#endif
};

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

/* ------------------------------------------------------------------ util */

static const char *find_bytes(const char *haystack, size_t haystack_len,
                              const char *needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (needle_len > haystack_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
    }
    return NULL;
}

static int wait_readable(studio_socket fd, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select((int)fd + 1, &read_fds, NULL, NULL, &tv);
    return ready > 0 ? 1 : 0;
}

static int send_all(studio_socket fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static void send_response(studio_socket fd, int status, const char *status_text,
                          const char *content_type, const char *body) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              status, status_text, content_type,
                              body ? strlen(body) : 0);
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        send_all(fd, header, (size_t)header_len);
    }
    if (body) send_all(fd, body, strlen(body));
}

/* ------------------------------------------------------ CSRF helpers (0136)
 * Case-insensitive byte-run compare (ASCII only; header names, scheme and
 * host are case-insensitive in HTTP). */
static int ascii_ci_eqn(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return 0;
    }
    return 1;
}

static int ascii_ci_str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Constant-time token comparison (both sides are fixed-length hex, so the
 * length check does not leak anything useful). */
static int csrf_token_equal(const char *a, const char *b) {
    size_t i = 0;
    unsigned char diff = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        diff |= (unsigned char)(a[i] ^ b[i]);
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0' && diff == 0;
}

/* Case-insensitive header lookup over the header zone (the lines after the
 * request line, "\r\n"-separated, NUL-terminated by the caller). Returns 1
 * with the trimmed value in out when the header is present; 0 when absent.
 * A value that does not fit in out is treated as present-but-empty (the
 * gates then reject it — fail closed). */
static int header_value(const char *headers, const char *name, char *out,
                        size_t cap) {
    out[0] = '\0';
    if (!headers || !name || !cap) return 0;
    size_t name_len = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        const char *colon = memchr(p, ':', line_len);
        if (colon && (size_t)(colon - p) == name_len &&
            ascii_ci_eqn(p, name, name_len)) {
            const char *v = colon + 1;
            size_t v_len = line_len - name_len - 1;
            while (v_len && (*v == ' ' || *v == '\t')) {
                ++v;
                --v_len;
            }
            while (v_len && (v[v_len - 1] == ' ' || v[v_len - 1] == '\t')) {
                --v_len;
            }
            if (v_len >= cap) return 1; /* too long -> empty -> reject */
            memcpy(out, v, v_len);
            out[v_len] = '\0';
            return 1;
        }
        if (!line_end) break;
        p = line_end + 2;
    }
    return 0;
}

/* Media type check: "application/json" (case-insensitive) with optional
 * parameters after ';' or ',' (e.g. "application/json; charset=utf-8"). */
static int content_type_is_json(const char *ct) {
    if (!ct || !ct[0]) return 0;
    size_t n = 0;
    while (ct[n] && ct[n] != ';' && ct[n] != ',') ++n;
    if (n != strlen("application/json")) return 0;
    return ascii_ci_eqn(ct, "application/json", n);
}

/* Generates the per-process CSRF token: OTSARDB_STUDIO_CSRF_BYTES random
 * bytes hex-encoded. Returns 1 on success; 0 on RNG failure (the caller
 * fails the server start — fail closed). */
static int studio_csrf_generate(char *out, size_t cap) {
    unsigned char bytes[OTSARDB_STUDIO_CSRF_BYTES];
    if (cap < OTSARDB_STUDIO_CSRF_HEX + 1) return 0;
    if (RAND_bytes(bytes, (int)sizeof(bytes)) != 1) return 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
    out[OTSARDB_STUDIO_CSRF_HEX] = '\0';
    return 1;
}

/* Reads until "\r\n\r\n"; returns 0 on failure (timeout/closed/oversize). */
static int read_request_headers(studio_socket fd, char *buffer, size_t cap,
                                size_t *body_offset, size_t *used_out) {
    size_t used = 0;
    for (;;) {
        if (used + 1 >= cap) return 0;
        if (!wait_readable(fd, OTSARDB_STUDIO_READ_TIMEOUT_MS)) return 0;
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
    if (!found) return -1;
    found += strlen("Content-Length:");
    while (*found == ' ' || *found == '\t') ++found;
    char *end = NULL;
    long parsed = strtol(found, &end, 10);
    if (end == found || parsed < 0) return -1;
    return parsed;
}

/* ------------------------------------------------------ request parsing */

/* Cell storage type (stable buffer owned by the request). */
typedef struct {
    int is_null;
    int is_number;
    char text[OTSARDB_EDIT_MAX_CELL];
} studio_cell_storage;

typedef struct {
    char target[1024];
    char sql[OTSARDB_STUDIO_MAX_SQL];
    char endpoint[512];
    char region[256];
    char bucket[512];
    char database[512];
    char access_key[512];
    char secret_key[512];
    /* Admin-action env window: "1" applies the variable for
     * the duration of the request only. */
    char auto_checkpoint[16];
    char auto_checkpoint_commits[16];
    char gc[16];
    /* v0.4: server-side paging fields (flat strings) and
     * the /api/schema with_sql flag. */
    char page_size[16];
    char page[16];
    char with_sql[16];
    /* Row-edit request (/api/edit). */
    char edit_table[256];
    char edit_op[16];
    char edit_cols[OTSARDB_EDIT_MAX_COLS][OTSARDB_EDIT_MAX_CELL];
    char edit_pk[OTSARDB_EDIT_MAX_COLS][OTSARDB_EDIT_MAX_CELL];
    studio_cell_storage edit_old[OTSARDB_EDIT_MAX_COLS];
    studio_cell_storage edit_new[OTSARDB_EDIT_MAX_COLS];
    int has_target, has_sql, has_endpoint, has_region;
    int has_bucket, has_database, has_access_key, has_secret_key;
    int has_auto_checkpoint, has_auto_checkpoint_commits, has_gc;
    int has_page_size, has_page, has_with_sql;
    int has_edit_table, has_edit_op;
    int n_edit_cols, n_edit_pk, n_edit_old, n_edit_new;
} studio_req;

/* Encodes one Unicode code point as UTF-8 into out (max 4 bytes); returns
 * the byte count. Surrogates and > BMP map to U+FFFD (documented v1
 * limitation). */
static int utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)0xEF;
    out[1] = (char)0xBF;
    out[2] = (char)0xBD;
    return 3;
}

static int hex4(const char *s, unsigned long *out) {
    unsigned long v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

/* Decodes the JSON string starting at body[*i] ('"' already at *i) into
 * out/out_cap; advances *i past the closing quote. 0 = success,
 * -1 = malformed or value too long. */
static int parse_string_value(const char *body, size_t len, size_t *i,
                              char *out, size_t out_cap) {
    if (*i >= len || body[*i] != '"') return -1;
    ++*i;
    size_t o = 0;
    while (*i < len) {
        unsigned char c = (unsigned char)body[*i];
        if (c == '"') {
            ++*i;
            out[o] = '\0';
            return 0;
        }
        if (c == '\\') {
            if (*i + 1 >= len) return -1;
            char e = body[*i + 1];
            char simple = 0;
            switch (e) {
            case '"': simple = '"'; break;
            case '\\': simple = '\\'; break;
            case '/': simple = '/'; break;
            case 'n': simple = '\n'; break;
            case 'r': simple = '\r'; break;
            case 't': simple = '\t'; break;
            case 'b': simple = '\b'; break;
            case 'f': simple = '\f'; break;
            default: break;
            }
            if (simple) {
                if (o + 1 >= out_cap) return -1;
                out[o++] = simple;
                *i += 2;
            } else if (e == 'u') {
                if (*i + 6 > len) return -1;
                unsigned long cp;
                if (!hex4(body + *i + 2, &cp)) return -1;
                char enc[4];
                int n = utf8_encode(cp, enc);
                if (o + (size_t)n >= out_cap) return -1;
                memcpy(out + o, enc, (size_t)n);
                o += (size_t)n;
                *i += 6;
            } else {
                return -1;
            }
        } else if (c < 0x20) {
            return -1; /* unescaped control character */
        } else {
            if (o + 1 >= out_cap) return -1;
            out[o++] = (char)c;
            ++*i;
        }
    }
    return -1;
}

/* Parses a JSON array of string scalars at body[*i] ('[' expected) into
 * entries (max entries, each up to cap bytes incl. NUL). On success *i is
 * advanced past the ']'. */
static int parse_string_array(const char *body, size_t len, size_t *i,
                              char (*entries)[OTSARDB_EDIT_MAX_CELL], int max,
                              int *count_out) {
    if (*i >= len || body[*i] != '[') return -1;
    ++*i;
    int count = 0;
    for (;;) {
        while (*i < len && (body[*i] == ' ' || body[*i] == '\t' ||
                            body[*i] == '\r' || body[*i] == '\n')) {
            ++*i;
        }
        if (*i >= len) return -1;
        if (body[*i] == ']') {
            ++*i;
            break;
        }
        if (count >= max) return -1;
        if (body[*i] != '"') return -1;
        if (parse_string_value(body, len, i, entries[count],
                               OTSARDB_EDIT_MAX_CELL) != 0) {
            return -1;
        }
        ++count;
        while (*i < len && (body[*i] == ' ' || body[*i] == '\t' ||
                            body[*i] == '\r' || body[*i] == '\n')) {
            ++*i;
        }
        if (*i >= len) return -1;
        if (body[*i] == ',') {
            ++*i;
            continue;
        }
        if (body[*i] == ']') {
            ++*i;
            break;
        }
        return -1;
    }
    *count_out = count;
    return 0;
}

/* Parses one JSON value into a cell: string (escapes), number (raw text),
 * null. On entry body[*i] is the value's first byte; on success *i is
 * advanced past the value. Returns 0 on success, -1 on malformed. */
static int parse_cell_value(const char *body, size_t len, size_t *i,
                            studio_cell_storage *cell) {
    if (*i >= len) return -1;
    if (body[*i] == '"') {
        if (parse_string_value(body, len, i, cell->text,
                               sizeof(cell->text)) != 0) {
            return -1;
        }
        cell->is_null = 0;
        cell->is_number = 0;
        return 0;
    }
    if (len - *i >= 4 && strncmp(body + *i, "null", 4) == 0) {
        cell->is_null = 1;
        cell->is_number = 0;
        cell->text[0] = '\0';
        *i += 4;
        return 0;
    }
    /* Number: a run of number characters (the full RFC 8259 grammar is
     * validated later by the edit generator). */
    size_t start = *i;
    while (*i < len && (body[*i] == '-' || body[*i] == '+' || body[*i] == '.' ||
                        body[*i] == 'e' || body[*i] == 'E' ||
                        (body[*i] >= '0' && body[*i] <= '9'))) {
        ++*i;
    }
    if (*i == start) return -1;
    size_t n = *i - start;
    if (n >= sizeof(cell->text)) return -1;
    memcpy(cell->text, body + start, n);
    cell->text[n] = '\0';
    cell->is_null = 0;
    cell->is_number = 1;
    return 0;
}

/* Parses a JSON array of scalar values at body[*i] ('[' expected) into
 * cells (max entries). On success *i is advanced past the ']'. */
static int parse_cell_array(const char *body, size_t len, size_t *i,
                            studio_cell_storage *cells, int max,
                            int *count_out) {
    if (*i >= len || body[*i] != '[') return -1;
    ++*i;
    int count = 0;
    for (;;) {
        while (*i < len && (body[*i] == ' ' || body[*i] == '\t' ||
                            body[*i] == '\r' || body[*i] == '\n')) {
            ++*i;
        }
        if (*i >= len) return -1;
        if (body[*i] == ']') {
            ++*i;
            break;
        }
        if (count >= max) return -1;
        if (parse_cell_value(body, len, i, &cells[count]) != 0) return -1;
        ++count;
        while (*i < len && (body[*i] == ' ' || body[*i] == '\t' ||
                            body[*i] == '\r' || body[*i] == '\n')) {
            ++*i;
        }
        if (*i >= len) return -1;
        if (body[*i] == ',') {
            ++*i;
            continue;
        }
        if (body[*i] == ']') {
            ++*i;
            break;
        }
        return -1;
    }
    *count_out = count;
    return 0;
}

/* Parses the flat JSON object of string values into req. 1 = parsed
 * (missing keys stay unset), 0 = malformed. The body must be a JSON
 * object ('{' ... '}' with only trailing whitespace allowed after the
 * closing brace); non-string values (numbers, null) are skipped, not
 * rejected — but anything that is not a JSON object is rejected (HTTP
 * 400). Values longer than a field's cap reject the request (fail-closed,
 * no truncation). */
static int parse_query_request(const char *body, size_t len, studio_req *req) {
    memset(req, 0, sizeof(*req));
    size_t i = 0;
    while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' ||
                       body[i] == '\n')) {
        ++i;
    }
    if (i >= len || body[i] != '{') return 0;
    ++i;
    while (i < len) {
        while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' ||
                           body[i] == '\n')) {
            ++i;
        }
        if (i >= len) return 0;
        if (body[i] == '}') {
            ++i;
            while (i < len && (body[i] == ' ' || body[i] == '\t' ||
                               body[i] == '\r' || body[i] == '\n')) {
                ++i;
            }
            return i == len ? 1 : 0; /* trailing junk after '}' -> malformed */
        }
        if (body[i] != '"') return 0; /* object members must be string keys */
        char key[64];
        if (parse_string_value(body, len, &i, key, sizeof(key)) != 0) return 0;
        while (i < len && (body[i] == ' ' || body[i] == '\t')) ++i;
        if (i >= len || body[i] != ':') return 0;
        ++i;
        while (i < len && (body[i] == ' ' || body[i] == '\t')) ++i;
        if (i < len && body[i] == '"') {
            char value[OTSARDB_STUDIO_MAX_SQL];
            if (parse_string_value(body, len, &i, value, sizeof(value)) != 0) {
                return 0;
            }
            if (strcmp(key, "target") == 0) {
                if (strlen(value) >= sizeof(req->target)) return 0;
                snprintf(req->target, sizeof(req->target), "%s", value);
                req->has_target = 1;
            } else if (strcmp(key, "sql") == 0) {
                if (strlen(value) >= sizeof(req->sql)) return 0;
                snprintf(req->sql, sizeof(req->sql), "%s", value);
                req->has_sql = 1;
            } else if (strcmp(key, "endpoint") == 0) {
                if (strlen(value) >= sizeof(req->endpoint)) return 0;
                snprintf(req->endpoint, sizeof(req->endpoint), "%s", value);
                req->has_endpoint = 1;
            } else if (strcmp(key, "region") == 0) {
                if (strlen(value) >= sizeof(req->region)) return 0;
                snprintf(req->region, sizeof(req->region), "%s", value);
                req->has_region = 1;
            } else if (strcmp(key, "bucket") == 0) {
                if (strlen(value) >= sizeof(req->bucket)) return 0;
                snprintf(req->bucket, sizeof(req->bucket), "%s", value);
                req->has_bucket = 1;
            } else if (strcmp(key, "database") == 0) {
                if (strlen(value) >= sizeof(req->database)) return 0;
                snprintf(req->database, sizeof(req->database), "%s", value);
                req->has_database = 1;
            } else if (strcmp(key, "access_key") == 0) {
                if (strlen(value) >= sizeof(req->access_key)) return 0;
                snprintf(req->access_key, sizeof(req->access_key), "%s", value);
                req->has_access_key = 1;
            } else if (strcmp(key, "secret_key") == 0) {
                if (strlen(value) >= sizeof(req->secret_key)) return 0;
                snprintf(req->secret_key, sizeof(req->secret_key), "%s", value);
                req->has_secret_key = 1;
            } else if (strcmp(key, "auto_checkpoint") == 0) {
                if (strlen(value) >= sizeof(req->auto_checkpoint)) return 0;
                snprintf(req->auto_checkpoint, sizeof(req->auto_checkpoint),
                         "%s", value);
                req->has_auto_checkpoint = 1;
            } else if (strcmp(key, "auto_checkpoint_commits") == 0) {
                if (strlen(value) >= sizeof(req->auto_checkpoint_commits)) {
                    return 0;
                }
                snprintf(req->auto_checkpoint_commits,
                         sizeof(req->auto_checkpoint_commits), "%s", value);
                req->has_auto_checkpoint_commits = 1;
            } else if (strcmp(key, "gc") == 0) {
                if (strlen(value) >= sizeof(req->gc)) return 0;
                snprintf(req->gc, sizeof(req->gc), "%s", value);
                req->has_gc = 1;
            } else if (strcmp(key, "page_size") == 0) {
                if (strlen(value) >= sizeof(req->page_size)) return 0;
                snprintf(req->page_size, sizeof(req->page_size), "%s", value);
                req->has_page_size = 1;
            } else if (strcmp(key, "page") == 0) {
                if (strlen(value) >= sizeof(req->page)) return 0;
                snprintf(req->page, sizeof(req->page), "%s", value);
                req->has_page = 1;
            } else if (strcmp(key, "with_sql") == 0) {
                if (strlen(value) >= sizeof(req->with_sql)) return 0;
                snprintf(req->with_sql, sizeof(req->with_sql), "%s", value);
                req->has_with_sql = 1;
            } else if (strcmp(key, "table") == 0) {
                if (strlen(value) >= sizeof(req->edit_table)) return 0;
                snprintf(req->edit_table, sizeof(req->edit_table), "%s", value);
                req->has_edit_table = 1;
            } else if (strcmp(key, "op") == 0) {
                if (strlen(value) >= sizeof(req->edit_op)) return 0;
                snprintf(req->edit_op, sizeof(req->edit_op), "%s", value);
                req->has_edit_op = 1;
            }
        } else if (body[i] == '[') {
            /* v0.3 edit-request arrays: "cols"/"pk" are string arrays,
             * "old"/"new" are scalar arrays. Other array values are
             * skipped (bounded; v1 parser limitation). */
            if (strcmp(key, "cols") == 0) {
                int count = 0;
                if (parse_string_array(body, len, &i, req->edit_cols,
                                       OTSARDB_EDIT_MAX_COLS, &count) != 0) {
                    return 0;
                }
                req->n_edit_cols = count;
            } else if (strcmp(key, "pk") == 0) {
                int count = 0;
                if (parse_string_array(body, len, &i, req->edit_pk,
                                       OTSARDB_EDIT_MAX_COLS, &count) != 0) {
                    return 0;
                }
                req->n_edit_pk = count;
            } else if (strcmp(key, "old") == 0) {
                int count = 0;
                if (parse_cell_array(body, len, &i, req->edit_old,
                                     OTSARDB_EDIT_MAX_COLS, &count) != 0) {
                    return 0;
                }
                req->n_edit_old = count;
            } else if (strcmp(key, "new") == 0) {
                int count = 0;
                if (parse_cell_array(body, len, &i, req->edit_new,
                                     OTSARDB_EDIT_MAX_COLS, &count) != 0) {
                    return 0;
                }
                req->n_edit_new = count;
            } else {
                while (i < len && body[i] != ']') ++i;
                if (i < len) ++i;
            }
        } else {
            /* non-string value (number/null/object): skip to the next
             * ',' or '}' (bounded, no nesting awareness — v1 limitation). */
            while (i < len && body[i] != ',' && body[i] != '}') ++i;
        }
        if (i < len && body[i] == ',') ++i;
    }
    return 0; /* '}' never seen */
}

/* ------------------------------------------------------- query execution */

typedef struct {
    const char *name;
    char previous[512];
    int was_set;
    int apply;
} env_override;

static int env_get(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    if (!v) return 0;
    snprintf(out, cap, "%s", v);
    return 1;
}

static void env_set(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    setenv(name, value ? value : "", 1);
#endif
}

static void env_restore(env_override *o) {
    if (!o->apply) return;
#if defined(_WIN32)
    _putenv_s(o->name, o->was_set ? o->previous : "");
#else
    if (o->was_set) setenv(o->name, o->previous, 1);
    else unsetenv(o->name);
#endif
}

static void apply_override(env_override *o, const char *value) {
    if (!value || !value[0]) return; /* blank field: keep process env */
    o->apply = 1;
    o->was_set = env_get(o->name, o->previous, sizeof(o->previous));
    env_set(o->name, value);
}

static int is_s3_target(const char *target) {
    return target &&
           (strncmp(target, "s3://", 5) == 0 ||
            strncmp(target, "otsardb+s3://", 11) == 0);
}

/* ------------------------------------------------------- schema JSON (0073)
 *
 * The --json-schema payload (shared by the CLI mode and POST /api/schema):
 * one {"ok":true,"tables":[...]} object built from sqlite_schema +
 * PRAGMA table_info(<t>) + PRAGMA index_list(<t>) + PRAGMA index_info(<idx>)
 * on the opened session. Every emission is hand-rolled (no JSON library)
 * and bounded: <= OTSARDB_SCHEMA_MAX_OBJECTS objects and <= out_cap bytes,
 * fail-closed with an explicit error object (never silent truncation).
 */

#define OTSARDB_SCHEMA_MAX_OBJECTS 10000

/* Growable byte buffer with a hard cap; overflow is sticky (fail-closed). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int overflow;
} sbuf;

static int sbuf_grow(sbuf *b, size_t need) {
    if (b->overflow) return -1;
    if (b->data && b->len + need <= b->cap) return 0;
    size_t target = b->len + need;
    if (target > b->cap) {
        /* The hard cap is a fail-closed limit: a need above it must never
         * realloc to a smaller buffer (heap overflow) — it sets overflow. */
        b->overflow = 1;
        return -1;
    }
    size_t new_cap = b->cap ? b->cap : 4096;
    while (new_cap < target) {
        size_t grown = new_cap * 2;
        if (grown > b->cap) { new_cap = b->cap; break; }
        new_cap = grown;
    }
    char *data = (char *)realloc(b->data, new_cap);
    if (!data) { b->overflow = 1; return -1; }
    b->data = data;
    b->cap = new_cap;
    return 0;
}

static int sbuf_putn(sbuf *b, const char *s, size_t n) {
    if (sbuf_grow(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int sbuf_puts(sbuf *b, const char *s) {
    return sbuf_putn(b, s, strlen(s));
}

static int sbuf_printf(sbuf *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return sbuf_putn(b, tmp, (size_t)n);
}

/* Appends `s` (n bytes) as a JSON string value (quotes included), escaping
 * quotes, backslashes and control characters (same rule as json_mode.c). */
static int sbuf_js_string(sbuf *b, const char *s, size_t n) {
    if (sbuf_puts(b, "\"") != 0) return -1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  if (sbuf_puts(b, "\\\"") != 0) return -1; break;
        case '\\': if (sbuf_puts(b, "\\\\") != 0) return -1; break;
        default:
            if (c < 0x20) {
                char tmp[8];
                int k = snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                if (k <= 0 || sbuf_putn(b, tmp, (size_t)k) != 0) return -1;
            } else if (sbuf_putn(b, (const char *)&c, 1) != 0) {
                return -1;
            }
        }
    }
    return sbuf_puts(b, "\"");
}

/* Appends `name` as a double-quoted SQLite identifier ("a""b" escaping). */
static int sbuf_quoted_ident(sbuf *b, const char *name) {
    if (sbuf_puts(b, "\"") != 0) return -1;
    for (const char *p = name; *p; ++p) {
        if (*p == '"' && sbuf_puts(b, "\"\"") != 0) return -1;
        if (*p != '"' && sbuf_putn(b, p, 1) != 0) return -1;
    }
    return sbuf_puts(b, "\"");
}

/* Prepares `sql` on the handle; on failure writes the error object to out. */
static sqlite3_stmt *schema_prepare(sqlite3 *h, const char *sql, char *out,
                                    size_t out_cap) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK) {
        otsardb_cli_json_error(sqlite3_errmsg(h), out, out_cap);
        return NULL;
    }
    return stmt;
}

/* Appends one table's columns array: PRAGMA table_info("<t>") rows
 * (cid, name, type, notnull, dflt_value, pk). */
static int schema_columns(sqlite3 *h, sbuf *b, const char *name,
                          char *out, size_t out_cap) {
    char sql[1024];
    sbuf q = {0, 0, sizeof(sql)};
    if (sbuf_puts(&q, "PRAGMA table_info(") != 0 ||
        sbuf_quoted_ident(&q, name) != 0 ||
        sbuf_puts(&q, ");") != 0) {
        free(q.data);
        otsardb_cli_json_error("schema: identifier too long", out, out_cap);
        return -1;
    }
    snprintf(sql, sizeof(sql), "%s", q.data ? q.data : "");
    free(q.data);
    sqlite3_stmt *stmt = schema_prepare(h, sql, out, out_cap);
    if (!stmt) return -1;
    int first = 1;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            otsardb_cli_json_error(sqlite3_errmsg(h), out, out_cap);
            sqlite3_finalize(stmt);
            return -1;
        }
        const char *cname = (const char *)sqlite3_column_text(stmt, 1);
        const char *ctype = (const char *)sqlite3_column_text(stmt, 2);
        int notnull = sqlite3_column_int(stmt, 3);
        int pk = sqlite3_column_int(stmt, 5);
        if (sbuf_puts(b, first ? "{" : ",{") != 0 ||
            sbuf_puts(b, "\"name\":") != 0 ||
            sbuf_js_string(b, cname ? cname : "", cname ? (size_t)sqlite3_column_bytes(stmt, 1) : 0) != 0 ||
            sbuf_puts(b, ",\"type\":") != 0 ||
            sbuf_js_string(b, ctype ? ctype : "", ctype ? (size_t)sqlite3_column_bytes(stmt, 2) : 0) != 0 ||
            sbuf_printf(b, ",\"pk\":%d,\"notnull\":%d}", pk, notnull) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
        first = 0;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* Appends one table's indexes array: PRAGMA index_list("<t>") then
 * PRAGMA index_info("<idx>") per index (seq, name, unique, origin, partial
 * / seqno, cid, name). */
static int schema_indexes(sqlite3 *h, sbuf *b, const char *name,
                          char *out, size_t out_cap) {
    char sql[1024];
    sbuf q = {0, 0, sizeof(sql)};
    if (sbuf_puts(&q, "PRAGMA index_list(") != 0 ||
        sbuf_quoted_ident(&q, name) != 0 ||
        sbuf_puts(&q, ");") != 0) {
        free(q.data);
        otsardb_cli_json_error("schema: identifier too long", out, out_cap);
        return -1;
    }
    snprintf(sql, sizeof(sql), "%s", q.data ? q.data : "");
    free(q.data);
    sqlite3_stmt *stmt = schema_prepare(h, sql, out, out_cap);
    if (!stmt) return -1;
    int first = 1;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            otsardb_cli_json_error(sqlite3_errmsg(h), out, out_cap);
            sqlite3_finalize(stmt);
            return -1;
        }
        const char *iname = (const char *)sqlite3_column_text(stmt, 1);
        int unique = sqlite3_column_int(stmt, 2);
        if (sbuf_puts(b, first ? "{" : ",{") != 0 ||
            sbuf_puts(b, "\"name\":") != 0 ||
            sbuf_js_string(b, iname ? iname : "", iname ? (size_t)sqlite3_column_bytes(stmt, 1) : 0) != 0 ||
            sbuf_printf(b, ",\"unique\":%d,\"columns\":[", unique) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
        /* PRAGMA index_info("<idx>") -> the indexed column names. */
        char isql[1024];
        sbuf iq = {0, 0, sizeof(isql)};
        if (sbuf_puts(&iq, "PRAGMA index_info(") != 0 ||
            sbuf_quoted_ident(&iq, iname ? iname : "") != 0 ||
            sbuf_puts(&iq, ");") != 0) {
            free(iq.data);
            sqlite3_finalize(stmt);
            return -1;
        }
        snprintf(isql, sizeof(isql), "%s", iq.data ? iq.data : "");
        free(iq.data);
        sqlite3_stmt *ist = schema_prepare(h, isql, out, out_cap);
        if (!ist) {
            sqlite3_finalize(stmt);
            return -1;
        }
        int cfirst = 1;
        for (;;) {
            int irc = sqlite3_step(ist);
            if (irc == SQLITE_DONE) break;
            if (irc != SQLITE_ROW) {
                otsardb_cli_json_error(sqlite3_errmsg(h), out, out_cap);
                sqlite3_finalize(ist);
                sqlite3_finalize(stmt);
                return -1;
            }
            const char *cname = (const char *)sqlite3_column_text(ist, 2);
            if (sbuf_puts(b, cfirst ? "" : ",") != 0 ||
                sbuf_js_string(b, cname ? cname : "",
                               cname ? (size_t)sqlite3_column_bytes(ist, 2) : 0) != 0) {
                sqlite3_finalize(ist);
                sqlite3_finalize(stmt);
                return -1;
            }
            cfirst = 0;
        }
        sqlite3_finalize(ist);
        if (sbuf_puts(b, "]}") != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
        first = 0;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int otsardb_studio_schema_json_opt(otsardb *db, int with_sql, char *out,
                                 size_t out_cap) {
    if (!out || out_cap == 0) return 1;
    out[0] = '\0';
    if (!db) {
        otsardb_cli_json_error("OtsarDB handle missing", out, out_cap);
        return 1;
    }
    sqlite3 *h = otsardb_internal_sqlite_handle(db);
    if (!h) {
        otsardb_cli_json_error("OtsarDB handle is not open", out, out_cap);
        return 1;
    }

    sbuf b = {0, 0, out_cap};
    int ok = 0;
    if (sbuf_puts(&b, "{\"ok\":true,\"tables\":[") != 0) goto done;
    sqlite3_stmt *stmt = schema_prepare(
        h, "SELECT name, type, sql FROM sqlite_schema "
           "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
           "ORDER BY name;", out, out_cap);
    if (!stmt) return 1;
    int first = 1;
    int objects = 0;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            otsardb_cli_json_error(sqlite3_errmsg(h), out, out_cap);
            sqlite3_finalize(stmt);
            return 1;
        }
        if (++objects > OTSARDB_SCHEMA_MAX_OBJECTS) {
            otsardb_cli_json_error("schema exceeds the object cap (10000)",
                                 out, out_cap);
            sqlite3_finalize(stmt);
            return 1;
        }
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *type = (const char *)sqlite3_column_text(stmt, 1);
        const char *ddl = (const char *)sqlite3_column_text(stmt, 2);
        if (!name) continue;
        if (sbuf_puts(&b, first ? "{" : ",{") != 0 ||
            sbuf_puts(&b, "\"name\":") != 0 ||
            sbuf_js_string(&b, name, strlen(name)) != 0 ||
            sbuf_puts(&b, ",\"type\":") != 0 ||
            sbuf_js_string(&b, type ? type : "", type ? strlen(type) : 0) != 0) {
            sqlite3_finalize(stmt);
            goto done;
        }
        /* v0.4: the verbatim stored CREATE statement
         * (sqlite_schema.sql) when the /api/schema request asked for it;
         * NULL renders as an empty string. Without the flag the payload is
         * byte-identical to the plain schema. */
        if (with_sql &&
            (sbuf_puts(&b, ",\"sql\":") != 0 ||
             sbuf_js_string(&b, ddl ? ddl : "", ddl ? (size_t)sqlite3_column_bytes(stmt, 2) : 0) != 0)) {
            sqlite3_finalize(stmt);
            goto done;
        }
        if (sbuf_puts(&b, ",\"columns\":[") != 0) {
            sqlite3_finalize(stmt);
            goto done;
        }
        if (schema_columns(h, &b, name, out, out_cap) != 0) {
            sqlite3_finalize(stmt);
            return 1;
        }
        if (sbuf_puts(&b, "],\"indexes\":[") != 0) {
            sqlite3_finalize(stmt);
            goto done;
        }
        if (schema_indexes(h, &b, name, out, out_cap) != 0) {
            sqlite3_finalize(stmt);
            return 1;
        }
        if (sbuf_puts(&b, "]}") != 0) {
            sqlite3_finalize(stmt);
            goto done;
        }
        first = 0;
    }
    sqlite3_finalize(stmt);
    if (sbuf_puts(&b, "]}\n") != 0) goto done;
    ok = 1;
done:
    if (!ok) {
        otsardb_cli_json_error("--json-schema output cap exceeded", out, out_cap);
        return 1;
    }
    size_t len = b.len < out_cap ? b.len : out_cap - 1;
    memcpy(out, b.data, len);
    out[len] = '\0';
    free(b.data);
    return 0;
}

int otsardb_studio_schema_json(otsardb *db, char *out, size_t out_cap) {
    return otsardb_studio_schema_json_opt(db, 0, out, out_cap);
}

/* ------------------------------------------------------- edit SQL (0078)
 *
 * Row-edit SQL generation for the v0.3 grid editor. Pure function
 * (unit-testable without sockets/stores); quoting rules mirror the schema
 * emitters: identifiers double-quoted with "" doubling, strings as '...'
 * literals with '' doubling, numbers raw (RFC 8259 text), NULL literal.
 */

/* RFC 8259 number grammar guard (same rule as json_mode.c's emitter):
 * -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
static int edit_is_number_text(const char *s) {
    if (!s || !*s) return 0;
    const char *p = s;
    if (*p == '-') ++p;
    if (!*p) return 0;
    if (*p == '0') {
        ++p;
    } else {
        if (*p < '1' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') ++p;
    }
    return *p == '\0';
}

/* Appends one cell as a SQL value: NULL / raw validated number / 'string'
 * with '' doubling. Returns 0 on success, -1 on failure (message set). */
static int sbuf_edit_cell(sbuf *b, const otsardb_studio_cell *cell,
                          char *err, size_t err_cap) {
    if (cell->is_null) return sbuf_puts(b, "NULL");
    if (cell->is_number) {
        if (!edit_is_number_text(cell->text)) {
            snprintf(err, err_cap, "invalid number text in row value: %s",
                     cell->text ? cell->text : "(null)");
            return -1;
        }
        return sbuf_puts(b, cell->text);
    }
    if (sbuf_puts(b, "'") != 0) return -1;
    for (const char *p = cell->text ? cell->text : ""; *p; ++p) {
        if (*p == '\'' && sbuf_puts(b, "''") != 0) return -1;
        if (*p != '\'' && sbuf_putn(b, p, 1) != 0) return -1;
    }
    return sbuf_puts(b, "'");
}

/* Appends "name" = <cell> (or "name" IS NULL for a NULL pk cell) for a
 * WHERE predicate. */
static int sbuf_edit_predicate(sbuf *b, const char *name,
                               const otsardb_studio_cell *cell, char *err,
                               size_t err_cap) {
    if (sbuf_quoted_ident(b, name) != 0) return -1;
    if (cell->is_null) return sbuf_puts(b, " IS NULL");
    if (sbuf_puts(b, "=") != 0) return -1;
    return sbuf_edit_cell(b, cell, err, err_cap);
}

int otsardb_studio_edit_sql(const otsardb_studio_edit *edit, char *out,
                          size_t out_cap) {
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    char err[256] = "invalid edit request";
    if (!edit) return -1;
    if (!edit->table || !edit->table[0]) {
        snprintf(err, sizeof(err), "missing table name");
        goto fail;
    }
    int is_update = 0, is_delete = 0, is_insert = 0;
    if (edit->op && strcmp(edit->op, "update") == 0) is_update = 1;
    else if (edit->op && strcmp(edit->op, "delete") == 0) is_delete = 1;
    else if (edit->op && strcmp(edit->op, "insert") == 0) is_insert = 1;
    else {
        snprintf(err, sizeof(err), "unsupported edit operation: %s",
                 edit->op ? edit->op : "(missing)");
        goto fail;
    }
    if (!edit->cols || edit->n_cols < 1) {
        snprintf(err, sizeof(err), "edit request has no columns");
        goto fail;
    }
    if (edit->n_cols > OTSARDB_EDIT_MAX_COLS) {
        snprintf(err, sizeof(err), "too many columns (max %d)",
                 OTSARDB_EDIT_MAX_COLS);
        goto fail;
    }
    if (!edit->pk || edit->n_pk < 1) {
        if (is_update || is_delete) {
            snprintf(err, sizeof(err), "update/delete require primary-key "
                     "columns (add \"pk\" to the request)");
            goto fail;
        }
    } else if (edit->n_pk > OTSARDB_EDIT_MAX_COLS) {
        snprintf(err, sizeof(err), "too many primary-key columns (max %d)",
                 OTSARDB_EDIT_MAX_COLS);
        goto fail;
    }
    if (is_update) {
        if (!edit->new_row || edit->n_new != edit->n_cols) {
            snprintf(err, sizeof(err), "update requires \"new\" with the "
                     "same column count");
            goto fail;
        }
        if (!edit->old_row || edit->n_old != edit->n_cols) {
            snprintf(err, sizeof(err), "update requires \"old\" with the "
                     "same column count");
            goto fail;
        }
    }
    if (is_insert) {
        if (!edit->new_row || edit->n_new != edit->n_cols) {
            snprintf(err, sizeof(err), "insert requires \"new\" with the "
                     "same column count");
            goto fail;
        }
    }
    if (is_delete) {
        if (!edit->old_row || edit->n_old != edit->n_cols) {
            snprintf(err, sizeof(err), "delete requires \"old\" with the "
                     "same column count");
            goto fail;
        }
    }

    sbuf b = {0, 0, out_cap};
    int ok = 1;
    if (is_update || is_delete) {
        /* WHERE on the original primary-key values: the row as it was
         * read, no locks (single-user optimistic editing). */
        if (sbuf_puts(&b, is_update ? "UPDATE " : "DELETE FROM ") != 0 ||
            sbuf_quoted_ident(&b, edit->table) != 0) {
            ok = 0;
        }
        if (ok && is_update) {
            if (sbuf_puts(&b, " SET ") != 0) ok = 0;
            for (int i = 0; i < edit->n_cols && ok; ++i) {
                if (i > 0 && sbuf_puts(&b, ",") != 0) { ok = 0; break; }
                if (sbuf_quoted_ident(&b, edit->cols[i]) != 0) { ok = 0; break; }
                if (sbuf_puts(&b, "=") != 0) { ok = 0; break; }
                if (sbuf_edit_cell(&b, &edit->new_row[i], err, sizeof(err)) != 0) {
                    ok = 0;
                }
            }
        }
        if (ok && sbuf_puts(&b, " WHERE ") != 0) ok = 0;
        for (int p = 0; p < edit->n_pk && ok; ++p) {
            /* Map the pk column name to its index in the row. */
            int idx = -1;
            for (int i = 0; i < edit->n_cols; ++i) {
                if (strcmp(edit->cols[i], edit->pk[p]) == 0) { idx = i; break; }
            }
            if (idx < 0) {
                snprintf(err, sizeof(err),
                         "primary-key column '%s' is not among the row "
                         "columns", edit->pk[p]);
                ok = 0;
                break;
            }
            if (p > 0 && sbuf_puts(&b, " AND ") != 0) { ok = 0; break; }
            if (sbuf_edit_predicate(&b, edit->pk[p],
                                    &edit->old_row[idx], err, sizeof(err)) != 0) {
                ok = 0;
            }
        }
    } else { /* insert */
        if (sbuf_puts(&b, "INSERT INTO ") != 0 ||
            sbuf_quoted_ident(&b, edit->table) != 0 ||
            sbuf_puts(&b, " (") != 0) {
            ok = 0;
        }
        for (int i = 0; i < edit->n_cols && ok; ++i) {
            if (i > 0 && sbuf_puts(&b, ",") != 0) { ok = 0; break; }
            if (sbuf_quoted_ident(&b, edit->cols[i]) != 0) { ok = 0; break; }
        }
        if (ok && sbuf_puts(&b, ") VALUES (") != 0) ok = 0;
        for (int i = 0; i < edit->n_cols && ok; ++i) {
            if (i > 0 && sbuf_puts(&b, ",") != 0) { ok = 0; break; }
            if (sbuf_edit_cell(&b, &edit->new_row[i], err, sizeof(err)) != 0) {
                ok = 0;
            }
        }
        if (ok && sbuf_puts(&b, ")") != 0) ok = 0;
    }
    if (ok && sbuf_puts(&b, ";") != 0) ok = 0;
    if (!ok) {
        if (err[0] == '\0') {
            snprintf(err, sizeof(err),
                     "edit SQL exceeds the %zu-byte output cap", out_cap);
        }
        goto fail;
    }
    size_t len = b.len < out_cap ? b.len : out_cap - 1;
    memcpy(out, b.data, len);
    out[len] = '\0';
    free(b.data);
    return 0;
fail:
    snprintf(out, out_cap, "%s", err);
    return -1;
}

/* ------------------------------------------------------------- paging (0086)
 *
 * Server-side paging SQL generation . The matcher is
 * hand-rolled (no regex library, mirroring the JSON emitters): optional
 * leading/trailing whitespace, case-insensitive SELECT/FROM keywords with
 * the main.c identifier-follow rule, exactly one '*', exactly one table
 * identifier (double-quoted with "" escapes, or a bare identifier), an
 * optional single trailing ';'. Everything else — WHERE/ORDER BY/LIMIT/
 * JOIN, comments, bracket/backtick identifiers, multi-statement strings —
 * is rejected (-1) and the caller keeps the original statement, so the UI
 * falls back to client-side paging over the full --json result (0073).
 */

static int paged_ci_eq(const char *text, const char *word) {
    size_t i = 0;
    while (word[i]) {
        char a = text[i];
        char b = word[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return 0;
        ++i;
    }
    return 1;
}

static int paged_ident_follows(const char *text, size_t n) {
    char c = text[n];
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '$');
}

/* Skips the whitespace run at sql[*i]; *i stays at the next byte. */
static void paged_skip_ws(const char *sql, size_t *i) {
    while (sql[*i] == ' ' || sql[*i] == '\t' || sql[*i] == '\r' ||
           sql[*i] == '\n') {
        ++*i;
    }
}

/* Copies the (unquoted) table name out of a double-quoted identifier
 * starting at sql[*i] ('"' at *i): "" doubling is unescaped, the closing
 * quote is consumed. 1 = ok, 0 = malformed or too long. */
static int paged_parse_quoted_ident(const char *sql, size_t *i, char *out,
                                    size_t out_cap) {
    size_t o = 0;
    ++*i; /* opening quote */
    while (sql[*i]) {
        char c = sql[*i];
        if (c == '"') {
            if (sql[*i + 1] == '"') { /* "" escape */
                if (o + 1 >= out_cap) return 0;
                out[o++] = '"';
                *i += 2;
                continue;
            }
            ++*i; /* closing quote */
            out[o] = '\0';
            return 1;
        }
        if (o + 1 >= out_cap) return 0;
        out[o++] = c;
        ++*i;
    }
    return 0;
}

int otsardb_studio_paged_sql(const char *sql, long page, long page_size,
                           int count_only, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (!sql) return -1;
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 200;
    if (page_size > 10000) page_size = 10000;
    long long offset = (long long)(page - 1) * (long long)page_size;
    if (offset > 2147483647LL) return -1; /* OFFSET bound */

    size_t i = 0;
    paged_skip_ws(sql, &i);
    if (!paged_ci_eq(sql + i, "select") ||
        !paged_ident_follows(sql, i + 6)) {
        return -1;
    }
    i += 6;
    paged_skip_ws(sql, &i);
    if (sql[i] != '*') return -1;
    ++i;
    paged_skip_ws(sql, &i);
    if (!paged_ci_eq(sql + i, "from") ||
        !paged_ident_follows(sql, i + 4)) {
        return -1;
    }
    i += 4;
    paged_skip_ws(sql, &i);
    char ident[512];
    if (sql[i] == '"') {
        if (!paged_parse_quoted_ident(sql, &i, ident, sizeof(ident))) return -1;
    } else {
        size_t start = i;
        while (sql[i] && ((sql[i] >= 'a' && sql[i] <= 'z') ||
                          (sql[i] >= 'A' && sql[i] <= 'Z') ||
                          (sql[i] >= '0' && sql[i] <= '9') ||
                          sql[i] == '_' || sql[i] == '$')) {
            ++i;
        }
        if (i == start) return -1;
        size_t n = i - start;
        if (n >= sizeof(ident)) return -1;
        memcpy(ident, sql + start, n);
        ident[n] = '\0';
    }
    /* Trailing: whitespace and an optional single ';' only. */
    paged_skip_ws(sql, &i);
    if (sql[i] == ';') {
        ++i;
        paged_skip_ws(sql, &i);
    }
    if (sql[i] != '\0') return -1;
    if (!ident[0]) return -1;

    sbuf b = {0, 0, out_cap};
    if (count_only) {
        if (sbuf_puts(&b, "SELECT COUNT(*) FROM ") != 0 ||
            sbuf_quoted_ident(&b, ident) != 0 ||
            sbuf_puts(&b, ";") != 0) {
            free(b.data);
            return -1;
        }
    } else {
        if (sbuf_puts(&b, "SELECT * FROM ") != 0 ||
            sbuf_quoted_ident(&b, ident) != 0 ||
            sbuf_printf(&b, " LIMIT %ld OFFSET %lld;", page_size, offset) != 0) {
            free(b.data);
            return -1;
        }
    }
    size_t len = b.len < out_cap ? b.len : out_cap - 1;
    memcpy(out, b.data, len);
    out[len] = '\0';
    free(b.data);
    return 0;
}

/* ------------------------------------------------------- request pipeline */

/* Execution options for the shared request pipeline. The paged flag
 * (v0.4) marks a query that was rewritten to
 * SELECT * FROM "t" LIMIT n OFFSET m by otsardb_studio_paged_sql: after the
 * executor ran it, the pipeline runs the pre-generated COUNT(*) on the
 * SAME session and appends ,"total":N to the (single-line, ok) payload. */
typedef struct {
    int schema_mode;
    int paged;
    const char *count_sql;
} run_options;

/* Appends ,"total":N to a single-statement --json ok payload (exactly one
 * line ending "}\n"). The surgery is deterministic because the --json
 * emitter is the only producer and every cell '"' is escaped as \". */
static void studio_append_total(char *out, size_t cap, long long total) {
    size_t n = strlen(out);
    if (n < 3 || out[n - 1] != '\n' || out[n - 2] != '}') return;
    if (strncmp(out, "{\"ok\":true", 10) != 0) return;
    char suffix[64];
    int k = snprintf(suffix, sizeof(suffix), ",\"total\":%lld", total);
    if (k <= 0 || n + (size_t)k + 1 >= cap) return;
    memmove(out + n - 2 + (size_t)k, out + n - 2, 3); /* "}\n\0" */
    memcpy(out + n - 2, suffix, (size_t)k);
}

/* Shared request execution: applies the per-request env window (endpoint/
 * region/keys), opens the target, runs the query (--json executor) or the
 * schema builder, restores the environment and closes. The env window is
 * race-free because the server is single-threaded. */
static int run_request(const studio_req *req, const run_options *opt,
                       int allow_local_files, char *out, size_t cap) {
    int schema_mode = opt ? opt->schema_mode : 0;
    char target[2048];
    if (req->has_target) {
        snprintf(target, sizeof(target), "%s", req->target);
    } else if (req->has_bucket && req->has_database) {
        snprintf(target, sizeof(target), "s3://%s/%s", req->bucket,
                 req->database);
    } else {
        otsardb_cli_json_error("missing target: provide \"target\" or "
                             "\"bucket\"+\"database\"", out, cap);
        return 0;
    }

    /* F08 (security audit 0129): local-file targets are
     * refused by default. Only s3:// (otsardb+s3://) and the in-memory
     * engine target ":memory:" (no file I/O, so outside the audit's
     * arbitrary-local-file boundary) are always allowed; any other local
     * path requires the server's explicit --local-files opt-in. Without it
     * a loopback client can no longer target arbitrary local paths the
     * process user can access. The refusal is an error JSON object (HTTP
     * 200), matching the "missing target" / "failed to open" convention. */
    if (!is_s3_target(target) && strcmp(target, ":memory:") != 0 &&
        !allow_local_files) {
        otsardb_cli_json_error("local-file targets are disabled in the "
                             "Studio server by default (audit F08); start "
                             "the server with --local-files (or "
                             "OTSARDB_STUDIO_ALLOW_LOCAL=1) to workbench "
                             "local paths", out, cap);
        return 0;
    }

    env_override overrides[7] = {
        {"OTSARDB_S3_ENDPOINT", {0}, 0, 0},
        {"OTSARDB_S3_REGION", {0}, 0, 0},
        {"OTSARDB_S3_ACCESS_KEY", {0}, 0, 0},
        {"OTSARDB_S3_SECRET_KEY", {0}, 0, 0},
        {"OTSARDB_AUTO_CHECKPOINT", {0}, 0, 0},
        {"OTSARDB_AUTO_CHECKPOINT_COMMITS", {0}, 0, 0},
        {"OTSARDB_GC", {0}, 0, 0},
    };
    apply_override(&overrides[0], req->has_endpoint ? req->endpoint : NULL);
    apply_override(&overrides[1], req->has_region ? req->region : NULL);
    apply_override(&overrides[2], req->has_access_key ? req->access_key : NULL);
    apply_override(&overrides[3], req->has_secret_key ? req->secret_key : NULL);
    /* Admin actions: the per-request env window carries
     * OTSARDB_AUTO_CHECKPOINT / OTSARDB_AUTO_CHECKPOINT_COMMITS / OTSARDB_GC for
     * the duration of the request only, so the closing session runs the
     * forced checkpoint (0018) and, with gc=1, the GC sweep (0040). */
    apply_override(&overrides[4],
                   req->has_auto_checkpoint ? req->auto_checkpoint : NULL);
    apply_override(&overrides[5], req->has_auto_checkpoint_commits
                                      ? req->auto_checkpoint_commits
                                      : NULL);
    apply_override(&overrides[6], req->has_gc ? req->gc : NULL);

    otsardb *db = NULL;
    int rc;
    if (is_s3_target(target)) {
#ifdef OTSARDB_ENABLE_S3
        rc = otsardb_s3_open_target_ex(target, NULL, &db);
        if (rc != OTSARDB_OK) {
            char msg[1600];
            snprintf(msg, sizeof(msg), "failed to open '%s': %s", target,
                     otsardb_s3_last_error());
            otsardb_cli_json_error(msg, out, cap);
            for (int k = 0; k < 7; ++k) env_restore(&overrides[k]);
            return 0;
        }
#else
        otsardb_cli_json_error("this OtsarDB binary was built without native S3 "
                             "support; rebuild with OTSARDB_ENABLE_S3=ON",
                             out, cap);
        for (int k = 0; k < 7; ++k) env_restore(&overrides[k]);
        return 0;
#endif
    } else {
        rc = otsardb_open(target, &db);
        if (rc != OTSARDB_OK) {
            char msg[1600];
            snprintf(msg, sizeof(msg), "failed to open '%s'", target);
            otsardb_cli_json_error(msg, out, cap);
            for (int k = 0; k < 7; ++k) env_restore(&overrides[k]);
            return 0;
        }
    }

    if (schema_mode) {
        (void)otsardb_studio_schema_json_opt(
            db, req->has_with_sql && req->with_sql[0] &&
                    strcmp(req->with_sql, "0") != 0
                ? 1
                : 0,
            out, cap);
    } else {
        (void)otsardb_cli_exec_json(db, req->sql, out, cap);
        if (opt && opt->paged && opt->count_sql &&
            strncmp(out, "{\"ok\":true", 10) == 0) {
            /* Row count on the same session: COUNT(*) of the same table.
             * A failing COUNT leaves the payload without "total" (the
             * SELECT page itself stays authoritative). */
            sqlite3 *h = otsardb_internal_sqlite_handle(db);
            sqlite3_stmt *cs = NULL;
            if (h && sqlite3_prepare_v2(h, opt->count_sql, -1, &cs, NULL) ==
                         SQLITE_OK) {
                if (sqlite3_step(cs) == SQLITE_ROW) {
                    studio_append_total(out, cap,
                                        (long long)sqlite3_column_int64(cs, 0));
                }
                sqlite3_finalize(cs);
            }
        }
    }
    otsardb_close(db);
    for (int k = 0; k < 7; ++k) env_restore(&overrides[k]);
    return 0;
}

/* Executes one /api/query request. On success (or handled failure) the
 * response body is the --json payload (ok or error object) and 0 is
 * returned; -1 means the request itself was malformed (HTTP 400). */
static int handle_query(const char *body, size_t len, int allow_local_files,
                        char *out, size_t cap) {
    studio_req req;
    if (!parse_query_request(body, len, &req)) {
        otsardb_cli_json_error("malformed request: expected a JSON object of "
                             "string fields (target and/or bucket+database, "
                             "sql)", out, cap);
        return -1;
    }
    if (!req.has_sql) {
        otsardb_cli_json_error("missing \"sql\" field", out, cap);
        return 0;
    }
    run_options opt;
    memset(&opt, 0, sizeof(opt));
    char count_sql[OTSARDB_STUDIO_MAX_SQL];
    if (req.has_page_size) {
        /* Server-side paging (v0.4): page_size present AND
         * the statement is the plain SELECT * FROM <ident> shape -> the
         * statement is rewritten to SELECT * FROM "t" LIMIT n OFFSET m and
         * COUNT(*) is run on the same session (the response gains
         * ,"total":N). Any other shape ignores the paging fields and keeps
         * the exact --json payload (client-side paging). */
        long page = 1, page_size = 200;
        if (req.has_page) {
            char *end = NULL;
            long v = strtol(req.page, &end, 10);
            if (end && *end == '\0' && v >= 1) page = v;
        }
        {
            char *end = NULL;
            long v = strtol(req.page_size, &end, 10);
            if (end && *end == '\0' && v >= 1) page_size = v;
            if (page_size > 10000) page_size = 10000;
        }
        char paged[OTSARDB_STUDIO_MAX_SQL];
        if (otsardb_studio_paged_sql(req.sql, page, page_size, 0, paged,
                                   sizeof(paged)) == 0 &&
            otsardb_studio_paged_sql(req.sql, page, page_size, 1, count_sql,
                                   sizeof(count_sql)) == 0) {
            snprintf(req.sql, sizeof(req.sql), "%s", paged);
            opt.paged = 1;
            opt.count_sql = count_sql;
        }
    }
    return run_request(&req, &opt, allow_local_files, out, cap);
}

/* Executes one /api/schema request: same connection
 * contract as /api/query; the "sql" field is optional and ignored. The
 * response is the --json-schema payload (ok or error object); the v0.4
 * "with_sql" field appends the verbatim stored CREATE statement to every
 * object. */
static int handle_schema(const char *body, size_t len, int allow_local_files,
                         char *out, size_t cap) {
    studio_req req;
    if (!parse_query_request(body, len, &req)) {
        otsardb_cli_json_error("malformed request: expected a JSON object of "
                             "string fields (target and/or bucket+database)",
                             out, cap);
        return -1;
    }
    run_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.schema_mode = 1;
    return run_request(&req, &opt, allow_local_files, out, cap);
}

/* Executes one /api/edit request: the server GENERATES
 * the per-row UPDATE/DELETE/INSERT SQL from the structured edit request
 * (table/op/cols/pk/old/new), then runs it through the same pipeline as
 * /api/query. The response is the --json payload of the generated
 * statement (ok or error object); malformed bodies -> 400. */
static int handle_edit(const char *body, size_t len, int allow_local_files,
                       char *out, size_t cap) {
    studio_req req;
    if (!parse_query_request(body, len, &req)) {
        otsardb_cli_json_error("malformed request: expected a JSON object of "
                             "string fields (target and/or bucket+database, "
                             "table, op, cols, pk, old, new)", out, cap);
        return -1;
    }
    if (!req.has_edit_table || !req.has_edit_op || req.n_edit_cols < 1) {
        otsardb_cli_json_error("edit request needs \"table\", \"op\" and "
                             "\"cols\"", out, cap);
        return 0;
    }

    /* Wrap the request storage in the pure generator's view. */
    const char *cols[OTSARDB_EDIT_MAX_COLS];
    const char *pk[OTSARDB_EDIT_MAX_COLS];
    otsardb_studio_cell old_row[OTSARDB_EDIT_MAX_COLS];
    otsardb_studio_cell new_row[OTSARDB_EDIT_MAX_COLS];
    for (int i = 0; i < req.n_edit_cols; ++i) {
        cols[i] = req.edit_cols[i];
        old_row[i].is_null = req.edit_old[i].is_null;
        old_row[i].is_number = req.edit_old[i].is_number;
        old_row[i].text = req.edit_old[i].text;
        new_row[i].is_null = req.edit_new[i].is_null;
        new_row[i].is_number = req.edit_new[i].is_number;
        new_row[i].text = req.edit_new[i].text;
    }
    for (int i = 0; i < req.n_edit_pk; ++i) pk[i] = req.edit_pk[i];

    otsardb_studio_edit edit;
    memset(&edit, 0, sizeof(edit));
    edit.table = req.edit_table;
    edit.op = req.edit_op;
    edit.cols = cols;
    edit.n_cols = req.n_edit_cols;
    edit.pk = pk;
    edit.n_pk = req.n_edit_pk;
    edit.old_row = old_row;
    edit.n_old = req.n_edit_old;
    edit.new_row = new_row;
    edit.n_new = req.n_edit_new;

    char sql[OTSARDB_STUDIO_MAX_SQL];
    if (otsardb_studio_edit_sql(&edit, sql, sizeof(sql)) != 0) {
        otsardb_cli_json_error(sql, out, cap);
        return 0;
    }
    if (strlen(sql) >= sizeof(req.sql)) {
        otsardb_cli_json_error("generated edit SQL exceeds the request cap",
                             out, cap);
        return 0;
    }
    snprintf(req.sql, sizeof(req.sql), "%s", sql);
    req.has_sql = 1;
    run_options opt;
    memset(&opt, 0, sizeof(opt));
    return run_request(&req, &opt, allow_local_files, out, cap);
}

/* ------------------------------------------------------------- health v3
 *
 * POST /api/health: the server executes the 
 * readiness probe IN-PROCESS per request — the same read-only open
 * (OTSARDB_S3_SINGLE_WRITER=1 forced in the env window: no lease claim, no
 * takeover, no writes) + SELECT count(*) FROM sqlite_schema + close — and
 * responds with the --health JSON contract extended with the monitor
 * fields: "lease" (read-only peek of <root>/lease.json via the store
 * client: state/owner/generation/address), "single_writer" (the AMBIENT
 * OTSARDB_S3_SINGLE_WRITER before the probe forces it — the connection's
 * real write posture) and "last_error" (the server's most recent failed
 * request message; empty when none). The 0061 base fields are emitted
 * byte-identical to `otsardb --health`. */

/* Monotonic wall-clock ms (same pattern as main.c). */
static unsigned long long studio_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

/* Most recent failed-request message (single-threaded server: no locks). */
static char g_studio_last_error[1024] = "";

/* Lease liveness constants mirrored from src/ha/lease.h (
 * contract): the default TTL and the conservative expiry skew. Kept local
 * so the CLI layer does not depend on the src/ha include path. */
#define OTSARDB_STUDIO_LEASE_TTL_DEFAULT_SECS 30u
#define OTSARDB_STUDIO_LEASE_SKEW_MARGIN_SECS 5

/* Ambient single-writer posture: 1 when OTSARDB_S3_SINGLE_WRITER is set to a
 * truthy value (1/true/yes/on, same rule as s3_database.cpp env_true). */
static int env_truthy_ambient(const char *name) {
    const char *value = getenv(name);
    if (!value || !value[0]) return 0;
    char low[16] = {0};
    for (size_t i = 0; value[i] && i + 1 < sizeof(low); ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        low[i] = c;
    }
    return strcmp(low, "1") == 0 || strcmp(low, "true") == 0 ||
           strcmp(low, "yes") == 0 || strcmp(low, "on") == 0;
}

/* Parses the lease.json shape {"owner":"...","generation":N,"address":"..."}
 * (address optional). Mirrors the field extraction rules of lease.c. */
static int lease_parse_field(const char *json, size_t size, const char *key,
                             char *out, size_t out_cap) {
    out[0] = '\0';
    const char *found = strstr(json, key);
    if (!found) return 0;
    const char *open = strchr(found + strlen(key), '"');
    if (!open) return 0;
    const char *start = open + 1;
    const char *close = strchr(start, '"');
    if (!close || close >= json + size) return 0;
    size_t n = (size_t)(close - start);
    if (n >= out_cap) return 0;
    memcpy(out, start, n);
    out[n] = '\0';
    return 1;
}

static int lease_parse_generation(const char *json, size_t size,
                                  unsigned long long *out) {
    const char *found = strstr(json, "\"generation\"");
    if (!found) return 0;
    const char *p = found + strlen("\"generation\"");
    while (*p == ':' || *p == ' ' || *p == '\t') ++p;
    if (*p == ' ') ++p;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return 0;
    if ((size_t)(end - json) > size) return 0;
    *out = v;
    return 1;
}

/* Read-only lease peek: <database_root>/lease.json via the store client
 * (the same store code the engine uses; never claims, never writes).
 * Fills lease_json with {"state":"absent"} / {"state":"leased",...} /
 * {"state":"error","error":...}. */
#ifdef OTSARDB_ENABLE_S3
static void studio_lease_peek(const char *target, char *lease_json,
                              size_t cap) {
    otsardb_s3_target parsed;
    if (!otsardb_s3_target_parse(target, &parsed)) {
        snprintf(lease_json, cap, "{\"state\":\"error\",\"error\":\"cannot "
                 "parse target for the lease peek\"}");
        return;
    }
    char key[OTSARDB_KEY_MAX + 32];
    if (snprintf(key, sizeof(key), "%s/lease.json", parsed.database_root) < 0) {
        snprintf(lease_json, cap,
                 "{\"state\":\"error\",\"error\":\"lease key too long\"}");
        return;
    }
    /* The request env window is already applied (endpoint/region/keys):
     * build the store config from the ambient process env exactly like
     * s3_database.cpp configure_store does. */
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *region = getenv("OTSARDB_S3_REGION");
    if (!region || !region[0]) region = getenv("AWS_REGION");
    if (!region || !region[0]) region = getenv("AWS_DEFAULT_REGION");
    if (!region || !region[0]) region = "us-east-1";
    const char *access_key = getenv("OTSARDB_S3_ACCESS_KEY");
    if (!access_key || !access_key[0]) access_key = getenv("AWS_ACCESS_KEY_ID");
    const char *secret_key = getenv("OTSARDB_S3_SECRET_KEY");
    if (!secret_key || !secret_key[0]) secret_key = getenv("AWS_SECRET_ACCESS_KEY");
    const char *prefix = getenv("OTSARDB_S3_PREFIX");

    otsardb_s3_config config;
    memset(&config, 0, sizeof(config));
    config.endpoint = endpoint && endpoint[0] ? endpoint : "https://s3.amazonaws.com";
    config.region = region;
    config.bucket = parsed.bucket;
    config.access_key = access_key && access_key[0] ? access_key : NULL;
    config.secret_key = secret_key && secret_key[0] ? secret_key : NULL;
    config.prefix = prefix && prefix[0] ? prefix : NULL;
    config.use_https = 1;

    otsardb_object_store *store = otsardb_s3_store_create(&config);
    if (!store) {
        snprintf(lease_json, cap, "{\"state\":\"error\",\"error\":\"cannot "
                 "create the S3 client for the lease peek\"}");
        return;
    }
    otsardb_store_object object;
    memset(&object, 0, sizeof(object));
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) {
        snprintf(lease_json, cap, "{\"state\":\"absent\"}");
        otsardb_s3_store_destroy(store);
        return;
    }
    if (rc != OTSARDB_STORE_OK) {
        snprintf(lease_json, cap, "{\"state\":\"error\",\"error\":\"lease "
                 "read failed (store rc %d)\"}", (int)rc);
        otsardb_s3_store_destroy(store);
        return;
    }

    char owner[128] = {0};
    char address[256] = {0};
    unsigned long long generation = 0;
    int parsed_ok = lease_parse_field((const char *)object.data, object.size,
                                      "\"owner\"", owner, sizeof(owner)) &&
                    owner[0] &&
                    lease_parse_generation((const char *)object.data,
                                           object.size, &generation);
    int64_t last_modified = object.last_modified_secs;
    if (!parsed_ok) {
        snprintf(lease_json, cap, "{\"state\":\"error\",\"error\":\"lease "
                 "object is not valid OtsarDB lease JSON\"}");
    } else {
        /* TTL from the same env knob s3_database.cpp uses; expired once the
         * age reaches ttl - skew (the lease.c liveness rule). */
        unsigned long long ttl = OTSARDB_STUDIO_LEASE_TTL_DEFAULT_SECS;
        const char *ttl_env = getenv("OTSARDB_LEASE_TTL");
        if (ttl_env && ttl_env[0]) {
            unsigned long long parsed_ttl = strtoull(ttl_env, NULL, 10);
            if (parsed_ttl > 0) ttl = parsed_ttl;
        }
        int expired = 0;
        if (last_modified > 0) {
            int64_t age = (int64_t)time(NULL) - last_modified;
            if (age < 0) age = 0;
            int64_t window = (int64_t)ttl - OTSARDB_STUDIO_LEASE_SKEW_MARGIN_SECS;
            if (window < 0) window = 0;
            expired = age >= window;
        }
        sbuf b = {0, 0, cap};
        int ok = 1;
        ok &= sbuf_puts(&b, "{\"state\":\"leased\",\"owner\":") == 0;
        ok &= sbuf_js_string(&b, owner, strlen(owner)) == 0;
        char num[32];
        snprintf(num, sizeof(num), ",\"generation\":%llu",
                 (unsigned long long)generation);
        ok &= sbuf_puts(&b, num) == 0;
        if (address[0]) {
            ok &= sbuf_puts(&b, ",\"address\":") == 0;
            ok &= sbuf_js_string(&b, address, strlen(address)) == 0;
        }
        ok &= sbuf_printf(&b, ",\"last_modified\":%lld",
                          (long long)last_modified) == 0;
        ok &= sbuf_printf(&b, ",\"expired\":%s}", expired ? "true" : "false") == 0;
        if (ok) {
            memcpy(lease_json, b.data, b.len + 1);
        } else {
            snprintf(lease_json, cap,
                     "{\"state\":\"error\",\"error\":\"lease output cap "
                     "exceeded\"}");
        }
        free(b.data);
    }
    otsardb_store_release_object(store, &object);
    otsardb_s3_store_destroy(store);
}
#endif /* OTSARDB_ENABLE_S3 */

/* The 0061 readiness probe executed per request (studio.c copy of
 * main.c's run_health_s3; the CLI copy stays byte-identical). Sets
 * status/role/s3_reachable/open_ms and fills error_msg on failure. */
#ifdef OTSARDB_ENABLE_S3
static void studio_health_probe_s3(const char *target, unsigned long long *open_ms,
                                   const char **status_out,
                                   int *s3_reachable_out,
                                   const char **error_out,
                                   char *error_buf, size_t error_buf_cap) {
    unsigned long long t0 = studio_now_ms();
    *s3_reachable_out = 1;
    otsardb *db = 0;
    int rc = otsardb_s3_open_target_ex(target, NULL, &db);
    if (rc != OTSARDB_OK) {
        const char *msg = otsardb_s3_last_error();
        *status_out = "error";
        /* s3_reachable classification (same strings as main.c): only the
         * two known store-unreachable classes report false. */
        if (msg &&
            (strstr(msg, "failed to initialize the S3 client") ||
             strstr(msg, "failed the OtsarDB S3 Core capability probe"))) {
            *s3_reachable_out = 0;
        }
        snprintf(error_buf, error_buf_cap, "%s",
                 msg && msg[0] ? msg : "failed to open OtsarDB S3 target");
        *error_out = error_buf;
        *open_ms = studio_now_ms() - t0;
        return;
    }
    char *exec_error = 0;
    int exec_rc = otsardb_exec(db, "SELECT count(*) FROM sqlite_schema;", 0, 0,
                             &exec_error);
    if (exec_error) {
        snprintf(error_buf, error_buf_cap, "%s", exec_error);
        otsardb_free(exec_error);
    } else if (exec_rc != OTSARDB_OK && otsardb_errmsg(db)) {
        snprintf(error_buf, error_buf_cap, "%s", otsardb_errmsg(db));
    }
    unsigned long long elapsed = studio_now_ms() - t0;
    otsardb_close(db);
    if (exec_rc == OTSARDB_OK) {
        *status_out = "ok";
        *error_out = NULL;
    } else {
        *status_out = "degraded";
        *error_out = error_buf[0] ? error_buf: "read probe failed";
    }
    *open_ms = elapsed;
}
#endif

/* Emits the /api/health response: the 0061 fields byte-identical plus the
 * lease/single_writer/last_error extensions. */
static void studio_emit_health(char *out, size_t cap, const char *status,
                               const char *role, int s3_reachable,
                               unsigned long long open_ms, const char *target,
                               const char *error_msg, const char *lease_json,
                               int single_writer) {
    sbuf b = {0, 0, cap};
    int ok = 1;
    ok = ok && sbuf_puts(&b, "{\"status\":") == 0;
    ok = ok && sbuf_js_string(&b, status, strlen(status)) == 0;
    ok = ok && sbuf_puts(&b, ",\"role\":") == 0;
    ok = ok && sbuf_js_string(&b, role, strlen(role)) == 0;
    ok = ok && sbuf_printf(&b, ",\"s3_reachable\":%s",
                           s3_reachable ? "true" : "false") == 0;
    ok = ok && sbuf_printf(&b, ",\"open_ms\":%llu", open_ms) == 0;
    ok = ok && sbuf_puts(&b, ",\"engine\":") == 0;
    ok = ok && sbuf_js_string(&b, otsardb_engine_version(),
                              strlen(otsardb_engine_version())) == 0;
    ok = ok && sbuf_puts(&b, ",\"target\":") == 0;
    ok = ok && sbuf_js_string(&b, target ? target : "",
                              target ? strlen(target) : 0) == 0;
    if (ok && error_msg && error_msg[0]) {
        ok = ok && sbuf_puts(&b, ",\"error\":") == 0;
        ok = ok && sbuf_js_string(&b, error_msg, strlen(error_msg)) == 0;
    }
    ok = ok && sbuf_puts(&b, ",\"lease\":") == 0;
    ok = ok && sbuf_puts(&b, lease_json ? lease_json : "{\"state\":\"absent\"}") == 0;
    ok = ok && sbuf_printf(&b, ",\"single_writer\":%s",
                           single_writer ? "true" : "false") == 0;
    ok = ok && sbuf_puts(&b, ",\"last_error\":") == 0;
    ok = ok && sbuf_js_string(&b, g_studio_last_error,
                              strlen(g_studio_last_error)) == 0;
    ok = ok && sbuf_puts(&b, "}\n") == 0;
    if (ok) {
        size_t len = b.len < cap ? b.len : cap - 1;
        memcpy(out, b.data, len);
        out[len] = '\0';
        free(b.data);
        return;
    }
    free(b.data);
    /* The only acceptable cap violation is a minimal error object. */
    otsardb_cli_json_error("--health output cap exceeded", out, cap);
}

/* POST /api/health handler. Returns 0 (200) or -1 (400 malformed). */
static int handle_health(const char *body, size_t len, char *out, size_t cap) {
    studio_req req;
    if (!parse_query_request(body, len, &req)) {
        otsardb_cli_json_error("malformed request: expected a JSON object of "
                             "string fields (target and/or bucket+database)",
                             out, cap);
        return -1;
    }
    char target[2048];
    if (req.has_target) {
        snprintf(target, sizeof(target), "%s", req.target);
    } else if (req.has_bucket && req.has_database) {
        snprintf(target, sizeof(target), "s3://%s/%s", req.bucket,
                 req.database);
    } else {
        studio_emit_health(out, cap, "error", "unknown", 0, 0, "",
                           "missing target: provide \"target\" or "
                           "\"bucket\"+\"database\"",
                           "{\"state\":\"absent\"}",
                           env_truthy_ambient("OTSARDB_S3_SINGLE_WRITER"));
        snprintf(g_studio_last_error, sizeof(g_studio_last_error),
                 "missing target: provide \"target\" or \"bucket\"+\"database\"");
        return 0;
    }

    /* The connection's real write posture: the AMBIENT single-writer env
     * BEFORE the probe forces OTSARDB_S3_SINGLE_WRITER=1 for its own read-only
     * open. */
    int single_writer = env_truthy_ambient("OTSARDB_S3_SINGLE_WRITER");

    /* The probe's own env window (0061): connection fields + forced
     * single-writer (read-only; never claims the lease). */
    env_override overrides[5] = {
        {"OTSARDB_S3_ENDPOINT", {0}, 0, 0},
        {"OTSARDB_S3_REGION", {0}, 0, 0},
        {"OTSARDB_S3_ACCESS_KEY", {0}, 0, 0},
        {"OTSARDB_S3_SECRET_KEY", {0}, 0, 0},
        {"OTSARDB_S3_SINGLE_WRITER", {0}, 0, 0},
    };
    apply_override(&overrides[0], req.has_endpoint ? req.endpoint : NULL);
    apply_override(&overrides[1], req.has_region ? req.region : NULL);
    apply_override(&overrides[2], req.has_access_key ? req.access_key : NULL);
    apply_override(&overrides[3], req.has_secret_key ? req.secret_key : NULL);
    apply_override(&overrides[4], "1");

    if (!is_s3_target(target)) {
        studio_emit_health(out, cap, "error", "unknown", 0, 0, target,
                           "health is only defined for s3:// (or "
                           "otsardb+s3://) targets",
                           "{\"state\":\"absent\"}", single_writer);
        snprintf(g_studio_last_error, sizeof(g_studio_last_error),
                 "health is only defined for s3:// (or otsardb+s3://) targets");
        for (int k = 0; k < 5; ++k) env_restore(&overrides[k]);
        return 0;
    }

    char lease_json[1024] = "{\"state\":\"absent\"}";
    const char *status = "error";
    const char *error_msg = NULL;
    char error_buf[1024] = {0};
    unsigned long long open_ms = 0;
    int s3_reachable = 0;
#ifdef OTSARDB_ENABLE_S3
    studio_health_probe_s3(target, &open_ms, &status, &s3_reachable,
                           &error_msg, error_buf, sizeof(error_buf));
    if (strcmp(status, "ok") == 0) {
        studio_lease_peek(target, lease_json, sizeof(lease_json));
    } else {
        /* The probe itself failed: the lease state was not observed. */
        snprintf(lease_json, sizeof(lease_json),
                 "{\"state\":\"error\",\"error\":\"probe failed; lease "
                 "state not observed\"}");
    }
#else
    error_msg = "this OtsarDB binary was built without native S3 support; "
                "rebuild with OTSARDB_ENABLE_S3=ON";
#endif
    /* Role (honest per 0061 invariant 4): with a live foreign lease
     * observed the monitor is a candidate (it could take over via an
     * expiry-proven claim); leader is unreachable (the probe never
     * claims); otherwise unknown. */
    const char *role = "unknown";
    if (status && strcmp(status, "ok") == 0) {
        if (strstr(lease_json, "\"state\":\"leased\"") &&
            !strstr(lease_json, "\"expired\":true")) {
            role = "candidate";
        }
    }
    if (error_msg) {
        snprintf(g_studio_last_error, sizeof(g_studio_last_error), "%s",
                 error_msg);
    } else {
        g_studio_last_error[0] = '\0';
    }
    studio_emit_health(out, cap, status, role, s3_reachable, open_ms, target,
                       error_msg, lease_json, single_writer);
    for (int k = 0; k < 5; ++k) env_restore(&overrides[k]);
    return 0;
}

/* ------------------------------------------------------- connection loop */

/* CSRF / loopback-origin gate for state-changing requests (
 * audit F01). Returns 1 when the request passed (caller continues), 0 when
 * a gate response was already sent (caller must return). The gate runs
 * before the body is read or dispatched, so the per-request endpoint/keys
 * override — the signed-request delegation vector of F01 — can only ever
 * reach run_request after this gate passed. */
static int studio_csrf_gate(otsardb_studio_server *server, studio_socket fd,
                            const char *header_zone) {
    char expected_host[64];
    snprintf(expected_host, sizeof(expected_host), "127.0.0.1:%d",
             server->port);
    char expected_origin[96];
    snprintf(expected_origin, sizeof(expected_origin), "http://127.0.0.1:%d",
             server->port);

    char value[512];
    if (!header_value(header_zone, "Host", value, sizeof(value)) ||
        !ascii_ci_str_eq(value, expected_host)) {
        send_response(fd, 403, "Forbidden", "text/plain",
                      "forbidden: Host must be 127.0.0.1:<port>");
        return 0;
    }
    if (header_value(header_zone, "Origin", value, sizeof(value)) &&
        !ascii_ci_str_eq(value, expected_origin)) {
        send_response(fd, 403, "Forbidden", "text/plain",
                      "forbidden: cross-origin requests are not allowed");
        return 0;
    }
    if (!header_value(header_zone, "X-OTSARDB-CSRF", value, sizeof(value)) ||
        !csrf_token_equal(value, server->csrf_token)) {
        send_response(fd, 403, "Forbidden", "text/plain",
                      "forbidden: missing or invalid X-OTSARDB-CSRF token");
        return 0;
    }
    if (!header_value(header_zone, "Content-Type", value, sizeof(value)) ||
        !content_type_is_json(value)) {
        send_response(fd, 415, "Unsupported Media Type", "text/plain",
                      "unsupported media type: Content-Type must be "
                      "application/json");
        return 0;
    }
    return 1;
}

/* Serves the embedded UI with the per-process CSRF token injected (0136):
 * <script>window.OTSARDB_CSRF_TOKEN="<hex>";</script> just before </head>,
 * so the UI's fetch wrapper can stamp every POST /api/* request. */
static void serve_index(otsardb_studio_server *server, studio_socket fd) {
    const char *head = strstr(studio_index_html, "</head>");
    if (!head) {
        send_response(fd, 200, "OK", "text/html; charset=utf-8",
                      studio_index_html);
        return;
    }
    size_t pre = (size_t)(head - studio_index_html);
    size_t suffix_len = strlen(head);
    char *html = (char *)malloc(pre + OTSARDB_STUDIO_CSRF_HEX + 96 +
                                suffix_len + 1);
    if (!html) {
        send_response(fd, 500, "Internal Server Error", "text/plain",
                      "out of memory");
        return;
    }
    memcpy(html, studio_index_html, pre);
    int n = snprintf(html + pre, OTSARDB_STUDIO_CSRF_HEX + 96,
                     "<script>window.OTSARDB_CSRF_TOKEN=\"%s\";</script>",
                     server->csrf_token);
    if (n > 0 && (size_t)n < OTSARDB_STUDIO_CSRF_HEX + 96) {
        strcpy(html + pre + (size_t)n, head);
        send_response(fd, 200, "OK", "text/html; charset=utf-8", html);
    } else {
        send_response(fd, 500, "Internal Server Error", "text/plain",
                      "internal error: CSRF token injection failed");
    }
    free(html);
}

/* One request on one connection. The connection is always closed. */
static void handle_connection(otsardb_studio_server *server, studio_socket fd) {
    /* Heap buffers: the request buffer alone exceeds the default 1 MiB
     * Windows thread stack. */
    size_t request_cap = OTSARDB_STUDIO_MAX_HEADERS + OTSARDB_STUDIO_MAX_BODY + 1;
    char *request = (char *)malloc(request_cap);
    char *response = (char *)malloc(OTSARDB_JSON_OUT_CAP);
    if (!request || !response) {
        send_response(fd, 500, "Internal Server Error", "text/plain",
                      "out of memory");
        free(request);
        free(response);
        return;
    }
    size_t body_offset = 0;
    size_t used = 0;
    if (!read_request_headers(fd, request, OTSARDB_STUDIO_MAX_HEADERS,
                              &body_offset, &used)) {
        send_response(fd, 400, "Bad Request", "text/plain",
                      "bad request: malformed headers");
        free(request);
        free(response);
        return;
    }

    char saved_body_byte = request[body_offset];
    request[body_offset] = '\0';

    char method[16] = {0};
    char path[256] = {0};
    if (!parse_request_line(request, method, sizeof(method), path,
                            sizeof(path))) {
        send_response(fd, 400, "Bad Request", "text/plain",
                      "bad request: unparsable request line");
        free(request);
        free(response);
        return;
    }

    const char *header_zone = find_bytes(request, body_offset, "\r\n", 2);
    header_zone = header_zone ? header_zone + 2 : request;

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0) {
            serve_index(server, fd);
        } else if (strcmp(path, "/health") == 0) {
            send_response(fd, 200, "OK", "text/plain", "ok");
        } else if (strcmp(path, "/api/csrf") == 0) {
            char body[96];
            int n = snprintf(body, sizeof(body), "{\"token\":\"%s\"}\n",
                             server->csrf_token);
            if (n > 0 && (size_t)n < sizeof(body)) {
                send_response(fd, 200, "OK", "application/json", body);
            } else {
                send_response(fd, 500, "Internal Server Error", "text/plain",
                              "internal error: token serialization failed");
            }
        } else {
            send_response(fd, 404, "Not Found", "text/plain", "not found");
        }
        free(request);
        free(response);
        return;
    }
    if (strcmp(method, "POST") != 0) {
        send_response(fd, 405, "Method Not Allowed", "text/plain",
                      "method not allowed");
        free(request);
        free(response);
        return;
    }
    /* CSRF gate (audit F01): before path routing, body
     * reads and dispatch, so a cross-site page cannot execute SQL and
     * cannot delegate signed S3 requests through the endpoint/keys
     * override. GET endpoints are not state-changing and stay open. */
    if (!studio_csrf_gate(server, fd, header_zone)) {
        free(request);
        free(response);
        return;
    }
    int is_schema = strcmp(path, "/api/schema") == 0;
    int is_edit = strcmp(path, "/api/edit") == 0;
    int is_health = strcmp(path, "/api/health") == 0;
    if (!is_schema && !is_edit && !is_health &&
        strcmp(path, "/api/query") != 0) {
        send_response(fd, 404, "Not Found", "text/plain", "not found");
        free(request);
        free(response);
        return;
    }

    long content_length = parse_content_length(header_zone);
    if (content_length < 0) {
        send_response(fd, 400, "Bad Request", "application/json",
                      "{\"ok\":false,\"error\":\"bad request: missing or "
                      "invalid Content-Length\"}");
        free(request);
        free(response);
        return;
    }
    if ((unsigned long)content_length > OTSARDB_STUDIO_MAX_BODY) {
        send_response(fd, 400, "Bad Request", "application/json",
                      "{\"ok\":false,\"error\":\"bad request: body exceeds "
                      "the 1 MiB cap\"}");
        free(request);
        free(response);
        return;
    }
    request[body_offset] = saved_body_byte;

    size_t have = used > body_offset ? used - body_offset : 0;
    if (have > (size_t)content_length) have = (size_t)content_length;
    size_t remaining = (size_t)content_length - have;
    while (remaining > 0) {
        if (!wait_readable(fd, OTSARDB_STUDIO_READ_TIMEOUT_MS)) {
            send_response(fd, 408, "Request Timeout", "text/plain",
                          "request timeout: body was not received");
            free(request);
            free(response);
            return;
        }
        int n = recv(fd, request + body_offset + have, (int)remaining, 0);
        if (n <= 0) {
            send_response(fd, 400, "Bad Request", "text/plain",
                          "bad request: truncated body");
            free(request);
            free(response);
            return;
        }
        remaining -= (size_t)n;
        have += (size_t)n;
    }
    request[body_offset + (size_t)content_length] = '\0';

    response[0] = '\0';
    int rc = 0;
    int allow_local_files = server->allow_local_files;
    if (is_health) {
        rc = handle_health(request + body_offset, (size_t)content_length,
                           response, OTSARDB_JSON_OUT_CAP);
    } else if (is_edit) {
        rc = handle_edit(request + body_offset, (size_t)content_length,
                         allow_local_files, response, OTSARDB_JSON_OUT_CAP);
    } else if (is_schema) {
        rc = handle_schema(request + body_offset, (size_t)content_length,
                           allow_local_files, response, OTSARDB_JSON_OUT_CAP);
    } else {
        rc = handle_query(request + body_offset, (size_t)content_length,
                          allow_local_files, response, OTSARDB_JSON_OUT_CAP);
    }
    if (!is_health) {
        /* Keep the monitor's last_error current for query/schema/edit
         * failures too: record the response's error message (the --json
         * error shape), clear it on success. */
        const char *err_key = strstr(response, "\"error\":\"");
        if (err_key) {
            size_t o = 0;
            const char *p = err_key + strlen("\"error\":\"");
            while (*p && o + 1 < sizeof(g_studio_last_error)) {
                if (*p == '\\' && p[1] == '"') {
                    if (o + 1 < sizeof(g_studio_last_error)) {
                        g_studio_last_error[o++] = '"';
                    }
                    p += 2;
                    continue;
                }
                if (*p == '"') break;
                g_studio_last_error[o++] = *p++;
            }
            g_studio_last_error[o] = '\0';
        } else if (strncmp(response, "{\"ok\":true", 10) == 0) {
            g_studio_last_error[0] = '\0';
        }
    }
    if (rc == 0) {
        send_response(fd, 200, "OK", "application/json", response);
    } else {
        send_response(fd, 400, "Bad Request", "application/json", response);
    }
    free(request);
    free(response);
}

#if defined(_WIN32)
static unsigned int __stdcall studio_server_main(void *arg) {
#else
static void *studio_server_main(void *arg) {
#endif
    otsardb_studio_server *server = (otsardb_studio_server *)arg;
    while (!server->stop) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server->listen_fd, &read_fds);
        struct timeval poll_timeout;
        poll_timeout.tv_sec = 0;
        poll_timeout.tv_usec = 200000; /* 200 ms: bounds the stop latency */
        int ready = select((int)server->listen_fd + 1, &read_fds, NULL, NULL,
                           &poll_timeout);
        if (ready <= 0 || server->stop) continue;
        studio_socket conn = accept(server->listen_fd, NULL, NULL);
        if (conn == STUDIO_INVALID_SOCKET) continue;
        handle_connection(server, conn);
        studio_socket_close(conn);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

otsardb_studio_server *otsardb_studio_server_start_ex(int port,
                                                   int allow_local_files,
                                                   char *error,
                                                   size_t error_cap) {
    if (error && error_cap) error[0] = '\0';
    if (port < 0 || port > 65535) {
        if (error && error_cap) snprintf(error, error_cap, "invalid port");
        return NULL;
    }
    if (!ensure_wsa()) {
        if (error && error_cap)
            snprintf(error, error_cap, "socket initialization failed");
        return NULL;
    }

    otsardb_studio_server *server =
        (otsardb_studio_server *)calloc(1, sizeof(*server));
    if (!server) {
        if (error && error_cap) snprintf(error, error_cap, "out of memory");
        return NULL;
    }
    server->listen_fd = STUDIO_INVALID_SOCKET;
    server->allow_local_files = allow_local_files;

    /* Per-process CSRF token (audit F01): generated before
     * the thread starts, so every request sees it; RNG failure fails the
     * server start (a server without the token cannot be secured). */
    if (!studio_csrf_generate(server->csrf_token, sizeof(server->csrf_token))) {
        if (error && error_cap)
            snprintf(error, error_cap, "cannot generate the CSRF token");
        free(server);
        return NULL;
    }

    /* Loopback-only: the endpoint executes arbitrary SQL with no
     * authentication; it must never bind a non-loopback interface (same
     * boundary as the forward server). */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo *resolved = NULL;
    if (getaddrinfo("127.0.0.1", port_str, &hints, &resolved) != 0 ||
        !resolved) {
        if (error && error_cap)
            snprintf(error, error_cap, "cannot resolve 127.0.0.1");
        free(server);
        return NULL;
    }

    studio_socket listen_fd = STUDIO_INVALID_SOCKET;
    for (struct addrinfo *ai = resolved; ai; ai = ai->ai_next) {
        listen_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd == STUDIO_INVALID_SOCKET) continue;
#if !defined(_WIN32)
        int reuse = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (bind(listen_fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        studio_socket_close(listen_fd);
        listen_fd = STUDIO_INVALID_SOCKET;
    }
    freeaddrinfo(resolved);
    if (listen_fd == STUDIO_INVALID_SOCKET) {
        if (error && error_cap)
            snprintf(error, error_cap,
                     "cannot bind 127.0.0.1:%d (in use?)", port);
        free(server);
        return NULL;
    }
    if (listen(listen_fd, 4) != 0) {
        if (error && error_cap)
            snprintf(error, error_cap, "listen failed (socket error %d)",
                     STUDIO_LAST_SOCKET_ERROR());
        studio_socket_close(listen_fd);
        free(server);
        return NULL;
    }

    server->listen_fd = listen_fd;
    server->port = port;
    if (port == 0) {
        struct sockaddr_storage bound;
        studio_socklen bound_len = (studio_socklen)sizeof(bound);
        if (getsockname(listen_fd, (struct sockaddr *)&bound, &bound_len) == 0) {
            server->port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
        }
    }

#if defined(_WIN32)
    server->thread = _beginthreadex(NULL, 0, studio_server_main, server, 0,
                                    NULL);
    if (!server->thread) {
        if (error && error_cap)
            snprintf(error, error_cap, "cannot start the server thread");
        studio_socket_close(listen_fd);
        free(server);
        return NULL;
    }
#else
    server->thread_running =
        pthread_create(&server->thread, NULL, studio_server_main, server) == 0;
    if (!server->thread_running) {
        if (error && error_cap)
            snprintf(error, error_cap, "cannot start the server thread");
        studio_socket_close(listen_fd);
        free(server);
        return NULL;
    }
#endif
    return server;
}

/* 3-argument form: the pre-0147 signature, with the
 * local-file opt-in OFF (the F08 default). Kept so the embedded test
 * servers and existing callers keep their exact call shape. */
otsardb_studio_server *otsardb_studio_server_start(int port, char *error,
                                               size_t error_cap) {
    return otsardb_studio_server_start_ex(port, 0, error, error_cap);
}

int otsardb_studio_server_port(const otsardb_studio_server *server) {
    return server ? server->port : 0;
}
void otsardb_studio_server_stop(otsardb_studio_server *server) {
    if (!server) return;
    server->stop = 1;
    if (server->listen_fd != STUDIO_INVALID_SOCKET) {
        studio_socket_close(server->listen_fd);
        server->listen_fd = STUDIO_INVALID_SOCKET;
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

/* ------------------------------------------------------------ CLI entry */

/* ------------------------------------------------------ window hook v0.6
 * The desktop window implementation lives in src/cli/studio_window.c,
 * linked only into the otsardb executable (and, when wired, the window unit
 * test). The studio unit-test targets link studio.c WITHOUT it, so the
 * call below is weakly bound:
 * - MSVC: the linker /alternatename directive resolves
 * otsardb_studio_window_open to the local fallback when studio_window.c
 * is absent; when it IS linked its definition wins.
 * - GCC/Clang: the weak reference stays NULL when studio_window.c is
 * absent and the runtime check picks the fallback.
 * The binary therefore always links and runs without the window component
 * — and without WebView2 at all (the loader DLL is found and loaded at
 * runtime, never at link time). */
#if !defined(_MSC_VER)
extern int otsardb_studio_window_open(int port, char *error, size_t error_cap)
    __attribute__((weak));
#else
int otsardb_studio_window_open(int port, char *error, size_t error_cap);
#pragma comment(linker, "/alternatename:otsardb_studio_window_open=" \
                        "otsardb_studio_window_open_fallback")
#endif

/* Non-static on purpose: the MSVC /alternatename target must be a
 * linker-visible symbol. */
int otsardb_studio_window_open_fallback(int port, char *error, size_t error_cap) {
    (void)port;
    if (error && error_cap) {
        snprintf(error, error_cap,
                 "this build does not include the desktop window component "
                 "(src/cli/studio_window.c is not linked)");
    }
    return 1;
}

static int studio_window_open_call(int port, char *error, size_t error_cap) {
#if defined(_MSC_VER)
    return otsardb_studio_window_open(port, error, error_cap);
#else
    if (otsardb_studio_window_open) {
        return otsardb_studio_window_open(port, error, error_cap);
    }
    return otsardb_studio_window_open_fallback(port, error, error_cap);
#endif
}

#if defined(_WIN32)
static volatile LONG studio_stop = 0;
static BOOL WINAPI studio_ctrl_handler(DWORD fdwCtrlType) {
    (void)fdwCtrlType;
    InterlockedExchange(&studio_stop, 1);
    return TRUE;
}
#else
static volatile sig_atomic_t studio_stop = 0;
static void studio_sigint_handler(int sig) {
    (void)sig;
    studio_stop = 1;
}
#endif

int otsardb_studio_serve_ex(int port, int window_mode,
                            int allow_local_files) {
    char error[256];
    otsardb_studio_server *server =
        otsardb_studio_server_start_ex(port, allow_local_files, error,
                                       sizeof(error));
    if (!server) {
        fprintf(stderr, "error: studio: %s\n", error);
        return 1;
    }
    int actual = otsardb_studio_server_port(server);
    printf("OtsarDB Studio v0.7 (OtsarDB %s)\n", otsardb_version());
    printf("Studio server listening on http://127.0.0.1:%d\n", actual);
    printf("Security: loopback-only, no authentication, per-process CSRF "
           "token required on every POST /api/* request (Origin/Host "
           "validated, Content-Type enforced). Do not "
           "expose this port (same boundary as --forward-port).\n");
    printf("Targets: s3:// (otsardb+s3://) and :memory: always; local-file "
           "paths are %s (audit F08).\n",
           allow_local_files ? "ENABLED (--local-files)"
                             : "DISABLED by default (use --local-files to "
                               "workbench local paths)");
    fflush(stdout);

    /* Desktop window mode (v0.6): when requested, host the
     * embedded UI in a native WebView2 window instead of a browser tab.
     * otsardb_studio_window_open returns 0 when the window ran until the
     * user closed it (the server then stops and the CLI exits cleanly);
     * any other return prints the exact reason and falls back to the v0.5
     * default-browser behavior. */
    if (window_mode) {
        char werr[256];
        int wrc = studio_window_open_call(actual, werr, sizeof(werr));
        if (wrc == 0) {
            printf("Desktop window closed; stopping the server.\n");
            otsardb_studio_server_stop(server);
            printf("OtsarDB Studio stopped.\n");
            return 0;
        }
        printf("Desktop window unavailable: %s\n", werr);
        printf("Falling back to the default browser.\n");
        fflush(stdout);
    }
    printf("Opening the default browser at http://127.0.0.1:%d/ ...\n",
           actual);
    fflush(stdout);
    {
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", actual);
        studio_open_browser(url);
    }
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);

#if defined(_WIN32)
    SetConsoleCtrlHandler(studio_ctrl_handler, TRUE);
#else
    signal(SIGINT, studio_sigint_handler);
#endif

    while (!studio_stop) {
#if defined(_WIN32)
        Sleep(200);
#else
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000;
        nanosleep(&ts, NULL);
#endif
    }
    otsardb_studio_server_stop(server);
    printf("OtsarDB Studio stopped.\n");
    return 0;
}

int otsardb_studio_serve(int port) {
    return otsardb_studio_serve_ex(port, 0, 0);
}

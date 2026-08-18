/* json_mode.c — the OtsarDB CLI --json output mode .
 *
 * The executor prepares/steps each statement on the SAME sqlite3
 * connection the CLI opened for the target
 * (otsardb_internal_sqlite_handle), i.e. the same engine path as
 * otsardb_exec: same VFS, WAL publisher, lease and durability machinery.
 * It cannot use the row-callback API (sqlite3_exec) because the JSON
 * cell-typing rule (numbers by SQLite affinity) needs
 * sqlite3_column_type() per value, which callbacks never expose.
 *
 * Output is NDJSON: one object per statement result, one object per
 * line. Emitted shapes (see json_mode.h):
 *
 * {"ok":true,"rows":N,"columns":[...],"data":[[...],[...]]}
 * {"ok":false,"error":"..."}
 *
 * All emission is hand-rolled (no JSON library, dependency-free) and
 * bounded: per-statement payload cap OTSARDB_JSON_MAX_BYTES, per-statement
 * row cap OTSARDB_JSON_MAX_ROWS, total output cap OTSARDB_JSON_OUT_CAP.
 * Exceeding a cap fails the statement with an explicit error object
 * (fail-closed), never silent truncation.
 */

#include "json_mode.h"
#include "otsardb_internal.h"
#include "sqlite3.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ buf */

/* Growable byte buffer; every append is bounds-checked against the cap. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} jbuf;

static int buf_reserve(jbuf *b, size_t need) {
    if (need > b->cap) return -1;
    if (b->data) {
        /* CLI-1 (bug hunt 0131): the cap is a hard fail-closed budget.
         * An append that would push len past cap must fail — the old
         * growth loop broke on its first iteration and returned success
         * with the buffer un-grown, and buf_appendn then memcpy'd past
         * the heap allocation. */
        return b->len <= b->cap - need ? 0 : -1;
    }
    if (b->cap == SIZE_MAX) return -1; /* defensive cap+1 wrap guard */
    char *data = (char *)realloc(NULL, b->cap + 1);
    if (!data) return -1;
    b->data = data;
    return 0;
}

static int buf_appendn(jbuf *b, const char *s, size_t n) {
    if (buf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_puts(jbuf *b, const char *s) {
    return buf_appendn(b, s, strlen(s));
}

static int buf_printf(jbuf *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return buf_appendn(b, tmp, (size_t)n);
}

static void buf_reset(jbuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void buf_free(jbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* ---------------------------------------------------------------- escape */

/* Appends `s` (n bytes) as a JSON string value (quotes included), escaping
 * quotes, backslashes and control characters. Bytes >= 0x20 pass through
 * under the UTF-8 assumption (documented limitation for BLOB content). */
static int append_json_string(jbuf *b, const char *s, size_t n) {
    if (buf_puts(b, "\"") != 0) return -1; /* opening quote */
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  if (buf_puts(b, "\\\"") != 0) return -1; break;
        case '\\': if (buf_puts(b, "\\\\") != 0) return -1; break;
        default:
            if (c < 0x20) {
                char tmp[8];
                int k = snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                if (k <= 0 || buf_appendn(b, tmp, (size_t)k) != 0) return -1;
            } else if (buf_appendn(b, (const char *)&c, 1) != 0) {
                return -1;
            }
        }
    }
    return buf_puts(b, "\""); /* closing quote */
}

/* RFC 8259 number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
 * Used as the "unambiguous number" guard for INTEGER/FLOAT text (rejects
 * Inf/NaN and any non-canonical rendering). */
static int is_json_number(const char *s) {
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

/* ------------------------------------------------------------ statement */

/* Appends one data cell: NULL -> null; INTEGER/FLOAT with valid number
 * text -> raw JSON number; everything else -> escaped string. */
static int append_cell(jbuf *b, sqlite3_stmt *stmt, int col) {
    int type = sqlite3_column_type(stmt, col);
    if (type == SQLITE_NULL) return buf_puts(b, "null");
    const char *text = (const char *)sqlite3_column_text(stmt, col);
    if ((type == SQLITE_INTEGER || type == SQLITE_FLOAT) && text &&
        is_json_number(text)) {
        return buf_puts(b, text);
    }
    size_t n = text ? (size_t)sqlite3_column_bytes(stmt, col) : 0;
    return append_json_string(b, text ? text : "", n);
}

/* Truncates out back to stmt_start and appends {"ok":false,"error":...}.
 * If even the error object cannot fit, the whole output is replaced by it
 * (the only acceptable output-cap violation is the error object itself). */
static void emit_stmt_error(jbuf *out, size_t stmt_start, const char *message) {
    char buf[512];
    if (!message || !message[0]) message = "unknown error";
    out->len = stmt_start;
    if (out->data) out->data[stmt_start] = '\0';
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":");
    if (buf_puts(out, buf) != 0) goto cap_out;
    if (append_json_string(out, message, strlen(message)) != 0) goto cap_out;
    if (buf_puts(out, "}\n") != 0) goto cap_out;
    return;
cap_out:
    buf_reset(out);
    buf_puts(out, "{\"ok\":false,\"error\":\"--json output cap exceeded\"}\n");
}

/* Executes one compiled statement and appends its result object to out.
 * Per-statement budgets: rows <= OTSARDB_JSON_MAX_ROWS and payload <=
 * OTSARDB_JSON_MAX_BYTES (and never more than the run's remaining cap).
 * Returns 0 on success, -1 on any failure (the error object is emitted). */
static int run_statement(sqlite3 *h, sqlite3_stmt *stmt, jbuf *out,
                         size_t out_cap) {
    size_t stmt_start = out->len;
    int ncols = sqlite3_column_count(stmt);
    int nrows = 0;

    size_t remaining = out_cap > out->len ? out_cap - out->len : 0;
    size_t stmt_budget = remaining < OTSARDB_JSON_MAX_BYTES ? remaining
                                                          : OTSARDB_JSON_MAX_BYTES;
    jbuf cols = {0, 0, stmt_budget};
    jbuf rows = {0, 0, stmt_budget};

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (nrows >= OTSARDB_JSON_MAX_ROWS) {
            emit_stmt_error(out, stmt_start,
                            "result set exceeds the --json row cap (100000); "
                            "narrow the query");
            sqlite3_finalize(stmt);
            buf_free(&cols);
            buf_free(&rows);
            return -1;
        }
        if (buf_puts(&rows, nrows ? ",[" : "[") != 0) { rc = 0; break; }
        int cell_ok = 1;
        for (int i = 0; i < ncols; ++i) {
            if (i > 0 && buf_puts(&rows, ",") != 0) { cell_ok = 0; break; }
            if (append_cell(&rows, stmt, i) != 0) { cell_ok = 0; break; }
        }
        if (!cell_ok || buf_puts(&rows, "]") != 0) { rc = 0; break; }
        ++nrows;
    }
    if (rc == 0) { /* buffer overflow while building a row */
        emit_stmt_error(out, stmt_start,
                        "result set exceeds the --json payload cap (8 MiB); "
                        "narrow the query");
        sqlite3_finalize(stmt);
        buf_free(&cols);
        buf_free(&rows);
        return -1;
    }

    if (rc != SQLITE_DONE) {
        emit_stmt_error(out, stmt_start, sqlite3_errmsg(h));
        sqlite3_finalize(stmt);
        buf_free(&cols);
        buf_free(&rows);
        return -1;
    }

    /* Compose {"ok":true,"rows":N,"columns":[...],"data":[...]}\n */
    if (rows.len + cols.len + 256 > stmt_budget) {
        emit_stmt_error(out, stmt_start,
                        "result set exceeds the --json payload cap (8 MiB); "
                        "narrow the query");
        sqlite3_finalize(stmt);
        buf_free(&cols);
        buf_free(&rows);
        return -1;
    }
    int ok = 1;
    if (buf_puts(out, "{\"ok\":true,\"rows\":") == 0 &&
        buf_printf(out, "%d", nrows) == 0 &&
        buf_puts(out, ",\"columns\":[") == 0) {
        for (int i = 0; i < ncols; ++i) {
            const char *name = sqlite3_column_name(stmt, i);
            if (i > 0 && buf_puts(out, ",") != 0) { ok = 0; break; }
            if (append_json_string(out, name ? name : "",
                                   name ? strlen(name) : 0) != 0) {
                ok = 0;
                break;
            }
        }
        if (ok &&
            buf_puts(out, "],\"data\":[") == 0 &&
            buf_appendn(out, rows.data ? rows.data : "", rows.len) == 0 &&
            buf_puts(out, "]}\n") == 0) {
            ok = 1;
        } else {
            ok = 0;
        }
    } else {
        ok = 0;
    }
    sqlite3_finalize(stmt);
    buf_free(&cols);
    buf_free(&rows);
    if (!ok) {
        emit_stmt_error(out, stmt_start,
                        "output exceeds the --json output cap (32 MiB); "
                        "narrow the query");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ api */

int otsardb_cli_exec_json(otsardb *db, const char *sql, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 1;
    out[0] = '\0';
    if (!db || !sql) {
        otsardb_cli_json_error("OtsarDB handle or SQL text missing", out, out_cap);
        return 1;
    }
    sqlite3 *h = otsardb_internal_sqlite_handle(db);
    if (!h) {
        otsardb_cli_json_error("OtsarDB handle is not open", out, out_cap);
        return 1;
    }

    jbuf outbuf = {0, 0, out_cap};
    if (buf_reserve(&outbuf, 4096) != 0) {
        otsardb_cli_json_error("out of memory", out, out_cap);
        return 1;
    }

    const char *tail = sql;
    int failed = 0;
    while (tail && *tail) {
        sqlite3_stmt *stmt = NULL;
        const char *next = NULL;
        int prc = sqlite3_prepare_v2(h, tail, -1, &stmt, &next);
        if (prc != SQLITE_OK) {
            emit_stmt_error(&outbuf, outbuf.len, sqlite3_errmsg(h));
            failed = 1;
            break;
        }
        if (!stmt) break; /* only whitespace/comments remain */
        tail = next;
        if (run_statement(h, stmt, &outbuf, out_cap) != 0) {
            failed = 1;
            break;
        }
    }

    if (outbuf.len >= out_cap) {
        /* CLI-2: an exact cap fill cannot be NUL-terminated in an
         * out_cap-byte caller buffer without truncating content — fail
         * closed with the explicit error object instead. */
        buf_reset(&outbuf);
        (void)buf_puts(&outbuf,
                       "{\"ok\":false,\"error\":\"--json output cap exceeded\"}\n");
        failed = 1;
    }
    size_t len = outbuf.len;
    memcpy(out, outbuf.data, len);
    out[len] = '\0';
    buf_free(&outbuf);
    return failed ? 1 : 0;
}

void otsardb_cli_json_error(const char *message, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    jbuf b = {0, 0, out_cap};
    if (buf_reserve(&b, 128) != 0) return;
    if (buf_puts(&b, "{\"ok\":false,\"error\":") == 0) {
        (void)append_json_string(&b, message ? message : "",
                                 message ? strlen(message) : 0);
        (void)buf_puts(&b, "}\n");
    }
    size_t len = b.len;
    if (len >= out_cap) {
        /* CLI-2: an exact cap fill cannot be NUL-terminated without
         * truncating content — fail closed with the explicit error
         * object instead. */
        buf_reset(&b);
        (void)buf_puts(&b, "{\"ok\":false,\"error\":\"--json output cap exceeded\"}\n");
        len = b.len;
    }
    if (len >= out_cap) len = out_cap - 1; /* last-resort bounds clamp */
    memcpy(out, b.data, len);
    out[len] = '\0';
    buf_free(&b);
}

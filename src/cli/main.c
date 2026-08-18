#include "otsardb.h"
#include "json_mode.h"
#include "studio.h"
#include "sqlite3.h"

#ifdef OTSARDB_ENABLE_S3
#include "forward.h"
#include "s3_database.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static volatile int interrupted = 0;
static BOOL WINAPI ctrl_handler(DWORD fdwCtrlType) {
    (void)fdwCtrlType;
    interrupted = 1;
    return TRUE;
}
#else
#include <signal.h>
static volatile sig_atomic_t interrupted = 0;
static void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
}
#endif

static int echo_mode = 0;
static int last_row_count = 0;
static double last_query_time = 0.0;
/* Health/readiness mode: set by --health / --health-json. */
static int health_mode = 0;
/* Machine-readable exec mode: set by --json. Emits one
 * JSON object per statement result on stdout (see json_mode.h). */
static int json_mode = 0;
/* Machine-readable schema mode: set by --json-schema.
 * Emits one {"ok":true,"tables":[...]} object built from sqlite_schema +
 * PRAGMA table_info/index_list/index_info (see studio.h). */
static int json_schema_mode = 0;

/* Monotonic wall-clock milliseconds for the readiness report. */
static unsigned long long now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}

static void json_escape_f(FILE *f, const char *s) {
    if (!s) return;
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
            else fputc(*p, f);
        }
    }
}

/* Emits the one-line machine-readable readiness report. Contract (
 * 0061, docs/AVAILABILITY_MODEL.md Level C):
 * {"status":"ok|degraded|error","role":"leader|candidate|unknown",
 * "s3_reachable":true|false,"open_ms":N,"engine":"<version>",
 * "target":"<target>"[,"error":"<message>"]}
 * status=ok only after a valid open + read succeeded; role is the lease
 * role when the public API can know it (see run_health_s3 for the honest
 * limitation); s3_reachable reflects the open+probe round trip. */
static void emit_health_json(const char *status, const char *role,
                             int s3_reachable, unsigned long long open_ms,
                             const char *target, const char *error_msg) {
    printf("{\"status\":\"%s\",\"role\":\"%s\",\"s3_reachable\":%s,"
           "\"open_ms\":%llu,\"engine\":\"",
           status, role, s3_reachable ? "true" : "false", open_ms);
    json_escape_f(stdout, otsardb_engine_version());
    fputs("\",\"target\":\"", stdout);
    json_escape_f(stdout, target ? target : "");
    fputs("\"", stdout);
    if (error_msg && error_msg[0]) {
        fputs(",\"error\":\"", stdout);
        json_escape_f(stdout, error_msg);
        fputs("\"", stdout);
    }
    fputs("}\n", stdout);
}

#ifdef OTSARDB_ENABLE_S3
/* Health/readiness probe (HA plan P3): opens the target
 * WITHOUT writing. OTSARDB_S3_SINGLE_WRITER=1 is forced for the probe so the
 * open skips the lease-claim block entirely: the writer lease is never
 * claimed, no takeover can be triggered, no lease thread keeps the probe
 * alive, and the database root is never written. The only store writes
 * possible are the standard capability-probe objects (first probe per
 * endpoint, outside the database root), identical to any other open.
 *
 * Role limitation (honest): with the read-only open the lease state is
 * never observed, so the JSON role field is always "unknown" today. The
 * public API exposes no lease peek: otsardb_s3_leader_address is only
 * populated by a candidate open, and a candidate open claims the lease
 * when it is free — a write a readiness probe must not perform. The
 * schema keeps "leader" and "candidate" for when a lease-read accessor
 * exists (recorded in the availability model risks). */
static int run_health_s3(const char *target) {
    unsigned long long t0 = now_ms();
    char error[1024] = {0};

#ifdef _WIN32
    _putenv_s("OTSARDB_S3_SINGLE_WRITER", "1");
#else
    setenv("OTSARDB_S3_SINGLE_WRITER", "1", 1);
#endif

    otsardb *db = 0;
    int rc = otsardb_s3_open_target_ex(target, NULL, &db);
    if (rc != OTSARDB_OK) {
        const char *msg = otsardb_s3_last_error();
        /* s3_reachable comes from the open+probe result: only the two
         * known store-unreachable failure classes report false; every
         * failure after the probe (identity, restore, lease) means the
         * store answered. String matching is deliberate and kept next to
         * the messages it depends on (s3_database.cpp configure_store). */
        int reachable = 1;
        if (msg &&
            (strstr(msg, "failed to initialize the S3 client") ||
             strstr(msg, "failed the OtsarDB S3 Core capability probe"))) {
            reachable = 0;
        }
        if (msg && msg[0]) snprintf(error, sizeof(error), "%s", msg);
        else snprintf(error, sizeof(error), "failed to open OtsarDB S3 target");
        emit_health_json("error", "unknown", reachable, now_ms() - t0, target, error);
        return 2;
    }

    /* Read probe: a real SQLite read through the materialized image. With
     * lazy hydration the read exercises the S3 fetch path for any page
     * outside the checkpoint base, so a sticky hydration error surfaces
     * as degraded (ready-for-read = false). */
    char *exec_error = 0;
    int exec_rc = otsardb_exec(db, "SELECT count(*) FROM sqlite_schema;", 0, 0, &exec_error);
    if (exec_error) {
        snprintf(error, sizeof(error), "%s", exec_error);
        otsardb_free(exec_error);
    } else if (exec_rc != OTSARDB_OK && otsardb_errmsg(db)) {
        snprintf(error, sizeof(error), "%s", otsardb_errmsg(db));
    }
    unsigned long long elapsed = now_ms() - t0;
    otsardb_close(db);

    if (exec_rc == OTSARDB_OK) {
        emit_health_json("ok", "unknown", 1, elapsed, target, NULL);
        return 0;
    }
    emit_health_json("degraded", "unknown", 1, elapsed, target,
                     error[0] ? error : "read probe failed");
    return 1;
}
#endif

static int is_s3_target(const char *target) {
    return target && (strncmp(target, "s3://", 5) == 0 || strncmp(target, "otsardb+s3://", 11) == 0);
}

static int print_row(void *ctx, int argc, char **argv, char **columns) {
    (void)ctx;
    (void)columns;
    for (int i = 0; i < argc; ++i) {
        if (i) fputs(" | ", stdout);
        fputs(argv[i] ? argv[i] : "NULL", stdout);
    }
    fputc('\n', stdout);
    return 0;
}

/* --- M2 write forwarding helpers ---
 * The CLI attempts every statement locally first. When the local attempt
 * fails (BUSY in candidate mode) and the statement is NOT one of the
 * small read-only prefixes (SELECT/PRAGMA/EXPLAIN, whitespace-insensitive),
 * it forwards the statement to the lease leader over HTTP. Read-only SQL
 * never needs forwarding (reads execute locally on any session). */
static int ci_eq_prefix(const char *text, const char *prefix) {
    size_t i = 0;
    while (prefix[i]) {
        char a = text[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return 0;
        ++i;
    }
    return 1;
}

static int ident_follows(const char *text, size_t n) {
    char c = text[n];
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_');
}

static int is_read_only_sql(const char *sql) {
    while (*sql == ' ' || *sql == '\t' || *sql == '\r' || *sql == '\n') ++sql;
    if (ci_eq_prefix(sql, "select") && ident_follows(sql, 6)) return 1;
    if (ci_eq_prefix(sql, "pragma") && ident_follows(sql, 6)) return 1;
    if (ci_eq_prefix(sql, "explain") && ident_follows(sql, 7)) return 1;
    return 0;
}

/* Prints a forwarded 200 response: each row line as-is, then consumes the
 * "# end <N>" trailing marker for the row count. */
static int forward_print_response(const char *body, int *rows_out) {
    int rows = 0;
    const char *line = body;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        int is_marker = (len >= 6 && strncmp(line, "# end ", 6) == 0);
        if (is_marker) {
            rows = atoi(line + 6);
        } else {
            fwrite(line, 1, len, stdout);
            fputc('\n', stdout);
        }
        if (!nl) break;
        line = nl + 1;
    }
    if (rows_out) *rows_out = rows;
    return 0;
}

#ifdef OTSARDB_ENABLE_S3
/* Best-effort forwarding of a locally failed (BUSY) write statement to the
 * lease leader. Returns 0 when the leader executed it (rows already
 * printed), 1 when forwarding was not possible/attempted or failed. The
 * caller then surfaces the original BUSY error. */
static int try_forward_write(otsardb *db, const char *sql, int show_timing,
                             double elapsed_seconds, const char *local_error) {
    if (is_read_only_sql(sql)) { return 1; }

    /* A commit that hit the BUSY fence leaves the local transaction in an
     * aborted state; roll it back so the connection stays usable for reads
     * and later local statements. No-op when nothing is pending. */
    char *rollback_error = 0;
    otsardb_exec(db, "ROLLBACK", 0, 0, &rollback_error);
    otsardb_free(rollback_error);

    char leader[512] = {0};
    if (!otsardb_s3_leader_address(db, leader, sizeof(leader))) return 1;

    char *response = (char *)malloc(OTSARDB_FORWARD_MAX_BODY + 1);
    if (!response) return 1;
    int status = 0;
    int rc = otsardb_forward_client_post(leader,
                                       sql,
                                       response,
                                       OTSARDB_FORWARD_MAX_BODY + 1,
                                       &status,
                                       OTSARDB_FORWARD_TIMEOUT_MS);
    if (rc == 0 && status == 200) {
        int rows = 0;
        forward_print_response(response, &rows);
        last_row_count = rows;
        last_query_time = elapsed_seconds;
        if (show_timing) {
            printf("Query OK, %d row%s, %.3fs (forwarded to %s)\n",
                   rows, rows == 1 ? "" : "s", elapsed_seconds, leader);
        }
        free(response);
        return 0;
    }
    if (rc != 0) {
        fprintf(stderr, "note: leader %s is unreachable; write was not "
                        "executed anywhere\n", leader);
    } else if (status == 421) {
        fprintf(stderr, "note: leader %s no longer holds the lease (421); "
                        "write was not executed\n", leader);
    } else {
        fprintf(stderr, "note: leader %s refused the write (HTTP %d): %s\n",
                leader, status, response[0] ? response : "(no body)");
    }
    fprintf(stderr, "error: %s\n", local_error);
    free(response);
    return 1;
}
#endif

static void usage(const char *program) {
    fprintf(stderr,
        "OtsarDB %s\nUsage:\n  %s <database>\n  %s <database> \"SQL\"\n  %s --version\n"
        "  %s studio\n\n"
        "Options:\n  -e, --echo    Print SQL before executing\n"
        "  --json <target> \"SQL\"  Print one JSON object per statement result on stdout\n"
        "                        (rows/columns/data or error; exit 0/1; incompatible with\n"
        "                        --echo, --forward-port and REPL mode)\n"
        "  --json-schema <target> Print one JSON object describing the schema on stdout\n"
        "                        ({\"ok\":true,\"tables\":[{name,type,columns,pk,notnull,\n"
        "                        indexes}]}; built from sqlite_schema + PRAGMA\n"
        "                        table_info/index_list/index_info; exit 0/1/2; no SQL\n"
        "                        argument; same incompatibilities as --json)\n"
        "  --port <port>         Studio port override (only valid with `otsardb studio`;\n"
        "                        wins over OTSARDB_STUDIO_PORT; 1..65535)\n"
        "  --window              Studio desktop window mode (only valid with `otsardb studio`;\n"
        "                        hosts the UI in a native WebView2 window when the WebView2\n"
        "                        loader is available, otherwise falls back to the browser;\n"
        "                        OTSARDB_STUDIO_WINDOW=1 is the environment equivalent)\n"
        "  --local-files         Studio local-file workbenching opt-in (only valid with\n"
        "                        `otsardb studio`): by default the Studio API accepts only\n"
        "                        s3:// (otsardb+s3://) and :memory: targets and REFUSES\n"
        "                        local paths (audit F08); --local-files re-enables them;\n"
        "                        OTSARDB_STUDIO_ALLOW_LOCAL=1 is the environment equivalent)\n"
        "  studio [--port <port>] [--window] [--local-files]\n"
        "                        Start the loopback web workbench on http://127.0.0.1:<port>\n"
        "                        (--port wins; OTSARDB_STUDIO_PORT fallback; default 8717;\n"
        "                        loopback-only, no auth; opens the default browser — with\n"
        "                        --window a native WebView2 desktop window when available)\n"
        "  --health <target>     Emit one-line machine-readable readiness JSON and exit\n"
        "                        without keeping a session open (exit 0 = ok, 1 = degraded, 2 = error)\n"
        "  --health-json <target> Alias for --health\n"
        "  --forward-port <port>    S3 leader mode: serve HTTP write forwarding on 127.0.0.1:<port>\n"
        "  --forward-to <host:port> S3 candidate mode: forward write SQL to this leader\n"
        "                           (default: the address published in the lease)\n\n"
        "Targets:\n  ./file.db                  local database\n  :memory:                   in-memory database\n"
        "  s3://bucket/database       native authoritative S3 storage\n\n"
        "S3 configuration:\n"
        "  OTSARDB_S3_ENDPOINT          optional; defaults to AWS S3\n"
        "  OTSARDB_S3_REGION            optional; AWS_REGION/AWS_DEFAULT_REGION fallback\n"
        "  OTSARDB_S3_ACCESS_KEY        or AWS_ACCESS_KEY_ID\n"
        "  OTSARDB_S3_SECRET_KEY        or AWS_SECRET_ACCESS_KEY\n"
        "  OTSARDB_S3_SESSION_TOKEN     or AWS_SESSION_TOKEN (optional)\n"
        "  OTSARDB_S3_PREFIX            optional global object prefix\n"
        "  OTSARDB_CACHE_DIR            optional disposable local execution directory\n"
        "  OTSARDB_S3_FORWARD_PORT      same as --forward-port\n"
        "  OTSARDB_S3_FORWARD_TO        same as --forward-to\n"
        "  OTSARDB_STUDIO_PORT          same as the studio port\n"
        "  OTSARDB_STUDIO_WINDOW        same as `otsardb studio --window` (1/true/yes/on)\n"
        "  OTSARDB_STUDIO_ALLOW_LOCAL   same as `otsardb studio --local-files` (1/true/yes/on)\n",
        otsardb_version(), program, program, program, program);
}

typedef struct {
    otsardb_row_callback inner;
    void *inner_ctx;
    int row_count;
} exec_context;

static int counting_callback(void *ctx, int argc, char **argv, char **columns) {
    exec_context *ec = (exec_context *)ctx;
    ec->row_count++;
    if (ec->inner) return ec->inner(ec->inner_ctx, argc, argv, columns);
    return 0;
}

static int execute(otsardb *db, const char *sql, int show_timing) {
    if (echo_mode) printf("-- %s\n", sql);

    exec_context ctx = { print_row, 0, 0 };
    clock_t start = clock();

    char *error = 0;
    int rc = otsardb_exec(db, sql, counting_callback, &ctx, &error);

    clock_t elapsed = clock() - start;
    double seconds = (double)elapsed / CLOCKS_PER_SEC;

    if (rc != OTSARDB_OK) {
        char message[512] = {0};
        snprintf(message, sizeof(message), "%s",
                 error ? error : otsardb_errmsg(db));
        int handled = 0;
#ifdef OTSARDB_ENABLE_S3
        handled = try_forward_write(db, sql, show_timing, seconds, message) == 0;
#endif
        if (!handled) fprintf(stderr, "error: %s\n", message);
        otsardb_free(error);
        return handled ? 0 : 1;
    }

    last_row_count = ctx.row_count;
    last_query_time = seconds;

    if (show_timing) {
        printf("Query OK, %d row%s, %.3fs\n",
               ctx.row_count, ctx.row_count == 1 ? "" : "s", seconds);
    }
    return 0;
}

static int meta_command(otsardb *db, const char *line, const char *target) {
    if (strcmp(line, ".quit\n") == 0 || strcmp(line, ".exit\n") == 0 ||
        strcmp(line, ".quit") == 0 || strcmp(line, ".exit") == 0) return -1;
    if (strncmp(line, ".help", 5) == 0) {
        puts(".help              Show this help");
        puts(".tables            List tables");
        puts(".schema            Show schema");
        puts(".status            Database status and last query info");
        puts(".clear             Clear screen and statement buffer");
        puts(".version           Show OtsarDB and engine versions");
        puts(".quit / .exit      Exit OtsarDB");
        return 1;
    }
    if (strncmp(line, ".tables", 7) == 0) {
        execute(db, "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;", 0);
        return 1;
    }
    if (strncmp(line, ".schema", 7) == 0) {
        execute(db, "SELECT sql FROM sqlite_schema WHERE sql IS NOT NULL ORDER BY name;", 0);
        return 1;
    }
    if (strncmp(line, ".status", 7) == 0) {
        printf("Connected to: %s\n", target ? target : "(unknown)");
        printf("Engine:       %s\n", otsardb_engine_version());
        printf("Last query:   %d row%s, %.3fs\n",
               last_row_count, last_row_count == 1 ? "" : "s", last_query_time);
        return 1;
    }
    if (strncmp(line, ".version", 8) == 0) {
        printf("OtsarDB %s (engine base %s)\n", otsardb_version(), otsardb_engine_version());
        return 1;
    }
    if (line[0] == '.') {
        fprintf(stderr, "unknown command: %s", line);
        return 1;
    }
    return 0;
}

static int repl(otsardb *db, const char *target) {
    char line[4096];
    char *statement = 0;
    size_t statement_len = 0;

    printf("OtsarDB %s (engine base %s)\n", otsardb_version(), otsardb_engine_version());
    puts("Enter SQL terminated with ';'. Enter '.help' for commands.");

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sigint_handler);
#endif

    for (;;) {
        if (interrupted) {
            fputs("\nInterrupt\n", stderr);
            interrupted = 0;
            free(statement);
            statement = 0;
            statement_len = 0;
            clearerr(stdin);
            continue;
        }

        fputs(statement_len ? "   ...> " : "otsardb> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            if (interrupted) {
                interrupted = 0;
                free(statement);
                statement = 0;
                statement_len = 0;
                clearerr(stdin);
                fputc('\n', stdout);
                continue;
            }
            break;
        }

        if (strncmp(line, ".clear", 6) == 0) {
            fputs("\033[2J\033[H", stdout);
            fflush(stdout);
            free(statement);
            statement = 0;
            statement_len = 0;
            continue;
        }

        if (statement_len == 0 && line[0] == '.') {
            int meta = meta_command(db, line, target);
            if (meta < 0) break;
            if (meta > 0) continue;
        }

        if (interrupted) {
            interrupted = 0;
            free(statement);
            statement = 0;
            statement_len = 0;
            clearerr(stdin);
            continue;
        }

        size_t line_len = strlen(line);
        char *next = (char *)realloc(statement, statement_len + line_len + 1);
        if (!next) {
            free(statement);
            fputs("error: out of memory\n", stderr);
            return 1;
        }
        statement = next;
        memcpy(statement + statement_len, line, line_len + 1);
        statement_len += line_len;

        if (sqlite3_complete(statement)) {
            execute(db, statement, 1);
            free(statement);
            statement = 0;
            statement_len = 0;
        }
    }

    free(statement);
    return 0;
}

int main(int argc, char **argv) {
    /* M2 write forwarding options: flags win over the
     * OTSARDB_S3_FORWARD_* environment variables. Consumed arguments are
     * marked so they are never treated as the target or as SQL text. */
    int forward_port = 0;
    char forward_to[256] = {0};
    /* Studio --port override: 0 = not given; only valid
     * with `otsardb studio`; wins over OTSARDB_STUDIO_PORT. */
    int studio_port_flag = 0;
    /* Studio desktop window mode (v0.6): 0 = not given;
     * only valid with `otsardb studio`; OTSARDB_STUDIO_WINDOW=1 is the env
     * fallback. */
    int studio_window_flag = 0;
    /* Studio local-file workbenching opt-in (audit F08):
     * 0 = not given; only valid with `otsardb studio`;
     * OTSARDB_STUDIO_ALLOW_LOCAL=1 is the env fallback. 0 keeps the Studio
     * API closed to non-s3:// (and ":memory:") targets; 1 restores the
     * pre-0147 behavior where a loopback client could open local paths. */
    int studio_local_files_flag = 0;
    unsigned char *consumed = (unsigned char *)calloc((size_t)argc, 1);
    if (!consumed) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("otsardb %s (engine base %s)\n", otsardb_version(), otsardb_engine_version());
            free(consumed);
            return 0;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--echo") == 0) {
            echo_mode = 1;
            consumed[i] = 1;
            continue;
        }
        if (strcmp(argv[i], "--json") == 0) {
            /* Machine-readable exec mode: one JSON object
             * per statement result on stdout. See src/cli/json_mode.h for
             * the contract and bounds. */
            json_mode = 1;
            consumed[i] = 1;
            continue;
        }
        if (strcmp(argv[i], "--json-schema") == 0) {
            /* Machine-readable schema mode: one
             * {"ok":true,"tables":[...]} object describing the database
             * schema (tables/views -> columns with type/pk -> indexes),
             * emitted by the shared executor in studio.c. */
            json_schema_mode = 1;
            consumed[i] = 1;
            continue;
        }
        if (strcmp(argv[i], "--health") == 0 || strcmp(argv[i], "--health-json") == 0) {
            health_mode = 1;
            consumed[i] = 1;
            continue;
        }
        if (strcmp(argv[i], "--forward-port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--forward-port requires a port argument\n");
                usage(argv[0]);
                free(consumed);
                return 2;
            }
            char *end = 0;
            long parsed = strtol(argv[i + 1], &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 65535) {
                fprintf(stderr, "--forward-port requires a port in 1..65535, got '%s'\n",
                        argv[i + 1]);
                free(consumed);
                return 2;
            }
            forward_port = (int)parsed;
            consumed[i] = consumed[i + 1] = 1;
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--forward-to") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--forward-to requires a host:port argument\n");
                usage(argv[0]);
                free(consumed);
                return 2;
            }
            snprintf(forward_to, sizeof(forward_to), "%s", argv[i + 1]);
            consumed[i] = consumed[i + 1] = 1;
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--port") == 0) {
            /* Studio port override: `otsardb studio
             * --port <port>`; wins over OTSARDB_STUDIO_PORT. Rejected
             * without the studio mode below (exit 2). */
            if (i + 1 >= argc) {
                fprintf(stderr, "--port requires a port argument\n");
                usage(argv[0]);
                free(consumed);
                return 2;
            }
            char *end = 0;
            long parsed = strtol(argv[i + 1], &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 65535) {
                fprintf(stderr,
                        "--port requires a port in 1..65535, got '%s'\n",
                        argv[i + 1]);
                free(consumed);
                return 2;
            }
            studio_port_flag = (int)parsed;
            consumed[i] = consumed[i + 1] = 1;
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--window") == 0) {
            /* Studio desktop window mode (v0.6): `otsardb
             * studio --window` hosts the UI in a native WebView2 window
             * (falling back to the browser when the WebView2 loader is
             * absent); rejected without the studio mode below (exit 2). */
            studio_window_flag = 1;
            consumed[i] = 1;
            continue;
        }
        if (strcmp(argv[i], "--local-files") == 0) {
            /* Studio local-file workbenching opt-in (
             * audit F08): `otsardb studio --local-files` re-enables local
             * file targets (the default refuses every non-s3:// target
             * except ":memory:"); rejected without the studio mode below
             * (exit 2). */
            studio_local_files_flag = 1;
            consumed[i] = 1;
            continue;
        }
    }

    if (forward_port == 0) {
        const char *env_port = getenv("OTSARDB_S3_FORWARD_PORT");
        if (env_port && env_port[0]) {
            char *end = 0;
            long parsed = strtol(env_port, &end, 10);
            if (end && *end == '\0' && parsed >= 1 && parsed <= 65535) {
                forward_port = (int)parsed;
            }
        }
    }
    if (!forward_to[0]) {
        const char *env_to = getenv("OTSARDB_S3_FORWARD_TO");
        if (env_to && env_to[0]) snprintf(forward_to, sizeof(forward_to), "%s", env_to);
    }

    /* --json is incompatible with the echo flag and with forward-port
     * serving (the forwarded text protocol has no JSON form; write
     * forwarding is disabled under --json by design). The
     * --json-schema mode (0073) shares the same incompatibilities. */
    if (json_mode && json_schema_mode) {
        char js[1024];
        otsardb_cli_json_error("--json cannot be combined with --json-schema",
                             js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }
    if ((json_mode || json_schema_mode) && echo_mode) {
        char js[1024];
        otsardb_cli_json_error("--json/--json-schema cannot be combined with "
                             "--echo", js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }
    if ((json_mode || json_schema_mode) && forward_port != 0) {
        char js[1024];
        otsardb_cli_json_error("--json/--json-schema cannot be combined with "
                             "--forward-port", js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }

    int target_idx = -1;
    for (int i = 1; i < argc; ++i) {
        if (!consumed[i]) {
            target_idx = i;
            break;
        }
    }

    /* Health/readiness mode (HA plan P3): machine-readable
     * status for orchestrators (Swarm/K8s restart policies). Always JSON,
     * always exits without a session, and never writes. Usage/argument
     * failures also emit JSON so the contract stays machine-parsable. */
    if (health_mode) {
        if (target_idx < 0) {
            emit_health_json("error", "unknown", 0, 0, "",
                             "missing target argument (usage: otsardb --health <s3://bucket/root>)");
        } else {
            const char *target_arg = argv[target_idx];
            if (!is_s3_target(target_arg)) {
                emit_health_json("error", "unknown", 0, 0, target_arg,
                                 "health is only defined for s3:// (or otsardb+s3://) targets");
            }
#ifdef OTSARDB_ENABLE_S3
            else {
                free(consumed);
                return run_health_s3(target_arg);
            }
#else
            else {
                emit_health_json("error", "unknown", 0, 0, target_arg,
                                 "this OtsarDB binary was built without native S3 support; rebuild with OTSARDB_ENABLE_S3=ON");
            }
#endif
        }
        free(consumed);
        return 2;
    }

    if (json_mode && target_idx < 0) {
        char js[1024];
        otsardb_cli_json_error("missing target argument (usage: otsardb --json "
                             "<target> \"SQL\")", js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }

    if (json_schema_mode && target_idx < 0) {
        char js[1024];
        otsardb_cli_json_error("missing target argument (usage: otsardb "
                             "--json-schema <target>)", js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }

    if (target_idx < 0) {
        usage(argv[0]);
        free(consumed);
        return 2;
    }

    const char *target_arg = argv[target_idx];

    /* Studio mode: `otsardb studio` starts the loopback
     * web workbench (src/cli/studio.c). Exclusive mode: no other CLI
     * option may combine. A database file named "studio" must be spelled
     * "./studio" (documented in the studio-mode docs). --port 
     * is the studio port override and is rejected without the mode. */
    if (studio_port_flag != 0 && strcmp(target_arg, "studio") != 0) {
        fprintf(stderr, "error: --port is only valid with `otsardb studio` "
                        "(`otsardb studio --port <port>`)\n");
        free(consumed);
        return 2;
    }
    if (studio_window_flag && strcmp(target_arg, "studio") != 0) {
        fprintf(stderr, "error: --window is only valid with `otsardb studio` "
                        "(`otsardb studio --window`)\n");
        free(consumed);
        return 2;
    }
    if (studio_local_files_flag && strcmp(target_arg, "studio") != 0) {
        fprintf(stderr, "error: --local-files is only valid with `otsardb "
                        "studio` (`otsardb studio --local-files`)\n");
        free(consumed);
        return 2;
    }
    if (strcmp(target_arg, "studio") == 0) {
        if (json_mode || json_schema_mode) {
            char js[1024];
            otsardb_cli_json_error("--json/--json-schema cannot be combined "
                                 "with `otsardb studio`", js, sizeof(js));
            fputs(js, stdout);
            free(consumed);
            return 2;
        }
        if (echo_mode || health_mode || forward_port != 0 || forward_to[0]) {
            fprintf(stderr, "error: `otsardb studio` cannot be combined with "
                            "--echo/--health/--forward-port/--forward-to\n");
            free(consumed);
            return 2;
        }
        /* Port precedence: --port > OTSARDB_STUDIO_PORT >
         * 8717 (otsardb_studio_resolve_port, unit-tested). */
        int studio_port = otsardb_studio_resolve_port(
            studio_port_flag, getenv("OTSARDB_STUDIO_PORT"));
        if (studio_port < 0) {
            const char *env_port = getenv("OTSARDB_STUDIO_PORT");
            fprintf(stderr, "error: OTSARDB_STUDIO_PORT must be a port in "
                            "1..65535, got '%s'\n",
                    env_port ? env_port : "");
            free(consumed);
            return 2;
        }
        /* Desktop window mode (v0.6): --window wins over
         * OTSARDB_STUDIO_WINDOW (1/true/yes/on), else 0 (otsardb_studio_window_mode,
         * unit-tested). */
        int studio_window = otsardb_studio_window_mode(
            studio_window_flag, getenv("OTSARDB_STUDIO_WINDOW"));
        /* Local-file workbenching opt-in (audit F08):
         * --local-files wins over OTSARDB_STUDIO_ALLOW_LOCAL (1/true/yes/on),
         * else 0. 0 restricts the API to s3:// and ":memory:" targets. */
        int studio_local_files = otsardb_studio_allow_local_files(
            studio_local_files_flag, getenv("OTSARDB_STUDIO_ALLOW_LOCAL"));
        free(consumed);
        return otsardb_studio_serve_ex(studio_port, studio_window,
                                       studio_local_files);
    }

    int sql_start = argc;
    for (int i = target_idx + 1; i < argc; ++i) {
        if (!consumed[i]) {
            sql_start = i;
            break;
        }
    }

     /* --json needs a SQL argument: the REPL and the interactive meta
      * commands have no machine-readable form. */
    if (json_mode && sql_start >= argc) {
        char js[1024];
        otsardb_cli_json_error("--json requires a SQL argument "
                             "(REPL mode is not supported)", js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }

    /* --json-schema takes NO SQL argument: the schema
     * tree is read from the opened session itself. */
    if (json_schema_mode && sql_start < argc) {
        char js[1024];
        otsardb_cli_json_error("--json-schema does not accept a SQL argument",
                             js, sizeof(js));
        fputs(js, stdout);
        free(consumed);
        return 2;
    }

    /* Health/readiness mode (HA plan P3): machine-readable
     * status for orchestrators (Swarm/K8s restart policies). Always JSON,
     * always exits without a session, and never writes. Usage/argument
     * failures also emit JSON so the contract stays machine-parsable. */
    if (health_mode) {
        if (target_idx < 0) {
            emit_health_json("error", "unknown", 0, 0, "",
                             "missing target argument (usage: otsardb --health <s3://bucket/root>)");
        } else if (!is_s3_target(target_arg)) {
            emit_health_json("error", "unknown", 0, 0, target_arg,
                             "health is only defined for s3:// (or otsardb+s3://) targets");
        }
#ifdef OTSARDB_ENABLE_S3
        else {
            free(consumed);
            return run_health_s3(target_arg);
        }
#else
        else {
            emit_health_json("error", "unknown", 0, 0, target_arg,
                             "this OtsarDB binary was built without native S3 support; rebuild with OTSARDB_ENABLE_S3=ON");
        }
#endif
        free(consumed);
        return 2;
    }

    otsardb *db = 0;
    int rc = OTSARDB_ERROR;
    if (is_s3_target(target_arg)) {
#ifdef OTSARDB_ENABLE_S3
        otsardb_s3_open_config config = {0};
        config.forward_port = forward_port;
        config.forward_to = forward_to[0] ? forward_to : NULL;
        rc = otsardb_s3_open_target_ex(target_arg, &config, &db);
        if (rc != OTSARDB_OK) {
            if (json_mode || json_schema_mode) {
                char js[2048];
                char msg[1900];
                snprintf(msg, sizeof(msg),
                         "failed to open OtsarDB S3 target '%s': %s",
                         target_arg, otsardb_s3_last_error());
                otsardb_cli_json_error(msg, js, sizeof(js));
                fputs(js, stdout);
            } else {
                fprintf(stderr, "failed to open OtsarDB S3 target '%s': %s\n",
                        target_arg, otsardb_s3_last_error());
            }
            free(consumed);
            return 1;
        }
#else
        fprintf(stderr,
                "this OtsarDB binary was built without native S3 support; rebuild with OTSARDB_ENABLE_S3=ON\n");
        free(consumed);
        return 3;
#endif
    } else {
        rc = otsardb_open(target_arg, &db);
        if (rc != OTSARDB_OK) {
            if (json_mode || json_schema_mode) {
                char js[2048];
                char msg[1900];
                snprintf(msg, sizeof(msg), "failed to open OtsarDB target '%s'",
                         target_arg);
                otsardb_cli_json_error(msg, js, sizeof(js));
                fputs(js, stdout);
            } else {
                fprintf(stderr, "failed to open OtsarDB target: %s\n", target_arg);
            }
            free(consumed);
            return 1;
        }
    }
    int exit_code = 0;
    if (sql_start < argc) {
        size_t total = 0;
        for (int i = sql_start; i < argc; ++i) {
            if (!consumed[i]) total += strlen(argv[i]) + 1;
        }
        char *sql = (char *)calloc(total + 1, 1);
        if (!sql) exit_code = 1;
        else {
            for (int i = sql_start; i < argc; ++i) {
                if (consumed[i]) continue;
                if (i > sql_start) strcat(sql, " ");
                strcat(sql, argv[i]);
            }
            if (json_mode) {
                /* Machine-readable exec: one JSON object
                 * per statement result. Write forwarding is deliberately
                 * NOT attempted here — a candidate-mode BUSY write returns
                 * the error object (see json_mode.h). */
                char *jbuf = (char *)malloc(OTSARDB_JSON_OUT_CAP);
                if (!jbuf) {
                    char js[1024];
                    otsardb_cli_json_error("out of memory", js, sizeof(js));
                    fputs(js, stdout);
                    exit_code = 1;
                } else {
                    exit_code = otsardb_cli_exec_json(db, sql, jbuf,
                                                    OTSARDB_JSON_OUT_CAP);
                    fputs(jbuf, stdout);
                    free(jbuf);
                }
            } else if (json_schema_mode) {
                /* Machine-readable schema: one
                 * {"ok":true,"tables":[...]} object built from
                 * sqlite_schema + PRAGMA table_info/index_list/index_info
                 * on the opened session (see studio.h). */
                char *jbuf = (char *)malloc(OTSARDB_JSON_OUT_CAP);
                if (!jbuf) {
                    char js[1024];
                    otsardb_cli_json_error("out of memory", js, sizeof(js));
                    fputs(js, stdout);
                    exit_code = 1;
                } else {
                    exit_code = otsardb_studio_schema_json(db, jbuf,
                                                         OTSARDB_JSON_OUT_CAP);
                    fputs(jbuf, stdout);
                    free(jbuf);
                }
            } else {
                exit_code = execute(db, sql, 0);
            }
            free(sql);
        }
    } else if (json_schema_mode) {
        /* --json-schema with no SQL argument: schema mode is complete in
         * itself. */
        char *jbuf = (char *)malloc(OTSARDB_JSON_OUT_CAP);
        if (!jbuf) {
            char js[1024];
            otsardb_cli_json_error("out of memory", js, sizeof(js));
            fputs(js, stdout);
            exit_code = 1;
        } else {
            exit_code = otsardb_studio_schema_json(db, jbuf, OTSARDB_JSON_OUT_CAP);
            fputs(jbuf, stdout);
            free(jbuf);
        }
    } else {
        exit_code = repl(db, target_arg);
    }

    free(consumed);
    otsardb_close(db);
    return exit_code;
}

#ifndef OTSARDB_CLI_STUDIO_H
#define OTSARDB_CLI_STUDIO_H

#include "otsardb.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OtsarDB Studio MVP web server . Loopback-only workbench
 * serving the embedded single-page UI and executing SQL through the same
 * engine path as the CLI (in-process open/exec/close per request).
 *
 * Endpoints (v3):
 * GET / -> 200 text/html: the embedded single-page UI
 * (inline CSS/JS, no external assets, offline-capable)
 * with the per-process CSRF token injected as
 * <script>window.OTSARDB_CSRF_TOKEN="<hex>";</script>
 * (security audit F01)
 * GET /health -> 200 text/plain "ok"
 * GET /api/csrf -> 200 application/json {"token":"<hex>"}: the
 * per-process CSRF token 
 * POST /api/query -> JSON request body, JSON response (see below);
 * malformed bodies -> 400 with a JSON error object
 * POST /api/schema request: same connection contract as /api/query (sql
 * optional and ignored); the response is the --json-schema payload (one
 * {"ok":true,"tables":[...]} object) or an error object; 400 on malformed
 * bodies. v0.4 additive field: "with_sql": "1" appends the verbatim stored
 * CREATE statement ("sql": "...", from sqlite_schema.sql) to every object
 * (table and view); without it the payload is byte-identical to v0.3
 * , and `otsardb --json-schema <target>` (the CLI mode) is
 * byte-identical either way.
 * POST /api/health-> same request contract as /api/query (sql optional
 * and ignored); the server executes the 
 * readiness probe per request and responds with the
 * --health JSON (status/role/s3_reachable/open_ms/
 * engine/target[/error]) extended with "lease"
 * (state/owner/generation/address from a read-only
 * peek of <root>/lease.json), "single_writer"
 * (ambient OTSARDB_S3_SINGLE_WRITER) and "last_error"
 * (the server's most recent failed-request message).
 * Deterministic on non-S3 targets (0061 contract:
 * status=error, health is only defined for s3://).
 * POST /api/edit -> row-edit endpoint (v0.3 grid editing): the server
 * GENERATES the per-row UPDATE/DELETE/INSERT SQL
 * (otsardb_studio_edit_sql) and executes it through the
 * same pipeline as /api/query; response is the --json
 * payload of the generated statement (or an error
 * object); 400 on malformed bodies
 * any other path -> 404; non-GET/POST -> 405
 *
 * CSRF / loopback-origin gate (security audit F01). Every
 * state-changing request (POST /api/*) must pass, in order:
 * 1. Host: 127.0.0.1:<port> (exact, case-insensitive) -> else 403
 * 2. Origin absent, or exactly http://127.0.0.1:<port> -> else 403
 * 3. X-OTSARDB-CSRF header equal to the per-process token
 * (64 hex chars, generated at server start; delivered to the UI in
 * the GET / page and via GET /api/csrf) -> else 403
 * 4. Content-Type: application/json (optional charset params) -> else 415
 * The gate runs before the body is read or dispatched, so the per-request
 * endpoint/region/keys override (the signed-request delegation vector of
 * F01) can only ever be applied to requests that already passed it. GET
 * endpoints (/ , /health, /api/csrf) are not state-changing and stay open.
 *
 * POST /api/query request (a flat JSON object of string values):
 * {"target": "s3://bucket/root", optional, wins over bucket+database
 * "sql": "SELECT ...", required
 * "endpoint": "...", "region": "...","bucket":"...", "database":"...",
 * "access_key": "...", "secret_key": "...",
 * "auto_checkpoint": "1", optional: OTSARDB_AUTO_CHECKPOINT=1
 * "auto_checkpoint_commits": "1", optional: OTSARDB_AUTO_CHECKPOINT_COMMITS=1
 * "gc": "1", optional: OTSARDB_GC=1
 * "page_size": "50", optional (v0.4): server-side paging
 * "page": "3"} optional (v0.4): 1-based page number
 * The three admin fields are applied to the process environment for the
 * duration of the request only (admin actions): the session
 * close then runs the automatic checkpoint (0018; the commits knob forces
 * it at every close) and, with gc=1, the GC sweep (0040).
 *
 * Server-side paging (v0.4): when "page_size" is present
 * AND the statement is the plain `SELECT * FROM <ident>` shape (the same
 * shape the grid editor accepts; matcher in otsardb_studio_paged_sql), the
 * server rewrites the statement to `SELECT * FROM "t" LIMIT <n> OFFSET
 * <m>` (page_size clamped to 1..10000, page >= 1) and executes it through
 * the same pipeline as --json; the response is the --json payload with
 * the additive field "total":N — the COUNT(*) of the same table on the
 * same session. Any other SQL shape (WHERE/ORDER BY/LIMIT/JOIN/comments/
 * bracket or backtick identifiers/multi-statement strings) IGNORES the
 * paging fields and keeps the exact --json payload, so the UI falls back
 * to client-side paging over the fetched result .
 *
 * When "target" is absent, the target is built as s3://<bucket>/<database>.
 * Per-request endpoint/region/access_key/secret_key values are applied to
 * the process environment for the duration of the request only (saved and
 * restored afterwards) — they are never logged and never written to disk
 * (R5). When a field is absent, the process environment is used
 * unchanged (the CLI's normal OTSARDB_S3_* configuration).
 *
 * The response is the --json payload (src/cli/json_mode.h): one object per
 * statement result, exactly as `otsardb --json` prints. HTTP status is 200
 * for every executed query (the JSON carries ok/error); 400 only for
 * unparsable request bodies.
 *
 * POST /api/schema response (the --json-schema payload):
 * {"ok":true,"tables":[
 * {"name":"jt","type":"table",
 * "columns":[{"name":"id","type":"INTEGER","pk":1,"notnull":0},...],
 * "indexes":[{"name":"idx_jt_v","unique":0,"columns":["v"]},...]},
 * {"name":"jv","type":"view","columns":[...],"indexes":[]}, ...]}
 * The tree is built from sqlite_schema + PRAGMA table_info(<t>) +
 * PRAGMA index_list(<t>) + PRAGMA index_info(<idx>) on the opened session;
 * views carry an empty indexes array. Bounded (<= 10000 objects, <= 8 MiB
 * payload, fail-closed error object).
 *
 * POST /api/edit request (a flat JSON object of string fields; the
 * connection fields are those of /api/query; the four array fields are
 * JSON arrays of scalar values carried as string field values):
 * {"target": ..., "endpoint": ..., "bucket": ..., "database": ...,
 * "table": "t", table name (unquoted)
 * "op": "update" | "delete" | "insert" row operation
 * "cols": "[\"id\",\"v\",\"w\"]", JSON array of column names (row order)
 * "pk": "[\"id\"]", JSON array of primary-key column names
 * "old": "[1,\"a\",null]", JSON array of current cell values
 * "new": "[1,\"b\",null]"} JSON array of new cell values
 * The server builds the UPDATE (SET all cols, WHERE pk from "old"),
 * DELETE (WHERE pk from "old") or INSERT (cols + "new") statement with
 * identifier double-quoting ("" doubling), string literals (' doubling),
 * validated raw numbers and NULL literals — see otsardb_studio_edit_sql.
 * Bounds: <= 64 columns, cell text <= 256 bytes, generated SQL <= the
 * studio SQL cap; violations fail closed with an error object.
 *
 * Security boundary (matches the M2 forward server): the
 * server binds 127.0.0.1 only, serves one connection at a time, speaks
 * plaintext HTTP/1.1 with Content-Length bodies, no keep-alive, no TLS and
 * no authentication. It executes arbitrary SQL on any target it can open:
 * it must never listen on a non-loopback interface. Since 
 * (audit F01) the loopback boundary is enforced against cross-site pages:
 * POST /api/* additionally requires the loopback Origin/Host match, the
 * per-process CSRF token header and Content-Type: application/json — see
 * the gate description above. Since (audit F08) the target
 * surface is ALSO closed by default: only s3:// (otsardb+s3://) and the
 * in-memory engine target ":memory:" are accepted; any other local path is
 * refused with an error object unless the server was started with the
 * explicit --local-files / OTSARDB_STUDIO_ALLOW_LOCAL=1 opt-in.
 */
typedef struct otsardb_studio_server otsardb_studio_server;

/* Starts the server thread on 127.0.0.1:port (0 = ephemeral; query with
 * otsardb_studio_server_port). Returns NULL on failure with error filled.
 * This 3-argument form keeps the pre-0147 signature with the local-file
 * opt-in OFF (the F08 default); otsardb_studio_server_start_ex carries
 * the explicit allow_local_files flag. */
otsardb_studio_server *otsardb_studio_server_start(int port, char *error,
                                               size_t error_cap);

/* Full form (security audit F08): allow_local_files is 0
 * unless the operator opted in with --local-files /
 * OTSARDB_STUDIO_ALLOW_LOCAL=1. With 0, run_request refuses every
 * non-s3:// (non-otsardb+s3://) target that is not ":memory:"; with 1,
 * local-file paths are accepted again (the pre-0147 workbench behavior). */
otsardb_studio_server *otsardb_studio_server_start_ex(int port,
                                                   int allow_local_files,
                                                   char *error,
                                                   size_t error_cap);

/* Actual bound port (usable when the server was started with port 0). */
int otsardb_studio_server_port(const otsardb_studio_server *server);

/* Signals stop, unblocks the accept loop and joins the server thread.
 * Safe to call once; the server must not be used afterwards. */
void otsardb_studio_server_stop(otsardb_studio_server *server);

/* Builds the --json-schema payload for db's own SQLite connection into out
 * (NUL-terminated; out must hold at least OTSARDB_JSON_OUT_CAP bytes): one
 * {"ok":true,"tables":[...]} object (or {"ok":false,"error":...} on
 * failure). Shared by `otsardb --json-schema <target>` (main.c) and the
 * POST /api/schema endpoint. Returns 0 on success, 1 on failure. */
int otsardb_studio_schema_json(otsardb *db, char *out, size_t out_cap);

/* Same as otsardb_studio_schema_json with the v0.4 "with_sql" flag: when
 * non-zero, every object also carries the verbatim stored CREATE statement
 * ("sql", from sqlite_schema.sql; NULL sql renders as an empty string).
 * The non-opt form is a wrapper with with_sql=0, so the CLI --json-schema
 * output and the default /api/schema payload stay byte-identical to 0073. */
int otsardb_studio_schema_json_opt(otsardb *db, int with_sql, char *out,
                                 size_t out_cap);

/* ------------------------------------------------------------------ v0.4
 * Server-side paging SQL generation . Matches the plain
 * `SELECT * FROM <ident>` shape — optional surrounding whitespace,
 * case-insensitive SELECT/FROM keywords, exactly one '*' and one table
 * identifier (double-quoted with "" escapes, or a bare identifier), an
 * optional single trailing ';' — and rewrites it to:
 *
 * count_only=0: SELECT * FROM "<t>" LIMIT <page_size> OFFSET <n>;
 * count_only=1: SELECT COUNT(*) FROM "<t>";
 *
 * page_size is clamped to 1..10000, page to >= 1 (OFFSET = (page-1)*size,
 * bounded). Returns 0 on success (out NUL-terminated); -1 when the shape
 * does not match or the output does not fit — the caller then keeps the
 * original statement and client-side paging. */
int otsardb_studio_paged_sql(const char *sql, long page, long page_size,
                           int count_only, char *out, size_t out_cap);

/* ------------------------------------------------------------------ v0.3
 * Row-edit SQL generation . The studio server generates
 * the per-row UPDATE/DELETE/INSERT statement for the grid editor; the
 * generator is a pure function so the quoting rules are unit-tested
 * without sockets or a store.
 *
 * Cell values carry their JSON type (the --json contract types): NULL,
 * validated RFC 8259 numbers (emitted raw), everything else as string
 * (emitted as '...' SQL literal with '' doubling). Identifiers are
 * double-quoted with "" doubling. The WHERE clause uses the PRIMARY-KEY
 * columns only, matched against the ORIGINAL ("old") row values — the
 * same optimistic-concurrency posture as the CLI's single-shot sessions:
 * the statement targets the row as it was read, without locks.
 */

#define OTSARDB_EDIT_MAX_COLS 64
#define OTSARDB_EDIT_MAX_CELL 256

typedef struct {
    int is_null;      /* 1 = JSON null */
    int is_number;    /* 1 = JSON number (text is the RFC 8259 number text) */
    const char *text; /* number text or string content (unquoted) */
} otsardb_studio_cell;

typedef struct {
    const char *table;           /* unquoted table name */
    const char *op;              /* "update" | "delete" | "insert" */
    const char *const *cols;     /* column names, in row order */
    int n_cols;
    const char *const *pk;       /* primary-key column names */
    int n_pk;
    const otsardb_studio_cell *old_row; /* original row (WHERE for update/delete) */
    int n_old;
    const otsardb_studio_cell *new_row; /* new row (SET for update, VALUES for insert) */
    int n_new;
} otsardb_studio_edit;

/* Generates the edit SQL into out (NUL-terminated, out_cap bytes).
 * Returns 0 on success; -1 on failure with a human-readable message in
 * out (fail-closed: missing table/op, unsupported op, no columns, pk
 * required but absent for update/delete, pk not among cols, old/new
 * length mismatches, > OTSARDB_EDIT_MAX_COLS columns, cell text longer than
 * OTSARDB_EDIT_MAX_CELL, malformed number text, output cap exceeded). */
int otsardb_studio_edit_sql(const otsardb_studio_edit *edit, char *out,
                          size_t out_cap);

/* Blocking CLI entry point: starts the server on 127.0.0.1:port, prints
 * the startup line (URL + loopback/no-auth warning) and the Ctrl+C hint,
 * then attempts to open the default browser (best-effort, silent on
 * failure: ShellExecuteA on Windows, xdg-open on POSIX) and serves until
 * interrupted. Returns 0 on clean stop, 1 on startup failure. */
int otsardb_studio_serve(int port);

/* Same as otsardb_studio_serve with the v0.6 desktop-window mode (
 * 0104): when window_mode is non-zero, the embedded UI is hosted in a
 * native WebView2 window (src/cli/studio_window.c) instead of a browser
 * tab. The window function returns 0 when the user closed the window —
 * the server is then stopped and the CLI exits cleanly. Any failure
 * (WebView2Loader.dll missing, environment/controller error, window
 * component not linked) prints the exact reason and falls back to the
 * v0.5 default-browser behavior (byte-identical output after the note).
 *
 * allow_local_files (audit F08): the explicit opt-in that
 * re-enables local-file targets in run_request. 0 (the default) restricts
 * the API to s3:// (otsardb+s3://) and ":memory:" targets. */
int otsardb_studio_serve_ex(int port, int window_mode, int allow_local_files);

/* ------------------------------------------------------------------ v0.5
 * Studio port resolution: the --port flag wins, then the
 * OTSARDB_STUDIO_PORT environment variable, then the default 8717. Pure
 * function (unit-tested without sockets): flag_port is 0 when --port was
 * not given; env_value may be NULL/empty (both mean "use the fallback").
 * Returns the resolved port in 1..65535, or -1 when the value actually
 * provided (flag or env) is out of range — the caller then reports the
 * usage error (exit 2). */
int otsardb_studio_resolve_port(int flag_port, const char *env_value);

/* ------------------------------------------------------------------ v0.6
 * Studio desktop-window mode . The window itself (Win32 +
 * WebView2) is implemented in src/cli/studio_window.c; these pure decision
 * functions encode the mode selection and the browser fallback so the
 * behavior is unit-tested without a GUI (tests/studio_window_test.c).
 *
 * otsardb_studio_window_mode(flag_window, env_value): 1 when the desktop
 * window was requested for `otsardb studio`: the --window flag wins, then
 * OTSARDB_STUDIO_WINDOW (truthy: 1/true/yes/on, case-insensitive — the same
 * parsing rule as the ambient env_truthy checks in studio.c), else 0. */
int otsardb_studio_window_mode(int flag_window, const char *env_value);

/* ------------------------------------------------------------------ F08
 * Local-file workbenching opt-in (security audit F08):
 * otsardb_studio_allow_local_files(flag_local_files, env_value) is 1 when
 * `otsardb studio --local-files` was given, then
 * OTSARDB_STUDIO_ALLOW_LOCAL (truthy 1/true/yes/on), else 0. 0 keeps
 * run_request closed to non-s3:// targets (except ":memory:"); 1 restores
 * the pre-0147 behavior where a loopback client could open any local
 * path. Pure function (unit-tested in tests/studio_local_target_test.c). */
int otsardb_studio_allow_local_files(int flag_local_files,
                                     const char *env_value);

#define OTSARDB_STUDIO_WINDOW_PLAN_NONE 0    /* window not requested: v0.5 */
#define OTSARDB_STUDIO_WINDOW_PLAN_WINDOW 1  /* requested + loader present */
#define OTSARDB_STUDIO_WINDOW_PLAN_BROWSER 2 /* requested + loader absent */

/* Fallback decision: given the window was requested and
 * whether the WebView2 loader DLL is available on this machine, decide
 * between the desktop window and the browser fallback. Pure function;
 * loader_available is the runtime probe result (studio_window.c). */
int otsardb_studio_window_plan(int window_requested, int loader_available);

#ifdef __cplusplus
}
#endif

#endif

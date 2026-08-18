#ifndef OTSARDB_CLI_JSON_MODE_H
#define OTSARDB_CLI_JSON_MODE_H

#include "otsardb.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The OtsarDB CLI --json output mode . Contract:
 *
 * otsardb --json <target> "SQL"
 *
 * prints one JSON object per statement result on stdout, one object per
 * line (NDJSON):
 *
 * {"ok":true,"rows":N,"columns":["c1",...],"data":[[...],[...]]}
 * {"ok":false,"error":"<message>"}
 *
 * Exit codes (same semantics as the non-JSON CLI): 0 = every statement
 * succeeded; 1 = open failure or a statement error (the error object is
 * still printed to stdout); 2 = usage errors (--json combined with
 * --echo/--forward-port, REPL mode, missing target/SQL) — usage errors
 * also print an {"ok":false,...} object so stdout stays machine-parsable.
 *
 * Cell typing follows SQLite affinity as observed per value through
 * sqlite3_column_type(): NULL -> null; INTEGER and FLOAT whose text is a
 * valid RFC 8259 JSON number -> JSON number; everything else (TEXT, BLOB,
 * non-finite FLOAT such as Inf/NaN) -> JSON string. Strings are escaped
 * (quotes/backslash/control chars); BLOB bytes >= 0x80 pass through under
 * the UTF-8 assumption (documented limitation).
 *
 * Write forwarding is NOT used in this mode: a
 * candidate-mode BUSY write returns the error object instead of being
 * forwarded (documented in the CLI docs).
 *
 * Bounds (fail-closed: an explicit error object replaces the result
 * instead of silent truncation):
 * OTSARDB_JSON_MAX_ROWS - max rows per statement result;
 * OTSARDB_JSON_MAX_BYTES - max payload per statement result;
 * OTSARDB_JSON_OUT_CAP - max total output per invocation.
 */
#define OTSARDB_JSON_MAX_ROWS 100000
#define OTSARDB_JSON_MAX_BYTES (8u << 20)
#define OTSARDB_JSON_OUT_CAP (4u * OTSARDB_JSON_MAX_BYTES)

/* Executes `sql` on db's own SQLite connection — the same engine path as
 * otsardb_exec (same VFS, WAL publisher, lease and durability machinery).
 * Writes the NDJSON payload into out (always NUL-terminated; out must
 * hold at least OTSARDB_JSON_OUT_CAP bytes). Returns 0 when every statement
 * succeeded, 1 otherwise; on failure the final line is the
 * {"ok":false,"error":...} object of the failing statement. */
int otsardb_cli_exec_json(otsardb *db, const char *sql, char *out, size_t out_cap);

/* Builds {"ok":false,"error":"<message>"}\n into out, escaped. The object
 * is small (<= 4096 bytes); out_cap >= 1024 is enough in practice. */
void otsardb_cli_json_error(const char *message, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif

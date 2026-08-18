#ifndef OTSARDB_H
#define OTSARDB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct otsardb otsardb;
typedef int (*otsardb_row_callback)(void *ctx, int argc, char **argv, char **columns);

#define OTSARDB_OK 0
#define OTSARDB_ERROR 1
#define OTSARDB_UNSUPPORTED 2

/* CLI health/readiness contract (availability model
 * Level C, HA plan P3): `otsardb --health <s3://...>` (alias --health-json)
 * emits one machine-readable JSON line and exits without keeping a session
 * open:
 *
 * {"status":"ok|degraded|error","role":"leader|candidate|unknown",
 * "s3_reachable":true|false,"open_ms":N,"engine":"<version>",
 * "target":"<target>"[,"error":"<message>"]}
 *
 * Exit codes: 0 = ok (valid open + read succeeded); 1 = degraded (open
 * succeeded, read failed); 2 = error (open failed or usage). The probe
 * opens read-only: it never claims the writer lease and never writes the
 * database. Full contract: docs/AVAILABILITY_MODEL.md Level C. */
int otsardb_initialize(void);
const char *otsardb_version(void);
const char *otsardb_engine_version(void);
int otsardb_open(const char *target, otsardb **out_db);
int otsardb_exec(otsardb *db, const char *sql, otsardb_row_callback callback, void *ctx, char **error_message);
const char *otsardb_errmsg(otsardb *db);
void otsardb_free(void *ptr);
void otsardb_close(otsardb *db);

#ifdef __cplusplus
}
#endif

#endif

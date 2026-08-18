#ifndef OTSARDB_S3_DATABASE_H
#define OTSARDB_S3_DATABASE_H

#include "otsardb.h"

#include <stddef.h>

/* Forward declaration so the header is self-contained when included by a
 * translation unit that does not pull in object_store.h (coordinator
 * consolidation). */
struct otsardb_object_store;

#ifdef __cplusplus
extern "C" {
#endif

/* M2 HTTP write forwarding configuration . NULL/zero
 * entries disable the corresponding feature; old callers that keep using
 * otsardb_s3_open_target() get the pre-0032 behavior. */
typedef struct otsardb_s3_open_config {
    /* Leader mode: when > 0, starts the embedded HTTP forward server on
     * 127.0.0.1:<forward_port> and publishes "127.0.0.1:<port>" in the
     * lease object at claim time so non-leader sessions can forward write
     * SQL. 0 = the session serves no forward requests. */
    int forward_port;
    /* Explicit forward target override ("host:port"). When non-NULL, the
     * session reports this address to the CLI instead of the lease-derived
     * leader address. Useful when the lease predates the address field or
     * when the leader listens on a non-loopback interface. */
    const char *forward_to;
} otsardb_s3_open_config;

/* Opens an OtsarDB database whose authoritative persistence is object storage.
 *
 * target:
 * s3://bucket/database/root
 * otsardb+s3://bucket/database/root
 *
 * Runtime S3 configuration is read from OTSARDB_S3_* variables, with AWS
 * credential/region variables accepted as fallbacks. The URI remains logical
 * and vendor-neutral: changing AWS/EVEO/Magalu/Wasabi/etc. does not change the
 * OtsarDB database format.
 */
int otsardb_s3_open_target(const char *target, otsardb **out_db);
int otsardb_s3_open_target_ex(const char *target,
                            const otsardb_s3_open_config *config,
                            otsardb **out_db);

/* Forwarding target for `db` (an S3-opened OtsarDB handle): 1 + the
 * "host:port" in out/out_cap when this session is NOT the current lease
 * leader and a leader address is known (explicit --forward-to wins over
 * the lease-derived address); 0 otherwise (leader, no address known, or a
 * non-S3 handle). Safe to call at any time; never blocks on the network
 * (reads the last known lease snapshot). */
int otsardb_s3_leader_address(otsardb *db, char *out, size_t cap);

/* Human-readable error for the most recent otsardb_s3_open_target() call on the
 * current thread. The pointer remains valid until the next S3 open attempt on
 * that thread. */
const char *otsardb_s3_last_error(void);

/* — deterministic test hook (NOT part of the public
 * contract). Opens a session over a caller-provided object store (memory/
 * fault stores in tests) instead of OTSARDB_S3_* env. The session BORROWS
 * the store (never destroys it; the caller owns its lifetime). Everything
 * after store construction runs the same open path as
 * otsardb_s3_open_target_ex, including OTSARDB_CONSISTENCY handling. */
int otsardb_s3_open_with_store(struct otsardb_object_store *store,
                               int s3_cas_ok,
                               const char *database_root,
                               const char *database_id,
                               const otsardb_s3_open_config *config,
                               otsardb **out_db);

/* (audit finding F04) — cache-dir hardening helper used by
 * make_execution_path and the probe-verdict cache, also callable from tests.
 * Creates `dir` (idempotent) and, on POSIX, applies mode 0700 to it — the
 * mode is re-applied on every call so a directory created by an older binary
 * (default umask, e.g. 0755) is tightened rather than left world-readable.
 * Returns 1 on success. A failed create or chmod returns 0 (fail closed:
 * callers refuse to place unencrypted database material in an unhardened
 * dir). On Windows the mode is not applicable: the directory is created with
 * the user's default %TEMP% ACLs and the function only fails when the create
 * itself fails. */
int otsardb_cache_dir_apply_perms(const char *dir);

#ifdef __cplusplus
}
#endif

#endif

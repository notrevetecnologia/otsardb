#ifndef OTSARDB_GC_H
#define OTSARDB_GC_H
#include "object_store.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Options for otsardb_gc_sweep . Zero-initialised options
 * behave as defaults. */
typedef struct otsardb_gc_options {
    /* Retention window in seconds: when > 0, superseded history is only
     * swept once the checkpoint pointer that covers it is at least this
     * old (OTSARDB_GC_KEEP_MINUTES x 60). 0 deletes immediately after the
     * checkpoint is published. When the store does not expose the pointer's
     * Last-Modified time, the sweep is skipped (cannot prove age). */
    uint64_t keep_secs;
    /* Expected owner of <root>/lease.json. When non-NULL/non-empty, the
     * writer lease is deleted only if its stored owner matches this value
     * (the closing session's own instance id), so a different live writer's
     * lease is never touched. NULL disables lease deletion. */
    const char *owner_expected;
} otsardb_gc_options;

/* Sweep objects no longer reachable from the latest checkpoint:
 * commit manifests strictly older than the checkpoint's covered generation
 * and their segments. The live set is never touched: HEAD, CHECKPOINT.json,
 * the checkpoint manifest/page groups, the checkpoint's own base manifest
 * (generation == covered_generation, which recovery replays) and everything
 * newer than it.
 *
 * Returns the number of objects deleted (0 means nothing to sweep, not an
 * error). Returns -1 on error (nothing deleted on a pre-verified error; a
 * mid-walk store failure stops the walk best-effort and returns the partial
 * count). Failures are logged to stderr and never abort the caller.
 */
int otsardb_gc_sweep(otsardb_object_store *store,
                   const char *database_root,
                   const otsardb_gc_options *options);

#ifdef __cplusplus
}
#endif
#endif

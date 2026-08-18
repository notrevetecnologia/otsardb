#ifndef OTSARDB_CHECKPOINT_BUILD_H
#define OTSARDB_CHECKPOINT_BUILD_H

#include "object_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build a materialised checkpoint of the current database state.
 *
 * Uses the existing recovery path to reconstruct a full database image,
 * partitions it into bounded page groups, uploads every group as an
 * immutable .otscp object, publishes a checkpoint manifest, and
 * finally publishes the CHECKPOINT.json pointer (CAS-fenced when the
 * store supports conditional PUT).
 *
 * Returns 1 on success, 0 on any failure.  Caller must provide a
 * writable temp_db_path for the intermediate materialisation.
 */
int otsardb_checkpoint_build(otsardb_object_store *store,
                           const char *database_root,
                           const char *temp_db_path);

#ifdef __cplusplus
}
#endif

#endif

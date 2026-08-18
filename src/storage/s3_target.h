#ifndef OTSARDB_S3_TARGET_H
#define OTSARDB_S3_TARGET_H

#include "journal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_S3_BUCKET_MAX 255

typedef struct otsardb_s3_target {
    char bucket[OTSARDB_S3_BUCKET_MAX + 1];
    char database_root[OTSARDB_KEY_MAX + 1];
} otsardb_s3_target;

/* Parse and normalize OtsarDB's logical S3 target.
 *
 * Accepted forms:
 *   s3://bucket/database/root
 *   otsardb+s3://bucket/database/root
 *
 * The URI deliberately contains no provider endpoint or credentials. Those
 * are runtime transport configuration, while bucket/root identify the logical
 * OtsarDB database. Query strings and fragments are currently rejected rather
 * than silently becoming part of an object key.
 */
int otsardb_s3_target_parse(const char *value, otsardb_s3_target *out_target);

/* Derive the stable OtsarDB database identity from the normalized logical target.
 * Both accepted schemes produce the same ID for the same bucket/root. */
int otsardb_s3_target_database_id(const otsardb_s3_target *target,
                                char out_database_id[OTSARDB_COMMIT_ID_MAX + 1]);

#ifdef __cplusplus
}
#endif

#endif

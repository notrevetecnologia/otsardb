#ifndef OTSARDB_S3_OBJECT_STORE_H
#define OTSARDB_S3_OBJECT_STORE_H

#include "object_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* (security audit finding F06): hard fail-closed cap on the
 * number of bytes a single GET request may accumulate in memory. The object
 * store is a vendor-neutral adapter; a malicious or buggy store could stream
 * an arbitrarily large body and exhaust memory (defense-in-depth — the
 * manifest is otherwise hash-verified). The largest legitimate object the
 * publisher produces is a WAL-batch segment bounded by
 * OTSARDB_WAL_PUBLISHER_DEFAULT_SEGMENT_PAYLOAD_LIMIT (16 MiB) plus encoder
 * overhead; 64 MiB keeps a 4x margin while bounding memory per request. */
#define OTSARDB_S3_GET_MAX_BYTES (64u * 1024u * 1024u)

typedef struct otsardb_s3_config {
    const char *endpoint;
    const char *region;
    const char *bucket;
    const char *access_key;
    const char *secret_key;
    const char *session_token;
    const char *prefix;
    int use_https;
} otsardb_s3_config;

/*
 * Creates the vendor-neutral OtsarDB S3 object store.
 *
 * access_key + secret_key are optional: when supplied they take precedence
 * (StaticProvider). When absent, OtsarDB resolves credentials through a
 * provider chain: AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY env vars →
 * MINIO_ROOT_USER / MINIO_ROOT_PASSWORD env vars → ~/.aws/credentials →
 * AWS instance metadata / IAM role.
 *
 * endpoint may be an AWS S3 endpoint or any S3-compatible endpoint.
 * A http:// or https:// scheme in endpoint overrides use_https.
 * prefix is optional and is prepended to every OtsarDB object key.
 *
 * Runtime capabilities are intentionally not assumed here. Run the
 * capability probe before enabling S3 writes and let OtsarDB select CAS or
 * SINGLE_WRITER from the reported endpoint behavior.
 */
otsardb_object_store *otsardb_s3_store_create(const otsardb_s3_config *config);
void otsardb_s3_store_destroy(otsardb_object_store *store);

#ifdef __cplusplus
}
#endif

#endif

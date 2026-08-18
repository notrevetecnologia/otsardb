#include "s3_target.h"

#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_forbidden_uri_suffix(const char *value, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (value[i] == '?' || value[i] == '#') return 1;
    }
    return 0;
}

int otsardb_s3_target_parse(const char *value, otsardb_s3_target *out_target) {
    if (!value || !out_target) return 0;
    memset(out_target, 0, sizeof(*out_target));

    const char *cursor = NULL;
    static const char S3_PREFIX[] = "s3://";
    static const char OTSARDB_S3_PREFIX[] = "otsardb+s3://";

    if (strncmp(value, S3_PREFIX, sizeof(S3_PREFIX) - 1u) == 0) {
        cursor = value + sizeof(S3_PREFIX) - 1u;
    } else if (strncmp(value, OTSARDB_S3_PREFIX, sizeof(OTSARDB_S3_PREFIX) - 1u) == 0) {
        cursor = value + sizeof(OTSARDB_S3_PREFIX) - 1u;
    } else {
        return 0;
    }

    const char *slash = strchr(cursor, '/');
    if (!slash || slash == cursor) return 0;

    size_t bucket_size = (size_t)(slash - cursor);
    if (bucket_size == 0 || bucket_size > OTSARDB_S3_BUCKET_MAX ||
        has_forbidden_uri_suffix(cursor, bucket_size)) {
        return 0;
    }

    const char *root = slash + 1;
    while (*root == '/') ++root;
    size_t root_size = strlen(root);
    while (root_size > 0 && root[root_size - 1u] == '/') --root_size;

    if (root_size == 0 || root_size > OTSARDB_KEY_MAX ||
        has_forbidden_uri_suffix(root, root_size)) {
        return 0;
    }

    memcpy(out_target->bucket, cursor, bucket_size);
    out_target->bucket[bucket_size] = '\0';
    memcpy(out_target->database_root, root, root_size);
    out_target->database_root[root_size] = '\0';
    return 1;
}

int otsardb_s3_target_database_id(const otsardb_s3_target *target,
                                char out_database_id[OTSARDB_COMMIT_ID_MAX + 1]) {
    if (!target || !out_database_id || target->bucket[0] == '\0' ||
        target->database_root[0] == '\0') {
        return 0;
    }

    size_t bucket_size = strlen(target->bucket);
    size_t root_size = strlen(target->database_root);
    if (bucket_size > OTSARDB_S3_BUCKET_MAX || root_size > OTSARDB_KEY_MAX) return 0;

    static const char PREFIX[] = "s3://";
    size_t identity_size = (sizeof(PREFIX) - 1u) + bucket_size + 1u + root_size;
    char *identity = (char *)malloc(identity_size + 1u);
    if (!identity) return 0;

    int written = snprintf(identity,
                           identity_size + 1u,
                           "s3://%s/%s",
                           target->bucket,
                           target->database_root);
    if (written < 0 || (size_t)written != identity_size) {
        free(identity);
        return 0;
    }

    char digest[OTSARDB_SHA256_HEX_SIZE + 1];
    int ok = otsardb_sha256_hex_data(identity, identity_size, digest);
    free(identity);
    if (!ok) return 0;

    /* 3-byte "db-" prefix + 48 hex chars = 51 bytes, safely below the
     * current 63-byte commit/database identifier envelope. */
    if (OTSARDB_COMMIT_ID_MAX < 51) return 0;
    memcpy(out_database_id, "db-", 3u);
    memcpy(out_database_id + 3u, digest, 48u);
    out_database_id[51] = '\0';
    return 1;
}

#ifndef OTSARDB_JOURNAL_H
#define OTSARDB_JOURNAL_H

#include "object_store.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_COMMIT_ID_MAX 63
#define OTSARDB_KEY_MAX 1023
#define OTSARDB_SHA256_HEX_MAX 64

typedef struct otsardb_head {
    uint32_t format_version;
    uint64_t generation;
    char database_id[OTSARDB_COMMIT_ID_MAX + 1];
    char commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char commit_key[OTSARDB_KEY_MAX + 1];
    char commit_sha256[OTSARDB_SHA256_HEX_MAX + 1];
} otsardb_head;

typedef enum otsardb_journal_result {
    OTSARDB_JOURNAL_OK = 0,
    OTSARDB_JOURNAL_NOT_FOUND = 1,
    OTSARDB_JOURNAL_CONFLICT = 2,
    OTSARDB_JOURNAL_INVALID = 3,
    OTSARDB_JOURNAL_ERROR = 4,
    OTSARDB_JOURNAL_AMBIGUOUS = 5,
    OTSARDB_JOURNAL_UNSUPPORTED = 6
} otsardb_journal_result;

otsardb_journal_result otsardb_journal_read_head(otsardb_object_store *store,
                                              const char *database_root,
                                              otsardb_head *out_head,
                                              char **out_etag);

otsardb_journal_result otsardb_journal_publish_head(otsardb_object_store *store,
                                                 const char *database_root,
                                                 const otsardb_head *head,
                                                 const char *expected_etag,
                                                 int create_if_missing,
                                                 char **out_etag);

otsardb_journal_result otsardb_journal_publish_head_unconditional(otsardb_object_store *store,
                                                               const char *database_root,
                                                               const otsardb_head *head,
                                                               char **out_etag);

int otsardb_head_encode_json(const otsardb_head *head, char **out_json, size_t *out_size);
int otsardb_head_decode_json(const char *json, size_t size, otsardb_head *out_head);

#ifdef __cplusplus
}
#endif

#endif

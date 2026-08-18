#ifndef OTSARDB_CHECKPOINT_MANIFEST_H
#define OTSARDB_CHECKPOINT_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_CHECKPOINT_MANIFEST_FORMAT_VERSION 1u
#define OTSARDB_CHECKPOINT_PAGE_GROUP_KEY_MAX 128u
#define OTSARDB_CHECKPOINT_MAX_PAGE_GROUPS 128u
#define OTSARDB_CHECKPOINT_ID_MAX 63
#define OTSARDB_CHECKPOINT_SHA256_HEX_SIZE 64

typedef struct otsardb_checkpoint_page_group_ref {
    char key[OTSARDB_CHECKPOINT_PAGE_GROUP_KEY_MAX + 1];
    char sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    uint64_t size;
} otsardb_checkpoint_page_group_ref;

typedef struct otsardb_checkpoint_manifest {
    uint32_t format_version;
    uint64_t covered_generation;
    uint32_t page_size;
    uint64_t database_size_pages;
    char database_id[OTSARDB_CHECKPOINT_ID_MAX + 1];
    char base_commit_id[OTSARDB_CHECKPOINT_ID_MAX + 1];
    char base_manifest_sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    uint32_t page_group_count;
    otsardb_checkpoint_page_group_ref page_groups[OTSARDB_CHECKPOINT_MAX_PAGE_GROUPS];
} otsardb_checkpoint_manifest;

int otsardb_checkpoint_manifest_encode(const otsardb_checkpoint_manifest *manifest,
                                     char **out_json,
                                     size_t *out_size,
                                     char out_sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1]);

int otsardb_checkpoint_manifest_decode(const char *json, size_t size,
                                     otsardb_checkpoint_manifest *out);

/* --- CHECKPOINT.json pointer --- */

typedef struct otsardb_checkpoint_pointer {
    uint64_t covered_generation;
    char checkpoint_sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    char checkpoint_key[1024];
    char database_id[OTSARDB_CHECKPOINT_ID_MAX + 1];
} otsardb_checkpoint_pointer;

typedef enum {
    OTSARDB_CHECKPOINT_PTR_OK = 0,
    OTSARDB_CHECKPOINT_PTR_NOT_FOUND = 1,
    OTSARDB_CHECKPOINT_PTR_ERROR = 4
} otsardb_checkpoint_ptr_result;

#include "object_store.h"

otsardb_checkpoint_ptr_result otsardb_checkpoint_ptr_read(otsardb_object_store *store,
                                                       const char *database_root,
                                                       otsardb_checkpoint_pointer *out,
                                                       char **out_etag);

int otsardb_checkpoint_ptr_encode(const otsardb_checkpoint_pointer *ptr,
                                char **out_json, size_t *out_size);
int otsardb_checkpoint_ptr_decode(const char *json, size_t size,
                                otsardb_checkpoint_pointer *out);

#include "journal.h"
otsardb_journal_result otsardb_checkpoint_ptr_publish(otsardb_object_store *store,
                                                   const char *database_root,
                                                   const otsardb_checkpoint_pointer *ptr,
                                                   const char *expected_etag,
                                                   int create_if_missing,
                                                   char **out_etag);

#ifdef __cplusplus
}
#endif

#endif

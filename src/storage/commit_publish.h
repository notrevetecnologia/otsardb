#ifndef OTSARDB_COMMIT_PUBLISH_H
#define OTSARDB_COMMIT_PUBLISH_H

#include "cipher.h"
#include "commit_manifest.h"
#include "journal.h"
#include "object_store.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum otsardb_write_strategy {
    OTSARDB_WRITE_STRATEGY_DEFAULT = 0,
    OTSARDB_WRITE_STRATEGY_CAS = 1,
    OTSARDB_WRITE_STRATEGY_SINGLE_WRITER = 2
} otsardb_write_strategy;

typedef struct otsardb_segment_input {
    const void *payload;
    size_t payload_size;
    /* v4 page set for this segment: sorted, deduped, 1-based page numbers
     * covered by payload. The publisher fills it per chunk. NULL with
     * page_count 0 means "unknown page set" — the manifest then records
     * "pages":[] and conflict detection must treat it as overlapping
     * everything. */
    const uint32_t *pages;
    uint32_t page_count;
} otsardb_segment_input;

typedef struct otsardb_commit_request {
    const char *database_root;
    const char *database_id;
    const char *commit_id;
    const char *parent_commit_id;
    const char *parent_manifest_sha256;
    uint64_t parent_generation;
    uint32_t page_size;
    uint64_t database_size_pages;
    /* Legacy single-segment convenience: segment_payload + size OR the
     * segments array, never both. New publication uses the array so a
     * very large transaction can be split into bounded chunks. */
    const void *segment_payload;
    size_t segment_payload_size;
    const otsardb_segment_input *segments;
    uint32_t segment_count;
    /* Optional rolling DB checksum of the state AFTER this commit applies.
     * Empty string = not provided (chain predates checksums). The manifest
     * records it and recovery validates it. */
    const char *post_apply_checksum;
    const char *expected_head_etag;
    int create_head_if_missing;
    otsardb_write_strategy write_strategy;
    /* at-rest segment encryption. NULL = plaintext publish
     * (byte-identical pre-0079 behavior). When set, every segment object of
     * this commit is stored as the AES-256-GCM envelope
     * [iv(12)][ciphertext][tag(16)] and the manifest gains the additive
     * top-level "segment_enc" array (one entry per segment). The WAL
     * publisher path (which memsets the request) passes NULL; the session's
     * cipher is then resolved from the thread-local registry set by
     * s3_database.cpp at open. */
    const struct otsardb_cipher *cipher;
} otsardb_commit_request;

typedef enum otsardb_commit_state {
    OTSARDB_COMMIT_STATE_UNKNOWN = 0,
    OTSARDB_COMMIT_STATE_COMMITTED = 1,
    OTSARDB_COMMIT_STATE_NOT_COMMITTED = 2,
    OTSARDB_COMMIT_STATE_SUPERSEDED = 3
} otsardb_commit_state;

typedef struct otsardb_commit_result {
    otsardb_head head;
    /* Built (not decoded) manifest. Its segments[i].pages pointers are
     * BORROWED from the request's segment inputs: they must not be freed and
     * are only valid while the caller keeps the request arrays alive. */
    otsardb_commit_manifest manifest;
    char head_etag[128];
} otsardb_commit_result;

otsardb_journal_result otsardb_publish_commit(otsardb_object_store *store,
                                          const otsardb_commit_request *request,
                                          otsardb_commit_result *out_result);

otsardb_journal_result otsardb_resolve_commit(otsardb_object_store *store,
                                          const char *database_root,
                                          const char *commit_id,
                                          uint64_t parent_generation,
                                          otsardb_commit_state *out_state);

/* M3 conflict detection: classify a CAS publish failure as a
 * genuine page-overlap conflict or a stale-writer retry. Walks the committed
 * chain from the current remote HEAD down to request->parent_generation
 * (same walking pattern as otsardb_resolve_commit) and reports whether any
 * interleaved manifest shares a page with the request's per-segment page
 * sets.
 *
 * out_state receives the current resolve state of the request's commit
 * (COMMITTED / NOT_COMMITTED / SUPERSEDED, mirroring otsardb_resolve_commit).
 * out_page_overlap is set to 1 when the intersection is non-empty, when the
 * request carries no page info (legacy single-segment path: pages == NULL),
 * or when any interleaved manifest carries no page info (conservative
 * unknown-page-set rule from the conservative-overlap design). */
otsardb_journal_result otsardb_publish_commit_detect_conflict(otsardb_object_store *store,
                                                          const char *database_root,
                                                          const otsardb_commit_request *request,
                                                          otsardb_commit_state *out_state,
                                                          int *out_page_overlap);

#ifdef __cplusplus
}
#endif

#endif

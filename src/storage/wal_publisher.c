#include "wal_publisher.h"

#include "checksum.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "recovery.h"
#include "replica.h"
#include "segment.h"
#include "wal_batch.h"
#include "sqlite3.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* parallel segment pre-upload uses the project thread
 * convention (s3_object_store.cpp workers, s3_database.cpp
 * lease thread, forward.c server thread): _beginthreadex +
 * WaitForSingleObject on Windows, pthread on POSIX (the GitHub Actions S3
 * workflow builds Linux). */
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

/* parallel segment pre-upload (defined in the appendix
 * below): uploads all segments of a multi-segment commit through
 * otsardb_store_put_stream in waves of `parallel` workers before
 * otsardb_publish_commit runs. Returns 1 on success; on failure *out_rc
 * carries the journal-level cause. */
static int wal_publisher_preupload_segments(otsardb_wal_publisher *publisher,
                                            const char *commit_id,
                                            const otsardb_segment_input *inputs,
                                            uint32_t segment_count,
                                            int parallel,
                                            otsardb_journal_result *out_rc);

/* env-gated wave size for parallel segment pre-upload
 * (default 2, clamp 1..8, 0 = off). Forward declaration so the strict
 * GCC build (C99+, no implicit declarations) accepts the static
 * definition below the first call site. */
static int parallel_segments_limit(void);

typedef struct publish_scan_context {
    otsardb_wal_publisher *publisher;
    uint32_t commit_index;
    int sqlite_rc;
    /* Rolling checksum computation state, carried across commits of one
     * sync: checksum begins as the state checksum and is updated per
     * published commit. */
    uint64_t checksum;
    int checksum_valid;
    otsardb_sqlite_wal_read_at db_read_at;
    void *db_read_ctx;
} publish_scan_context;

typedef struct counted_reader {
    otsardb_sqlite_wal_read_at read_at;
    void *read_ctx;
    uint64_t bytes_read;
} counted_reader;

/* Session page map: page_no -> CRC contribution of the page's current value.
 * Grows as pages are touched; linear scan is acceptable for correctness-first
 * (bounded by pages touched per session). */
static uint64_t *page_map_get(otsardb_wal_publisher *p,
                              uint32_t page_no,
                              int *found) {
    for (uint32_t i = 0; i < p->page_contrib_count; ++i) {
        if (p->page_contrib_nos[i] == page_no) {
            if (found) *found = 1;
            return &p->page_contrib_values[i];
        }
    }
    if (found) *found = 0;
    return NULL;
}

static int page_map_set(otsardb_wal_publisher *p,
                        uint32_t page_no,
                        uint64_t contrib) {
    uint64_t *slot = page_map_get(p, page_no, NULL);
    if (slot) {
        *slot = contrib;
        return 1;
    }
    uint32_t next = p->page_contrib_count;
    uint32_t *new_nos = (uint32_t *)realloc(p->page_contrib_nos,
                                            (size_t)(next + 1u) * sizeof(uint32_t));
    uint64_t *new_vals = (uint64_t *)realloc(p->page_contrib_values,
                                             (size_t)(next + 1u) * sizeof(uint64_t));
    if (!new_nos || !new_vals) {
        free(new_nos ? new_nos : p->page_contrib_nos);
        free(new_vals ? new_vals : p->page_contrib_values);
        p->page_contrib_nos = NULL;
        p->page_contrib_values = NULL;
        p->page_contrib_count = 0;
        return 0;
    }
    p->page_contrib_nos = new_nos;
    p->page_contrib_values = new_vals;
    p->page_contrib_nos[next] = page_no;
    p->page_contrib_values[next] = contrib;
    p->page_contrib_count = next + 1u;
    return 1;
}

static void page_map_clear(otsardb_wal_publisher *p) {
    free(p->page_contrib_nos);
    free(p->page_contrib_values);
    p->page_contrib_nos = NULL;
    p->page_contrib_values = NULL;
    p->page_contrib_count = 0;
}

/* Read one page's OLD bytes from the local db file and return its CRC
 * contribution (page_no || old_page). Returns:
 * 1 contribution computed from the file's actual bytes;
 * 0 page is beyond the current file size (did not exist before this
 * commit â€” old contribution is 0, nothing to XOR out).
 */
static int old_page_contrib(otsardb_wal_publisher *p,
                            uint32_t page_no,
                            uint32_t page_size,
                            otsardb_sqlite_wal_read_at db_read_at,
                            void *db_read_ctx,
                            uint64_t *out_contrib) {
    if (!db_read_at) return 0;
    unsigned char *buf = (unsigned char *)malloc(page_size ? page_size : 1u);
    if (!buf) return 0;
    memset(buf, 0, page_size);
    uint64_t offset = ((uint64_t)page_no - 1u) * (uint64_t)page_size;
    if (offset > (uint64_t)INT64_MAX ||
        !db_read_at(db_read_ctx, offset, buf, page_size)) {
        free(buf);
        return 0;
    }
    *out_contrib = otsardb_checksum_accumulate(0, page_no, buf, page_size);
    free(buf);
    return 1;
}

typedef struct memory_reader {
    const unsigned char *data;
    size_t size;
} memory_reader;

static int counted_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    counted_reader *reader = (counted_reader *)ctx;
    if (!reader || !reader->read_at || !reader->read_at(reader->read_ctx, offset, buffer, size)) {
        return 0;
    }
    reader->bytes_read += size;
    return 1;
}

static int memory_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    memory_reader *reader = (memory_reader *)ctx;
    if (!reader || !buffer || offset > reader->size || size > reader->size - (size_t)offset) {
        return 0;
    }
    if (size) memcpy(buffer, reader->data + (size_t)offset, size);
    return 1;
}

static otsardb_write_strategy choose_strategy(otsardb_object_store *store,
                                            otsardb_write_strategy requested) {
    if (requested != OTSARDB_WRITE_STRATEGY_DEFAULT) return requested;
    if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE) &&
        otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_UPDATE)) {
        return OTSARDB_WRITE_STRATEGY_CAS;
    }
    return OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t current_wal_head_commit_index(const otsardb_wal_publisher *publisher) {
    if (!publisher || !publisher->wal_generation_known ||
        publisher->current_commit_id[0] == '\0') {
        return 0;
    }

    unsigned int salt1 = 0;
    unsigned int salt2 = 0;
    unsigned int commit_index = 0;
    char trailing = '\0';
    int matched = sscanf(publisher->current_commit_id,
                         "w%8x%8x-%8x%c",
                         &salt1,
                         &salt2,
                         &commit_index,
                         &trailing);
    if (matched != 3 || salt1 != publisher->wal_salt1 || salt2 != publisher->wal_salt2) {
        return 0;
    }
    return commit_index;
}

static int refresh_head(otsardb_wal_publisher *publisher) {
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(publisher->store,
                                                       publisher->database_root,
                                                       &head,
                                                       &etag);
    if (rc == OTSARDB_JOURNAL_NOT_FOUND) {
        publisher->head_loaded = 1;
        publisher->current_generation = 0;
        publisher->current_commit_id[0] = '\0';
        publisher->current_manifest_sha256[0] = '\0';
        publisher->current_head_etag[0] = '\0';
        free(etag);
        return SQLITE_OK;
    }
    if (rc != OTSARDB_JOURNAL_OK) {
        free(etag);
        return SQLITE_IOERR_READ;
    }
    if (strcmp(head.database_id, publisher->database_id) != 0 ||
        strlen(head.commit_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
        free(etag);
        return SQLITE_MISMATCH;
    }

    /* Load the parent's rolling checksum (if the chain carries one) so the
     * next commit can XOR-in its pages. The page map is preserved when the
     * remote HEAD is unchanged (same chain, same local image). */
    int chain_changed = publisher->head_loaded &&
                        (publisher->current_generation != head.generation ||
                         strcmp(publisher->current_commit_id, head.commit_id) != 0);

    /* (H3): the rolling-checksum re-base below must never be
     * dropped silently. Record whether the chain carried a checksum before
     * this refresh so a re-base that cannot be applied on a checksum-bearing
     * chain can fail the sync instead of publishing a checksum-less commit
     * (silently degrading recovery's image gate for that chain segment —
     * failure-mode audit H3/B5). */
    const int prev_checksum_known = publisher->checksum_known;
    const uint64_t prev_state_checksum = publisher->state_checksum;
    const uint32_t prev_verify_page_size = publisher->local_image_verify_page_size;
    const uint64_t prev_verify_page_count = publisher->local_image_verify_page_count;
    const int prev_head_loaded = publisher->head_loaded;
    /* (dual-publish retry): while a replica-phase failure
     * left a commit pending (cache rolled back to its parent), the head
     * reaching OUR OWN pending commit must NOT advance the cached parent
     * state — the retry re-runs the SAME commit id against the parent and
     * the HEAD CAS preconditions-fails against our own HEAD. The page map
     * and the local-image proof are preserved too (they describe exactly
     * the state the retry re-computes). Every OTHER chain change keeps the
     * pre-0072 behavior: adopt the head, clear the map, drop the proof. */
    int our_pending_retry = publisher->dual_pending &&
                            head.commit_id[0] != '\0' &&
                            strcmp(publisher->dual_pending_commit_id,
                                   head.commit_id) == 0;
    publisher->head_loaded = 1;
    if (!our_pending_retry) {
        publisher->current_generation = head.generation;
        snprintf(publisher->current_commit_id,
                 sizeof(publisher->current_commit_id),
                 "%s",
                 head.commit_id);
        snprintf(publisher->current_manifest_sha256,
                 sizeof(publisher->current_manifest_sha256),
                 "%s",
                 head.commit_sha256);
        snprintf(publisher->current_head_etag,
                 sizeof(publisher->current_head_etag),
                 "%s",
                 etag ? etag : "");
    }
    free(etag);

    if (chain_changed && !our_pending_retry) {
        page_map_clear(publisher);
        /* a different chain head means the page map refers
         * to a discarded image; the local-image proof must be re-established
         * before the next checksum-contributing publish. */
        publisher->local_image_verified = 0;
    }
    publisher->checksum_known = 0;
    publisher->state_checksum = 0;
    publisher->local_image_verify_page_size = 0;
    publisher->local_image_verify_page_count = 0;
    if (head.generation > 0) {
        /* Same chain head we already hold the checksum for: preserve the
         * base. The manifest GET below would be redundant, and a transient
         * failure on it must not degrade a healthy checksum-bearing chain to
         * checksum-less (H3 behavior). */
        int head_unchanged = prev_head_loaded && !chain_changed;
        if (head_unchanged && prev_checksum_known) {
            publisher->checksum_known = 1;
            publisher->state_checksum = prev_state_checksum;
            publisher->local_image_verify_page_size = prev_verify_page_size;
            publisher->local_image_verify_page_count = prev_verify_page_count;
            return SQLITE_OK;
        }
        /* Rolling-checksum re-base from the freshly read head manifest:
         * identical to the adopted cache in the normal path, and OUR OWN
         * pending commit in the dual-retry path (its post-apply checksum is
         * exactly the base the retry recomputes from the preserved map). */
        char manifest_key[OTSARDB_KEY_MAX + 1];
        int key_len = snprintf(manifest_key,
                               sizeof(manifest_key),
                               "%s/commits/%s.json",
                               publisher->database_root,
                               head.commit_id);
        if (key_len <= 0 || (size_t)key_len >= sizeof(manifest_key)) {
            fprintf(stderr,
                    "otsardb: refresh_head: malformed head manifest key; the "
                    "rolling-checksum re-base could not be applied%s\n",
                    prev_checksum_known
                        ? " and the sync is FAILED because the chain was "
                          "checksum-bearing"
                        : "");
            if (prev_checksum_known) return SQLITE_IOERR_READ;
            return SQLITE_OK;
        }
        otsardb_store_object manifest_object = {0};
        otsardb_store_result get_rc =
            otsardb_store_get(publisher->store, manifest_key, &manifest_object);
        if (get_rc != OTSARDB_STORE_OK) {
            fprintf(stderr,
                    "otsardb: refresh_head: could not fetch the head manifest "
                    "%s (store rc=%d); the rolling-checksum re-base could not "
                    "be applied%s\n",
                    manifest_key, (int)get_rc,
                    prev_checksum_known
                        ? " and the sync is FAILED because the chain was "
                          "checksum-bearing"
                        : "");
            if (prev_checksum_known) return SQLITE_IOERR_READ;
            return SQLITE_OK;
        }
        otsardb_commit_manifest manifest = {0};
        if (!otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                               manifest_object.size,
                                               &manifest)) {
            fprintf(stderr,
                    "otsardb: refresh_head: the head manifest %s could not be "
                    "decoded; the rolling-checksum re-base could not be "
                    "applied%s\n",
                    manifest_key,
                    prev_checksum_known
                        ? " and the sync is FAILED because the chain was "
                          "checksum-bearing"
                        : "");
            otsardb_store_release_object(publisher->store, &manifest_object);
            if (prev_checksum_known) return SQLITE_IOERR_READ;
            return SQLITE_OK;
        }
        if (manifest.post_apply_checksum[0] != '\0') {
            uint64_t checksum = 0;
            if (otsardb_checksum_parse(manifest.post_apply_checksum, &checksum)) {
                publisher->state_checksum = checksum;
                publisher->checksum_known = 1;
                /* Stash the parent's page range so publish_one_commit
                 * knows which pages need an old value from the local
                 * file at all. */
                publisher->local_image_verify_page_size = manifest.page_size;
                publisher->local_image_verify_page_count = manifest.database_size_pages;
            } else {
                fprintf(stderr,
                        "otsardb: refresh_head: the head manifest %s carries an "
                        "invalid rolling checksum; the re-base could not be "
                        "applied%s\n",
                        manifest_key,
                        prev_checksum_known
                            ? " and the sync is FAILED because the chain was "
                              "checksum-bearing"
                            : "");
                otsardb_commit_manifest_free(&manifest);
                otsardb_store_release_object(publisher->store, &manifest_object);
                if (prev_checksum_known) return SQLITE_IOERR_READ;
                return SQLITE_OK;
            }
        } else if (prev_checksum_known) {
            /* The new head genuinely carries no checksum (the 0063
             * checksum-blocked / legacy path): this is the documented
             * degradation, not a dropped re-base — log it (B5 direction)
             * and continue checksum-less. */
            fprintf(stderr,
                    "otsardb: refresh_head: the head manifest %s carries no "
                    "post_apply_checksum; the chain degraded from "
                    "checksum-bearing to checksum-less (0063/legacy path)\n",
                    manifest_key);
        }
        otsardb_commit_manifest_free(&manifest);
        otsardb_store_release_object(publisher->store, &manifest_object);
    }
    return SQLITE_OK;
}

static int ensure_metadata(otsardb_wal_publisher *publisher, uint32_t page_size) {
    if (publisher->metadata_ready) return SQLITE_OK;

    otsardb_db_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.format_version = 1;
    metadata.page_size = page_size;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", publisher->database_id);

    otsardb_metadata_result rc = otsardb_db_metadata_create(publisher->store,
                                                         publisher->database_root,
                                                         &metadata);
    if (rc == OTSARDB_METADATA_OK) {
        publisher->metadata_ready = 1;
        return SQLITE_OK;
    }
    if (rc != OTSARDB_METADATA_CONFLICT) return SQLITE_IOERR_WRITE;

    otsardb_db_metadata existing;
    rc = otsardb_db_metadata_read(publisher->store, publisher->database_root, &existing);
    if (rc != OTSARDB_METADATA_OK ||
        existing.page_size != page_size ||
        strcmp(existing.database_id, publisher->database_id) != 0) {
        return SQLITE_MISMATCH;
    }

    publisher->metadata_ready = 1;
    return SQLITE_OK;
}

static int journal_result_to_sqlite(otsardb_journal_result rc) {
    switch (rc) {
        case OTSARDB_JOURNAL_OK: return SQLITE_OK;
        case OTSARDB_JOURNAL_CONFLICT: return SQLITE_BUSY;
        case OTSARDB_JOURNAL_INVALID: return SQLITE_CORRUPT;
        case OTSARDB_JOURNAL_NOT_FOUND: return SQLITE_NOTFOUND;
        case OTSARDB_JOURNAL_UNSUPPORTED:
        case OTSARDB_JOURNAL_AMBIGUOUS:
        case OTSARDB_JOURNAL_ERROR:
        default: return SQLITE_IOERR_WRITE;
    }
}

/* ---------------------------------------------------------------------------
 * bounded exponential-backoff retry for transient publish
 * failures.
 *
 * Policy (the retry policy): a publish attempt that fails with a
 * TRANSPORT-class result is re-attempted with the SAME commit request under
 * bounded exponential backoff. Retryable:
 * - OTSARDB_JOURNAL_ERROR â€” the store write/read failed at the transport
 * level; the object either never landed or landed unverified;
 * - OTSARDB_JOURNAL_AMBIGUOUS â€” a timeout left the commit identity unknown;
 * every attempt re-resolves by commit id BEFORE retrying, and a commit
 * that is already durable (COMMITTED) is reported as success with no
 * further attempts.
 * Re-attempting the same request is idempotent: segment and manifest PUTs
 * are conditional creates on content-addressed keys (an existing identical
 * object is accepted via the GET+compare in immutable_put), and the HEAD CAS
 * re-applies the SAME parent etag, so it can only advance HEAD once â€” there
 * is no duplicate HEAD and no double-commit. NEVER retried (the fencing /
 * authoritative results): OTSARDB_JOURNAL_CONFLICT (the HEAD CAS precondition
 * failure), NOT_FOUND, INVALID (validation failure), UNSUPPORTED (capability
 * absent). On final failure the FIRST attempt's error is returned unchanged:
 * the sync fails and the commit is not acknowledged (the durability
 * contract). The verified-parent-state invariant (
 * 0063) is untouched: the rolling checksum is computed once per commit
 * before the retry cycle; a retry re-runs only the store calls.
 *
 * ADAPT of the probe-retry pattern (3 attempts) and the AWS
 * SDK v2 standard retry mode (bounded exponential backoff) — see
 * .
 * ------------------------------------------------------------------------- */

/* OTSARDB_PUBLISH_RETRIES: total publish attempts INCLUDING the first
 * (default 3, clamped 1..10; 1 disables retry). Garbage/unset -> 3. */
static int publish_retries_limit(void) {
    const char *value = getenv("OTSARDB_PUBLISH_RETRIES");
    if (!value || value[0] == '\0') return 3;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0') return 3;
    if (n < 1) return 1;
    if (n > 10) return 10;
    return (int)n;
}

/* OTSARDB_PUBLISH_RETRY_BASE_MS: backoff base delay (default 250 ms, clamped
 * 0..60000). Delays between attempts double: base, 2*base, 4*base, ... */
static long publish_retry_base_ms(void) {
    const char *value = getenv("OTSARDB_PUBLISH_RETRY_BASE_MS");
    if (!value || value[0] == '\0') return 250L;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0') return 250L;
    if (n < 0) return 0L;
    if (n > 60000L) return 60000L;
    return n;
}

static int publish_rc_retryable(otsardb_journal_result rc) {
    return rc == OTSARDB_JOURNAL_ERROR || rc == OTSARDB_JOURNAL_AMBIGUOUS;
}

/* exported for the replica module (replica.c reuses the
 * SAME policy: OTSARDB_PUBLISH_RETRIES / OTSARDB_PUBLISH_RETRY_BASE_MS). */
int otsardb_wal_publisher_rc_retryable(otsardb_journal_result rc) {
    return publish_rc_retryable(rc);
}

static void retry_sleep_ms(long delay_ms) {
    if (delay_ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)delay_ms);
#else
    struct timespec ts;
    ts.tv_sec = delay_ms / 1000L;
    ts.tv_nsec = (delay_ms % 1000L) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* One bounded retry cycle over an idempotent journal-level store write.
 * `attempt` returns 1 when the operation durably succeeded (out_rc carries
 * the accompanying result), 0 on failure. The cycle stops on the first
 * non-retryable result and after `attempts` tries; on final failure out_rc
 * carries the FIRST attempt's result (the "original error" of the durability
 * contract). */
static int publish_retry_cycle(int (*attempt)(void *opaque, otsardb_journal_result *out_rc),
                               void *opaque,
                               otsardb_journal_result *out_rc) {
    int attempts = publish_retries_limit();
    long delay_ms = publish_retry_base_ms();
    otsardb_journal_result first_rc = OTSARDB_JOURNAL_ERROR;
    for (int i = 1; i <= attempts; ++i) {
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        if (attempt(opaque, &rc)) {
            *out_rc = rc;
            return 1;
        }
        if (i == 1) first_rc = rc;
        if (!publish_rc_retryable(rc)) {
            *out_rc = rc;
            return 0;
        }
        if (i < attempts) retry_sleep_ms(delay_ms);
        delay_ms *= 2;
    }
    *out_rc = first_rc;
    return 0;
}

/* exported for the replica module — the replica phase
 * reuses the SAME bounded retry cycle and policy knobs as the primary
 * publish stages (the retry policy). */
int otsardb_wal_publisher_retry_cycle(int (*attempt)(void *opaque,
                                                   otsardb_journal_result *out_rc),
                                    void *opaque,
                                    otsardb_journal_result *out_rc) {
    return publish_retry_cycle(attempt, opaque, out_rc);
}

typedef struct publish_attempt_ctx {
    otsardb_wal_publisher *publisher;
    const otsardb_commit_request *request;
    otsardb_commit_result result;
    /* Success came from commit-identity resolution of an AMBIGUOUS timeout
     * (the caller refreshes the cached HEAD instead of consuming result). */
    int identity_resolved;
} publish_attempt_ctx;

/* One publish attempt of the whole segment/manifest/HEAD sequence. Returns 1
 * when the commit is durably published: otsardb_publish_commit returned OK, or
 * it returned AMBIGUOUS and otsardb_resolve_commit classifies the commit
 * COMMITTED by identity (the durability contract). */
static int publish_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    publish_attempt_ctx *attempt_ctx = (publish_attempt_ctx *)opaque;
    attempt_ctx->identity_resolved = 0;
    memset(&attempt_ctx->result, 0, sizeof(attempt_ctx->result));
    otsardb_journal_result rc = otsardb_publish_commit(attempt_ctx->publisher->store,
                                                   attempt_ctx->request,
                                                   &attempt_ctx->result);
    if (rc == OTSARDB_JOURNAL_OK) {
        *out_rc = rc;
        return 1;
    }
    if (rc == OTSARDB_JOURNAL_AMBIGUOUS) {
        otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
        if (otsardb_resolve_commit(attempt_ctx->publisher->store,
                                 attempt_ctx->publisher->database_root,
                                 attempt_ctx->request->commit_id,
                                 attempt_ctx->request->parent_generation,
                                 &state) == OTSARDB_JOURNAL_OK &&
            state == OTSARDB_COMMIT_STATE_COMMITTED) {
            attempt_ctx->identity_resolved = 1;
            *out_rc = rc;
            return 1;
        }
    }
    *out_rc = rc;
    return 0;
}

/* the recovery/materialization anchor. The native open and
 * takeover paths (s3_database.cpp) call otsardb_wal_publisher_set_local_image_generation
 * immediately after otsardb_recovery_restore_store / otsardb_lazy_restore_store
 * materialised the local execution image at the remote HEAD. While the head
 * does not exceed the anchor, the live file IS the parent image for every
 * page the session never touched (first-touch old-page reads are exact); the
 * 0024 generation fence refuses writes once another writer advances the
 * head. A full-file checksum comparison against the parent manifest was
 * considered and rejected: in WAL mode SQLite writes pages only to the WAL,
 * so the local file lags at the last checkpoint and NEVER byte-equals the
 * parent state mid-session â€” the comparison fails on healthy sessions
 * (observed during test development). */
static int local_image_proof_current(const otsardb_wal_publisher *publisher) {
    return publisher->local_image_generation_known &&
           publisher->local_image_generation <= publisher->current_generation;
}

/* ---------------------------------------------------------------------------
 * — Model B synchronous dual publish (
 * section 6;).
 *
 * When a replica destination is configured (OTSARDB_S3_REPLICA_* environment),
 * the publish path becomes DUAL: the primary publish above is UNCHANGED
 * (segments -> manifest -> HEAD CAS), then — before the ack — the replica
 * phase runs (src/storage/replica.c): replicate the commit's objects to the
 * replica, write the PREPARED ledger on BOTH destinations, CAS the replica
 * HEAD to the SAME commit id, write the COMMITTED ledger on BOTH
 * destinations. Only then does the sync return success: the durability
 * contract (/0021/0065) becomes ack == COMMITTED ledger on
 * both destinations.
 *
 * Failure contract (0072): primary OK + replica FAIL -> the primary ledger
 * marks ABORTED (best-effort) and the sync FAILS — the commit is NOT acked
 * (INCONCLUSIVE per 0021: the commit exists on the primary; a later retry
 * re-resolves by identity). The publisher state rolls back to the parent so
 * the 0065 retry semantics cover the WHOLE dual operation: a retry re-runs
 * the same commit id, the primary re-verifies via GET+compare, the HEAD CAS
 * preconditions-fails on our own commit and the conflict interception below
 * re-enters the replica phase.
 *
 * When NO replica is configured, every branch below is dead code: the
 * single-destination path is byte-identical to pre-0072 behavior.
 * ------------------------------------------------------------------------- */

/* Run the replica phase for a commit already durably published on the
 * primary. Lazy-opens the replica handle on first use (fail-closed: an open
 * failure fails the sync, never a silent single-destination fallback).
 * Returns 1 on success; 0 with *out_rc carrying the journal-level cause.
 *
 * The parent identity is passed from the pre-publish STASH: request fields
 * that borrow publisher buffers (parent_commit_id points into
 * current_commit_id) are overwritten by the publish success path before the
 * phase runs, so they cannot be read from the request here. */
static int wal_publisher_run_replica_phase(otsardb_wal_publisher *publisher,
                                           const otsardb_commit_request *request,
                                           const char *parent_commit_id,
                                           const char *parent_manifest_sha256,
                                           otsardb_journal_result *out_rc) {
    if (!publisher || !request || !out_rc) {
        if (out_rc) *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    if (!publisher->replica) {
        char err[256];
        err[0] = '\0';
        publisher->replica = otsardb_replica_open(publisher->store,
                                                publisher->database_root,
                                                publisher->database_id,
                                                err, sizeof(err));
        if (!publisher->replica) {
            fprintf(stderr,
                    "otsardb: replica open failed (fail-closed; sync fails): %s\n",
                    err[0] != '\0' ? err : "unknown error");
            *out_rc = OTSARDB_JOURNAL_ERROR;
            return 0;
        }
    }

    return otsardb_replica_replicate_commit(publisher->replica,
                                          publisher->store,
                                          publisher->database_root,
                                          request->commit_id,
                                          request->parent_generation + 1u,
                                          request->parent_generation,
                                          parent_commit_id,
                                          parent_manifest_sha256,
                                          publisher->current_manifest_sha256,
                                          publisher->current_head_etag,
                                          request->segments,
                                          request->segment_count,
                                          out_rc);
}

/* Roll the publisher's cached HEAD state back to the commit's parent after a
 * replica-phase failure. The page map and the rolling checksum base are NOT
 * rolled back: they now describe the failed commit's state, which is exactly
 * what the retry re-computes (old == new contributions cancel out and the
 * re-published manifest is byte-identical, so GET+compare accepts it). The
 * dual-pending identity is recorded so refresh_head preserves the map when
 * the head reaches our own commit again. */
static void wal_publisher_rollback_after_replica_failure(otsardb_wal_publisher *publisher,
                                                         const char *commit_id,
                                                         uint64_t parent_generation,
                                                         const char *parent_commit_id,
                                                         const char *parent_sha256,
                                                         const char *parent_head_etag,
                                                         uint32_t parent_published_count) {
    publisher->current_generation = parent_generation;
    snprintf(publisher->current_commit_id,
             sizeof(publisher->current_commit_id), "%s", parent_commit_id);
    snprintf(publisher->current_manifest_sha256,
             sizeof(publisher->current_manifest_sha256), "%s", parent_sha256);
    snprintf(publisher->current_head_etag,
             sizeof(publisher->current_head_etag), "%s", parent_head_etag);
    publisher->head_loaded = 1;
    publisher->published_commit_count = parent_published_count;
    publisher->dual_pending = 1;
    snprintf(publisher->dual_pending_commit_id,
             sizeof(publisher->dual_pending_commit_id), "%s", commit_id);
    if (publisher->local_image_generation_known) {
        publisher->local_image_generation = parent_generation;
    }
}

/* content-identity verification for the dual-publish retry.
 * The primary publish of a rolled-back commit re-runs the SAME commit id
 * (deterministic salt+index). Normally the WAL frames are unchanged and the
 * attempt reproduces the exact published manifest (accepted by GET+compare,
 * HEAD CAS preconditions-fails on our own HEAD). But if the WAL was
 * truncated/rewritten between the failure and the retry, the same commit id
 * would name DIFFERENT content — the retry must then NOT ack. Verify that
 * the manifest durably published on the primary is content-identical to what
 * THIS attempt publishes: manifest identity fields, parent identity, rolling
 * checksum, and every segment's re-encoded SHA-256/size/page set. Returns 1
 * when identical (the retry may re-enter the replica phase). */
static int wal_publisher_primary_matches_request(otsardb_wal_publisher *publisher,
                                                 const otsardb_commit_request *request,
                                                 uint32_t page_size,
                                                 uint64_t database_size_pages,
                                                 const char *checksum_hex,
                                                 int checksum_computed,
                                                 const char *parent_commit_id,
                                                 const char *parent_manifest_sha256) {
    char manifest_key[OTSARDB_KEY_MAX + 1];
    {
        size_t root_len = strlen(publisher->database_root);
        int needs_slash = root_len > 0 &&
                          publisher->database_root[root_len - 1] != '/';
        int n = snprintf(manifest_key, sizeof(manifest_key),
                         "%s%scommits/%s.json",
                         publisher->database_root,
                         needs_slash ? "/" : "",
                         request->commit_id);
        if (n <= 0 || (size_t)n >= sizeof(manifest_key)) return 0;
    }

    otsardb_store_object object = {0};
    if (otsardb_store_get(publisher->store, manifest_key, &object) != OTSARDB_STORE_OK) return 0;

    otsardb_commit_manifest manifest = {0};
    int decoded = otsardb_commit_manifest_decode_json((const char *)object.data,
                                                    object.size,
                                                    &manifest);
    if (!decoded) {
        otsardb_store_release_object(publisher->store, &object);
        return 0;
    }

    int match = manifest.generation == request->parent_generation + 1u &&
                strcmp(manifest.commit_id, request->commit_id) == 0 &&
                manifest.page_size == page_size &&
                manifest.database_size_pages == database_size_pages;
    if (match && request->parent_generation == 0) {
        match = manifest.parent_generation == 0 &&
                manifest.parent_commit_id[0] == '\0';
    } else if (match) {
        /* The parent identity is passed explicitly: request.parent_commit_id
         * and request.parent_manifest_sha256 are POINTERS into the
         * publisher's cached HEAD buffers, which the dual-retry CONFLICT
         * intercept overwrites with the commit's OWN identity before this
         * guard runs — comparing through the request would always mismatch
         * (the request's "parent" would be the child itself). */
        match = manifest.parent_generation == request->parent_generation &&
                strcmp(manifest.parent_commit_id,
                       parent_commit_id ? parent_commit_id : "") == 0 &&
                strcmp(manifest.parent_manifest_sha256,
                       parent_manifest_sha256 ? parent_manifest_sha256 : "") == 0;
    }
    if (match) {
        if (checksum_computed) {
            match = strcmp(manifest.post_apply_checksum, checksum_hex) == 0;
        } else {
            match = manifest.post_apply_checksum[0] == '\0';
        }
    }
    if (match && manifest.segment_count == request->segment_count) {
        for (uint32_t i = 0; i < request->segment_count && match; ++i) {
            unsigned char *segment_data = NULL;
            size_t segment_size = 0;
            char segment_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
            if (!otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                                  request->commit_id,
                                                  request->segments[i].payload,
                                                  request->segments[i].payload_size,
                                                  &segment_data,
                                                  &segment_size,
                                                  segment_sha256,
                                                  1,
                                                  NULL)) {
                match = 0;
                break;
            }
            const otsardb_manifest_segment_ref *ref = &manifest.segments[i];
            match = strcmp(ref->sha256, segment_sha256) == 0 &&
                    ref->size == (uint64_t)segment_size;
            if (match && ref->page_count > 0 && request->segments[i].pages) {
                match = ref->page_count == request->segments[i].page_count &&
                        memcmp(ref->pages, request->segments[i].pages,
                               ref->page_count * sizeof(uint32_t)) == 0;
            }
            free(segment_data);
        }
    } else if (match) {
        match = 0;
    }

    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(publisher->store, &object);
    return match;
}

static int publish_one_commit(uint32_t page_size,
                              uint64_t database_size_pages,
                              const otsardb_sqlite_wal_frame_ref *frames,
                              uint32_t frame_count,
                              otsardb_sqlite_wal_read_at read_at,
                              void *read_ctx,
                              void *ctx_ptr) {
    publish_scan_context *ctx = (publish_scan_context *)ctx_ptr;
    otsardb_wal_publisher *publisher = ctx->publisher;
    ++ctx->commit_index;

    int rc = ensure_metadata(publisher, page_size);
    if (rc != SQLITE_OK) {
        ctx->sqlite_rc = rc;
        return 1;
    }

    /* Bound the memory needed to encode this commit: page images are re-read
     * from the already-fsynced WAL through read_at in chunks of at most
     * segment_payload_limit encoded bytes each. Frame references only cost
     * 12 bytes per frame in the scanner. */
    size_t frame_record_size = OTSARDB_WAL_BATCH_FRAME_HEADER_SIZE + (size_t)page_size;
    uint64_t limit = publisher->segment_payload_limit;
    size_t frames_per_chunk = limit > 0 ? (size_t)(limit / frame_record_size) : 0;
    if (frames_per_chunk == 0) frames_per_chunk = 1;

    uint32_t chunk_count = (uint32_t)((frame_count + (uint32_t)frames_per_chunk - 1u) /
                                      (uint32_t)frames_per_chunk);
    if (chunk_count > OTSARDB_MANIFEST_MAX_SEGMENTS) {
        ctx->sqlite_rc = SQLITE_TOOBIG;
        return 1;
    }

    otsardb_segment_input inputs[OTSARDB_MANIFEST_MAX_SEGMENTS];
    memset(inputs, 0, sizeof(inputs));

    uint32_t *chunk_page_nos = NULL;
    unsigned char *chunk_pages = NULL;
    unsigned char *batches[OTSARDB_MANIFEST_MAX_SEGMENTS];
    memset(batches, 0, sizeof(batches));
    /* Per-chunk sorted/deduped page sets handed to the commit request; freed
     * after publication because the manifest refs only borrow them. */
    uint32_t *chunk_page_sets[OTSARDB_MANIFEST_MAX_SEGMENTS];
    memset(chunk_page_sets, 0, sizeof(chunk_page_sets));

    int ok = 1;
    for (uint32_t c = 0; c < chunk_count; ++c) {
        uint32_t first = c * (uint32_t)frames_per_chunk;
        uint32_t chunk_frames = frame_count - first;
        if (chunk_frames > (uint32_t)frames_per_chunk) chunk_frames = (uint32_t)frames_per_chunk;

        chunk_page_nos = (uint32_t *)realloc(chunk_page_nos,
                                             (size_t)chunk_frames * sizeof(uint32_t));
        chunk_pages = (unsigned char *)realloc(chunk_pages,
                                               (size_t)chunk_frames * (size_t)page_size);
        if (!chunk_page_nos || !chunk_pages) {
            ctx->sqlite_rc = SQLITE_NOMEM;
            ok = 0;
            break;
        }

        for (uint32_t i = 0; i < chunk_frames; ++i) {
            const otsardb_sqlite_wal_frame_ref *ref = &frames[first + i];
            chunk_page_nos[i] = ref->page_no;
            if (ref->frame_offset > UINT64_MAX - OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE ||
                !read_at(read_ctx,
                         ref->frame_offset + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE,
                         chunk_pages + (size_t)i * page_size,
                         page_size)) {
                ctx->sqlite_rc = SQLITE_IOERR_READ;
                ok = 0;
                break;
            }
        }
        if (!ok) break;

        if (!otsardb_wal_batch_encode(page_size,
                                    database_size_pages,
                                    chunk_page_nos,
                                    chunk_pages,
                                    chunk_frames,
                                    &batches[c],
                                    &inputs[c].payload_size)) {
            ctx->sqlite_rc = SQLITE_NOMEM;
            ok = 0;
            break;
        }
        inputs[c].payload = batches[c];

        /* Page set for this segment: sort the chunk's frame page numbers and
         * dedupe (a page can appear more than once in one commit). */
        uint32_t *page_set = (uint32_t *)malloc((size_t)chunk_frames * sizeof(uint32_t));
        if (!page_set) {
            ctx->sqlite_rc = SQLITE_NOMEM;
            ok = 0;
            break;
        }
        memcpy(page_set, chunk_page_nos, (size_t)chunk_frames * sizeof(uint32_t));
        qsort(page_set, chunk_frames, sizeof(uint32_t), cmp_u32);
        uint32_t unique_pages = 0;
        for (uint32_t i = 0; i < chunk_frames; ++i) {
            if (unique_pages == 0 || page_set[i] != page_set[unique_pages - 1]) {
                page_set[unique_pages++] = page_set[i];
            }
        }
        chunk_page_sets[c] = page_set;
        inputs[c].pages = page_set;
        inputs[c].page_count = unique_pages;
    }

    free(chunk_page_nos);
    free(chunk_pages);

    if (!ok) {
        for (uint32_t c = 0; c < chunk_count; ++c) {
            free(batches[c]);
            free(chunk_page_sets[c]);
        }
        return 1;
    }

    char commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    int commit_len = snprintf(commit_id,
                              sizeof(commit_id),
                              "w%08x%08x-%08x",
                              publisher->wal_salt1,
                              publisher->wal_salt2,
                              ctx->commit_index);
    if (commit_len <= 0 || (size_t)commit_len >= sizeof(commit_id)) {
        for (uint32_t c = 0; c < chunk_count; ++c) {
            free(batches[c]);
            free(chunk_page_sets[c]);
        }
        ctx->sqlite_rc = SQLITE_TOOBIG;
        return 1;
    }

    /* M2 lease fence: only the current lease holder may publish (design
     *). The callback is process-local; the
     * HEAD CAS below is the physical barrier that fences a stolen lease. */
    if (publisher->lease_fence && !publisher->lease_fence(publisher->lease_ctx)) {
        ctx->sqlite_rc = SQLITE_BUSY;
        return 1;
    }

    /* parallel multipart segment pre-upload. For commits
     * split into two or more segments, the segment objects are uploaded
     * through otsardb_store_put_stream (parallel multipart parts, 
     * 0039) in waves of OTSARDB_S3_PARALLEL_SEGMENTS concurrent workers, and
     * ONLY then is the commit request handed to otsardb_publish_commit (whose
     * own uploads then verify the pre-uploaded objects, followed by the
     * manifest PUT and the HEAD CAS - ordering and CAS fencing unchanged).
     * A failure aborts the remaining waves and fails the sync: no manifest
     * is written and HEAD never advances. Gated on the store reporting
     * OTSARDB_STORE_CAP_MULTIPART (probe-gated) and on the store exposing
     * put_stream; everything else keeps the current sequential PUT path. */
    if (chunk_count >= 2u &&
        otsardb_store_supports(publisher->store, OTSARDB_STORE_CAP_MULTIPART) &&
        publisher->store->ops && publisher->store->ops->put_stream) {
        int parallel = parallel_segments_limit();
        if (parallel > 0) {
            otsardb_journal_result preupload_rc = OTSARDB_JOURNAL_OK;
            if (!wal_publisher_preupload_segments(publisher,
                                                  commit_id,
                                                  inputs,
                                                  chunk_count,
                                                  parallel,
                                                  &preupload_rc)) {
                for (uint32_t c = 0; c < chunk_count; ++c) {
                    free(batches[c]);
                    free(chunk_page_sets[c]);
                }
                ctx->sqlite_rc = journal_result_to_sqlite(preupload_rc);
                return 1;
            }
        }
    }

    otsardb_commit_request request;
    memset(&request, 0, sizeof(request));
    request.database_root = publisher->database_root;
    request.database_id = publisher->database_id;
    request.commit_id = commit_id;
    request.parent_commit_id = publisher->current_generation == 0
                                   ? NULL
                                   : publisher->current_commit_id;
    request.parent_manifest_sha256 = publisher->current_generation == 0
                                         ? NULL
                                         : publisher->current_manifest_sha256;
    request.parent_generation = publisher->current_generation;
    request.page_size = page_size;
    request.database_size_pages = database_size_pages;
    request.segments = inputs;
    request.segment_count = chunk_count;
    request.create_head_if_missing = publisher->current_generation == 0;
    request.write_strategy = publisher->strategy;
    if (publisher->strategy == OTSARDB_WRITE_STRATEGY_CAS && publisher->current_generation != 0) {
        request.expected_head_etag = publisher->current_head_etag;
    }

    /* Rolling checksum: XOR out old page contributions, XOR in the new ones.
     * Recovery starts from an EMPTY state and applies segments in order, so:
     * - genesis: base = 0; pages never seen have old contribution = 0
     * (the local file may contain SQLite-internal pages that were never
     * published â€” never read it for the base).
     * - chained commits: base = parent manifest's checksum; a page first
     * touched now has its old contribution read from the local file,
     * which recovery materialised exactly at the parent state. */
    char checksum_hex[OTSARDB_CHECKSUM_HEX_SIZE + 1];
    int checksum_computed = 0;
    if (publisher->checksum_known && ctx->checksum_valid) {
        ctx->checksum = publisher->state_checksum;
    } else if (publisher->current_generation == 0) {
        ctx->checksum = 0;
    }
    if (publisher->checksum_known || publisher->current_generation == 0) {
        unsigned char *new_page = (unsigned char *)malloc(page_size ? page_size : 1u);
        if (new_page) {
            int ok = 1;
            int checksum_blocked = 0;
            for (uint32_t i = 0; i < frame_count && ok; ++i) {
                const otsardb_sqlite_wal_frame_ref *ref = &frames[i];
                if (!read_at(read_ctx,
                             ref->frame_offset + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE,
                             new_page,
                             page_size)) {
                    ok = 0;
                    break;
                }
                int have_old = 0;
                uint64_t *old_contrib = page_map_get(publisher, ref->page_no, &have_old);
                if (!have_old) {
                    uint64_t from_db = 0;
                    if (publisher->current_generation > 0 &&
                        (uint64_t)ref->page_no <= publisher->local_image_verify_page_count) {
                        /* First touch of a page INSIDE the parent state on a
                         * chained commit: the old value can only come from the
                         * live local file, which is legitimate only when the
                         * file is proven to be the parent image (
                         * 0063: recovery/materialization anchor or retry
                         * re-materialisation). Without the proof the file may
                         * be a fresh/disposable execution image â€” reading it
                         * would publish a checksum wrong by a constant that
                         * fails the next recovery. */
                        if (publisher->local_image_verified &&
                            old_page_contrib(publisher, ref->page_no, page_size,
                                             ctx->db_read_at, ctx->db_read_ctx,
                                             &from_db)) {
                            ctx->checksum ^= from_db;
                        } else {
                            checksum_blocked = 1;
                        }
                    }
                    /* Page beyond the parent state (genesis or growth):
                     * old contribution is 0 â€” nothing to XOR out. */
                } else {
                    ctx->checksum ^= *old_contrib;
                }
                (void)old_contrib;
                uint64_t new_contrib = otsardb_checksum_accumulate(0, ref->page_no,
                                                                 new_page, page_size);
                ctx->checksum ^= new_contrib;
                if (!page_map_set(publisher, ref->page_no, new_contrib)) {
                    ok = 0;
                    break;
                }
            }
            free(new_page);
            if (ok && !checksum_blocked) {
                otsardb_checksum_hex(ctx->checksum, checksum_hex);
                request.post_apply_checksum = checksum_hex;
                checksum_computed = 1;
            } else if (checksum_blocked) {
                /* The local image is unverifiable: NEVER publish a checksum
                 * that is not the true checksum of the published state. An
                 * absent checksum means "chain predates checksums" to
                 * recovery (gate skipped), so the durability contract is
                 * preserved. Drop the base so no later commit of this sync
                 * (whose parent chain now carries no checksum) publishes
                 * one either. */
                publisher->checksum_known = 0;
                publisher->state_checksum = 0;
                ctx->checksum_valid = 0;
            }
        }
    }

    /* capture the parent HEAD state before the publish
     * cycle. On a replica-phase failure the publisher state rolls back here
     * so the 0065 retry semantics cover the whole dual operation: a retry
     * re-runs the SAME commit id and the primary re-verifies via GET+compare.
     * The page map and the rolling checksum base are deliberately NOT part
     * of the rollback (see wal_publisher_rollback_after_replica_failure). */
    uint64_t parent_generation = publisher->current_generation;
    char parent_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char parent_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    char parent_head_etag[128];
    uint32_t parent_published_count = publisher->published_commit_count;
    snprintf(parent_commit_id, sizeof(parent_commit_id), "%s",
             publisher->current_commit_id);
    snprintf(parent_sha256, sizeof(parent_sha256), "%s",
             publisher->current_manifest_sha256);
    snprintf(parent_head_etag, sizeof(parent_head_etag), "%s",
             publisher->current_head_etag);

    publish_attempt_ctx attempt_ctx;
    memset(&attempt_ctx, 0, sizeof(attempt_ctx));
    attempt_ctx.publisher = publisher;
    attempt_ctx.request = &request;
    otsardb_commit_result result;
    memset(&result, 0, sizeof(result));
    otsardb_journal_result publish_rc = OTSARDB_JOURNAL_ERROR;
    int published = publish_retry_cycle(publish_attempt_once, &attempt_ctx, &publish_rc);
    if (published) result = attempt_ctx.result;

    /* Fix (Linux/GCC portability): the page sets referenced
     * by `request.segments` are freed only after the conflict/ambiguity
     * handling below â€” otsardb_publish_commit_detect_conflict reads them.
     * Freeing earlier was a use-after-free that MSVC's allocator happened
     * to mask (stale bytes) and glibc exposed (reused heap -> garbage page
     * list -> OTSARDB_JOURNAL_INVALID -> spurious SQLITE_CORRUPT). */
    int publish_finish_rc = 1;
    if (published) {
        /* an ambiguous timeout whose commit resolved
         * COMMITTED by identity. Refresh the cached head exactly like the
         * former direct AMBIGUOUS path — the head manifest re-bases the
         * rolling checksum and etag state — and count the commit. The
         * publisher's head state stays consistent for the next sync. */
        if (attempt_ctx.identity_resolved) {
            rc = refresh_head(publisher);
            if (rc != SQLITE_OK) {
                ctx->sqlite_rc = rc;
                goto publish_cleanup;
            }
            publisher->published_commit_count = ctx->commit_index;
        } else {
            publisher->current_generation = result.head.generation;
            if (publisher->local_image_generation_known) {
                publisher->local_image_generation = result.head.generation;
            }
            if (checksum_computed) {
                publisher->checksum_known = 1;
                publisher->state_checksum = ctx->checksum;
            }
            snprintf(publisher->current_commit_id,
                     sizeof(publisher->current_commit_id),
                     "%s",
                     result.head.commit_id);
            snprintf(publisher->current_manifest_sha256,
                     sizeof(publisher->current_manifest_sha256),
                     "%s",
                     result.head.commit_sha256);
            snprintf(publisher->current_head_etag,
                     sizeof(publisher->current_head_etag),
                     "%s",
                     result.head_etag);
            publisher->head_loaded = 1;
            publisher->published_commit_count = ctx->commit_index;
        }

        /* (Model B synchronous dual publish): the primary is
         * durable; BEFORE the ack, run the replica phase. Only when BOTH
         * destinations hold all objects AND the COMMITTED ledger entry does
         * the sync return success (ack == both durable). On replica failure:
         * best-effort ABORTED marker on the primary ledger, roll the cached
         * HEAD state back to the parent, and fail the sync — the commit is
         * NOT acked (INCONCLUSIVE: the commit exists on
         * the primary; a retry re-resolves by identity). */
        if (publisher->replica || otsardb_replica_configured()) {
            otsardb_journal_result replica_rc = OTSARDB_JOURNAL_ERROR;
            if (!wal_publisher_run_replica_phase(publisher, &request,
                                                 parent_commit_id,
                                                 parent_sha256,
                                                 &replica_rc)) {
                otsardb_replica_mark_aborted(publisher->store,
                                           publisher->database_root,
                                           commit_id,
                                           request.parent_generation + 1u,
                                           request.parent_generation,
                                           publisher->current_manifest_sha256,
                                           publisher->current_head_etag);
                wal_publisher_rollback_after_replica_failure(publisher,
                                                             commit_id,
                                                             parent_generation,
                                                             parent_commit_id,
                                                             parent_sha256,
                                                             parent_head_etag,
                                                             parent_published_count);
                ctx->sqlite_rc = journal_result_to_sqlite(replica_rc);
                goto publish_cleanup;
            }
        }
        if (publisher->dual_pending &&
            strcmp(publisher->dual_pending_commit_id, commit_id) == 0) {
            /* The dual operation completed: both destinations hold the
             * commit and the COMMITTED ledger — the retry bookkeeping is
             * no longer pending. */
            publisher->dual_pending = 0;
            publisher->dual_pending_commit_id[0] = '\0';
        }
        publish_finish_rc = 0;
        goto publish_cleanup;
    }

    if (publish_rc == OTSARDB_JOURNAL_AMBIGUOUS) {
        otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
        otsardb_journal_result resolve_rc = otsardb_resolve_commit(publisher->store,
                                                               publisher->database_root,
                                                               commit_id,
                                                               request.parent_generation,
                                                               &state);
        if (resolve_rc == OTSARDB_JOURNAL_OK && state == OTSARDB_COMMIT_STATE_COMMITTED) {
            rc = refresh_head(publisher);
            if (rc != SQLITE_OK) {
                ctx->sqlite_rc = rc;
                goto publish_cleanup;
            }
            publisher->published_commit_count = ctx->commit_index;
            publish_finish_rc = 0;
            goto publish_cleanup;
        }
        ctx->sqlite_rc = SQLITE_IOERR_WRITE;
        goto publish_cleanup;
    }

    if (publish_rc == OTSARDB_JOURNAL_CONFLICT) {
        /* (dual-publish retry): when a replica phase failed
         * for THIS commit in an earlier sync, the rollback re-runs the same
         * commit id. The primary publish then re-verifies the immutable
         * objects via GET+compare (accepted) and the HEAD CAS preconditions-
         * fails against the commit's own HEAD. That CONFLICT is OUR OWN
         * previous attempt — verify by identity and re-enter the replica
         * phase instead of classifying it as a fencing conflict. The page
         * map and checksum base are preserved (the commit's state is exactly
         * what the retry re-computes); only the cached HEAD fields advance.
         * A primary HEAD that is NOT this commit id falls through to the
         * normal conflict handling below. */
        int dual_retry_handled = 0;
        if (publisher->replica || otsardb_replica_configured()) {
            otsardb_head head;
            char *head_etag = NULL;
            otsardb_journal_result head_rc = otsardb_journal_read_head(publisher->store,
                                                                   publisher->database_root,
                                                                   &head,
                                                                   &head_etag);
            if (head_rc == OTSARDB_JOURNAL_OK &&
                strcmp(head.commit_id, commit_id) == 0 &&
                head.generation == request.parent_generation + 1u) {
                publisher->current_generation = head.generation;
                snprintf(publisher->current_commit_id,
                         sizeof(publisher->current_commit_id), "%s", head.commit_id);
                snprintf(publisher->current_manifest_sha256,
                         sizeof(publisher->current_manifest_sha256), "%s", head.commit_sha256);
                snprintf(publisher->current_head_etag,
                         sizeof(publisher->current_head_etag), "%s",
                         head_etag ? head_etag : "");
                publisher->head_loaded = 1;
                free(head_etag);

                /* Content-identity guard: the primary commit must be exactly
                 * what THIS attempt would publish (the WAL is normally
                 * unchanged, but a truncated-and-rewritten WAL could reuse
                 * the commit id for different frames — that must NOT ack).
                 * A mismatch falls through to the normal conflict handling. */
                if (wal_publisher_primary_matches_request(publisher,
                                                          &request,
                                                          page_size,
                                                          database_size_pages,
                                                          checksum_hex,
                                                          checksum_computed,
                                                          parent_commit_id,
                                                          parent_sha256)) {
                    otsardb_journal_result replica_rc = OTSARDB_JOURNAL_ERROR;
                    if (!wal_publisher_run_replica_phase(publisher, &request,
                                                         parent_commit_id,
                                                         parent_sha256,
                                                         &replica_rc)) {
                        otsardb_replica_mark_aborted(publisher->store,
                                                   publisher->database_root,
                                                   commit_id,
                                                   request.parent_generation + 1u,
                                                   request.parent_generation,
                                                   publisher->current_manifest_sha256,
                                                   publisher->current_head_etag);
                        wal_publisher_rollback_after_replica_failure(publisher,
                                                                     commit_id,
                                                                     parent_generation,
                                                                     parent_commit_id,
                                                                     parent_sha256,
                                                                     parent_head_etag,
                                                                     parent_published_count);
                        ctx->sqlite_rc = journal_result_to_sqlite(replica_rc);
                        goto publish_cleanup;
                    }
                    publisher->published_commit_count = ctx->commit_index;
                    publisher->dual_pending = 0;
                    publisher->dual_pending_commit_id[0] = '\0';
                    publish_finish_rc = 0;
                    goto publish_cleanup;
                }
                /* The primary commit is NOT what this attempt publishes: the
                 * WAL moved on. The dual retry is abandoned — clear the
                 * pending bookkeeping and classify this as a normal conflict
                 * (the application re-executes at the new HEAD; the pending
                 * commit remains durable-but-unacked on the primary). */
                publisher->dual_pending = 0;
                publisher->dual_pending_commit_id[0] = '\0';
                dual_retry_handled = 1;
            } else {
                free(head_etag);
            }
        }

        /* M3 conflict detection: a CAS failure alone does
         * not tell the client whether another writer touched pages this
         * commit also touches. Walk the interleaved manifest chain and
         * classify: genuine page overlap -> SQLITE_BUSY_SNAPSHOT (the
         * application must re-run against the new HEAD image); disjoint
         * pages -> plain SQLITE_BUSY (stale writer, retryable). */
        (void)dual_retry_handled;
        otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
        int page_overlap = 0;
        otsardb_journal_result detect_rc = otsardb_publish_commit_detect_conflict(
            publisher->store,
            publisher->database_root,
            &request,
            &state,
            &page_overlap);
        if (detect_rc != OTSARDB_JOURNAL_OK) {
            ctx->sqlite_rc = journal_result_to_sqlite(detect_rc);
            goto publish_cleanup;
        }
        if (page_overlap) {
            publisher->sqlite_extended_conflict = 1;
            ctx->sqlite_rc = SQLITE_BUSY_SNAPSHOT;
            goto publish_cleanup;
        }
        ctx->sqlite_rc = SQLITE_BUSY;
        goto publish_cleanup;
    }

    /* Every other failure (non-retryable result, or retryable result whose
     * budget was exhausted) fails the sync with the mapped error: the commit
     * is not acknowledged (durability contract). */
    ctx->sqlite_rc = journal_result_to_sqlite(publish_rc);

publish_cleanup:
    for (uint32_t c = 0; c < chunk_count; ++c) {
        free(batches[c]);
        free(chunk_page_sets[c]);
    }
    return publish_finish_rc;
}

static void reset_wal_generation(otsardb_wal_publisher *publisher,
                                 const otsardb_sqlite_wal_header *header) {
    publisher->wal_generation_known = 1;
    publisher->wal_salt1 = header->salt1;
    publisher->wal_salt2 = header->salt2;
    publisher->published_commit_count = 0;
    otsardb_sqlite_wal_cursor_reset(&publisher->wal_cursor, header);
    publisher->wal_cursor_ready = 1;
    page_map_clear(publisher);
    /* the rolling-checksum base is NOT cleared here. This
     * reset runs inside sync_reader AFTER refresh_head, which has already
     * re-based state_checksum on the CURRENT remote head manifest (or
     * zeroed it for a headless chain); the cleared page map plus the
     * proof gate (checksum_blocked while
     * local_image_verified is unproven) make every publish that would need
     * an old-page file read checksum-less exactly as before, while pages
     * beyond the parent state (old contribution 0) still publish a correct
     * checksum. Keeping the base makes a FRESH session's first chained
     * publish byte-identical to a same-session retry of the same
     * deterministic commit id (the 0072 idempotency invariant holds across
     * sessions: the replica phase's immutable GET+compare re-verifies the
     * manifest bytes). */
    /* a WAL generation restart invalidates the dual-pending
     * bookkeeping (the pending commit id belonged to the old salts). */
    publisher->dual_pending = 0;
    publisher->dual_pending_commit_id[0] = '\0';
    /* a WAL generation restart (new salts) may also have
     * rewritten the live file; the local-image proof no longer holds. */
    publisher->local_image_verified = 0;
}

static int rebuild_cursor_from_head(otsardb_wal_publisher *publisher,
                                    otsardb_sqlite_wal_read_at read_at,
                                    void *read_ctx,
                                    uint64_t wal_size,
                                    uint32_t target_commit_count) {
    if (target_commit_count == 0) return SQLITE_OK;

    otsardb_sqlite_wal_scan_result result;
    if (!otsardb_sqlite_wal_scan_reader(read_at,
                                      read_ctx,
                                      wal_size,
                                      &publisher->wal_cursor,
                                      target_commit_count,
                                      NULL,
                                      NULL,
                                      &result)) {
        return SQLITE_IOERR_READ;
    }
    if (result.commit_count != target_commit_count) return SQLITE_CORRUPT;
    publisher->published_commit_count = target_commit_count;
    return SQLITE_OK;
}

int otsardb_wal_publisher_init(otsardb_wal_publisher *publisher,
                             otsardb_object_store *store,
                             const char *database_root,
                             const char *database_id,
                             otsardb_write_strategy strategy) {
    if (!publisher || !store || !database_root || !database_id ||
        database_root[0] == '\0' || database_id[0] == '\0' ||
        strlen(database_root) > OTSARDB_KEY_MAX ||
        strlen(database_id) > OTSARDB_COMMIT_ID_MAX) {
        return 0;
    }

    otsardb_write_strategy effective = choose_strategy(store, strategy);
    if (effective != OTSARDB_WRITE_STRATEGY_CAS && effective != OTSARDB_WRITE_STRATEGY_SINGLE_WRITER) {
        return 0;
    }
    if (effective == OTSARDB_WRITE_STRATEGY_CAS &&
        (!otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE) ||
         !otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_UPDATE))) {
        return 0;
    }

    memset(publisher, 0, sizeof(*publisher));
    publisher->store = store;
    publisher->strategy = effective;
    publisher->segment_payload_limit = OTSARDB_WAL_PUBLISHER_DEFAULT_SEGMENT_PAYLOAD_LIMIT;
    snprintf(publisher->database_root, sizeof(publisher->database_root), "%s", database_root);
    snprintf(publisher->database_id, sizeof(publisher->database_id), "%s", database_id);
    return 1;
}

void otsardb_wal_publisher_set_segment_payload_limit(otsardb_wal_publisher *publisher,
                                                   uint64_t limit_bytes) {
    if (!publisher) return;
    publisher->segment_payload_limit =
        limit_bytes == 0 ? OTSARDB_WAL_PUBLISHER_DEFAULT_SEGMENT_PAYLOAD_LIMIT : limit_bytes;
}

void otsardb_wal_publisher_set_local_image_generation(otsardb_wal_publisher *publisher,
                                                    uint64_t generation) {
    if (!publisher) return;
    publisher->local_image_generation = generation;
    publisher->local_image_generation_known = 1;
}

void otsardb_wal_publisher_set_lease_fence(otsardb_wal_publisher *publisher,
                                         int (*lease_fence)(void *lease_ctx),
                                         void *lease_ctx) {
    if (!publisher) return;
    publisher->lease_fence = lease_fence;
    publisher->lease_ctx = lease_ctx;
}

/* release the lazily-opened replica destination (its S3
 * store instance). Callers that own a publisher (the s3_database.cpp
 * session teardown) should call this; the replica module stays safe when it
 * is not called (the handle is reclaimed at process exit). */
void otsardb_wal_publisher_destroy(otsardb_wal_publisher *publisher) {
    if (!publisher) return;
    if (publisher->replica) {
        otsardb_replica_close(publisher->replica);
        publisher->replica = NULL;
    }
    page_map_clear(publisher);
}

/* operator promotion at open (OTSARDB_S3_PROMOTE=1). The
 * replica handle is lazily opened like the publish path (fail closed), then
 * otsardb_replica_promote runs the full protocol: old-primary lease check,
 * epoch CAS raise, full recovery from the promoted store. On success the
 * handle holds the raised epoch and *out_epoch reports it (the session
 * swaps the stores and proceeds — s3_database.cpp wiring is a documented
 * follow-up). */
int otsardb_wal_publisher_promote(otsardb_wal_publisher *publisher,
                                const char *local_db_path,
                                uint64_t *out_epoch,
                                char *err,
                                size_t err_size,
                                otsardb_journal_result *out_rc) {
    if (out_epoch) *out_epoch = 0;
    if (!publisher || !out_rc) {
        if (out_rc) *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    if (err && err_size > 0) err[0] = '\0';
    if (!local_db_path || local_db_path[0] == '\0') {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }

    if (!publisher->replica) {
        char open_err[256];
        open_err[0] = '\0';
        publisher->replica = otsardb_replica_open(publisher->store,
                                                publisher->database_root,
                                                publisher->database_id,
                                                open_err, sizeof(open_err));
        if (!publisher->replica) {
            if (err) {
                snprintf(err, err_size, "promotion: replica open failed: %s",
                         open_err[0] != '\0' ? open_err : "unknown error");
            }
            *out_rc = OTSARDB_JOURNAL_ERROR;
            return 0;
        }
    }

    if (!otsardb_replica_promote(publisher->replica, publisher->store,
                               local_db_path, 0, err, err_size, out_rc)) {
        return 0;
    }
    if (out_epoch) *out_epoch = otsardb_replica_epoch(publisher->replica);
    return 1;
}

int otsardb_wal_publisher_sync_reader(const char *wal_name,
                                    uint64_t wal_size,
                                    otsardb_sqlite_wal_read_at read_at,
                                    void *read_ctx,
                                    otsardb_sqlite_wal_read_at db_read_at,
                                    void *db_read_ctx,
                                    void *ctx) {
    (void)wal_name;
    otsardb_wal_publisher *publisher = (otsardb_wal_publisher *)ctx;
    if (!publisher || !publisher->store || !read_at) return SQLITE_MISUSE;
    publisher->sqlite_extended_conflict = 0;

    counted_reader reader = {read_at, read_ctx, 0};
    publisher->wal_bytes_read_last_sync = 0;

    if (wal_size == 0) {
        publisher->wal_generation_known = 0;
        publisher->wal_salt1 = 0;
        publisher->wal_salt2 = 0;
        publisher->published_commit_count = 0;
        publisher->wal_cursor_ready = 0;
        memset(&publisher->wal_cursor, 0, sizeof(publisher->wal_cursor));
        return SQLITE_OK;
    }

    otsardb_sqlite_wal_header header;
    if (!otsardb_sqlite_wal_read_header(counted_read_at, &reader, wal_size, &header)) {
        publisher->wal_bytes_read_last_sync = reader.bytes_read;
        publisher->wal_bytes_read_total += reader.bytes_read;
        return SQLITE_IOERR_READ;
    }

    int new_generation = !publisher->wal_generation_known ||
                         publisher->wal_salt1 != header.salt1 ||
                         publisher->wal_salt2 != header.salt2 ||
                         !publisher->wal_cursor_ready;

    /* Refresh the remote HEAD on every sync so a concurrent writer's commit
     * is visible here; then refuse writes when our local image is stale. */
    {
        int head_rc = refresh_head(publisher);
        if (head_rc != SQLITE_OK) {
            publisher->wal_bytes_read_last_sync = reader.bytes_read;
            publisher->wal_bytes_read_total += reader.bytes_read;
            return head_rc;
        }
    }

    if (publisher->local_image_generation_known &&
        publisher->current_generation > publisher->local_image_generation) {
        /* Another process advanced HEAD beyond the generation our local
         * execution image was materialised from. Writing now would base
         * new pages on a stale snapshot and could silently overwrite the
         * other writer's acknowledged commit. Refuse instead. */
        publisher->wal_bytes_read_last_sync = reader.bytes_read;
        publisher->wal_bytes_read_total += reader.bytes_read;
        return SQLITE_BUSY;
    }

    if (!new_generation) {
        uint64_t frame_size = 24u + (uint64_t)header.page_size;
        uint64_t cursor_end = 32u + (uint64_t)publisher->wal_cursor.frame_count * frame_size;
        if (cursor_end > wal_size) new_generation = 1;
    }

    if (new_generation) {
        reset_wal_generation(publisher, &header);
        uint32_t resume_count = current_wal_head_commit_index(publisher);
        int rebuild_rc = rebuild_cursor_from_head(publisher,
                                                  counted_read_at,
                                                  &reader,
                                                  wal_size,
                                                  resume_count);
        if (rebuild_rc != SQLITE_OK) {
            publisher->wal_bytes_read_last_sync = reader.bytes_read;
            publisher->wal_bytes_read_total += reader.bytes_read;
            return rebuild_rc;
        }
    }

    /* refresh_head may have resurrected the parent's rolling
     * checksum over an EMPTY page map (a header-only WAL sync reset the map,
     * then the next sync's refresh re-armed the base). Re-establish the
     * local-image proof from the recovery/materialization anchor so the
     * commits below may read old-page values from the live file; sessions
     * without the anchor (fresh/disposable local files) are denied by
     * publish_one_commit per commit. */
    if (publisher->checksum_known && !publisher->local_image_verified &&
        local_image_proof_current(publisher)) {
        publisher->local_image_verified = 1;
    }

    publish_scan_context publish_ctx;
    memset(&publish_ctx, 0, sizeof(publish_ctx));
    publish_ctx.publisher = publisher;
    publish_ctx.commit_index = publisher->published_commit_count;
    publish_ctx.sqlite_rc = SQLITE_OK;
    publish_ctx.db_read_at = db_read_at;
    publish_ctx.db_read_ctx = db_read_ctx;
    publish_ctx.checksum = publisher->checksum_known ? publisher->state_checksum : 0;
    publish_ctx.checksum_valid = publisher->checksum_known;

    otsardb_sqlite_wal_scan_result scan;
    if (!otsardb_sqlite_wal_scan_reader(counted_read_at,
                                      &reader,
                                      wal_size,
                                      &publisher->wal_cursor,
                                      0,
                                      publish_one_commit,
                                      &publish_ctx,
                                      &scan)) {
        publisher->wal_bytes_read_last_sync = reader.bytes_read;
        publisher->wal_bytes_read_total += reader.bytes_read;
        return publish_ctx.sqlite_rc != SQLITE_OK ? publish_ctx.sqlite_rc : SQLITE_IOERR_READ;
    }

    publisher->wal_bytes_read_last_sync = reader.bytes_read;
    publisher->wal_bytes_read_total += reader.bytes_read;
    return publish_ctx.sqlite_rc;
}

int otsardb_wal_publisher_sync(const char *wal_name,
                             const void *wal_data,
                             size_t wal_size,
                             void *ctx) {
    if (!wal_data && wal_size != 0) return SQLITE_IOERR_READ;
    memory_reader reader = {(const unsigned char *)wal_data, wal_size};
    return otsardb_wal_publisher_sync_reader(wal_name,
                                           (uint64_t)wal_size,
                                           memory_read_at,
                                           &reader,
                                           NULL,
                                           NULL,
                                           ctx);
}

/* ---------------------------------------------------------------------------
 * M3 phase 3: explicit re-materialise-and-retry for
 * disjoint-page conflicts. Public entry points; the static helpers below are
 * appended so publish_one_commit and sync_reader above stay untouched.
 * ------------------------------------------------------------------------- */

typedef struct retry_file_reader {
    FILE *file;
} retry_file_reader;

/* Random-access reader over one local file (the re-materialised database for
 * the checksum base, or the fsynced WAL for frame page images). */
static int retry_file_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    retry_file_reader *reader = (retry_file_reader *)ctx;
    if (!reader || !reader->file || offset > (uint64_t)LONG_MAX) return 0;
    if (fseek(reader->file, (long)offset, SEEK_SET) != 0) return 0;
    return fread(buffer, 1, size, reader->file) == size;
}

/* Build a page set from the frame references (sorted, deduped â€” the v4
 * invariant publish_one_commit enforces for its manifest page sets). */
static uint32_t *retry_build_page_set(const otsardb_sqlite_wal_frame_ref *frames,
                                      uint32_t frame_count,
                                      uint32_t *out_page_count) {
    uint32_t *pages = (uint32_t *)malloc((size_t)frame_count * sizeof(uint32_t));
    if (!pages) return NULL;
    for (uint32_t i = 0; i < frame_count; ++i) pages[i] = frames[i].page_no;
    qsort(pages, frame_count, sizeof(uint32_t), cmp_u32);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < frame_count; ++i) {
        if (unique == 0 || pages[i] != pages[unique - 1]) pages[unique++] = pages[i];
    }
    *out_page_count = unique;
    return pages;
}

/* Classify the interleaved commits between the remote HEAD and
 * parent_generation against our page set, using the shared M3 conflict walk
 * (otsardb_publish_commit_detect_conflict). Returns 1 on
 * success with *out_overlap set, 0 when the chain could not be classified
 * (store error / invalid chain â€” the caller treats it as an error). */
static int retry_conflict_check(otsardb_wal_publisher *publisher,
                                uint64_t parent_generation,
                                const char *parent_commit_id,
                                const char *parent_manifest_sha256,
                                const uint32_t *our_pages,
                                uint32_t our_page_count,
                                int *out_overlap) {
    otsardb_segment_input segments[1];
    memset(segments, 0, sizeof(segments));
    segments[0].pages = our_pages;
    segments[0].page_count = our_page_count;

    otsardb_commit_request request;
    memset(&request, 0, sizeof(request));
    request.database_root = publisher->database_root;
    request.database_id = publisher->database_id;
    request.commit_id = parent_commit_id && parent_commit_id[0] != '\0'
                            ? parent_commit_id
                            : "otsardb-retry-check";
    request.parent_generation = parent_generation;
    request.parent_commit_id = parent_generation == 0 ? NULL : parent_commit_id;
    request.parent_manifest_sha256 = parent_generation == 0 ? NULL : parent_manifest_sha256;
    request.create_head_if_missing = parent_generation == 0;
    request.segments = segments;
    request.segment_count = 1;

    otsardb_commit_state state = OTSARDB_COMMIT_STATE_UNKNOWN;
    int overlap = 0;
    otsardb_journal_result rc = otsardb_publish_commit_detect_conflict(publisher->store,
                                                                   publisher->database_root,
                                                                   &request,
                                                                   &state,
                                                                   &overlap);
    if (rc != OTSARDB_JOURNAL_OK) return 0;
    *out_overlap = overlap;
    return 1;
}

int otsardb_wal_publisher_refresh_head(otsardb_wal_publisher *publisher) {
    if (!publisher || !publisher->store) return SQLITE_MISUSE;
    return refresh_head(publisher);
}

/* ---------------------------------------------------------------------------
 * parallel multipart segment publish (multi-segment
 * commits).
 *
 * otsardb_publish_commit uploads the segments of one commit strictly
 * sequentially (its immutable_put loop). For commits split into two or more
 * segments (transaction payload above segment_payload_limit) this publisher
 * pre-uploads every segment through otsardb_store_put_stream - the windowed
 * parallel multipart path - in waves of
 * OTSARDB_S3_PARALLEL_SEGMENTS (default 2, clamp [1, 8]) concurrent workers,
 * then hands the UNCHANGED commit request to otsardb_publish_commit. That
 * call's own per-segment uploads then hit the conditional-create refusal
 * (412) on the pre-uploaded object and resolve via the existing GET +
 * byte-compare: the pre-uploaded bytes are identical (same encoder, same
 * commit id, same content-addressed key), so publication proceeds exactly
 * as before - segments verified, then manifest, then HEAD CAS. Only the
 * upload parallelism changes.
 *
 * Ordering and fencing are unchanged:
 * - all segments are durably uploaded (or the commit fails) BEFORE the
 * manifest PUT and the HEAD CAS inside otsardb_publish_commit;
 * - a failed segment aborts the remaining waves, no manifest is written
 * and HEAD never advances (the lease fence above runs first, so a
 * fenced publisher uploads nothing);
 * - retries/ambiguous publishes are idempotent: a segment that already
 * exists with identical bytes is accepted by the same resolution used
 * by commit_publish.c's immutable_put.
 *
 * Memory bound: one encoded segment buffer per in-flight worker (each
 * bounded by segment_payload_limit plus encoder overhead; at the default
 * 16 MiB budget and OTSARDB_S3_PARALLEL_SEGMENTS=2 that is up to ~32 MiB)
 * plus, inside each put_stream call, one window of buffered parts
 * (OTSARDB_S3_MP_PARALLEL x 5 MiB, up to 16 x 5 MiB at the clamp ceiling).
 * ------------------------------------------------------------------------- */

/* OTSARDB_S3_PARALLEL_SEGMENTS: how many segments of one
 * commit upload concurrently through put_stream. Default 2; clamped to
 * [1, 8]; 0 disables the pre-upload entirely (the current sequential PUT
 * path of otsardb_publish_commit runs unchanged - diagnostic/escape hatch).
 * Garbage/unset -> 2.
 *
 * Measured on Cloudflare R2 (2026-08-11, 60k-row two-segment commit,
 * 3 samples per setting): PARALLEL=1 median 23.5 s vs PARALLEL=2 median
 * 21.4 s; link variance is ~2x run-to-run (recorded).
 * The fan-out is the net win on loopback/edge deployments and neutral to
 * mildly positive on the real link, so 2 is the default. */
static int parallel_segments_limit(void) {
    const char *value = getenv("OTSARDB_S3_PARALLEL_SEGMENTS");
    if (!value || value[0] == '\0') return 2;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0') return 2;
    if (n < 0) return 2;
    if (n == 0) return 0;
    if (n > 8) return 8;
    return (int)n;
}

/* Pull source over one encoded segment buffer. The source is only ever
 * read on the thread that owns the put_stream call (each pre-upload worker
 * owns its buffer and its put_stream; the multipart window workers never
 * invoke the pull callback - contract), so no locking is
 * needed. */
typedef struct preupload_segment_source {
    const unsigned char *data;
    size_t size;
    size_t pos;
} preupload_segment_source;

static long preupload_segment_read(void *ctx, void *buffer, size_t max_size) {
    preupload_segment_source *src = (preupload_segment_source *)ctx;
    if (!src || !buffer || src->pos >= src->size) return 0;
    size_t n = src->size - src->pos;
    if (n > max_size) n = max_size;
    memcpy(buffer, src->data + src->pos, n);
    src->pos += n;
    return (long)n;
}

static otsardb_journal_result preupload_existing_matches(otsardb_object_store *store,
                                                       const char *key,
                                                       const void *data,
                                                       size_t size) {
    otsardb_store_object existing;
    memset(&existing, 0, sizeof(existing));
    otsardb_store_result rc = otsardb_store_get(store, key, &existing);
    if (rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_JOURNAL_NOT_FOUND;
    if (rc != OTSARDB_STORE_OK) return OTSARDB_JOURNAL_ERROR;
    int identical = existing.size == size &&
                    (size == 0 || memcmp(existing.data, data, size) == 0);
    otsardb_store_release_object(store, &existing);
    return identical ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_CONFLICT;
}

/* Mirror of commit_publish.c's immutable_put over otsardb_store_put_stream:
 * conditional create on the multipart init, PRECONDITION_FAILED / TIMEOUT
 * resolved by GET + byte compare on the content-addressed key. S3-Core
 * stores without conditional create get the verify-then-write branch,
 * exactly like immutable_put. */
static otsardb_journal_result preupload_immutable_put_stream(otsardb_object_store *store,
                                                           const char *key,
                                                           const void *data,
                                                           size_t size) {
    if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
        otsardb_store_put_options options;
        memset(&options, 0, sizeof(options));
        options.if_none_match = 1;
        preupload_segment_source source = {(const unsigned char *)data, size, 0};
        otsardb_store_result rc = otsardb_store_put_stream(store,
                                                       key,
                                                       preupload_segment_read,
                                                       &source,
                                                       (uint64_t)size,
                                                       &options,
                                                       NULL);
        if (rc == OTSARDB_STORE_OK) return OTSARDB_JOURNAL_OK;
        if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
            return preupload_existing_matches(store, key, data, size);
        }
        if (rc == OTSARDB_STORE_TIMEOUT) {
            otsardb_journal_result resolved = preupload_existing_matches(store, key, data, size);
            return resolved == OTSARDB_JOURNAL_NOT_FOUND ? OTSARDB_JOURNAL_AMBIGUOUS : resolved;
        }
        if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
        return OTSARDB_JOURNAL_ERROR;
    }

    otsardb_journal_result existing = preupload_existing_matches(store, key, data, size);
    if (existing == OTSARDB_JOURNAL_OK || existing == OTSARDB_JOURNAL_CONFLICT) return existing;
    if (existing != OTSARDB_JOURNAL_NOT_FOUND) return existing;

    otsardb_store_put_options options;
    memset(&options, 0, sizeof(options));
    preupload_segment_source source = {(const unsigned char *)data, size, 0};
    otsardb_store_result rc = otsardb_store_put_stream(store,
                                                   key,
                                                   preupload_segment_read,
                                                   &source,
                                                   (uint64_t)size,
                                                   &options,
                                                   NULL);
    if (rc == OTSARDB_STORE_OK) return OTSARDB_JOURNAL_OK;
    if (rc == OTSARDB_STORE_TIMEOUT) {
        otsardb_journal_result resolved = preupload_existing_matches(store, key, data, size);
        return resolved == OTSARDB_JOURNAL_NOT_FOUND ? OTSARDB_JOURNAL_AMBIGUOUS : resolved;
    }
    if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
    return OTSARDB_JOURNAL_ERROR;
}

typedef struct preupload_attempt_ctx {
    otsardb_object_store *store;
    const char *key;
    const void *data;
    size_t size;
} preupload_attempt_ctx;

/* One pre-upload attempt of one segment through otsardb_store_put_stream.
 * Re-attempts are idempotent: the key is content-addressed and the bytes are
 * identical, so a retry after a partial/ambiguous upload either completes
 * the object or accepts the existing identical object via the GET+compare
 * inside preupload_immutable_put_stream. */
static int preupload_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    preupload_attempt_ctx *attempt_ctx = (preupload_attempt_ctx *)opaque;
    otsardb_journal_result rc = preupload_immutable_put_stream(attempt_ctx->store,
                                                             attempt_ctx->key,
                                                             attempt_ctx->data,
                                                             attempt_ctx->size);
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK;
}

/* One pre-upload job: encode the segment with the SAME encoder and args
 * commit_publish.c uses (otsardb_segment_encode_with_frames, same commit id,
 * frame table NULL - the object bytes are identical), derive the
 * content-addressed key, and upload immutably via put_stream. The batch
 * payload is borrowed from publish_one_commit's inputs and stays alive
 * until every worker has been joined. */
typedef struct preupload_job {
    otsardb_object_store *store;
    const char *database_root;
    const char *commit_id;
    const void *batch;
    size_t batch_size;
    otsardb_journal_result rc;
} preupload_job;

static int preupload_job_run(preupload_job *job) {
    if (!job || !job->store || !job->database_root || !job->commit_id) {
        if (job) job->rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    unsigned char *segment_data = NULL;
    size_t segment_size = 0;
    char segment_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    if (!otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                          job->commit_id,
                                          job->batch,
                                          job->batch_size,
                                          &segment_data,
                                          &segment_size,
                                          segment_sha256,
                                          1,
                                          NULL)) {
        job->rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }

    char segment_suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(segment_suffix,
                     sizeof(segment_suffix),
                     "segments/sha256/%.2s/%s.otsseg",
                     segment_sha256,
                     segment_sha256);
    if (n <= 0 || (size_t)n >= sizeof(segment_suffix)) {
        free(segment_data);
        job->rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }

    /* Same root-join rule as commit_publish.c's root_join(). */
    size_t root_len = strlen(job->database_root);
    int needs_slash = root_len > 0 && job->database_root[root_len - 1] != '/';
    char key[OTSARDB_KEY_MAX + 1];
    int key_len = snprintf(key,
                           sizeof(key),
                           "%s%s%s",
                           job->database_root,
                           needs_slash ? "/" : "",
                           segment_suffix);
    if (key_len <= 0 || (size_t)key_len >= sizeof(key)) {
        free(segment_data);
        job->rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }

    /* bounded-backoff retry for transient failures of the
     * streamed segment upload (same policy as the publish stages; a
     * pre-upload retry is idempotent â€” content-addressed key, identical
     * bytes). On final failure the first error is preserved. */
    preupload_attempt_ctx attempt_ctx;
    attempt_ctx.store = job->store;
    attempt_ctx.key = key;
    attempt_ctx.data = segment_data;
    attempt_ctx.size = segment_size;
    otsardb_journal_result retry_rc = OTSARDB_JOURNAL_ERROR;
    job->rc = publish_retry_cycle(preupload_attempt_once, &attempt_ctx, &retry_rc)
                  ? OTSARDB_JOURNAL_OK
                  : retry_rc;
    free(segment_data);
    return job->rc == OTSARDB_JOURNAL_OK;
}

#if defined(_WIN32)
static unsigned __stdcall preupload_worker_main(void *opaque)
#else
static void *preupload_worker_main(void *opaque)
#endif
{
    preupload_job *job = (preupload_job *)opaque;
    preupload_job_run(job);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
typedef uintptr_t otsardb_preupload_thread_t;
static int otsardb_preupload_thread_create(otsardb_preupload_thread_t *out, preupload_job *job) {
    uintptr_t handle = _beginthreadex(NULL, 0, preupload_worker_main, job, 0, NULL);
    if (handle == 0) return -1;
    *out = handle;
    return 0;
}
static void otsardb_preupload_thread_join(otsardb_preupload_thread_t thread) {
    if (thread == 0) return;
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
}
#else
typedef pthread_t otsardb_preupload_thread_t;
static int otsardb_preupload_thread_create(otsardb_preupload_thread_t *out, preupload_job *job) {
    return pthread_create(out, NULL, preupload_worker_main, job) == 0 ? 0 : -1;
}
static void otsardb_preupload_thread_join(otsardb_preupload_thread_t thread) {
    pthread_join(thread, NULL);
}
#endif

/* Upload all segments of one multi-segment commit in waves of `parallel`
 * concurrent workers. Every spawned worker of a wave is joined before any
 * result is inspected and before any early return - no thread can outlive
 * this call. On the first failing segment the remaining waves are not
 * spawned and the error propagates. Returns 1 on success with *out_rc =
 * OK, 0 on failure with *out_rc set. */
static int wal_publisher_preupload_segments(otsardb_wal_publisher *publisher,
                                            const char *commit_id,
                                            const otsardb_segment_input *inputs,
                                            uint32_t segment_count,
                                            int parallel,
                                            otsardb_journal_result *out_rc) {
    if (!publisher || !commit_id || !inputs || segment_count == 0 || !out_rc ||
        parallel < 1) {
        return 0;
    }

    preupload_job *jobs = (preupload_job *)calloc(segment_count, sizeof(*jobs));
    if (!jobs) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    for (uint32_t i = 0; i < segment_count; ++i) {
        jobs[i].store = publisher->store;
        jobs[i].database_root = publisher->database_root;
        jobs[i].commit_id = commit_id;
        jobs[i].batch = inputs[i].payload;
        jobs[i].batch_size = inputs[i].payload_size;
        jobs[i].rc = OTSARDB_JOURNAL_OK;
    }

    int failed = 0;
    otsardb_journal_result first_rc = OTSARDB_JOURNAL_OK;
    for (uint32_t wave_start = 0; wave_start < segment_count && !failed;
         wave_start += (uint32_t)parallel) {
        uint32_t wave_count = segment_count - wave_start;
        if (wave_count > (uint32_t)parallel) wave_count = (uint32_t)parallel;

        otsardb_preupload_thread_t threads[8];
        uint32_t spawned = 0;
        for (uint32_t j = 0; j < wave_count; ++j) {
            preupload_job *job = &jobs[wave_start + j];
            if (otsardb_preupload_thread_create(&threads[spawned], job) == 0) {
                ++spawned;
            } else {
                job->rc = OTSARDB_JOURNAL_ERROR;
                if (first_rc == OTSARDB_JOURNAL_OK) first_rc = OTSARDB_JOURNAL_ERROR;
                failed = 1;
            }
        }
        for (uint32_t j = 0; j < spawned; ++j) {
            otsardb_preupload_thread_join(threads[j]);
        }
        if (!failed) {
            for (uint32_t j = 0; j < wave_count; ++j) {
                if (jobs[wave_start + j].rc != OTSARDB_JOURNAL_OK) {
                    if (first_rc == OTSARDB_JOURNAL_OK) first_rc = jobs[wave_start + j].rc;
                    failed = 1;
                    break;
                }
            }
        }
    }

    free(jobs);
    if (failed) {
        *out_rc = first_rc;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

int otsardb_wal_publisher_retry_publish(otsardb_wal_publisher *publisher,
                                      const char *local_db_path,
                                      uint32_t page_size,
                                      uint64_t database_size_pages,
                                      const otsardb_sqlite_wal_frame_ref *frames,
                                      uint32_t frame_count) {
    if (!publisher || !publisher->store || !publisher->database_root[0] ||
        !local_db_path || local_db_path[0] == '\0' || page_size == 0 ||
        database_size_pages == 0 || frame_count == 0 || !frames) {
        return -2;
    }

    /* 1. The commit was attempted against the publisher's cached parent.
     * Capture it before the refresh below overwrites it. */
    uint64_t attempt_parent_generation = publisher->current_generation;
    char attempt_parent_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char attempt_parent_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    snprintf(attempt_parent_commit_id,
             sizeof(attempt_parent_commit_id),
             "%s",
             publisher->current_commit_id);
    snprintf(attempt_parent_sha256,
             sizeof(attempt_parent_sha256),
             "%s",
             publisher->current_manifest_sha256);

    /* 2. Refresh the remote HEAD. */
    if (refresh_head(publisher) != SQLITE_OK) return -2;

    /* 3. Remote HEAD unchanged (still the attempt's parent): the local image
     * is already current, nothing to re-materialise â€” the caller retries
     * the normal publish path. */
    if (publisher->current_generation == attempt_parent_generation &&
        strcmp(publisher->current_commit_id, attempt_parent_commit_id) == 0) {
        return 0;
    }

    /* 4. Our page set (sorted/deduped), used for both overlap checks. */
    uint32_t our_page_count = 0;
    uint32_t *our_pages = retry_build_page_set(frames, frame_count, &our_page_count);
    if (!our_pages) return -2;

    /* 5. Defence in depth: the caller promised disjointness (precondition),
     * but re-check against the CURRENT interleaved chain. A genuine
     * overlap means re-materialising and re-publishing would silently
     * overwrite the winner's pages â€” refuse before touching anything. */
    int overlap = 0;
    if (!retry_conflict_check(publisher,
                              attempt_parent_generation,
                              attempt_parent_commit_id,
                              attempt_parent_sha256,
                              our_pages,
                              our_page_count,
                              &overlap)) {
        free(our_pages);
        return -2;
    }
    if (overlap) {
        free(our_pages);
        return -1;
    }

    /* 6. Materialise the new HEAD state into the local execution image.
     * This also validates the new chain (hash links + rolling checksum). */
    if (!otsardb_recovery_restore_store(publisher->store,
                                      publisher->database_root,
                                      local_db_path)) {
        free(our_pages);
        return -2;
    }

    /* 6b. otsardb_recovery_restore_store validated the new chain INCLUDING the
     * head's post-apply checksum against the materialised file, so the local
     * image is now provably the new parent state â€” re-arm the rolling
     * checksum base for the re-publish. */
    publisher->local_image_verified = 1;

    /* 7. The local image changed: every page-map contribution recorded for
     * the old image is stale. refresh_head already cleared the map when
     * the chain changed and re-based state_checksum on the NEW parent's
     * post-apply checksum; clearing again is idempotent and covers the
     * first-refresh corner case. The checksum base stays as refresh_head
     * set it â€” publish_one_commit XORs the OLD page values from the
     * re-materialised file (via db_read_at) instead of the stale map. */
    page_map_clear(publisher);

    /* 8. Re-publish the SAME frames onto the new parent. Page images are
     * re-read from the fsynced WAL "<local_db_path>-wal" at the frames'
     * recorded offsets (the caller keeps that file). The commit id is
     * FRESH: the failed attempt already used index
     * published_commit_count + 1 and may have left an orphan manifest
     * under that id naming the OLD parent â€” immutable objects cannot be
     * rewritten, so a same-id retry would collide (see). */
    char wal_path[OTSARDB_KEY_MAX + 1];
    int wal_len = snprintf(wal_path, sizeof(wal_path), "%s-wal", local_db_path);
    if (wal_len <= 0 || (size_t)wal_len >= sizeof(wal_path)) {
        free(our_pages);
        return -2;
    }

    retry_file_reader wal_reader = {NULL};
    retry_file_reader db_reader = {NULL};
    wal_reader.file = fopen(wal_path, "rb");
    if (!wal_reader.file) {
        free(our_pages);
        return -2;
    }
    db_reader.file = fopen(local_db_path, "rb");
    if (!db_reader.file) {
        fclose(wal_reader.file);
        free(our_pages);
        return -2;
    }

    publish_scan_context publish_ctx;
    memset(&publish_ctx, 0, sizeof(publish_ctx));
    publish_ctx.publisher = publisher;
    publish_ctx.commit_index = publisher->published_commit_count + 1u;
    publish_ctx.sqlite_rc = SQLITE_OK;
    publish_ctx.db_read_at = retry_file_read_at;
    publish_ctx.db_read_ctx = &db_reader;
    publish_ctx.checksum = publisher->checksum_known ? publisher->state_checksum : 0;
    publish_ctx.checksum_valid = publisher->checksum_known;

    int cb_rc = publish_one_commit(page_size,
                                   database_size_pages,
                                   frames,
                                   frame_count,
                                   retry_file_read_at,
                                   &wal_reader,
                                   &publish_ctx);
    fclose(wal_reader.file);
    fclose(db_reader.file);
    free(our_pages);

    if (cb_rc == 0 && publish_ctx.sqlite_rc == SQLITE_OK) return 1;

    /* Re-publish conflicted again: publish_one_commit classified it via
     * otsardb_publish_commit_detect_conflict. */
    if (publish_ctx.sqlite_rc == SQLITE_BUSY_SNAPSHOT) return -1;

    /* SQLITE_BUSY here is a SECOND DISJOINT CAS failure (another writer
     * interleaved during our retry) or a lease-fence refusal; any other rc is
     * an I/O/encode error. Either way this one-shot retry is done: the caller
     * falls back to the v1 contract (surface BUSY, re-execute at the new
     * HEAD). */
    return -2;
}

/* tests/lazy_coalesce_stats_test.c — range-GET coalescing
 * refinement (OTSARDB_S3_COALESCE_MAX byte window + parallel issue) and the
 * per-read-path counters ( OTSARDB_S3_READ_STATS).
 *
 * Deterministic unit test over a memory store with a hand-crafted (fake)
 * page index (one v4 manifest, one v3 frame-offset segment), driving
 * otsardb_lazy_hydrate_range directly and observing the store through a
 * counting wrapper:
 *
 * 1. byte-window merge: pages 1..6 claimed, page 3 already materialised —
 * the demand path fetches ONE range GET covering offsets[0]..offsets[6]
 * (the default OTSARDB_S3_COALESCE_MAX=4096 window merges the two runs
 * [1-2] and [4-6]); the gap frame (page 3) is fetched and validated but
 * never overwrites the materialised page; stats: total_gets 1,
 * range_gets 1, coalesced_runs 1, demand_misses 5;
 * 2. strict 0059 mode (OTSARDB_S3_COALESCE_MAX=0): the same demand issues
 * TWO range GETs (one per consecutive-page run) and stats reflect it
 * (range_gets 2, coalesced_runs 2);
 * 3. parallel issue: with OTSARDB_LAZY_PARALLEL=2 and three single-page
 * runs, the range GETs run on two ephemeral workers — the counting
 * store observes max-in-flight == 2 (each GET is slowed to 50 ms) and
 * single-page runs are NOT counted as coalesced;
 * 4. whole-object path (store without RANGE_GET): one whole GET, zero
 * range GETs, demand_misses 6, bytes == segment size;
 * 5. prefetch stats: a 1..16 sequential scan over a 1..32 segment with
 * OTSARDB_LAZY_PREFETCH=8 produces exactly 6 range GETs (demand misses
 * 1, 2, 11, 12 + windows [3..10] and [13..20]) and the counters read
 * total_gets 6, range_gets 6, demand_misses 4, prefetch_fetches 16,
 * prefetch_hits 12, coalesced_runs 2, sequential_streaks 2;
 * 6. OTSARDB_S3_READ_STATS output format: otsardb_lazy_hydration_stats_format
 * renders the canonical line, and with OTSARDB_S3_READ_STATS=1
 * otsardb_lazy_destroy prints exactly that line to stderr (captured via
 * dup2) with the session's real counts.
 */

#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "memory_store.h"
#include "object_store.h"
#include "segment.h"
#include "wal_batch.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define otsardb_unlink _unlink
#define otsardb_dup _dup
#define otsardb_dup2 _dup2
#define otsardb_close _close
static void test_sleep_ms(unsigned long ms) { Sleep(ms); }
#else
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_dup dup
#define otsardb_dup2 dup2
#define otsardb_close close
static void test_sleep_ms(unsigned long ms) { usleep(ms * 1000u); }
#endif

#define CS_PAGE_SIZE 512u
#define CS_DB_PAGES 64u
#define CS_IMAGE "lazy-coalesce-image.db"
#define CS_CAPTURE "lazy-coalesce-stats.txt"

static const char *ROOT_6 = "lazy/coalesce-6";
static const char *DB_6 = "cs-six-db";
static const char *COMMIT_6 = "cs-six-0001";
static const char *ROOT_32 = "lazy/coalesce-32";
static const char *DB_32 = "cs-thirtytwo-db";
static const char *COMMIT_32 = "cs-thirtytwo-0001";
static otsardb_object_store *store_global = NULL;
static otsardb_head head_6;
static otsardb_head head_32;

static void cleanup_files(void) {
    otsardb_unlink(CS_IMAGE);
    otsardb_unlink(CS_CAPTURE);
}

static void set_env_text(const char *name, const char *value) {
#if defined(_WIN32)
    char buf[512];
    snprintf(buf, sizeof(buf), "%s=%s", name, value);
    _putenv(buf);
#else
    setenv(name, value, 1);
#endif
}

/* ------------------------------------------------------------------------- *
 * Hand-crafted chain (same pattern as tests/lazy_prefetch_test.c).
 * ------------------------------------------------------------------------- */

typedef struct cs_frame_table {
    uint64_t offsets[64];
    uint32_t page_nos[64];
    uint32_t count;
    uint64_t segment_size; /* object size: the last block ends here */
} cs_frame_table;

static void craft_chain(otsardb_object_store *store,
                        const char *root,
                        const char *db_id,
                        const char *commit_id,
                        const uint32_t *page_nos,
                        uint32_t page_count,
                        const unsigned char *page_bufs,
                        cs_frame_table *frames_out) {
    assert(page_count <= 64);

    char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(CS_PAGE_SIZE, CS_DB_PAGES, page_nos,
                                  page_bufs, page_count, (unsigned char **)&batch,
                                  &batch_size));
    unsigned char *segment_data = NULL;
    size_t segment_size = 0;
    char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    otsardb_segment_frame_info frames;
    memset(&frames, 0, sizeof(frames));
    assert(otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                            commit_id,
                                            batch,
                                            batch_size,
                                            &segment_data,
                                            &segment_size,
                                            segment_sha,
                                            1,
                                            &frames));
    free(batch);
    assert(frames.count == page_count);
    memcpy(frames_out->offsets, frames.offsets, page_count * sizeof(uint64_t));
    memcpy(frames_out->page_nos, frames.page_nos, page_count * sizeof(uint32_t));
    frames_out->count = page_count;
    frames_out->segment_size = segment_size;

    otsardb_db_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.format_version = 1;
    metadata.page_size = CS_PAGE_SIZE;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", db_id);
    assert(otsardb_db_metadata_create(store, root, &metadata) == OTSARDB_METADATA_OK);

    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.page_size = CS_PAGE_SIZE;
    manifest.database_size_pages = CS_DB_PAGES;
    snprintf(manifest.database_id, sizeof(manifest.database_id), "%s", db_id);
    snprintf(manifest.commit_id, sizeof(manifest.commit_id), "%s", commit_id);
    manifest.segment_count = 1;
    snprintf(manifest.segments[0].key,
             sizeof(manifest.segments[0].key),
             "segments/%s.otsseg", commit_id);
    snprintf(manifest.segments[0].sha256,
             sizeof(manifest.segments[0].sha256),
             "%s", segment_sha);
    manifest.segments[0].size = segment_size;
    manifest.segments[0].pages = (uint32_t *)page_nos;
    manifest.segments[0].page_count = page_count;
    manifest.segments[0].frame_offsets = frames.offsets;
    char *manifest_json = NULL;
    size_t manifest_size = 0;
    char manifest_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    assert(otsardb_commit_manifest_encode_json(&manifest,
                                             &manifest_json,
                                             &manifest_size,
                                             manifest_sha));

    char seg_key[OTSARDB_KEY_MAX + 1];
    snprintf(seg_key, sizeof(seg_key), "%s/%s", root, manifest.segments[0].key);
    otsardb_store_put_options plain = {0};
    assert(otsardb_store_put(store, seg_key, segment_data, segment_size,
                           &plain, NULL) == OTSARDB_STORE_OK);
    char man_key[OTSARDB_KEY_MAX + 1];
    snprintf(man_key, sizeof(man_key), "%s/commits/%s.json", root, commit_id);
    assert(otsardb_store_put(store, man_key, manifest_json, manifest_size,
                           &plain, NULL) == OTSARDB_STORE_OK);

    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = 1;
    snprintf(head.database_id, sizeof(head.database_id), "%s", db_id);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", commit_id);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s", man_key);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", manifest_sha);
    char *head_json = NULL;
    size_t head_size = 0;
    assert(otsardb_head_encode_json(&head, &head_json, &head_size));
    char head_key[OTSARDB_KEY_MAX + 1];
    snprintf(head_key, sizeof(head_key), "%s/HEAD.json", root);
    assert(otsardb_store_put(store, head_key, head_json, head_size,
                           &plain, NULL) == OTSARDB_STORE_OK);

    otsardb_segment_frame_info_release(&frames);
    free(head_json);
    free(manifest_json);
    free(segment_data);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

/* ------------------------------------------------------------------------- *
 * Counting store wrapper: counts get / get_range, records every range GET
 * (offset, length), counts whole-object GETs of segment keys, tracks the
 * maximum number of range GETs in flight at once (parallel-issue probe) and
 * can slow every range GET down (so the parallel scenario overlaps).
 * ------------------------------------------------------------------------- */

typedef struct cs_range_rec {
    size_t offset;
    size_t length;
} cs_range_rec;

typedef struct cs_store_ctx {
    otsardb_object_store *inner;
    size_t get_range_count;
    size_t segment_get_count;
    cs_range_rec recs[64];
    size_t rec_count;
    long in_flight;
    long max_in_flight;
    int slow_range_ms;
    int range_capable;
} cs_store_ctx;

#if defined(_WIN32)
static CRITICAL_SECTION g_cs_lock;
static int g_lock_init;
#else
static pthread_mutex_t g_cs_lock;
static int g_lock_init;
#endif

static void cs_lock(void) {
#if defined(_WIN32)
    if (!g_lock_init) {
        InitializeCriticalSection(&g_cs_lock);
        g_lock_init = 1;
    }
    EnterCriticalSection(&g_cs_lock);
#else
    if (!g_lock_init) {
        pthread_mutex_init(&g_cs_lock, NULL);
        g_lock_init = 1;
    }
    pthread_mutex_lock(&g_cs_lock);
#endif
}

static void cs_unlock(void) {
#if defined(_WIN32)
    LeaveCriticalSection(&g_cs_lock);
#else
    pthread_mutex_unlock(&g_cs_lock);
#endif
}

static cs_store_ctx *cs_ctx(otsardb_object_store *store) {
    return (cs_store_ctx *)store->context;
}

static otsardb_store_result cs_get(otsardb_object_store *store,
                                 const char *key,
                                 otsardb_store_object *out) {
    cs_store_ctx *ctx = cs_ctx(store);
    if (key && strstr(key, "segments/") != NULL) ++ctx->segment_get_count;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result cs_get_range(otsardb_object_store *store,
                                       const char *key,
                                       size_t offset,
                                       size_t length,
                                       otsardb_store_object *out) {
    cs_store_ctx *ctx = cs_ctx(store);
    cs_lock();
    ++ctx->get_range_count;
    assert(ctx->rec_count < 64);
    ctx->recs[ctx->rec_count].offset = offset;
    ctx->recs[ctx->rec_count].length = length;
    ++ctx->rec_count;
    ++ctx->in_flight;
    if (ctx->in_flight > ctx->max_in_flight) ctx->max_in_flight = ctx->in_flight;
    cs_unlock();
    if (ctx->slow_range_ms > 0) test_sleep_ms((unsigned long)ctx->slow_range_ms);
    otsardb_store_result rc = otsardb_store_get_range(ctx->inner, key, offset, length, out);
    cs_lock();
    --ctx->in_flight;
    cs_unlock();
    return rc;
}

static otsardb_store_result cs_put(otsardb_object_store *store,
                                 const char *key,
                                 const void *data,
                                 size_t size,
                                 const otsardb_store_put_options *options,
                                 char **out_etag) {
    return otsardb_store_put(cs_ctx(store)->inner, key, data, size, options, out_etag);
}

static void cs_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(cs_ctx(store)->inner, object);
}

static otsardb_store_result cs_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(cs_ctx(store)->inner, key);
}

static otsardb_store_result cs_list(otsardb_object_store *store,
                                  const char *prefix,
                                  otsardb_store_list_callback callback,
                                  void *ctx) {
    return otsardb_store_list(cs_ctx(store)->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops CS_OPS = {
    .get = cs_get,
    .get_range = cs_get_range,
    .put = cs_put,
    .release_object = cs_release,
    .delete_object = cs_delete,
    .list = cs_list,
};

static otsardb_object_store *cs_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    cs_store_ctx *ctx = (cs_store_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    ctx->range_capable = otsardb_store_supports(inner, OTSARDB_STORE_CAP_RANGE_GET);
    store->ops = &CS_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void cs_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

/* ------------------------------------------------------------------------- *
 * Image assertions
 * ------------------------------------------------------------------------- */

static void read_image_page(uint32_t page_no, unsigned char out[CS_PAGE_SIZE]) {
    FILE *file = fopen(CS_IMAGE, "rb");
    assert(file);
    assert(fseek(file, (long)(page_no - 1u) * CS_PAGE_SIZE, SEEK_SET) == 0);
    assert(fread(out, 1, CS_PAGE_SIZE, file) == CS_PAGE_SIZE);
    fclose(file);
}

static void assert_page_filled(uint32_t page_no, unsigned char fill) {
    unsigned char page[CS_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < CS_PAGE_SIZE; ++i) {
        assert(page[i] == fill);
    }
}

static void assert_page_zeros(uint32_t page_no) {
    unsigned char page[CS_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < CS_PAGE_SIZE; ++i) {
        assert(page[i] == 0);
    }
}

/* Poll `*counter` until it reaches `target` (the prefetch pool worker runs
 * asynchronously; memory-store ops complete in microseconds). */
static void poll_until(size_t *counter, size_t target) {
    unsigned long waited_ms = 0;
    while (*counter < target && waited_ms < 5000) {
        test_sleep_ms(5);
        waited_ms += 5;
    }
    assert(*counter >= target);
}

/* ------------------------------------------------------------------------- *
 * Scenario 1: byte-window merge (default OTSARDB_S3_COALESCE_MAX = 4096).
 * Pages 1..6 claimed by one segment; page 3 marked materialised. The two
 * demand runs [1-2] and [4-6] sit within the byte window (gap = the frame
 * of page 3), so the demand path issues ONE range GET covering the whole
 * page-1..6 byte span. The gap frame is validated but never overwrites the
 * materialised page.
 * ------------------------------------------------------------------------- */

static void scenario_1_merge(cs_frame_table *frames) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_COALESCE_MAX", ""); /* default 4096 */
    cs_store_ctx *ctx = cs_ctx(store_global);
    ctx->get_range_count = 0;
    ctx->rec_count = 0;
    ctx->segment_get_count = 0;
    ctx->slow_range_ms = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store_global, ROOT_6, CS_IMAGE,
                                    &head_6, &h));
    otsardb_lazy_mark_range(h, 3, 3);
    assert(otsardb_lazy_hydrate_range(h, 1, 6) == 1);

    assert(ctx->get_range_count == 1);
    assert(ctx->rec_count == 1);
    assert(ctx->recs[0].offset == (size_t)frames->offsets[0]);
    assert(ctx->recs[0].length ==
           (size_t)(frames->segment_size - frames->offsets[0]));
    assert(ctx->segment_get_count == 0);

    assert_page_filled(1, 1);
    assert_page_filled(2, 2);
    assert_page_zeros(3); /* the gap page was never overwritten */
    assert_page_filled(4, 4);
    assert_page_filled(5, 5);
    assert_page_filled(6, 6);

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 1);
    assert(stats.range_gets == 1);
    assert(stats.bytes_fetched == ctx->recs[0].length);
    assert(stats.demand_misses == 5);
    assert(stats.prefetch_fetches == 0);
    assert(stats.prefetch_hits == 0);
    assert(stats.coalesced_runs == 1);
    assert(stats.sequential_streaks == 0);

    otsardb_lazy_destroy(h);
    otsardb_unlink(CS_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * Scenario 2: strict 0059 behavior (OTSARDB_S3_COALESCE_MAX=0): consecutive
 * page numbers still merge, but runs separated by a materialised page stay
 * separate — exactly TWO range GETs for the same demand.
 * ------------------------------------------------------------------------- */

static void scenario_2_strict(cs_frame_table *frames) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_COALESCE_MAX", "0");
    cs_store_ctx *ctx = cs_ctx(store_global);
    ctx->get_range_count = 0;
    ctx->rec_count = 0;
    ctx->segment_get_count = 0;
    ctx->slow_range_ms = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store_global, ROOT_6, CS_IMAGE,
                                    &head_6, &h));
    otsardb_lazy_mark_range(h, 3, 3);
    assert(otsardb_lazy_hydrate_range(h, 1, 6) == 1);

    assert(ctx->get_range_count == 2);
    assert(ctx->rec_count == 2);
    assert(ctx->recs[0].offset == (size_t)frames->offsets[0]);
    assert(ctx->recs[0].length ==
           (size_t)(frames->offsets[2] - frames->offsets[0]));
    assert(ctx->recs[1].offset == (size_t)frames->offsets[3]);
    assert(ctx->recs[1].length ==
           (size_t)(frames->segment_size - frames->offsets[3]));
    assert(ctx->segment_get_count == 0);

    assert_page_filled(1, 1);
    assert_page_filled(2, 2);
    assert_page_zeros(3);
    assert_page_filled(4, 4);
    assert_page_filled(5, 5);
    assert_page_filled(6, 6);

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 2);
    assert(stats.range_gets == 2);
    assert(stats.bytes_fetched == ctx->recs[0].length + ctx->recs[1].length);
    assert(stats.demand_misses == 5);
    assert(stats.coalesced_runs == 2);
    assert(stats.sequential_streaks == 0);

    otsardb_lazy_destroy(h);
    otsardb_unlink(CS_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * Scenario 3: parallel issue. Pages 2 and 4 materialised -> the demand
 * pages {1}, {3}, {5, 6} form three runs; with OTSARDB_LAZY_PARALLEL=2 the
 * demand path fetches them on two ephemeral workers, so the counting store
 * observes max-in-flight == 2 (each GET slowed to 50 ms). Only the [5-6]
 * run covers more than one page -> coalesced_runs == 1.
 * ------------------------------------------------------------------------- */

static void scenario_3_parallel(cs_frame_table *frames) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_COALESCE_MAX", "0");
    set_env_text("OTSARDB_LAZY_PARALLEL", "2");
    cs_store_ctx *ctx = cs_ctx(store_global);
    ctx->get_range_count = 0;
    ctx->rec_count = 0;
    ctx->segment_get_count = 0;
    ctx->slow_range_ms = 50;
    ctx->in_flight = 0;
    ctx->max_in_flight = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store_global, ROOT_6, CS_IMAGE,
                                    &head_6, &h));
    otsardb_lazy_mark_range(h, 2, 2);
    otsardb_lazy_mark_range(h, 4, 4);
    assert(otsardb_lazy_hydrate_range(h, 1, 6) == 1);
    ctx->slow_range_ms = 0;

    assert(ctx->get_range_count == 3);
    assert(ctx->max_in_flight == 2);
    assert(ctx->segment_get_count == 0);
    /* The three runs: page 1 (frame 0), page 3 (frame 2), pages 5-6
     * (frames 4..end). Parallel workers record in arrival order, so the
     * expected (offset, length) pairs are compared as a set. */
    size_t expected_offsets[3] = {
        (size_t)frames->offsets[0],
        (size_t)frames->offsets[2],
        (size_t)frames->offsets[4],
    };
    size_t expected_lengths[3] = {
        (size_t)(frames->offsets[1] - frames->offsets[0]),
        (size_t)(frames->offsets[3] - frames->offsets[2]),
        (size_t)(frames->segment_size - frames->offsets[4]),
    };
    size_t matched[3] = {0, 0, 0};
    for (size_t r = 0; r < ctx->rec_count; ++r) {
        int found = 0;
        for (size_t e = 0; e < 3; ++e) {
            if (!matched[e] && ctx->recs[r].offset == expected_offsets[e] &&
                ctx->recs[r].length == expected_lengths[e]) {
                matched[e] = 1;
                found = 1;
                break;
            }
        }
        assert(found);
    }
    assert(matched[0] && matched[1] && matched[2]);

    assert_page_filled(1, 1);
    assert_page_zeros(2);
    assert_page_filled(3, 3);
    assert_page_zeros(4);
    assert_page_filled(5, 5);
    assert_page_filled(6, 6);

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 3);
    assert(stats.range_gets == 3);
    assert(stats.bytes_fetched ==
           ctx->recs[0].length + ctx->recs[1].length + ctx->recs[2].length);
    assert(stats.demand_misses == 4);
    assert(stats.coalesced_runs == 1);

    otsardb_lazy_destroy(h);
    otsardb_unlink(CS_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * Scenario 4: whole-object path (store without RANGE_GET): the demand path
 * falls back to one whole-object GET with exact size + SHA-256 verification;
 * stats: total_gets 1, range_gets 0, demand_misses 6, bytes == segment size.
 * ------------------------------------------------------------------------- */

static void scenario_4_whole_object(void) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    otsardb_object_store *inner_no_range =
        otsardb_test_memory_store_create_with_capabilities(
            OTSARDB_STORE_CAP_ETAG | OTSARDB_STORE_CAP_READ_AFTER_WRITE |
            OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
            OTSARDB_STORE_CAP_CONDITIONAL_UPDATE);
    assert(inner_no_range != NULL);
    otsardb_object_store *store = cs_store_create(inner_no_range);
    assert(!otsardb_store_supports(store, OTSARDB_STORE_CAP_RANGE_GET));

    uint32_t pages[6] = {1, 2, 3, 4, 5, 6};
    unsigned char *page_bufs = (unsigned char *)malloc(6 * CS_PAGE_SIZE);
    assert(page_bufs);
    for (uint32_t i = 0; i < 6; ++i) {
        memset(page_bufs + (size_t)i * CS_PAGE_SIZE, (unsigned char)(i + 1u),
               CS_PAGE_SIZE);
    }
    cs_frame_table frames;
    memset(&frames, 0, sizeof(frames));
    craft_chain(store, "lazy/coalesce-norange", "cs-norange-db", "cs-norange-0001",
                pages, 6, page_bufs, &frames);
    free(page_bufs);
    otsardb_head head = read_head(store, "lazy/coalesce-norange");

    cs_store_ctx *ctx = cs_ctx(store);
    ctx->get_range_count = 0;
    ctx->rec_count = 0;
    ctx->segment_get_count = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store, "lazy/coalesce-norange", CS_IMAGE,
                                    &head, &h));
    assert(otsardb_lazy_hydrate_range(h, 1, 6) == 1);
    assert(ctx->get_range_count == 0);
    assert(ctx->segment_get_count == 1);

    for (uint32_t p = 1; p <= 6; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 1);
    assert(stats.range_gets == 0);
    assert(stats.bytes_fetched == frames.segment_size);
    assert(stats.demand_misses == 6);
    assert(stats.coalesced_runs == 0);

    otsardb_lazy_destroy(h);
    cs_store_destroy(store);
    otsardb_test_memory_store_destroy(inner_no_range);
    otsardb_unlink(CS_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * Scenario 5: prefetch counters. A 1..16 sequential scan over a 1..32
 * segment with OTSARDB_LAZY_PREFETCH=8 issues exactly 6 range GETs: demand
 * misses 1, 2, 11, 12 + windows [3..10] and [13..20] (the windows apply
 * 16 pages; demand reads 4; pages 17..20 are prefetched but never demanded).
 * ------------------------------------------------------------------------- */

static void scenario_5_prefetch(cs_frame_table *frames) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "8");
    set_env_text("OTSARDB_S3_COALESCE_MAX", "");
    cs_store_ctx *ctx = cs_ctx(store_global);
    ctx->get_range_count = 0;
    ctx->rec_count = 0;
    ctx->segment_get_count = 0;
    ctx->slow_range_ms = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store_global, ROOT_32, CS_IMAGE,
                                    &head_32, &h));
    for (uint32_t p = 1; p <= 16; ++p) {
        assert(otsardb_lazy_hydrate_range(h, p, p) == 1);
    }
    poll_until(&ctx->get_range_count, 6);
    test_sleep_ms(200); /* settle: no further jobs may appear */

    assert(ctx->get_range_count == 6);
    assert(ctx->segment_get_count == 0);
    for (uint32_t p = 1; p <= 20; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }
    assert_page_zeros(21);

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 6);
    assert(stats.range_gets == 6);
    assert(stats.demand_misses == 4);
    assert(stats.prefetch_fetches == 16);
    assert(stats.prefetch_hits == 12);
    assert(stats.coalesced_runs == 2);
    assert(stats.sequential_streaks == 2);
    size_t total_bytes = 0;
    for (size_t r = 0; r < ctx->rec_count; ++r) {
        total_bytes += ctx->recs[r].length;
    }
    assert(stats.bytes_fetched == (uint64_t)total_bytes);

    otsardb_lazy_destroy(h);
    otsardb_unlink(CS_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * Scenario 6: OTSARDB_S3_READ_STATS output format. The format function
 * renders the canonical one-line record; with OTSARDB_S3_READ_STATS=1
 * otsardb_lazy_destroy prints exactly that line (captured via dup2) with the
 * session's real counts.
 * ------------------------------------------------------------------------- */

static void capture_stderr_begin(FILE **cap, int *saved_fd) {
    fflush(stderr);
    *saved_fd = otsardb_dup(fileno(stderr));
    assert(*saved_fd >= 0);
    *cap = fopen(CS_CAPTURE, "w");
    assert(*cap != NULL);
    assert(otsardb_dup2(fileno(*cap), fileno(stderr)) >= 0);
}

static void capture_stderr_end(FILE **cap, int *saved_fd) {
    fflush(stderr);
    otsardb_dup2(*saved_fd, fileno(stderr));
    otsardb_close(*saved_fd);
    fclose(*cap);
    *cap = NULL;
}

static void scenario_6_stats_line(cs_frame_table *frames) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_COALESCE_MAX", "");

    /* Canonical format. */
    otsardb_lazy_hydration_stats sample;
    memset(&sample, 0, sizeof(sample));
    sample.total_gets = 1;
    sample.range_gets = 1;
    sample.bytes_fetched = 4242;
    sample.demand_misses = 5;
    sample.prefetch_fetches = 3;
    sample.prefetch_hits = 7;
    sample.coalesced_runs = 1;
    sample.sequential_streaks = 2;
    sample.hydration_ms = 41;
    char line[512];
    int n = otsardb_lazy_hydration_stats_format(&sample, line, sizeof(line));
    assert(n > 0 && (size_t)n < sizeof(line));
    assert(strncmp(line, "otsardb: read-stats total_gets=1 range_gets=1 "
                          "bytes_fetched=4242 demand_misses=5 "
                          "prefetch_fetches=3 prefetch_hits=7 "
                          "coalesced_runs=1 sequential_streaks=2 "
                          "hydration_ms=41\n",
                    strlen(line)) == 0);

    /* Session-close print with real counts (dup2 capture). */
    cs_store_ctx *ctx = cs_ctx(store_global);
    ctx->get_range_count = 0;
    ctx->rec_count = 0;
    ctx->segment_get_count = 0;
    ctx->slow_range_ms = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store_global, ROOT_6, CS_IMAGE,
                                    &head_6, &h));
    otsardb_lazy_mark_range(h, 3, 3);
    assert(otsardb_lazy_hydrate_range(h, 1, 6) == 1);
    const size_t span = (size_t)(frames->segment_size - frames->offsets[0]);
    assert(ctx->get_range_count == 1 && ctx->recs[0].length == span);

    set_env_text("OTSARDB_S3_READ_STATS", "1");
    FILE *cap = NULL;
    int saved_fd = -1;
    capture_stderr_begin(&cap, &saved_fd);
    otsardb_lazy_destroy(h);
    capture_stderr_end(&cap, &saved_fd);
    set_env_text("OTSARDB_S3_READ_STATS", "0");

    FILE *file = fopen(CS_CAPTURE, "rb");
    assert(file != NULL);
    char captured[512];
    assert(fgets(captured, sizeof(captured), file) != NULL);
    fclose(file);
    assert(strncmp(captured, "otsardb: read-stats ", 18) == 0);
    unsigned long long tg = 0, rg = 0, bf = 0, dm = 0, pf = 0, ph = 0, cr = 0,
                       ss = 0, hms = 0;
    int parsed = sscanf(captured,
                        "otsardb: read-stats total_gets=%llu range_gets=%llu "
                        "bytes_fetched=%llu demand_misses=%llu "
                        "prefetch_fetches=%llu prefetch_hits=%llu "
                        "coalesced_runs=%llu sequential_streaks=%llu "
                        "hydration_ms=%llu",
                        &tg, &rg, &bf, &dm, &pf, &ph, &cr, &ss, &hms);
    assert(parsed == 9);
    assert(tg == 1);
    assert(rg == 1);
    assert(bf == (unsigned long long)span);
    assert(dm == 5);
    assert(pf == 0);
    assert(ph == 0);
    assert(cr == 1);
    assert(ss == 0);
    assert(hms >= 0);

    otsardb_unlink(CS_IMAGE);
}

int main(void) {
    cleanup_files();
    set_env_text("OTSARDB_S3_READ_STATS", "0");

    /* Six-page segment: page i filled with byte i. */
    uint32_t pages_6[6] = {1, 2, 3, 4, 5, 6};
    unsigned char *page_bufs_6 = (unsigned char *)malloc(6 * CS_PAGE_SIZE);
    assert(page_bufs_6);
    for (uint32_t i = 0; i < 6; ++i) {
        memset(page_bufs_6 + (size_t)i * CS_PAGE_SIZE, (unsigned char)(i + 1u),
               CS_PAGE_SIZE);
    }
    cs_frame_table frames_6;
    memset(&frames_6, 0, sizeof(frames_6));

    /* Thirty-two-page segment (prefetch window coverage). */
    uint32_t pages_32[32];
    unsigned char *page_bufs_32 = (unsigned char *)malloc(32 * CS_PAGE_SIZE);
    assert(page_bufs_32);
    for (uint32_t i = 0; i < 32; ++i) {
        pages_32[i] = i + 1u;
        memset(page_bufs_32 + (size_t)i * CS_PAGE_SIZE, (unsigned char)(i + 1u),
               CS_PAGE_SIZE);
    }
    cs_frame_table frames_32;
    memset(&frames_32, 0, sizeof(frames_32));

    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    store_global = cs_store_create(inner);
    assert(store_global != NULL);

    craft_chain(store_global, ROOT_6, DB_6, COMMIT_6, pages_6, 6,
                page_bufs_6, &frames_6);
    craft_chain(store_global, ROOT_32, DB_32, COMMIT_32, pages_32, 32,
                page_bufs_32, &frames_32);
    free(page_bufs_6);
    free(page_bufs_32);

    head_6 = read_head(store_global, ROOT_6);
    head_32 = read_head(store_global, ROOT_32);

    scenario_1_merge(&frames_6);
    scenario_2_strict(&frames_6);
    scenario_3_parallel(&frames_6);
    scenario_4_whole_object();
    scenario_5_prefetch(&frames_32);
    scenario_6_stats_line(&frames_6);

    printf("lazy coalesce/stats test: all assertions passed\n");
    cs_store_destroy(store_global);
    otsardb_test_memory_store_destroy(inner);
    cleanup_files();
    return 0;
}

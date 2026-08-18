/* tests/lazy_prefetch_test.c — sequential read-ahead
 * prefetch in lazy hydration.
 *
 * Deterministic unit test over a memory store with a hand-crafted (fake)
 * page index: one v4 manifest, one v3 frame-offset segment claiming a known
 * page set. The tests drive otsardb_lazy_hydrate_range directly (the same
 * entry point the VFS page hooks use) and observe the store through a
 * counting wrapper:
 *
 * 1. a sequential miss pattern (pages 1, 2, ...) triggers background
 * prefetch: after the streak is detected, exactly ONE extra coalesced
 * byte-range GET appears and the prefetched pages are applied with the
 * correct bytes (K = OTSARDB_LAZY_PREFETCH, default 8);
 * 2. a random miss pattern never schedules a prefetch (no extra range
 * GETs, no whole-object segment GETs);
 * 3. only pages that exist in the claiming segment's page set are
 * prefetched (sparse page set: the single prefetch GET covers exactly
 * the in-set run, never the missing pages);
 * 4. a prefetch failure (exact-range GET fault) is dropped silently: the
 * next explicit demand miss re-fetches the same page and succeeds;
 * prefetch never poisons the sticky demand-failure flag;
 * 5. OTSARDB_LAZY_PREFETCH=0 disables prefetch entirely;
 * 6. the demand path consumes in-flight windows (no redundant demand
 * GETs for covered pages).
 *
 * The prefetch runs on the 0043 pool worker thread, so assertions that
 * depend on the worker having finished poll a store counter with a
 * generous timeout (memory-store ops complete in microseconds).
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
static void test_sleep_ms(unsigned long ms) { Sleep(ms); }
#else
#include <unistd.h>
#define otsardb_unlink unlink
static void test_sleep_ms(unsigned long ms) { usleep(ms * 1000u); }
#endif

#define PF_PAGE_SIZE 512u
#define PF_DB_PAGES 64u
#define PF_IMAGE "lazy-prefetch-image.db"

static void cleanup_files(void) {
    otsardb_unlink(PF_IMAGE);
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
 * Hand-crafted chain: db.json + one v3 frame-offset segment + one v4
 * manifest recording frame_offsets + HEAD.json. The page set is fully
 * controlled by the test (the "fake page index"). The frame info returned
 * by the encoder is copied out so the test can compute expected byte ranges.
 * ------------------------------------------------------------------------- */

typedef struct pf_frame_table {
    uint64_t offsets[64];
    uint32_t page_nos[64];
    uint32_t count;
} pf_frame_table;

static void craft_chain(otsardb_object_store *store,
                        const char *root,
                        const char *db_id,
                        const char *commit_id,
                        const uint32_t *page_nos,
                        uint32_t page_count,
                        const unsigned char *page_bufs, /* page_count * page_size */
                        pf_frame_table *frames_out) {
    assert(page_count <= 64);

    /* WAL batch + v3 segment with per-frame blocks. */
    char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(PF_PAGE_SIZE, PF_DB_PAGES, page_nos,
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

    /* db.json. */
    otsardb_db_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.format_version = 1;
    metadata.page_size = PF_PAGE_SIZE;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", db_id);
    assert(otsardb_db_metadata_create(store, root, &metadata) == OTSARDB_METADATA_OK);

    /* v4 manifest with pages + frame_offsets. */
    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.page_size = PF_PAGE_SIZE;
    manifest.database_size_pages = PF_DB_PAGES;
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
    manifest.segments[0].pages = (uint32_t *)page_nos; /* borrowed */
    manifest.segments[0].page_count = page_count;
    manifest.segments[0].frame_offsets = frames.offsets; /* borrowed */
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

    /* HEAD.json. */
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
 * (offset, length), counts whole-object GETs of segment keys, and can fault
 * EXACTLY one (offset, length) range — enough to make a prefetch GET fail
 * deterministically while every demand GET (single-page ranges, or the
 * coalesced multi-page run in case 3) still succeeds.
 * ------------------------------------------------------------------------- */

typedef struct pf_range_rec {
    size_t offset;
    size_t length;
} pf_range_rec;

typedef struct pf_store_ctx {
    otsardb_object_store *inner;
    size_t get_range_count;
    size_t segment_get_count;
    pf_range_rec recs[64];
    size_t rec_count;
    int fault_active;
    size_t fault_offset;
    size_t fault_length;
    size_t fault_hits;
} pf_store_ctx;

static pf_store_ctx *pf_ctx(otsardb_object_store *store) {
    return (pf_store_ctx *)store->context;
}

static otsardb_store_result pf_get(otsardb_object_store *store,
                                 const char *key,
                                 otsardb_store_object *out) {
    pf_store_ctx *ctx = pf_ctx(store);
    if (key && strstr(key, "segments/") != NULL) ++ctx->segment_get_count;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result pf_get_range(otsardb_object_store *store,
                                       const char *key,
                                       size_t offset,
                                       size_t length,
                                       otsardb_store_object *out) {
    pf_store_ctx *ctx = pf_ctx(store);
    ++ctx->get_range_count;
    assert(ctx->rec_count < 64);
    ctx->recs[ctx->rec_count].offset = offset;
    ctx->recs[ctx->rec_count].length = length;
    ++ctx->rec_count;
    if (ctx->fault_active && offset == ctx->fault_offset &&
        length == ctx->fault_length) {
        ++ctx->fault_hits;
        memset(out, 0, sizeof(*out));
        return OTSARDB_STORE_ERROR;
    }
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result pf_put(otsardb_object_store *store,
                                 const char *key,
                                 const void *data,
                                 size_t size,
                                 const otsardb_store_put_options *options,
                                 char **out_etag) {
    return otsardb_store_put(pf_ctx(store)->inner, key, data, size, options, out_etag);
}

static void pf_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(pf_ctx(store)->inner, object);
}

static otsardb_store_result pf_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(pf_ctx(store)->inner, key);
}

static otsardb_store_result pf_list(otsardb_object_store *store,
                                  const char *prefix,
                                  otsardb_store_list_callback callback,
                                  void *ctx) {
    return otsardb_store_list(pf_ctx(store)->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops PF_OPS = {
    .get = pf_get,
    .get_range = pf_get_range,
    .put = pf_put,
    .release_object = pf_release,
    .delete_object = pf_delete,
    .list = pf_list,
};

static otsardb_object_store *pf_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    pf_store_ctx *ctx = (pf_store_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    store->ops = &PF_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void pf_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
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

/* Read one page of the image file. */
static void read_image_page(uint32_t page_no, unsigned char out[PF_PAGE_SIZE]) {
    FILE *file = fopen(PF_IMAGE, "rb");
    assert(file);
    assert(fseek(file, (long)(page_no - 1u) * PF_PAGE_SIZE, SEEK_SET) == 0);
    assert(fread(out, 1, PF_PAGE_SIZE, file) == PF_PAGE_SIZE);
    fclose(file);
}

static void assert_page_filled(uint32_t page_no, unsigned char fill) {
    unsigned char page[PF_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < PF_PAGE_SIZE; ++i) {
        assert(page[i] == fill);
    }
}

static void assert_page_zeros(uint32_t page_no) {
    unsigned char page[PF_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < PF_PAGE_SIZE; ++i) {
        assert(page[i] == 0);
    }
}

int main(void) {
    cleanup_files();

    const char *root_full = "lazy/prefetch";
    const char *db_full = "lazy-prefetch-db";
    const char *commit_full = "pf-0001";
    const char *root_sparse = "lazy/prefetch-sparse";
    const char *db_sparse = "lazy-prefetch-sparse-db";
    const char *commit_sparse = "pf-sparse-0001";

    /* Segment claiming pages 1..32; page i is filled with byte i. */
    uint32_t pages_full[32];
    unsigned char *page_bufs_full = (unsigned char *)malloc(32 * PF_PAGE_SIZE);
    assert(page_bufs_full);
    for (uint32_t i = 0; i < 32; ++i) {
        pages_full[i] = i + 1u;
        memset(page_bufs_full + (size_t)i * PF_PAGE_SIZE, (unsigned char)(i + 1u),
               PF_PAGE_SIZE);
    }
    pf_frame_table frames_full;
    memset(&frames_full, 0, sizeof(frames_full));

    /* Sparse segment claiming pages {1, 2, 9, 10, 11, 12}. */
    uint32_t pages_sparse[6] = {1, 2, 9, 10, 11, 12};
    unsigned char *page_bufs_sparse = (unsigned char *)malloc(6 * PF_PAGE_SIZE);
    assert(page_bufs_sparse);
    for (uint32_t i = 0; i < 6; ++i) {
        memset(page_bufs_sparse + (size_t)i * PF_PAGE_SIZE,
               (unsigned char)pages_sparse[i], PF_PAGE_SIZE);
    }
    pf_frame_table frames_sparse;
    memset(&frames_sparse, 0, sizeof(frames_sparse));

    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *store = pf_store_create(inner);
    assert(store != NULL);

    craft_chain(store, root_full, db_full, commit_full,
                pages_full, 32, page_bufs_full, &frames_full);
    craft_chain(store, root_sparse, db_sparse, commit_sparse,
                pages_sparse, 6, page_bufs_sparse, &frames_sparse);
    free(page_bufs_full);
    free(page_bufs_sparse);

    otsardb_head head_full = read_head(store, root_full);
    otsardb_head head_sparse = read_head(store, root_sparse);

    /* ------------------------------------------------------------------ *
     * 1. Sequential scan prefetches: pages 1, 2 (two consecutive misses)
     * schedule the window [3..10] (K = OTSARDB_LAZY_PREFETCH = 8) as ONE
     * coalesced range GET on the pool worker. The prefetched pages are
     * applied with the correct bytes; nothing beyond the window is
     * fetched and no whole-object segment GET ever happens. */
    {
        set_env_text("OTSARDB_LAZY_PREFETCH", "8");
        pf_store_ctx *ctx = pf_ctx(store);
        ctx->fault_active = 0;
        ctx->get_range_count = 0;
        ctx->rec_count = 0;
        ctx->segment_get_count = 0;

        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, root_full, PF_IMAGE,
                                        &head_full, &h));
        assert(ctx->get_range_count == 0);
        assert(ctx->segment_get_count == 0);

        assert(otsardb_lazy_hydrate_range(h, 1, 1) == 1); /* miss 1, streak 1 */
        assert(ctx->get_range_count >= 1);
        assert(otsardb_lazy_hydrate_range(h, 2, 2) == 1); /* miss 2, streak 2 */
        assert(ctx->get_range_count >= 2);

        /* Exactly one background prefetch GET: window [3..10], pages 3..10
         * are indices 2..9 of the sorted page array. */
        poll_until(&ctx->get_range_count, 3);
        test_sleep_ms(200); /* settle: no further jobs may appear */
        assert(ctx->get_range_count == 3);
        assert(ctx->rec_count == 3);
        assert(ctx->recs[2].offset == (size_t)frames_full.offsets[2]);
        assert(ctx->recs[2].length ==
               (size_t)(frames_full.offsets[10] - frames_full.offsets[2]));

        /* Prefetched and applied with the right bytes. */
        for (uint32_t p = 3; p <= 10; ++p) {
            assert_page_filled(p, (unsigned char)p);
        }
        assert_page_filled(1, 1);
        assert_page_filled(2, 2);
        /* Nothing beyond the K window was fetched (still zero-filled). */
        assert_page_zeros(11);
        assert_page_zeros(32);
        /* Demand + prefetch never used a whole-object segment GET. */
        assert(ctx->segment_get_count == 0);

        otsardb_lazy_destroy(h);
        otsardb_unlink(PF_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 2. Random access never prefetches: non-consecutive misses (1, 3, 7,
     * 2, 5) keep the streak at 1, so exactly the 5 demand range GETs
     * happen and nothing is fetched in the background. */
    {
        pf_store_ctx *ctx = pf_ctx(store);
        ctx->fault_active = 0;
        ctx->get_range_count = 0;
        ctx->rec_count = 0;
        ctx->segment_get_count = 0;

        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, root_full, PF_IMAGE,
                                        &head_full, &h));
        const uint32_t random_misses[5] = {1, 3, 7, 2, 5};
        for (size_t i = 0; i < 5; ++i) {
            assert(otsardb_lazy_hydrate_range(h, random_misses[i], random_misses[i]) == 1);
        }
        test_sleep_ms(200); /* any mis-scheduled prefetch would have landed */
        assert(ctx->get_range_count == 5);
        assert(ctx->segment_get_count == 0);

        otsardb_lazy_destroy(h);
        otsardb_unlink(PF_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 3. Only pages in the claiming segment's page set are prefetched: the
     * sparse segment holds {1, 2, 9, 10, 11, 12}, so the window
     * [3..10] after the miss streak becomes the single run [9..10] —
     * one range GET covering exactly pages 9..10, never 3..8. */
    {
        pf_store_ctx *ctx = pf_ctx(store);
        ctx->fault_active = 0;
        ctx->get_range_count = 0;
        ctx->rec_count = 0;
        ctx->segment_get_count = 0;

        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, root_sparse, PF_IMAGE,
                                        &head_sparse, &h));
        assert(otsardb_lazy_hydrate_range(h, 1, 1) == 1);
        assert(ctx->get_range_count >= 1);
        assert(otsardb_lazy_hydrate_range(h, 2, 2) == 1);
        assert(ctx->get_range_count >= 2);

        /* In the sorted sparse page set, page 9 is index 2: the prefetch
         * run [9..10] spans offsets[2]..offsets[4]. */
        poll_until(&ctx->get_range_count, 3);
        test_sleep_ms(200);
        assert(ctx->get_range_count == 3);
        assert(ctx->recs[2].offset == (size_t)frames_sparse.offsets[2]);
        assert(ctx->recs[2].length ==
               (size_t)(frames_sparse.offsets[4] - frames_sparse.offsets[2]));

        assert_page_filled(9, 9);
        assert_page_filled(10, 10);
        assert_page_zeros(3); /* never fetched: not in the page set */
        assert_page_zeros(8);
        assert(ctx->segment_get_count == 0);

        otsardb_lazy_destroy(h);
        otsardb_unlink(PF_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 4. Prefetch failure is dropped silently: the prefetch's exact range
     * GET (pages 3..10) is faulted with OTSARDB_STORE_ERROR, the job is
     * dropped without touching the sticky demand-failure flag, and the
     * next explicit demand miss for page 3 re-fetches and succeeds. */
    {
        pf_store_ctx *ctx = pf_ctx(store);
        ctx->fault_active = 1;
        ctx->fault_offset = (size_t)frames_full.offsets[2];
        ctx->fault_length =
            (size_t)(frames_full.offsets[10] - frames_full.offsets[2]);
        ctx->get_range_count = 0;
        ctx->rec_count = 0;
        ctx->segment_get_count = 0;
        ctx->fault_hits = 0;

        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, root_full, PF_IMAGE,
                                        &head_full, &h));
        assert(otsardb_lazy_hydrate_range(h, 1, 1) == 1);
        assert(otsardb_lazy_hydrate_range(h, 2, 2) == 1);

        /* The prefetch GET must have been attempted (and faulted). */
        poll_until(&ctx->fault_hits, 1);
        test_sleep_ms(100);

        /* Demand still works: the next explicit miss re-fetches page 3. */
        assert(otsardb_lazy_hydrate_range(h, 3, 3) == 1);
        assert(otsardb_lazy_hydrate_range(h, 4, 4) == 1);
        test_sleep_ms(200); /* let the follow-up prefetch (window [4..11])
                               finish before the final counts */
        assert_page_filled(3, 3);
        assert_page_filled(4, 4);
        /* The demand ranges never matched the fault (single-page ranges). */
        assert(ctx->fault_hits == 1);
        assert(ctx->segment_get_count == 0);

        otsardb_lazy_destroy(h);
        otsardb_unlink(PF_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 5. OTSARDB_LAZY_PREFETCH=0 disables prefetch: even a long sequential
     * miss run issues exactly the demand range GETs and nothing else. */
    {
        set_env_text("OTSARDB_LAZY_PREFETCH", "0");
        pf_store_ctx *ctx = pf_ctx(store);
        ctx->fault_active = 0;
        ctx->get_range_count = 0;
        ctx->rec_count = 0;
        ctx->segment_get_count = 0;

        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, root_full, PF_IMAGE,
                                        &head_full, &h));
        for (uint32_t p = 1; p <= 6; ++p) {
            assert(otsardb_lazy_hydrate_range(h, p, p) == 1);
        }
        test_sleep_ms(200);
        assert(ctx->get_range_count == 6);
        assert(ctx->segment_get_count == 0);

        otsardb_lazy_destroy(h);
        otsardb_unlink(PF_IMAGE);
    }

    /* ------------------------------------------------------------------ *
     * 6. The demand path CONSUMES in-flight windows: on a sequential run
     * of misses, every page covered by an active prefetch window is
     * waited on instead of re-fetched. The wire sees exactly 6 range
     * GETs — demand misses 1, 2, 11, 12 + window GETs 3..10 (scheduled
     * at miss 2) and 13..20 (scheduled at miss 12) — never one demand
     * GET per covered page. After a window, the streak restarts at the
     * first uncovered miss (page 11), so the second window schedules
     * from miss 12. */
    {
        set_env_text("OTSARDB_LAZY_PREFETCH", "8");
        pf_store_ctx *ctx = pf_ctx(store);
        ctx->fault_active = 0;
        ctx->get_range_count = 0;
        ctx->rec_count = 0;
        ctx->segment_get_count = 0;

        otsardb_lazy_hydrator *h = NULL;
        assert(otsardb_lazy_restore_store(store, root_full, PF_IMAGE,
                                        &head_full, &h));
        for (uint32_t p = 1; p <= 16; ++p) {
            assert(otsardb_lazy_hydrate_range(h, p, p) == 1);
        }
        /* Every hydrate that waited on a window completed synchronously:
         * exactly 6 range GETs total (4 demand + 2 windows). */
        test_sleep_ms(200);
        assert(ctx->get_range_count == 6);
        assert(ctx->segment_get_count == 0);
        for (uint32_t p = 1; p <= 20; ++p) {
            assert_page_filled(p, (unsigned char)p);
        }
        assert_page_zeros(21); /* beyond the last window (13..20) */

        otsardb_lazy_destroy(h);
        otsardb_unlink(PF_IMAGE);
    }

    printf("lazy prefetch test: all assertions passed\n");
    pf_store_destroy(store);
    otsardb_test_memory_store_destroy(inner);
    cleanup_files();
    return 0;
}

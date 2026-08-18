/* tests/lazy_prefetch_overlap_test.c — regression test for ,
 * audit finding LH-1 (DEFINITE): in `lazy_prefetch_fetch_run`, pages that
 * were already materialised (`bitmap_get`) or no longer claimed by the
 * segment (`lazy_resolve_page != ordinal`) were skipped with `continue`
 * WITHOUT consuming their block from the fetched slice. The next block
 * decode then read the skipped block's header, the positional check
 * (`frame_page != page_no`) failed, and the WHOLE prefetch job was dropped
 * — a silent throughput loss whenever a prefetch window overlapped
 * already-materialised pages (the demand path re-fetched them later).
 *
 * Discrimination: this test builds the overlap deterministically:
 *
 * 1. one v3 frame-offset segment claims pages 1..32 (page i filled with
 * byte i) — the same hand-crafted chain as tests/lazy_prefetch_test.c;
 * 2. demand misses 1 and 2 schedule the prefetch window [3..10] (K =
 * OTSARDB_LAZY_PREFETCH = 8);
 * 3. the counting store GATES the window's exact byte-range GET (the
 * prefetch pool worker blocks inside the GET, hydrator mutex released);
 * 4. while the worker is blocked, the test materialises page 5 (the
 * SQLite write path, otsardb_lazy_mark_range — it never waits on
 * prefetch windows), then opens the gate.
 *
 * With the pre-0144 code the worker's apply loop `continue`s on page 5
 * without decoding its block, the page-6 decode misaligns and drops the
 * job: pages 6..10 stay zero-filled. With the fix every block is
 * decoded/consumed and only the WRITE is skipped: pages 3,4,6..10 are
 * prefetched with the correct bytes and page 5 keeps its marked state.
 *
 * Deterministic: the gate guarantees the mark happens strictly between the
 * window GET and its apply; no timing race, no sleeps in the assertion
 * path (only a bounded poll for the async worker).
 *
 * Wiring (coordinator — , NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_lazy_prefetch_overlap_test
 * tests/lazy_prefetch_overlap_test.c
 * tests/memory_store.c)
 * target_include_directories(otsardb_lazy_prefetch_overlap_test
 * PRIVATE include src/core src/storage tests)
 * target_link_libraries(otsardb_lazy_prefetch_overlap_test PRIVATE otsardb_core)
 * add_test(NAME otsardb_lazy_prefetch_overlap
 * COMMAND otsardb_lazy_prefetch_overlap_test)
 * set_tests_properties(otsardb_lazy_prefetch_overlap PROPERTIES TIMEOUT 60)
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

#define LH1_PAGE_SIZE 512u
#define LH1_DB_PAGES 64u
#define LH1_IMAGE "lazy-prefetch-overlap-image.db"
#define LH1_ROOT "lazy/prefetch-overlap"
#define LH1_DB_ID "lazy-prefetch-overlap-db"
#define LH1_COMMIT "lh1-0001"

static void cleanup_files(void) {
    otsardb_unlink(LH1_IMAGE);
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
 * Hand-crafted chain: one v3 frame-offset segment claiming pages 1..32.
 * ------------------------------------------------------------------------- */

typedef struct lh1_frame_table {
    uint64_t offsets[64];
    uint32_t page_nos[64];
    uint32_t count;
    uint64_t segment_size;
} lh1_frame_table;

static void craft_chain(otsardb_object_store *store,
                        const uint32_t *page_nos,
                        uint32_t page_count,
                        const unsigned char *page_bufs,
                        lh1_frame_table *frames_out) {
    assert(page_count <= 64);

    char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(LH1_PAGE_SIZE, LH1_DB_PAGES, page_nos,
                                  page_bufs, page_count, (unsigned char **)&batch,
                                  &batch_size));
    unsigned char *segment_data = NULL;
    size_t segment_size = 0;
    char segment_sha[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
    otsardb_segment_frame_info frames;
    memset(&frames, 0, sizeof(frames));
    assert(otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                            LH1_COMMIT,
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
    metadata.page_size = LH1_PAGE_SIZE;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", LH1_DB_ID);
    assert(otsardb_db_metadata_create(store, LH1_ROOT, &metadata) == OTSARDB_METADATA_OK);

    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.page_size = LH1_PAGE_SIZE;
    manifest.database_size_pages = LH1_DB_PAGES;
    snprintf(manifest.database_id, sizeof(manifest.database_id), "%s", LH1_DB_ID);
    snprintf(manifest.commit_id, sizeof(manifest.commit_id), "%s", LH1_COMMIT);
    manifest.segment_count = 1;
    snprintf(manifest.segments[0].key,
             sizeof(manifest.segments[0].key),
             "segments/%s.otsseg", LH1_COMMIT);
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
    snprintf(seg_key, sizeof(seg_key), "%s/%s", LH1_ROOT, manifest.segments[0].key);
    otsardb_store_put_options plain = {0};
    assert(otsardb_store_put(store, seg_key, segment_data, segment_size,
                           &plain, NULL) == OTSARDB_STORE_OK);
    char man_key[OTSARDB_KEY_MAX + 1];
    snprintf(man_key, sizeof(man_key), "%s/commits/%s.json", LH1_ROOT, LH1_COMMIT);
    assert(otsardb_store_put(store, man_key, manifest_json, manifest_size,
                           &plain, NULL) == OTSARDB_STORE_OK);

    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = 1;
    snprintf(head.database_id, sizeof(head.database_id), "%s", LH1_DB_ID);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", LH1_COMMIT);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s", man_key);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", manifest_sha);
    char *head_json = NULL;
    size_t head_size = 0;
    assert(otsardb_head_encode_json(&head, &head_json, &head_size));
    char head_key[OTSARDB_KEY_MAX + 1];
    snprintf(head_key, sizeof(head_key), "%s/HEAD.json", LH1_ROOT);
    assert(otsardb_store_put(store, head_key, head_json, head_size,
                           &plain, NULL) == OTSARDB_STORE_OK);

    otsardb_segment_frame_info_release(&frames);
    free(head_json);
    free(manifest_json);
    free(segment_data);
}

static otsardb_head read_head(otsardb_object_store *store) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, LH1_ROOT, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

/* ------------------------------------------------------------------------- *
 * Gated counting store: counts get_range / whole-object segment GETs and
 * can block EXACTLY the prefetch window's byte-range GET until the test
 * opens the gate (the worker is then mid-GET, holding no hydrator lock).
 * ------------------------------------------------------------------------- */

typedef struct lh1_store_ctx {
    otsardb_object_store *inner;
    size_t get_range_count;
    size_t segment_get_count;
    size_t rec_count;
    size_t gate_offset;
    size_t gate_length;
    volatile int gate_active;
    volatile int gate_entered;
    volatile int gate_open;
} lh1_store_ctx;

static lh1_store_ctx *lh1_ctx(otsardb_object_store *store) {
    return (lh1_store_ctx *)store->context;
}

static otsardb_store_result lh1_get(otsardb_object_store *store,
                                   const char *key,
                                   otsardb_store_object *out) {
    lh1_store_ctx *ctx = lh1_ctx(store);
    if (key && strstr(key, "segments/") != NULL) ++ctx->segment_get_count;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result lh1_get_range(otsardb_object_store *store,
                                        const char *key,
                                        size_t offset,
                                        size_t length,
                                        otsardb_store_object *out) {
    lh1_store_ctx *ctx = lh1_ctx(store);
    ++ctx->get_range_count;
    if (ctx->gate_active && offset == ctx->gate_offset &&
        length == ctx->gate_length) {
        ctx->gate_entered = 1;
        while (!ctx->gate_open) test_sleep_ms(1);
    }
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result lh1_put(otsardb_object_store *store,
                                   const char *key,
                                   const void *data,
                                   size_t size,
                                   const otsardb_store_put_options *options,
                                   char **out_etag) {
    return otsardb_store_put(lh1_ctx(store)->inner, key, data, size, options, out_etag);
}

static void lh1_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(lh1_ctx(store)->inner, object);
}

static otsardb_store_result lh1_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(lh1_ctx(store)->inner, key);
}

static otsardb_store_result lh1_list(otsardb_object_store *store,
                                   const char *prefix,
                                   otsardb_store_list_callback callback,
                                   void *ctx) {
    return otsardb_store_list(lh1_ctx(store)->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops LH1_OPS = {
    .get = lh1_get,
    .get_range = lh1_get_range,
    .put = lh1_put,
    .release_object = lh1_release,
    .delete_object = lh1_delete,
    .list = lh1_list,
};

static otsardb_object_store *lh1_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    lh1_store_ctx *ctx = (lh1_store_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    store->ops = &LH1_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void lh1_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

/* ------------------------------------------------------------------------- *
 * Image assertions + bounded polls.
 * ------------------------------------------------------------------------- */

static void read_image_page(uint32_t page_no, unsigned char out[LH1_PAGE_SIZE]) {
    FILE *file = fopen(LH1_IMAGE, "rb");
    assert(file);
    assert(fseek(file, (long)(page_no - 1u) * LH1_PAGE_SIZE, SEEK_SET) == 0);
    assert(fread(out, 1, LH1_PAGE_SIZE, file) == LH1_PAGE_SIZE);
    fclose(file);
}

static int page_filled(uint32_t page_no, unsigned char fill) {
    unsigned char page[LH1_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < LH1_PAGE_SIZE; ++i) {
        if (page[i] != fill) return 0;
    }
    return 1;
}

static void assert_page_filled(uint32_t page_no, unsigned char fill) {
    assert(page_filled(page_no, fill));
}

static void assert_page_zeros(uint32_t page_no) {
    unsigned char page[LH1_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < LH1_PAGE_SIZE; ++i) {
        assert(page[i] == 0);
    }
}

static void poll_until(int *flag) {
    unsigned long waited_ms = 0;
    while (!*flag && waited_ms < 5000) {
        test_sleep_ms(5);
        waited_ms += 5;
    }
    assert(*flag);
}

static void poll_page_filled(uint32_t page_no, unsigned char fill) {
    unsigned long waited_ms = 0;
    while (!page_filled(page_no, fill) && waited_ms < 5000) {
        test_sleep_ms(5);
        waited_ms += 5;
    }
    assert(page_filled(page_no, fill));
}

int main(void) {
    cleanup_files();

    uint32_t pages[32];
    unsigned char *page_bufs = (unsigned char *)malloc(32 * LH1_PAGE_SIZE);
    assert(page_bufs);
    for (uint32_t i = 0; i < 32; ++i) {
        pages[i] = i + 1u;
        memset(page_bufs + (size_t)i * LH1_PAGE_SIZE, (unsigned char)(i + 1u),
               LH1_PAGE_SIZE);
    }
    lh1_frame_table frames;
    memset(&frames, 0, sizeof(frames));

    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *store = lh1_store_create(inner);
    assert(store != NULL);

    craft_chain(store, pages, 32, page_bufs, &frames);
    free(page_bufs);
    otsardb_head head = read_head(store);

    /* ------------------------------------------------------------------ *
     * The overlap: demand misses 1, 2 schedule the window [3..10]; the
     * window's range GET is gated mid-flight; page 5 is materialised by
     * the SQLite write path while the GET is in flight; the gate then
     * opens. Pages 3, 4, 6..10 must ALL be prefetched (only the page-5
     * WRITE is skipped), never a dropped job. */
    set_env_text("OTSARDB_LAZY_PREFETCH", "8");
    lh1_store_ctx *ctx = lh1_ctx(store);
    ctx->gate_active = 1;
    /* Window [3..10] = block indices 2..9 of the sorted page array. */
    ctx->gate_offset = (size_t)frames.offsets[2];
    ctx->gate_length = (size_t)(frames.offsets[10] - frames.offsets[2]);
    ctx->gate_entered = 0;
    ctx->gate_open = 0;
    ctx->get_range_count = 0;
    ctx->segment_get_count = 0;

    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store, LH1_ROOT, LH1_IMAGE, &head, &h));

    /* Two sequential demand misses schedule the window. */
    assert(otsardb_lazy_hydrate_range(h, 1, 1) == 1);
    assert(otsardb_lazy_hydrate_range(h, 2, 2) == 1);

    /* The worker must now be blocked inside the window's range GET
     * (hydrator mutex released), before any apply can start. */
    poll_until((int *)&ctx->gate_entered);
    assert(ctx->get_range_count >= 2); /* two demand GETs have happened */

    /* Materialise page 5 — INSIDE the prefetch window — while the GET is
     * in flight. This is the LH-1 overlap. */
    otsardb_lazy_mark_range(h, 5, 5);

    /* Open the gate: the worker completes the GET and applies. */
    ctx->gate_open = 1;

    /* Every remaining page of the window must be prefetched. Wait for the
     * far end (page 10) to prove the whole run was consumed and applied. */
    poll_page_filled(10, 10);
    test_sleep_ms(200); /* settle: no further prefetch jobs may appear */

    assert_page_filled(1, 1);
    assert_page_filled(2, 2);
    assert_page_filled(3, 3);
    assert_page_filled(4, 4);
    assert_page_zeros(5);   /* marked but never written by the window */
    assert_page_filled(6, 6);
    assert_page_filled(7, 7);
    assert_page_filled(8, 8);
    assert_page_filled(9, 9);
    assert_page_filled(10, 10);
    /* Nothing beyond the K window was fetched. */
    assert_page_zeros(11);
    assert_page_zeros(32);
    /* Range GETs: demand 1, demand 2, window [3..10]. No whole-object GETs. */
    assert(ctx->get_range_count == 3);
    assert(ctx->segment_get_count == 0);

    otsardb_lazy_destroy(h);
    lh1_store_destroy(store);
    otsardb_test_memory_store_destroy(inner);
    cleanup_files();

    printf("lazy prefetch overlap test: all assertions passed\n");
    return 0;
}

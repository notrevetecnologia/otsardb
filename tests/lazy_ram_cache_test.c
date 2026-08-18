/* tests/lazy_ram_cache_test.c - RAM page cache tier in
 * lazy hydration.
 *
 * Deterministic unit test over a memory store with a hand-crafted (fake)
 * page index (one v4 manifest, one v3 frame-offset segment claiming a known
 * page set), driving otsardb_lazy_hydrate_range directly - the same entry
 * point the VFS page hooks use - and observing the store through a counting
 * wrapper:
 *
 * 1. N reads -> 1 GET: the first pass over 16 pages costs exactly ONE
 * coalesced range GET and caches every page; after a bitmap clear
 * (otsardb_lazy_clear_beyond, the truncate-hook equivalent) the second
 * pass costs ZERO GETs - every page is served from the RAM tier
 * (ram_hits == 16, ram_misses == 16, image bytes identical);
 * 2. strict LRU eviction under OTSARDB_S3_RAM_CACHE_MB: a 1 MiB budget holds
 * 2048 pages of 512 B; hydrating 3000 pages evicts the oldest entries
 * (pages 1..952), so after a clear the evicted pages re-fetch (1 GET)
 * while the resident ones (953..3000) hit (0 GETs);
 * 3. OTSARDB_S3_RAM_CACHE_MB=0 disables the tier: a cleared re-read issues a
 * fresh GET and the ram counters stay at 0;
 * 4. stats: otsardb_lazy_hydration_stats_get exposes ram_hits/ram_misses and
 * OTSARDB_S3_READ_STATS=1 prints the canonical line (still
 * byte-identical, parsed field-by-field) followed by the RAM tier line
 * "otsardb: read-stats ram_hits=N ram_misses=N" (stderr captured via
 * dup2);
 * 5. prefetch fills the cache too: a sequential miss streak's background
 * window materialises AND caches pages 3..10; after a bitmap clear the
 * whole window is served from RAM with zero additional GETs;
 * 6. SQLite-written pages are invalidated: otsardb_lazy_mark_range removes
 * the marked page from the tier (a superseded copy must never be served
 * after a bitmap clear) - the re-read re-fetches exactly that one page;
 * 7. whole-object path caching: a store WITHOUT RANGE_GET serves pages via
 * one whole-object GET; after a bitmap clear the re-read is served from
 * the tier with zero additional GETs (the per-page demand loop resolves
 * the RAM tier when the inline pass cannot run).
 *
 * The prefetch window in scenario 5 runs on the 0043 pool worker thread, so
 * assertions that depend on it having finished poll a store counter with a
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

#define RC_PAGE_SIZE 512u
#define RC_DB_PAGES 4096u
#define RC_IMAGE "lazy-ram-cache-image.db"
#define RC_CAPTURE "lazy-ram-cache-stderr.cap"

static void cleanup_files(void) {
    otsardb_unlink(RC_IMAGE);
    otsardb_unlink(RC_CAPTURE);
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
 * controlled by the test. frame_out (optional) receives a COPY of the
 * encoder's frame table (release with otsardb_segment_frame_info_release).
 * ------------------------------------------------------------------------- */

static void craft_chain(otsardb_object_store *store,
                        const char *root,
                        const char *db_id,
                        const char *commit_id,
                        const uint32_t *page_nos,
                        uint32_t page_count,
                        const unsigned char *page_bufs, /* page_count * page_size */
                        otsardb_segment_frame_info *frames_out) {
    char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(RC_PAGE_SIZE, RC_DB_PAGES, page_nos,
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

    otsardb_db_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.format_version = 1;
    metadata.page_size = RC_PAGE_SIZE;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", db_id);
    assert(otsardb_db_metadata_create(store, root, &metadata) == OTSARDB_METADATA_OK);

    otsardb_commit_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.format_version = 4;
    manifest.generation = 1;
    manifest.parent_generation = 0;
    manifest.page_size = RC_PAGE_SIZE;
    manifest.database_size_pages = RC_DB_PAGES;
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

    if (frames_out) {
        otsardb_segment_frame_info_release(frames_out);
        *frames_out = frames;
        frames.offsets = NULL;
        frames.page_nos = NULL;
        frames.count = 0;
    }
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
 * Counting store wrapper: counts range GETs (with offsets/lengths) and
 * whole-object segment GETs, and can strip the RANGE_GET capability (the
 * whole-object-path scenario). All other calls delegate to the inner store.
 * ------------------------------------------------------------------------- */

typedef struct rc_range_rec {
    size_t offset;
    size_t length;
} rc_range_rec;

typedef struct rc_store_ctx {
    otsardb_object_store *inner;
    size_t get_range_count;
    size_t segment_get_count;
    rc_range_rec recs[64];
    size_t rec_count;
    otsardb_store_capabilities capabilities; /* override (0 = inherit) */
} rc_store_ctx;

static rc_store_ctx *rc_ctx(otsardb_object_store *store) {
    return (rc_store_ctx *)store->context;
}

static otsardb_store_result rc_get(otsardb_object_store *store,
                                 const char *key,
                                 otsardb_store_object *out) {
    rc_store_ctx *ctx = rc_ctx(store);
    if (key && strstr(key, "segments/") != NULL) ++ctx->segment_get_count;
    return otsardb_store_get(ctx->inner, key, out);
}

static otsardb_store_result rc_get_range(otsardb_object_store *store,
                                       const char *key,
                                       size_t offset,
                                       size_t length,
                                       otsardb_store_object *out) {
    rc_store_ctx *ctx = rc_ctx(store);
    ++ctx->get_range_count;
    assert(ctx->rec_count < 64);
    ctx->recs[ctx->rec_count].offset = offset;
    ctx->recs[ctx->rec_count].length = length;
    ++ctx->rec_count;
    return otsardb_store_get_range(ctx->inner, key, offset, length, out);
}

static otsardb_store_result rc_put(otsardb_object_store *store,
                                 const char *key,
                                 const void *data,
                                 size_t size,
                                 const otsardb_store_put_options *options,
                                 char **out_etag) {
    return otsardb_store_put(rc_ctx(store)->inner, key, data, size, options, out_etag);
}

static void rc_release(otsardb_object_store *store, otsardb_store_object *object) {
    otsardb_store_release_object(rc_ctx(store)->inner, object);
}

static otsardb_store_result rc_delete(otsardb_object_store *store, const char *key) {
    return otsardb_store_delete(rc_ctx(store)->inner, key);
}

static otsardb_store_result rc_list(otsardb_object_store *store,
                                  const char *prefix,
                                  otsardb_store_list_callback callback,
                                  void *ctx) {
    return otsardb_store_list(rc_ctx(store)->inner, prefix, callback, ctx);
}

static const otsardb_object_store_ops RC_OPS = {
    .get = rc_get,
    .get_range = rc_get_range,
    .put = rc_put,
    .release_object = rc_release,
    .delete_object = rc_delete,
    .list = rc_list,
};

static otsardb_object_store *rc_store_create(otsardb_object_store *inner) {
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    rc_store_ctx *ctx = (rc_store_ctx *)calloc(1, sizeof(*ctx));
    assert(store && ctx);
    ctx->inner = inner;
    store->ops = &RC_OPS;
    store->context = ctx;
    store->capabilities = inner->capabilities;
    return store;
}

static void rc_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    free(store->context);
    free(store);
}

static void rc_reset_counts(otsardb_object_store *store) {
    rc_store_ctx *ctx = rc_ctx(store);
    ctx->get_range_count = 0;
    ctx->segment_get_count = 0;
    ctx->rec_count = 0;
}

static void rc_disable_range_get(otsardb_object_store *store) {
    rc_store_ctx *ctx = rc_ctx(store);
    ctx->capabilities = ctx->inner->capabilities & ~OTSARDB_STORE_CAP_RANGE_GET;
    store->capabilities = ctx->capabilities;
}

static void rc_enable_capabilities(otsardb_object_store *store) {
    rc_store_ctx *ctx = rc_ctx(store);
    ctx->capabilities = ctx->inner->capabilities;
    store->capabilities = ctx->capabilities;
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
static void read_image_page(uint32_t page_no, unsigned char out[RC_PAGE_SIZE]) {
    FILE *file = fopen(RC_IMAGE, "rb");
    assert(file);
    assert(fseek(file, (long)(page_no - 1u) * RC_PAGE_SIZE, SEEK_SET) == 0);
    assert(fread(out, 1, RC_PAGE_SIZE, file) == RC_PAGE_SIZE);
    fclose(file);
}

static void assert_page_filled(uint32_t page_no, unsigned char fill) {
    unsigned char page[RC_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < RC_PAGE_SIZE; ++i) {
        assert(page[i] == fill);
    }
}

static void assert_page_zeros(uint32_t page_no) {
    unsigned char page[RC_PAGE_SIZE];
    read_image_page(page_no, page);
    for (size_t i = 0; i < RC_PAGE_SIZE; ++i) {
        assert(page[i] == 0);
    }
}

static void capture_stderr_begin(FILE **cap, int *saved_fd) {
    fflush(stderr);
    *saved_fd = otsardb_dup(fileno(stderr));
    assert(*saved_fd >= 0);
    *cap = fopen(RC_CAPTURE, "w");
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

/* ------------------------------------------------------------------------- *
 * 1. N reads -> 1 GET: a first pass over pages 1..16 costs ONE coalesced
 * range GET and caches every page; after a bitmap clear (the truncate
 * hook equivalent) a second full pass costs ZERO additional GETs -
 * 16 ram_hits, 0 GETs, byte-identical image. A third pass, one page at a
 * time, also costs ZERO GETs (per-read cache hits). */
static void scenario_1_ntoone(otsardb_object_store *store, const char *root,
                              const char *db, const char *commit,
                              uint32_t *pages, unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "64");
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));
    assert(rc_ctx(store)->get_range_count == 0);
    assert(rc_ctx(store)->segment_get_count == 0);

    /* First pass: 16 consecutive pages -> ONE coalesced range GET. */
    assert(otsardb_lazy_hydrate_range(h, 1, 16) == 1);
    assert(rc_ctx(store)->get_range_count == 1);
    assert(rc_ctx(store)->segment_get_count == 0);
    for (uint32_t p = 1; p <= 16; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 1);
    assert(stats.range_gets == 1);
    assert(stats.demand_misses == 16);
    assert(stats.ram_hits == 0);
    assert(stats.ram_misses == 16);

    /* Bitmap clear (SQLite xTruncate hook): pages must re-materialise. */
    otsardb_lazy_clear_beyond(h, 0);

    /* Second pass: everything served from the RAM tier - zero GETs. */
    assert(otsardb_lazy_hydrate_range(h, 1, 16) == 1);
    assert(rc_ctx(store)->get_range_count == 1);
    for (uint32_t p = 1; p <= 16; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 1);
    assert(stats.demand_misses == 32); /* 16 fetched + 16 cache-served */
    assert(stats.ram_hits == 16);
    assert(stats.ram_misses == 16);

    /* Third pass, one page at a time (the per-read demand shape): still
     * zero GETs - every read is a RAM hit. */
    otsardb_lazy_clear_beyond(h, 0);
    for (uint32_t p = 1; p <= 16; ++p) {
        assert(otsardb_lazy_hydrate_range(h, p, p) == 1);
    }
    assert(rc_ctx(store)->get_range_count == 1);
    for (uint32_t p = 1; p <= 16; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.ram_hits == 32);
    assert(stats.ram_misses == 16);

    otsardb_lazy_destroy(h);
    otsardb_unlink(RC_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * 2. Strict LRU eviction under budget: 1 MiB holds 2048 pages of 512 B;
 * hydrating 3000 pages evicts pages 1..952. After a bitmap clear, the
 * evicted pages re-fetch (exactly ONE range GET each) while the resident
 * ones are served from RAM (zero GETs). */
static void scenario_2_lru(otsardb_object_store *store, const char *root,
                           const char *db, const char *commit,
                           uint32_t *pages, unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "1");
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));

    /* One coalesced GET over the whole 3000-page run; every page is cached,
     * evicting strict-LRU (oldest first) down to the 2048-entry budget. */
    assert(otsardb_lazy_hydrate_range(h, 1, 3000) == 1);
    assert(rc_ctx(store)->get_range_count == 1);
    for (uint32_t p = 1; p <= 3000; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.ram_hits == 0);
    assert(stats.ram_misses == 3000);

    /* Bitmap clear: pages 1..952 were evicted (LRU), 953..3000 resident. */
    otsardb_lazy_clear_beyond(h, 0);

    /* Evicted page: a fresh single-page range GET. */
    assert(otsardb_lazy_hydrate_range(h, 1, 1) == 1);
    assert(rc_ctx(store)->get_range_count == 2);

    /* Resident pages: served from RAM, zero GETs. */
    assert(otsardb_lazy_hydrate_range(h, 3000, 3000) == 1);
    assert(otsardb_lazy_hydrate_range(h, 954, 954) == 1);
    assert(rc_ctx(store)->get_range_count == 2);

    /* The LRU tail moved: page 953 was evicted when page 1 was re-stored. */
    assert(otsardb_lazy_hydrate_range(h, 953, 953) == 1);
    assert(rc_ctx(store)->get_range_count == 3);

    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.total_gets == 3);
    assert(stats.ram_hits == 2);
    assert(stats.ram_misses == 3002);

    otsardb_lazy_destroy(h);
    otsardb_unlink(RC_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * 3. OTSARDB_S3_RAM_CACHE_MB=0 disables the tier: a cleared re-read issues a
 * fresh GET and the ram counters stay at 0. */
static void scenario_3_disabled(otsardb_object_store *store, const char *root,
                                const char *db, const char *commit,
                                uint32_t *pages, unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "0");
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));

    assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
    assert(rc_ctx(store)->get_range_count == 1);

    otsardb_lazy_clear_beyond(h, 0);

    /* Without the tier the cleared re-read re-fetches from the store. */
    assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
    assert(rc_ctx(store)->get_range_count == 2);
    for (uint32_t p = 1; p <= 4; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.ram_hits == 0);
    assert(stats.ram_misses == 0);
    assert(stats.total_gets == 2);

    otsardb_lazy_destroy(h);
    otsardb_unlink(RC_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * 4. stats + OTSARDB_S3_READ_STATS: the RAM tier counters ride on a SECOND
 * "otsardb: read-stats ram_hits=N ram_misses=N" line; the canonical 0082
 * line stays byte-identical (parsed field-by-field, 9 fields). */
static void scenario_4_stats(otsardb_object_store *store, const char *root,
                             const char *db, const char *commit,
                             uint32_t *pages, unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "64");
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));
    assert(otsardb_lazy_hydrate_range(h, 1, 16) == 1);
    otsardb_lazy_clear_beyond(h, 0);
    assert(otsardb_lazy_hydrate_range(h, 1, 16) == 1);

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.ram_hits == 16);
    assert(stats.ram_misses == 16);
    assert(stats.total_gets == 1);

    /* Canonical line format is unchanged (0082 contract): 9 fields, same
     * order; bytes_fetched/hydration_ms are timing/codec-dependent, so the
     * field layout is verified by parsing, not by byte comparison. */
    char line[512];
    int n = otsardb_lazy_hydration_stats_format(&stats, line, sizeof(line));
    assert(n > 0 && (size_t)n < sizeof(line));
    unsigned long long tg = 0, rg = 0, bf = 0, dm = 0, pf = 0, ph = 0, cr = 0,
                       ss = 0, hms = 0;
    int parsed = sscanf(line,
                        "otsardb: read-stats total_gets=%llu range_gets=%llu "
                        "bytes_fetched=%llu demand_misses=%llu "
                        "prefetch_fetches=%llu prefetch_hits=%llu "
                        "coalesced_runs=%llu sequential_streaks=%llu "
                        "hydration_ms=%llu",
                        &tg, &rg, &bf, &dm, &pf, &ph, &cr, &ss, &hms);
    assert(parsed == 9);
    assert(tg == 1 && rg == 1 && dm == 32 && pf == 0 && ph == 0 && cr == 1 &&
           ss == 0);

    /* RAM tier line format. */
    char ram_line[128];
    n = otsardb_lazy_hydration_stats_format_ram(&stats, ram_line, sizeof(ram_line));
    assert(n > 0 && (size_t)n < sizeof(ram_line));
    assert(strncmp(ram_line, "otsardb: read-stats ram_hits=16 ram_misses=16\n",
                   strlen(ram_line)) == 0);

    /* Session-close print (dup2 capture): line 1 = canonical 9 fields,
     * line 2 = the RAM tier line with the session's real counts. */
    set_env_text("OTSARDB_S3_READ_STATS", "1");
    FILE *cap = NULL;
    int saved_fd = -1;
    capture_stderr_begin(&cap, &saved_fd);
    otsardb_lazy_destroy(h);
    capture_stderr_end(&cap, &saved_fd);
    set_env_text("OTSARDB_S3_READ_STATS", "0");

    FILE *file = fopen(RC_CAPTURE, "rb");
    assert(file != NULL);
    char captured[512];
    assert(fgets(captured, sizeof(captured), file) != NULL);
    assert(strncmp(captured, "otsardb: read-stats ", 18) == 0);
    unsigned long long c_tg = 0, c_rg = 0, c_bf = 0, c_dm = 0, c_pf = 0,
                       c_ph = 0, c_cr = 0, c_ss = 0, c_hms = 0;
    int parsed2 = sscanf(captured,
                        "otsardb: read-stats total_gets=%llu range_gets=%llu "
                        "bytes_fetched=%llu demand_misses=%llu "
                        "prefetch_fetches=%llu prefetch_hits=%llu "
                        "coalesced_runs=%llu sequential_streaks=%llu "
                        "hydration_ms=%llu",
                        &c_tg, &c_rg, &c_bf, &c_dm, &c_pf, &c_ph, &c_cr, &c_ss,
                        &c_hms);
    assert(parsed2 == 9);
    assert(c_tg == 1 && c_rg == 1 && c_dm == 32 && c_cr == 1);
    assert(fgets(captured, sizeof(captured), file) != NULL);
    unsigned long long rh = 0, rm = 0;
    parsed = sscanf(captured, "otsardb: read-stats ram_hits=%llu ram_misses=%llu",
                    &rh, &rm);
    assert(parsed == 2);
    assert(rh == 16);
    assert(rm == 16);
    fclose(file);

    otsardb_unlink(RC_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * 5. Prefetch fills the cache: the sequential-miss window [3..10] is
 * materialised AND cached by the background worker; after a bitmap clear
 * the whole window is served from RAM with zero additional GETs. */
static void scenario_5_prefetch(otsardb_object_store *store, const char *root,
                                const char *db, const char *commit,
                                uint32_t *pages, unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "8");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "64");
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));

    assert(otsardb_lazy_hydrate_range(h, 1, 1) == 1);
    assert(rc_ctx(store)->get_range_count >= 1);
    assert(otsardb_lazy_hydrate_range(h, 2, 2) == 1);
    assert(rc_ctx(store)->get_range_count >= 2);

    /* Exactly one background window GET (pages 3..10). */
    poll_until(&rc_ctx(store)->get_range_count, 3);
    test_sleep_ms(200); /* settle: no further jobs may appear */
    assert(rc_ctx(store)->get_range_count == 3);
    assert(rc_ctx(store)->segment_get_count == 0);
    for (uint32_t p = 1; p <= 10; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    /* Bitmap clear; the whole window is served from RAM - zero GETs. */
    otsardb_lazy_clear_beyond(h, 0);
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    assert(otsardb_lazy_hydrate_range(h, 1, 10) == 1);
    assert(rc_ctx(store)->get_range_count == 3);
    for (uint32_t p = 1; p <= 10; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.ram_hits == 10);
    assert(stats.ram_misses == 2);
    assert(stats.prefetch_fetches == 8);

    otsardb_lazy_destroy(h);
    otsardb_unlink(RC_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * 6. SQLite-written pages are invalidated: otsardb_lazy_mark_range removes
 * the marked page from the tier, so after a bitmap clear the re-read
 * re-fetches EXACTLY that page while its neighbours still hit. */
static void scenario_6_mark_invalidate(otsardb_object_store *store,
                                       const char *root, const char *db,
                                       const char *commit, uint32_t *pages,
                                       unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "64");
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));

    assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
    assert(rc_ctx(store)->get_range_count == 1);

    /* SQLite wrote page 2 (VFS xWrite hook): the cached copy is superseded. */
    otsardb_lazy_mark_range(h, 2, 2);
    otsardb_lazy_clear_beyond(h, 0);

    /* Pages 1, 3, 4 hit; page 2 must re-fetch (its cache entry is gone). */
    assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
    assert(rc_ctx(store)->get_range_count == 2);
    for (uint32_t p = 1; p <= 4; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    assert(stats.ram_hits == 3);
    /* First pass consulted the tier for pages 1..4 (4 misses); the re-read
     * consulted it again for 1..4 (page 2 was invalidated by mark_range, so
     * it misses again): 4 + 1 = 5. */
    assert(stats.ram_misses == 5);

    otsardb_lazy_destroy(h);
    otsardb_unlink(RC_IMAGE);
}

/* ------------------------------------------------------------------------- *
 * 7. Whole-object path caching: a store WITHOUT RANGE_GET serves pages via
 * one whole-object GET (v1-v3 fallback shape); after a bitmap clear the
 * re-read is served from the tier with zero additional GETs - the
 * per-page demand loop resolves the RAM tier when the inline pass cannot
 * run. */
static void scenario_7_whole_object(otsardb_object_store *store,
                                    const char *root, const char *db,
                                    const char *commit, uint32_t *pages,
                                    unsigned char *page_bufs) {
    set_env_text("OTSARDB_LAZY_PREFETCH", "0");
    set_env_text("OTSARDB_S3_RAM_CACHE_MB", "64");
    rc_disable_range_get(store);
    rc_reset_counts(store);

    otsardb_lazy_hydrator *h = NULL;
    otsardb_head head = read_head(store, root);
    assert(otsardb_lazy_restore_store(store, root, RC_IMAGE, &head, &h));

    assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
    assert(rc_ctx(store)->get_range_count == 0);
    assert(rc_ctx(store)->segment_get_count == 1);
    for (uint32_t p = 1; p <= 4; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_clear_beyond(h, 0);
    assert(otsardb_lazy_hydrate_range(h, 1, 4) == 1);
    assert(rc_ctx(store)->get_range_count == 0);
    assert(rc_ctx(store)->segment_get_count == 1);
    for (uint32_t p = 1; p <= 4; ++p) {
        assert_page_filled(p, (unsigned char)p);
    }

    otsardb_lazy_hydration_stats stats;
    assert(otsardb_lazy_hydration_stats_get(h, &stats));
    /* Pass 1: page 1 misses (1), its whole-object apply caches all 32
     * segment pages, so pages 2..4 hit (3); pass 2 after the clear: pages
     * 1..4 all hit (4 more). Total: 7 hits, 1 miss, 1 whole-object GET. */
    assert(stats.ram_hits == 7);
    assert(stats.ram_misses == 1);

    rc_enable_capabilities(store);
    otsardb_lazy_destroy(h);
    otsardb_unlink(RC_IMAGE);
}

int main(void) {
    cleanup_files();
    set_env_text("OTSARDB_S3_READ_STATS", "0");

    /* Chain 1: pages 1..32, page i filled with byte i (scenarios 1, 3-7). */
    const char *root_a = "lazy/ram-cache-a";
    const char *db_a = "lazy-ram-cache-db-a";
    const char *commit_a = "rc-0001";
    uint32_t pages_a[32];
    unsigned char *page_bufs_a = (unsigned char *)malloc(32 * RC_PAGE_SIZE);
    assert(page_bufs_a);
    for (uint32_t i = 0; i < 32; ++i) {
        pages_a[i] = i + 1u;
        memset(page_bufs_a + (size_t)i * RC_PAGE_SIZE, (unsigned char)(i + 1u),
               RC_PAGE_SIZE);
    }

    /* Chain 2: 3000 pages, page i filled with byte i (scenario 2, LRU). */
    const char *root_b = "lazy/ram-cache-b";
    const char *db_b = "lazy-ram-cache-db-b";
    const char *commit_b = "rc-3000-0001";
    uint32_t *pages_b = (uint32_t *)malloc(3000 * sizeof(uint32_t));
    unsigned char *page_bufs_b = (unsigned char *)malloc(3000 * RC_PAGE_SIZE);
    assert(pages_b && page_bufs_b);
    for (uint32_t i = 0; i < 3000; ++i) {
        pages_b[i] = i + 1u;
        memset(page_bufs_b + (size_t)i * RC_PAGE_SIZE, (unsigned char)(i + 1u),
               RC_PAGE_SIZE);
    }

    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *store = rc_store_create(inner);
    assert(store != NULL);

    otsardb_segment_frame_info frames_a;
    memset(&frames_a, 0, sizeof(frames_a));
    craft_chain(store, root_a, db_a, commit_a, pages_a, 32, page_bufs_a, &frames_a);
    otsardb_segment_frame_info frames_b;
    memset(&frames_b, 0, sizeof(frames_b));
    craft_chain(store, root_b, db_b, commit_b, pages_b, 3000, page_bufs_b,
                &frames_b);

    scenario_1_ntoone(store, root_a, db_a, commit_a, pages_a, page_bufs_a);
    scenario_2_lru(store, root_b, db_b, commit_b, pages_b, page_bufs_b);
    scenario_3_disabled(store, root_a, db_a, commit_a, pages_a, page_bufs_a);
    scenario_4_stats(store, root_a, db_a, commit_a, pages_a, page_bufs_a);
    scenario_5_prefetch(store, root_a, db_a, commit_a, pages_a, page_bufs_a);
    scenario_6_mark_invalidate(store, root_a, db_a, commit_a, pages_a,
                               page_bufs_a);
    scenario_7_whole_object(store, root_a, db_a, commit_a, pages_a, page_bufs_a);

    printf("lazy ram cache test: all assertions passed\n");
    rc_store_destroy(store);
    otsardb_test_memory_store_destroy(inner);
    free(page_bufs_a);
    free(pages_b);
    free(page_bufs_b);
    cleanup_files();
    return 0;
}

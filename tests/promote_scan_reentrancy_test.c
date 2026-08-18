#include "journal.h"
#include "memory_store.h"
#include "object_store.h"
#include "replica.h"
#include "sqlite_wal.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <limits.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#define otsardb_unlink unlink
#endif

/* deterministic regression test: the promotion safe-scan
 * must NOT re-enter the store from inside the list callback.
 *
 * The defect ( OBSERVED 3/3 on real S3 stores):
 * replica_safe_scan_cb (src/storage/replica.c) called otsardb_store_get from
 * inside the list callback, while the S3 adapter (s3_object_store.cpp)
 * holds the process-wide 0064 request mutex ACROSS the callback â€” a
 * deterministic self-deadlock of OTSARDB_S3_PROMOTE=1. The 0091/0077 suites
 * never caught it because memory_store has no request mutex.
 *
 * This test wraps memory_store in a GUARD store whose GET detects calls
 * made while a listing is active (the exact re-entrancy class, whatever
 * the locking strategy of the real store) and drives otsardb_replica_promote
 * over the guarded destination:
 * - the promotion pick scans the destination's <root>/replica/ listing
 * (one COMMITTED ledger entry from the genesis dual publish);
 * - pre-0103 code: the GET happens INSIDE the list callback ->
 * guard->reentrant_get = 1 -> assertion FAILS;
 * - post-0103 code: the keys are buffered in the callback and the GETs
 * run after otsardb_store_list returns -> guard->reentrant_get stays 0
 * AND the ledger is still read (the pick still works, epoch raised,
 * full recovery materialises the image).
 *
 * Exit 0 = passed; nonzero = failed (also fails on pre-0103 code).
 */

#define TEST_PAGE_SIZE 4096u

static const char *WAL_PATH = "otsardb-promote-scan.wal";
static const char *RECOVERED_DB = "otsardb-promote-scan-recovered.db";
static const char *ROOT = "promote-scan-test/db";
static const char *DATABASE_ID = "promote-scan-db";

/* ---- guard store: detects GET-while-listing re-entrancy ------------------ */

typedef struct guard_store {
    otsardb_object_store *inner;
    int listing_active;   /* 1 while a list callback is in flight */
    int reentrant_get;    /* set when a GET arrives while listing active */
    long list_calls;
    long get_calls;
    long get_calls_while_listed;
} guard_store;

static otsardb_store_result guard_get(otsardb_object_store *store,
                                    const char *key,
                                    otsardb_store_object *out) {
    guard_store *guard = (guard_store *)store->context;
    ++guard->get_calls;
    if (guard->listing_active) {
        guard->reentrant_get = 1;
        ++guard->get_calls_while_listed;
    }
    return otsardb_store_get(guard->inner, key, out);
}

static otsardb_store_result guard_get_range(otsardb_object_store *store,
                                          const char *key,
                                          size_t offset,
                                          size_t length,
                                          otsardb_store_object *out) {
    guard_store *guard = (guard_store *)store->context;
    return otsardb_store_get_range(guard->inner, key, offset, length, out);
}

static otsardb_store_result guard_put(otsardb_object_store *store,
                                    const char *key,
                                    const void *data,
                                    size_t size,
                                    const otsardb_store_put_options *options,
                                    char **out_etag) {
    guard_store *guard = (guard_store *)store->context;
    return otsardb_store_put(guard->inner, key, data, size, options, out_etag);
}

static otsardb_store_result guard_put_stream(otsardb_object_store *store,
                                           const char *key,
                                           otsardb_store_source_read source_read,
                                           void *source_ctx,
                                           uint64_t expected_size,
                                           const otsardb_store_put_options *options,
                                           char **out_etag) {
    guard_store *guard = (guard_store *)store->context;
    return otsardb_store_put_stream(guard->inner, key, source_read, source_ctx,
                                  expected_size, options, out_etag);
}

static void guard_release_object(otsardb_object_store *store,
                                 otsardb_store_object *object) {
    guard_store *guard = (guard_store *)store->context;
    otsardb_store_release_object(guard->inner, object);
}

static otsardb_store_result guard_delete_object(otsardb_object_store *store,
                                              const char *key) {
    guard_store *guard = (guard_store *)store->context;
    return otsardb_store_delete(guard->inner, key);
}

static otsardb_store_result guard_list(otsardb_object_store *store,
                                     const char *prefix,
                                     otsardb_store_list_callback callback,
                                     void *ctx) {
    guard_store *guard = (guard_store *)store->context;
    ++guard->list_calls;
    guard->listing_active = 1;
    otsardb_store_result rc = otsardb_store_list(guard->inner, prefix, callback, ctx);
    guard->listing_active = 0;
    return rc;
}

static const otsardb_object_store_ops guard_ops = {
    guard_get,
    guard_get_range,
    guard_put,
    guard_put_stream,
    guard_release_object,
    guard_delete_object,
    guard_list,
};

static otsardb_object_store *guard_store_create(otsardb_object_store *inner) {
    guard_store *guard = (guard_store *)calloc(1, sizeof(*guard));
    assert(guard != NULL);
    guard->inner = inner;
    otsardb_object_store *store = (otsardb_object_store *)calloc(1, sizeof(*store));
    assert(store != NULL);
    store->ops = &guard_ops;
    store->context = guard;
    store->capabilities = inner->capabilities;
    return store;
}

static void guard_store_destroy(otsardb_object_store *store) {
    free(store->context);
    free(store);
}

/* ---- synthetic WAL (the 0065/0072/0091 harness pattern) ------------------ */

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void wal_checksum(const unsigned char *data, size_t size,
                         uint32_t *s0, uint32_t *s1) {
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint32_t w0 = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                      ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
        uint32_t w1 = (uint32_t)data[i + 4] | ((uint32_t)data[i + 5] << 8) |
                      ((uint32_t)data[i + 6] << 16) | ((uint32_t)data[i + 7] << 24);
        *s0 += w0 + *s1;
        *s1 += w1 + *s0;
    }
}

static void build_wal_frame(unsigned char *frame, uint32_t page_no,
                            uint32_t n_truncate, const unsigned char *page,
                            uint32_t page_size, uint32_t salt1, uint32_t salt2,
                            uint32_t *s0, uint32_t *s1) {
    memset(frame, 0, OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + page_size);
    put_be32(frame, page_no);
    put_be32(frame + 4, n_truncate);
    put_be32(frame + 8, salt1);
    put_be32(frame + 12, salt2);
    memcpy(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE, page, page_size);
    wal_checksum(frame, 8, s0, s1);
    wal_checksum(frame + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE, page_size, s0, s1);
    put_be32(frame + 16, *s0);
    put_be32(frame + 20, *s1);
}

/* One WAL file, one commit of 3 frames (n_truncate 3 = a 3-page database). */
static size_t build_synthetic_wal(void) {
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + TEST_PAGE_SIZE;
    size_t full_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + 3u * frame_size;
    unsigned char *wal = (unsigned char *)malloc(full_size);
    assert(wal != NULL);
    memset(wal, 0, OTSARDB_SQLITE_WAL_HEADER_SIZE);
    put_be32(wal, 0x377f0682u);
    put_be32(wal + 4, 3007000u);
    put_be32(wal + 8, TEST_PAGE_SIZE);
    put_be32(wal + 12, 1u);
    put_be32(wal + 16, 0x11111111u);
    put_be32(wal + 20, 0x22222222u);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    put_be32(wal + 24, s0);
    put_be32(wal + 28, s1);
    s0 = 0;
    s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);

    unsigned char *page = (unsigned char *)malloc(TEST_PAGE_SIZE);
    assert(page != NULL);
    for (size_t i = 0; i < 3u; ++i) {
        uint32_t page_no = (uint32_t)i + 1u;
        uint32_t n_truncate = i == 2u ? 3u : 0u;
        memset(page, (int)(page_no * 7u), TEST_PAGE_SIZE);
        build_wal_frame(wal + OTSARDB_SQLITE_WAL_HEADER_SIZE + i * frame_size,
                        page_no, n_truncate, page, TEST_PAGE_SIZE,
                        0x11111111u, 0x22222222u, &s0, &s1);
    }
    free(page);

    FILE *f = fopen(WAL_PATH, "wb");
    assert(f != NULL);
    assert(fwrite(wal, 1, full_size, f) == full_size);
    assert(fclose(f) == 0);
    free(wal);
    return full_size;
}

typedef struct wal_file_reader {
    FILE *file;
} wal_file_reader;

static int wal_file_read_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
    wal_file_reader *reader = (wal_file_reader *)ctx;
    if (!reader || !reader->file || offset > (uint64_t)LONG_MAX) return 0;
    if (fseek(reader->file, (long)offset, SEEK_SET) != 0) return 0;
    return fread(buffer, 1, size, reader->file) == size;
}

static int drive_sync(size_t wal_size, otsardb_wal_publisher *publisher) {
    wal_file_reader reader = {NULL};
    reader.file = fopen(WAL_PATH, "rb");
    assert(reader.file != NULL);
    int rc = otsardb_wal_publisher_sync_reader(WAL_PATH,
                                             (uint64_t)wal_size,
                                             wal_file_read_at,
                                             &reader,
                                             NULL,
                                             NULL,
                                             publisher);
    fclose(reader.file);
    return rc;
}

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void assert_epoch_val(otsardb_object_store *store,
                             const char *root,
                             uint64_t expected) {
    otsardb_replica_epoch_view view;
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_epoch_read(store, root, &view, &rc));
    assert(rc == OTSARDB_JOURNAL_OK && view.exists && view.epoch == expected);
}

int main(void) {
    set_env("OTSARDB_PUBLISH_RETRIES", "3");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");
    set_env("OTSARDB_LEASE_TTL", "30");
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(RECOVERED_DB);

    otsardb_object_store *primary_inner = otsardb_test_memory_store_create();
    assert(primary_inner != NULL);
    otsardb_object_store *dest_inner = otsardb_test_memory_store_create();
    assert(dest_inner != NULL);
    otsardb_object_store *dest = guard_store_create(dest_inner);
    assert(dest != NULL);

    /* Genesis dual publish: the destination gets the full chain + the
     * PREPARED/COMMITTED ledger + the epoch record THROUGH the guard. */
    otsardb_wal_publisher publisher;
    memset(&publisher, 0, sizeof(publisher));
    assert(otsardb_wal_publisher_init(&publisher, primary_inner, ROOT,
                                    DATABASE_ID, OTSARDB_WRITE_STRATEGY_DEFAULT));
    assert(otsardb_replica_attach(&publisher, dest, OTSARDB_WRITE_STRATEGY_CAS, 1));
    assert(otsardb_replica_destination_count(publisher.replica) == 1);
    size_t wal_size = build_synthetic_wal();
    assert(drive_sync(wal_size, &publisher) == 0);
    assert_epoch_val(dest_inner, ROOT, 1);

    /* Baseline: no GET must ever have been observed inside a list during
     * the genesis publish either. */
    guard_store *guard = (guard_store *)dest->context;
    assert(guard->reentrant_get == 0);

    /* The promotion pick: list <root>/replica/ + GET each ledger. On the
     * pre-0103 code the GETs run INSIDE the list callback and trip the
     * guard; on the fixed code the GETs run after the list returns. */
    char err[512];
    err[0] = '\0';
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    assert(otsardb_replica_promote(publisher.replica, primary_inner,
                                 RECOVERED_DB, 30, err, sizeof(err), &rc));
    assert(rc == OTSARDB_JOURNAL_OK);
    assert(otsardb_replica_promoted_index(publisher.replica) == 0);
    assert(otsardb_replica_epoch(publisher.replica) == 2);
    assert_epoch_val(dest_inner, ROOT, 2);

    /* THE regression assertion: no store GET happened while a list was
     * active. Fails on pre-0103 replica.c (replica_safe_scan_cb). */
    assert(guard->reentrant_get == 0);
    /* The scan really ran (a list happened) and really read the ledgers
     * (GETs happened AFTER the list) â€” the fix must not skip the work. */
    assert(guard->list_calls >= 1);
    assert(guard->get_calls_while_listed == 0);
    fprintf(stderr, "promote scan: lists=%ld gets=%ld (re-entrant=%ld) -> "
                    "buffered scan completed, no GET inside the list callback\n",
            guard->list_calls, guard->get_calls, guard->get_calls_while_listed);

    /* Full recovery from the promoted destination materialised the 3-page
     * image (genesis commit, n_truncate 3). */
    {
        FILE *f = fopen(RECOVERED_DB, "rb");
        assert(f != NULL);
        assert(fseek(f, 0, SEEK_END) == 0);
        long size = ftell(f);
        fclose(f);
        assert(size == (long)(3u * TEST_PAGE_SIZE));
    }

    otsardb_wal_publisher_destroy(&publisher);
    guard_store_destroy(dest);
    otsardb_test_memory_store_destroy(dest_inner);
    otsardb_test_memory_store_destroy(primary_inner);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(RECOVERED_DB);
    printf("promote safe-scan re-entrancy regression passed\n");
    return 0;
}

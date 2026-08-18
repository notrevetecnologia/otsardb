/* tests/lazy_checkpoint_base_complete_test.c — regression test for
 * , audit finding LH-2 (SUSPECTED -> CONFIRMED): 
 * `otsardb_lazy_is_complete` counted only marked bitmap bits, but pages
 * covered by a checkpoint base are deliberately NOT pre-marked. With a
 * checkpoint base the image was never reported complete until every page
 * was touched, so the close-time cache save silently never happened and the
 * next open paid a full recovery.
 *
 * Scenario (real SQLite chain, memory store):
 *
 * 1. build a database and publish commits (gen 1);
 * 2. checkpoint the head (covers every page of gen 1);
 * 3. publish one more commit that modifies rows IN PLACE (gen 2 — touches
 * a few pages, no database growth), so database_size_pages == the
 * checkpoint's covered_size_pages and every page is either
 * base-covered-unclaimed (base bytes are newest) or claimed by the
 * gen-2 segment (must be hydrated);
 * 4. lazy-restore the head: the base image materialises pages 1..covered
 * without marking them;
 * 5. assert the image is NOT yet complete (the claimed gen-2 pages are
 * unmarked — true before AND after the fix);
 * 6. hydrate EXACTLY the pages the gen-2 segment claims (read from the
 * head manifest), then assert `otsardb_lazy_is_complete`:
 * - pre-0144: FALSE — the base-covered-unclaimed pages are never
 * counted, so the image only ever completes after every page is
 * touched;
 * - post-0144: TRUE — marked (claimed) + base_complete (covered &
 * unclaimed) == database_size_pages, and the bytes are newest for
 * every page.
 * 7. byte-compare the hydrated image against a full recovery — the
 * completeness gate never accepts a partial/wrong image.
 *
 * Deterministic: no network, no sleeps, no timing dependence (the gen-2
 * segment's page set is read straight from the store's head manifest).
 *
 * Wiring (coordinator — , NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_lazy_checkpoint_base_complete_test
 * tests/lazy_checkpoint_base_complete_test.c
 * tests/memory_store.c)
 * target_include_directories(otsardb_lazy_checkpoint_base_complete_test
 * PRIVATE include src/core src/storage tests)
 * target_link_libraries(otsardb_lazy_checkpoint_base_complete_test PRIVATE otsardb_core)
 * add_test(NAME otsardb_lazy_checkpoint_base_complete
 * COMMAND otsardb_lazy_checkpoint_base_complete_test)
 * set_tests_properties(otsardb_lazy_checkpoint_base_complete PROPERTIES TIMEOUT 120)
 */

#include "otsardb.h"
#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "sqlite3.h"
#include "wal_batch.h"
#include "wal_publisher.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#define otsardb_unlink unlink
#endif

#define CC_SOURCE "lazy-cp-complete-source.db"
#define CC_WAL "lazy-cp-complete-source.db-wal"
#define CC_SHM "lazy-cp-complete-source.db-shm"
#define CC_FULL "lazy-cp-complete-full.db"
#define CC_LAZY "lazy-cp-complete-lazy.db"
#define CC_ROOT "lazy/cp-complete"
#define CC_DB_ID "lazy-cp-complete-db"

static void cleanup_files(void) {
    otsardb_unlink(CC_SOURCE);
    otsardb_unlink(CC_WAL);
    otsardb_unlink(CC_SHM);
    otsardb_unlink(CC_FULL);
    otsardb_unlink(CC_LAZY);
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

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static void publish_commit(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *sql) {
    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    root,
                                    db_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb_head head;
    char *etag = NULL;
    if (otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK) {
        free(etag);
        assert(otsardb_recovery_restore_store(store, root, CC_SOURCE));
    } else {
        free(etag);
    }

    otsardb *db = NULL;
    assert(otsardb_open(CC_SOURCE, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    int exec_rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (exec_rc != OTSARDB_OK) {
        fprintf(stderr, "publish_commit exec failed: %s\n",
                error_message ? error_message : "(no message)");
    }
    assert(exec_rc == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_unlink(CC_SOURCE);
    otsardb_unlink(CC_WAL);
    otsardb_unlink(CC_SHM);
}

/* Collect every page claimed by the segments of the head manifest into
 * `out` (a bitmap over pages 1..db_size). Returns the number of distinct
 * claimed pages. */
static uint32_t collect_claimed_pages(otsardb_object_store *store,
                                      const otsardb_head *head,
                                      uint64_t db_size,
                                      unsigned char *out) {
    otsardb_store_object manifest_obj = {0};
    assert(otsardb_store_get(store, head->commit_key, &manifest_obj) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_obj.data,
                                             manifest_obj.size,
                                             &manifest));
    uint32_t claimed = 0;
    for (uint32_t s = 0; s < manifest.segment_count; ++s) {
        const otsardb_manifest_segment_ref *ref = &manifest.segments[s];
        for (uint32_t p = 0; p < ref->page_count; ++p) {
            uint32_t page = ref->pages[p];
            if (page == 0 || (uint64_t)page > db_size) continue;
            if (!(out[page] & 1u)) {
                out[page] = 1u;
                ++claimed;
            }
        }
    }
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_obj);
    return claimed;
}

static int read_file_bytes(const char *path, unsigned char **out, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *data = (unsigned char *)malloc((size_t)size ? (size_t)size : 1u);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

int main(void) {
    cleanup_files();
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* Gen 1: a table with enough rows to fill several pages. */
    publish_commit(store, CC_ROOT, CC_DB_ID,
                   "CREATE TABLE cc_t(id INTEGER PRIMARY KEY, v TEXT, pad TEXT);");
    {
        char insert[16384] = "INSERT INTO cc_t(v, pad) VALUES('a', 'xxxx'); ";
        for (int i = 1; i < 300; ++i) {
            char one[64];
            snprintf(one, sizeof(one), "INSERT INTO cc_t(v, pad) VALUES('a%d', 'yyyy'); ", i);
            strncat(insert, one, sizeof(insert) - strlen(insert) - 1u);
        }
        publish_commit(store, CC_ROOT, CC_DB_ID, insert);
    }

    /* Checkpoint the gen-2 head: the base covers every current page. */
    otsardb_head cp_head = read_head(store, CC_ROOT);
    assert(otsardb_recovery_restore_store(store, CC_ROOT, CC_SOURCE));
    assert(otsardb_checkpoint_build(store, CC_ROOT, CC_SOURCE));
    otsardb_unlink(CC_SOURCE);
    otsardb_unlink(CC_WAL);
    otsardb_unlink(CC_SHM);
    (void)cp_head;

    /* Gen 3: update rows in place — touches pages, does not grow the
     * database, so database_size_pages stays == the checkpoint's
     * covered_size_pages. */
    publish_commit(store, CC_ROOT, CC_DB_ID,
                   "UPDATE cc_t SET v='updated' WHERE id BETWEEN 1 AND 50;");

    otsardb_head head = read_head(store, CC_ROOT);

    /* Decode the head manifest to learn the gen-3 claimed pages. */
    otsardb_store_object head_manifest_obj = {0};
    assert(otsardb_store_get(store, head.commit_key, &head_manifest_obj) == OTSARDB_STORE_OK);
    otsardb_commit_manifest head_manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)head_manifest_obj.data,
                                             head_manifest_obj.size,
                                             &head_manifest));
    const uint64_t db_size = head_manifest.database_size_pages;
    const uint32_t page_size = head_manifest.page_size;
    assert(db_size > 0 && page_size > 0 && db_size < (uint64_t)UINT32_MAX);
    unsigned char *claimed =
        (unsigned char *)calloc((size_t)db_size + 1u, 1u);
    assert(claimed);
    const uint32_t claimed_count =
        collect_claimed_pages(store, &head, db_size, claimed);
    assert(claimed_count > 0); /* the UPDATE must have touched pages */
    otsardb_commit_manifest_free(&head_manifest);
    otsardb_store_release_object(store, &head_manifest_obj);

    /* Full recovery baseline. */
    assert(otsardb_recovery_restore_store(store, CC_ROOT, CC_FULL));

    /* Lazy restore over the checkpoint base. */
    otsardb_lazy_hydrator *h = NULL;
    assert(otsardb_lazy_restore_store(store, CC_ROOT, CC_LAZY, &head, &h));

    /* Right after restore the image is genuinely partial: the claimed gen-3
     * pages are unmaterialised (their base bytes are stale). */
    assert(!otsardb_lazy_is_complete(h));

    /* Hydrate EXACTLY the claimed pages — nothing else. */
    for (uint64_t p = 1; p <= db_size; ++p) {
        if (claimed[p]) {
            assert(otsardb_lazy_hydrate_range(h, (uint32_t)p, (uint32_t)p) == 1);
        }
    }

    /* (LH-2): with a checkpoint base, the image is complete
     * as soon as every CLAIMED page is marked — the base-covered-unclaimed
     * pages are materialised by the base image. Pre-0144 this assertion
     * failed (is_complete stayed false until every base page was touched),
     * which silently skipped the close-time cache save. */
    assert(otsardb_lazy_is_complete(h));

    /* The image the gate now accepts is byte-identical to full recovery. */
    unsigned char *lazy_bytes = NULL, *full_bytes = NULL;
    size_t lazy_size = 0, full_size = 0;
    assert(read_file_bytes(CC_LAZY, &lazy_bytes, &lazy_size));
    assert(read_file_bytes(CC_FULL, &full_bytes, &full_size));
    assert(lazy_size == full_size);
    assert(memcmp(lazy_bytes, full_bytes, lazy_size) == 0);
    free(lazy_bytes);
    free(full_bytes);

    /* A partial image must STILL fail the gate: destroy and rebuild the
     * hydrator, hydrate only ONE claimed page, and assert incomplete. */
    otsardb_lazy_destroy(h);
    otsardb_unlink(CC_LAZY);
    h = NULL;
    assert(otsardb_lazy_restore_store(store, CC_ROOT, CC_LAZY, &head, &h));
    uint32_t first_claimed = 0;
    for (uint64_t p = 1; p <= db_size; ++p) {
        if (claimed[p]) {
            first_claimed = (uint32_t)p;
            break;
        }
    }
    assert(first_claimed != 0);
    assert(otsardb_lazy_hydrate_range(h, first_claimed, first_claimed) == 1);
    /* One claimed page marked; the rest of the image is still partial. */
    assert(!otsardb_lazy_is_complete(h));

    otsardb_lazy_destroy(h);
    free(claimed);
    otsardb_test_memory_store_destroy(store);
    cleanup_files();

    printf("lazy checkpoint base completeness test: all assertions passed\n");
    return 0;
}

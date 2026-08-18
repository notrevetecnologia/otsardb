/* checkpoint_discovery_test.c ??? regression tests for 
 * 0146, failure-mode audit finding C2 (GC behind a checkpoint vs recovery
 * fallback after CHECKPOINT.json pointer loss).
 *
 * C2 (MEDIUM, PROJECTED, needs-test): with GC enabled and
 * OTSARDB_GC_KEEP_MINUTES=0 (default), history below the checkpoint's
 * covered generation is deleted immediately. If the CHECKPOINT.json POINTER
 * is then lost (S3 lifecycle policy, manual cleanup, partial restore), the
 * full-chain recovery fallback hits the swept manifests/segments and the
 * open fails (availability loss ??? fail-closed, not silent). 
 * adds a recovery-side fallback: the checkpoint manifest is content
 * addressed (key == sha256 of its JSON) and survives GC, so recovery
 * redisovers it by listing the checkpoints/ prefix, re-verifies it by its
 * own key hash and re-binds it to the chain before applying its pages.
 *
 * Discrimination: after GC + pointer deletion, recovery returns 0 on the
 * pre-0146 code (test FAILS) and succeeds with the correct row count on the
 * fixed code (test PASSES). Control cases guard the pointer-present and
 * no-checkpoint paths.
 *
 * Deterministic, no network, no sleeps: memory store, WAL publisher,
 * checkpoint build, GC sweep.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_checkpoint_discovery_test
 * tests/checkpoint_discovery_test.c tests/memory_store.c)
 * target_include_directories(otsardb_checkpoint_discovery_test
 * PRIVATE include src/core src/crypto src/storage src/ha tests)
 * target_link_libraries(otsardb_checkpoint_discovery_test
 * PRIVATE otsardb_core)
 * add_test(NAME otsardb_checkpoint_discovery
 * COMMAND otsardb_checkpoint_discovery_test)
 * set_tests_properties(otsardb_checkpoint_discovery
 * PROPERTIES TIMEOUT 600)
 */
#include "otsardb.h"
#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "gc.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "s3_database.h"
#include "wal_publisher.h"

#include <assert.h>
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

#define C2_ROOT "146/c2"
#define C2_DB_ID "0146-c2-db"

static const char *SOURCE = "otsardb-0146-c2-source.db";
static const char *SOURCE_WAL = "otsardb-0146-c2-source.db-wal";
static const char *SOURCE_SHM = "otsardb-0146-c2-source.db-shm";
static const char *REF = "otsardb-0146-c2-ref.db";
static const char *FINAL = "otsardb-0146-c2-final.db";

static int collect_count(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    int *out = (int *)ctx;
    *out = atoi(argv[0]);
    return 0;
}

static void publish(otsardb_object_store *store, const char *sql) {
    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    C2_ROOT,
                                    C2_DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);

    otsardb_head head;
    char *etag = NULL;
    if (otsardb_journal_read_head(store, C2_ROOT, &head, &etag) == OTSARDB_JOURNAL_OK) {
        free(etag);
        assert(otsardb_recovery_restore_store(store, C2_ROOT, SOURCE));
    } else {
        free(etag);
    }

    otsardb *db = NULL;
    if (otsardb_open(SOURCE, &db) != OTSARDB_OK) {
        fprintf(stderr, "publish: otsardb_open(%s) failed: %s\n", SOURCE,
                otsardb_s3_last_error());
        assert(0);
    }
    char *error_message = NULL;
    assert(otsardb_exec(db,
                       "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;",
                       NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_unlink(SOURCE);
    otsardb_unlink(SOURCE_WAL);
    otsardb_unlink(SOURCE_SHM);
}

static otsardb_object_store *build_store(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);
    publish(store, "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);");
    publish(store, "INSERT INTO items(id, value) "
                   "WITH RECURSIVE cnt(x) AS (VALUES(1) UNION ALL "
                   "SELECT x+1 FROM cnt WHERE x<100) "
                   "SELECT x, printf('a-%d', x) FROM cnt;");
    publish(store, "INSERT INTO items(id, value) "
                   "WITH RECURSIVE cnt(x) AS (VALUES(101) UNION ALL "
                   "SELECT x+1 FROM cnt WHERE x<200) "
                   "SELECT x, printf('b-%d', x) FROM cnt;");
    return store;
}

static int object_exists(otsardb_object_store *store, const char *key) {
    otsardb_store_object obj = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &obj);
    if (rc == OTSARDB_STORE_OK) otsardb_store_release_object(store, &obj);
    return rc == OTSARDB_STORE_OK;
}

static void delete_object(otsardb_object_store *store, const char *key) {
    assert(otsardb_store_delete(store, key) == OTSARDB_STORE_OK);
}

static int recovered_count(otsardb_object_store *store, const char *db_path) {
    char wal_path[256];
    char shm_path[256];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
    otsardb_unlink(db_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    assert(otsardb_recovery_restore_store(store, C2_ROOT, db_path));
    otsardb *db = NULL;
    assert(otsardb_open(db_path, &db) == OTSARDB_OK);
    int count = 0;
    char *error_message = NULL;
    assert(otsardb_exec(db, "SELECT count(*) FROM items;", collect_count, &count,
                        &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    return count;
}

/* C2 hazard: GC sweeps history below the checkpoint; losing the
 * CHECKPOINT.json pointer then breaks the full-chain fallback. 
 * 0146 restores availability by discovering + re-binding the surviving
 * checkpoint manifest. */
static void test_pointer_loss_discovery_recovery(void) {
    otsardb_object_store *store = build_store(); /* head generation 3 */

    /* Reference: full-chain recovery before any checkpoint/GC. */
    assert(recovered_count(store, REF) == 200);

    /* Build a checkpoint at generation 3. */
    otsardb_unlink(FINAL);
    assert(otsardb_checkpoint_build(store, C2_ROOT, FINAL) == 1);

    /* Advance the head past the checkpoint (generation 4) so the
     * checkpoint-accelerated path must replay above-covered segments. */
    publish(store, "INSERT INTO items(id, value) "
                   "WITH RECURSIVE cnt(x) AS (VALUES(201) UNION ALL "
                   "SELECT x+1 FROM cnt WHERE x<250) "
                   "SELECT x, printf('c-%d', x) FROM cnt;");

    /* GC with keep_secs = 0 (OTSARDB_GC_KEEP_MINUTES default): sweep the
     * history strictly below the covered generation. */
    otsardb_gc_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.keep_secs = 0;
    assert(otsardb_gc_sweep(store, C2_ROOT, &opts) > 0);

    /* The pointer exists now; the swept manifests are gone. */
    otsardb_checkpoint_pointer ptr;
    char *ptr_etag = NULL;
    assert(otsardb_checkpoint_ptr_read(store, C2_ROOT, &ptr, &ptr_etag) ==
           OTSARDB_CHECKPOINT_PTR_OK);
    free(ptr_etag);
    assert(ptr.covered_generation == 3);

    /* The checkpoint manifest itself survives GC (content-addressed). */
    assert(object_exists(store, ptr.checkpoint_key));

    /* LOSE the pointer (S3 lifecycle / manual cleanup / partial restore). */
    delete_object(store, C2_ROOT "/CHECKPOINT.json");

    /* Recovery must now succeed via discovery: the surviving checkpoint is
     * found under checkpoints/, self-verified, chain-bound to generation 3,
     * and generation 4 is replayed on top. */
    assert(recovered_count(store, FINAL) == 250);

    otsardb_unlink(REF);
    otsardb_unlink(FINAL);
    otsardb_test_memory_store_destroy(store);
    puts("passed");
}

/* Control: pointer present ??? the existing checkpoint path still works after
 * GC (no regression). */
static void test_pointer_present_recovery(void) {
    otsardb_object_store *store = build_store();
    otsardb_unlink(FINAL);
    assert(otsardb_checkpoint_build(store, C2_ROOT, FINAL) == 1);
    publish(store, "INSERT INTO items(id, value) VALUES(999, 'extra');");

    otsardb_gc_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.keep_secs = 0;
    assert(otsardb_gc_sweep(store, C2_ROOT, &opts) > 0);

    assert(recovered_count(store, FINAL) == 201);

    otsardb_unlink(FINAL);
    otsardb_test_memory_store_destroy(store);
    puts("passed");
}

/* Control: no checkpoint ever built ??? the full-chain fallback still works
 * (no regression, discovery must not interfere). */
static void test_no_checkpoint_full_chain(void) {
    otsardb_object_store *store = build_store();
    assert(recovered_count(store, FINAL) == 200);
    otsardb_unlink(FINAL);
    otsardb_test_memory_store_destroy(store);
    puts("passed");
}

int main(void) {
    otsardb_unlink(SOURCE);
    otsardb_unlink(SOURCE_WAL);
    otsardb_unlink(SOURCE_SHM);
    assert(otsardb_initialize() == OTSARDB_OK);

    test_no_checkpoint_full_chain();
    test_pointer_present_recovery();
    test_pointer_loss_discovery_recovery();

    puts("checkpoint pointer-loss discovery recovery passed (C2)");
    return 0;
}

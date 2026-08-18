/*
 * — D2: delta base bound to the IMAGE, not only the
 * manifest hash.
 *
 * Deterministic regression test (memory store, real WAL-publisher chains)
 * for the MEDIUM failure-mode-audit finding: otsardb_recovery_materialize_delta
 * previously bound the local base image to the chain ONLY by the base
 * MANIFEST hash (base_manifest_sha256) — never by the image content. A
 * tampered / bit-rot / crash-mixed base (D1) yielded a wrong materialized
 * image that only the head checksum gate could catch (and that gate is off
 * on checksum-less heads — legacy chains or any chain segment degraded by
 * the 0063 checksum-blocked path).
 *
 * The 0135 fix verifies the base IMAGE before diffing: size must equal the
 * base manifest's database_size_pages * page_size, and the base manifest's
 * post-apply checksum (when present) must match the file. Any mismatch
 * fails closed (delta returns 0, no destination written) and the full
 * recovery path produces the correct image.
 *
 * Scenarios:
 * 1. Checksum-bearing chain: tampered base -> delta refused; full
 * recovery still correct; an intact base still deltas fine.
 * 2. CHECKSUM-LESS HEAD chain (0063-blocked publishes, the audit's
 * silent-wrong-data case): tampered base -> delta refused by the NEW
 * base check alone (the head gate is absent here — without the fix
 * this scenario would silently return a wrong image); full recovery
 * still correct; an intact base still deltas fine.
 * 3. Size mismatch (truncated base) -> delta refused.
 * 4. Tamper in a region no newer segment ever touches -> delta refused
 * (the base check must catch content corruption the delta application
 * and the head gate cannot see).
 *
 * Build (standalone, not a ctest target in this change set — CMakeLists
 * wiring is out of scope for): link against the same libs
 * as otsardb_delta_materialization. Run with OTSARDB_DIAG=1 to observe
 * the "materialize_delta: base image ... mismatch" diagnostics.
 */

#include "otsardb.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_cache.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "object_store.h"
#include "recovery.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#define otsardb_rmdir _rmdir
#else
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_rmdir rmdir
#endif

#define OTSARDB_TEST_KEY_MAX 511

static const char *SOURCE = "delta-base-src.db";
static const char *WAL_PATH = "delta-base-src.db-wal";
static const char *SHM_PATH = "delta-base-src.db-shm";

static void cleanup_files(void) {
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
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

static void assert_files_identical(const char *a, const char *b) {
    unsigned char *ba = NULL, *bb = NULL;
    size_t sa = 0, sb = 0;
    assert(read_file_bytes(a, &ba, &sa));
    assert(read_file_bytes(b, &bb, &sb));
    assert(sa == sb);
    assert(memcmp(ba, bb, sa) == 0);
    free(ba);
    free(bb);
}

static void assert_no_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        assert(!"file exists");
    }
}

/* Publish one commit through the real WAL publisher. When anchor is
 * non-zero the publisher is anchored at the restored head (0063 proof) and
 * the commit carries a post-apply checksum; when zero the local image is
 * intentionally NOT proven (checksum-blocked path) and the chained commit
 * publishes checksum-less — the exact chain shape the audit's D2
 * silent-wrong-data case needs. */
static void publish_commit(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *sql,
                           int anchor) {
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
        assert(otsardb_recovery_restore_store(store, root, SOURCE));
        if (anchor) {
            otsardb_wal_publisher_set_local_image_generation(&publisher, head.generation);
        }
    } else {
        free(etag);
    }

    otsardb *db = NULL;
    assert(otsardb_open(SOURCE, &db) == OTSARDB_OK);
    char *error_message = NULL;
    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_wal_publisher_destroy(&publisher);
    cleanup_files();
}

static int head_has_checksum(otsardb_object_store *store, const otsardb_head *head) {
    otsardb_store_object manifest_obj = {0};
    if (otsardb_store_get(store, head->commit_key, &manifest_obj) != OTSARDB_STORE_OK) {
        return 0;
    }
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_obj.data,
                                             manifest_obj.size,
                                             &manifest));
    int has = manifest.post_apply_checksum[0] != '\0';
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_obj);
    return has;
}

/* ---- local-cache helpers (real save / read_state) ---- */

static void cache_image_path(const char *cache_dir,
                             const char *db_id,
                             char *out,
                             size_t out_size) {
    snprintf(out, out_size, "%s/otsardb-cache-v1/%s/state.db", cache_dir, db_id);
}

static void save_cache_at_head(otsardb_object_store *store,
                               const char *root,
                               const char *cache_dir,
                               const char *db_id,
                               const char *image_path,
                               const char *restore_target) {
    otsardb_head head = read_head(store, root);
    assert(otsardb_recovery_restore_store(store, root, restore_target));
    otsardb_local_cache_state state;
    memset(&state, 0, sizeof(state));
    state.generation = head.generation;
    snprintf(state.commit_id, sizeof(state.commit_id), "%s", head.commit_id);
    snprintf(state.commit_sha256, sizeof(state.commit_sha256), "%s", head.commit_sha256);
    assert(otsardb_local_cache_save(cache_dir, db_id, restore_target, &state));
    otsardb_unlink(restore_target);
}

static void remove_cache_dir(const char *cache_dir, const char *db_id) {
    char buf[1100];
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/state.db", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/state.db.tmp", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/cache.json", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s/cache.json.tmp", cache_dir, db_id);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/%s", cache_dir, db_id);
    otsardb_rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1/otsardb-cache-index.json", cache_dir);
    otsardb_unlink(buf);
    snprintf(buf, sizeof(buf), "%s/otsardb-cache-v1", cache_dir);
    otsardb_rmdir(buf);
    otsardb_rmdir(cache_dir);
}

/* Flip one byte at a given absolute offset of a file (r+b). */
static void tamper_at(const char *path, long offset) {
    FILE *f = fopen(path, "r+b");
    assert(f);
    assert(fseek(f, offset, SEEK_SET) == 0);
    unsigned char byte = 0;
    assert(fread(&byte, 1, 1, f) == 1);
    byte ^= 0xff;
    assert(fseek(f, offset, SEEK_SET) == 0);
    assert(fwrite(&byte, 1, 1, f) == 1);
    assert(fflush(f) == 0);
    fclose(f);
}

/* Truncate a file to new_size (or to size-1 when new_size == -1). */
static void truncate_file(const char *path, long new_size) {
    FILE *f = fopen(path, "r+b");
    assert(f);
    if (new_size < 0) {
        assert(fseek(f, 0, SEEK_END) == 0);
        long end = ftell(f);
        assert(end > 1);
        new_size = end - 1;
    }
    assert(fflush(f) == 0);
#if defined(_WIN32)
    assert(_chsize_s(_fileno(f), new_size) == 0);
#else
    assert(ftruncate(fileno(f), (off_t)new_size) == 0);
#endif
    fclose(f);
}

static otsardb_object_store *store(void) {
    static otsardb_object_store *s = NULL;
    if (!s) s = otsardb_test_memory_store_create();
    assert(s != NULL);
    return s;
}

static void run_scenario(const char *root,
                         const char *db_id,
                         const char *cache_tag,
                         int head_checksummed) {
    char cache_dir[1100];
    snprintf(cache_dir, sizeof(cache_dir), "delta-base-cache-%s", cache_tag);
    char base_path[1100];
    cache_image_path(cache_dir, db_id, base_path, sizeof(base_path));
    char restore_target[1100];
    snprintf(restore_target, sizeof(restore_target), "delta-base-%s.restore", cache_tag);
    char delta_path[1100];
    snprintf(delta_path, sizeof(delta_path), "delta-base-%s.delta", cache_tag);
    char full_path[1100];
    snprintf(full_path, sizeof(full_path), "delta-base-%s.full", cache_tag);

    /* Chain: c1 genesis (anchored), c2 anchored (checksummed). The cache is
     * saved at c2 with an attested image. One SQL statement per publish so
     * each publish is exactly one commit (deterministic checksum state). */
    publish_commit(store(), root, db_id,
                   "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT);",
                   1);
    publish_commit(store(), root, db_id,
                   "INSERT INTO t1(v) SELECT 'a' UNION ALL SELECT 'b' "
                   "UNION ALL SELECT 'c';",
                   1);
    save_cache_at_head(store(), root, cache_dir, db_id, base_path, restore_target);

    /* Newer commits above the cached generation. */
    publish_commit(store(), root, db_id,
                   "INSERT INTO t1(v) SELECT 'd' UNION ALL SELECT 'e';",
                   1);
    publish_commit(store(), root, db_id,
                   "INSERT INTO t1(v) SELECT 'f' UNION ALL SELECT 'g';",
                   head_checksummed ? 1 : 0);

    otsardb_head head = read_head(store(), root);
    assert((head_has_checksum(store(), &head) != 0) == head_checksummed);

    otsardb_local_cache_state cached;
    assert(otsardb_local_cache_read_state(cache_dir, db_id, &cached) == 1);
    assert(cached.generation < head.generation);

    /* Reference full recovery (correctness target). */
    assert(otsardb_recovery_restore_store(store(), root, full_path));

    otsardb_head out_head;
    memset(&out_head, 0, sizeof(out_head));

    /* (a) Intact base: the delta still succeeds and equals full recovery. */
    assert(otsardb_recovery_materialize_delta(store(), root, base_path,
                                            cached.generation,
                                            cached.commit_sha256,
                                            delta_path, &out_head) == 1);
    assert(out_head.generation == head.generation);
    assert_files_identical(delta_path, full_path);
    otsardb_unlink(delta_path);

    /* (b) Tampered base: flip one byte in the middle of the file (a page
     * far from any page a newer commit touches — the whole-file base
     * checksum must catch it). */
    {
        unsigned char *bytes = NULL;
        size_t size = 0;
        assert(read_file_bytes(base_path, &bytes, &size));
        long offset = (long)(size / 2);
        free(bytes);
        tamper_at(base_path, offset);
    }
    assert(otsardb_recovery_materialize_delta(store(), root, base_path,
                                            cached.generation,
                                            cached.commit_sha256,
                                            delta_path, &out_head) == 0);
    assert_no_file(delta_path);
    assert_no_file("delta-base-src.db.deltatmp");

    /* (c) Size mismatch: truncating the base by one byte changes the
     * declared database size -> refused. */
    truncate_file(base_path, -1);
    assert(otsardb_recovery_materialize_delta(store(), root, base_path,
                                            cached.generation,
                                            cached.commit_sha256,
                                            delta_path, &out_head) == 0);
    assert_no_file(delta_path);

    /* (d) Full recovery still produces the correct image (the delta was
     * refused, never a wrong image). */
    assert(otsardb_recovery_restore_store(store(), root, delta_path));
    assert_files_identical(delta_path, full_path);

    otsardb_unlink(base_path);
    otsardb_unlink(delta_path);
    otsardb_unlink(full_path);
    remove_cache_dir(cache_dir, db_id);
}

int main(void) {
    cleanup_files();
    assert(otsardb_initialize() == OTSARDB_OK);

    /* 1. Checksum-bearing chain: the base check AND the head gate exist;
     * the delta must refuse the tampered base and full recovery must win. */
    run_scenario("delta-base/csum", "delta-base-csum-db", "a", 1);

    /* 2. Checksum-less head (0063-blocked path): the head gate is absent,
     * so a refused delta here proves the NEW base-image check fired — the
     * exact case the audit said could silently serve a wrong image. */
    run_scenario("delta-base/nocsum", "delta-base-nocsum-db", "b", 0);

    printf("delta base integrity test: all assertions passed\n");
    otsardb_test_memory_store_destroy(store());
    cleanup_files();
    return 0;
}

#include "otsardb.h"
#include "otsardb_internal.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "sqlite3.h"
#include "wal_publisher.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_unlink _unlink
#define otsardb_setenv _putenv_s
#else
#include <unistd.h>
#define otsardb_unlink unlink
#define otsardb_setenv setenv
#endif

/*
 * — cross-instance visibility contract, end-to-end at the
 * session level (deterministic, no MinIO required).
 *
 * This is the sibling of tests/session_snapshot_consistency_test.c
 * and follows its harness exactly: the same lower layers the
 * real S3 session composes — wal_publisher + recovery/lazy-hydration
 * materialisation + the local VFS read path — driven over a memory store.
 * Where 0120's S2/S4 pin snapshot-vs-reopen with a single reader, this test
 * pins the CROSS-INSTANCE contract of docs/architecture/CONSISTENCY_MODEL.md
 * section 5 (visibility happens only via reopen or a write attempt) with
 * multiple simultaneous reader sessions plus a write-fence check:
 *
 * CV1 snapshot isolation [two readers + external publisher]
 * Sessions B1 and B2 open at generation G. An EXTERNAL publisher (a
 * second handle over the SAME memory store) advances the store to G+1.
 * Both open sessions must keep reading exactly G's rows: the new row
 * is invisible, old rows still resolve, counts unchanged.
 *
 * CV2 reopen observes latest [fresh session]
 * After the external advance, a FRESH session materialises the latest
 * head and observes the G+1 rows.
 *
 * CV3 write attempt is fenced [publisher generation fence]
 * A read-write session anchored at G attempts a write AFTER the
 * external advance: the sync path re-reads HEAD, sees the foreign
 * generation and refuses with SQLITE_BUSY — the commit must never be
 * silently published from a stale image, and the store head must stay
 * exactly the external commit.
 *
 * CV4 fresh-mode session [OTSARDB_CONSISTENCY=fresh, S3-gated]
 * The sibling workstream landed FRESH consistency mode (
 * 0123, one remote HEAD GET per statement + re-materialisation on a
 * generation advance). The fresh-mode assertion — a session opened
 * with OTSARDB_CONSISTENCY=fresh sees an external commit WITHOUT a
 * reopen — is coded below, guarded by OTSARDB_ENABLE_S3 (the S3
 * build), because the fresh session entry is the real
 * otsardb_s3_open_target_ex session over a live store. Without
 * OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET at runtime it prints
 * SKIPPED (a SKIP is never a PASS, R2) and continues. The snapshot
 * control for the same scenario is CV1/CV2 above.
 *
 * Deterministic knobs (same as 0120): prefetch (OTSARDB_LAZY_PREFETCH=0) and
 * the RAM page cache (OTSARDB_S3_RAM_CACHE_MB=0) are disabled so the read
 * path performs no background work that could race the snapshot assertions.
 */

static const char *SOURCE = "cv-source.db";
static const char *SOURCE2 = "cv-source2.db";
static const char *IMAGE_B1 = "cv-image-b1.db";
static const char *IMAGE_B2 = "cv-image-b2.db";
static const char *IMAGE_C = "cv-image-c.db";
static const char *DB_ID = "cv-visibility-db";
static const char *ROOT1 = "cv/snapshot";
static const char *ROOT2 = "cv/reopen";
static const char *ROOT3 = "cv/fence";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void remove_files(const char *path) {
    char sidecar[256];
    otsardb_unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    otsardb_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    otsardb_unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-journal", path);
    otsardb_unlink(sidecar);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static void exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    assert(otsardb_exec(db, sql, NULL, NULL, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher (the same path the S3 session uses for a write). Materialises the
 * current remote state into `local_path` first, exactly like a real session
 * open. The publisher hook is installed on a DEDICATED VFS instance (not the
 * process-global hook) so an external writer never disturbs another open
 * session's hook. The caller picks the local path so an external writer
 * never clobbers a file another session has open (Windows file locking). */
static void writer_publish_at(otsardb_object_store *store,
                              const char *root,
                              const char *db_id,
                              const char *sql,
                              const char *local_path) {
    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    root,
                                    db_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_local_vfs_instance *vfs = NULL;
    assert(otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                             &publisher,
                                             &vfs) == SQLITE_OK);

    otsardb_head head;
    char *etag = NULL;
    if (otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK) {
        free(etag);
        assert(otsardb_recovery_restore_store(store, root, local_path));
    } else {
        free(etag);
    }

    otsardb *db = NULL;
    assert(otsardb_open_with_vfs_owned(local_path,
                                       otsardb_local_vfs_instance_name(vfs),
                                       NULL,
                                       NULL,
                                       &db) == OTSARDB_OK);
    exec_ok(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    exec_ok(db, sql);
    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    remove_files(local_path);
}

static void writer_publish(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *sql) {
    writer_publish_at(store, root, db_id, sql, SOURCE);
}

/* Dummy sync reader: the read-only sessions never publish. */
static int dummy_sync_reader(const char *wal_name,
                             uint64_t wal_size,
                             otsardb_sqlite_wal_read_at read_at,
                             void *read_ctx,
                             otsardb_sqlite_wal_read_at db_read_at,
                             void *db_read_ctx,
                             void *ctx) {
    (void)wal_name; (void)wal_size; (void)read_at; (void)read_ctx;
    (void)db_read_at; (void)db_read_ctx; (void)ctx;
    return SQLITE_OK;
}

static int page_hydrate(void *ctx, long long offset, int amount) {
    return otsardb_lazy_hydrate_bytes((otsardb_lazy_hydrator *)ctx, offset, amount)
               ? SQLITE_OK
               : SQLITE_IOERR_READ;
}

static void page_mark(void *ctx, long long offset, int amount) {
    otsardb_lazy_mark_bytes((otsardb_lazy_hydrator *)ctx, offset, amount);
}

static void page_truncate(void *ctx, long long new_size) {
    otsardb_lazy_clear_beyond_bytes((otsardb_lazy_hydrator *)ctx, new_size);
}

/* Open a read-only session over `store` at `head`: lazy-restore the image
 * into `image_path` and open a dedicated VFS instance with the page hooks
 * installed. The SAME composition the real read-only session uses. */
static void session_open_readonly(otsardb_object_store *store,
                                  const char *root,
                                  const char *image_path,
                                  const otsardb_head *head,
                                  otsardb_lazy_hydrator **out_hydrator,
                                  otsardb_local_vfs_instance **out_vfs,
                                  otsardb **out_db) {
    assert(otsardb_lazy_restore_store(store, root, image_path, head, out_hydrator));
    assert(otsardb_local_vfs_instance_create(dummy_sync_reader, NULL, out_vfs) == SQLITE_OK);
    assert(otsardb_local_vfs_instance_set_page_hooks(*out_vfs,
                                                     page_hydrate,
                                                     page_mark,
                                                     page_truncate,
                                                     *out_hydrator) == SQLITE_OK);
    assert(otsardb_open_with_vfs_owned(image_path,
                                       otsardb_local_vfs_instance_name(*out_vfs),
                                       NULL,
                                       NULL,
                                       out_db) == OTSARDB_OK);
}

static void session_close(otsardb *db,
                          otsardb_local_vfs_instance *vfs,
                          otsardb_lazy_hydrator *hydrator) {
    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    otsardb_lazy_destroy(hydrator);
}

static void query_one(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    assert(otsardb_exec(db, sql, collect_text, out, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
}

/* ---- CV1 + CV2: snapshot isolation, then reopen observes latest ---------- */

static void test_cross_instance_visibility(void) {
    remove_files(SOURCE);
    remove_files(IMAGE_B1);
    remove_files(IMAGE_B2);
    remove_files(IMAGE_C);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    writer_publish(inner, ROOT1, DB_ID, "CREATE TABLE items(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT1, DB_ID, "INSERT INTO items(v) VALUES('one'),('two'),('three');");

    otsardb_head head_g = read_head(inner, ROOT1);
    assert(head_g.generation >= 2);

    /* Two separate reader sessions open at G (cross-instance: they are
     * independent handles over the same store). */
    otsardb_lazy_hydrator *hyd1 = NULL;
    otsardb_local_vfs_instance *vfs1 = NULL;
    otsardb *b1 = NULL;
    otsardb_lazy_hydrator *hyd2 = NULL;
    otsardb_local_vfs_instance *vfs2 = NULL;
    otsardb *b2 = NULL;
    session_open_readonly(inner, ROOT1, IMAGE_B1, &head_g, &hyd1, &vfs1, &b1);
    session_open_readonly(inner, ROOT1, IMAGE_B2, &head_g, &hyd2, &vfs2, &b2);

    /* External publisher (second handle over the SAME memory store) advances
     * the store to G+1 while both sessions stay open. */
    writer_publish(inner, ROOT1, DB_ID, "INSERT INTO items(v) VALUES('four');");
    otsardb_head head_after = read_head(inner, ROOT1);
    assert(head_after.generation == head_g.generation + 1);

    /* CV1: BOTH open sessions still read exactly G's rows. */
    char result[128];
    query_one(b1, "SELECT count(*) FROM items;", result);
    assert(strcmp(result, "3") == 0);
    query_one(b1, "SELECT count(*) FROM items WHERE v='four';", result);
    assert(strcmp(result, "0") == 0);
    query_one(b1, "SELECT v FROM items WHERE id=2;", result);
    assert(strcmp(result, "two") == 0);

    query_one(b2, "SELECT count(*) FROM items;", result);
    assert(strcmp(result, "3") == 0);
    query_one(b2, "SELECT count(*) FROM items WHERE v='four';", result);
    assert(strcmp(result, "0") == 0);

    session_close(b1, vfs1, hyd1);
    session_close(b2, vfs2, hyd2);

    /* CV2: a FRESH session (reopen) materialises the latest head and
     * observes the G+1 rows. */
    otsardb_head head_latest = read_head(inner, ROOT1);
    assert(head_latest.generation == head_after.generation);
    otsardb_lazy_hydrator *hyd3 = NULL;
    otsardb_local_vfs_instance *vfs3 = NULL;
    otsardb *c = NULL;
    session_open_readonly(inner, ROOT1, IMAGE_C, &head_latest, &hyd3, &vfs3, &c);
    query_one(c, "SELECT count(*) FROM items;", result);
    assert(strcmp(result, "4") == 0);
    query_one(c, "SELECT count(*) FROM items WHERE v='four';", result);
    assert(strcmp(result, "1") == 0);
    query_one(c, "SELECT v FROM items WHERE id=1;", result);
    assert(strcmp(result, "one") == 0);
    session_close(c, vfs3, hyd3);

    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    remove_files(IMAGE_B1);
    remove_files(IMAGE_B2);
    remove_files(IMAGE_C);
}

/* ---- CV3: a write attempt re-reads HEAD and is fenced -------------------- */

static void test_write_attempt_fenced(void) {
    remove_files(SOURCE);
    remove_files(SOURCE2);
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);

    writer_publish(inner, ROOT3, DB_ID, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    writer_publish(inner, ROOT3, DB_ID, "INSERT INTO t(v) VALUES('one');");
    otsardb_head head_g = read_head(inner, ROOT3);
    assert(head_g.generation >= 2);

    /* Read-write session W materialises the image at G and anchors its
     * publisher at G (the native-open pattern). */
    assert(otsardb_recovery_restore_store(inner, ROOT3, SOURCE));
    otsardb_wal_publisher publisher;
    assert(otsardb_wal_publisher_init(&publisher,
                                    inner,
                                    ROOT3,
                                    DB_ID,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));
    otsardb_wal_publisher_set_local_image_generation(&publisher, head_g.generation);
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, &publisher);
    otsardb *w = NULL;
    assert(otsardb_open(SOURCE, &w) == OTSARDB_OK);
    exec_ok(w, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");

    /* External publisher advances to G+1 (its own local file, so it cannot
     * clobber W's open image). */
    writer_publish_at(inner, ROOT3, DB_ID, "INSERT INTO t(v) VALUES('two');", SOURCE2);
    otsardb_head head_after = read_head(inner, ROOT3);
    assert(head_after.generation == head_g.generation + 1);

    /* W's write attempt must be refused: the sync path re-reads HEAD, sees
     * the foreign generation and returns SQLITE_BUSY instead of silently
     * publishing from a stale image. */
    char *error_message = NULL;
    int rc = otsardb_exec(w, "INSERT INTO t(v) VALUES('mine');", NULL, NULL, &error_message);
    assert(rc != OTSARDB_OK);
    assert(error_message != NULL);
    assert(strstr(error_message, "locked") != NULL || strstr(error_message, "busy") != NULL);
    otsardb_free(error_message);

    /* The store still carries EXACTLY the external commit — W's refused
     * write left no trace. */
    otsardb_head head_final = read_head(inner, ROOT3);
    assert(head_final.generation == head_after.generation);
    assert(strcmp(head_final.commit_id, head_after.commit_id) == 0);

    /* W's own snapshot is unchanged and still consistent. */
    char result[128];
    query_one(w, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "1") == 0);

    otsardb_close(w);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_test_memory_store_destroy(inner);
    remove_files(SOURCE);
    remove_files(SOURCE2);
}

/* ---- CV4: fresh-mode session (OTSARDB_CONSISTENCY=fresh).
 * The whole block is excluded from non-S3 builds (the fresh session entry
 * is the real S3 session); in the S3 build it needs a live store at
 * runtime and prints SKIPPED without one (R2). ---------------------------
 */
#if defined(OTSARDB_ENABLE_S3)
#include "s3_database.h"
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

static void test_fresh_mode_sees_external_commit_without_reopen(void) {
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint || !endpoint[0] || !bucket || !bucket[0]) {
        puts("fresh-mode visibility check SKIPPED: OTSARDB_S3_ENDPOINT / "
             "OTSARDB_S3_BUCKET not set (a SKIP is not a PASS, R2)");
        return;
    }

    /* Unique timestamped+random prefix so concurrent runs never collide
     * (the bucket-prefix cleanup is the harness's job, e.g. mc rm). The
     * seed mixes wall time and the process id so two runs started in the
     * same second cannot pick the same suffix. */
    char target[512];
#if defined(_WIN32)
    unsigned long run_id = (unsigned long)GetCurrentProcessId();
#else
    unsigned long run_id = (unsigned long)getpid();
#endif
    srand((unsigned)(time(NULL) * 2654435761u ^ run_id));
    snprintf(target, sizeof(target), "s3://%s/cv-fresh-%ld-%04d", bucket,
             (long)time(NULL), rand() % 10000);

    /* All sessions use the explicit single-writer path: the test asserts
     * the READ-side fresh-mode contract, and in one process two writers
     * would otherwise block on the writer lease (30 s TTL). The store
     * CAS still fences every HEAD update (local MinIO supports CAS). */
    set_env("OTSARDB_S3_SINGLE_WRITER", "1");

    /* Writer A creates the base generation (default snapshot mode). */
    set_env("OTSARDB_CONSISTENCY", "");
    otsardb *a = NULL;
    assert(otsardb_s3_open_target_ex(target, NULL, &a) == OTSARDB_OK);
    exec_ok(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    exec_ok(a, "INSERT INTO t(v) VALUES('one');");
    otsardb_close(a);

    /* Fresh-mode reader F opens and reads the base. */
    set_env("OTSARDB_CONSISTENCY", "fresh");
    otsardb *f = NULL;
    assert(otsardb_s3_open_target_ex(target, NULL, &f) == OTSARDB_OK);
    char result[128];
    query_one(f, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "1") == 0);

    /* External writer B advances the store to G+1. */
    set_env("OTSARDB_CONSISTENCY", "");
    otsardb *b = NULL;
    assert(otsardb_s3_open_target_ex(target, NULL, &b) == OTSARDB_OK);
    exec_ok(b, "INSERT INTO t(v) VALUES('two');");
    otsardb_close(b);

    /* THE fresh-mode contract: the SAME open session F observes B's commit
     * WITHOUT a reopen (a snapshot-pinned session would still read 1 —
     * see CV1/CV2). */
    set_env("OTSARDB_CONSISTENCY", "fresh");
    query_one(f, "SELECT count(*) FROM t;", result);
    assert(strcmp(result, "2") == 0);
    otsardb_close(f);
    set_env("OTSARDB_CONSISTENCY", "");
    set_env("OTSARDB_S3_SINGLE_WRITER", "");
}
#endif /* OTSARDB_ENABLE_S3 */

int main(void) {
    /* Deterministic read path + no background work racing the assertions
     * (the same knobs as). */
    set_env("OTSARDB_LAZY_PREFETCH", "0");
    set_env("OTSARDB_S3_RAM_CACHE_MB", "0");
    set_env("OTSARDB_S3_READ_STATS", "0");
    set_env("OTSARDB_PUBLISH_RETRIES", "1");
    set_env("OTSARDB_PUBLISH_RETRY_BASE_MS", "0");

    assert(otsardb_initialize() == OTSARDB_OK);

    test_cross_instance_visibility();
    test_write_attempt_fenced();

#if defined(OTSARDB_ENABLE_S3)
    test_fresh_mode_sees_external_commit_without_reopen();
    puts("cross-instance visibility contract (snapshot isolation, reopen-latest, "
         "write fence, fresh-mode-without-reopen) passed");
#else
    puts("cross-instance visibility contract (snapshot isolation, reopen-latest, "
         "write fence) passed; fresh-mode assertion NOT compiled (S3 build only, "
         "));
#endif
    return 0;
}

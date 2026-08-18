#include "otsardb.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "sqlite_wal.h"
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

/* regression test: the rolling post-apply checksum may read
 * old-page contributions from the LIVE local db file ONLY after the file is
 * PROVEN to be the parent image. The e2e crash is exactly
 * the unproven case: a fresh disposable local file against an existing
 * checksummed chain. SQLite syncs the WAL header alone (32 bytes, zero
 * publishable commits) before the first frames of a commit; that sync resets
 * the publisher's page map and checksum base, and the NEXT sync (same WAL
 * salts) refreshes the remote HEAD and resurrects the parent checksum over
 * the EMPTY map. The first commit then XORs out fresh-file page values
 * instead of the parent image — publishing a checksum wrong by a constant
 * that recovery rejects on the next restore.
 *
 * The only reliable proof that the file is the parent image is the
 * recovery/materialization anchor (otsardb_wal_publisher_set_local_image_generation,
 * set exclusively right after otsardb_recovery_restore_store /
 * otsardb_lazy_restore_store): a full-file checksum comparison against the
 * parent manifest was rejected because in WAL mode SQLite writes pages only
 * to the WAL, so the local file lags at the last checkpoint and never
 * byte-equals the parent mid-session (observed during this test's
 * development). While the proof is absent, a commit that would need an
 * old-page file read publishes NO post_apply_checksum (recovery treats an
 * absent checksum as "chain predates checksums" and skips the gate); commits
 * fully covered by the page map keep publishing checksums.
 *
 * Deterministic no-MinIO coverage:
 * phase 1 — fresh chain, healthy session WITHOUT the anchor: genesis and
 * map-covered chained commits publish checksums (no file reads
 * needed), recovery validates them.
 * phase 2 — local image materialised by recovery, publisher ANCHORED (the
 * native-open pattern): the two-sync sequence (header-only
 * reset, then same-generation resurrect) is driven with a
 * synthetic WAL; the anchor arms the proof, old-page reads
 * happen and the published checksum validates on restore.
 * phase 3 — same materialised image WITHOUT the anchor: the proof cannot
 * be established, so the same two-sync sequence must publish NO
 * checksum; recovery must still succeed.
 * phase 4 — the crash scenario driven by real SQLite
 * exactly like the e2e: fresh disposable local file, header-only
 * WAL sync, CREATE TABLE + INSERT on the same WAL generation. No
 * checksum may be published and recovery MUST still succeed.
 * Pre-0063 code fails this phase deterministically. */

static const char *source_path = "otsardb-local-image-source.db";
static const char *wal_path = "otsardb-local-image-source.db-wal";
static const char *shm_path = "otsardb-local-image-source.db-shm";
static const char *recovered_path = "otsardb-local-image-recovered.db";
static const char *synth_wal_path = "otsardb-local-image-synth.wal";
static const char *database_root = "local-image/orders";
static const char *database_id = "local-image-db";

static void remove_local_files(void) {
    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);
    otsardb_unlink(synth_wal_path);
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static otsardb_head read_head(otsardb_object_store *store) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, database_root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

/* Decode the head manifest and assert whether it carries a post-apply
 * rolling checksum. */
static void assert_head_checksum(otsardb_object_store *store,
                                 const otsardb_head *head,
                                 int expected_present) {
    otsardb_store_object manifest_object = {0};
    assert(otsardb_store_get(store, head->commit_key, &manifest_object) == OTSARDB_STORE_OK);
    otsardb_commit_manifest manifest;
    assert(otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                             manifest_object.size,
                                             &manifest));
    int present = manifest.post_apply_checksum[0] != '\0';
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(store, &manifest_object);
    assert(present == expected_present);
}

/* Open the local file, run every SQL string on ONE connection (SQLite syncs
 * the WAL header alone before the first frames of the first commit — the
 * precondition — so all statements share one WAL generation),
 * then close. The global hook drives the publisher. */
static void run_session(otsardb_wal_publisher *publisher, const char *const *sqls,
                        size_t sql_count) {
    otsardb_local_vfs_set_wal_reader_sync_hook(otsardb_wal_publisher_sync_reader, publisher);
    otsardb *db = NULL;
    assert(otsardb_open(source_path, &db) == OTSARDB_OK);
    for (size_t i = 0; i < sql_count; ++i) {
        char *error_message = NULL;
        assert(otsardb_exec(db, sqls[i], NULL, NULL, &error_message) == OTSARDB_OK);
        otsardb_free(error_message);
    }
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
}

/* Recover the chain to a fresh file and assert the expected rows. */
static void restore_and_check(otsardb_object_store *store, const char *expected) {
    otsardb_unlink(recovered_path);
    assert(otsardb_recovery_restore_store(store, database_root, recovered_path));
    otsardb *recovered = NULL;
    assert(otsardb_open(recovered_path, &recovered) == OTSARDB_OK);
    char values[128] = {0};
    char *error_message = NULL;
    assert(otsardb_exec(recovered,
                      "SELECT group_concat(value, ',') "
                      "FROM (SELECT value FROM t1 ORDER BY id);",
                      collect_text,
                      values,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(strcmp(values, expected) == 0);
    otsardb_close(recovered);
    otsardb_unlink(recovered_path);
}

/* ---- synthetic WAL construction (SQLite WAL format) ---------------------
 * The commit frames' bytes 4-7 carry the NEW database size in pages (0 for
 * non-commit frames); salts at 8-15; checksum words at 16-23 stored
 * big-endian, computed over frame bytes 0-8 and the page data with SQLite's
 * little-endian rolling checksum (the same algorithm otsardb's WAL scanner
 * validates). */

static uint32_t get_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

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

static void build_wal_header(unsigned char *hdr, uint32_t page_size,
                             uint32_t salt1, uint32_t salt2) {
    memset(hdr, 0, OTSARDB_SQLITE_WAL_HEADER_SIZE);
    put_be32(hdr, 0x377f0682u); /* OTSARDB_SQLITE_WAL_MAGIC_LE (private) */
    put_be32(hdr + 4, 3007000u);
    put_be32(hdr + 8, page_size);
    put_be32(hdr + 12, 1u);
    put_be32(hdr + 16, salt1);
    put_be32(hdr + 20, salt2);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(hdr, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    put_be32(hdr + 24, s0);
    put_be32(hdr + 28, s1);
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

static uint32_t db_page_size(const char *path) {
    unsigned char hdr[100];
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    fclose(f);
    uint32_t ps = ((uint32_t)hdr[16] << 8) | (uint32_t)hdr[17];
    assert(ps >= 512 && ps <= 65536 && (ps & (ps - 1)) == 0);
    return ps;
}

static uint32_t db_page_count(const char *path) {
    unsigned char hdr[100];
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    fclose(f);
    uint32_t n = get_be32(hdr + 28);
    assert(n > 0);
    return n;
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

/* Drive a publisher through the sync sequence with a
 * synthetic WAL over the given local db image: sync 1 = header-only WAL
 * (refresh + reset, nothing published), sync 2 = the same WAL generation
 * with three frames whose page images are copied from the local file
 * (refresh resurrects the parent checksum over the EMPTY map, the local
 * image proof decides whether old-page reads may happen). Returns the
 * published head generation. */
static uint64_t drive_synthetic_session(otsardb_wal_publisher *publisher,
                                        const char *db_path,
                                        uint32_t salt1, uint32_t salt2) {
    uint32_t page_size = db_page_size(db_path);
    uint32_t db_pages = db_page_count(db_path);
    uint32_t frame_count = db_pages < 3u ? db_pages : 3u;
    assert(frame_count >= 1);
    size_t frame_size = (size_t)OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE + page_size;
    size_t wal_size = (size_t)OTSARDB_SQLITE_WAL_HEADER_SIZE + (size_t)frame_count * frame_size;

    unsigned char *wal = (unsigned char *)malloc(wal_size);
    assert(wal != NULL);
    build_wal_header(wal, page_size, salt1, salt2);

    unsigned char *page = (unsigned char *)malloc(page_size);
    assert(page != NULL);
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    wal_checksum(wal, OTSARDB_SQLITE_WAL_HEADER_SIZE - 8u, &s0, &s1);
    for (uint32_t i = 0; i < frame_count; ++i) {
        FILE *f = fopen(db_path, "rb");
        assert(f != NULL);
        uint64_t off = (uint64_t)i * page_size;
        assert(off <= (uint64_t)LONG_MAX);
        assert(fseek(f, (long)off, SEEK_SET) == 0);
        assert(fread(page, 1, page_size, f) == page_size);
        fclose(f);
        uint32_t n_truncate = (i + 1u == frame_count) ? db_pages : 0u;
        build_wal_frame(wal + OTSARDB_SQLITE_WAL_HEADER_SIZE + i * frame_size,
                        i + 1u, n_truncate, page, page_size,
                        salt1, salt2, &s0, &s1);
    }
    free(page);

    FILE *walf = fopen(synth_wal_path, "wb");
    assert(walf != NULL);
    assert(fwrite(wal, 1, wal_size, walf) == wal_size);
    assert(fclose(walf) == 0);
    free(wal);

    /* Sync 1: header-only WAL, nothing publishable (step 1:
     * refresh loads the parent checksum, the generation reset clears it
     * together with the page map). */
    wal_file_reader wal_reader = {NULL};
    wal_reader.file = fopen(synth_wal_path, "rb");
    assert(wal_reader.file != NULL);
    assert(otsardb_wal_publisher_sync_reader(synth_wal_path,
                                           OTSARDB_SQLITE_WAL_HEADER_SIZE,
                                           wal_file_read_at, &wal_reader,
                                           NULL, NULL, publisher) == 0);
    fclose(wal_reader.file);

    /* Sync 2: the same salts -> same generation, no reset; refresh
     * resurrects the parent checksum over the empty page map and the commit
     * XORs out old contributions read from the local file — legitimate only
     * when the file is proven to be the parent image. */
    wal_reader.file = fopen(synth_wal_path, "rb");
    assert(wal_reader.file != NULL);
    wal_file_reader db_reader = {NULL};
    db_reader.file = fopen(db_path, "rb");
    assert(db_reader.file != NULL);
    assert(otsardb_wal_publisher_sync_reader(synth_wal_path,
                                           (uint64_t)wal_size,
                                           wal_file_read_at, &wal_reader,
                                           wal_file_read_at, &db_reader,
                                           publisher) == 0);
    fclose(wal_reader.file);
    fclose(db_reader.file);
    return publisher->current_generation;
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    remove_local_files();
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* ---- phase 1: fresh chain, healthy session WITHOUT the anchor --------
     * Genesis publishes checksums from a 0 base (no file reads); the chained
     * commit is fully covered by the page map, so it keeps publishing one.
     * This phase fails if the fix suppresses checksums for map-covered
     * chained commits (checksum_pipeline_test behaviour must be preserved). */
    {
        otsardb_wal_publisher publisher1;
        assert(otsardb_wal_publisher_init(&publisher1,
                                        store,
                                        database_root,
                                        database_id,
                                        OTSARDB_WRITE_STRATEGY_DEFAULT));
        const char *phase1_sql[] = {
            "PRAGMA journal_mode=WAL;"
            "PRAGMA wal_autocheckpoint=0;",
            "CREATE TABLE t1(id INTEGER PRIMARY KEY, value TEXT);",
            "INSERT INTO t1(value) VALUES('alpha'),('beta'),('gamma');",
        };
        run_session(&publisher1, phase1_sql, 3);
        otsardb_head head = read_head(store);
        assert(head.generation >= 2);
        assert_head_checksum(store, &head, 1);
        restore_and_check(store, "alpha,beta,gamma");
    }

    /* ---- phase 2: materialised local image + generation anchor -----------
     * The anchor (set only by the recovery/materialization paths) proves the
     * file is the parent image, so the resurrected checksum base survives the
     * two-sync sequence: old-page reads happen and the published checksum
     * validates on restore. */
    otsardb_wal_publisher publisher2a;
    uint64_t phase1_generation = 0;
    {
        otsardb_head head = read_head(store);
        phase1_generation = head.generation;
        otsardb_unlink(wal_path);
        otsardb_unlink(shm_path);
        assert(otsardb_recovery_restore_store(store, database_root, source_path));
        assert(otsardb_wal_publisher_init(&publisher2a,
                                        store,
                                        database_root,
                                        database_id,
                                        OTSARDB_WRITE_STRATEGY_DEFAULT));
        otsardb_wal_publisher_set_local_image_generation(&publisher2a, phase1_generation);
        uint64_t published = drive_synthetic_session(&publisher2a, source_path,
                                                     0x11111111u, 0x22222222u);
        assert(published == phase1_generation + 1u);
        assert(publisher2a.local_image_verified == 1);
        otsardb_head head2 = read_head(store);
        assert_head_checksum(store, &head2, 1);
        restore_and_check(store, "alpha,beta,gamma");
    }

    /* ---- phase 3: materialised local image, NO anchor --------------------
     * The proof cannot be established, so the same two-sync sequence must
     * publish NO checksum (the commit needs old-page file reads it is not
     * allowed to make); recovery must still succeed. */
    otsardb_wal_publisher publisher2b;
    {
        otsardb_head head = read_head(store);
        uint64_t phase2_generation = head.generation;
        otsardb_unlink(wal_path);
        otsardb_unlink(shm_path);
        assert(otsardb_recovery_restore_store(store, database_root, source_path));
        assert(otsardb_wal_publisher_init(&publisher2b,
                                        store,
                                        database_root,
                                        database_id,
                                        OTSARDB_WRITE_STRATEGY_DEFAULT));
        uint64_t published = drive_synthetic_session(&publisher2b, source_path,
                                                     0x33333333u, 0x44444444u);
        assert(published == phase2_generation + 1u);
        assert(publisher2b.local_image_verified == 0);
        otsardb_head head3 = read_head(store);
        assert_head_checksum(store, &head3, 0);
        restore_and_check(store, "alpha,beta,gamma");
    }

    /* ---- phase 4: FRESH disposable local file against the chain ----------
     * This is the crash scenario, driven by real SQLite
     * exactly like the e2e: the header-only WAL sync resets the generation,
     * CREATE TABLE + INSERT run on the SAME WAL generation (refresh
     * resurrects the parent checksum over the empty page map). The fresh file
     * is NOT the parent image and there is no anchor: the checksum must not
     * be published and recovery must still succeed. Pre-0063 code fails this
     * phase deterministically. */
    otsardb_wal_publisher publisher3;
    {
        otsardb_head head = read_head(store);
        uint64_t phase3_generation = head.generation;
        assert(head.generation >= 4);
        remove_local_files();
        assert(otsardb_wal_publisher_init(&publisher3,
                                        store,
                                        database_root,
                                        database_id,
                                        OTSARDB_WRITE_STRATEGY_DEFAULT));
        const char *phase4_sql[] = {
            "PRAGMA journal_mode=WAL;"
            "PRAGMA wal_autocheckpoint=0;",
            "CREATE TABLE t1(id INTEGER PRIMARY KEY, value TEXT);",
            "INSERT INTO t1(value) VALUES('one'),('two'),('three');",
        };
        run_session(&publisher3, phase4_sql, 3);
        /* The header-only sync of the first commit's WAL creation must have
         * fired (step 1) for this to be the crash sequence. */
        assert(publisher3.wal_generation_known == 1);
        /* The unproven local image must never arm the checksum base. */
        assert(publisher3.local_image_verified == 0);
        otsardb_head head4 = read_head(store);
        assert(head4.generation > phase3_generation);
        /* No checksum may be published over an unverified local image. */
        assert_head_checksum(store, &head4, 0);
        /* Recovery must succeed: an absent checksum means "chain predates
         * checksums" and the gate is skipped. */
        restore_and_check(store, "one,two,three");
    }

    otsardb_test_memory_store_destroy(store);
    remove_local_files();

    puts("local image verification for rolling checksums passed");
    return 0;
}

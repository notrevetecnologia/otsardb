/* segment_encryption_test.c — AES-256-GCM at-rest segment encryption end-to-end
 * .
 *
 * Deterministic (memory_store + real SQLite through the WAL publisher, same
 * harness pattern as lazy_hydration_test). Scenarios:
 * 1. round-trip: publish with a key -> eager restore with the key produces
 * a byte-identical image to the plaintext reference;
 * 2. cross-key read fails closed (GCM tag path: object hashes still verify
 * against the chain-authenticated manifest, decryption does not);
 * 3. missing key on an encrypted chain fails closed with a clear error;
 * 4. plaintext chain + key set still decodes (manifests without
 * "segment_enc" decode unchanged);
 * 5. mixed chain (plaintext / encrypted / plaintext / encrypted commits)
 * decodes per-entry;
 * 6. ciphertext byte flip fails the restore (chain-authenticated SHA-256
 * over the stored envelope);
 * 7. lazy-hydration fetch path decrypts (SQL SELECT over the hydrated
 * image) and fails closed without the key;
 * 8. stored envelopes never contain the plaintext segment magic
 * ("OTSSEG01") — payload confidentiality at rest;
 * 9. encrypted segments carry "segment_enc" in the manifest and no
 * frame_offsets (range reads are impossible on a GCM envelope).
 *
 * Publish/restore plumbing: the WAL publisher builds its own commit request,
 * so encryption of a publish is gated by the thread-local session cipher
 * (otsardb_cipher_thread_set — the exact mechanism s3_database.cpp uses at
 * session open). The parent-chain materialisation inside the helper passes
 * the restore cipher EXPLICITLY (a mixed chain's plaintext publish must
 * still decrypt its encrypted parent).
 *
 * Wiring (coordinator, listed in):
 * add_executable(otsardb_segment_encryption_test
 * tests/segment_encryption_test.c tests/memory_store.c)
 * target_include_directories(otsardb_segment_encryption_test
 * PRIVATE include src/core src/storage src/crypto tests)
 * target_link_libraries(otsardb_segment_encryption_test PRIVATE otsardb_core)
 * add_test(NAME otsardb_segment_encryption COMMAND otsardb_segment_encryption_test)
 */
#include "otsardb.h"
#include "cipher.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "local_vfs.h"
#include "memory_store.h"
#include "recovery.h"
#include "segment.h"
#include "sqlite3.h"
#include "wal_batch.h"
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

static const char KEY_A_HEX[] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char KEY_B_HEX[] =
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

static const char *SOURCE = "seg-enc-source.db";
static const char *WAL_PATH = "seg-enc-source.db-wal";
static const char *SHM_PATH = "seg-enc-source.db-shm";
static const char *PLAIN_IMAGE = "seg-enc-plain.db";
static const char *ENC_IMAGE = "seg-enc-image.db";
static const char *ROOT = "seg-enc/orders";
static const char *DB_ID = "seg-enc-orders-db";

static void cleanup_files(void) {
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
    otsardb_unlink(PLAIN_IMAGE);
    otsardb_unlink(ENC_IMAGE);
}

static otsardb_head read_head(otsardb_object_store *store, const char *root) {
    otsardb_head head;
    char *etag = NULL;
    assert(otsardb_journal_read_head(store, root, &head, &etag) == OTSARDB_JOURNAL_OK);
    free(etag);
    return head;
}

/* Publish one commit by driving a real SQLite connection through the WAL
 * publisher (the same path the S3 session uses). When a thread cipher is
 * registered (otsardb_cipher_thread_set), the publish path encrypts every
 * segment — exactly like a session opened with OTSARDB_S3_ENC_KEY.
 * restore_cipher decrypts the parent chain during materialisation (NULL
 * for a fully-plaintext parent). */
static void publish_commit(otsardb_object_store *store,
                           const char *root,
                           const char *db_id,
                           const char *sql,
                           const otsardb_cipher *restore_cipher) {
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
        assert(otsardb_recovery_restore_store_ex(store, root, SOURCE, restore_cipher));
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
    int exec_rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (exec_rc != OTSARDB_OK) {
        fprintf(stderr, "publish_commit: exec failed rc=%d err=%s\n",
                exec_rc, error_message ? error_message : "(null)");
    }
    assert(exec_rc == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    otsardb_local_vfs_set_wal_reader_sync_hook(NULL, NULL);
    otsardb_unlink(SOURCE);
    otsardb_unlink(WAL_PATH);
    otsardb_unlink(SHM_PATH);
}

/* Read the newest manifest JSON + its parsed segment_enc set. */
static void read_head_manifest(otsardb_object_store *store,
                               const char *root,
                               const otsardb_head *head,
                               otsardb_commit_manifest *manifest,
                               otsardb_segment_enc_set *enc) {
    memset(manifest, 0, sizeof(*manifest));
    memset(enc, 0, sizeof(*enc));
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, head->commit_key, &object) == OTSARDB_STORE_OK);
    assert(otsardb_commit_manifest_decode_json((const char *)object.data,
                                             object.size,
                                             manifest));
    assert(otsardb_segment_enc_parse_json((const char *)object.data,
                                        object.size,
                                        enc));
    otsardb_store_release_object(store, &object);
}

static int file_bytes_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    assert(fseek(fa, 0, SEEK_END) == 0);
    assert(fseek(fb, 0, SEEK_END) == 0);
    long sa = ftell(fa);
    long sb = ftell(fb);
    assert(sa >= 0 && sb >= 0);
    assert(fseek(fa, 0, SEEK_SET) == 0);
    assert(fseek(fb, 0, SEEK_SET) == 0);
    if (sa != sb) {
        fclose(fa);
        fclose(fb);
        return 0;
    }
    int equal = 1;
    unsigned char buf_a[4096];
    unsigned char buf_b[4096];
    long remaining = sa;
    while (remaining > 0 && equal) {
        size_t want = remaining > (long)sizeof(buf_a) ? sizeof(buf_a) : (size_t)remaining;
        assert(fread(buf_a, 1, want, fa) == want);
        assert(fread(buf_b, 1, want, fb) == want);
        if (memcmp(buf_a, buf_b, want) != 0) equal = 0;
        remaining -= (long)want;
    }
    fclose(fa);
    fclose(fb);
    return equal;
}

/* Does the object contain the plaintext segment magic anywhere? */
static int store_object_contains_plaintext_magic(otsardb_object_store *store,
                                                 const char *key) {
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_OK);
    static const char MAGIC[] = "OTSSEG01";
    int found = 0;
    if (object.size >= sizeof(MAGIC) - 1u) {
        for (size_t i = 0; i + sizeof(MAGIC) - 1u <= object.size; ++i) {
            if (memcmp((const char *)object.data + i, MAGIC, sizeof(MAGIC) - 1u) == 0) {
                found = 1;
                break;
            }
        }
    }
    otsardb_store_release_object(store, &object);
    return found;
}

static const char SQL_CREATE_ITEMS[] =
    "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, price REAL);";
static const char SQL_INSERT_ITEMS2[] =
    "INSERT INTO items(name, price) VALUES ('alpha', 1.5), ('beta', 2.25);";
static const char SQL_INSERT_ITEMS200[] =
    "INSERT INTO items(name, price) SELECT 'row' || id, id * 1.5 FROM "
    "(WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt "
    "WHERE x < 200) SELECT x AS id FROM cnt);";

static void test_round_trip(void) {
    otsardb_object_store *plain_store = otsardb_test_memory_store_create();
    otsardb_object_store *enc_store = otsardb_test_memory_store_create();
    assert(plain_store && enc_store);

    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
    assert(otsardb_db_metadata_create(plain_store, ROOT, &metadata) == OTSARDB_METADATA_OK);
    assert(otsardb_db_metadata_create(enc_store, ROOT, &metadata) == OTSARDB_METADATA_OK);

    /* Reference: plaintext chain, same SQL. */
    publish_commit(plain_store, ROOT, DB_ID, SQL_CREATE_ITEMS, NULL);
    publish_commit(plain_store, ROOT, DB_ID, SQL_INSERT_ITEMS2, NULL);
    publish_commit(plain_store, ROOT, DB_ID, SQL_INSERT_ITEMS200, NULL);
    otsardb_head plain_head = read_head(plain_store, ROOT);
    assert(plain_head.generation == 3);

    /* Encrypted chain: same SQL with the session cipher registered. */
    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);
    otsardb_cipher_thread_set(cipher);
    publish_commit(enc_store, ROOT, DB_ID, SQL_CREATE_ITEMS, NULL);
    publish_commit(enc_store, ROOT, DB_ID, SQL_INSERT_ITEMS2, cipher);
    publish_commit(enc_store, ROOT, DB_ID, SQL_INSERT_ITEMS200, cipher);
    otsardb_cipher_thread_set(NULL);
    otsardb_head enc_head = read_head(enc_store, ROOT);
    assert(enc_head.generation == 3);

    /* Round-trip: restore the encrypted store WITH the key and compare the
     * image against the plaintext reference byte for byte. */
    assert(otsardb_recovery_restore_store(plain_store, ROOT, PLAIN_IMAGE));
    assert(otsardb_recovery_restore_store_ex(enc_store, ROOT, ENC_IMAGE, cipher));
    assert(file_bytes_equal(PLAIN_IMAGE, ENC_IMAGE));

    /* The encrypted chain's manifest carries segment_enc; the plaintext
     * chain's does not. Encrypted segments never record frame_offsets. */
    otsardb_commit_manifest enc_manifest;
    otsardb_segment_enc_set enc;
    read_head_manifest(enc_store, ROOT, &enc_head, &enc_manifest, &enc);
    assert(enc.present);
    assert(enc.count == enc_manifest.segment_count);
    for (uint32_t i = 0; i < enc_manifest.segment_count; ++i) {
        assert(enc.entries[i].present);
        assert(enc_manifest.segments[i].frame_offsets == NULL);
        assert(enc_manifest.segments[i].page_count > 0);
        /* The stored object must not contain the plaintext magic. */
        char absolute_key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(absolute_key, sizeof(absolute_key), "%s/%s",
                         ROOT, enc_manifest.segments[i].key);
        assert(n > 0 && (size_t)n < sizeof(absolute_key));
        assert(!store_object_contains_plaintext_magic(enc_store, absolute_key));
    }
    otsardb_commit_manifest_free(&enc_manifest);

    otsardb_commit_manifest plain_manifest;
    otsardb_segment_enc_set plain_enc;
    read_head_manifest(plain_store, ROOT, &plain_head, &plain_manifest, &plain_enc);
    assert(!plain_enc.present);
    otsardb_commit_manifest_free(&plain_manifest);

    otsardb_test_memory_store_destroy(plain_store);
    otsardb_test_memory_store_destroy(enc_store);
    otsardb_cipher_destroy(cipher);
}

static void test_cross_key_and_missing_key_fail_closed(void) {
    otsardb_object_store *enc_store = otsardb_test_memory_store_create();
    assert(enc_store != NULL);
    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
    assert(otsardb_db_metadata_create(enc_store, ROOT, &metadata) == OTSARDB_METADATA_OK);

    otsardb_cipher *cipher_a = otsardb_cipher_create_from_hex(KEY_A_HEX);
    otsardb_cipher *cipher_b = otsardb_cipher_create_from_hex(KEY_B_HEX);
    assert(cipher_a && cipher_b);

    otsardb_cipher_thread_set(cipher_a);
    publish_commit(enc_store, ROOT, DB_ID,
                   "CREATE TABLE t(x INTEGER PRIMARY KEY, y TEXT);", NULL);
    publish_commit(enc_store, ROOT, DB_ID,
                   "INSERT INTO t(y) VALUES ('secret'), ('rows');", cipher_a);
    otsardb_cipher_thread_set(NULL);
    assert(read_head(enc_store, ROOT).generation == 2);

    /* Wrong key: object hashes verify (chain-authenticated manifest) but the
     * GCM tag fails -> restore fails closed. */
    assert(!otsardb_recovery_restore_store_ex(enc_store, ROOT, ENC_IMAGE, cipher_b));

    /* Missing key: the manifest says the segment is encrypted -> fail closed
     * before any decryption. */
    assert(!otsardb_recovery_restore_store_ex(enc_store, ROOT, ENC_IMAGE, NULL));

    /* Right key still works after the failures. */
    assert(otsardb_recovery_restore_store_ex(enc_store, ROOT, ENC_IMAGE, cipher_a));

    otsardb_test_memory_store_destroy(enc_store);
    otsardb_cipher_destroy(cipher_a);
    otsardb_cipher_destroy(cipher_b);
}

static void test_plaintext_chain_with_key_decodes(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);
    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
    assert(otsardb_db_metadata_create(store, ROOT, &metadata) == OTSARDB_METADATA_OK);

    /* Plaintext chain (no cipher registered), then restore with a key set:
     * manifests without "segment_enc" decode unchanged. */
    publish_commit(store, ROOT, DB_ID,
                   "CREATE TABLE t(x INTEGER PRIMARY KEY, y TEXT);", NULL);
    publish_commit(store, ROOT, DB_ID,
                   "INSERT INTO t(y) VALUES ('plaintext rows');", NULL);
    assert(read_head(store, ROOT).generation == 2);

    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);
    assert(otsardb_recovery_restore_store_ex(store, ROOT, ENC_IMAGE, cipher));

    /* And the legacy entry point with the thread cipher registered decodes
     * the same chain (the session-path behavior). */
    otsardb_cipher_thread_set(cipher);
    assert(otsardb_recovery_restore_store(store, ROOT, PLAIN_IMAGE));
    otsardb_cipher_thread_set(NULL);
    assert(file_bytes_equal(PLAIN_IMAGE, ENC_IMAGE));

    otsardb_test_memory_store_destroy(store);
    otsardb_cipher_destroy(cipher);
}

static void test_mixed_chain_decodes_per_entry(void) {
    otsardb_object_store *plain_store = otsardb_test_memory_store_create();
    otsardb_object_store *mixed_store = otsardb_test_memory_store_create();
    assert(plain_store && mixed_store);
    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
    assert(otsardb_db_metadata_create(plain_store, ROOT, &metadata) == OTSARDB_METADATA_OK);
    assert(otsardb_db_metadata_create(mixed_store, ROOT, &metadata) == OTSARDB_METADATA_OK);

    /* Reference plaintext chain, same SQL. */
    publish_commit(plain_store, ROOT, DB_ID,
                   "CREATE TABLE t(x INTEGER PRIMARY KEY, y TEXT);", NULL);
    publish_commit(plain_store, ROOT, DB_ID,
                   "INSERT INTO t(y) VALUES ('first'), ('second');", NULL);
    publish_commit(plain_store, ROOT, DB_ID,
                   "UPDATE t SET y = y || '!';", NULL);
    publish_commit(plain_store, ROOT, DB_ID,
                   "INSERT INTO t(y) SELECT 'bulk' || id FROM (WITH RECURSIVE cnt(x) AS "
                   "(SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 150) "
                   "SELECT x AS id FROM cnt);", NULL);

    /* Mixed chain: plaintext, encrypted, plaintext, encrypted. The thread
     * cipher gates the PUBLISH; the restore cipher decrypts the parent. */
    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);

    publish_commit(mixed_store, ROOT, DB_ID,
                   "CREATE TABLE t(x INTEGER PRIMARY KEY, y TEXT);", NULL);
    otsardb_cipher_thread_set(cipher);
    publish_commit(mixed_store, ROOT, DB_ID,
                   "INSERT INTO t(y) VALUES ('first'), ('second');", cipher);
    otsardb_cipher_thread_set(NULL);
    publish_commit(mixed_store, ROOT, DB_ID,
                   "UPDATE t SET y = y || '!';", cipher);
    otsardb_cipher_thread_set(cipher);
    publish_commit(mixed_store, ROOT, DB_ID,
                   "INSERT INTO t(y) SELECT 'bulk' || id FROM (WITH RECURSIVE cnt(x) AS "
                   "(SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 150) "
                   "SELECT x AS id FROM cnt);", cipher);
    otsardb_cipher_thread_set(NULL);

    assert(read_head(plain_store, ROOT).generation == 4);
    assert(read_head(mixed_store, ROOT).generation == 4);

    /* Per-entry decode: the mixed chain materialises byte-identical to the
     * all-plaintext reference. */
    assert(otsardb_recovery_restore_store(plain_store, ROOT, PLAIN_IMAGE));
    assert(otsardb_recovery_restore_store_ex(mixed_store, ROOT, ENC_IMAGE, cipher));
    assert(file_bytes_equal(PLAIN_IMAGE, ENC_IMAGE));

    /* The mixed chain's newest manifest carries segment_enc with per-entry
     * present flags (the commit is encrypted as a whole). */
    otsardb_head mixed_head = read_head(mixed_store, ROOT);
    otsardb_commit_manifest manifest;
    otsardb_segment_enc_set enc;
    read_head_manifest(mixed_store, ROOT, &mixed_head, &manifest, &enc);
    assert(enc.present);
    assert(enc.count == manifest.segment_count);
    for (uint32_t i = 0; i < manifest.segment_count; ++i) assert(enc.entries[i].present);
    otsardb_commit_manifest_free(&manifest);

    otsardb_test_memory_store_destroy(plain_store);
    otsardb_test_memory_store_destroy(mixed_store);
    otsardb_cipher_destroy(cipher);
}

static void test_tampered_ciphertext_fails(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);
    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
    assert(otsardb_db_metadata_create(store, ROOT, &metadata) == OTSARDB_METADATA_OK);

    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);
    otsardb_cipher_thread_set(cipher);
    publish_commit(store, ROOT, DB_ID,
                   "CREATE TABLE t(x INTEGER PRIMARY KEY, y TEXT);", NULL);
    publish_commit(store, ROOT, DB_ID,
                   "INSERT INTO t(y) VALUES ('tamper me');", cipher);
    otsardb_cipher_thread_set(NULL);

    otsardb_head head = read_head(store, ROOT);
    otsardb_commit_manifest manifest;
    otsardb_segment_enc_set enc;
    read_head_manifest(store, ROOT, &head, &manifest, &enc);
    assert(enc.present && enc.entries[0].present);

    char absolute_key[OTSARDB_KEY_MAX + 1];
    int n = snprintf(absolute_key, sizeof(absolute_key), "%s/%s",
                     ROOT, manifest.segments[0].key);
    assert(n > 0 && (size_t)n < sizeof(absolute_key));

    /* Flip one ciphertext byte inside the newest segment object. */
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, absolute_key, &object) == OTSARDB_STORE_OK);
    unsigned char *mutated = (unsigned char *)malloc(object.size ? object.size : 1u);
    assert(mutated);
    memcpy(mutated, object.data, object.size);
    mutated[OTSARDB_CIPHER_IV_BYTES + 1] ^= 0xff; /* inside the ciphertext region */
    assert(otsardb_store_put(store, absolute_key, mutated, object.size, NULL, NULL) ==
           OTSARDB_STORE_OK);
    free(mutated);
    otsardb_store_release_object(store, &object);

    /* The stored bytes no longer hash to the manifest ref -> the restore
     * fails closed (chain-authenticated integrity; the GCM tag failure path
     * is covered deterministically by cipher_test and by the cross-key
     * scenario). */
    assert(!otsardb_recovery_restore_store_ex(store, ROOT, ENC_IMAGE, cipher));

    otsardb_commit_manifest_free(&manifest);
    otsardb_test_memory_store_destroy(store);
    otsardb_cipher_destroy(cipher);
}

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

static int collect_count(void *ctx, int argc, char **argv, char **columns) {
    (void)argv;
    (void)columns;
    if (argc != 1) return 1;
    *((long *)ctx) = strtol(argv[0], NULL, 10);
    return 0;
}

static long query_rows(otsardb_object_store *store,
                       const char *root,
                       const char *image_path,
                       const otsardb_cipher *cipher) {
    otsardb_head head = read_head(store, root);
    otsardb_lazy_hydrator *hydrator = NULL;
    if (cipher) {
        assert(otsardb_lazy_restore_store_ex(store, root, image_path, &head,
                                           &hydrator, cipher));
    } else {
        assert(!otsardb_lazy_restore_store_ex(store, root, image_path, &head,
                                            &hydrator, NULL));
        return -1; /* fail closed: no hydrator */
    }

    otsardb_local_vfs_instance *vfs = NULL;
    assert(otsardb_local_vfs_instance_create(dummy_sync_reader, NULL, &vfs) == SQLITE_OK);
    assert(otsardb_local_vfs_instance_set_page_hooks(vfs,
                                                   page_hydrate,
                                                   page_mark,
                                                   page_truncate,
                                                   hydrator) == SQLITE_OK);

    otsardb *db = NULL;
    assert(otsardb_open_with_vfs_owned(image_path,
                                     otsardb_local_vfs_instance_name(vfs),
                                     NULL,
                                     NULL,
                                     &db) == OTSARDB_OK);
    long count = -1;
    char *error_message = NULL;
    assert(otsardb_exec(db, "SELECT COUNT(*) FROM t;",
                      (otsardb_row_callback)collect_count, &count, &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    otsardb_close(db);
    otsardb_local_vfs_instance_destroy(vfs);
    otsardb_lazy_destroy(hydrator);
    return count;
}

static void test_lazy_fetch_decrypts(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);
    otsardb_db_metadata metadata = {0};
    metadata.format_version = 1;
    metadata.page_size = 4096;
    snprintf(metadata.database_id, sizeof(metadata.database_id), "%s", DB_ID);
    assert(otsardb_db_metadata_create(store, ROOT, &metadata) == OTSARDB_METADATA_OK);

    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);
    otsardb_cipher_thread_set(cipher);
    publish_commit(store, ROOT, DB_ID,
                   "CREATE TABLE t(x INTEGER PRIMARY KEY, y TEXT);", NULL);
    publish_commit(store, ROOT, DB_ID,
                   "INSERT INTO t(y) SELECT 'lazy' || id FROM (WITH RECURSIVE cnt(x) AS "
                   "(SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 100) "
                   "SELECT x AS id FROM cnt);", cipher);
    otsardb_cipher_thread_set(NULL);
    assert(read_head(store, ROOT).generation == 2);

    /* Lazy restore + real SQL SELECT through the VFS page hooks: the lazy
     * fetch path decrypts encrypted segments. */
    assert(query_rows(store, ROOT, ENC_IMAGE, cipher) == 100);

    /* Lazy restore without the key: the OPEN validates the chain without
     * fetching segments (it succeeds), and the first page read fails closed
     * (SQLITE_IOERR_READ) instead of serving garbage. */
    otsardb_head head = read_head(store, ROOT);
    otsardb_lazy_hydrator *hydrator = NULL;
    assert(otsardb_lazy_restore_store_ex(store, ROOT, ENC_IMAGE, &head, &hydrator, NULL));
    assert(hydrator != NULL);

    otsardb_local_vfs_instance *vfs = NULL;
    assert(otsardb_local_vfs_instance_create(dummy_sync_reader, NULL, &vfs) == SQLITE_OK);
    assert(otsardb_local_vfs_instance_set_page_hooks(vfs,
                                                   page_hydrate,
                                                   page_mark,
                                                   page_truncate,
                                                   hydrator) == SQLITE_OK);
    otsardb *db = NULL;
    /* SQLite reads page 1 during open: the hydrate hook fails closed
     * (SQLITE_IOERR_READ) — the open itself refuses the encrypted database
     * without a key instead of serving garbage. */
    int open_rc = otsardb_open_with_vfs_owned(ENC_IMAGE,
                                            otsardb_local_vfs_instance_name(vfs),
                                            NULL,
                                            NULL,
                                            &db);
    assert(open_rc != OTSARDB_OK);
    assert(db == NULL);
    otsardb_local_vfs_instance_destroy(vfs);
    otsardb_lazy_destroy(hydrator);

    otsardb_test_memory_store_destroy(store);
    otsardb_cipher_destroy(cipher);
}

int main(void) {
    assert(otsardb_initialize() == OTSARDB_OK);
    cleanup_files();
    test_round_trip();
    test_cross_key_and_missing_key_fail_closed();
    test_plaintext_chain_with_key_decodes();
    test_mixed_chain_decodes_per_entry();
    test_tampered_ciphertext_fails();
    test_lazy_fetch_decrypts();
    cleanup_files();
    printf("otsardb_segment_encryption_test: ALL PASS\n");
    return 0;
}

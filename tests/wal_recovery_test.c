#include "otsardb.h"
#include "recovery.h"
#include "segment.h"
#include "sqlite_wal.h"
#include "wal_batch.h"

#include <assert.h>
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

typedef struct recovery_context {
    const char *recovered_path;
    unsigned int commit_count;
} recovery_context;

static void put_u32_le(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char *data = (unsigned char *)malloc(length > 0 ? (size_t)length : 1u);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (length > 0 && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

static int capture_commit(uint32_t page_size,
                          uint64_t database_size_pages,
                          const otsardb_sqlite_wal_frame_ref *frames,
                          uint32_t frame_count,
                          otsardb_sqlite_wal_read_at read_at,
                          void *read_ctx,
                          void *ctx_ptr) {
    recovery_context *ctx = (recovery_context *)ctx_ptr;

    uint32_t *page_nos = (uint32_t *)malloc((size_t)frame_count * sizeof(uint32_t));
    unsigned char *pages = (unsigned char *)malloc((size_t)frame_count * (size_t)page_size);
    assert(page_nos != NULL && pages != NULL);
    for (uint32_t i = 0; i < frame_count; ++i) {
        page_nos[i] = frames[i].page_no;
        assert(read_at(read_ctx,
                       frames[i].frame_offset + OTSARDB_SQLITE_WAL_FRAME_HEADER_SIZE,
                       pages + (size_t)i * page_size,
                       page_size));
    }

    unsigned char *batch = NULL;
    size_t batch_size = 0;
    assert(otsardb_wal_batch_encode(page_size,
                                  database_size_pages,
                                  page_nos,
                                  pages,
                                  frame_count,
                                  &batch,
                                  &batch_size));
    free(page_nos);
    free(pages);

    char commit_id[64];
    snprintf(commit_id, sizeof(commit_id), "wal-commit-%u", ctx->commit_count + 1u);

    unsigned char *segment = NULL;
    size_t segment_size = 0;
    char segment_sha256[65];
    int compress = (ctx->commit_count % 2 == 0) ? 1 : 0;
    assert(otsardb_segment_encode(OTSARDB_SEGMENT_WAL_FRAMES,
                                commit_id,
                                batch,
                                batch_size,
                                &segment,
                                &segment_size,
                                segment_sha256,
                                compress));
    free(batch);

    otsardb_segment_view segment_view;
    assert(otsardb_segment_decode(segment, segment_size, &segment_view));
    assert(segment_view.kind == OTSARDB_SEGMENT_WAL_FRAMES);
    assert(strcmp(segment_view.commit_id, commit_id) == 0);
    if (segment_view.compressed) {
        unsigned char *decompressed = NULL;
        size_t decompressed_size = 0;
        assert(otsardb_segment_decompress(&segment_view,
                                        (void **)&decompressed,
                                        &decompressed_size));
        assert(otsardb_recovery_apply_wal_batch_file(decompressed,
                                                   decompressed_size,
                                                   ctx->recovered_path));
        free(decompressed);
    } else {
        /* Legacy v1-format decode: v1's header is byte-identical to v2 with
         * flags=0 and format_version=1, so rewriting the format field of a
         * raw (compress=0) v2 segment produces exactly what a v1 encoder
         * would have written. Decode must accept it uncompressed. */
        unsigned char *v1_segment = (unsigned char *)malloc(segment_size);
        assert(v1_segment != NULL);
        memcpy(v1_segment, segment, segment_size);
        put_u32_le(v1_segment + 8, 1u);

        otsardb_segment_view v1_view;
        assert(otsardb_segment_decode(v1_segment, segment_size, &v1_view));
        assert(v1_view.format_version == 1u);
        assert(!v1_view.compressed);
        assert(v1_view.payload_size == segment_view.payload_size);
        assert(memcmp(v1_view.payload, segment_view.payload, segment_view.payload_size) == 0);
        assert(otsardb_recovery_apply_wal_batch_file(v1_view.payload,
                                                   v1_view.payload_size,
                                                   ctx->recovered_path));
        free(v1_segment);
    }
    free(segment);
    ++ctx->commit_count;
    return 0;
}

static int collect_value(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    char *output = (char *)ctx;
    size_t used = strlen(output);
    if (used != 0) strcat(output, ",");
    strcat(output, argv[0]);
    return 0;
}

int main(void) {
    const char *source_path = "otsardb-wal-source.db";
    const char *wal_path = "otsardb-wal-source.db-wal";
    const char *shm_path = "otsardb-wal-source.db-shm";
    const char *recovered_path = "otsardb-wal-recovered.db";

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);

    otsardb *source = NULL;
    assert(otsardb_open(source_path, &source) == OTSARDB_OK);

    char *error_message = NULL;
    assert(otsardb_exec(source,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;"
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    assert(otsardb_exec(source,
                      "INSERT INTO items(value) VALUES('one');",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = NULL;

    assert(otsardb_exec(source,
                      "BEGIN;"
                      "INSERT INTO items(value) VALUES('two');"
                      "INSERT INTO items(value) VALUES('three');"
                      "COMMIT;",
                      NULL,
                      NULL,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);

    size_t wal_size = 0;
    unsigned char *wal_data = read_file(wal_path, &wal_size);
    assert(wal_data != NULL);
    assert(wal_size > 32u);

    recovery_context recovery = {
        .recovered_path = recovered_path,
        .commit_count = 0,
    };
    otsardb_sqlite_wal_scan_result scan;
    assert(otsardb_sqlite_wal_scan(wal_data,
                                 wal_size,
                                 capture_commit,
                                 &recovery,
                                 &scan));
    free(wal_data);

    assert(scan.page_size >= 512u);
    assert(scan.commit_count >= 3u);
    assert(scan.valid_frames == scan.committed_frames);
    assert(recovery.commit_count == scan.commit_count);

    otsardb_close(source);

    otsardb *recovered = NULL;
    assert(otsardb_open(recovered_path, &recovered) == OTSARDB_OK);
    char values[128] = {0};
    error_message = NULL;
    assert(otsardb_exec(recovered,
                      "SELECT value FROM items ORDER BY id;",
                      collect_value,
                      values,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(strcmp(values, "one,two,three") == 0);
    otsardb_close(recovered);

    otsardb_unlink(source_path);
    otsardb_unlink(wal_path);
    otsardb_unlink(shm_path);
    otsardb_unlink(recovered_path);

    printf("real WAL recovery passed: commits=%u frames=%u page_size=%u\n",
           scan.commit_count,
           scan.committed_frames,
           scan.page_size);
    return 0;
}

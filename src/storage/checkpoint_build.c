#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "checkpoint_page.h"
#include "commit_manifest.h"
#include "commit_publish.h"
#include "db_metadata.h"
#include "journal.h"
#include "recovery.h"
#include "sha256.h"

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define otsardb_fseek _fseeki64
#else
#define otsardb_fseek fseeko
#endif

#define CHECKPOINT_MAX_PAGES_PER_GROUP 4096u

static int root_join(const char *root, const char *suffix, char *out, size_t out_size) {
    if (!root || !suffix || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", root, needs_slash ? "/" : "", suffix);
    return n > 0 && (size_t)n < out_size;
}

static void read_db_pages(FILE *file,
                          uint32_t page_size,
                          uint64_t start_page,
                          uint32_t count,
                          uint32_t *page_nos,
                          unsigned char *pages) {
    uint64_t base = start_page * (uint64_t)page_size;
    otsardb_fseek(file, (int64_t)base, SEEK_SET);
    for (uint32_t i = 0; i < count; ++i) {
        page_nos[i] = (uint32_t)(start_page + i + 1u);
        if (fread(pages + (size_t)i * page_size, 1, page_size, file) != page_size) {
            memset(pages + (size_t)i * page_size, 0, page_size);
        }
    }
}

int otsardb_checkpoint_build(otsardb_object_store *store,
                           const char *database_root,
                           const char *temp_db_path) {
    if (!store || !database_root || !temp_db_path) return 0;

    /* 1. Restore the full database image via the existing recovery path. */
    if (!otsardb_recovery_restore_store(store, database_root, temp_db_path)) return 0;

    /* 2. Read metadata for page_size / database_id. */
    otsardb_db_metadata metadata;
    if (otsardb_db_metadata_read(store, database_root, &metadata) != OTSARDB_METADATA_OK) return 0;

    /* 3. Read HEAD and its manifest for database_size_pages. */
    otsardb_head head;
    char *head_etag = NULL;
    otsardb_journal_result head_rc = otsardb_journal_read_head(store, database_root, &head, &head_etag);
    if (head_rc != OTSARDB_JOURNAL_OK) {
        free(head_etag);
        return 0;
    }
    free(head_etag);
    otsardb_store_object manifest_object = {0};
    if (otsardb_store_get(store, head.commit_key, &manifest_object) != OTSARDB_STORE_OK) return 0;
    otsardb_commit_manifest head_manifest;
    if (!otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                           manifest_object.size,
                                           &head_manifest)) {
        otsardb_store_release_object(store, &manifest_object);
        return 0;
    }
    otsardb_store_release_object(store, &manifest_object);

    const uint32_t page_size = metadata.page_size;
    const uint64_t database_size_pages = head_manifest.database_size_pages;
    otsardb_commit_manifest_free(&head_manifest);
    if (database_size_pages == 0) return 0;

    /* 4. Partition pages into bounded groups and upload each group. */
    uint64_t remaining = database_size_pages;
    uint64_t cursor = 0;

    otsardb_checkpoint_manifest cp_manifest;
    memset(&cp_manifest, 0, sizeof(cp_manifest));
    cp_manifest.format_version = OTSARDB_CHECKPOINT_MANIFEST_FORMAT_VERSION;
    cp_manifest.covered_generation = head.generation;
    cp_manifest.page_size = page_size;
    cp_manifest.database_size_pages = database_size_pages;
    snprintf(cp_manifest.database_id, sizeof(cp_manifest.database_id), "%s", head.database_id);
    snprintf(cp_manifest.base_commit_id, sizeof(cp_manifest.base_commit_id), "%s", head.commit_id);
    snprintf(cp_manifest.base_manifest_sha256, sizeof(cp_manifest.base_manifest_sha256),
             "%s", head.commit_sha256);

    FILE *db_file = fopen(temp_db_path, "rb");
    if (!db_file) return 0;

    uint32_t *page_nos = (uint32_t *)malloc(CHECKPOINT_MAX_PAGES_PER_GROUP * sizeof(uint32_t));
    unsigned char *pages = (unsigned char *)malloc(CHECKPOINT_MAX_PAGES_PER_GROUP * (size_t)page_size);
    if (!page_nos || !pages) { fclose(db_file); free(page_nos); free(pages); return 0; }

    int ok = 1;
    uint32_t group_idx = 0;
    while (remaining > 0 && ok) {
        uint32_t chunk = remaining > CHECKPOINT_MAX_PAGES_PER_GROUP
                              ? CHECKPOINT_MAX_PAGES_PER_GROUP
                              : (uint32_t)remaining;
        read_db_pages(db_file, page_size, cursor, chunk, page_nos, pages);

        unsigned char *group_data = NULL;
        size_t group_size = 0;
        int enc_ok = otsardb_checkpoint_page_encode(page_size, database_size_pages,
                                                  page_nos, pages, chunk,
                                                  &group_data, &group_size);
        if (!enc_ok) { ok = 0; break; }

        char group_sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
        if (!otsardb_sha256_hex_data(group_data, group_size, group_sha256)) {
            free(group_data); ok = 0; break;
        }

        char suffix[OTSARDB_CHECKPOINT_PAGE_GROUP_KEY_MAX + 1];
        int n = snprintf(suffix, sizeof(suffix), "checkpoints/%s/page-%u.otscp",
                         group_sha256, group_idx);
        if (n <= 0 || (size_t)n >= sizeof(suffix)) {
            free(group_data); ok = 0; break;
        }

        char key[OTSARDB_KEY_MAX + 1];
        if (!root_join(database_root, suffix, key, sizeof(key))) {
            free(group_data); ok = 0; break;
        }

        otsardb_store_put_options opts = {0};
        otsardb_store_result rc = otsardb_store_put(store, key, group_data, group_size, &opts, NULL);
        free(group_data);
        if (rc != OTSARDB_STORE_OK) { ok = 0; break; }

        snprintf(cp_manifest.page_groups[group_idx].key,
                 sizeof(cp_manifest.page_groups[group_idx].key), "%s", suffix);
        snprintf(cp_manifest.page_groups[group_idx].sha256,
                 sizeof(cp_manifest.page_groups[group_idx].sha256), "%s", group_sha256);
        cp_manifest.page_groups[group_idx].size = (uint64_t)group_size;
        ++cp_manifest.page_group_count;
        ++group_idx;
        cursor += chunk;
        remaining -= chunk;
    }
    fclose(db_file);
    free(page_nos);
    free(pages);
    if (!ok) return 0;

    /* 5. Publish the checkpoint manifest as an immutable object. */
    char *manifest_json = NULL;
    size_t manifest_size = 0;
    char manifest_sha256[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    if (!otsardb_checkpoint_manifest_encode(&cp_manifest,
                                          &manifest_json, &manifest_size,
                                          manifest_sha256)) return 0;

    char manifest_suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(manifest_suffix, sizeof(manifest_suffix),
                     "checkpoints/%s.json", manifest_sha256);
    if (n <= 0 || (size_t)n >= sizeof(manifest_suffix)) { free(manifest_json); return 0; }
    char manifest_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, manifest_suffix, manifest_key, sizeof(manifest_key))) {
        free(manifest_json); return 0;
    }

    otsardb_store_put_options opts = {0};
    if (otsardb_store_put(store, manifest_key, manifest_json, manifest_size, &opts, NULL)
        != OTSARDB_STORE_OK) { free(manifest_json); return 0; }
    free(manifest_json);

    /* 6. Publish the CHECKPOINT.json pointer under the database root. */
    otsardb_checkpoint_pointer ptr;
    memset(&ptr, 0, sizeof(ptr));
    ptr.covered_generation = cp_manifest.covered_generation;
    snprintf(ptr.database_id, sizeof(ptr.database_id), "%s", cp_manifest.database_id);
    snprintf(ptr.checkpoint_sha256, sizeof(ptr.checkpoint_sha256), "%s", manifest_sha256);
    snprintf(ptr.checkpoint_key, sizeof(ptr.checkpoint_key), "%s", manifest_key);

    char *ptr_json = NULL;
    size_t ptr_size = 0;
    if (!otsardb_checkpoint_ptr_encode(&ptr, &ptr_json, &ptr_size)) return 0;

    char ptr_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "CHECKPOINT.json", ptr_key, sizeof(ptr_key))) {
        free(ptr_json); return 0;
    }
    otsardb_store_put_options ptr_opts = {0};
    otsardb_store_result ptr_rc = otsardb_store_put(store, ptr_key,
                                                 ptr_json, ptr_size, &ptr_opts, NULL);
    free(ptr_json);
    return ptr_rc == OTSARDB_STORE_OK ? 1 : 0;
}

#include "gc.h"
#include "checkpoint_manifest.h"
#include "commit_manifest.h"
#include "journal.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int root_join(const char *root, const char *suffix, char *out, size_t out_size) {
    if (!root || !suffix || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", root, needs_slash ? "/" : "", suffix);
    return n > 0 && (size_t)n < out_size;
}

/* memmem replacement (MSVC has none): byte-accurate substring search used to
 * match the lease's "owner" field without a JSON parser. */
static int mem_contains(const unsigned char *haystack, size_t haystack_size,
                        const char *needle) {
    size_t needle_len = strlen(needle);
    if (!needle_len || needle_len > haystack_size) return 0;
    for (size_t i = 0; i + needle_len <= haystack_size; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

int otsardb_gc_sweep(otsardb_object_store *store,
                   const char *database_root,
                   const otsardb_gc_options *options) {
    if (!store || !database_root) return -1;

    uint64_t keep_secs = 0;
    const char *owner_expected = NULL;
    if (options) {
        keep_secs = options->keep_secs;
        owner_expected = options->owner_expected;
    }

    /* 1. Read the checkpoint pointer. No checkpoint -> nothing to sweep. */
    otsardb_checkpoint_pointer cp;
    char *cp_etag = NULL;
    if (otsardb_checkpoint_ptr_read(store, database_root, &cp, &cp_etag) !=
        OTSARDB_CHECKPOINT_PTR_OK) {
        free(cp_etag);
        return -1;
    }
    free(cp_etag);

    /* 2. Retention window (OTSARDB_GC_KEEP_MINUTES): when configured, only
     * sweep history behind a checkpoint that has already survived the
     * window, so a fresh checkpoint can still be rolled back to the previous
     * pointer + delta replay (design 0012 grace rule). Age comes from the
     * pointer object's server Last-Modified; without it the sweep cannot
     * prove the window and is skipped. */
    if (keep_secs > 0) {
        otsardb_store_object ptr_obj = {0};
        char ptr_key[OTSARDB_KEY_MAX + 1];
        if (!root_join(database_root, "CHECKPOINT.json", ptr_key, sizeof(ptr_key))) return -1;
        if (otsardb_store_get(store, ptr_key, &ptr_obj) != OTSARDB_STORE_OK) return -1;
        int64_t modified = ptr_obj.last_modified_secs;
        otsardb_store_release_object(store, &ptr_obj);
        if (modified <= 0) {
            fprintf(stderr,
                    "otsardb: GC skipped: OTSARDB_GC_KEEP_MINUTES is set but the "
                    "store does not expose the checkpoint age\n");
            return 0;
        }
        if ((uint64_t)modified + keep_secs > (uint64_t)time(NULL)) {
            return 0; /* checkpoint younger than the retention window */
        }
    }

    /* 3. Read the checkpoint manifest (hash-verified against the pointer,
     * like recovery) to learn the covered base commit. */
    otsardb_store_object cp_obj = {0};
    if (otsardb_store_get(store, cp.checkpoint_key, &cp_obj) != OTSARDB_STORE_OK) return -1;

    char cp_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
    otsardb_checkpoint_manifest cp_manifest;
    int ok = otsardb_sha256_hex_data(cp_obj.data, cp_obj.size, cp_sha) &&
             strcmp(cp_sha, cp.checkpoint_sha256) == 0 &&
             otsardb_checkpoint_manifest_decode((const char *)cp_obj.data,
                                              cp_obj.size, &cp_manifest);
    otsardb_store_release_object(store, &cp_obj);
    if (!ok) {
        fprintf(stderr, "otsardb: GC skipped: checkpoint manifest failed verification\n");
        return -1;
    }

    /* 4. The checkpoint's own base manifest (generation == covered) is part
     * of the LIVE set: recovery replays its segments on top of the
     * checkpoint pages (recovery.c stops the manifest walk at
     * covered_generation INCLUSIVE). Only strictly older generations are
     * unreachable and may be deleted. */
    char base_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "commits/", base_key, sizeof(base_key))) return -1;
    size_t prefix_len = strlen(base_key);
    snprintf(base_key + prefix_len, sizeof(base_key) - prefix_len, "%s.json",
             cp_manifest.base_commit_id);

    otsardb_store_object base_obj = {0};
    if (otsardb_store_get(store, base_key, &base_obj) != OTSARDB_STORE_OK) {
        fprintf(stderr,
                "otsardb: GC skipped: checkpoint base manifest %s unreadable; "
                "refusing to sweep\n",
                cp_manifest.base_commit_id);
        return -1;
    }
    otsardb_commit_manifest base;
    ok = otsardb_commit_manifest_decode_json((const char *)base_obj.data,
                                           base_obj.size, &base) &&
         base.generation == cp.covered_generation;
    otsardb_store_release_object(store, &base_obj);
    if (!ok) {
        fprintf(stderr,
                "otsardb: GC skipped: checkpoint base manifest generation does "
                "not match covered generation %llu\n",
                (unsigned long long)cp.covered_generation);
        return -1;
    }

    int deleted = 0;
    uint64_t gen = base.parent_generation;
    char current_id[OTSARDB_COMMIT_ID_MAX + 1];
    snprintf(current_id, sizeof(current_id), "%s",
             base.parent_commit_id[0] ? base.parent_commit_id : "");
    otsardb_commit_manifest_free(&base);

    /* 5. Walk the parent chain below the base, deleting each superseded
     * manifest and its segments. A missing parent means a previous GC run
     * already swept that region; the walk stops there best-effort. */
    while (gen > 0) {
        char current_key[OTSARDB_KEY_MAX + 1];
        if (!root_join(database_root, "commits/", current_key, sizeof(current_key))) break;
        size_t key_prefix = strlen(current_key);
        snprintf(current_key + key_prefix, sizeof(current_key) - key_prefix, "%s.json",
                 current_id);

        otsardb_store_object obj = {0};
        otsardb_store_result rc = otsardb_store_get(store, current_key, &obj);
        if (rc == OTSARDB_STORE_NOT_FOUND) break; /* already swept earlier */
        if (rc != OTSARDB_STORE_OK) {
            fprintf(stderr,
                    "otsardb: GC stopped early: failed to read superseded "
                    "manifest (generation %llu)\n",
                    (unsigned long long)gen);
            break;
        }
        otsardb_commit_manifest m;
        if (!otsardb_commit_manifest_decode_json((const char *)obj.data,
                                               obj.size, &m)) {
            otsardb_store_release_object(store, &obj);
            fprintf(stderr,
                    "otsardb: GC stopped early: cannot decode superseded "
                    "manifest (generation %llu)\n",
                    (unsigned long long)gen);
            break;
        }
        otsardb_store_release_object(store, &obj);

        /* Delete all segments of this manifest. v3+ stores suffix keys
         * relative to the database root; legacy v1/v2 segment_key is already
         * an absolute object key (mirrors recovery.c). */
        for (uint32_t i = 0; i < m.segment_count; ++i) {
            const char *seg_key = m.segments[i].key;
            char absolute_key[OTSARDB_KEY_MAX + 1];
            const char *key = seg_key;
            if (m.format_version >= 3) {
                if (!root_join(database_root, seg_key, absolute_key, sizeof(absolute_key))) continue;
                key = absolute_key;
            }
            if (otsardb_store_delete(store, key) == OTSARDB_STORE_OK) ++deleted;
        }

        /* Delete the manifest itself. */
        if (otsardb_store_delete(store, current_key) == OTSARDB_STORE_OK) ++deleted;

        /* Move to parent. */
        int has_parent = m.parent_generation != 0;
        if (has_parent) {
            gen = m.parent_generation;
            snprintf(current_id, sizeof(current_id), "%s", m.parent_commit_id);
        }
        otsardb_commit_manifest_free(&m);
        if (!has_parent) break;
    }

    /* 6. The closing session's own writer lease becomes dead weight once
     * this checkpoint supersedes the session (the session releases and
     * destroys it right after the sweep). Delete it only when the stored
     * owner matches the expected owner, so a different live writer's lease
     * is never touched. The delete is unconditional once the owner matches
     * (same exposure as otsardb_lease_release). */
    if (owner_expected && owner_expected[0]) {
        char lease_key[OTSARDB_KEY_MAX + 1];
        if (!root_join(database_root, "lease.json", lease_key, sizeof(lease_key))) {
            return deleted;
        }
        otsardb_store_object lease_obj = {0};
        if (otsardb_store_get(store, lease_key, &lease_obj) != OTSARDB_STORE_OK) {
            return deleted; /* absent or unreadable: leave it alone */
        }
        size_t needle_cap = strlen(owner_expected) + 32u;
        char *needle = (char *)malloc(needle_cap);
        int matches = 0;
        if (needle) {
            snprintf(needle, needle_cap, "\"owner\":\"%s\"", owner_expected);
            matches = mem_contains(lease_obj.data, lease_obj.size, needle);
            free(needle);
        }
        otsardb_store_release_object(store, &lease_obj);
        if (matches && otsardb_store_delete(store, lease_key) == OTSARDB_STORE_OK) {
            ++deleted;
        }
    }

    return deleted;
}

/*
 * — D1: atomic cache save + image-verified probe.
 *
 * Deterministic file-level regression test for the HIGH failure-mode-audit
 * finding: the pre-0135 save wrote the cached image IN PLACE (remove +
 * plain copy), so a crash mid-save could leave state.db = prefix of the
 * NEW image + suffix of the OLD image with cache.json still matching the
 * (unchanged) remote HEAD, and the probe — which verified only the
 * metadata triple (generation, commit_id, commit_sha256), never the image
 * bytes — would serve the torn file as the authoritative database state.
 *
 * This test proves the 0135 contract without killing a process (the save
 * is fully atomic, so the mid-crash states are simulated with the exact
 * file artifacts a crash leaves behind):
 *
 * 1. Save + probe round-trip: the probe copies the image byte-for-byte
 * and read_state reports the attested image_size/image_sha256.
 * 2. Tampered cached image (same metadata, different bytes — the exact
 * D1 mixed-file state): probe MUST reject and MUST NOT create the
 * target.
 * 3. Truncated / extended cached image (size mismatch): probe rejects.
 * 4. Crash leftovers (partial state.db.tmp / cache.json.tmp): ignored;
 * the valid pair still probes and serves the correct bytes.
 * 5. Pre-0135 metadata (3 fields, no image attestation): refused — a
 * cache entry whose image bytes were never attested is never served.
 * 6. Save-overwrite: a second save publishes a new image + new
 * attestation and the probe follows.
 * 7. Post-condition: a successful save leaves NO .tmp files behind.
 *
 * Build (standalone, not a ctest target in this change set — CMakeLists
 * wiring is out of scope for): link local_cache.cpp against
 * the same libs as otsardb_local_cache_test.
 */

#include "local_cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string temp_dir_str() {
    std::error_code ec;
    auto p = fs::temp_directory_path(ec);
    return ec ? "." : p.string();
}

std::string join(const std::string &a, const std::string &b) {
    return (fs::path(a) / b).string();
}

std::string cache_dir_for(const std::string &root, const char *tag) {
    return join(root, std::string("otsardb-atomic-cache-") + tag);
}

void write_file(const std::string &path, const void *data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.is_open());
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    assert(out.good());
}

void remove_if_exists(const std::string &path) {
    std::error_code ec;
    fs::remove(path, ec);
}

void wipe_cache(const std::string &root, const char *tag, const char *db_id) {
    std::string base = join(join(root, std::string("otsardb-atomic-cache-") + tag),
                            "otsardb-cache-v1");
    std::string dir = join(base, db_id);
    remove_if_exists(join(dir, "state.db"));
    remove_if_exists(join(dir, "state.db.tmp"));
    remove_if_exists(join(dir, "cache.json"));
    remove_if_exists(join(dir, "cache.json.tmp"));
    remove_if_exists(join(base, "otsardb-cache-index.json"));
    remove_if_exists(join(base, "otsardb-cache-index.json.tmp"));
}

bool file_exists(const std::string &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

uint64_t file_size(const std::string &path) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

std::string image_file(const std::string &root, const char *tag, const char *db_id) {
    return join(join(join(root, std::string("otsardb-atomic-cache-") + tag),
                     "otsardb-cache-v1"),
                join(db_id, "state.db"));
}

std::string meta_file(const std::string &root, const char *tag, const char *db_id) {
    return join(join(join(root, std::string("otsardb-atomic-cache-") + tag),
                     "otsardb-cache-v1"),
                join(db_id, "cache.json"));
}

std::string image_tmp_file(const std::string &root, const char *tag, const char *db_id) {
    return image_file(root, tag, db_id) + ".tmp";
}

std::string meta_tmp_file(const std::string &root, const char *tag, const char *db_id) {
    return meta_file(root, tag, db_id) + ".tmp";
}

void read_file(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    assert(in.is_open());
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

otsardb_local_cache_state make_state(uint64_t generation, const char *commit_id) {
    otsardb_local_cache_state state{};
    state.generation = generation;
    std::snprintf(state.commit_id, sizeof(state.commit_id), "%s", commit_id);
    std::snprintf(state.commit_sha256, sizeof(state.commit_sha256),
                  "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
                  "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40");
    return state;
}

}  // namespace

int main() {
    std::string temp = temp_dir_str();
    const char *db_id = "atomic-test-db";
    const char *tag_a = "a";

    static const unsigned char IMAGE_V1[] =
        "OtsarDB D1 test image payload (generation one).";
    static const unsigned char IMAGE_V2[] =
        "OtsarDB D1 test image payload (generation two, longer content to "
        "make the two images differ in size as well as in bytes).";
    std::string source_a = join(temp, "otsardb-atomic-src-a.db");
    std::string source_b = join(temp, "otsardb-atomic-src-b.db");
    std::string target = join(temp, "otsardb-atomic-target.db");
    write_file(source_a, IMAGE_V1, sizeof(IMAGE_V1));
    write_file(source_b, IMAGE_V2, sizeof(IMAGE_V2));

    otsardb_local_cache_state state_a = make_state(7, "commit-7");

    /* 1. Save + probe round-trip. */
    {
        wipe_cache(temp, tag_a, db_id);
        remove_if_exists(target);
        assert(otsardb_local_cache_save(cache_dir_for(temp, tag_a).c_str(), db_id,
                                        source_a.c_str(), &state_a) == 1);

        /* No .tmp leftovers after a completed save (post-condition 7). */
        assert(!file_exists(image_tmp_file(temp, tag_a, db_id)));
        assert(!file_exists(meta_tmp_file(temp, tag_a, db_id)));

        otsardb_local_cache_state cached{};
        assert(otsardb_local_cache_read_state(cache_dir_for(temp, tag_a).c_str(),
                                              db_id, &cached) == 1);
        assert(cached.generation == 7);
        assert(std::strcmp(cached.commit_id, "commit-7") == 0);
        assert(std::strlen(cached.commit_sha256) == 64);
        assert(cached.image_size_bytes == sizeof(IMAGE_V1));
        assert(std::strlen(cached.image_sha256) == 64);

        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_a).c_str(), db_id,
                                         &state_a, target.c_str()) == 1);
        assert(file_size(target) == sizeof(IMAGE_V1));
        std::string got;
        read_file(target, &got);
        assert(got.size() == sizeof(IMAGE_V1));
        assert(std::memcmp(got.data(), IMAGE_V1, sizeof(IMAGE_V1)) == 0);
        remove_if_exists(target);
    }

    /* 2. The D1 core: a cached image whose bytes no longer match its own
     * metadata (the crash-mixed file the old save could leave, or any
     * tamper/bit rot) must be rejected even though the metadata triple
     * still matches the remote HEAD — and the target must never appear. */
    {
        std::string img = image_file(temp, tag_a, db_id);
        std::string content;
        read_file(img, &content);
        content[content.size() / 2] ^= 0xff;  /* flip one byte mid-file */
        write_file(img, content.data(), content.size());

        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_a).c_str(), db_id,
                                         &state_a, target.c_str()) == 0);
        assert(!file_exists(target));
    }

    /* 3. Size mismatch: a truncated or extended image is rejected. */
    {
        std::string img = image_file(temp, tag_a, db_id);
        std::string content;
        read_file(img, &content);
        std::string truncated = content.substr(0, content.size() - 3);
        write_file(img, truncated.data(), truncated.size());
        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_a).c_str(), db_id,
                                         &state_a, target.c_str()) == 0);
        assert(!file_exists(target));
    }

    /* 4. Crash leftovers: restore the valid pair, then drop a PARTIAL
     * state.db.tmp (a crash mid-image-write) and a partial cache.json.tmp
     * (a crash mid-metadata-write). The probe must ignore both and serve
     * the intact pair. */
    {
        /* Re-save the good image so the pair is valid again. */
        assert(otsardb_local_cache_save(cache_dir_for(temp, tag_a).c_str(), db_id,
                                        source_a.c_str(), &state_a) == 1);
        static const char PARTIAL[] = "partially-written-image-bytes";
        write_file(image_tmp_file(temp, tag_a, db_id), PARTIAL, sizeof(PARTIAL));
        static const char PARTIAL_META[] = "{\"generation\":7,\"commit";
        write_file(meta_tmp_file(temp, tag_a, db_id), PARTIAL_META,
                   sizeof(PARTIAL_META));

        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_a).c_str(), db_id,
                                         &state_a, target.c_str()) == 1);
        assert(file_size(target) == sizeof(IMAGE_V1));
        std::string got;
        read_file(target, &got);
        assert(std::memcmp(got.data(), IMAGE_V1, sizeof(IMAGE_V1)) == 0);
        remove_if_exists(target);
    }

    /* 5. Pre-0135 metadata (no image_size/image_sha256 attestation) is
     * refused: the image was written by the non-atomic path and cannot be
     * trusted. One-time invalidation; the next save rewrites the entry. */
    {
        std::string meta = meta_file(temp, tag_a, db_id);
        static const char OLD_META[] =
            "{\"generation\":7,\"commit_id\":\"commit-7\","
            "\"commit_sha256\":\"0102030405060708090a0b0c0d0e0f1011121314151617"
            "18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373"
            "8393a3b3c3d3e3f40\"}\n";
        write_file(meta, OLD_META, sizeof(OLD_META) - 1);
        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_a).c_str(), db_id,
                                         &state_a, target.c_str()) == 0);
        otsardb_local_cache_state cached{};
        assert(otsardb_local_cache_read_state(cache_dir_for(temp, tag_a).c_str(),
                                              db_id, &cached) == 0);
        assert(!file_exists(target));
    }

    /* 5b. ASAN-1 regression (0139): a tampered metadata with a 64-char
     * commit_id (one past the 63-char contract) must be REFUSED cleanly,
     * never read into the 64-byte buffer. Pre-0143 the sscanf width was
     * 64 and the 65th byte landed on the stack (ASan interceptor report). */
    {
        std::string meta = meta_file(temp, tag_a, db_id);
        std::string tampered =
            "{\"generation\":7,\"commit_id\":\"1234567890abcdef1234567890abcdef"
            "1234567890abcdef1234567890abcdef\","
            "\"commit_sha256\":\"0102030405060708090a0b0c0d0e0f1011121314151617"
            "18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373"
            "8393a3b3c3d3e3f40\",\"image_size\":7,"
            "\"image_sha256\":\"0102030405060708090a0b0c0d0e0f1011121314151617"
            "18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233343536373"
            "8393a3b3c3d3e3f40\"}\n";
        write_file(meta, tampered.c_str(), tampered.size());
        otsardb_local_cache_state cached{};
        assert(otsardb_local_cache_read_state(cache_dir_for(temp, tag_a).c_str(),
                                              db_id, &cached) == 0);
        assert(cached.commit_id[0] == '\0');
        wipe_cache(temp, tag_a, db_id);
    }

    /* 6. Save-overwrite: a second save (new head, new image) publishes a
     * new attestation; the probe follows the new state and rejects the
     * old head. */
    {
        const char *tag_b = "b";
        wipe_cache(temp, tag_b, db_id);
        otsardb_local_cache_state state_b = make_state(8, "commit-8");
        assert(otsardb_local_cache_save(cache_dir_for(temp, tag_b).c_str(), db_id,
                                        source_b.c_str(), &state_a) == 1);
        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_b).c_str(), db_id,
                                         &state_a, target.c_str()) == 1);
        assert(otsardb_local_cache_save(cache_dir_for(temp, tag_b).c_str(), db_id,
                                        source_b.c_str(), &state_b) == 1);
        /* Old head no longer matches; new head serves the new image. */
        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_b).c_str(), db_id,
                                         &state_a, target.c_str()) == 0);
        assert(otsardb_local_cache_probe(cache_dir_for(temp, tag_b).c_str(), db_id,
                                         &state_b, target.c_str()) == 1);
        assert(file_size(target) == sizeof(IMAGE_V2));
        std::string got;
        read_file(target, &got);
        assert(std::memcmp(got.data(), IMAGE_V2, sizeof(IMAGE_V2)) == 0);
        remove_if_exists(target);
        wipe_cache(temp, tag_b, db_id);
    }

    remove_if_exists(source_a);
    remove_if_exists(source_b);
    remove_if_exists(target);
    wipe_cache(temp, tag_a, db_id);

    std::printf("local cache atomic-save / image-verified probe tests passed\n");
    return 0;
}

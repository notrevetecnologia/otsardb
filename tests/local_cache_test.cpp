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

void write_file(const std::string &path, const void *data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.is_open());
    out.write(static_cast<const char *>(data),
              static_cast<std::streamsize>(size));
}

void remove_if_exists(const std::string &path) {
    std::error_code ec;
    fs::remove(path, ec);
}

}  // namespace

int main() {
    std::string temp = temp_dir_str();
    std::string cache_dir = join(temp, "otsardb-cache-ut");
    const char *database_id = "test-db-id";

    // Clean stale state.
    remove_if_exists(join(cache_dir, "otsardb-cache-v1/test-db-id/cache.json"));
    remove_if_exists(join(cache_dir, "otsardb-cache-v1/test-db-id/state.db"));

    static const unsigned char IMAGE[] = "OtsarDB cache test image payload.";
    std::string image_file = join(temp, "otsardb-cache-image.db");
    write_file(image_file, IMAGE, sizeof(IMAGE));

    // --- Save ---
    otsardb_local_cache_state state{};
    state.generation = 42;
    std::snprintf(state.commit_id, sizeof(state.commit_id), "commit-42");
    std::snprintf(state.commit_sha256, sizeof(state.commit_sha256),
                  "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
                  "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40");
    assert(otsardb_local_cache_save(cache_dir.c_str(), database_id,
                                  image_file.c_str(), &state));

    // --- Probe: match ---
    std::string target = join(temp, "otsardb-cache-target.db");
    remove_if_exists(target);
    assert(otsardb_local_cache_probe(cache_dir.c_str(), database_id,
                                    &state, target.c_str()) == 1);
    assert(fs::file_size(target) == sizeof(IMAGE));
    remove_if_exists(target);

    // --- Probe: wrong generation ---
    otsardb_local_cache_state wrong_gen = state;
    wrong_gen.generation = 99;
    assert(otsardb_local_cache_probe(cache_dir.c_str(), database_id,
                                    &wrong_gen, target.c_str()) == 0);

    // --- Probe: wrong commit_id ---
    otsardb_local_cache_state wrong_id = state;
    wrong_id.commit_id[0] = 'X';
    assert(otsardb_local_cache_probe(cache_dir.c_str(), database_id,
                                    &wrong_id, target.c_str()) == 0);

    // --- Probe: wrong sha ---
    otsardb_local_cache_state wrong_sha = state;
    wrong_sha.commit_sha256[0] = 'f';
    assert(otsardb_local_cache_probe(cache_dir.c_str(), database_id,
                                    &wrong_sha, target.c_str()) == 0);

    // --- Save: source does not exist ---
    std::string missing = join(temp, "otsardb-cache-nonexistent.db");
    assert(otsardb_local_cache_save(cache_dir.c_str(), database_id,
                                  missing.c_str(), &state) == 0);

    // --- Corrupted metadata ---
    std::string meta = join(cache_dir, "otsardb-cache-v1/test-db-id/cache.json");
    write_file(meta, "bogus", 5);
    assert(otsardb_local_cache_probe(cache_dir.c_str(), database_id,
                                    &state, target.c_str()) == 0);

    // Cleanup.
    remove_if_exists(meta);
    remove_if_exists(join(cache_dir, "otsardb-cache-v1/test-db-id/state.db"));
    remove_if_exists(target);
    remove_if_exists(image_file);

    std::printf("manifest-aware local cache tests passed\n");
    return 0;
}

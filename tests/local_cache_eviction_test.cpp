#include "local_cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void set_cache_max_mb(const char *value) {
#if defined(_WIN32)
    _putenv_s("OTSARDB_CACHE_MAX_MB", value);
#else
    setenv("OTSARDB_CACHE_MAX_MB", value, 1);
#endif
}

std::string temp_dir_str() {
    std::error_code ec;
    auto p = fs::temp_directory_path(ec);
    return ec ? "." : p.string();
}

std::string join(const std::string &a, const std::string &b) {
    return (fs::path(a) / b).string();
}

void remove_all(const std::string &path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

bool exists(const std::string &path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

void write_file(const std::string &path, const void *data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.is_open());
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    assert(out.good());
}

void write_pattern_file(const std::string &path, size_t size, unsigned seed) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::vector<char> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>((seed * 31 + i * 7) % 251);
    }
    write_file(path, bytes.data(), bytes.size());
}

bool file_equals(const std::string &path, const std::string &expected) {
    std::ifstream in(path, std::ios::binary);
    std::ifstream ref(expected, std::ios::binary);
    if (!in || !ref) return false;
    std::vector<char> a((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    std::vector<char> b((std::istreambuf_iterator<char>(ref)),
                        std::istreambuf_iterator<char>());
    return a == b;
}

void make_state(otsardb_local_cache_state *s, uint64_t generation) {
    std::memset(s, 0, sizeof(*s));
    s->generation = generation;
    std::snprintf(s->commit_id, sizeof(s->commit_id), "commit-%llu",
                  static_cast<unsigned long long>(generation));
    std::snprintf(s->commit_sha256, sizeof(s->commit_sha256),
                  "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
                  "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40");
}

constexpr char kProbeHex[] =
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
    "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40";

/* probe must succeed and the copied image must byte-match the source. */
void expect_probe_hit(const std::string &cache_dir, const char *db_id,
                      const otsardb_local_cache_state *state,
                      const std::string &src) {
    std::string target = join(temp_dir_str(), "otsardb-cache-evict-target.db");
    remove_all(target);
    assert(otsardb_local_cache_probe(cache_dir.c_str(), db_id, state,
                                   target.c_str()) == 1);
    assert(file_equals(target, src));
    remove_all(target);
}

}  // namespace

int main() {
    std::string temp = temp_dir_str();
    std::string cache_dir = join(temp, "otsardb-cache-evict-ut");
    std::string src_dir = join(temp, "otsardb-cache-evict-src");
    remove_all(cache_dir);
    remove_all(src_dir);

    // Three source images with distinct sizes: 2048, 4096, 6144 bytes.
    // OTSARDB_CACHE_MAX_MB=0.01 -> 10485-byte budget (fractional MB).
    const std::vector<std::string> dbs = {"db-a", "db-b", "db-c"};
    const std::vector<uint64_t> sizes = {2048, 4096, 6144};
    const std::vector<uint64_t> gens = {11, 22, 33};
    std::vector<std::string> srcs;
    for (size_t i = 0; i < dbs.size(); ++i) {
        std::string src = join(src_dir, dbs[i] + ".db");
        write_pattern_file(src, sizes[i], static_cast<unsigned>(i + 1));
        srcs.push_back(src);
    }

    set_cache_max_mb("0.01");
    std::vector<otsardb_local_cache_state> states(3);
    for (size_t i = 0; i < dbs.size(); ++i) {
        make_state(&states[i], gens[i]);
    }

    std::string index_file = join(cache_dir, "otsardb-cache-v1/otsardb-cache-index.json");

    // --- Scenario 1: budget overrun evicts the OLDEST image, and the
    // just-saved image is never evicted by its own save.
    assert(otsardb_local_cache_save(cache_dir.c_str(), dbs[0].c_str(),
                                  srcs[0].c_str(), &states[0]));
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(otsardb_local_cache_save(cache_dir.c_str(), dbs[1].c_str(),
                                  srcs[1].c_str(), &states[1]));
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(otsardb_local_cache_save(cache_dir.c_str(), dbs[2].c_str(),
                                  srcs[2].c_str(), &states[2]));
    // Total 2048+4096+6144 = 12288 > 10485 -> db-a (oldest) evicted.
    assert(!exists(join(cache_dir, "otsardb-cache-v1/db-a/state.db")));
    assert(!exists(join(cache_dir, "otsardb-cache-v1/db-a/cache.json")));
    assert(otsardb_local_cache_probe(cache_dir.c_str(), dbs[0].c_str(),
                                   &states[0], join(temp, "x.db").c_str()) == 0);
    remove_all(join(temp, "x.db"));
    // Survivors still load with byte-identical content.
    expect_probe_hit(cache_dir, dbs[1].c_str(), &states[1], srcs[1]);
    expect_probe_hit(cache_dir, dbs[2].c_str(), &states[2], srcs[2]);

    // Re-save db-a: total 10240+2048 = 12288 > 10485 -> db-b (now oldest)
    // evicted; db-a's own save survives it.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(otsardb_local_cache_save(cache_dir.c_str(), dbs[0].c_str(),
                                  srcs[0].c_str(), &states[0]));
    assert(!exists(join(cache_dir, "otsardb-cache-v1/db-b/state.db")));
    expect_probe_hit(cache_dir, dbs[0].c_str(), &states[0], srcs[0]);
    expect_probe_hit(cache_dir, dbs[2].c_str(), &states[2], srcs[2]);

    // --- Scenario 2: index deleted -> rebuilt from the directory scan on
    // the next save; eviction still works from the rebuilt index.
    assert(exists(index_file));
    remove_all(index_file);
    assert(!exists(index_file));
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    otsardb_local_cache_state new_b = states[1];
    new_b.generation = 99;
    new_b.commit_id[0] = 'N';
    new_b.commit_sha256[0] = 'f';
    assert(otsardb_local_cache_save(cache_dir.c_str(), dbs[1].c_str(),
                                  srcs[1].c_str(), &new_b));
    // Rebuilt from disk: db-a, db-c tracked; db-b updated -> total
    // 2048+4096+6144 = 12288 > 10485 -> db-c (oldest by mtime) evicted.
    assert(exists(index_file));
    // Regression guard: the rebuilt entries must carry
    // real save times. On Linux the file-clock epoch conversion used to
    // clamp every mtime to 0 (saved_at:0), which degenerated the LRU to
    // the db-id tie-break and evicted db-a instead of db-c.
    {
        std::ifstream in(index_file, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        assert(!json.empty());
        assert(json.find("\"saved_at\":0") == std::string::npos);
    }
    assert(!exists(join(cache_dir, "otsardb-cache-v1/db-c/state.db")));
    expect_probe_hit(cache_dir, dbs[1].c_str(), &new_b, srcs[1]);
    expect_probe_hit(cache_dir, dbs[0].c_str(), &states[0], srcs[0]);

    // --- Scenario 3: unlimited (0) disables eviction entirely.
    set_cache_max_mb("0");
    assert(otsardb_local_cache_save(cache_dir.c_str(), dbs[2].c_str(),
                                  srcs[2].c_str(), &states[2]));
    assert(exists(join(cache_dir, "otsardb-cache-v1/db-c/state.db")));
    assert(exists(join(cache_dir, "otsardb-cache-v1/db-b/state.db")));
    assert(exists(join(cache_dir, "otsardb-cache-v1/db-a/state.db")));

    set_cache_max_mb("512");
    remove_all(cache_dir);
    remove_all(src_dir);
    std::printf("local cache eviction tests passed\n");
    return 0;
}

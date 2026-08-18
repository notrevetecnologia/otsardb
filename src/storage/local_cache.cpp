#include "local_cache.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <openssl/evp.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char *kCacheVersionDir = "otsardb-cache-v1";
constexpr const char *kIndexName = "otsardb-cache-index.json";
constexpr uint64_t kDefaultMaxBytes = 512ull * 1024 * 1024;

struct cache_entry {
    std::string db_id;
    std::string path;  // image path relative to the cache version dir
    uint64_t size_bytes = 0;
    uint64_t saved_at_ms = 0;
};

std::string cache_root_path(const char *cache_dir) {
    std::string root = cache_dir ? cache_dir : "";
    if (root.empty()) return "";
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + kCacheVersionDir;
}

std::string cache_db_dir(const char *cache_dir, const char *database_id) {
    return cache_root_path(cache_dir) + "/" + std::string(database_id);
}

std::string metadata_path(const char *cache_dir, const char *database_id) {
    return cache_db_dir(cache_dir, database_id) + "/cache.json";
}

std::string image_path(const char *cache_dir, const char *database_id) {
    return cache_db_dir(cache_dir, database_id) + "/state.db";
}

std::string index_path(const char *cache_dir) {
    return cache_root_path(cache_dir) + "/" + kIndexName;
}

std::string tmp_path(const std::string &dest) {
    return dest + ".tmp";
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

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

/* File mtime as unix epoch milliseconds (1970). The standard does not fix
 * std::filesystem::file_time_type's epoch, and implementations disagree:
 * MSVC uses FILETIME (1601-01-01) and libstdc++ (GCC 12+, OBSERVED on
 * Ubuntu 24.04 / GCC 13) uses an epoch ~1766 whose now() reads back as a
 * NEGATIVE tick count — a hardcoded per-platform shift is fragile, and
 * clamping negatives to 0 collapsed every rebuilt entry to saved_at=0
 * (Linux eviction ordered by db id, not by save time).
 * Derive the value epoch-independently instead: measure the file's age in
 * the file clock (the same clock the save path stamps with) and subtract
 * that age from the current system time. */
uint64_t mtime_ms(const std::string &path) {
    std::error_code ec;
    auto ft = fs::last_write_time(path, ec);
    if (ec) return 0;
    auto age = fs::file_time_type::clock::now() - ft;
    auto sys_mtime = std::chrono::system_clock::now() - age;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  sys_mtime.time_since_epoch())
                  .count();
    return ms > 0 ? static_cast<uint64_t>(ms) : 0;
}

/* Monotonic guard: consecutive saves within the same millisecond still get
 * strictly increasing timestamps so LRU order is deterministic. */
uint64_t next_saved_at(uint64_t now) {
    static uint64_t last = 0;
    if (now <= last) now = last + 1;
    last = now;
    return now;
}

bool read_metadata(const std::string &path,
                   otsardb_local_cache_state *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::string json;
    json.reserve(512);
    std::string line;
    while (std::getline(in, line)) { json += line; }
    in.close();

    char id[OTSARDB_LOCAL_CACHE_COMMIT_ID_MAX + 1] = {0};
    char sha[OTSARDB_LOCAL_CACHE_SHA256_MAX + 1] = {0};
    char img_sha[OTSARDB_LOCAL_CACHE_SHA256_MAX + 1] = {0};
    unsigned long long gen = 0;
    unsigned long long img_size = 0;

    /* (D1): the metadata must attest the IMAGE BYTES
     * (image_size + image_sha256). A pre-0135 metadata file without those
     * fields is refused: its image was saved by the non-atomic path and
     * could be a crash-mixed old/new file, so it must never be served or
     * used as a delta base (one-time invalidation; the next save rewrites
     * the entry in the new format). */
    /* (ASAN-1): commit_id is a 63-char contract field in a
     * 64-byte buffer, so its width is 63 (a 64-char tampered value must
     * fail the scan, never overflow the buffer). The sha256 fields are
     * 64-char contracts in 65-byte buffers: width 64 fits exactly and the
     * strlen == 64 check below verifies completeness. */
    int n = std::sscanf(json.c_str(),
                        R"({"generation":%llu,"commit_id":"%63[^"]","commit_sha256":"%64[^"]","image_size":%llu,"image_sha256":"%64[^"]"})",
                        &gen, id, sha, &img_size, img_sha);
    if (n != 5) return false;
    if (gen == 0 || id[0] == '\0' || std::strlen(sha) != 64 ||
        std::strlen(img_sha) != 64 || img_size == 0) {
        return false;
    }

    out->generation = static_cast<uint64_t>(gen);
    std::snprintf(out->commit_id, sizeof(out->commit_id), "%s", id);
    std::snprintf(out->commit_sha256, sizeof(out->commit_sha256), "%s", sha);
    out->image_size_bytes = static_cast<uint64_t>(img_size);
    std::snprintf(out->image_sha256, sizeof(out->image_sha256), "%s", img_sha);
    return true;
}

/* Flush the stream to the OS and fsync the underlying descriptor to disk
 * (_commit on Windows). Used before the atomic renames so a power loss
 * cannot leave a renamed-but-not-durable file. */
bool fsync_stream(FILE *stream) {
    if (std::fflush(stream) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(stream)) == 0;
#else
    return fsync(fileno(stream)) == 0;
#endif
}

bool write_metadata(const std::string &path,
                    const otsardb_local_cache_state *state) {
    std::string tmp = tmp_path(path);
    try {
        std::filesystem::create_directories(fs::path(tmp).parent_path());
    } catch (...) {
        return false;
    }

    FILE *out = std::fopen(tmp.c_str(), "wb");
    if (!out) return false;

    bool ok = std::fprintf(out,
                           "{\"generation\":%llu,\"commit_id\":\"%s\","
                           "\"commit_sha256\":\"%s\",\"image_size\":%llu,"
                           "\"image_sha256\":\"%s\"}\n",
                           (unsigned long long)state->generation,
                           state->commit_id,
                           state->commit_sha256,
                           (unsigned long long)state->image_size_bytes,
                           state->image_sha256) > 0 &&
              fsync_stream(out);
    if (std::fclose(out) != 0) ok = false;

    if (!ok) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

/* ---- image integrity helpers ---- */

/* Streaming SHA-256 of a file. Returns 0 on any I/O failure. */
bool sha256_file(const std::string &path,
                 uint64_t *out_size,
                 char out_hex[OTSARDB_LOCAL_CACHE_SHA256_MAX + 1]) {
    FILE *in = std::fopen(path.c_str(), "rb");
    if (!in) return false;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx != nullptr && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    uint64_t total = 0;
    unsigned char buf[1u << 18];
    if (ok) {
        for (;;) {
            size_t n = std::fread(buf, 1, sizeof(buf), in);
            if (n > 0) {
                if (EVP_DigestUpdate(ctx, buf, n) != 1) { ok = false; break; }
                total += n;
            }
            if (n < sizeof(buf)) {
                if (std::ferror(in)) ok = false;
                break;
            }
        }
    }
    if (ok && out_size) *out_size = total;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (ok && EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1 &&
        digest_len == 32) {
        static const char HEX[] = "0123456789abcdef";
        for (unsigned int i = 0; i < digest_len; ++i) {
            out_hex[i * 2] = HEX[(digest[i] >> 4) & 0x0f];
            out_hex[i * 2 + 1] = HEX[digest[i] & 0x0f];
        }
        out_hex[64] = '\0';
    } else {
        ok = false;
    }

    EVP_MD_CTX_free(ctx);
    std::fclose(in);
    return ok;
}

/* Copy src to dst byte-for-byte, hashing the SOURCE bytes as they stream,
 * then verify size + SHA-256 against the attested values. On any mismatch
 * dst is removed (never left behind) and 0 is returned. This is the probe's
 * verify-while-copy: the cached image is read exactly once. */
bool copy_file_verified(const std::string &src,
                        const std::string &dst,
                        uint64_t expected_size,
                        const char *expected_sha256_hex) {
    FILE *in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx != nullptr && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    uint64_t total = 0;
    unsigned char buf[1u << 18];
    if (ok) {
        for (;;) {
            size_t n = std::fread(buf, 1, sizeof(buf), in);
            if (n > 0) {
                if (EVP_DigestUpdate(ctx, buf, n) != 1 ||
                    std::fwrite(buf, 1, n, out) != n) {
                    ok = false;
                    break;
                }
                total += n;
            }
            if (n < sizeof(buf)) {
                if (std::ferror(in)) ok = false;
                break;
            }
        }
    }
    if (std::fclose(out) != 0) ok = false;
    std::fclose(in);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (ok && EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1 &&
        digest_len == 32) {
        static const char HEX[] = "0123456789abcdef";
        char actual[OTSARDB_LOCAL_CACHE_SHA256_MAX + 1];
        for (unsigned int i = 0; i < digest_len; ++i) {
            actual[i * 2] = HEX[(digest[i] >> 4) & 0x0f];
            actual[i * 2 + 1] = HEX[digest[i] & 0x0f];
        }
        actual[64] = '\0';
        ok = total == expected_size &&
             std::strlen(expected_sha256_hex) == 64 &&
             std::strcmp(actual, expected_sha256_hex) == 0;
    } else {
        ok = false;
    }
    EVP_MD_CTX_free(ctx);

    if (!ok) {
        std::error_code ec;
        fs::remove(dst, ec);
    }
    return ok;
}

/* Copy src to dst (a fresh temp path) and fsync it, WITHOUT publishing it
 * under its final name. The caller renames dst into place afterwards so the
 * final path only ever appears complete (same-directory atomic rename). */
bool copy_file_fsync_tmp(const std::string &src, const std::string &dst) {
    FILE *in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }

    unsigned char buf[1u << 18];
    bool ok = true;
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), in);
        if (n > 0 && std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
        if (n < sizeof(buf)) {
            if (std::ferror(in)) ok = false;
            break;
        }
    }
    if (ok) ok = fsync_stream(out);
    if (std::fclose(out) != 0) ok = false;
    std::fclose(in);

    if (!ok) {
        std::error_code ec;
        fs::remove(dst, ec);
    }
    return ok;
}

/* ---- cache index ---- */

std::string json_escape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void skip_ws(const std::string &s, size_t *pos) {
    while (*pos < s.size() &&
           (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == '\r' ||
            s[*pos] == '\n')) {
        ++(*pos);
    }
}

bool expect_char(const std::string &s, size_t *pos, char c) {
    skip_ws(s, pos);
    if (*pos >= s.size() || s[*pos] != c) return false;
    ++(*pos);
    return true;
}

bool read_json_string(const std::string &s, size_t *pos, std::string *out) {
    if (!expect_char(s, pos, '"')) return false;
    std::string v;
    while (*pos < s.size()) {
        char c = s[*pos];
        if (c == '\\') {
            if (*pos + 1 >= s.size()) return false;
            v.push_back(s[*pos + 1]);
            *pos += 2;
        } else if (c == '"') {
            ++(*pos);
            *out = std::move(v);
            return true;
        } else {
            v.push_back(c);
            ++(*pos);
        }
    }
    return false;
}

bool read_json_number(const std::string &s, size_t *pos, uint64_t *out) {
    skip_ws(s, pos);
    if (*pos >= s.size() ||
        !std::isdigit(static_cast<unsigned char>(s[*pos]))) {
        return false;
    }
    uint64_t v = 0;
    while (*pos < s.size() &&
           std::isdigit(static_cast<unsigned char>(s[*pos]))) {
        v = v * 10 + static_cast<uint64_t>(s[*pos] - '0');
        ++(*pos);
    }
    *out = v;
    return true;
}

/* Parses the exact format written by write_index_json:
 * {"format":1,"entries":[{"db":"a","path":"a/state.db","size_bytes":1,"saved_at":2},...]}
 * Any structural mismatch returns false (caller rebuilds by scanning). */
bool parse_index_json(const std::string &json, std::vector<cache_entry> *out) {
    out->clear();
    size_t pos = 0;
    std::string key;
    if (!expect_char(json, &pos, '{')) return false;
    if (!read_json_string(json, &pos, &key) || key != "format") return false;
    if (!expect_char(json, &pos, ':')) return false;
    uint64_t format = 0;
    if (!read_json_number(json, &pos, &format) || format != 1) return false;
    if (!expect_char(json, &pos, ',')) return false;
    if (!read_json_string(json, &pos, &key) || key != "entries") return false;
    if (!expect_char(json, &pos, ':') || !expect_char(json, &pos, '[')) {
        return false;
    }
    for (;;) {
        skip_ws(json, &pos);
        if (pos < json.size() && json[pos] == ']') {
            ++pos;
            break;
        }
        if (!expect_char(json, &pos, '{')) return false;
        cache_entry e;
        if (!read_json_string(json, &pos, &key) || key != "db") return false;
        if (!expect_char(json, &pos, ':')) return false;
        if (!read_json_string(json, &pos, &e.db_id)) return false;
        if (!expect_char(json, &pos, ',')) return false;
        if (!read_json_string(json, &pos, &key) || key != "path") return false;
        if (!expect_char(json, &pos, ':')) return false;
        if (!read_json_string(json, &pos, &e.path)) return false;
        if (!expect_char(json, &pos, ',')) return false;
        if (!read_json_string(json, &pos, &key) || key != "size_bytes") {
            return false;
        }
        if (!expect_char(json, &pos, ':')) return false;
        if (!read_json_number(json, &pos, &e.size_bytes)) return false;
        if (!expect_char(json, &pos, ',')) return false;
        if (!read_json_string(json, &pos, &key) || key != "saved_at") {
            return false;
        }
        if (!expect_char(json, &pos, ':')) return false;
        if (!read_json_number(json, &pos, &e.saved_at_ms)) return false;
        if (!expect_char(json, &pos, '}')) return false;
        if (e.db_id.empty() || e.path.empty()) return false;
        out->push_back(std::move(e));
        skip_ws(json, &pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == ']') {
            ++pos;
            break;
        }
        return false;
    }
    return true;
}

bool write_index_json(const std::string &path,
                      const std::vector<cache_entry> &entries) {
    std::string tmp = tmp_path(path);
    try {
        std::filesystem::create_directories(fs::path(tmp).parent_path());
    } catch (...) {
        return false;
    }

    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out) return false;

    out << "{\"format\":1,\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) out << ",";
        out << "{\"db\":\"" << json_escape(entries[i].db_id)
            << "\",\"path\":\"" << json_escape(entries[i].path)
            << "\",\"size_bytes\":" << entries[i].size_bytes
            << ",\"saved_at\":" << entries[i].saved_at_ms << "}";
    }
    out << "]}\n";
    out.close();

    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

bool entry_older(const cache_entry &a, const cache_entry &b) {
    if (a.saved_at_ms != b.saved_at_ms) return a.saved_at_ms < b.saved_at_ms;
    return a.db_id < b.db_id;
}

/* Load the index and reconcile it with the real directory contents. Dead
 * entries (image or metadata missing) are dropped; database directories
 * present on disk but not tracked are added from file sizes + mtimes. A
 * missing or corrupt index file therefore rebuilds itself from the scan —
 * the directory is the crash-safe source of truth. */
bool load_index(const char *cache_dir, std::vector<cache_entry> *entries) {
    std::string root = cache_root_path(cache_dir);

    std::vector<cache_entry> parsed;
    bool parsed_ok = false;
    std::string idx = index_path(cache_dir);
    if (file_exists(idx)) {
        std::ifstream in(idx, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        parsed_ok = parse_index_json(json, &parsed);
    }

    entries->clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return true;

    for (const auto &dir_entry : fs::directory_iterator(root, ec)) {
        if (!dir_entry.is_directory(ec)) continue;
        std::string db = dir_entry.path().filename().string();
        if (db.empty() || db == kIndexName) continue;
        std::string meta = metadata_path(cache_dir, db.c_str());
        std::string img = image_path(cache_dir, db.c_str());
        if (!file_exists(meta) || !file_exists(img)) continue;

        uint64_t size = file_size(img);
        uint64_t mtime = mtime_ms(img);
        bool found = false;
        if (parsed_ok) {
            for (const auto &e : parsed) {
                if (e.db_id == db) {
                    found = true;
                    cache_entry keep = e;
                    keep.size_bytes = size;  // trust the real file
                    if (keep.saved_at_ms == 0) keep.saved_at_ms = mtime;
                    entries->push_back(std::move(keep));
                    break;
                }
            }
        }
        if (!found) {
            cache_entry e;
            e.db_id = db;
            e.path = db + "/state.db";
            e.size_bytes = size;
            e.saved_at_ms = mtime;
            entries->push_back(std::move(e));
        }
    }
    std::sort(entries->begin(), entries->end(), entry_older);
    return true;
}

/* OTSARDB_CACHE_MAX_MB: default 512; fractional values accepted (0.01 =
 * ~10 KiB); 0 = unlimited (pre-0068 behavior). Unparsable -> default. */
uint64_t cache_budget_bytes() {
    const char *value = std::getenv("OTSARDB_CACHE_MAX_MB");
    if (!value || value[0] == '\0') return kDefaultMaxBytes;
    errno = 0;
    char *end = nullptr;
    double mb = std::strtod(value, &end);
    if (end == value || *end != '\0' || mb < 0.0 || !std::isfinite(mb)) {
        return kDefaultMaxBytes;
    }
    if (mb == 0.0) return 0;
    return static_cast<uint64_t>(mb * 1024.0 * 1024.0);
}

/* Entries must be sorted oldest-first. Evict the oldest databases other than
 * protected_db until the tracked total fits the budget. The current session's
 * image is never a candidate during its own close-save. Deleting other
 * databases' images is safe: sessions only save at close and load at open,
 * and a lost cache entry only costs a future recovery (S3 stays
 * authoritative). */
void evict_over_budget(const char *cache_dir,
                       const std::string &protected_db,
                       uint64_t budget_bytes,
                       std::vector<cache_entry> *entries) {
    if (budget_bytes == 0) return;
    uint64_t total = 0;
    for (const auto &e : *entries) total += e.size_bytes;

    for (auto it = entries->begin(); it != entries->end() && total > budget_bytes;) {
        if (it->db_id == protected_db) {
            ++it;
            continue;
        }
        std::error_code ec;
        fs::remove(image_path(cache_dir, it->db_id.c_str()), ec);
        fs::remove(metadata_path(cache_dir, it->db_id.c_str()), ec);
        /* also drop crash leftovers (.tmp files) of an
         * evicted entry so they cannot accumulate. */
        fs::remove(tmp_path(image_path(cache_dir, it->db_id.c_str())), ec);
        fs::remove(tmp_path(metadata_path(cache_dir, it->db_id.c_str())), ec);
        fs::remove(cache_db_dir(cache_dir, it->db_id.c_str()), ec);  // empty dir
        total -= it->size_bytes;
        it = entries->erase(it);
    }
}

}  // namespace

extern "C" int otsardb_local_cache_probe(const char *cache_dir,
                                        const char *database_id,
                                        const otsardb_local_cache_state *remote_head,
                                        const char *target_image_path) {
    if (!cache_dir || !database_id || !remote_head || !target_image_path ||
        database_id[0] == '\0' || remote_head->commit_id[0] == '\0' ||
        remote_head->generation == 0) {
        return 0;
    }

    std::string meta_file = metadata_path(cache_dir, database_id);
    std::string img_file   = image_path(cache_dir, database_id);

    if (!file_exists(meta_file) || !file_exists(img_file)) return 0;

    otsardb_local_cache_state cached;
    if (!read_metadata(meta_file, &cached)) return 0;

    if (cached.generation != remote_head->generation ||
        std::strcmp(cached.commit_id, remote_head->commit_id) != 0 ||
        std::strcmp(cached.commit_sha256, remote_head->commit_sha256) != 0) {
        return 0;
    }

    /* (D1): never serve an image whose bytes do not match
     * the metadata's attestation. The size is checked first (cheap) and the
     * SHA-256 is verified WHILE the image is copied to the target, so a
     * crash-mixed, tampered or bit-rotten cached image (the pre-0135
     * non-atomic save could leave a torn old/new file that this probe used
     * to accept) is rejected and forces a full recovery. */
    if (file_size(img_file) != cached.image_size_bytes) return 0;
    if (!copy_file_verified(img_file, target_image_path,
                            cached.image_size_bytes, cached.image_sha256)) {
        return 0;
    }

    return static_cast<uint64_t>(remote_head->generation) ==
                   cached.generation
               ? 1
               : 0;
}

extern "C" int otsardb_local_cache_read_state(const char *cache_dir,
                                            const char *database_id,
                                            otsardb_local_cache_state *out) {
    if (!cache_dir || !database_id || !out || database_id[0] == '\0') {
        return 0;
    }

    std::string meta_file = metadata_path(cache_dir, database_id);
    std::string img_file   = image_path(cache_dir, database_id);

    if (!file_exists(meta_file) || !file_exists(img_file)) return 0;

    otsardb_local_cache_state cached;
    if (!read_metadata(meta_file, &cached)) return 0;

    *out = cached;
    return 1;
}

extern "C" int otsardb_local_cache_save(const char *cache_dir,
                                       const char *database_id,
                                       const char *source_image_path,
                                       const otsardb_local_cache_state *state) {
    if (!cache_dir || !database_id || !source_image_path || !state ||
        database_id[0] == '\0' || state->commit_id[0] == '\0' ||
        state->generation == 0 || !file_exists(source_image_path)) {
        return 0;
    }

    std::string meta_file = metadata_path(cache_dir, database_id);
    std::string img_file   = image_path(cache_dir, database_id);

    try {
        std::filesystem::create_directories(
            std::filesystem::path(img_file).parent_path());
    } catch (...) {
        return 0;
    }

    /* (D1): attest the image bytes and publish the image
     * ATOMICALLY — stream to <image>.tmp in the same directory, fsync,
     * then rename over the old image. A crash at any point leaves either
     * the previous consistent pair (old image + old metadata) or a
     * metadata/image mismatch the probe rejects; a torn image can never
     * be renamed into place because the final name only ever appears as
     * a complete file. */
    otsardb_local_cache_state attested = *state;
    if (attested.image_size_bytes == 0 ||
        std::strlen(attested.image_sha256) != 64) {
        uint64_t size = 0;
        if (!sha256_file(source_image_path, &size, attested.image_sha256)) {
            return 0;
        }
        attested.image_size_bytes = size;
    }

    if (!copy_file_fsync_tmp(source_image_path, tmp_path(img_file))) return 0;
    {
        std::error_code ec;
        fs::rename(tmp_path(img_file), img_file, ec);
        if (ec) return 0;
    }

    // Stamp the image with the save time. fs::copy_file preserves the
    // source mtime on Windows (CopyFileW), so without this the directory
    // scan used to rebuild a lost index would order images by their source
    // file's timestamp, not by when they were saved.
    {
        std::error_code ec;
        fs::last_write_time(img_file, fs::file_time_type::clock::now(), ec);
    }

    // Now atomically publish the metadata (temp + fsync + rename). A crash
    // between the image rename and this rename leaves the NEW image with
    // the OLD metadata — a mismatched pair the probe rejects — so the
    // next open just falls back to recovery.
    if (!write_metadata(meta_file, &attested)) return 0;

    // Bounded cache: track sizes in the cache-dir index
    // and evict the oldest other images when the budget is exceeded.
    // Best-effort: if the index cannot be written the cache stays valid
    // (unbounded, pre-0068 behavior) and the next save reconciles.
    std::vector<cache_entry> entries;
    load_index(cache_dir, &entries);

    cache_entry current;
    current.db_id = database_id;
    current.path = std::string(database_id) + "/state.db";
    current.size_bytes = file_size(img_file);
    current.saved_at_ms = next_saved_at(now_ms());

    bool replaced = false;
    for (auto &e : entries) {
        if (e.db_id == database_id) {
            e = current;
            replaced = true;
            break;
        }
    }
    if (!replaced) entries.push_back(current);
    std::sort(entries.begin(), entries.end(), entry_older);

    evict_over_budget(cache_dir, database_id, cache_budget_bytes(), &entries);

    write_index_json(index_path(cache_dir), entries);
    return 1;
}

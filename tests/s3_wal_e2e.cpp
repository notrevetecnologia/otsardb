#include "otsardb.h"
#include "otsardb_internal.h"
#include "capability_probe.h"
#include "local_vfs.h"
#include "recovery.h"
#include "s3_object_store.h"
#include "wal_publisher.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#define otsardb_getpid _getpid
#else
#include <unistd.h>
#define otsardb_getpid getpid
#endif

namespace {

const char *required_env(const char *name) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        std::fprintf(stderr, "missing required environment variable: %s\n", name);
        std::exit(2);
    }
    return value;
}

const char *optional_env(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

double wall_ms() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch()).count();
}

int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    auto *output = static_cast<std::string *>(ctx);
    *output = argv[0];
    return 0;
}

void remove_sqlite_files(const std::string &base) {
    std::remove(base.c_str());
    std::string wal = base + "-wal";
    std::string shm = base + "-shm";
    std::remove(wal.c_str());
    std::remove(shm.c_str());
}

std::string test_temp_dir() {
    const char *candidates[] = {
        std::getenv("OTSARDB_TEST_TMP"),
        std::getenv("TMPDIR"),
        std::getenv("TEMP"),
        "/tmp",
    };
    for (const char *candidate : candidates) {
        if (candidate && candidate[0] != '\0') return candidate;
    }
    return "/tmp";
}

/* Deterministic PRNG payload source for the multipart put_stream scenario
 * . Records the thread id of the first call and flags any
 * later call from a DIFFERENT thread: the S3 adapter must never invoke the
 * caller's pull callback from its upload workers, so this flag must stay
 * false. */
struct mp_source {
    uint64_t state;
    size_t remaining;
    std::thread::id first_caller;
    bool called_from_another_thread;
};

long mp_read(void *opaque, void *buffer, size_t max_size) {
    auto *src = static_cast<mp_source *>(opaque);
    std::thread::id current = std::this_thread::get_id();
    if (src->first_caller == std::thread::id()) {
        src->first_caller = current;
    } else if (current != src->first_caller) {
        src->called_from_another_thread = true;
    }
    size_t n = src->remaining < max_size ? src->remaining : max_size;
    auto *bytes = static_cast<unsigned char *>(buffer);
    for (size_t i = 0; i < n; ++i) {
        src->state = src->state * 6364136223846793005ull + 1442695040888963407ull;
        bytes[i] = static_cast<unsigned char>(src->state >> 33);
    }
    src->remaining -= n;
    return static_cast<long>(n);
}

long mp_fail_after_read(void *opaque, void *buffer, size_t max_size) {
    auto *budget = static_cast<size_t *>(opaque);
    if (*budget == 0) return -1;
    size_t n = max_size < *budget ? max_size : *budget;
    std::memset(buffer, 0xAB, n);
    *budget -= n;
    return static_cast<long>(n);
}

long mp_empty_read(void *opaque, void *buffer, size_t max_size) {
    (void)opaque;
    (void)buffer;
    (void)max_size;
    return 0;
}

}  // namespace

int main() {
    assert(otsardb_initialize() == OTSARDB_OK);

    otsardb_s3_config config{};
    config.endpoint = required_env("OTSARDB_S3_ENDPOINT");
    config.region = optional_env("OTSARDB_S3_REGION", "us-east-1");
    config.bucket = required_env("OTSARDB_S3_BUCKET");
    config.access_key = required_env("OTSARDB_S3_ACCESS_KEY");
    config.secret_key = required_env("OTSARDB_S3_SECRET_KEY");
    config.session_token = std::getenv("OTSARDB_S3_SESSION_TOKEN");
    config.prefix = optional_env("OTSARDB_S3_PREFIX", "otsardb-e2e");
    config.use_https = std::strncmp(config.endpoint, "http://", 7) != 0;

    otsardb_object_store *store = otsardb_s3_store_create(&config);
    assert(store != nullptr);

    otsardb_store_probe_report report{};
    assert(otsardb_store_probe_capabilities(store,
                                          "probe/wal-e2e-capabilities",
                                          &report) == OTSARDB_STORE_OK);
    assert(report.s3_core_ok);

    /* --- multipart put_stream scenario --------------------
     * 12 x 5 MiB parts (the minio-cpp kMinPartSize): exercises the parallel
     * part-upload windows. Asserts wire identity (read-back equals the
     * deterministic payload), non-empty multipart ETag, and that the pull
     * callback is only ever invoked from the calling thread. Times the
     * upload to stderr so parallel=1 vs N can be compared across runs. */
    const size_t mp_part_size = 5u * 1024u * 1024u;
    size_t mp_part_count = 12;
    if (const char *override = std::getenv("OTSARDB_S3_MP_E2E_PARTS")) {
        long parsed = std::strtol(override, nullptr, 10);
        if (parsed >= 2 && parsed <= 512) mp_part_count = static_cast<size_t>(parsed);
    }
    const size_t mp_total = mp_part_size * mp_part_count;
    const uint64_t mp_seed = 0x9e3779b97f4a7c15ull;

    std::string mp_expected;
    mp_expected.resize(mp_total);
    {
        uint64_t state = mp_seed;
        for (size_t i = 0; i < mp_total; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            mp_expected[i] = static_cast<char>(state >> 33);
        }
    }

    mp_source mp_src{mp_seed, mp_total, {}, false};
    const std::string mp_key = "mp/put-stream-" + std::to_string(otsardb_getpid());
    otsardb_store_put_options plain{};
    char *mp_etag = nullptr;
    double mp_start_ms = wall_ms();
    assert(otsardb_store_put_stream(store,
                                  mp_key.c_str(),
                                  mp_read,
                                  &mp_src,
                                  static_cast<uint64_t>(mp_total),
                                  &plain,
                                  &mp_etag) == OTSARDB_STORE_OK);
    double mp_elapsed_ms = wall_ms() - mp_start_ms;
    assert(mp_etag != nullptr && mp_etag[0] != '\0');
    assert(mp_src.remaining == 0);
    assert(!mp_src.called_from_another_thread);

    otsardb_store_object mp_obj{};
    assert(otsardb_store_get(store, mp_key.c_str(), &mp_obj) == OTSARDB_STORE_OK);
    assert(mp_obj.size == mp_total);
    assert(std::memcmp(mp_obj.data, mp_expected.data(), mp_total) == 0);
    otsardb_store_release_object(store, &mp_obj);

    std::fprintf(stderr,
                 "mp_put_stream: parts=%zu bytes=%zu ms=%.1f mbps=%.1f etag=%s\n",
                 mp_part_count,
                 mp_total,
                 mp_elapsed_ms,
                 static_cast<double>(mp_total) / (mp_elapsed_ms / 1000.0) / 1000000.0,
                 mp_etag);
    std::free(mp_etag);
    mp_etag = nullptr;
    assert(otsardb_store_delete(store, mp_key.c_str()) == OTSARDB_STORE_OK);

    /* --- routed put scenario ------------------------------
     * otsardb_store_put payloads at or above OTSARDB_S3_MP_MIN are routed
     * through the windowed PARALLEL multipart machinery when the store
     * reports OTSARDB_STORE_CAP_MULTIPART. Note: miniocpp's own PutObject
     * already splits objects above ~5 MiB into SEQUENTIAL multipart parts
     * internally, so a large etag shape cannot distinguish the paths; the
     * deterministic proof is a SMALL payload with OTSARDB_S3_MP_MIN=1: the
     * routed path produces a multipart-shaped etag ("<md5>-<parts>") where
     * the plain single PUT would return a bare md5. OTSARDB_S3_MP_MIN=0
     * disables routing (miniocpp's internal sequential multipart/single
     * PUT behavior is the fallback). All routed puts must read back
     * byte-identical. */
    auto mp_min_env = [](const char *value) {
#if defined(_WIN32)
        if (value) {
            _putenv_s("OTSARDB_S3_MP_MIN", value);
        } else {
            _putenv_s("OTSARDB_S3_MP_MIN", "");
        }
#else
        if (value) {
            setenv("OTSARDB_S3_MP_MIN", value, 1);
        } else {
            unsetenv("OTSARDB_S3_MP_MIN");
        }
#endif
    };
    const bool store_has_multipart =
        (store->capabilities & OTSARDB_STORE_CAP_MULTIPART) != 0;

    /* Deterministic routing proof: 1 MiB payload (below miniocpp's own
     * multipart split threshold) with routing forced by OTSARDB_S3_MP_MIN=1.
     * On a multipart store the etag must carry the multipart part-count
     * suffix; with OTSARDB_S3_MP_MIN=0 the same payload must produce a bare
     * md5 etag (plain single PUT). */
    const size_t small_routed_size = 1024u * 1024u;
    std::string small_routed;
    small_routed.resize(small_routed_size);
    for (size_t i = 0; i < small_routed_size; ++i) {
        small_routed[i] = static_cast<char>((i * 31u) & 0xffu);
    }
    const std::string routed_key = "mp/routed-put-" + std::to_string(otsardb_getpid());

    mp_min_env("1");
    char *small_etag = nullptr;
    assert(otsardb_store_put(store,
                           routed_key.c_str(),
                           small_routed.data(),
                           small_routed.size(),
                           nullptr,
                           &small_etag) == OTSARDB_STORE_OK);
    assert(small_etag != nullptr && small_etag[0] != '\0');
    if (store_has_multipart) {
        assert(std::strchr(small_etag, '-') != nullptr);
    }
    std::free(small_etag);
    small_etag = nullptr;

    mp_min_env("0");
    char *flat_etag = nullptr;
    assert(otsardb_store_put(store,
                           routed_key.c_str(),
                           small_routed.data(),
                           small_routed.size(),
                           nullptr,
                           &flat_etag) == OTSARDB_STORE_OK);
    assert(flat_etag != nullptr && flat_etag[0] != '\0');
    if (store_has_multipart) {
        assert(std::strchr(flat_etag, '-') == nullptr);
    }
    std::free(flat_etag);
    flat_etag = nullptr;

    /* Large payload at the default threshold (10 MiB >= 8 MiB): routed on a
     * multipart store, sequential-internal multipart otherwise; identity
     * must hold in both cases. */
    const size_t routed_size = 10u * 1024u * 1024u;
    std::string routed_payload;
    routed_payload.resize(routed_size);
    {
        uint64_t state = 0x123456789abcdef0ull;
        for (size_t i = 0; i < routed_size; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            routed_payload[i] = static_cast<char>(state >> 33);
        }
    }

    mp_min_env(nullptr);
    char *routed_etag = nullptr;
    double routed_start_ms = wall_ms();
    assert(otsardb_store_put(store,
                           routed_key.c_str(),
                           routed_payload.data(),
                           routed_payload.size(),
                           nullptr,
                           &routed_etag) == OTSARDB_STORE_OK);
    double routed_elapsed_ms = wall_ms() - routed_start_ms;
    assert(routed_etag != nullptr && routed_etag[0] != '\0');
    std::fprintf(stderr,
                 "routed_put: bytes=%zu ms=%.1f mbps=%.1f etag=%s\n",
                 routed_size,
                 routed_elapsed_ms,
                 static_cast<double>(routed_size) / (routed_elapsed_ms / 1000.0) / 1000000.0,
                 routed_etag);
    otsardb_store_object routed_obj{};
    assert(otsardb_store_get(store, routed_key.c_str(), &routed_obj) == OTSARDB_STORE_OK);
    assert(routed_obj.size == routed_payload.size());
    assert(std::memcmp(routed_obj.data, routed_payload.data(), routed_payload.size()) == 0);
    otsardb_store_release_object(store, &routed_obj);
    std::free(routed_etag);
    routed_etag = nullptr;
    mp_min_env(nullptr);

    assert(otsardb_store_delete(store, routed_key.c_str()) == OTSARDB_STORE_OK);

    /* Source error mid-part: the adapter must abort the multipart and
     * return OTSARDB_STORE_ERROR (no worker was spawned for this path). */
    size_t fail_budget = 1000;
    assert(otsardb_store_put_stream(store,
                                  mp_key.c_str(),
                                  mp_fail_after_read,
                                  &fail_budget,
                                  static_cast<uint64_t>(mp_total),
                                  &plain,
                                  nullptr) == OTSARDB_STORE_ERROR);

    /* Exhausted source (zero bytes): same abort + OTSARDB_STORE_ERROR. */
    assert(otsardb_store_put_stream(store,
                                  mp_key.c_str(),
                                  mp_empty_read,
                                  nullptr,
                                  static_cast<uint64_t>(mp_total),
                                  &plain,
                                  nullptr) == OTSARDB_STORE_ERROR);

    const char *database_root = "e2e/orders";
    const char *database_id = "orders-s3-e2e";
    const std::string source_path = test_temp_dir() + "/otsardb-s3-source.db";
    const std::string recovered_path = test_temp_dir() + "/otsardb-s3-recovered.db";
    remove_sqlite_files(source_path);
    remove_sqlite_files(recovered_path);

    otsardb_wal_publisher publisher{};
    assert(otsardb_wal_publisher_init(&publisher,
                                    store,
                                    database_root,
                                    database_id,
                                    OTSARDB_WRITE_STRATEGY_DEFAULT));

    otsardb_local_vfs_instance *vfs = nullptr;
    assert(otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                           &publisher,
                                           &vfs) == 0);

    otsardb *db = nullptr;
    assert(otsardb_open_with_vfs(source_path.c_str(),
                               otsardb_local_vfs_instance_name(vfs),
                               &db) == OTSARDB_OK);
    char *error_message = nullptr;

    assert(otsardb_exec(db,
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA wal_autocheckpoint=0;",
                      nullptr,
                      nullptr,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = nullptr;

    assert(otsardb_exec(db,
                      "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);",
                      nullptr,
                      nullptr,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    error_message = nullptr;

    assert(otsardb_exec(db,
                      "INSERT INTO items(value) VALUES('one');"
                      "BEGIN;"
                      "INSERT INTO items(value) VALUES('two');"
                      "INSERT INTO items(value) VALUES('three');"
                      "COMMIT;",
                      nullptr,
                      nullptr,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);

    assert(publisher.wal_bytes_read_total > 0);
    otsardb_close(db);
    db = nullptr;
    otsardb_local_vfs_instance_destroy(vfs);
    vfs = nullptr;

    remove_sqlite_files(source_path);
    remove_sqlite_files(recovered_path);

    assert(otsardb_recovery_restore_store(store, database_root, recovered_path.c_str()));

    otsardb *recovered = nullptr;
    assert(otsardb_open(recovered_path.c_str(), &recovered) == OTSARDB_OK);
    std::string values;
    error_message = nullptr;
    assert(otsardb_exec(recovered,
                      "SELECT group_concat(value, ',') "
                      "FROM (SELECT value FROM items ORDER BY id);",
                      collect_text,
                      &values,
                      &error_message) == OTSARDB_OK);
    otsardb_free(error_message);
    assert(values == "one,two,three");
    otsardb_close(recovered);

    remove_sqlite_files(recovered_path);
    otsardb_s3_store_destroy(store);

    std::printf("live per-database incremental S3 WAL recovery passed; strategy=%s\n",
                report.s3_cas_ok ? "CAS" : "SINGLE_WRITER");
    return 0;
}

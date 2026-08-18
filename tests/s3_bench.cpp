/*
 * s3_bench.cpp - live-S3 commit path benchmark for OtsarDB.
 *
 * Scenario A measures per-commit latency of 20 transactions of 10 inserts
 * each against a WAL publisher backed by a live S3-compatible store.
 * Scenario B measures one 100000-row transaction. Afterwards the HEAD
 * manifest of the big commit is read back from the store and the number of
 * segments and total segment bytes are reported.
 *
 * All work is bounded (fixed iteration counts, no retry loops in this
 * binary); a failed S3 operation reports a clear error and exits non-zero.
 * The bench never reads stdin, so it can never block on terminal input.
 * Every error path and the success path tear down the store, VFS, database
 * and local files, so repeated runs do not accumulate stale local state.
 *
 * Environment: OTSARDB_S3_ENDPOINT, OTSARDB_S3_REGION (default us-east-1),
 * OTSARDB_S3_BUCKET, OTSARDB_S3_ACCESS_KEY, OTSARDB_S3_SECRET_KEY,
 * OTSARDB_S3_PREFIX (default "bench").
 */

#include "otsardb.h"
#include "otsardb_internal.h"
#include "capability_probe.h"
#include "commit_manifest.h"
#include "journal.h"
#include "local_vfs.h"
#include "s3_object_store.h"
#include "wal_publisher.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define otsardb_getpid _getpid
#else
#include <unistd.h>
#define otsardb_getpid getpid
#endif

namespace {

double wall_ms() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch()).count();
}

bool env_str(const char *name, const char *fallback, std::string *out) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        if (!fallback) {
            std::fprintf(stderr, "missing required environment variable: %s\n", name);
            return false;
        }
        *out = fallback;
        return true;
    }
    *out = value;
    return true;
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

void remove_sqlite_files(const std::string &base) {
    std::remove(base.c_str());
    std::string wal = base + "-wal";
    std::string shm = base + "-shm";
    std::string journal = base + "-journal";
    std::remove(wal.c_str());
    std::remove(shm.c_str());
    std::remove(journal.c_str());
}

bool exec_ok(otsardb *db, const char *sql, std::string *out_error) {
    char *error_message = nullptr;
    int rc = otsardb_exec(db, sql, nullptr, nullptr, &error_message);
    if (rc != OTSARDB_OK) {
        *out_error = error_message ? std::string(error_message) : std::string("otsardb_exec failed");
        otsardb_free(error_message);
        return false;
    }
    otsardb_free(error_message);
    return true;
}

/* Owns every resource the bench can create so that all error paths share one
 * teardown. Modeled on tests/s3_wal_e2e.cpp. */
struct bench_resources {
    otsardb_object_store *store = nullptr;
    otsardb_wal_publisher *publisher = nullptr;
    otsardb_local_vfs_instance *vfs = nullptr;
    otsardb *db = nullptr;
    std::string source_path;
};

void cleanup(bench_resources *r) {
    if (!r) return;
    if (r->db) {
        otsardb_close(r->db);
        r->db = nullptr;
    }
    if (r->vfs) {
        otsardb_local_vfs_instance_destroy(r->vfs);
        r->vfs = nullptr;
    }
    if (!r->source_path.empty()) {
        remove_sqlite_files(r->source_path);
        r->source_path.clear();
    }
    if (r->store) {
        otsardb_s3_store_destroy(r->store);
        r->store = nullptr;
    }
}

int fail(bench_resources *r, const char *message) {
    std::fprintf(stderr, "s3_bench: %s\n", message);
    cleanup(r);
    return 1;
}

}  // namespace

int main() {
    if (otsardb_initialize() != OTSARDB_OK) {
        std::fprintf(stderr, "s3_bench: otsardb_initialize failed\n");
        return 1;
    }

    std::string endpoint;
    std::string region;
    std::string bucket;
    std::string access_key;
    std::string secret_key;
    std::string prefix;
    if (!env_str("OTSARDB_S3_ENDPOINT", nullptr, &endpoint) ||
        !env_str("OTSARDB_S3_REGION", "us-east-1", &region) ||
        !env_str("OTSARDB_S3_BUCKET", nullptr, &bucket) ||
        !env_str("OTSARDB_S3_ACCESS_KEY", nullptr, &access_key) ||
        !env_str("OTSARDB_S3_SECRET_KEY", nullptr, &secret_key) ||
        !env_str("OTSARDB_S3_PREFIX", "bench", &prefix)) {
        return 1;
    }

    otsardb_s3_config config{};
    config.endpoint = endpoint.c_str();
    config.region = region.c_str();
    config.bucket = bucket.c_str();
    config.access_key = access_key.c_str();
    config.secret_key = secret_key.c_str();
    config.session_token = nullptr;
    config.prefix = prefix.c_str();
    config.use_https = endpoint.rfind("http://", 0) != 0;

    bench_resources res;
    res.store = otsardb_s3_store_create(&config);
    if (!res.store) {
        return fail(&res,
                    "otsardb_s3_store_create failed (is the endpoint reachable and are the "
                    "OTSARDB_S3_* variables valid?)");
    }

    /* Probe is optional: warm it up and report, but never fail the bench. A
     * failed probe means store->capabilities stays empty, so the publisher
     * falls back to the SINGLE_WRITER strategy and the S3 operations below
     * surface any real endpoint problem as explicit errors. */
    otsardb_store_probe_report report{};
    otsardb_store_result probe_rc =
        otsardb_store_probe_capabilities(res.store, "probe/perf-bench-capabilities", &report);
    if (probe_rc != OTSARDB_STORE_OK) {
        std::fprintf(stderr,
                     "s3_bench: warning: capability probe failed (%d); continuing with "
                     "SINGLE_WRITER fallback\n",
                     (int)probe_rc);
    }

    const std::string database_root =
        prefix + "/perf/" + std::to_string(otsardb_getpid());
    const char *database_id = "perf-bench";
    res.source_path = test_temp_dir() + "/otsardb-s3-perf.db";
    remove_sqlite_files(res.source_path);

    otsardb_wal_publisher publisher{};
    res.publisher = &publisher;
    /* otsardb_wal_publisher_init returns 1 on success and 0 on failure (the
     * e2e tests assert() on it); success must not be treated as failure. */
    if (otsardb_wal_publisher_init(&publisher,
                                 res.store,
                                 database_root.c_str(),
                                 database_id,
                                 OTSARDB_WRITE_STRATEGY_DEFAULT) == 0) {
        return fail(&res, "otsardb_wal_publisher_init failed");
    }

    if (otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader, &publisher, &res.vfs) != 0) {
        return fail(&res, "otsardb_local_vfs_instance_create failed");
    }

    if (otsardb_open_with_vfs(res.source_path.c_str(),
                            otsardb_local_vfs_instance_name(res.vfs),
                            &res.db) != OTSARDB_OK) {
        return fail(&res, "otsardb_open_with_vfs failed");
    }

    std::string error_message;
    if (!exec_ok(res.db,
                 "PRAGMA journal_mode=WAL;"
                 "PRAGMA wal_autocheckpoint=0;"
                 "CREATE TABLE t(id INTEGER PRIMARY KEY, value TEXT);",
                 &error_message)) {
        std::fprintf(stderr, "s3_bench: setup failed: %s\n", error_message.c_str());
        return fail(&res, "setup failed");
    }

    /* Scenario A: 20 commits of 10 inserts each; wall time per COMMIT. */
    double commit_ms_total = 0.0;
    for (int iter = 0; iter < 20; ++iter) {
        if (!exec_ok(res.db, "BEGIN;", &error_message)) {
            std::fprintf(stderr, "s3_bench: BEGIN failed: %s\n", error_message.c_str());
            return fail(&res, "BEGIN failed");
        }
        for (int j = 0; j < 10; ++j) {
            char sql[128];
            snprintf(sql, sizeof(sql), "INSERT INTO t(value) VALUES('c%d-%d');", iter, j);
            if (!exec_ok(res.db, sql, &error_message)) {
                std::fprintf(stderr, "s3_bench: insert failed: %s\n", error_message.c_str());
                return fail(&res, "insert failed");
            }
        }
        double commit_start_ms = wall_ms();
        if (!exec_ok(res.db, "COMMIT;", &error_message)) {
            std::fprintf(stderr, "s3_bench: COMMIT failed: %s\n", error_message.c_str());
            return fail(&res, "COMMIT failed");
        }
        commit_ms_total += wall_ms() - commit_start_ms;
    }
    std::printf("commit_latency_ms_avg=%.3f\n", commit_ms_total / 20.0);

    /* Scenario B: one 100000-row transaction via WITH RECURSIVE. */
    const unsigned long long big_rows = 100000ull;
    const char *big_sql =
        "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x<100000) "
        "INSERT INTO t(value) SELECT 'row-'||x FROM cnt;";
    double big_start_ms = wall_ms();
    if (!exec_ok(res.db, big_sql, &error_message)) {
        std::fprintf(stderr, "s3_bench: big transaction failed: %s\n", error_message.c_str());
        return fail(&res, "big transaction failed");
    }
    double big_elapsed_ms = wall_ms() - big_start_ms;
    double big_mbps = big_elapsed_ms > 0.0
                          ? (double)big_rows * 40.0 / (big_elapsed_ms / 1000.0) / 1000000.0
                          : 0.0;
    std::printf("big_tx_ms=%.1f\n", big_elapsed_ms);
    std::printf("big_tx_mbps=%.2f\n", big_mbps);

    /* Close the database side; the store must stay alive for the manifest
     * read below. */
    otsardb_close(res.db);
    res.db = nullptr;
    otsardb_local_vfs_instance_destroy(res.vfs);
    res.vfs = nullptr;
    remove_sqlite_files(res.source_path);
    res.source_path.clear();

    /* Read the HEAD manifest of the big commit and summarize its segments. */
    otsardb_head head{};
    char *etag = nullptr;
    otsardb_journal_result jrc =
        otsardb_journal_read_head(res.store, database_root.c_str(), &head, &etag);
    otsardb_free(etag);
    if (jrc != OTSARDB_JOURNAL_OK) {
        std::fprintf(stderr, "s3_bench: otsardb_journal_read_head failed (%d)\n", (int)jrc);
        return fail(&res, "otsardb_journal_read_head failed");
    }

    otsardb_store_object manifest_object{};
    otsardb_store_result src = otsardb_store_get(res.store, head.commit_key, &manifest_object);
    if (src != OTSARDB_STORE_OK) {
        std::fprintf(stderr, "s3_bench: manifest get failed (%d) key=%s\n", (int)src, head.commit_key);
        return fail(&res, "manifest get failed");
    }

    otsardb_commit_manifest manifest{};
    if (!otsardb_commit_manifest_decode_json(
            reinterpret_cast<const char *>(manifest_object.data), manifest_object.size, &manifest)) {
        std::fprintf(stderr, "s3_bench: otsardb_commit_manifest_decode_json failed\n");
        otsardb_store_release_object(res.store, &manifest_object);
        return fail(&res, "manifest decode failed");
    }

    uint64_t segment_bytes_total = 0;
    for (uint32_t i = 0; i < manifest.segment_count; ++i) {
        segment_bytes_total += manifest.segments[i].size;
    }
    std::printf("segments_in_big_commit=%u\n", manifest.segment_count);
    std::printf("segment_bytes_total=%llu\n", (unsigned long long)segment_bytes_total);
    otsardb_commit_manifest_free(&manifest);
    otsardb_store_release_object(res.store, &manifest_object);

    cleanup(&res);
    std::printf("bench_ok=1\n");
    fflush(stdout);
    return 0;
}

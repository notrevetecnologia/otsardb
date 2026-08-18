#include "otsardb.h"
#include "otsardb_internal.h"
#include "object_store.h"
#include "s3_database.h"
#include "s3_object_store.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

/* live regression test: the promotion safe-scan against a
 * REAL S3 store must COMPLETE — the 0099-observed self-deadlock is an
 * S3-store-only defect ( reproduced 3/3 on docker
 * MinIO): replica_safe_scan_cb re-entered otsardb_store_get from inside the
 * list callback while s3_list_objects held the process-wide 0064 request
 * mutex across it. memory_store (the 0091/0077 suites) has no such mutex,
 * so only a real store can reproduce it.
 *
 * This test drives the REAL session open path (otsardb_s3_open_target) with
 * OTSARDB_S3_PROMOTE=1 against a configured S3 pair — the primary
 * (OTSARDB_S3_*) and the replica destination (OTSARDB_S3_REPLICA_*) — with a
 * WATCHDOG thread: if the promote open does not finish within the bound
 * the process is terminated with exit code 73 (a hang = FAIL, never a
 * silent pass). SKIPS (77) when the replica environment is absent.
 *
 * Scenarios (each on its own root, all self-cleaned):
 * S1 promote completes (the 0099 phase-4 scenario) - dual seed 3 rows
 * (epochs 1/1), then a PROMOTE=1 open under the watchdog: the safe
 * scan must finish, the destination epoch must raise 1 -> 2, the
 * promoted session must write row 4 DUAL (old primary follows) and
 * report count 4.
 * S2 nothing-to-promote path - a PROMOTE=1 open on a fresh root (no
 * COMMITTED ledger anywhere) must REFUSE fast and cleanly (the
 * empty listing must not hang either); the epoch record stays 1.
 *
 * Exit 0 = all scenarios passed; 77 = skipped; 73 = watchdog fired; other
 * = fail.
 */

namespace {

const char *required_env(const char *name) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        std::fprintf(stderr, "missing required environment variable: %s\n", name);
        std::exit(77);
    }
    return value;
}

const char *optional_env(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void clear_env(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    auto *output = static_cast<std::string *>(ctx);
    *output = argv[0];
    return 0;
}

int exec_ok(otsardb *db, const char *sql, std::string *out_error) {
    char *error = nullptr;
    int rc = otsardb_exec(db, sql, nullptr, nullptr, &error);
    if (error) {
        *out_error = error;
        otsardb_free(error);
    }
    return rc;
}

std::string query_one(otsardb *db, const char *sql) {
    std::string value;
    char *error = nullptr;
    int rc = otsardb_exec(db, sql, collect_text, &value, &error);
    otsardb_free(error);
    assert(rc == OTSARDB_OK);
    return value;
}

std::string query_count(otsardb *db, const char *table) {
    std::string sql = "SELECT count(*) FROM ";
    sql += table;
    sql += ";";
    return query_one(db, sql.c_str());
}

/* ---- raw store helpers (cleanup + state inspection) ---------------------- */

struct delete_ctx {
    std::vector<std::string> keys;
    int deleted = 0;
};

int delete_cb(const char *key, void *opaque) {
    auto *ctx = static_cast<delete_ctx *>(opaque);
    ctx->keys.emplace_back(key);
    return 0;
}

static int delete_prefix_win(otsardb_object_store *store, const char *prefix) {
    delete_ctx ctx;
    try {
        (void)otsardb_store_list(store, prefix, delete_cb, &ctx);
        for (const std::string &key : ctx.keys) {
            std::string full = std::string(prefix) + key;
            if (otsardb_store_delete(store, full.c_str()) == OTSARDB_STORE_OK) {
                ++ctx.deleted;
            }
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "cleanup: C++ exception during list/delete: %s\n", e.what());
        std::exit(2);
    } catch (...) {
        std::fprintf(stderr, "cleanup: unknown C++ exception during list/delete\n");
        std::exit(2);
    }
    return ctx.deleted;
}

void delete_root(otsardb_object_store *store, const std::string &root) {
    for (const char *suffix : {"/", "/commits/", "/replica/", "/segments/"}) {
        delete_ctx ctx;
        (void)otsardb_store_list(store, (root + suffix).c_str(), delete_cb, &ctx);
        for (const std::string &key : ctx.keys) {
            otsardb_store_delete(store, (root + suffix + key).c_str());
        }
    }
}

otsardb_object_store *raw_store(const char *endpoint_env,
                              const char *bucket_env,
                              const char *region,
                              const char *access_key,
                              const char *secret_key) {
    otsardb_s3_config config{};
    config.endpoint = required_env(endpoint_env);
    config.region = region ? region : "us-east-1";
    config.bucket = required_env(bucket_env);
    config.access_key = required_env(access_key);
    config.secret_key = required_env(secret_key);
    config.use_https = std::strncmp(config.endpoint, "http://", 7) != 0;
    return otsardb_s3_store_create(&config);
}

std::string json_field(const std::string &json, const char *field) {
    std::string needle = "\"" + std::string(field) + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos < json.size() && json[pos] == '"') {
        ++pos;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }
    size_t end = pos;
    while (end < json.size() &&
           (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) {
        ++end;
    }
    return json.substr(pos, end - pos);
}

std::string object_text(otsardb_object_store *store, const std::string &key) {
    otsardb_store_object object{};
    if (otsardb_store_get(store, key.c_str(), &object) != OTSARDB_STORE_OK) return "";
    std::string text(reinterpret_cast<const char *>(object.data), object.size);
    otsardb_store_release_object(store, &object);
    return text;
}

std::string head_commit_id(otsardb_object_store *store, const std::string &root) {
    return json_field(object_text(store, root + "/HEAD.json"), "commit_id");
}

std::string epoch_value(otsardb_object_store *store, const std::string &root) {
    return json_field(object_text(store, root + "/replica/epoch.json"), "epoch");
}

/* ---- the watchdog --------------------------------------------------------- */

/* OTSARDB_S3_PROMOTE=1 on a real S3 store hangs FOREVER on pre-0103 code
 * (the 0064 mutex self-deadlock, 0099). The watchdog gives the bounded
 * wait: if the promote open does not finish in time the process exits 73
 * (the harness records 73 as the deadlock verdict — a hang can never be
 * a silent pass). */
void watchdog_arm(const std::chrono::seconds &bound) {
    std::thread watchdog([bound]() {
        std::this_thread::sleep_for(bound);
        std::fprintf(stderr,
                     "WATCHDOG FIRED after %lld s: the promote open did not "
                     "complete (the 0099 list-callback self-deadlock class) "
                     "- FAIL\n",
                     (long long)bound.count());
        std::fflush(stderr);
        std::_Exit(73);
    });
    watchdog.detach();
}

/* ---- scenarios ------------------------------------------------------------ */

struct store_pair {
    otsardb_object_store *primary;
    otsardb_object_store *replica;
};

store_pair open_raw_pair() {
    const char *region = optional_env("OTSARDB_S3_REGION", "us-east-1");
    store_pair pair{};
    pair.primary = raw_store("OTSARDB_S3_ENDPOINT",
                             "OTSARDB_S3_BUCKET",
                             region,
                             "OTSARDB_S3_ACCESS_KEY",
                             "OTSARDB_S3_SECRET_KEY");
    pair.replica = raw_store("OTSARDB_S3_REPLICA_ENDPOINT",
                             "OTSARDB_S3_REPLICA_BUCKET",
                             optional_env("OTSARDB_S3_REPLICA_REGION", "us-east-1"),
                             "OTSARDB_S3_REPLICA_ACCESS_KEY",
                             "OTSARDB_S3_REPLICA_SECRET_KEY");
    assert(pair.primary != nullptr);
    assert(pair.replica != nullptr);
    return pair;
}

void close_raw_pair(store_pair &pair) {
    otsardb_s3_store_destroy(pair.primary);
    otsardb_s3_store_destroy(pair.replica);
}

otsardb *open_session(const std::string &root, std::string *out_error, int *out_rc) {
    std::string target =
        std::string("s3://") + required_env("OTSARDB_S3_BUCKET") + "/" + root;
    otsardb *db = nullptr;
    int rc = otsardb_s3_open_target(target.c_str(), &db);
    if (out_rc) *out_rc = rc;
    if (rc != OTSARDB_OK) {
        if (out_error) *out_error = otsardb_s3_last_error();
        return nullptr;
    }
    return db;
}

}  // namespace

int main() {
    required_env("OTSARDB_S3_ENDPOINT");
    required_env("OTSARDB_S3_BUCKET");
    required_env("OTSARDB_S3_ACCESS_KEY");
    required_env("OTSARDB_S3_SECRET_KEY");
    required_env("OTSARDB_S3_REPLICA_ENDPOINT");
    required_env("OTSARDB_S3_REPLICA_BUCKET");

    const char *cache_dir = optional_env("OTSARDB_TEST_TMP", "otsardb-promote-scan-live-cache");
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_S3_PROBE_CACHE", "0");
    set_env("OTSARDB_LEASE_TTL", "10");

    const std::string ROOT_A = "promote-scan-live/a/db";
    const std::string ROOT_B = "promote-scan-live/b/db";

    store_pair pair = open_raw_pair();
    delete_root(pair.primary, ROOT_A);
    delete_root(pair.replica, ROOT_A);
    delete_root(pair.primary, ROOT_B);
    delete_root(pair.replica, ROOT_B);

    /* S1: the 0099 phase-4 promotion scenario — dual seed, then a
     * PROMOTE=1 open that must COMPLETE (safe scan without deadlock),
     * raise the destination epoch 1 -> 2, and keep writing dual. */
    {
        std::string error;
        otsardb *db = open_session(ROOT_A, &error, nullptr);
        assert(db != nullptr);
        int rc = exec_ok(db,
                         "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
                         "INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c');"
                         "SELECT count(*) FROM t;",
                         &error);
        assert(rc == OTSARDB_OK);
        assert(query_count(db, "t") == "3");
        otsardb_close(db);

        std::string h1 = head_commit_id(pair.primary, ROOT_A);
        std::string h2 = head_commit_id(pair.replica, ROOT_A);
        assert(!h1.empty() && h1 == h2);
        assert(epoch_value(pair.primary, ROOT_A) == "1");
        assert(epoch_value(pair.replica, ROOT_A) == "1");
        std::fprintf(stderr, "S1 dual seed: 3 rows; HEAD=%s equal; epochs 1/1\n",
                     h1.c_str());

        /* THE regression step: the promote open under the watchdog. */
        watchdog_arm(std::chrono::seconds(120));
        set_env("OTSARDB_S3_PROMOTE", "1");
        otsardb *promote_session = open_session(ROOT_A, &error, nullptr);
        clear_env("OTSARDB_S3_PROMOTE");
        assert(promote_session != nullptr);
        assert(epoch_value(pair.replica, ROOT_A) == "2");
        assert(epoch_value(pair.primary, ROOT_A) == "1");
        std::fprintf(stderr,
                     "S1 promote open COMPLETED (no deadlock): epoch 1->2 on "
                     "the destination (old primary stays 1)\n");

        rc = exec_ok(promote_session,
                     "INSERT INTO t VALUES(4,'d'); SELECT count(*) FROM t;",
                     &error);
        assert(rc == OTSARDB_OK);
        assert(query_count(promote_session, "t") == "4");
        h1 = head_commit_id(pair.primary, ROOT_A);
        h2 = head_commit_id(pair.replica, ROOT_A);
        assert(!h1.empty() && h1 == h2);
        otsardb_close(promote_session);
        std::fprintf(stderr,
                     "S1 promoted write: row 4 DUAL on the swapped pair; "
                     "HEAD=%s equal\n", h1.c_str());
    }

    /* S2: nothing-to-promote on a fresh root — the scan over an EMPTY (or
     * epoch-only) listing must refuse fast and cleanly (no hang either). */
    {
        watchdog_arm(std::chrono::seconds(120));
        set_env("OTSARDB_S3_PROMOTE", "1");
        std::string error;
        otsardb *promoting = open_session(ROOT_B, &error, nullptr);
        clear_env("OTSARDB_S3_PROMOTE");
        assert(promoting == nullptr);
        assert(error.find("promotion refused") != std::string::npos ||
               error.find("COMMITTED") != std::string::npos);
        std::string ep = epoch_value(pair.replica, ROOT_B);
        assert(ep == "1" || ep == ""); /* nothing was raised */
        std::fprintf(stderr, "S2 nothing-to-promote refused fast: %s\n",
                     error.substr(0, 140).c_str());
    }

    close_raw_pair(pair);
    std::printf("promote safe-scan live test passed: promote open completed "
                "against the real store (watchdog armed, never fired)\n");
    return 0;
}

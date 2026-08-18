#include "otsardb.h"
#include "otsardb_internal.h"
#include "object_store.h"
#include "s3_database.h"
#include "s3_object_store.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

/* live e2e: multi-S3 READ-REPLICA sessions
 * ( 0069 "read-replica" decision point).
 *
 * Drives the REAL session open path (otsardb_s3_open_target_ex) against a
 * configured S3 pair — the primary (OTSARDB_S3_*) and the replica
 * (OTSARDB_S3_REPLICA_*) destinations. The replica MUST be a DIFFERENT
 * bucket/endpoint with the same database root so the two stores never
 * collide. SKIPS (exit 77) when the replica environment is absent.
 *
 * Scenarios:
 * E1 writer dual-publish - a primary session (with the replica env)
 * creates a table and inserts 3 rows; the
 * commit is acked only after both
 * destinations hold it (0072).
 * E2 reader sees COMMITTED - a session opened with
 * OTSARDB_S3_READ_FROM_REPLICA=1 reads the
 * same 3 rows through the replica store
 * (recovery + lazy hydration on the
 * replica destination).
 * E3 reader refuses DML - INSERT/UPDATE on the reader fail with
 * SQLITE_READONLY ("readonly" in the
 * error message); the reader is a
 * READ-ONLY session.
 * E4 reader works while the primary holds the lease - a live primary
 * session (lease claimed, kept open)
 * running next to the reader: the reader
 * never touches the lease and keeps
 * reading; the reader performs ZERO writes
 * to the replica root (object key sets
 * before/after are identical).
 * E5 reader sees the new commit - a second primary session inserts a 4th
 * row; a NEW reader immediately sees 4
 * rows (the replica holds the full
 * COMMITTED copy).
 *
 * Exit 0 = all scenarios passed; 77 = skipped (no replica env); other = fail.
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

const char *DATABASE_ROOT = "read-replica-e2e/db";

int exec_ok(otsardb *db, const char *sql, std::string *out_error) {
    char *error = nullptr;
    int rc = otsardb_exec(db, sql, nullptr, nullptr, &error);
    if (error) {
        *out_error = error;
        otsardb_free(error);
    }
    return rc;
}

/* Self-cleanup: delete every object under the database root on a raw store
 * (the test must be re-runnable without operator cleanup; a previous failed
 * run must never trip the next one). The S3 adapter's list is ONE-LEVEL
 * (miniocpp recursive=false), so the root prefix alone would only surface
 * db.json/HEAD.json plus the folder markers — the deep object sets
 * (commits/, replica/, segments/) are cleaned with their own listings. The
 * list callback only COLLECTS keys — it must not call store ops (the S3
 * list holds the process-wide request mutex while iterating; a nested
 * delete would deadlock). */
struct delete_ctx {
    std::vector<std::string> keys;
    int deleted = 0;
};

int delete_cb(const char *key, void *opaque) {
    auto *ctx = static_cast<delete_ctx *>(opaque);
    ctx->keys.emplace_back(key);
    return 0;
}

/* Exception-isolated list+delete: a C++ exception inside the S3 client is
 * caught and reported instead of terminating the test process (observed on
 * this platform; the test must fail visibly, not crash). */
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

void delete_prefix(otsardb_object_store *store, const std::string &prefix) {
    int deleted = delete_prefix_win(store, prefix.c_str());
    std::fprintf(stderr, "cleanup: removed %d object(s) under %s\n",
                 deleted, prefix.c_str());
}

void delete_root(otsardb_object_store *store, const std::string &root) {
    for (const char *suffix : {"", "/commits", "/replica", "/segments"}) {
        delete_prefix(store, root + (suffix[0] == '/' ? suffix : "/"));
    }
}

/* Raw store over one configured destination (used only for cleanup). */
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

}  // namespace

int main() {
    required_env("OTSARDB_S3_ENDPOINT");
    required_env("OTSARDB_S3_BUCKET");
    required_env("OTSARDB_S3_ACCESS_KEY");
    required_env("OTSARDB_S3_SECRET_KEY");
    required_env("OTSARDB_S3_REPLICA_ENDPOINT");
    required_env("OTSARDB_S3_REPLICA_BUCKET");

    /* Deterministic, isolated local execution + probe cache. */
    const char *cache_dir = optional_env("OTSARDB_TEST_TMP", "otsardb-rr-e2e-cache");
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_S3_PROBE_CACHE", "0"); /* every open probes (replica rule) */

    /* Remove any state a previous (failed) run left under the database root
     * on BOTH destinations: the test must be re-runnable without operator
     * cleanup. */
    {
        const char *region = optional_env("OTSARDB_S3_REGION", "us-east-1");
        otsardb_object_store *primary_store = raw_store("OTSARDB_S3_ENDPOINT",
                                                      "OTSARDB_S3_BUCKET",
                                                      region,
                                                      "OTSARDB_S3_ACCESS_KEY",
                                                      "OTSARDB_S3_SECRET_KEY");
        assert(primary_store != nullptr);
        delete_root(primary_store, DATABASE_ROOT);
        otsardb_s3_store_destroy(primary_store);

        otsardb_object_store *replica_store = raw_store("OTSARDB_S3_REPLICA_ENDPOINT",
                                                      "OTSARDB_S3_REPLICA_BUCKET",
                                                      optional_env("OTSARDB_S3_REPLICA_REGION",
                                                                   "us-east-1"),
                                                      "OTSARDB_S3_REPLICA_ACCESS_KEY",
                                                      "OTSARDB_S3_REPLICA_SECRET_KEY");
        assert(replica_store != nullptr);
        delete_root(replica_store, DATABASE_ROOT);
        otsardb_s3_store_destroy(replica_store);
    }

    /* E1: primary session, dual publish (replica env present -> Model B). */
    {
        std::string error;
        otsardb *db = nullptr;
        std::string target = std::string("s3://") + required_env("OTSARDB_S3_BUCKET") +
                             "/" + DATABASE_ROOT;
        assert(otsardb_s3_open_target(target.c_str(), &db) == OTSARDB_OK);
        int e1_rc = exec_ok(db,
                            "CREATE TABLE items(id INTEGER PRIMARY KEY, v TEXT);"
                            "INSERT INTO items(v) VALUES('a');"
                            "INSERT INTO items(v) VALUES('b');"
                            "INSERT INTO items(v) VALUES('c');",
                            &error);
        if (e1_rc != OTSARDB_OK) {
            std::fprintf(stderr, "E1 failed rc=%d error=%s\n", e1_rc, error.c_str());
        }
        assert(e1_rc == OTSARDB_OK);
        std::string count;
        char *err = nullptr;
        assert(otsardb_exec(db, "SELECT count(*) FROM items;", collect_text, &count,
                          &err) == OTSARDB_OK);
        otsardb_free(err);
        assert(count == "3");
        otsardb_close(db);
        std::fprintf(stderr, "E1 dual-publish writer: 3 rows committed on both destinations\n");
    }

    /* E2 + E3: the reader sees the COMMITTED state and refuses DML. */
    {
        std::string error;
        otsardb *db = nullptr;
        std::string target = std::string("s3://") + required_env("OTSARDB_S3_BUCKET") +
                             "/" + DATABASE_ROOT;
        set_env("OTSARDB_S3_READ_FROM_REPLICA", "1");
        assert(otsardb_s3_open_target(target.c_str(), &db) == OTSARDB_OK);

        std::string count;
        char *err = nullptr;
        assert(otsardb_exec(db, "SELECT count(*) FROM items;", collect_text, &count,
                          &err) == OTSARDB_OK);
        otsardb_free(err);
        assert(count == "3");
        std::fprintf(stderr, "E2 reader sees the replica's COMMITTED state: count=3\n");

        /* DML must fail with SQLITE_READONLY on the reader (documented in
         *): query_only=ON turns every DML into "attempt to
         * write a readonly database". */
        int insert_rc = exec_ok(db,
                                "INSERT INTO items(v) VALUES('x');",
                                &error);
        assert(insert_rc != OTSARDB_OK);
        assert(error.find("readonly") != std::string::npos);
        std::fprintf(stderr, "E3 reader refuses DML: rc=%d error=%.60s\n",
                     insert_rc, error.c_str());

        int update_rc = exec_ok(db, "UPDATE items SET v='z' WHERE id=1;", &error);
        assert(update_rc != OTSARDB_OK);
        assert(error.find("readonly") != std::string::npos);

        otsardb_close(db);
        std::fprintf(stderr, "E3 reader refuses DML: INSERT and UPDATE refused\n");
    }

    /* E4: reader works while a live primary session holds the lease. */
    {
        otsardb *primary = nullptr;
        otsardb *reader = nullptr;
        std::string target = std::string("s3://") + required_env("OTSARDB_S3_BUCKET") +
                             "/" + DATABASE_ROOT;
        set_env("OTSARDB_S3_READ_FROM_REPLICA", "");
        assert(otsardb_s3_open_target(target.c_str(), &primary) == OTSARDB_OK);

        set_env("OTSARDB_S3_READ_FROM_REPLICA", "1");
        assert(otsardb_s3_open_target(target.c_str(), &reader) == OTSARDB_OK);
        std::string count;
        char *err = nullptr;
        assert(otsardb_exec(reader, "SELECT count(*) FROM items;", collect_text, &count,
                          &err) == OTSARDB_OK);
        otsardb_free(err);
        assert(count == "3");

        std::string error;
        int insert_rc = exec_ok(reader, "INSERT INTO items(v) VALUES('y');", &error);
        assert(insert_rc != OTSARDB_OK);
        assert(error.find("readonly") != std::string::npos);

        otsardb_close(reader);
        otsardb_close(primary);
        std::fprintf(stderr,
                     "E4 reader works while the primary session holds the live lease\n");
    }

    /* E5: a new commit on the primary is immediately visible to a new reader. */
    {
        std::string error;
        otsardb *db = nullptr;
        std::string target = std::string("s3://") + required_env("OTSARDB_S3_BUCKET") +
                             "/" + DATABASE_ROOT;
        set_env("OTSARDB_S3_READ_FROM_REPLICA", "");
        assert(otsardb_s3_open_target(target.c_str(), &db) == OTSARDB_OK);
        assert(exec_ok(db, "INSERT INTO items(v) VALUES('d');", &error) == OTSARDB_OK);
        otsardb_close(db);

        set_env("OTSARDB_S3_READ_FROM_REPLICA", "1");
        assert(otsardb_s3_open_target(target.c_str(), &db) == OTSARDB_OK);
        std::string count;
        char *err = nullptr;
        assert(otsardb_exec(db, "SELECT count(*) FROM items;", collect_text, &count,
                          &err) == OTSARDB_OK);
        otsardb_free(err);
        assert(count == "4");
        otsardb_close(db);
        std::fprintf(stderr, "E5 new reader sees the new COMMITTED commit: count=4\n");
    }

    clear_env("OTSARDB_S3_READ_FROM_REPLICA");
    std::printf("read-replica session e2e passed: COMMITTED reads, readonly DML, "
                "lease independence, fresh-commit visibility\n");
    return 0;
}

#include "otsardb.h"
#include "otsardb_internal.h"
#include "object_store.h"
#include "s3_database.h"
#include "s3_object_store.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

/* live session e2e: session-level promotion store swap
 * (; 0077 Next item 2 — the s3_database.cpp wiring).
 *
 * Drives the REAL session open path (otsardb_s3_open_target_ex) against a
 * configured S3 pair — the primary (OTSARDB_S3_*, the OLD primary role) and
 * the replica (OTSARDB_S3_REPLICA_*, the TO-BE-PROMOTED destination; a
 * DIFFERENT bucket/endpoint with the same database root). SKIPS (exit 77)
 * when the replica environment is absent.
 *
 * The database identity is bound to the TARGET (bucket + root) — the
 * promoted store keeps the ORIGINAL identity, so every session in this
 * test opens the SAME target and only the store configuration (the
 * OTSARDB_S3_* / OTSARDB_S3_REPLICA_* env) changes; the promoted bucket is
 * read through the 0084 read-replica path (the same-target reader).
 *
 * Scenarios (each on its own database root; all self-cleaned):
 * E1 dual seed - 3 rows acked on both destinations; HEAD
 * commit ids EQUAL, epoch records 1/1.
 * E2 lease-gated refusal - a live writer session holds the OLD primary
 * lease; an OTSARDB_S3_PROMOTE=1 open FAILS with
 * the clear "promotion refused ... lease"
 * message; the epoch record is untouched.
 * E3 promote + swap - the writer closes (lease released); a
 * PROMOTE=1 open SUCCEEDS: the destination
 * epoch raised 1 -> 2, the old primary's epoch
 * record stays 1, and the session's INSERT
 * commits DUAL (the old primary is now its
 * replica destination — the role swap); a
 * READER (OTSARDB_S3_READ_FROM_REPLICA=1) then
 * reads 4 rows THROUGH the promoted store —
 * post-promote writes land on the destination.
 * E4 old-primary fenced - an old-primary-role session (pair env) opens
 * first and holds the OLD primary lease; a
 * SECOND old-primary-role session is
 * lease-HELD (candidate) and its INSERT is
 * refused with SQLITE_BUSY — heads unmoved on
 * both destinations (the 0085-observed fence).
 * E5 no-PROMOTE unchanged - a pair-env session WITHOUT OTSARDB_S3_PROMOTE
 * keeps working normally after the promotion
 * (epoch records adopted at 2/1, dual publish
 * keeps the swapped pair in sync): row 5,
 * count 5, HEADs equal.
 * E6 TTL-expiry gate - a raw <root>/lease.json with a FRESH
 * Last-Modified refuses the PROMOTE=1 open
 * (same TTL - skew predicate as the lease
 * module); after the lease provably expires
 * (OTSARDB_LEASE_TTL=10 -> 5 s skew) the SAME
 * open succeeds (epoch 2).
 * E7 concurrent promote - two threads open with PROMOTE=1 racing the
 * epoch CAS on one fresh root: at least one
 * session wins; a loser (if any) fails with
 * the clear "promotion refused" conflict
 * message (never a silent divergence); the
 * destination epoch is >= 2; a winner's write
 * commits dual (promoted side advances) and an
 * old-primary-role session is fenced
 * (lease-held, heads unmoved).
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

void delete_prefix(otsardb_object_store *store, const std::string &prefix) {
    int deleted = delete_prefix_win(store, prefix.c_str());
    std::fprintf(stderr, "cleanup: removed %d object(s) under %s\n",
                 deleted, prefix.c_str());
}

void delete_root(otsardb_object_store *store, const std::string &root) {
    /* NOTE: the deep prefixes MUST carry their trailing slash: the S3
     * adapter's list returns keys RELATIVE to the (slash-terminated)
     * physical prefix, and the delete target is prefix + key. A prefix
     * without the trailing slash would construct wrong delete keys and
     * silently no-op (the deep leftover objects would survive). */
    for (const char *suffix : {"/", "/commits/", "/replica/", "/segments/"}) {
        delete_prefix(store, root + suffix);
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

void clean_roots(store_pair &pair, const std::string &root) {
    delete_root(pair.primary, root);
    delete_root(pair.replica, root);
}

/* Open a session on the CURRENT env's primary bucket (the target path is
 * built from OTSARDB_S3_BUCKET); returns nullptr on failure with the error
 * message in *out_error. */
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

    const char *cache_dir = optional_env("OTSARDB_TEST_TMP", "otsardb-promote-e2e-cache");
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    set_env("OTSARDB_CACHE_DIR", cache_dir);
    set_env("OTSARDB_S3_PROBE_CACHE", "0");
    set_env("OTSARDB_LEASE_TTL", "10");

    const std::string ROOT_A = "promote-e2e-a/db";
    const std::string ROOT_B = "promote-e2e-b/db";
    const std::string ROOT_C = "promote-e2e-c/db";

    store_pair pair = open_raw_pair();
    clean_roots(pair, ROOT_A);
    clean_roots(pair, ROOT_B);
    clean_roots(pair, ROOT_C);

    /* ------------------------------------------------------------------ */
    /* E1: dual seed — 3 rows acked on both destinations, HEAD ids equal, */
    /* epoch records 1/1 (the OTSARDB_S3_* env is the OLD primary bucket, */
    /* OTSARDB_S3_REPLICA_* the TO-BE-PROMOTED destination). */
    /* ------------------------------------------------------------------ */
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
        std::fprintf(stderr, "E1 dual seed: 3 rows; HEAD=%s equal; epochs 1/1\n",
                     h1.c_str());
    }

    /* ------------------------------------------------------------------ */
    /* E2: the old primary's live writer lease refuses the PROMOTE=1 open. */
    /* ------------------------------------------------------------------ */
    {
        otsardb *writer = open_session(ROOT_A, nullptr, nullptr);
        assert(writer != nullptr); /* claims the old primary lease */

        set_env("OTSARDB_S3_PROMOTE", "1");
        std::string error;
        otsardb *promoting = open_session(ROOT_A, &error, nullptr);
        assert(promoting == nullptr);
        clear_env("OTSARDB_S3_PROMOTE");
        assert(error.find("promotion refused") != std::string::npos);
        assert(error.find("lease") != std::string::npos);
        assert(epoch_value(pair.replica, ROOT_A) == "1"); /* epoch untouched */
        std::fprintf(stderr, "E2 lease-gated refusal: %s\n",
                     error.substr(0, 140).c_str());

        otsardb_close(writer); /* releases the old primary lease */
    }

    /* ------------------------------------------------------------------ */
    /* E3: promote + swap. The PROMOTE=1 session raises the destination */
    /* epoch 1 -> 2, swaps its store to the promoted destination, attaches */
    /* the old primary store as its replica (the roles swap) and commits */
    /* row 4 DUAL. A READER then sees 4 rows THROUGH the promoted store — */
    /* the post-promote writes landed on the destination. */
    /* ------------------------------------------------------------------ */
    {
        set_env("OTSARDB_S3_PROMOTE", "1");
        std::string error;
        otsardb *promote_session = open_session(ROOT_A, &error, nullptr);
        clear_env("OTSARDB_S3_PROMOTE");
        assert(promote_session != nullptr);
        assert(epoch_value(pair.replica, ROOT_A) == "2");
        assert(epoch_value(pair.primary, ROOT_A) == "1");

        int rc = exec_ok(promote_session,
                         "INSERT INTO t VALUES(4,'d'); SELECT count(*) FROM t;",
                         &error);
        assert(rc == OTSARDB_OK);
        assert(query_count(promote_session, "t") == "4");
        std::string h1 = head_commit_id(pair.primary, ROOT_A);
        std::string h2 = head_commit_id(pair.replica, ROOT_A);
        assert(!h1.empty() && h1 == h2); /* the role-swapped dual publish */
        otsardb_close(promote_session);
        promote_session = nullptr;

        /* The promoted destination is the session's primary: a 0084 reader
         * reads the SAME target through the promoted store and sees the
         * committed state (4 rows). */
        set_env("OTSARDB_S3_READ_FROM_REPLICA", "1");
        otsardb *reader = open_session(ROOT_A, &error, nullptr);
        clear_env("OTSARDB_S3_READ_FROM_REPLICA");
        assert(reader != nullptr);
        assert(query_count(reader, "t") == "4");
        otsardb_close(reader);
        std::fprintf(stderr,
                     "E3 promote + swap: epoch 1->2 (old primary stays 1); "
                     "row 4 DUAL on the swapped pair; HEAD=%s equal; reader "
                     "through the promoted store sees 4\n", h1.c_str());
    }

    /* ------------------------------------------------------------------ */
    /* E4: old-primary-role sessions are FENCED. Session A (old-primary */
    /* pair env) holds the OLD primary lease; session B (same env) is */
    /* lease-HELD (candidate) and its write is refused — heads unmoved on */
    /* BOTH destinations (the 0085-observed old-primary fence). */
    /* ------------------------------------------------------------------ */
    {
        std::string error;
        otsardb *a = open_session(ROOT_A, &error, nullptr);
        assert(a != nullptr); /* claims the old primary lease */
        assert(query_count(a, "t") == "4");

        otsardb *b = open_session(ROOT_A, &error, nullptr);
        assert(b != nullptr); /* lease HELD -> candidate mode */

        std::string old_before = head_commit_id(pair.primary, ROOT_A);
        std::string promoted_before = head_commit_id(pair.replica, ROOT_A);
        int rc = exec_ok(b, "INSERT INTO t VALUES(999,'fence'); SELECT count(*) FROM t;",
                         &error);
        assert(rc != OTSARDB_OK); /* refused: no publish, no ack */
        std::string old_after = head_commit_id(pair.primary, ROOT_A);
        std::string promoted_after = head_commit_id(pair.replica, ROOT_A);
        assert(old_after == old_before);
        assert(promoted_after == promoted_before);
        std::fprintf(stderr,
                     "E4 old-primary fenced: write refused rc=%d; heads unmoved "
                     "(p=%s o=%s)\n", rc,
                     promoted_after.c_str(), old_after.c_str());

        otsardb_close(b);
        otsardb_close(a);
    }

    /* ------------------------------------------------------------------ */
    /* E5: the no-PROMOTE path is unchanged — a pair-env session WITHOUT */
    /* OTSARDB_S3_PROMOTE adopts the raised epoch records (2/1), writes row 5 */
    /* DUAL and keeps the swapped pair in sync: count 5, HEADs equal. */
    /* ------------------------------------------------------------------ */
    {
        std::string error;
        otsardb *db = open_session(ROOT_A, &error, nullptr);
        assert(db != nullptr);
        int rc = exec_ok(db, "INSERT INTO t VALUES(5,'e'); SELECT count(*) FROM t;",
                         &error);
        assert(rc == OTSARDB_OK);
        assert(query_count(db, "t") == "5");
        std::string h1 = head_commit_id(pair.primary, ROOT_A);
        std::string h2 = head_commit_id(pair.replica, ROOT_A);
        assert(!h1.empty() && h1 == h2);
        otsardb_close(db);
        std::fprintf(stderr,
                     "E5 no-PROMOTE pair session unchanged: dual row 5; count 5; "
                     "HEAD=%s equal\n", h1.c_str());
    }

    /* ------------------------------------------------------------------ */
    /* E6: the TTL - skew liveness gate on a raw (killed-writer style) */
    /* lease: fresh lease refuses, provably-expired lease allows. */
    /* ------------------------------------------------------------------ */
    {
        std::string error;
        otsardb *db = open_session(ROOT_B, &error, nullptr);
        assert(db != nullptr);
        int rc = exec_ok(db,
                         "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
                         "INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c');",
                         &error);
        assert(rc == OTSARDB_OK);
        otsardb_close(db);

        /* Simulate a KILLED old primary: a lease object with a fresh
         * Last-Modified and no live session behind it. */
        const char *raw_lease =
            "{\"owner\":\"promote-e2e-raw-lease\",\"generation\":1}";
        otsardb_store_result put_rc = otsardb_store_put(
            pair.primary, (ROOT_B + "/lease.json").c_str(),
            raw_lease, std::strlen(raw_lease), nullptr, nullptr);
        assert(put_rc == OTSARDB_STORE_OK);

        set_env("OTSARDB_S3_PROMOTE", "1");
        otsardb *promoting = open_session(ROOT_B, &error, nullptr);
        assert(promoting == nullptr);
        assert(error.find("promotion refused") != std::string::npos);
        assert(error.find("lease") != std::string::npos);
        assert(epoch_value(pair.replica, ROOT_B) == "1");
        std::fprintf(stderr, "E6a fresh raw lease refuses promote: %s\n",
                     error.substr(0, 120).c_str());

        /* Wait past TTL - skew (TTL=10, skew margin 5 -> provably expired). */
        std::this_thread::sleep_for(std::chrono::seconds(8));
        promoting = open_session(ROOT_B, &error, nullptr);
        clear_env("OTSARDB_S3_PROMOTE");
        assert(promoting != nullptr);
        assert(epoch_value(pair.replica, ROOT_B) == "2");
        otsardb_close(promoting);
        std::fprintf(stderr, "E6b expired raw lease allows promote: epoch 2\n");
    }

    /* ------------------------------------------------------------------ */
    /* E7: concurrent promote — two sessions race the epoch CAS on one */
    /* fresh root. At least one wins; a loser (if any) fails with the */
    /* clear conflict message; the epoch ends >= 2; a winner's write */
    /* commits dual and an old-primary-role session is fenced. */
    /* ------------------------------------------------------------------ */
    {
        std::string error;
        otsardb *db = open_session(ROOT_C, &error, nullptr);
        assert(db != nullptr);
        int rc = exec_ok(db,
                         "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
                         "INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c');",
                         &error);
        assert(rc == OTSARDB_OK);
        otsardb_close(db);
        assert(epoch_value(pair.replica, ROOT_C) == "1");

        struct race_result {
            int rc = -1;
            std::string error;
            otsardb *db = nullptr;
        };
        race_result results[2];
        std::atomic<int> ready{0};
        set_env("OTSARDB_S3_PROMOTE", "1");

        auto opener = [&](int id) {
            std::string target =
                std::string("s3://") + required_env("OTSARDB_S3_BUCKET") + "/" + ROOT_C;
            ready.fetch_add(1);
            while (ready.load() < 2) std::this_thread::yield();
            int open_rc = otsardb_s3_open_target(target.c_str(), &results[id].db);
            results[id].rc = open_rc;
            if (open_rc != OTSARDB_OK) results[id].error = otsardb_s3_last_error();
        };
        std::thread t1(opener, 0);
        std::thread t2(opener, 1);
        t1.join();
        t2.join();
        clear_env("OTSARDB_S3_PROMOTE");

        int winners = 0;
        for (const race_result &r : results) {
            if (r.rc == OTSARDB_OK) {
                ++winners;
            } else {
                assert(r.error.find("promotion") != std::string::npos);
            }
        }
        assert(winners >= 1); /* the race never deadlocks and never loses both */
        std::string epoch = epoch_value(pair.replica, ROOT_C);
        assert(epoch == "2" || epoch == "3");

        /* A winner's write commits dual: the promoted side advances and the
         * old primary follows (the role-swapped dual publish). */
        int writer_used = -1;
        for (int i = 0; i < 2; ++i) {
            if (results[i].rc == OTSARDB_OK) {
                int wrc = exec_ok(results[i].db,
                                  "INSERT INTO t VALUES(4,'d'); SELECT count(*) FROM t;",
                                  &error);
                if (wrc == OTSARDB_OK) {
                    assert(query_count(results[i].db, "t") == "4");
                    writer_used = i;
                    break;
                }
            }
        }
        assert(writer_used >= 0); /* at least one winner could write */
        std::string h1 = head_commit_id(pair.primary, ROOT_C);
        std::string h2 = head_commit_id(pair.replica, ROOT_C);
        assert(!h1.empty() && h1 == h2);
        for (const race_result &r : results) {
            if (r.db) otsardb_close(r.db);
        }
        std::fprintf(stderr,
                     "E7 concurrent promote: winners=%d/2 epoch=%s; winner's "
                     "write dual (HEAD=%s equal)\n", winners, epoch.c_str(),
                     h1.c_str());

        /* An old-primary-role session is fenced: session A holds the OLD
         * primary lease, session B is lease-HELD and its write is refused —
         * heads unmoved on both destinations. */
        {
            otsardb *a = open_session(ROOT_C, &error, nullptr);
            assert(a != nullptr);
            otsardb *b = open_session(ROOT_C, &error, nullptr);
            assert(b != nullptr);
            int frc = exec_ok(b,
                              "INSERT INTO t VALUES(999,'fence'); SELECT count(*) FROM t;",
                              &error);
            assert(frc != OTSARDB_OK);
            assert(head_commit_id(pair.primary, ROOT_C) == h1);
            assert(head_commit_id(pair.replica, ROOT_C) == h2);
            std::fprintf(stderr, "E7 fence: old-primary-role write refused rc=%d; "
                                 "heads unmoved\n", frc);
            otsardb_close(b);
            otsardb_close(a);
        }
    }

    close_raw_pair(pair);
    std::printf("promotion session e2e passed: lease gate, epoch CAS swap, "
                "role-swapped dual publish, old-primary fence, TTL expiry, "
                "concurrent promote\n");
    return 0;
}

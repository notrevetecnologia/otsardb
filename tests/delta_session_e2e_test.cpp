#include "otsardb.h"
#include "otsardb_internal.h"
#include "journal.h"
#include "local_cache.h"
#include "object_store.h"
#include "s3_database.h"
#include "s3_object_store.h"
#include "s3_target.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define OTSARDB_DUP2 _dup2
#define OTSARDB_DUP _dup
#define OTSARDB_CLOSE _close
#define OTSARDB_FILENO _fileno
#else
#include <unistd.h>
#define OTSARDB_DUP2 dup2
#define OTSARDB_DUP dup
#define OTSARDB_CLOSE close
#define OTSARDB_FILENO fileno
#endif

/* live session e2e: the session-level delta materialization
 * wiring in otsardb_s3_open_target_ex (s3_database.cpp) — the three wiring
 * defects fixed in this work (inverted read_state check, wrong base
 * path, fallback structure) plus the inverted delta success check.
 *
 * Drives the REAL session open path against a configured S3 endpoint
 * (OTSARDB_S3_* env; local MinIO in the validation). SKIPS (exit 77) when
 * the primary S3 environment is absent. The database root is unique and
 * self-cleaned at exit.
 *
 * Scenario (on one database root, one cache dir C1 + a fresh control dir
 * C2, both under a scratch temp dir):
 *
 * D1 seed - session with OTSARDB_AUTO_CHECKPOINT=1 +
 * OTSARDB_AUTO_CHECKPOINT_COMMITS=1 (checkpoint on close,
 *) writes 2000 rows; the close saves the
 * complete image to C1 at generation G (cached.generation).
 * D2 commit - a session on the SAME cache dir commits 500 more rows.
 * It runs with OTSARDB_CACHE_REUSE=0 (note):
 * with cache reuse ON the open would take the exact-match
 * cache-probe path (head == cached at its open) and the
 * session's close would re-save the C1 image at the new
 * head — the cache would never be stale and the delta could
 * never be exercised on the reopen. With reuse off the
 * session lazy-restores (incomplete hydrator -> no close
 * save), the C1 image stays at generation G while the
 * remote HEAD advances to H = G+1.
 * D3 delta - reopen with the SAME cache dir, stderr captured:
 * SELECT count(*) == 2500;
 * the always-on diagnostic
 * "otsardb: delta materialization applied generations .."
 * is present and parses to start == G+1, end == H;
 * the C1 image file is byte-identical before vs during the
 * session (the delta reads the cache and never modifies it
 * in place);
 * with OTSARDB_S3_READ_STATS=1 NO "otsardb: read-stats" line
 * is printed (the delta image is complete at head — no
 * hydrator exists, zero hydration GETs by construction).
 * D4 control - the SAME reopen with a FRESH empty cache dir (C2) goes
 * down the full lazy path: count == 2500, the delta
 * diagnostic is ABSENT and the canonical read-stats line
 * IS present (the lazy hydrator ran and reported).
 *
 * Honest note: the session open path cannot be unit-tested
 * without a real store (it builds its own store from the environment), so
 * this e2e is MinIO-gated like otsardb_read_replica_e2e / otsardb_promote_
 * session_e2e; the 20k-row numbers of the acceptance proof are produced by
 * the live CLI runs recorded in the test docs. This test pins the session
 * behaviour at a smaller scale with the same assertions.
 */

namespace {

constexpr int SKIP = 77;

const char *required_env(const char *name) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        std::fprintf(stderr, "missing required environment variable: %s\n", name);
        std::exit(SKIP);
    }
    return value;
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

/* ---- stderr capture (same pattern as lazy_ram_cache_test.c) ------------- */

struct stderr_capture {
    FILE *cap = nullptr;
    int saved_fd = -1;
};

static stderr_capture *g_active_capture = nullptr;

void fail(const char *what, const char *detail = "") {
    /* A failure raised inside an active stderr capture would be swallowed
     * into the capture file: restore the real stderr first. */
    if (g_active_capture) {
        OTSARDB_DUP2(g_active_capture->saved_fd, OTSARDB_FILENO(stderr));
        OTSARDB_CLOSE(g_active_capture->saved_fd);
        g_active_capture = nullptr;
    }
    std::fprintf(stderr, "FAIL: %s%s%s\n", what, detail[0] ? ": " : "", detail);
    std::exit(1);
}

void capture_begin(stderr_capture *c) {
    c->cap = tmpfile();
    if (!c->cap) fail("tmpfile");
    std::fflush(stderr);
    /* Duplicate the CURRENT stderr target (not just its fd number): the
     * dup2 below replaces fd 2, so restoring with the raw number 2 would
     * be a no-op and every later stderr write (FAIL messages included)
     * would land in the capture file. */
    c->saved_fd = OTSARDB_DUP(OTSARDB_FILENO(stderr));
    if (c->saved_fd < 0 || OTSARDB_DUP2(OTSARDB_FILENO(c->cap), OTSARDB_FILENO(stderr)) < 0) {
        fail("dup2(stderr)");
    }
    g_active_capture = c;
}

std::string capture_end(stderr_capture *c) {
    std::fflush(stderr);
    OTSARDB_DUP2(c->saved_fd, OTSARDB_FILENO(stderr));
    OTSARDB_CLOSE(c->saved_fd);
    std::fflush(stderr);
    g_active_capture = nullptr;
    std::rewind(c->cap);
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), c->cap)) > 0) {
        text.append(buf, n);
    }
    std::fclose(c->cap);
    c->cap = nullptr;
    return text;
}

/* ---- helpers ------------------------------------------------------------- */

std::string read_file_bytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    return bytes;
}

std::string query_one(otsardb *db, const char *sql) {
    std::string value;
    char *error = nullptr;
    struct collect_ctx {
        std::string *out;
    } ctx = {&value};
    auto collect = [](void *opaque, int argc, char **argv, char **columns) -> int {
        (void)columns;
        auto *c = static_cast<collect_ctx *>(opaque);
        if (argc != 1 || !argv || !argv[0]) return 1;
        *c->out = argv[0];
        return 0;
    };
    int rc = otsardb_exec(db, sql, collect, &ctx, &error);
    if (error) {
        std::string msg = error;
        otsardb_free(error);
        fail("otsardb_exec", msg.c_str());
    }
    if (rc != OTSARDB_OK) fail("otsardb_exec rc", std::to_string(rc).c_str());
    return value;
}

void exec_ok(otsardb *db, const char *sql) {
    char *error = nullptr;
    int rc = otsardb_exec(db, sql, nullptr, nullptr, &error);
    if (error) {
        std::string msg = error;
        otsardb_free(error);
        fail("otsardb_exec", msg.c_str());
    }
    if (rc != OTSARDB_OK) fail("otsardb_exec rc", std::to_string(rc).c_str());
}

otsardb *open_session(const char *target) {
    otsardb *db = nullptr;
    int rc = otsardb_s3_open_target_ex(target, nullptr, &db);
    if (rc != OTSARDB_OK || !db) {
        const char *msg = otsardb_s3_last_error();
        fail("session open", msg ? msg : "unknown error");
    }
    return db;
}

/* ---- raw store helpers (cleanup + head inspection) ----------------------- */

struct delete_ctx {
    std::vector<std::string> keys;
    int deleted = 0;
};

int delete_cb(const char *key, void *opaque) {
    auto *ctx = static_cast<delete_ctx *>(opaque);
    ctx->keys.emplace_back(key);
    return 0;
}

void delete_prefix(otsardb_object_store *store, const std::string &prefix) {
    delete_ctx ctx;
    (void)otsardb_store_list(store, prefix.c_str(), delete_cb, &ctx);
    for (const std::string &key : ctx.keys) {
        std::string full = prefix + key;
        if (otsardb_store_delete(store, full.c_str()) == OTSARDB_STORE_OK) {
            ++ctx.deleted;
        }
    }
    std::fprintf(stderr, "cleanup: removed %d object(s) under %s\n",
                 ctx.deleted, prefix.c_str());
}

void delete_root(otsardb_object_store *store, const std::string &root) {
    for (const char *suffix : {"/", "/commits/", "/replica/", "/segments/"}) {
        delete_prefix(store, root + suffix);
    }
}

otsardb_object_store *raw_store() {
    otsardb_s3_config config{};
    config.endpoint = required_env("OTSARDB_S3_ENDPOINT");
    config.region = "us-east-1";
    config.bucket = required_env("OTSARDB_S3_BUCKET");
    config.access_key = required_env("OTSARDB_S3_ACCESS_KEY");
    config.secret_key = required_env("OTSARDB_S3_SECRET_KEY");
    config.use_https = std::strncmp(config.endpoint, "http://", 7) != 0;
    otsardb_object_store *store = otsardb_s3_store_create(&config);
    if (!store) fail("raw store create");
    return store;
}

uint64_t remote_head_generation(otsardb_object_store *store, const char *root) {
    otsardb_head head{};
    char *etag = nullptr;
    if (otsardb_journal_read_head(store, root, &head, &etag) != OTSARDB_JOURNAL_OK) {
        fail("journal head read");
    }
    std::free(etag);
    return head.generation;
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *endpoint = required_env("OTSARDB_S3_ENDPOINT");
    const char *bucket = required_env("OTSARDB_S3_BUCKET");
    (void)endpoint;
    required_env("OTSARDB_S3_ACCESS_KEY");
    required_env("OTSARDB_S3_SECRET_KEY");

    /* Never run with replica/quorum env: this test drives plain primary
     * sessions and the sessions would go dual (R5: clean child env). */
    clear_env("OTSARDB_S3_REPLICA_ENDPOINT");
    clear_env("OTSARDB_S3_REPLICA_BUCKET");
    clear_env("OTSARDB_S3_REPLICA_ACCESS_KEY");
    clear_env("OTSARDB_S3_REPLICA_SECRET_KEY");
    clear_env("OTSARDB_S3_QUORUM");
    clear_env("OTSARDB_S3_PROMOTE");
    clear_env("OTSARDB_S3_READ_FROM_REPLICA");

    /* Scratch cache dirs (per-run unique, removed at exit). */
    std::error_code ec;
    const std::string scratch =
        (std::filesystem::temp_directory_path() / "otsardb-delta-session-0097").string();
    const std::string c1 = scratch + "/c1";
    const std::string c2 = scratch + "/c2";
    const std::string c3 = scratch + "/c3";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(c1, ec);
    std::filesystem::create_directories(c2, ec);
    std::filesystem::create_directories(c3, ec);

    const std::string root = "delta-0097/db";
    const std::string target = std::string("s3://") + bucket + "/" + root;

    /* Deterministic database id (s3_target.c): db-<48 hex> of "s3://b/r". */
    otsardb_s3_target parsed{};
    char database_id[OTSARDB_COMMIT_ID_MAX + 1] = {0};
    if (!otsardb_s3_target_parse(target.c_str(), &parsed) ||
        !otsardb_s3_target_database_id(&parsed, database_id)) {
        fail("target parse");
    }
    const std::string cache_image = c1 + "/otsardb-cache-v1/" + database_id + "/state.db";

    otsardb_object_store *store = raw_store();

    /* Self-cleaning: a failed previous run may have left the root behind. */
    delete_root(store, root);

    /* ---- D1 seed: 2000 rows, checkpoint on close -> cache saved at G ---- */
    set_env("OTSARDB_CACHE_DIR", c1.c_str());
    set_env("OTSARDB_AUTO_CHECKPOINT", "1");
    set_env("OTSARDB_AUTO_CHECKPOINT_COMMITS", "1");
    {
        otsardb *db = open_session(target.c_str());
        exec_ok(db, "CREATE TABLE delta_rows(v INTEGER);");
        exec_ok(db, "INSERT INTO delta_rows(v) WITH RECURSIVE cnt(x) AS "
                    "(SELECT 1 UNION ALL SELECT x + 1 FROM cnt WHERE x < 2000) "
                    "SELECT x FROM cnt;");
        otsardb_close(db);
    }
    clear_env("OTSARDB_AUTO_CHECKPOINT");
    clear_env("OTSARDB_AUTO_CHECKPOINT_COMMITS");

    otsardb_local_cache_state cached{};
    if (!otsardb_local_cache_read_state(c1.c_str(), database_id, &cached)) {
        fail("seed did not save a cache image at close (D1)");
    }
    const uint64_t G = cached.generation;
    std::fprintf(stderr, "D1: cache saved at generation %llu\n",
                 static_cast<unsigned long long>(G));

    /* ---- D2 commit: 500 more rows, no checkpoint ------------------------ */
    /* (LH-2 fix): close-time cache save now legitimately
     * happens for a checkpoint-base session (base-covered pages count in
     * the completeness gate), so D2 must NOT write into c1 or its close
     * would advance the cached image to gen 3 and the D3 reopen would take
     * the exact-match probe instead of the delta ladder. D2 therefore uses
     * its own empty cache dir (c2): it commits 500 rows against the remote
     * head, its close-save lands in c2 (correct behaviour, verified below),
     * and c1 is left untouched at generation G - so D3 exercises the delta. */
    set_env("OTSARDB_CACHE_DIR", c2.c_str());
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");
    set_env("OTSARDB_CACHE_REUSE", "0");
    {
        otsardb *db = open_session(target.c_str());
        exec_ok(db, "INSERT INTO delta_rows(v) WITH RECURSIVE cnt(x) AS "
                    "(SELECT 2001 UNION ALL SELECT x + 1 FROM cnt WHERE x < 2500) "
                    "SELECT x FROM cnt;");
        otsardb_close(db);
    }
    clear_env("OTSARDB_AUTO_CHECKPOINT");
    clear_env("OTSARDB_CACHE_REUSE");

    const uint64_t H = remote_head_generation(store, root.c_str());
    if (H <= G) {
        fail("D2 did not advance the remote head above the cached generation");
    }
    std::fprintf(stderr, "D2: remote head advanced to generation %llu, cache stays at %llu\n",
                 static_cast<unsigned long long>(H),
                 static_cast<unsigned long long>(G));

    /* c1 must still be the G image (D2 wrote to c2, not c1). */
    {
        otsardb_local_cache_state still{};
        if (!otsardb_local_cache_read_state(c1.c_str(), database_id, &still) ||
            still.generation != G) {
            std::fprintf(stderr,
                         "FAIL: D2 modified the c1 cache image (observed generation "
                         "%llu, expected %llu)\n",
                         static_cast<unsigned long long>(still.generation),
                         static_cast<unsigned long long>(G));
            std::exit(1);
        }
    }
    /* c2 holds D2's close-saved image at the new head (0144 correct save). */
    {
        otsardb_local_cache_state d2{};
        if (!otsardb_local_cache_read_state(c2.c_str(), database_id, &d2) ||
            d2.generation != H) {
            std::fprintf(stderr,
                         "FAIL: D2 close did not save its image into c2 at gen %llu "
                         "(observed %llu)\n",
                         static_cast<unsigned long long>(H),
                         static_cast<unsigned long long>(d2.generation));
            std::exit(1);
        }
    }

    /* ---- D3 delta reopen on the SAME cache dir -------------------------- */
    set_env("OTSARDB_CACHE_DIR", c1.c_str());
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");
    set_env("OTSARDB_S3_READ_STATS", "1");
    const std::string cache_bytes_before = read_file_bytes(cache_image);
    if (cache_bytes_before.empty()) fail("cache image unreadable before D3");
    std::string d3_stderr;
    std::string d3_count;
    {
        stderr_capture cap;
        capture_begin(&cap);
        otsardb *db = open_session(target.c_str());
        d3_count = query_one(db, "SELECT count(*) FROM delta_rows;");
        const std::string cache_bytes_mid = read_file_bytes(cache_image);
        if (cache_bytes_mid != cache_bytes_before) {
            fail("the delta modified the cached image in place (must be read-only)");
        }
        otsardb_close(db);
        d3_stderr = capture_end(&cap);
    }
    if (d3_count != "2500") {
        fail("D3 count != 2500 (delta image)", d3_count.c_str());
    }
    clear_env("OTSARDB_S3_READ_STATS");
    clear_env("OTSARDB_CACHE_DIR");

    const std::string diag_prefix = "otsardb: delta materialization applied generations ";
    const size_t diag_pos = d3_stderr.find(diag_prefix);
    if (diag_pos == std::string::npos) {
        fail("delta diagnostic missing in D3 reopen", d3_stderr.c_str());
    }
    unsigned long long d_start = 0, d_end = 0, d_count = 0, d_bytes = 0;
    const int parsed_fields = std::sscanf(
        d3_stderr.c_str() + diag_pos,
        "otsardb: delta materialization applied generations %llu..%llu (%llu "
        "generation(s), image %llu bytes)",
        &d_start, &d_end, &d_count, &d_bytes);
    if (parsed_fields != 4) {
        fail("delta diagnostic format unparsable", d3_stderr.c_str());
    }
    if (d_start != G + 1 || d_end != H || d_count != H - G || d_bytes == 0) {
        std::fprintf(stderr,
                     "FAIL: delta diagnostic range %llu..%llu (%llu gen, %llu bytes); "
                     "expected %llu..%llu (%llu gen)\n",
                     d_start, d_end, d_count, d_bytes,
                     static_cast<unsigned long long>(G + 1),
                     static_cast<unsigned long long>(H),
                     static_cast<unsigned long long>(H - G));
        std::exit(1);
    }
    if (d3_stderr.find("otsardb: read-stats ") != std::string::npos) {
        fail("delta session printed a read-stats line (no hydrator must exist)",
             d3_stderr.c_str());
    }
    std::fprintf(stderr,
                 "D3: delta applied generations %llu..%llu (%llu generation(s), "
                 "image %llu bytes), count=%s, cache image untouched, no "
                 "read-stats line\n",
                 d_start, d_end, d_count, d_bytes, d3_count.c_str());

    /* ---- D4 control: same reopen with a FRESH empty cache dir ----------- */
    /* (c2 holds D2's gen-3 image; D4 must start from an empty cache, so it
     * uses c3 - the full lazy path with read-stats, no delta.) */
    set_env("OTSARDB_CACHE_DIR", c3.c_str());
    set_env("OTSARDB_AUTO_CHECKPOINT", "0");
    set_env("OTSARDB_S3_READ_STATS", "1");
    std::string d4_stderr;
    {
        stderr_capture cap;
        capture_begin(&cap);
        otsardb *db = open_session(target.c_str());
        const std::string count = query_one(db, "SELECT count(*) FROM delta_rows;");
        if (count != "2500") {
            fail("D4 count != 2500", count.c_str());
        }
        otsardb_close(db);
        d4_stderr = capture_end(&cap);
    }
    clear_env("OTSARDB_S3_READ_STATS");
    clear_env("OTSARDB_CACHE_DIR");

    if (d4_stderr.find(diag_prefix) != std::string::npos) {
        fail("fresh-cache reopen took the delta path (must be the full lazy path)");
    }
    if (d4_stderr.find("otsardb: read-stats ") == std::string::npos) {
        fail("fresh-cache lazy reopen printed no read-stats line", d4_stderr.c_str());
    }
    std::fprintf(stderr, "D4: fresh-cache lazy reopen count=2500, delta diagnostic "
                         "absent, read-stats line present\n");

    delete_root(store, root);
    otsardb_s3_store_destroy(store);
    std::filesystem::remove_all(scratch, ec);

    std::fprintf(stderr, "PASS: delta session wiring (D1-D4) all green\n");
    return 0;
}

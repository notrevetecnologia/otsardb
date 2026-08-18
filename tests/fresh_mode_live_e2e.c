#include "otsardb.h"
#include "s3_database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#endif

/*
 * — FRESH consistency mode: LIVE cross-process e2e.
 *
 * MinIO-gated (exit 77 without OTSARDB_S3_ENDPOINT; pattern:
 * otsardb_delta_session_e2e). The decisive test this feature demands:
 *
 * setup one session creates the table and seeds one row
 * (generation G) and closes;
 * reader-fresh a SECOND PROCESS opens a FRESH session at G, runs a
 * SELECT (sees the seed), spawns a THIRD PROCESS that
 * commits one more row (generation G+1), waits for it,
 * then runs the NEXT SELECT in the SAME session and
 * must observe the new row (cross-instance visibility
 * without reopen or write attempt);
 * reader-snapshot control: a SESSION PINNED AT OPEN (default mode)
 * opened at G' runs a SELECT, an external process
 * advances the store, and the NEXT SELECT must NOT see
 * the new row (pre-0123 snapshot contract unchanged).
 *
 * The reader processes set OTSARDB_S3_SINGLE_WRITER=1 so they never claim
 * the writer lease (they are pure readers); the writer child claims and
 * releases the lease normally. Everything else runs the REAL session
 * path (materialisation ladder, publisher, FRESH pre-exec hook).
 *
 * Usage:
 * otsardb_fresh_mode_live_e2e setup <s3-root>
 * otsardb_fresh_mode_live_e2e writer <s3-root> <value>
 * otsardb_fresh_mode_live_e2e reader-fresh <s3-root>
 * otsardb_fresh_mode_live_e2e reader-snapshot <s3-root>
 */

static const char *g_self_path = "";

static void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static int collect_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (argc != 1 || !argv || !argv[0]) return 1;
    snprintf((char *)ctx, 128, "%s", argv[0]);
    return 0;
}

static void exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    int rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "SQL failed: %s\n  %s\n",
                error_message ? error_message : otsardb_errmsg(db), sql);
        otsardb_free(error_message);
        exit(1);
    }
    otsardb_free(error_message);
}

static void query_one(otsardb *db, const char *sql, char *out) {
    char *error_message = NULL;
    out[0] = '\0';
    int rc = otsardb_exec(db, sql, collect_text, out, &error_message);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "SQL failed: %s\n  %s\n",
                error_message ? error_message : otsardb_errmsg(db), sql);
        otsardb_free(error_message);
        exit(1);
    }
    otsardb_free(error_message);
}

static otsardb *open_target(const char *target) {
    otsardb *db = NULL;
    int rc = otsardb_s3_open_target_ex(target, NULL, &db);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "open failed: %s\n", otsardb_s3_last_error());
        exit(1);
    }
    return db;
}

/* Spawn this very binary as a child process and wait for its exit code. */
static int spawn_and_wait(const char *mode, const char *root, const char *value) {
    char cmd[2048];
    if (value && value[0]) {
        snprintf(cmd, sizeof(cmd), "\"%s\" %s %s %s", g_self_path, mode, root, value);
    } else {
        snprintf(cmd, sizeof(cmd), "\"%s\" %s %s", g_self_path, mode, root);
    }
    int rc = system(cmd);
    if (rc == -1) return -1;
#if defined(_WIN32)
    return rc;
#else
    return WEXITSTATUS(rc);
#endif
}

static int run_setup(const char *root) {
    otsardb *db = open_target(root);
    exec_ok(db, "CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v TEXT);");
    exec_ok(db, "INSERT INTO t(v) VALUES('seed');");
    char result[128];
    query_one(db, "SELECT count(*) FROM t;", result);
    otsardb_close(db);
    printf("setup: seed committed, count=%s\n", result);
    return strcmp(result, "1") == 0 ? 0 : 1;
}

static int run_writer(const char *root, const char *value) {
    if (!value || !value[0]) {
        fprintf(stderr, "writer mode requires a value\n");
        return 1;
    }
    otsardb *db = open_target(root);
    char sql[256];
    snprintf(sql, sizeof(sql), "INSERT INTO t(v) VALUES('%s');", value);
    exec_ok(db, sql);
    otsardb_close(db);
    return 0;
}

static int run_reader_fresh(const char *root) {
    set_env("OTSARDB_CONSISTENCY", "fresh");
    /* Pure reader: never claims the writer lease. */
    set_env("OTSARDB_S3_SINGLE_WRITER", "1");

    otsardb *db = open_target(root);
    char before[128];
    query_one(db, "SELECT count(*) FROM t;", before);
    printf("fresh reader: baseline count=%s\n", before);

    int rc = spawn_and_wait("writer", root, "fresh-two");
    if (rc != 0) {
        fprintf(stderr, "writer child failed (rc=%d)\n", rc);
        otsardb_close(db);
        return 1;
    }

    /* The decisive statement: the SAME open session, the NEXT exec, must
     * observe the other process's commit. */
    char after[128];
    query_one(db, "SELECT count(*) FROM t;", after);
    char found[128];
    query_one(db, "SELECT count(*) FROM t WHERE v='fresh-two';", found);
    otsardb_close(db);

    int count_before = atoi(before);
    int count_after = atoi(after);
    printf("fresh reader: after external commit count=%s (row found=%s)\n",
           after, found);
    if (count_after != count_before + 1 || strcmp(found, "1") != 0) {
        fprintf(stderr, "FRESH live e2e FAILED: expected %d rows and the new "
                        "row visible, got %s / found=%s\n",
                count_before + 1, after, found);
        return 1;
    }
    puts("FRESH live e2e PASSED: cross-process commit visible to the next "
         "statement of an open fresh session");
    return 0;
}

static int run_reader_snapshot(const char *root) {
    set_env("OTSARDB_CONSISTENCY", NULL);
    set_env("OTSARDB_S3_SINGLE_WRITER", "1");

    otsardb *db = open_target(root);
    char before[128];
    query_one(db, "SELECT count(*) FROM t;", before);
    printf("snapshot reader: baseline count=%s\n", before);

    int rc = spawn_and_wait("writer", root, "snap-two");
    if (rc != 0) {
        fprintf(stderr, "writer child failed (rc=%d)\n", rc);
        otsardb_close(db);
        return 1;
    }

    char after[128];
    query_one(db, "SELECT count(*) FROM t;", after);
    otsardb_close(db);

    printf("snapshot reader: after external commit count=%s (pinned)\n", after);
    if (strcmp(before, after) != 0) {
        fprintf(stderr, "SNAPSHOT live e2e FAILED: the pinned session saw the "
                        "external commit (%s -> %s)\n",
                before, after);
        return 1;
    }
    puts("SNAPSHOT live e2e PASSED: session pinned at open does not see the "
         "external commit (default mode unchanged)");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 0) g_self_path = argv[0];

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <setup|writer|reader-fresh|reader-snapshot> <s3-root> [value]\n",
                argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const char *root = argv[2];
    const char *value = argc > 3 ? argv[3] : NULL;

    /* MinIO-gated: exit 77 (SKIP) without the S3 environment. */
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    if (!endpoint || !endpoint[0]) {
        fprintf(stderr, "SKIP: OTSARDB_S3_ENDPOINT is not set\n");
        return 77;
    }

    if (strcmp(mode, "setup") == 0) return run_setup(root);
    if (strcmp(mode, "writer") == 0) return run_writer(root, value);
    if (strcmp(mode, "reader-fresh") == 0) return run_reader_fresh(root);
    if (strcmp(mode, "reader-snapshot") == 0) return run_reader_snapshot(root);
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}

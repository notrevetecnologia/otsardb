/*
 * perf_bench.c - local SQL throughput benchmark for OtsarDB.
 *
 * Measures rows/sec for single-row autocommit inserts, batched inserts,
 * single-row updates, single-row deletes and a full table scan against a
 * local database file. Output is line-oriented "metric=<name> rows_per_sec=<n>"
 * so scripts can parse it.
 *
 * Throughput mode: the benchmark sets PRAGMA journal_mode=MEMORY and
 * PRAGMA synchronous=OFF. Every autocommit statement otherwise creates a
 * rollback journal and fsyncs (3-30 ms per statement on Windows), which turns
 * the 80k-statement run into a 30-45 minute silent grind that looks like a
 * hang. This benchmark measures SQL engine throughput; crash durability is
 * covered by the S3 WAL path and its tests, not by this bench.
 *
 * Unlike the test binaries this never aborts on normal runs: every failure
 * is reported on stderr and surfaces as exit code 1. Each phase also has a
 * wall-clock budget: if a phase exceeds PHASE_TIMEOUT_MS the bench aborts
 * with a clear error instead of hanging silently. The bench never reads
 * stdin, so it can never block on terminal input.
 */

#include "otsardb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#define otsardb_unlink _unlink
#else
#include <unistd.h>
#define otsardb_unlink unlink
#endif

#define BENCH_DB_PATH "otsardb-perf-bench.db"
#define BENCH_WAL_PATH "otsardb-perf-bench.db-wal"
#define BENCH_SHM_PATH "otsardb-perf-bench.db-shm"
#define BENCH_JOURNAL_PATH "otsardb-perf-bench.db-journal"

#define INSERT_COUNT 20000u
#define TX_COUNT 20u
#define TX_ROW_COUNT 1000u
#define UPDATE_COUNT 20000u
#define DELETE_COUNT 20000u
#define SCAN_ROW_COUNT 20000u

/* Safety budget per phase, not a performance target: with the benchmark
 * PRAGMAs the phases complete in ~1 s each on a normal machine. */
#define PHASE_TIMEOUT_MS 120000.0
#define TIMEOUT_CHECK_INTERVAL 1000u

typedef struct bench_metric {
    const char *name;
    unsigned long long rows;
    double elapsed_ms;
} bench_metric;

static double now_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER frequency = {0};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
#endif
}

static void print_metric(const bench_metric *metric) {
    double rows_per_sec =
        metric->elapsed_ms > 0.0 ? (double)metric->rows * 1000.0 / metric->elapsed_ms : 0.0;
    printf("metric=%s rows_per_sec=%.0f\n", metric->name, rows_per_sec);
    fflush(stdout);
}

static void remove_db_files(void) {
    const char *const files[] = {
        BENCH_DB_PATH,
        BENCH_WAL_PATH,
        BENCH_SHM_PATH,
        BENCH_JOURNAL_PATH,
    };
    size_t i;
    for (i = 0u; i < sizeof(files) / sizeof(files[0]); ++i) {
        if (otsardb_unlink(files[i]) != 0 && errno != ENOENT) {
            /* Windows refuses to delete files still held open by another
             * process (e.g. a leftover from a killed run). Report but keep
             * going: the other unlinks may still succeed. */
            fprintf(stderr, "perf_bench: warning: could not remove %s\n", files[i]);
        }
    }
}

static int fail(otsardb *db, const char *message) {
    fprintf(stderr, "perf_bench: %s\n", message);
    if (db) otsardb_close(db);
    remove_db_files();
    return 1;
}

static int exec_ok(otsardb *db, const char *sql) {
    char *error_message = NULL;
    int rc = otsardb_exec(db, sql, NULL, NULL, &error_message);
    if (rc != OTSARDB_OK) {
        fprintf(stderr, "perf_bench: sql failed (%d): %s\n    %s\n",
                rc, error_message ? error_message : otsardb_errmsg(db), sql);
        otsardb_free(error_message);
        return 0;
    }
    otsardb_free(error_message);
    return 1;
}

static int phase_expired(const char *phase, double started_ms) {
    if (now_ms() - started_ms > PHASE_TIMEOUT_MS) {
        fprintf(stderr,
                "perf_bench: phase '%s' exceeded the %.0f s time budget; aborting "
                "instead of hanging\n",
                phase, PHASE_TIMEOUT_MS / 1000.0);
        return 1;
    }
    return 0;
}

int main(void) {
    const double total_start_ms = now_ms();
    remove_db_files();

    if (otsardb_initialize() != OTSARDB_OK) {
        fprintf(stderr, "perf_bench: otsardb_initialize failed\n");
        return 1;
    }

    otsardb *db = NULL;
    if (otsardb_open(BENCH_DB_PATH, &db) != OTSARDB_OK) {
        fprintf(stderr,
                "perf_bench: otsardb_open(%s) failed\n"
                "perf_bench: check that no other process (e.g. a leftover process from a\n"
                "perf_bench: previous killed run) still holds %s, and that no stale\n"
                "perf_bench: %s or %s files exist in the working directory\n",
                BENCH_DB_PATH, BENCH_DB_PATH, BENCH_JOURNAL_PATH, BENCH_WAL_PATH);
        remove_db_files();
        return 1;
    }

    /* See the file header: MEMORY journal + synchronous=OFF keeps the bench
     * measuring engine throughput instead of per-statement disk fsync. */
    if (!exec_ok(db, "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;")) {
        return fail(db, "failed to set benchmark PRAGMAs");
    }

    if (!exec_ok(db, "DROP TABLE IF EXISTS t;") ||
        !exec_ok(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, value TEXT);")) {
        return fail(db, "failed to prepare table");
    }

    char sql[256];
    unsigned long long i;
    double start_ms;
    bench_metric metric;

    /* (a) 20000 single-row autocommit inserts. */
    fprintf(stderr, "perf_bench: phase insert_autocommit: 20000 single-row autocommit inserts\n");
    start_ms = now_ms();
    for (i = 1u; i <= INSERT_COUNT; ++i) {
        snprintf(sql, sizeof(sql), "INSERT INTO t(value) VALUES('v%llu');", i);
        if (!exec_ok(db, sql)) return fail(db, "autocommit insert failed");
        if ((i % TIMEOUT_CHECK_INTERVAL) == 0u && phase_expired("insert_autocommit", start_ms)) {
            return fail(db, "insert_autocommit exceeded its time budget");
        }
    }
    metric.name = "insert_autocommit";
    metric.rows = INSERT_COUNT;
    metric.elapsed_ms = now_ms() - start_ms;
    print_metric(&metric);

    /* (b) 20000 inserts inside 20 transactions of 1000 rows each. */
    fprintf(stderr, "perf_bench: phase insert_batched: 20 transactions of 1000 rows each\n");
    start_ms = now_ms();
    for (unsigned long long batch = 0u; batch < TX_COUNT; ++batch) {
        if (!exec_ok(db, "BEGIN;")) return fail(db, "BEGIN failed");
        for (unsigned long long row = 0u; row < TX_ROW_COUNT; ++row) {
            i = INSERT_COUNT + batch * TX_ROW_COUNT + row + 1u;
            snprintf(sql, sizeof(sql), "INSERT INTO t(value) VALUES('v%llu');", i);
            if (!exec_ok(db, sql)) return fail(db, "batched insert failed");
        }
        if (!exec_ok(db, "COMMIT;")) return fail(db, "COMMIT failed");
        if (phase_expired("insert_batched", start_ms)) {
            return fail(db, "insert_batched exceeded its time budget");
        }
    }
    metric.name = "insert_batched";
    metric.rows = INSERT_COUNT;
    metric.elapsed_ms = now_ms() - start_ms;
    print_metric(&metric);

    /* (c) 20000 single-row updates on the first 20000 ids. */
    fprintf(stderr, "perf_bench: phase update: 20000 single-row updates\n");
    start_ms = now_ms();
    for (i = 1u; i <= UPDATE_COUNT; ++i) {
        snprintf(sql, sizeof(sql), "UPDATE t SET value=value||'x' WHERE id=%llu;", i);
        if (!exec_ok(db, sql)) return fail(db, "update failed");
        if ((i % TIMEOUT_CHECK_INTERVAL) == 0u && phase_expired("update", start_ms)) {
            return fail(db, "update exceeded its time budget");
        }
    }
    metric.name = "update";
    metric.rows = UPDATE_COUNT;
    metric.elapsed_ms = now_ms() - start_ms;
    print_metric(&metric);

    /* (d) 20000 single-row deletes of the same ids, leaving 20000 rows. */
    fprintf(stderr, "perf_bench: phase delete: 20000 single-row deletes\n");
    start_ms = now_ms();
    for (i = 1u; i <= DELETE_COUNT; ++i) {
        snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id=%llu;", i);
        if (!exec_ok(db, sql)) return fail(db, "delete failed");
        if ((i % TIMEOUT_CHECK_INTERVAL) == 0u && phase_expired("delete", start_ms)) {
            return fail(db, "delete exceeded its time budget");
        }
    }
    metric.name = "delete";
    metric.rows = DELETE_COUNT;
    metric.elapsed_ms = now_ms() - start_ms;
    print_metric(&metric);

    /* (e) SELECT count(*) warm-up, then a full scan of the 20000 rows. */
    fprintf(stderr, "perf_bench: phase select: full table scan of 20000 rows\n");
    if (!exec_ok(db, "SELECT count(*) FROM t;")) return fail(db, "select count(*) failed");
    start_ms = now_ms();
    if (!exec_ok(db, "SELECT count(*), sum(length(value)) FROM t;")) {
        return fail(db, "full scan select failed");
    }
    if (phase_expired("select", start_ms)) {
        return fail(db, "select exceeded its time budget");
    }
    metric.name = "select";
    metric.rows = SCAN_ROW_COUNT;
    metric.elapsed_ms = now_ms() - start_ms;
    print_metric(&metric);

    printf("total_elapsed_ms=%.1f\n", now_ms() - total_start_ms);
    printf("bench_ok=1\n");
    fflush(stdout);

    otsardb_close(db);
    remove_db_files();
    return 0;
}

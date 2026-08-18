/* health_contract_posix_test.c — POSIX sibling of the
 * readiness-contract CTest.
 *
 * Drives the otsardb CLI against a live S3 endpoint (local MinIO or any
 * OTSARDB_S3_* configured provider), the same scenario as 0061:
 * 1. create a database through the CLI (normal write open);
 * 2. `otsardb --health <target>` -> exit 0, JSON {"status":"ok",...}
 * with s3_reachable/open_ms/engine/target present;
 * 3. `otsardb --health-json <target>` -> same (alias);
 * 4. without S3 credentials -> nonzero exit, JSON error;
 * 5. (POSIX-only extra) wrong credentials -> nonzero exit, JSON error.
 *
 * POSIX-only by design (R4: never touch the Windows path of the 0061
 * test). On _WIN32 this binary is a compile-time stub that returns 77
 * (SKIP) so wiring it in CMakeLists cannot break the Windows CTest set.
 *
 * Spawning difference vs 0061: children are forked and exec'd directly
 * (no /bin/sh script, no system()). The 0061 driver's POSIX branch
 * builds one shell command line per phase with the SQL UNQUOTED
 * (`"exe" "target" DROP TABLE ... (...);` — the %s placeholders for
 * flag and SQL carry no quotes in its snprintf format) and hands it to
 * a temp script with a hardcoded ".cmd" suffix; dash then dies on the
 * SQL's "(" with "Syntax error". That branch has therefore never run
 * anywhere (Linux ctest skipped it for lack of store env, 0067), and
 * "the same scenario" here is re-implemented quote-free via execv so
 * shell metacharacters in the SQL are irrelevant.
 *
 * Phase 4 watchdog: the CLI is killed with SIGKILL after
 * OTSARDB_HEALTH_WATCHDOG_SEC (default 60) if it does not exit. On Linux
 * TODAY the credential-less probe does NOT exit at all (OBSERVED
 * >= 284 s, killed by the harness): with no explicit credentials,
 * s3_object_store.cpp:826-839 builds miniocpp's ChainedProvider whose
 * IamAwsProvider probes the AWS instance metadata endpoint
 * (169.254.169.254); on Linux that connect never fails fast (Windows
 * returns exit 2 within ~10 s per). A phase-4 watchdog
 * kill is therefore a FAIL, not a pass: the readiness contract
 * (0061 invariant 3: every failure exits nonzero WITH JSON) is broken
 * on Linux by that engine path, and the red result is the contract
 * test doing its job. The engine fix is a separate change (src/ is
 * out of scope for 0075). Phase 5 (invalid credentials, non-empty) is
 * bounded today (OBSERVED exit 2 in ~1 s on Linux) and keeps a
 * negative-case signal in the Linux CTest set meanwhile.
 *
 * Environment: OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET required, else SKIP
 * (exit 77) so plain ctest without a live store stays clean (R2).
 * Phase 4 snapshots and clears OTSARDB_S3_* and AWS_* credential names
 * and restores them afterwards so no ambient credential leaks (R5).
 *
 * Expected CTest registration (coordinator wiring):
 * add_executable(otsardb_health_contract_posix_check tests/health_contract_posix_test.c)
 * add_test(NAME otsardb_health_contract_posix
 * COMMAND otsardb_health_contract_posix_check $<TARGET_FILE:otsardb>)
 * set_tests_properties(otsardb_health_contract_posix PROPERTIES
 * SKIP_RETURN_CODE 77 TIMEOUT 120)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
int main(void) {
    printf("SKIPPED: otsardb_health_contract_posix is a POSIX-only test "
           "(Windows covered by otsardb_health_contract from the readiness contract)\n");
    return 77;
}
#else /* POSIX implementation */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUTBUF_CAP (16 * 1024)
#define WATCHDOG_SEC 60

static int g_watchdog = WATCHDOG_SEC;

static const char *const cred_env[] = {
    "OTSARDB_S3_ACCESS_KEY", "OTSARDB_S3_SECRET_KEY", "OTSARDB_S3_SESSION_TOKEN",
    "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
};
#define CRED_COUNT ((int)(sizeof(cred_env) / sizeof(cred_env[0])))

static void make_scratch(char *buf, size_t cap, const char *suffix) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-health-0075-%lu%s", dir,
             (unsigned long)getpid(), suffix);
}

/* Runs: <exe> [flag] <target> [sql] > <outfile> 2>&1
 * via fork + execv (no shell involved). Returns the child exit code,
 * or -3 on exec failure, -2 on fork/pipe failure; *timed_out is set
 * when the watchdog had to SIGKILL the child. */
static int run_cli(const char *exe, const char *flag, const char *target,
                   const char *sql, char *outbuf, size_t outcap,
                   int *timed_out) {
    char outfile[1024];
    char *argv[8];
    int argc = 0;
    argv[argc++] = (char *)exe;
    if (flag && flag[0]) argv[argc++] = (char *)flag;
    argv[argc++] = (char *)target;
    if (sql && sql[0]) argv[argc++] = (char *)sql;
    argv[argc] = NULL;

    make_scratch(outfile, sizeof(outfile), ".log");
    unlink(outfile);
    *timed_out = 0;

    pid_t pid = fork();
    if (pid < 0) return -2;
    if (pid == 0) {
        int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) _exit(127);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
        execv(exe, argv);
        _exit(126); /* exec failure: never returns normally */
    }

    int status = 0;
    for (int waited = 0; ; ) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -2;
        if (waited >= g_watchdog) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            *timed_out = 1;
            break;
        }
        usleep(100 * 1000);
        waited += 1;
    }

    if (outbuf && outcap) {
        outbuf[0] = '\0';
        FILE *r = fopen(outfile, "rb");
        if (r) {
            size_t got = fread(outbuf, 1, outcap - 1, r);
            outbuf[got] = '\0';
            fclose(r);
        }
    }
    unlink(outfile);

    if (*timed_out) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int has(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static void snapshot_creds(char (*saved)[256], int *set) {
    for (int i = 0; i < CRED_COUNT; ++i) {
        set[i] = 0;
        saved[i][0] = '\0';
        const char *v = getenv(cred_env[i]);
        if (v && v[0]) {
            set[i] = 1;
            snprintf(saved[i], 256, "%s", v);
        }
    }
}

static void clear_creds(void) {
    for (int i = 0; i < CRED_COUNT; ++i) unsetenv(cred_env[i]);
}

static void restore_creds(const char (*saved)[256], const int *set) {
    for (int i = 0; i < CRED_COUNT; ++i) {
        if (set[i]) setenv(cred_env[i], saved[i], 1);
        else unsetenv(cred_env[i]);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("SKIPPED: usage: otsardb_health_contract_posix_check <path-to-otsardb-cli>\n");
        return 77;
    }
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint || !endpoint[0] || !bucket || !bucket[0]) {
        printf("SKIPPED: OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET not set (no live store)\n");
        return 77;
    }
    const char *exe = argv[1];
    const char *wenv = getenv("OTSARDB_HEALTH_WATCHDOG_SEC");
    if (wenv && wenv[0]) {
        int v = atoi(wenv);
        if (v > 0) g_watchdog = v;
    }
    char target[512];
    snprintf(target, sizeof(target), "s3://%s/health-0075/db", bucket);
    char out[OUTBUF_CAP];
    int rc, timed_out, failed = 0;

    /* Phase 1: create the database through the CLI (normal write open).
     * Idempotent on purpose: the target may persist between runs. */
    const char *create_sql =
        "DROP TABLE IF EXISTS health_t; "
        "CREATE TABLE health_t(id INTEGER PRIMARY KEY, v TEXT); "
        "INSERT INTO health_t(v) VALUES('0075');";
    rc = run_cli(exe, NULL, target, create_sql, out, sizeof(out), &timed_out);
    printf("health-0075 phase1 create: exit %d\n", rc);
    if (timed_out) {
        printf("FAIL [phase1 create]: CLI watchdog-killed after %d s\n%s\n", WATCHDOG_SEC, out);
        return 1;
    }
    if (rc != 0) {
        printf("FAIL [phase1 create]: otsardb CLI could not create the database.\n%s\n", out);
        return 1;
    }

    /* Phase 2: --health -> exit 0, JSON status ok with all contract fields. */
    rc = run_cli(exe, "--health", target, NULL, out, sizeof(out), &timed_out);
    printf("health-0075 phase2 --health: exit %d\n", rc);
    if (timed_out || rc != 0 || !has(out, "\"status\":\"ok\"") ||
        !has(out, "\"s3_reachable\":true") || !has(out, "\"open_ms\":") ||
        !has(out, "\"engine\":") || !has(out, "\"target\":")) {
        printf("FAIL [phase2 --health]: expected exit 0 + JSON status ok with "
               "s3_reachable/open_ms/engine/target.\n%s\n", out);
        failed = 1;
    }

    /* Phase 3: --health-json alias behaves identically. */
    rc = run_cli(exe, "--health-json", target, NULL, out, sizeof(out), &timed_out);
    printf("health-0075 phase3 --health-json: exit %d\n", rc);
    if (timed_out || rc != 0 || !has(out, "\"status\":\"ok\"")) {
        printf("FAIL [phase3 --health-json]: alias must behave like --health.\n%s\n", out);
        failed = 1;
    }

    /* Phase 4: without credentials -> nonzero exit + JSON error.
     * KNOWN LINUX GAP: today the credential-less probe
     * never exits on Linux (miniocpp IamAwsProvider / IMDS lookup,
     * s3_object_store.cpp:826-839; OBSERVED >= 284 s, killed here), so
     * a watchdog kill is a FAIL by design — the contract requires a
     * prompt nonzero exit with JSON. */
    char saved[CRED_COUNT][256];
    int saved_set[CRED_COUNT];
    snapshot_creds(saved, saved_set);
    clear_creds();
    rc = run_cli(exe, "--health", target, NULL, out, sizeof(out), &timed_out);
    restore_creds(saved, saved_set);
    if (timed_out) {
        printf("FAIL [phase4 no-credentials]: CLI did not exit within %d s "
               "(watchdog SIGKILL). Known Linux engine gap: no-credential path "
               "hangs in miniocpp ChainedProvider/IamAwsProvider IMDS lookup "
               "(s3_object_store.cpp:826-839); Windows exits 2 in ~10 s "
               ". Contract invariant 3 broken on Linux; "
               "engine fix pending in a separate change.\n", g_watchdog);
        failed = 1;
    } else {
        printf("health-0075 phase4 no-credentials: exit %d\n", rc);
        if (rc == 0 || !has(out, "\"status\":\"error\"")) {
            printf("FAIL [phase4 no-credentials]: expected nonzero exit + JSON status error.\n%s\n", out);
            failed = 1;
        }
    }

    /* Phase 5 (POSIX-only extra): wrong-but-present credentials -> nonzero
     * exit + JSON error. Bounded on Linux today (OBSERVED exit 2 in ~1 s,
     *) and keeps a negative-case signal in the Linux set
     * while phase 4 is blocked by the engine gap. */
    setenv("OTSARDB_S3_ACCESS_KEY", "definitely-wrong-key-0075", 1);
    setenv("OTSARDB_S3_SECRET_KEY", "definitely-wrong-secret-0075", 1);
    rc = run_cli(exe, "--health", target, NULL, out, sizeof(out), &timed_out);
    printf("health-0075 phase5 wrong-credentials: exit %d\n", rc);
    if (timed_out || rc == 0 || !has(out, "\"status\":\"error\"")) {
        printf("FAIL [phase5 wrong-credentials]: expected nonzero exit + JSON status error.\n%s\n", out);
        failed = 1;
    }

    if (failed) {
        printf("FAIL: otsardb --health POSIX readiness contract on %s "
               "(endpoint %s) — see per-phase messages\n", target, endpoint);
        return 1;
    }
    printf("PASS: otsardb --health POSIX readiness contract (create, ok, alias, "
           "no-credential error, wrong-credential error) on %s (endpoint %s)\n",
           target, endpoint);
    return 0;
}
#endif /* POSIX implementation */

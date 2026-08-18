/* health_contract_check.c — readiness-contract CTest.
 *
 * Drives the otsardb CLI against a live S3 endpoint (local MinIO or any
 * OTSARDB_S3_* configured provider):
 * 1. create a database through the CLI (normal write open);
 * 2. `otsardb --health <target>` -> exit 0, JSON {"status":"ok",...}
 * with s3_reachable/open_ms/engine/target present;
 * 3. `otsardb --health-json <target>` -> same (alias);
 * 4. without S3 credentials -> nonzero exit, JSON error.
 *
 * The test SKIPS (exit 77) when OTSARDB_S3_ENDPOINT or OTSARDB_S3_BUCKET are
 * absent, so a plain `ctest` run without a live store still reports
 * cleanly; the harness that starts the store runs this test with those
 * variables set. Phase 4 clears both OTSARDB_S3_* and AWS_* credential
 * names so no ambient credential leaks into the negative case (R5), and
 * restores them afterwards.
 *
 * Children are spawned via system() (cmd.exe /bin/sh), which inherits
 * this process environment; output is captured through a temp file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define OUTBUF_CAP (16 * 1024)

static const char *const cred_env[] = {
    "OTSARDB_S3_ACCESS_KEY", "OTSARDB_S3_SECRET_KEY", "OTSARDB_S3_SESSION_TOKEN",
    "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
};
#define CRED_COUNT ((int)(sizeof(cred_env) / sizeof(cred_env[0])))

static int system_exit_code(int rc) {
#if defined(_WIN32)
    return rc;
#else
    return rc >= 0 ? WEXITSTATUS(rc) : -1;
#endif
}

static void make_scratch(char *buf, size_t cap, const char *ext) {
#if defined(_WIN32)
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(buf, cap, "%s\\otsardb-health-0061-%lu%s", dir, (unsigned long)getpid(), ext);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-health-0061-%lu%s", dir, (unsigned long)getpid(), ext);
#endif
}

/* Runs: "<exe>" <flag> "<target>" <sql> > <outfile> 2>&1
 * Returns the child's exit code and fills outbuf with the merged output.
 * flag may be "" (plain SQL mode); sql may be "" (no trailing argument).
 *
 * The command is written to a temporary script (.bat on Windows, .sh
 * elsewhere) and executed through that script, instead of being passed on
 * the cmd.exe /c command line: the CRT's system() passes the string to
 * `cmd /c <string>`, and cmd's quote-stripping rule for /c lines corrupts
 * any command containing internal quotes, which all of ours do. Batch/sh
 * parsing does not apply that rule. */
#if !defined(_WIN32)
/* Single-quotes a string for /bin/sh: wraps in '...' and escapes any inner
 * ' as '\'' so shell metacharacters in the SQL ( '(' ')' ';' ) are data,
 * not syntax. The SQL in this file intentionally exercises all of them.
 * Pre-existing bug (documented in the project audit trail): the SQL used to
 * be placed raw on the command line, and dash died on the "(" — that POSIX
 * branch had never run. */
static void sh_quote(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (n + 1 < cap) dst[n++] = '\'';
    for (; src && *src && n + 4 < cap; ++src) {
        if (*src == '\'') {
            dst[n++] = '\'';
            dst[n++] = '\\';
            dst[n++] = '\'';
            dst[n++] = '\'';
        } else {
            dst[n++] = *src;
        }
    }
    if (n + 1 < cap) dst[n++] = '\'';
    dst[n < cap ? n : cap - 1] = '\0';
}
#endif

static int run_cli(const char *exe, const char *flag, const char *target,
                   const char *sql, char *outbuf, size_t outcap) {
    char outfile[1024];
    char script[1024];
    char line[4096];
    make_scratch(outfile, sizeof(outfile), ".log");
    make_scratch(script, sizeof(script), ".cmd");
#if defined(_WIN32)
    int n = snprintf(line, sizeof(line), "\"%s\" %s \"%s\" %s > \"%s\" 2>&1",
                     exe, (flag && flag[0]) ? flag : "", target,
                     (sql && sql[0]) ? sql : "", outfile);
#else
    char qsql[4096];
    if (sql && sql[0]) {
        sh_quote(qsql, sizeof(qsql), sql);
    } else {
        qsql[0] = '\0';
    }
    int n = snprintf(line, sizeof(line), "\"%s\" %s \"%s\" %s > \"%s\" 2>&1",
                     exe, (flag && flag[0]) ? flag : "", target, qsql, outfile);
#endif
    if (n < 0 || n >= (int)sizeof(line)) return -2;
#if defined(_WIN32)
    FILE *f = fopen(script, "w");
    if (!f) return -2;
    fprintf(f, "@echo off\r\n%s\r\nexit /b %%errorlevel%%\r\n", line);
    fclose(f);
#else
    FILE *f = fopen(script, "w");
    if (!f) return -2;
    fprintf(f, "#!/bin/sh\nexec %s\n", line);
    fclose(f);
    chmod(script, 0755);
#endif
    int rc = system(script);
    if (outbuf && outcap) {
        outbuf[0] = '\0';
        FILE *r = fopen(outfile, "rb");
        if (r) {
            size_t got = fread(outbuf, 1, outcap - 1, r);
            outbuf[got] = '\0';
            fclose(r);
        }
        remove(outfile);
    }
    remove(script);
    return system_exit_code(rc);
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
    for (int i = 0; i < CRED_COUNT; ++i) {
#if defined(_WIN32)
        _putenv_s(cred_env[i], "");
#else
        setenv(cred_env[i], "", 1);
#endif
    }
}

static void restore_creds(const char (*saved)[256], const int *set) {
    for (int i = 0; i < CRED_COUNT; ++i) {
#if defined(_WIN32)
        _putenv_s(cred_env[i], set[i] ? saved[i] : "");
#else
        if (set[i]) setenv(cred_env[i], saved[i], 1);
        else unsetenv(cred_env[i]);
#endif
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("SKIPPED: usage: otsardb_health_contract_check <path-to-otsardb-cli>\n");
        return 77;
    }
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint || !endpoint[0] || !bucket || !bucket[0]) {
        printf("SKIPPED: OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET not set (no live store)\n");
        return 77;
    }
    const char *exe = argv[1];
    char target[512];
    snprintf(target, sizeof(target), "s3://%s/health-0061/db", bucket);
    char out[OUTBUF_CAP];
    int rc;

    /* Phase 1: create the database through the CLI (normal write open).
     * Idempotent on purpose: the target may persist between runs. */
    const char *create_sql =
        "DROP TABLE IF EXISTS health_t; "
        "CREATE TABLE health_t(id INTEGER PRIMARY KEY, v TEXT); "
        "INSERT INTO health_t(v) VALUES('0061');";
    rc = run_cli(exe, "", target, create_sql, out, sizeof(out));
    printf("health-0061 phase1 create: exit %d\n", rc);
    if (rc != 0) {
        printf("FAIL [phase1 create]: otsardb CLI could not create the database.\n%s\n", out);
        return 1;
    }

    /* Phase 2: --health -> exit 0, JSON status ok with all contract fields. */
    rc = run_cli(exe, "--health", target, "", out, sizeof(out));
    printf("health-0061 phase2 --health: exit %d\n", rc);
    if (rc != 0 || !has(out, "\"status\":\"ok\"") || !has(out, "\"s3_reachable\":true") ||
        !has(out, "\"open_ms\":") || !has(out, "\"engine\":") || !has(out, "\"target\":")) {
        printf("FAIL [phase2 --health]: expected exit 0 + JSON status ok with "
               "s3_reachable/open_ms/engine/target.\n%s\n", out);
        return 1;
    }

    /* Phase 3: --health-json alias behaves identically. */
    rc = run_cli(exe, "--health-json", target, "", out, sizeof(out));
    printf("health-0061 phase3 --health-json: exit %d\n", rc);
    if (rc != 0 || !has(out, "\"status\":\"ok\"")) {
        printf("FAIL [phase3 --health-json]: alias must behave like --health.\n%s\n", out);
        return 1;
    }

    /* Phase 4: without credentials -> nonzero exit + JSON error. */
    char saved[CRED_COUNT][256];
    int saved_set[CRED_COUNT];
    snapshot_creds(saved, saved_set);
    clear_creds();
    rc = run_cli(exe, "--health", target, "", out, sizeof(out));
    restore_creds(saved, saved_set);
    printf("health-0061 phase4 no-credentials: exit %d\n", rc);
    if (rc == 0 || !has(out, "\"status\":\"error\"")) {
        printf("FAIL [phase4 no-credentials]: expected nonzero exit + JSON status error.\n%s\n", out);
        return 1;
    }

    printf("PASS: otsardb --health readiness contract (create, ok, alias, no-credential error) "
           "on %s (endpoint %s)\n", target, endpoint);
    return 0;
}

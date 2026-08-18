/* json_mode_check.c — --json output mode CTest.
 *
 * Drives the otsardb CLI with --json and asserts exact NDJSON output:
 * 1. :memory: multi-statement run — one object per statement, exact
 * object shape, affinity-based typing (REAL -> 1.5 number, INTEGER
 * -> number, NULL -> null, TEXT '123'-style stays a string),
 * escaping of quotes/backslash/control chars, exit 0;
 * 2. :memory: SQL error — exit 1 + {"ok":false,"error":"no such table"...
 * (exact contract: the JSON line IS the error surface);
 * 3. usage errors — exit 2 + {"ok":false,...} (missing SQL argument,
 * --json combined with --echo);
 * 4. live S3 target (SKIPPED 77 without OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET,
 * same convention as otsardb_health_contract): create + select through
 * --json, error case, and a zero-cache reopen
 * (OTSARDB_CACHE_REUSE=0 + fresh OTSARDB_CACHE_DIR) reading the committed
 * rows back.
 *
 * Children are spawned via system() through a temp .cmd/.sh script (the
 * same quoting-safe pattern as health_contract_check.c); output is
 * captured through a temp file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#define OUTBUF_CAP (16 * 1024)

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
    snprintf(buf, cap, "%s\\otsardb-json-0070-%lu%s", dir, (unsigned long)getpid(), ext);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-json-0070-%lu%s", dir, (unsigned long)getpid(), ext);
#endif
}

/* POSIX single-quote wrapper for shell lines: ' -> '\'' (the standard
 * shell idiom). The SQL is spliced raw into the command line; without
 * quoting, dash rejects '(' and other metacharacters. */
static char *quote_sh_single(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 4 + 3);
    if (!out) return NULL;
    char *p = out;
    *p++ = '\'';
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

/* Runs: "<exe>" <flags> "<target>" <sql> > <outfile> 2>&1
 * flags is spliced raw (may be "" or "--json --echo"). Returns the child's
 * exit code and fills outbuf with the merged output. */
static int run_cli(const char *exe, const char *flags, const char *target,
                   const char *sql, char *outbuf, size_t outcap) {
    char outfile[1024];
    char script[1024];
    char line[8192];
    make_scratch(outfile, sizeof(outfile), ".log");
    make_scratch(script, sizeof(script), ".cmd");
    int n = snprintf(line, sizeof(line), "\"%s\" %s \"%s\" %s > \"%s\" 2>&1",
                     exe, (flags && flags[0]) ? flags : "", target,
                     (sql && sql[0]) ? sql : "", outfile);
    if (n < 0 || n >= (int)sizeof(line)) return -2;
#if defined(_WIN32)
    /* cmd.exe treats '|' as a pipe operator even inside a batch line;
     * escape every '|' as '^|' so SQL containing '||' reaches the CLI
     * intact (the caret is consumed by cmd, not by the CLI). */
    {
        char escaped[8192];
        size_t e = 0;
        for (size_t s = 0; line[s] && e + 3 < sizeof(escaped); ++s) {
            if (line[s] == '|') escaped[e++] = '^';
            escaped[e++] = line[s];
        }
        escaped[e] = '\0';
        snprintf(line, sizeof(line), "%s", escaped);
    }
#else
    /* POSIX: single-quote the raw SQL argument (the '\'' escape idiom)
     * so metacharacters like '(' and '|' reach the CLI intact. An empty
     * SQL splices NOTHING (missing-arg usage error must still occur). */
    {
        char *qs = (sql && sql[0]) ? quote_sh_single(sql) : NULL;
        char qline[8192];
        int qn = snprintf(qline, sizeof(qline), "\"%s\" %s \"%s\" %s > \"%s\" 2>&1",
                          exe, (flags && flags[0]) ? flags : "", target,
                          qs ? qs : "", outfile);
        free(qs);
        if (qn < 0 || qn >= (int)sizeof(qline)) return -2;
        snprintf(line, sizeof(line), "%s", qline);
    }
#endif
    FILE *f = fopen(script, "w");
    if (!f) return -2;
#if defined(_WIN32)
    fprintf(f, "@echo off\r\n%s\r\nexit /b %%errorlevel%%\r\n", line);
#else
    fprintf(f, "#!/bin/sh\nexec %s\n", line);
    chmod(script, 0755);
#endif
    fclose(f);
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

/* Splits out into lines (mutates out); fills lines[] (NULL-terminated).
 * Strips a trailing '\r' per line (MSVC text-mode stdout translation) and
 * drops "otsardb:" diagnostic lines (stderr, convention —
 * they interleave in the merged 2>&1 capture). Returns the number of
 * lines. */
static int split_lines(char *out, char **lines, int max_lines) {
    int count = 0;
    char *p = out;
    while (p && *p && count < max_lines) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl) *nl = '\0';
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        if (strncmp(line, "otsardb:", 6) != 0) lines[count++] = line;
        if (!nl) break;
        p = nl + 1;
    }
    return count;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("SKIPPED: usage: otsardb_json_mode_check <path-to-otsardb-cli>\n");
        return 77;
    }
    const char *exe = argv[1];
    char out[OUTBUF_CAP];
    char *lines[64];
    int rc;

    /* ---- Phase 1: :memory: multi-statement, exact NDJSON contract ----
     * The SQL avoids literal double quotes in the command line (cmd quote
     * stripping); the quote/backslash value is built with char(34)/char(92).
     * Expected lines (6 statements): five empty-result objects then the
     * SELECT with affinity typing (1.5 REAL -> number, 0/2 INTEGER ->
     * numbers, NULL -> null) and escaping ("q\"r\\s", "nl\nx"). */
    const char *sql1 =
        "CREATE TABLE jt(id INTEGER PRIMARY KEY, v TEXT, n REAL); "
        "INSERT INTO jt(v,n) VALUES('a',1.5); "
        "INSERT INTO jt(v,n) VALUES('b',NULL); "
        "INSERT INTO jt(v,n) VALUES('q' || char(34) || 'r' || char(92) || 's',0); "
        "INSERT INTO jt(v,n) VALUES('nl' || char(10) || 'x',2); "
        "SELECT id,v,n FROM jt ORDER BY id;";
    rc = run_cli(exe, "--json", ":memory:", sql1, out, sizeof(out));
    printf("json-0070 phase1 :memory: multi-statement: exit %d\n", rc);
    if (rc != 0) {
        printf("FAIL [phase1]: expected exit 0.\n%s\n", out);
        return 1;
    }
    int nlines = split_lines(out, lines, 64);
    if (nlines != 6) {
        printf("FAIL [phase1]: expected 6 NDJSON lines, got %d.\n%s\n", nlines, out);
        return 1;
    }
    const char *empty_object = "{\"ok\":true,\"rows\":0,\"columns\":[],\"data\":[]}";
    for (int i = 0; i < 5; ++i) {
        if (strcmp(lines[i], empty_object) != 0) {
            printf("FAIL [phase1]: statement %d must be %s, got:\n%s\n",
                   i + 1, empty_object, lines[i]);
            return 1;
        }
    }
    const char *expected =
        "{\"ok\":true,\"rows\":4,\"columns\":[\"id\",\"v\",\"n\"],"
        "\"data\":[[1,\"a\",1.5],[2,\"b\",null],[3,\"q\\\"r\\\\s\",0.0],"
        "[4,\"nl\\u000ax\",2.0]]}";
    if (strcmp(lines[5], expected) != 0) {
        printf("FAIL [phase1]: SELECT line mismatch.\nExpected: %s\nGot:      %s\n",
               expected, lines[5]);
        return 1;
    }

    /* ---- Phase 2: SQL error -> exit 1 + {"ok":false,...} ---- */
    rc = run_cli(exe, "--json", ":memory:",
                 "SELECT * FROM no_such_table_xyz;", out, sizeof(out));
    printf("json-0070 phase2 error case: exit %d\n", rc);
    nlines = split_lines(out, lines, 64);
    if (rc != 1 || nlines != 1 || !has(lines[0], "{\"ok\":false,\"error\":") ||
        !has(lines[0], "no such table")) {
        printf("FAIL [phase2]: expected exit 1 + single error object.\n%s\n", out);
        return 1;
    }

    /* ---- Phase 3: usage errors -> exit 2 + {"ok":false,...} ---- */
    rc = run_cli(exe, "--json", ":memory:", "", out, sizeof(out));
    printf("json-0070 phase3a missing SQL: exit %d\n", rc);
    if (rc != 2 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "requires a SQL argument")) {
        printf("FAIL [phase3a]: expected exit 2 + JSON usage error.\n%s\n", out);
        return 1;
    }
    rc = run_cli(exe, "--json --echo", ":memory:", "SELECT 1;", out, sizeof(out));
    printf("json-0070 phase3b --json+--echo: exit %d\n", rc);
    if (rc != 2 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "--echo")) {
        printf("FAIL [phase3b]: expected exit 2 + JSON conflict error.\n%s\n", out);
        return 1;
    }

    /* ---- Phase 4: live S3 target (SKIPPED without a store) ---- */
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint || !endpoint[0] || !bucket || !bucket[0]) {
        printf("SKIPPED: OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET not set (no live store)\n");
        return 77;
    }
    char target[512];
    snprintf(target, sizeof(target), "s3://%s/studio-0070/db", bucket);

    const char *create_sql =
        "DROP TABLE IF EXISTS jt_s3; "
        "CREATE TABLE jt_s3(id INTEGER PRIMARY KEY, v TEXT); "
        "INSERT INTO jt_s3(v) VALUES('s3json');";
    rc = run_cli(exe, "--json", target, create_sql, out, sizeof(out));
    printf("json-0070 phase4a s3 create: exit %d\n", rc);
    if (rc != 0) {
        printf("FAIL [phase4a]: create through --json failed.\n%s\n", out);
        return 1;
    }
    nlines = split_lines(out, lines, 64);
    if (nlines != 3 ||
        strcmp(lines[nlines - 1], "{\"ok\":true,\"rows\":0,\"columns\":[],\"data\":[]}") != 0) {
        printf("FAIL [phase4a]: expected 3 lines ending with the empty result "
               "object.\n%s\n", out);
        return 1;
    }

    rc = run_cli(exe, "--json", target, "SELECT count(*) AS n FROM jt_s3;",
                 out, sizeof(out));
    printf("json-0070 phase4b s3 select: exit %d\n", rc);
    const char *expected_count =
        "{\"ok\":true,\"rows\":1,\"columns\":[\"n\"],\"data\":[[1]]}";
    nlines = split_lines(out, lines, 64);
    if (rc != 0 || nlines != 1 || strcmp(lines[0], expected_count) != 0) {
        printf("FAIL [phase4b]: expected count object.\nExpected: %s\nGot: %s\n",
               expected_count, out);
        return 1;
    }

    rc = run_cli(exe, "--json", target, "SELECT * FROM missing_t_xyz;",
                 out, sizeof(out));
    printf("json-0070 phase4c s3 error: exit %d\n", rc);
    if (rc != 1 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "no such table")) {
        printf("FAIL [phase4c]: expected exit 1 + error object.\n%s\n", out);
        return 1;
    }

    /* Zero-cache reopen: fresh cache dir + cache reuse off, read the
     * committed row back from S3. */
    char cache_dir[1024];
    make_scratch(cache_dir, sizeof(cache_dir), "-cache");
#if defined(_WIN32)
    _putenv_s("OTSARDB_CACHE_DIR", cache_dir);
    _putenv_s("OTSARDB_CACHE_REUSE", "0");
#else
    setenv("OTSARDB_CACHE_DIR", cache_dir, 1);
    setenv("OTSARDB_CACHE_REUSE", "0", 1);
#endif
    rc = run_cli(exe, "--json", target, "SELECT count(*) AS n FROM jt_s3;",
                 out, sizeof(out));
#if defined(_WIN32)
    _putenv_s("OTSARDB_CACHE_REUSE", "");
#else
    unsetenv("OTSARDB_CACHE_REUSE");
#endif
    printf("json-0070 phase4d s3 zero-cache reopen: exit %d\n", rc);
    nlines = split_lines(out, lines, 64);
    if (rc != 0 || nlines != 1 || strcmp(lines[0], expected_count) != 0) {
        printf("FAIL [phase4d]: zero-cache reopen must return count 1.\n%s\n", out);
        return 1;
    }

    printf("PASS: otsardb --json contract on :memory: and %s (endpoint %s)\n",
           target, endpoint);
    return 0;
}

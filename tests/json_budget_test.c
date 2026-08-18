/* json_budget_test.c — --json budget fail-closed CTest.
 *
 * Regression for bug-hunt finding CLI-1 (src/cli/json_mode.c buf_reserve
 * never grew the buffer and returned success, so appends memcpy'd past the
 * heap allocation instead of failing closed) and CLI-2 (output truncation
 * without an error marker).
 *
 * Drives the otsardb CLI with --json against :memory: targets whose result
 * sets exceed the documented budgets and asserts the fail-closed contract:
 * an explicit {"ok":false,"error":...} object, no partial data line, the
 * correct exit code, and no crash:
 *
 * P1 payload cap: ~24 MB result -> exit 1, exact "payload cap (8 MiB)"
 * P2 row cap: 102400 rows -> exit 1, exact "row cap (100000)"
 * P3 multi-statement: ok line preserved + payload-cap error line
 * P4 run cap: 5 x ~6.9 MB stmts -> exit 1, error object, output
 * bounded below OTSARDB_JSON_OUT_CAP (32 MiB)
 * P5 near-cap success: ~5.3 MB result -> exit 0, well-formed object
 *
 * The SQL never uses '|' or quotes (pure hex() output); '<' appears in the
 * recursive CTE bounds and is caret-escaped for cmd.exe. Harness pattern:
 * same as tests/json_mode_check.c (temp .cmd/.sh script + system() +
 * temp output file). Child spawning via system() means this target links
 * nothing from the OtsarDB tree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#endif

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CTE_PREFIX \
    "WITH RECURSIVE a(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM a WHERE x<512), " \
    "b(y) AS (SELECT 1 UNION ALL SELECT y+1 FROM b WHERE y<"
#define CTE_SUFFIX \
    ") SELECT hex(randomblob(128)) FROM a, b;"
#define CTE_SUFFIX_SMALL \
    ") SELECT a.x FROM a, b;"

#define ERR_PAYLOAD \
    "{\"ok\":false,\"error\":\"result set exceeds the --json payload cap " \
    "(8 MiB); narrow the query\"}"
#define ERR_ROWS \
    "{\"ok\":false,\"error\":\"result set exceeds the --json row cap " \
    "(100000); narrow the query\"}"

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
    snprintf(buf, cap, "%s\\otsardb-json-budget-0132-%lu%s", dir,
             (unsigned long)_getpid(), ext);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-json-budget-0132-%lu%s", dir,
             (unsigned long)getpid(), ext);
#endif
}

/* POSIX single-quote wrapper for shell lines: ' -> '\'' (standard idiom,
 * same as json_mode_check.c). */
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

/* cmd.exe metacharacters (& | < > ^) get a caret escape so the SQL reaches
 * the CLI literally (json_mode_check.c escaped only '|'; the recursive CTE
 * bounds also use '<'). */
static void cmd_escape(const char *in, char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 3 < outcap; ++i) {
        if (strchr("&|<>^", in[i])) out[o++] = '^';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

/* Runs: "<exe>" --json "<target>" <sql> > <outfile> 2>&1
 * through a temp .cmd/.sh script. Captures the first outcap-1 bytes of the
 * output file into outbuf (or the LAST outcap-1 bytes when tail_mode);
 * *outsize receives the full output file size. Returns the child's exit
 * code (negative = harness failure). */
static int run_cli(const char *exe, const char *target, const char *sql,
                   char *outbuf, size_t outcap, int tail_mode,
                   long long *outsize) {
    char outfile[1024];
    char script[1024];
    char line[16384];
    make_scratch(outfile, sizeof(outfile), ".log");
    make_scratch(script, sizeof(script), ".cmd");
#if defined(_WIN32)
    char sql_esc[4096];
    cmd_escape(sql, sql_esc, sizeof(sql_esc));
    int n = snprintf(line, sizeof(line),
                     "\"%s\" --json \"%s\" %s > \"%s\" 2>&1",
                     exe, target, sql_esc, outfile);
#else
    char *qs = quote_sh_single(sql);
    int n = snprintf(line, sizeof(line),
                     "\"%s\" --json \"%s\" %s > \"%s\" 2>&1",
                     exe, target, qs ? qs : "", outfile);
    free(qs);
#endif
    if (n < 0 || n >= (int)sizeof(line)) return -2;
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
    long long size = 0;
    if (outbuf && outcap) {
        outbuf[0] = '\0';
        FILE *r = fopen(outfile, "rb");
        if (r) {
            fseek(r, 0, SEEK_END);
            size = (long long)ftell(r);
            fseek(r, 0, SEEK_SET);
            long long skip = tail_mode ? (size - (long long)(outcap - 1)) : 0;
            if (skip < 0) skip = 0;
            if (tail_mode && skip > 0) fseek(r, (long)skip, SEEK_SET);
            size_t want = tail_mode ? (size_t)(size - skip) : (size_t)size;
            size_t got = want < outcap - 1 ? want : outcap - 1;
            got = fread(outbuf, 1, got, r);
            outbuf[got] = '\0';
            fclose(r);
        }
    }
    if (outsize) *outsize = size;
    remove(outfile);
    remove(script);
    return system_exit_code(rc);
}

static int has(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/* Splits out into lines (mutates out); strips a trailing '\r' per line and
 * drops "otsardb:" diagnostic lines (stderr, convention).
 * Returns the number of lines. */
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

static int check_line(const char *phase, char *out, int rc, int line_idx,
                      const char *expected) {
    char *lines[64];
    int nlines = split_lines(out, lines, 64);
    if (rc != 1 || nlines != line_idx + 1 ||
        strcmp(lines[line_idx], expected) != 0) {
        printf("FAIL [%s]: expected exit 1 + exactly one error line "
               "(index %d).\nexit=%d lines=%d\n%s\n",
               phase, line_idx, rc, nlines, out);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("SKIPPED: usage: otsardb_json_budget_test <path-to-otsardb-cli>\n");
        return 77;
    }
    const char *exe = argv[1];
    char small[4096];
    long long size = 0;
    int rc;

    char payload_sql[1024];
    snprintf(payload_sql, sizeof(payload_sql), "%s180%s", CTE_PREFIX, CTE_SUFFIX);
    char rows_sql[1024];
    snprintf(rows_sql, sizeof(rows_sql), "%s200%s", CTE_PREFIX, CTE_SUFFIX_SMALL);
    char ok_sql[1024];
    snprintf(ok_sql, sizeof(ok_sql), "SELECT 1; %s180%s", CTE_PREFIX, CTE_SUFFIX);
    char multi_sql[2048];
    multi_sql[0] = '\0';
    for (int i = 0; i < 5; ++i) {
        if (i > 0) strcat(multi_sql, " ");
        strcat(multi_sql, CTE_PREFIX);
        strcat(multi_sql, "52");
        strcat(multi_sql, CTE_SUFFIX);
    }

    /* ---- P1: per-statement payload cap (8 MiB) ---- */
    rc = run_cli(exe, ":memory:", payload_sql, small, sizeof(small), 0, &size);
    printf("json-budget P1 payload cap: exit %d, out %lld bytes\n", rc, size);
    if (check_line("P1", small, rc, 0, ERR_PAYLOAD)) return 1;

    /* ---- P2: per-statement row cap (100000) ---- */
    rc = run_cli(exe, ":memory:", rows_sql, small, sizeof(small), 0, &size);
    printf("json-budget P2 row cap: exit %d, out %lld bytes\n", rc, size);
    if (check_line("P2", small, rc, 0, ERR_ROWS)) return 1;

    /* ---- P3: multi-statement keeps the ok line, error line last ---- */
    rc = run_cli(exe, ":memory:", ok_sql, small, sizeof(small), 0, &size);
    printf("json-budget P3 multi-statement: exit %d, out %lld bytes\n", rc, size);
    {
        char *lines[64];
        int nlines = split_lines(small, lines, 64);
        if (rc != 1 || nlines != 2 ||
            strcmp(lines[0], "{\"ok\":true,\"rows\":1,\"columns\":[\"1\"],"
                             "\"data\":[[1]]}") != 0 ||
            strcmp(lines[1], ERR_PAYLOAD) != 0) {
            printf("FAIL [P3]: expected ok line + payload-cap error line.\n"
                   "exit=%d lines=%d\n%s\n", rc, nlines, small);
            return 1;
        }
    }

    /* ---- P4: per-invocation run cap (32 MiB, 5 x ~6.9 MB statements) ---- */
    rc = run_cli(exe, ":memory:", multi_sql, small, sizeof(small), 1, &size);
    printf("json-budget P4 run cap: exit %d, out %lld bytes\n", rc, size);
    {
        char *lines[64];
        int nlines = split_lines(small, lines, 64);
        if (rc != 1 || nlines < 1 ||
            strcmp(lines[nlines - 1], ERR_PAYLOAD) != 0) {
            printf("FAIL [P4]: expected the run to fail closed with the "
                   "payload-cap error line last.\nexit=%d lines=%d\n%s\n",
                   rc, nlines, small);
            return 1;
        }
        if (size <= (24LL << 20) || size >= (32LL << 20)) {
            printf("FAIL [P4]: output size %lld bytes must prove 4 full "
                   "statements (>= 24 MiB) while staying below the 32 MiB "
                   "cap.\n", size);
            return 1;
        }
    }

    /* ---- P5: near-cap success must still work (no over-fail) ---- */
    {
        char big_sql[1024];
        snprintf(big_sql, sizeof(big_sql), "%s40%s", CTE_PREFIX, CTE_SUFFIX);
        size_t bigcap = 6u << 20;
        char *big = (char *)malloc(bigcap);
        if (!big) {
            printf("FAIL [P5]: out of memory in harness\n");
            return 1;
        }
        rc = run_cli(exe, ":memory:", big_sql, big, bigcap, 0, &size);
        printf("json-budget P5 near-cap success: exit %d, out %lld bytes\n",
               rc, size);
        if (rc != 0 || size <= (4LL << 20) || size >= (8LL << 20)) {
            printf("FAIL [P5]: ~5.3 MB result must succeed (exit 0).\n"
                   "exit=%d size=%lld\n", rc, size);
            free(big);
            return 1;
        }
        char *lines[8];
        int nlines = split_lines(big, lines, 8);
        const char *prefix =
            "{\"ok\":true,\"rows\":20480,"
            "\"columns\":[\"hex(randomblob(128))\"],\"data\":[[";
        if (nlines != 1 || strncmp(lines[0], prefix, strlen(prefix)) != 0 ||
            !has(lines[0], "]]}") || has(lines[0], "\"ok\":false")) {
            printf("FAIL [P5]: near-cap success payload malformed.\n"
                   "lines=%d\n%s\n", nlines,
                   nlines > 0 && lines[0] ? lines[0] : "(empty)");
            free(big);
            return 1;
        }
        free(big);
    }

    printf("PASS: --json budget caps fail closed (8 MiB payload, 100k rows, "
           "32 MiB run) with exit 1 + explicit error objects\n");
    return 0;
}

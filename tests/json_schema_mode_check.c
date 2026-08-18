/* json_schema_mode_check.c — --json-schema mode CTest.
 *
 * Drives the otsardb CLI with --json-schema and --json and asserts:
 * 1. :memory: schema -> one exact {"ok":true,"tables":[]} object (exit 0);
 * 2. file target: DDL via --json, then --json-schema -> exact schema tree
 * object (tables/views -> columns with type/pk/notnull -> indexes with
 * unique + columns), exit 0;
 * 3. the PRAGMA NDJSON contract via --json (PRAGMA table_info /
 * index_list rows must flow through the same typed-cell NDJSON shape);
 * 4. usage errors -> exit 2 + {"ok":false,...} (missing target,
 * --json+--json-schema, --echo, SQL argument);
 * 5. live S3 target (SKIPPED 77 without OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET,
 * same convention as otsardb_health_contract): create+index via --json,
 * --json-schema reads the tree back (substring asserts), PRAGMA via
 * --json against the store, and a bogus-endpoint open failure ->
 * exit 1 + {"ok":false,...}.
 *
 * Children are spawned via system() through a temp .cmd/.sh script (the
 * same quoting-safe pattern as json_mode_check.c); output is captured
 * through a temp file.
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
    snprintf(buf, cap, "%s\\otsardb-json-schema-0073-%lu%s", dir,
             (unsigned long)getpid(), ext);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(buf, cap, "%s/otsardb-json-schema-0073-%lu%s", dir,
             (unsigned long)getpid(), ext);
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

/* Runs: "<exe>" <flags> [target] [sql] > <outfile> 2>&1
 * The target is only spliced when non-empty (an absent target is a usage
 * error case for --json-schema). Returns the child's exit code and fills
 * outbuf with the merged output. */
static int run_cli(const char *exe, const char *flags, const char *target,
                   const char *sql, char *outbuf, size_t outcap) {
    char outfile[1024];
    char script[1024];
    char line[8192];
    make_scratch(outfile, sizeof(outfile), ".log");
    make_scratch(script, sizeof(script), ".cmd");
    int n;
    if (target && target[0]) {
        n = snprintf(line, sizeof(line), "\"%s\" %s \"%s\" %s > \"%s\" 2>&1",
                     exe, (flags && flags[0]) ? flags : "", target,
                     (sql && sql[0]) ? sql : "", outfile);
    } else {
        n = snprintf(line, sizeof(line), "\"%s\" %s %s > \"%s\" 2>&1",
                     exe, (flags && flags[0]) ? flags : "",
                     (sql && sql[0]) ? sql : "", outfile);
    }
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
        int qn;
        if (target && target[0]) {
            qn = snprintf(qline, sizeof(qline), "\"%s\" %s \"%s\" %s > \"%s\" 2>&1",
                          exe, (flags && flags[0]) ? flags : "", target,
                          qs ? qs : "", outfile);
        } else {
            qn = snprintf(qline, sizeof(qline), "\"%s\" %s %s > \"%s\" 2>&1",
                          exe, (flags && flags[0]) ? flags : "",
                          qs ? qs : "", outfile);
        }
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
 * Strips a trailing '\r' per line and drops "otsardb:" diagnostic lines
 * (stderr, convention). Returns the number of lines. */
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

static int phase = 0;

static int fail(const char *name, const char *detail) {
    printf("FAIL [%s]: %s\n%s\n", name, detail, detail[0] ? "" : "");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("SKIPPED: usage: otsardb_json_schema_mode_check <path-to-otsardb-cli>\n");
        return 77;
    }
    const char *exe = argv[1];
    char out[OUTBUF_CAP];
    char *lines[64];
    int rc;

    /* ---- Phase 1: :memory: empty schema ---- */
    phase = 1;
    rc = run_cli(exe, "--json-schema", ":memory:", "", out, sizeof(out));
    printf("json-schema-0073 phase1 :memory: exit %d\n", rc);
    if (rc != 0 || !has(out, "{\"ok\":true,\"tables\":[]}")) {
        printf("FAIL [phase1]: expected exit 0 + empty tables object.\n%s\n", out);
        return 1;
    }

    /* ---- Phase 2: file target, exact schema tree ----
     * DDL first via --json (the file persists between invocations), then
     * --json-schema reads the tree back. Expected exact object:
     * jt (table): id INTEGER pk=1, v TEXT notnull=1; index idx_jt_v on v;
     * jv (view): column id, no indexes. */
    phase = 2;
    char db[1024];
    make_scratch(db, sizeof(db), ".db");
    remove(db);
    const char *ddl =
        "CREATE TABLE jt(id INTEGER PRIMARY KEY, v TEXT NOT NULL); "
        "CREATE INDEX idx_jt_v ON jt(v); "
        "CREATE VIEW jv AS SELECT id FROM jt;";
    rc = run_cli(exe, "--json", db, ddl, out, sizeof(out));
    if (rc != 0) {
        printf("FAIL [phase2a]: DDL via --json failed (exit %d).\n%s\n", rc, out);
        return 1;
    }
    rc = run_cli(exe, "--json-schema", db, "", out, sizeof(out));
    printf("json-schema-0073 phase2 file schema: exit %d\n", rc);
    const char *expected =
        "{\"ok\":true,\"tables\":["
        "{\"name\":\"jt\",\"type\":\"table\","
        "\"columns\":[{\"name\":\"id\",\"type\":\"INTEGER\",\"pk\":1,\"notnull\":0},"
        "{\"name\":\"v\",\"type\":\"TEXT\",\"pk\":0,\"notnull\":1}],"
        "\"indexes\":[{\"name\":\"idx_jt_v\",\"unique\":0,\"columns\":[\"v\"]}]},"
        "{\"name\":\"jv\",\"type\":\"view\","
        "\"columns\":[{\"name\":\"id\",\"type\":\"INTEGER\",\"pk\":0,\"notnull\":0}],"
        "\"indexes\":[]}]}";
    int nlines = split_lines(out, lines, 64);
    if (rc != 0 || nlines != 1 || strcmp(lines[0], expected) != 0) {
        printf("FAIL [phase2b]: schema tree mismatch.\nExpected: %s\nGot:      %s\n",
               expected, out);
        return 1;
    }

    /* ---- Phase 3: PRAGMA NDJSON contract via --json ----
     * PRAGMA table_info/index_list/index_info rows flow through the same
     * typed-cell NDJSON shape (integers as numbers, NULL dflt_value as
     * null) — the exact shapes the schema explorer executes. */
    phase = 3;
    const char *pragma_sql =
        "CREATE TABLE pt(id INTEGER PRIMARY KEY, v TEXT NOT NULL, w REAL); "
        "CREATE INDEX idx_pt_v ON pt(v); "
        "PRAGMA table_info(pt); PRAGMA index_list(pt); PRAGMA index_info(idx_pt_v);";
    rc = run_cli(exe, "--json", ":memory:", pragma_sql, out, sizeof(out));
    printf("json-schema-0073 phase3 pragma contract: exit %d\n", rc);
    if (rc != 0) {
        printf("FAIL [phase3a]: pragma run failed.\n%s\n", out);
        return 1;
    }
    nlines = split_lines(out, lines, 64);
    if (nlines != 5) {
        printf("FAIL [phase3b]: expected 5 NDJSON lines.\n%s\n", out);
        return 1;
    }
    const char *empty_object = "{\"ok\":true,\"rows\":0,\"columns\":[],\"data\":[]}";
    if (strcmp(lines[0], empty_object) != 0 || strcmp(lines[1], empty_object) != 0) {
        printf("FAIL [phase3c]: expected two empty DDL objects.\n%s\n", out);
        return 1;
    }
    const char *expected_info =
        "{\"ok\":true,\"rows\":3,"
        "\"columns\":[\"cid\",\"name\",\"type\",\"notnull\",\"dflt_value\",\"pk\"],"
        "\"data\":[[0,\"id\",\"INTEGER\",0,null,1],[1,\"v\",\"TEXT\",1,null,0],"
        "[2,\"w\",\"REAL\",0,null,0]]}";
    if (strcmp(lines[2], expected_info) != 0) {
        printf("FAIL [phase3d]: table_info line mismatch.\nExpected: %s\nGot:      %s\n",
               expected_info, lines[2]);
        return 1;
    }
    const char *expected_list =
        "{\"ok\":true,\"rows\":1,"
        "\"columns\":[\"seq\",\"name\",\"unique\",\"origin\",\"partial\"],"
        "\"data\":[[0,\"idx_pt_v\",0,\"c\",0]]}";
    if (strcmp(lines[3], expected_list) != 0) {
        printf("FAIL [phase3e]: index_list line mismatch.\nExpected: %s\nGot:      %s\n",
               expected_list, lines[3]);
        return 1;
    }
    const char *expected_iinfo =
        "{\"ok\":true,\"rows\":1,"
        "\"columns\":[\"seqno\",\"cid\",\"name\"],"
        "\"data\":[[0,1,\"v\"]]}";
    if (strcmp(lines[4], expected_iinfo) != 0) {
        printf("FAIL [phase3f]: index_info line mismatch.\nExpected: %s\nGot:      %s\n",
               expected_iinfo, lines[4]);
        return 1;
    }

    /* ---- Phase 4: usage errors -> exit 2 + {"ok":false,...} ---- */
    phase = 4;
    rc = run_cli(exe, "--json-schema", "", "", out, sizeof(out));
    printf("json-schema-0073 phase4a missing target: exit %d\n", rc);
    if (rc != 2 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "missing target argument")) {
        printf("FAIL [phase4a]: expected exit 2 + JSON usage error.\n%s\n", out);
        return 1;
    }
    rc = run_cli(exe, "--json-schema --json", ":memory:", "SELECT 1;", out,
                 sizeof(out));
    printf("json-schema-0073 phase4b +--json: exit %d\n", rc);
    if (rc != 2 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "--json")) {
        printf("FAIL [phase4b]: expected exit 2 + JSON conflict error.\n%s\n", out);
        return 1;
    }
    rc = run_cli(exe, "--json-schema --echo", ":memory:", "", out, sizeof(out));
    printf("json-schema-0073 phase4c +--echo: exit %d\n", rc);
    if (rc != 2 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "--echo")) {
        printf("FAIL [phase4c]: expected exit 2 + JSON conflict error.\n%s\n", out);
        return 1;
    }
    rc = run_cli(exe, "--json-schema", ":memory:", "SELECT 1;", out, sizeof(out));
    printf("json-schema-0073 phase4d SQL argument: exit %d\n", rc);
    if (rc != 2 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "does not accept a SQL argument")) {
        printf("FAIL [phase4d]: expected exit 2 + JSON usage error.\n%s\n", out);
        return 1;
    }

    /* ---- Phase 5: live S3 target (SKIPPED without a store) ---- */
    phase = 5;
    const char *endpoint = getenv("OTSARDB_S3_ENDPOINT");
    const char *bucket = getenv("OTSARDB_S3_BUCKET");
    if (!endpoint || !endpoint[0] || !bucket || !bucket[0]) {
        printf("SKIPPED: OTSARDB_S3_ENDPOINT/OTSARDB_S3_BUCKET not set (no live store)\n");
        return 77;
    }
    char target[512];
    snprintf(target, sizeof(target), "s3://%s/studio-0073/db", bucket);

    const char *s3_ddl =
        "DROP TABLE IF EXISTS jt_s3; "
        "CREATE TABLE jt_s3(id INTEGER PRIMARY KEY, v TEXT); "
        "CREATE INDEX idx_s3_v ON jt_s3(v); "
        "INSERT INTO jt_s3(v) VALUES('s3schema');";
    rc = run_cli(exe, "--json", target, s3_ddl, out, sizeof(out));
    printf("json-schema-0073 phase5a s3 create: exit %d\n", rc);
    if (rc != 0) {
        printf("FAIL [phase5a]: create through --json failed.\n%s\n", out);
        return 1;
    }
    rc = run_cli(exe, "--json-schema", target, "", out, sizeof(out));
    printf("json-schema-0073 phase5b s3 schema: exit %d\n", rc);
    if (rc != 0 || !has(out, "\"name\":\"jt_s3\"") ||
        !has(out, "\"name\":\"idx_s3_v\"") ||
        !has(out, "\"pk\":1") || !has(out, "[\"v\"]")) {
        printf("FAIL [phase5b]: schema tree must contain jt_s3 + idx_s3_v.\n%s\n", out);
        return 1;
    }
    rc = run_cli(exe, "--json", target, "PRAGMA table_info(jt_s3);", out,
                 sizeof(out));
    printf("json-schema-0073 phase5c s3 pragma: exit %d\n", rc);
    const char *expected_s3_info =
        "{\"ok\":true,\"rows\":2,"
        "\"columns\":[\"cid\",\"name\",\"type\",\"notnull\",\"dflt_value\",\"pk\"],"
        "\"data\":[[0,\"id\",\"INTEGER\",0,null,1],[1,\"v\",\"TEXT\",0,null,0]]}";
    nlines = split_lines(out, lines, 64);
    if (rc != 0 || nlines != 1 || strcmp(lines[0], expected_s3_info) != 0) {
        printf("FAIL [phase5c]: s3 table_info mismatch.\nExpected: %s\nGot: %s\n",
               expected_s3_info, out);
        return 1;
    }

    /* Bogus endpoint: open must fail -> exit 1 + {"ok":false,...}. */
    const char *saved_endpoint = getenv("OTSARDB_S3_ENDPOINT");
#if defined(_WIN32)
    _putenv_s("OTSARDB_S3_ENDPOINT", "http://127.0.0.1:9");
#else
    setenv("OTSARDB_S3_ENDPOINT", "http://127.0.0.1:9", 1);
#endif
    rc = run_cli(exe, "--json-schema", target, "", out, sizeof(out));
#if defined(_WIN32)
    _putenv_s("OTSARDB_S3_ENDPOINT", saved_endpoint ? saved_endpoint : "");
#else
    if (saved_endpoint) setenv("OTSARDB_S3_ENDPOINT", saved_endpoint, 1);
    else unsetenv("OTSARDB_S3_ENDPOINT");
#endif
    printf("json-schema-0073 phase5d s3 open failure: exit %d\n", rc);
    if (rc != 1 || !has(out, "{\"ok\":false,\"error\":") ||
        !has(out, "failed to open")) {
        printf("FAIL [phase5d]: expected exit 1 + JSON open error.\n%s\n", out);
        return 1;
    }

    remove(db);
    printf("PASS: --json-schema contract on :memory:, file and %s "
           "(endpoint %s)\n", target, endpoint);
    return 0;
}

/* studio_paging_sql_test.c — Studio v0.4 server-side
 * paging SQL generation unit test.
 *
 * Drives otsardb_studio_paged_sql() (src/cli/studio.c) directly — a pure
 * function, no sockets, no store — and asserts the EXACT generated SQL
 * for the plain `SELECT * FROM <ident>` shape:
 * count_only=0: SELECT * FROM "<t>" LIMIT <page_size> OFFSET <n>;
 * count_only=1: SELECT COUNT(*) FROM "<t>";
 * plus the fail-closed rejections: any other shape (WHERE/ORDER BY/
 * LIMIT/JOIN/comments/backtick or bracket identifiers/multi-statement
 * strings/trailing junk) returns -1 so the caller keeps the original
 * statement and client-side paging .
 *
 * The wire-level behavior lives in studio_v04_smoke_test.c.
 */

#include "studio.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("paging-sql-0086 %-56s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

/* Exact-SQL assertion helper: generates and compares byte-for-byte. */
static void expect_sql(const char *name, const char *sql, long page,
                       long page_size, int count_only, const char *expected) {
    char out[2048];
    int rc = otsardb_studio_paged_sql(sql, page, page_size, count_only, out,
                                    sizeof(out));
    check(name, rc == 0 && strcmp(out, expected) == 0, out);
}

static void expect_reject(const char *name, const char *sql) {
    char out[2048];
    int rc = otsardb_studio_paged_sql(sql, 1, 50, 0, out, sizeof(out));
    check(name, rc == -1, sql);
}

int main(void) {
    /* ---- page rewrite: exact SQL ---- */
    expect_sql("page 1 of 50", "SELECT * FROM t;", 1, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("page 3 of 200", "SELECT * FROM t;", 3, 200,
               0, "SELECT * FROM \"t\" LIMIT 200 OFFSET 400;");
    expect_sql("page 1 of 1000", "SELECT * FROM t;", 1, 1000,
               0, "SELECT * FROM \"t\" LIMIT 1000 OFFSET 0;");
    expect_sql("quoted identifier", "SELECT * FROM \"a\"\"b\";", 1, 50,
               0, "SELECT * FROM \"a\"\"b\" LIMIT 50 OFFSET 0;");
    expect_sql("keyword table name stays quoted", "SELECT * FROM order;", 1, 50,
               0, "SELECT * FROM \"order\" LIMIT 50 OFFSET 0;");
    expect_sql("lowercase keywords", "select * from t;", 1, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("mixed-case keywords", "SeLeCt * FrOm t;", 1, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("leading whitespace", "  \tSELECT * FROM t;", 1, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("trailing whitespace", "SELECT * FROM t ; \n\t", 1, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("no semicolon", "SELECT * FROM t", 1, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("page clamp to 1", "SELECT * FROM t;", 0, 50,
               0, "SELECT * FROM \"t\" LIMIT 50 OFFSET 0;");
    expect_sql("page_size clamp up", "SELECT * FROM t;", 1, 20000,
               0, "SELECT * FROM \"t\" LIMIT 10000 OFFSET 0;");
    expect_sql("page_size clamp down", "SELECT * FROM t;", 1, -3,
               0, "SELECT * FROM \"t\" LIMIT 200 OFFSET 0;");
    {
        char out[2048];
        int rc = otsardb_studio_paged_sql("SELECT * FROM t;", 300000000, 10000,
                                        0, out, sizeof(out));
        check("offset bound rejected", rc == -1, out);
    }

    /* ---- count form ---- */
    expect_sql("count basic", "SELECT * FROM t;", 1, 50, 1,
               "SELECT COUNT(*) FROM \"t\";");
    expect_sql("count quoted identifier", "SELECT * FROM \"a\"\"b\";", 1, 50, 1,
               "SELECT COUNT(*) FROM \"a\"\"b\";");
    expect_sql("count lowercase", "select * from t", 1, 50, 1,
               "SELECT COUNT(*) FROM \"t\";");

    /* ---- rejections: the guard is deliberately narrow ---- */
    expect_reject("reject WHERE", "SELECT * FROM t WHERE id=1;");
    expect_reject("reject ORDER BY", "SELECT * FROM t ORDER BY id;");
    expect_reject("reject LIMIT", "SELECT * FROM t LIMIT 5;");
    expect_reject("reject OFFSET", "SELECT * FROM t OFFSET 5;");
    expect_reject("reject JOIN", "SELECT * FROM t JOIN s ON t.id=s.id;");
    expect_reject("reject column list", "SELECT id FROM t;");
    expect_reject("reject star expression", "SELECT t.* FROM t;");
    expect_reject("reject line comment", "SELECT * FROM t -- comment\n;");
    expect_reject("reject block comment", "SELECT * FROM t /* x */;");
    expect_reject("reject backtick identifier", "SELECT * FROM `t`;");
    expect_reject("reject bracket identifier", "SELECT * FROM [t];");
    expect_reject("reject multi-statement", "SELECT * FROM t; SELECT 1;");
    expect_reject("reject trailing junk", "SELECT * FROM t garbage");
    expect_reject("reject trailing comma junk", "SELECT * FROM t, s;");
    expect_reject("reject missing from", "SELECT * t;");
    expect_reject("reject missing star", "SELECT FROM t;");
    expect_reject("reject keyword suffix", "SELECTS * FROM t;");
    expect_reject("reject from suffix", "SELECT * FROMx t;");
    expect_reject("reject empty", "");
    expect_reject("reject null", NULL);
    expect_reject("reject unclosed quote", "SELECT * FROM \"t;");
    expect_reject("reject cte", "WITH x AS (SELECT 1) SELECT * FROM x;");

    if (failures) {
        printf("paging-sql-0086 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio v0.4 paging SQL generation (page/count "
           "rewrites, quoting, clamps, rejections)\n");
    return 0;
}

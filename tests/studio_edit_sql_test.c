/* studio_edit_sql_test.c — Studio v0.3 row-edit SQL
 * generator unit test.
 *
 * Drives otsardb_studio_edit_sql() (src/cli/studio.c) directly — a pure
 * function, no sockets, no store — and asserts the EXACT generated SQL for:
 * - UPDATE: SET all columns, WHERE primary key matched against the
 * ORIGINAL ("old") row values; string/number/NULL rendering;
 * - DELETE: WHERE primary key from the old row; NULL pk -> IS NULL;
 * - INSERT: column list + VALUES from the new row;
 * - quoting: identifiers double-quoted with "" doubling (table and
 * column names containing quotes), strings '...' with '' doubling,
 * raw validated numbers, NULL literals;
 * - fail-closed errors: missing table/op, unsupported op, no columns,
 * update/delete without pk, pk not among the row columns, old/new
 * length mismatches, invalid number text, column cap, output cap.
 *
 * The generator is the server-side half of the grid editor contract
 * (POST /api/edit); the wire smoke lives in studio_v03_smoke_test.c.
 */

#include "studio.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, int cond, const char *detail) {
    printf("edit-sql-0078 %-58s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) {
        failures++;
        if (detail && detail[0]) printf("  detail: %s\n", detail);
    }
}

/* Exact-SQL assertion helper: generates and compares byte-for-byte. */
static void expect_sql(const char *name, const otsardb_studio_edit *edit,
                       const char *expected) {
    char out[4096];
    int rc = otsardb_studio_edit_sql(edit, out, sizeof(out));
    check(name, rc == 0 && strcmp(out, expected) == 0, out);
}

static void expect_error(const char *name, const otsardb_studio_edit *edit,
                         const char *needle) {
    char out[4096];
    int rc = otsardb_studio_edit_sql(edit, out, sizeof(out));
    check(name, rc == -1 && strstr(out, needle) != NULL, out);
}

int main(void) {
    /* ---- fixture: people(id INTEGER PK, name TEXT, age REAL) ---- */
    const char *cols[] = {"id", "name", "age"};
    const char *pk[] = {"id"};
    otsardb_studio_cell old_row[3];
    otsardb_studio_cell new_row[3];
    otsardb_studio_edit edit;
    memset(&edit, 0, sizeof(edit));
    edit.table = "people";
    edit.cols = cols;
    edit.n_cols = 3;
    edit.pk = pk;
    edit.n_pk = 1;

    /* old: (1, "alice", 30.5) new: (1, "alice", 31) */
    old_row[0].is_null = 0; old_row[0].is_number = 1; old_row[0].text = "1";
    old_row[1].is_null = 0; old_row[1].is_number = 0; old_row[1].text = "alice";
    old_row[2].is_null = 0; old_row[2].is_number = 1; old_row[2].text = "30.5";
    new_row[0].is_null = 0; new_row[0].is_number = 1; new_row[0].text = "1";
    new_row[1].is_null = 0; new_row[1].is_number = 0; new_row[1].text = "alice";
    new_row[2].is_null = 0; new_row[2].is_number = 1; new_row[2].text = "31";
    edit.old_row = old_row;
    edit.n_old = 3;
    edit.new_row = new_row;
    edit.n_new = 3;
    edit.op = "update";
    expect_sql("update basic",
               &edit,
               "UPDATE \"people\" SET \"id\"=1,\"name\"='alice',"
               "\"age\"=31 WHERE \"id\"=1;");

    /* ---- UPDATE with string quoting (' -> '') ---- */
    new_row[1].text = "O'Brien";
    expect_sql("update string quoting",
               &edit,
               "UPDATE \"people\" SET \"id\"=1,\"name\"='O''Brien',"
               "\"age\"=31 WHERE \"id\"=1;");
    new_row[1].text = "alice";

    /* ---- UPDATE with NULL cells (SET NULL + pk IS NULL) ---- */
    new_row[1].is_null = 1;
    new_row[1].text = "";
    old_row[0].is_null = 1;
    old_row[0].text = "";
    expect_sql("update null cells",
               &edit,
               "UPDATE \"people\" SET \"id\"=1,\"name\"=NULL,"
               "\"age\"=31 WHERE \"id\" IS NULL;");
    new_row[1].is_null = 0;
    new_row[1].text = "alice";
    old_row[0].is_null = 0;
    old_row[0].text = "1";

    /* ---- UPDATE with a negative number and exponent ---- */
    new_row[2].text = "-1.5e2";
    expect_sql("update exponent number",
               &edit,
               "UPDATE \"people\" SET \"id\"=1,\"name\"='alice',"
               "\"age\"=-1.5e2 WHERE \"id\"=1;");
    new_row[2].text = "31";

    /* ---- DELETE ---- */
    edit.op = "delete";
    expect_sql("delete basic",
               &edit,
               "DELETE FROM \"people\" WHERE \"id\"=1;");
    edit.op = "update";

    /* ---- INSERT ---- */
    edit.op = "insert";
    new_row[0].text = "7";
    expect_sql("insert basic",
               &edit,
               "INSERT INTO \"people\" (\"id\",\"name\",\"age\") "
               "VALUES (7,'alice',31);");
    new_row[0].is_null = 1;
    new_row[0].text = "";
    expect_sql("insert null pk",
               &edit,
               "INSERT INTO \"people\" (\"id\",\"name\",\"age\") "
               "VALUES (NULL,'alice',31);");
    new_row[0].is_null = 0;
    new_row[0].text = "7";
    edit.op = "update";

    /* ---- identifier quoting: table and column with embedded quotes ---- */
    {
        const char *qcols[] = {"we\"ird", "v"};
        const char *qpk[] = {"we\"ird"};
        otsardb_studio_cell qold[2];
        otsardb_studio_cell qnew[2];
        qold[0].is_null = 0; qold[0].is_number = 1; qold[0].text = "1";
        qold[1].is_null = 0; qold[1].is_number = 0; qold[1].text = "x";
        qnew[0].is_null = 0; qnew[0].is_number = 1; qnew[0].text = "1";
        qnew[1].is_null = 0; qnew[1].is_number = 0; qnew[1].text = "y";
        otsardb_studio_edit qedit;
        memset(&qedit, 0, sizeof(qedit));
        qedit.table = "we\"ird table";
        qedit.op = "update";
        qedit.cols = qcols;
        qedit.n_cols = 2;
        qedit.pk = qpk;
        qedit.n_pk = 1;
        qedit.old_row = qold;
        qedit.n_old = 2;
        qedit.new_row = qnew;
        qedit.n_new = 2;
        expect_sql("update quoted identifiers",
                   &qedit,
                   "UPDATE \"we\"\"ird table\" SET \"we\"\"ird\"=1,"
                   "\"v\"='y' WHERE \"we\"\"ird\"=1;");
    }

    /* ---- composite primary key ---- */
    {
        const char *ccols[] = {"a", "b", "c"};
        const char *cpk[] = {"a", "b"};
        otsardb_studio_cell cold[3];
        otsardb_studio_cell cnew[3];
        cold[0].is_null = 0; cold[0].is_number = 1; cold[0].text = "1";
        cold[1].is_null = 0; cold[1].is_number = 1; cold[1].text = "2";
        cold[2].is_null = 0; cold[2].is_number = 0; cold[2].text = "z";
        cnew[0].is_null = 0; cnew[0].is_number = 1; cnew[0].text = "1";
        cnew[1].is_null = 0; cnew[1].is_number = 1; cnew[1].text = "2";
        cnew[2].is_null = 0; cnew[2].is_number = 0; cnew[2].text = "w";
        otsardb_studio_edit cedit;
        memset(&cedit, 0, sizeof(cedit));
        cedit.table = "t";
        cedit.op = "update";
        cedit.cols = ccols;
        cedit.n_cols = 3;
        cedit.pk = cpk;
        cedit.n_pk = 2;
        cedit.old_row = cold;
        cedit.n_old = 3;
        cedit.new_row = cnew;
        cedit.n_new = 3;
        expect_sql("update composite pk",
                   &cedit,
                   "UPDATE \"t\" SET \"a\"=1,\"b\"=2,\"c\"='w' "
                   "WHERE \"a\"=1 AND \"b\"=2;");
    }

    /* ---- fail-closed errors ---- */
    expect_error("error missing table", NULL, "");
    {
        otsardb_studio_edit e2 = edit;
        e2.table = "";
        expect_error("error empty table", &e2, "missing table name");
    }
    {
        otsardb_studio_edit e2 = edit;
        e2.op = "upsert";
        expect_error("error unsupported op", &e2, "unsupported edit operation");
    }
    {
        otsardb_studio_edit e2 = edit;
        e2.n_cols = 0;
        expect_error("error no columns", &e2, "no columns");
    }
    {
        otsardb_studio_edit e2 = edit;
        e2.n_pk = 0;
        expect_error("error update without pk", &e2, "primary-key");
    }
    {
        otsardb_studio_edit e2 = edit;
        const char *badpk[] = {"missing"};
        e2.pk = badpk;
        expect_error("error pk not among cols", &e2, "not among the row columns");
    }
    {
        otsardb_studio_edit e2 = edit;
        e2.n_old = 2;
        expect_error("error old length mismatch", &e2, "\"old\"");
    }
    {
        otsardb_studio_edit e2 = edit;
        e2.n_new = 2;
        expect_error("error new length mismatch", &e2, "\"new\"");
    }
    {
        otsardb_studio_edit e2 = edit;
        otsardb_studio_cell bad[3];
        memcpy(bad, old_row, sizeof(old_row));
        /* The invalid number must sit in a rendered position: the pk
         * value is used in the WHERE clause. */
        bad[0].is_number = 1;
        bad[0].text = "1.2.3";
        e2.old_row = bad;
        expect_error("error invalid number text", &e2, "invalid number text");
    }
    {
        otsardb_studio_edit e2 = edit;
        const char *many[OTSARDB_EDIT_MAX_COLS + 1];
        for (int i = 0; i <= OTSARDB_EDIT_MAX_COLS; ++i) many[i] = "c";
        e2.cols = many;
        e2.n_cols = OTSARDB_EDIT_MAX_COLS + 1;
        expect_error("error column cap", &e2, "too many columns");
    }
    {
        otsardb_studio_edit e2 = edit;
        char tiny[16];
        int rc = otsardb_studio_edit_sql(&e2, tiny, sizeof(tiny));
        check("error output cap", rc == -1, tiny);
    }

    if (failures) {
        printf("edit-sql-0078 FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: OtsarDB Studio v0.3 edit-row SQL generation (update/delete/"
           "insert, quoting, nulls, pk matching, caps, errors)\n");
    return 0;
}

#include "s3_target.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    otsardb_s3_target a;
    otsardb_s3_target b;
    otsardb_s3_target other;

    assert(otsardb_s3_target_parse("s3://company-data/orders/primary", &a));
    assert(strcmp(a.bucket, "company-data") == 0);
    assert(strcmp(a.database_root, "orders/primary") == 0);

    assert(otsardb_s3_target_parse("otsardb+s3://company-data//orders/primary///", &b));
    assert(strcmp(b.bucket, a.bucket) == 0);
    assert(strcmp(b.database_root, a.database_root) == 0);

    char id_a[OTSARDB_COMMIT_ID_MAX + 1];
    char id_b[OTSARDB_COMMIT_ID_MAX + 1];
    assert(otsardb_s3_target_database_id(&a, id_a));
    assert(otsardb_s3_target_database_id(&b, id_b));
    assert(strcmp(id_a, id_b) == 0);
    assert(strncmp(id_a, "db-", 3) == 0);
    assert(strlen(id_a) == 51);

    assert(otsardb_s3_target_parse("s3://company-data/orders/secondary", &other));
    char other_id[OTSARDB_COMMIT_ID_MAX + 1];
    assert(otsardb_s3_target_database_id(&other, other_id));
    assert(strcmp(id_a, other_id) != 0);

    assert(!otsardb_s3_target_parse("http://company-data/orders", &other));
    assert(!otsardb_s3_target_parse("s3://", &other));
    assert(!otsardb_s3_target_parse("s3://company-data", &other));
    assert(!otsardb_s3_target_parse("s3:///orders", &other));
    assert(!otsardb_s3_target_parse("s3://company-data/", &other));
    assert(!otsardb_s3_target_parse("s3://company-data/orders?version=1", &other));
    assert(!otsardb_s3_target_parse("s3://company-data/orders#fragment", &other));

    puts("S3 logical target parsing and identity passed");
    return 0;
}

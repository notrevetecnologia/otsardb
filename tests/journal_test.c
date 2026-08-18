#include "journal.h"
#include "memory_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static otsardb_head make_head(unsigned long long generation, const char *commit_id) {
    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = generation;
    snprintf(head.database_id, sizeof(head.database_id), "db-test");
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", commit_id);
    snprintf(head.commit_key, sizeof(head.commit_key), "commits/%s.json", commit_id);
    memset(head.commit_sha256, 'a', 64);
    head.commit_sha256[64] = '\0';
    return head;
}

int main(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    otsardb_head genesis = make_head(0, "genesis");
    char *etag0 = NULL;
    assert(otsardb_journal_publish_head(store, "tenant/orders", &genesis, NULL, 1, &etag0) == OTSARDB_JOURNAL_OK);
    assert(etag0 != NULL);

    otsardb_head duplicate = make_head(0, "other-genesis");
    assert(otsardb_journal_publish_head(store, "tenant/orders", &duplicate, NULL, 1, NULL) == OTSARDB_JOURNAL_CONFLICT);

    otsardb_head observed;
    char *reader_etag = NULL;
    assert(otsardb_journal_read_head(store, "tenant/orders", &observed, &reader_etag) == OTSARDB_JOURNAL_OK);
    assert(observed.generation == 0);
    assert(strcmp(observed.commit_id, "genesis") == 0);
    assert(strcmp(reader_etag, etag0) == 0);

    /* Two writers race from the same generation. */
    otsardb_head writer_a = make_head(1, "commit-a");
    otsardb_head writer_b = make_head(1, "commit-b");
    char *etag1 = NULL;
    assert(otsardb_journal_publish_head(store, "tenant/orders", &writer_a, reader_etag, 0, &etag1) == OTSARDB_JOURNAL_OK);
    assert(etag1 != NULL);
    assert(otsardb_journal_publish_head(store, "tenant/orders", &writer_b, reader_etag, 0, NULL) == OTSARDB_JOURNAL_CONFLICT);

    char *final_etag = NULL;
    assert(otsardb_journal_read_head(store, "tenant/orders", &observed, &final_etag) == OTSARDB_JOURNAL_OK);
    assert(observed.generation == 1);
    assert(strcmp(observed.commit_id, "commit-a") == 0);
    assert(strcmp(final_etag, etag1) == 0);

    free(etag0);
    free(reader_etag);
    free(etag1);
    free(final_etag);
    otsardb_test_memory_store_destroy(store);

    puts("journal CAS tests passed");
    return 0;
}

#ifndef OTSARDB_TEST_MEMORY_STORE_H
#define OTSARDB_TEST_MEMORY_STORE_H

#include "object_store.h"

otsardb_object_store *otsardb_test_memory_store_create(void);
otsardb_object_store *otsardb_test_memory_store_create_with_capabilities(otsardb_store_capabilities capabilities);
void otsardb_test_memory_store_destroy(otsardb_object_store *store);

/* Test helper: move an entry's Last-Modified back by `seconds` (clamped at
 * 0) to simulate an object that was written in the past. Enables
 * deterministic lease-expiry tests without sleeping. */
void otsardb_test_memory_store_age(otsardb_object_store *store,
                                 const char *key,
                                 int64_t seconds);

#endif

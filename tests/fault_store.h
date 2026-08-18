#ifndef OTSARDB_TEST_FAULT_STORE_H
#define OTSARDB_TEST_FAULT_STORE_H

#include "object_store.h"
#include <stddef.h>

otsardb_object_store *otsardb_test_fault_store_create(otsardb_object_store *inner);
void otsardb_test_fault_store_destroy(otsardb_object_store *store);
void otsardb_test_fault_store_fail_put(otsardb_object_store *store,
                                     size_t put_index,
                                     otsardb_store_result result,
                                     int after_delegate);
/* Fail every PUT whose 1-based index appears in `indexes` (multi-shot fault
 * injection for retry tests —). `after_delegate`=1 lets the
 * inner store commit first and overrides the result only on success. */
void otsardb_test_fault_store_fail_puts(otsardb_object_store *store,
                                      const size_t *indexes,
                                      size_t count,
                                      otsardb_store_result result,
                                      int after_delegate);
void otsardb_test_fault_store_fail_get(otsardb_object_store *store,
                                     size_t get_index,
                                     otsardb_store_result result);
void otsardb_test_fault_store_clear(otsardb_object_store *store);
/* Number of PUT calls seen since the last fail_put/fail_puts/clear call
 * (the counters are reset by the fault-injection setters). */
size_t otsardb_test_fault_store_put_count(otsardb_object_store *store);

#endif

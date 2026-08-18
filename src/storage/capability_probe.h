#ifndef OTSARDB_CAPABILITY_PROBE_H
#define OTSARDB_CAPABILITY_PROBE_H

#include "object_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct otsardb_store_probe_report {
    otsardb_store_capabilities capabilities;
    int s3_core_ok;
    int s3_cas_ok;
} otsardb_store_probe_report;

/* Run the capability probe against `store` using `probe_key` and fill
 * `out_report`. The probe is one-shot: on success the report is authoritative
 * and `store->capabilities` is updated.
 *
 * Retry policy: every S3 operation (PUT / GET / GET_RANGE / conditional PUTs /
 * streamed multipart PUT) is attempted up to 3 times total, with a 200 ms
 * sleep between attempts, but ONLY when the operation fails with a transient
 * transport result (OTSARDB_STORE_ERROR, OTSARDB_STORE_TIMEOUT,
 * OTSARDB_STORE_UNSUPPORTED). OTSARDB_STORE_PRECONDITION_FAILED and
 * OTSARDB_STORE_NOT_FOUND are valid probe answers and are never retried.
 * A valid result stops the retries immediately. If all attempts fail
 * transiently, the probe behaves exactly as before this policy: hard probe
 * steps report the failure (fail-closed, no write-capable open) and optional
 * capability steps simply leave the capability unset. */
otsardb_store_result otsardb_store_probe_capabilities(otsardb_object_store *store,
                                                   const char *probe_key,
                                                   otsardb_store_probe_report *out_report);

#ifdef __cplusplus
}
#endif

#endif

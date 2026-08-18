#include "capability_probe.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define OTSARDB_PROBE_MAX_ATTEMPTS 3
#define OTSARDB_PROBE_RETRY_DELAY_MS 200

static void probe_sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Only transient transport failures are retried. OTSARDB_STORE_PRECONDITION_FAILED
 * and OTSARDB_STORE_NOT_FOUND are valid probe answers, not failures. */
static int probe_rc_transient(otsardb_store_result rc) {
    return rc == OTSARDB_STORE_ERROR ||
           rc == OTSARDB_STORE_TIMEOUT ||
           rc == OTSARDB_STORE_UNSUPPORTED;
}

/* Run `attempt_expr` up to OTSARDB_PROBE_MAX_ATTEMPTS times, sleeping
 * OTSARDB_PROBE_RETRY_DELAY_MS between attempts, retrying only transient result
 * codes (see probe_rc_transient). A valid result is returned immediately;
 * after all attempts are exhausted the last transient result is returned. */
#define PROBE_RETRY(rc_var, attempt_expr)                                        \
    do {                                                                        \
        int probe_attempt = 0;                                                  \
        otsardb_store_result probe_rc;                                            \
        for (;;) {                                                              \
            probe_rc = (attempt_expr);                                          \
            if (!probe_rc_transient(probe_rc)) break;                           \
            probe_attempt++;                                                    \
            if (probe_attempt >= OTSARDB_PROBE_MAX_ATTEMPTS) break;               \
            probe_sleep_ms(OTSARDB_PROBE_RETRY_DELAY_MS);                         \
        }                                                                       \
        (rc_var) = probe_rc;                                                    \
    } while (0)

static int object_equals(const otsardb_store_object *object, const void *data, size_t size) {
    return object && object->size == size && (size == 0 || memcmp(object->data, data, size) == 0);
}

typedef struct mp_source {
    const unsigned char *parts[2];
    size_t sizes[2];
    int index;
} mp_source;

static long mp_read(void *ctx, void *buffer, size_t max_size) {
    mp_source *src = (mp_source *)ctx;
    if (src->index >= 2) return 0;
    size_t avail = src->sizes[src->index];
    if (avail > max_size) avail = max_size;
    if (avail == 0) return 0;
    memcpy(buffer, src->parts[src->index], avail);
    src->index++;
    return (long)avail;
}

otsardb_store_result otsardb_store_probe_capabilities(otsardb_object_store *store,
                                                   const char *probe_key,
                                                   otsardb_store_probe_report *out_report) {
    if (!store || !probe_key || probe_key[0] == '\0' || !out_report) {
        return OTSARDB_STORE_ERROR;
    }

    memset(out_report, 0, sizeof(*out_report));

    static const unsigned char BASE[] = "otsardb-s3-probe-0123456789";
    static const unsigned char NEXT[] = "otsardb-s3-probe-next";
    static const unsigned char BAD[] = "otsardb-s3-probe-bad";

    otsardb_store_put_options plain = {0};
    otsardb_store_result rc;
    PROBE_RETRY(rc, otsardb_store_put(store,
                                    probe_key,
                                    BASE,
                                    sizeof(BASE) - 1,
                                    &plain,
                                    NULL));
    if (rc != OTSARDB_STORE_OK) return rc;

    otsardb_store_object current = {0};
    PROBE_RETRY(rc, otsardb_store_get(store, probe_key, &current));
    if (rc != OTSARDB_STORE_OK) return rc;
    if (!object_equals(&current, BASE, sizeof(BASE) - 1)) {
        otsardb_store_release_object(store, &current);
        return OTSARDB_STORE_ERROR;
    }

    out_report->capabilities |= OTSARDB_STORE_CAP_READ_AFTER_WRITE;
    if (current.etag && current.etag[0] != '\0') {
        out_report->capabilities |= OTSARDB_STORE_CAP_ETAG;
    }
    otsardb_store_release_object(store, &current);

    otsardb_store_object range = {0};
    PROBE_RETRY(rc, otsardb_store_get_range(store, probe_key, 8, 8, &range));
    if (rc == OTSARDB_STORE_OK) {
        static const unsigned char EXPECTED_RANGE[] = "s3-probe";
        if (object_equals(&range, EXPECTED_RANGE, sizeof(EXPECTED_RANGE) - 1)) {
            out_report->capabilities |= OTSARDB_STORE_CAP_RANGE_GET;
        }
        otsardb_store_release_object(store, &range);
    }

    otsardb_store_put_options create_only = {0};
    create_only.if_none_match = 1;
    PROBE_RETRY(rc, otsardb_store_put(store,
                                    probe_key,
                                    BAD,
                                    sizeof(BAD) - 1,
                                    &create_only,
                                    NULL));
    if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
        out_report->capabilities |= OTSARDB_STORE_CAP_CONDITIONAL_CREATE;
    } else if (rc == OTSARDB_STORE_OK) {
        PROBE_RETRY(rc, otsardb_store_put(store,
                                        probe_key,
                                        BASE,
                                        sizeof(BASE) - 1,
                                        &plain,
                                        NULL));
        if (rc != OTSARDB_STORE_OK) return rc;
    }

    memset(&current, 0, sizeof(current));
    PROBE_RETRY(rc, otsardb_store_get(store, probe_key, &current));
    if (rc != OTSARDB_STORE_OK) return rc;

    if (current.etag && current.etag[0] != '\0') {
        char *etag = (char *)malloc(strlen(current.etag) + 1);
        if (!etag) {
            otsardb_store_release_object(store, &current);
            return OTSARDB_STORE_ERROR;
        }
        strcpy(etag, current.etag);
        otsardb_store_release_object(store, &current);

        otsardb_store_put_options stale = {0};
        stale.if_match_etag = "otsardb-stale-etag-that-must-not-match";
        PROBE_RETRY(rc, otsardb_store_put(store,
                                        probe_key,
                                        BAD,
                                        sizeof(BAD) - 1,
                                        &stale,
                                        NULL));
        if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
            otsardb_store_put_options correct = {0};
            correct.if_match_etag = etag;
            PROBE_RETRY(rc, otsardb_store_put(store,
                                            probe_key,
                                            NEXT,
                                            sizeof(NEXT) - 1,
                                            &correct,
                                            NULL));
            if (rc == OTSARDB_STORE_OK) {
                out_report->capabilities |= OTSARDB_STORE_CAP_CONDITIONAL_UPDATE;
            }
        }
        free(etag);
    } else {
        otsardb_store_release_object(store, &current);
    }

    const otsardb_store_capabilities core_required =
        OTSARDB_STORE_CAP_ETAG |
        OTSARDB_STORE_CAP_RANGE_GET |
        OTSARDB_STORE_CAP_READ_AFTER_WRITE;
    const otsardb_store_capabilities cas_required =
        OTSARDB_STORE_CAP_CONDITIONAL_CREATE |
        OTSARDB_STORE_CAP_CONDITIONAL_UPDATE;

    static const unsigned char MP_PART1[] = "otsardb-multipart-probe-";
    static const unsigned char MP_PART2[] = "0123456789abcdef";

    mp_source mp_src;
    mp_src.parts[0] = MP_PART1;
    mp_src.sizes[0] = sizeof(MP_PART1) - 1;
    mp_src.parts[1] = MP_PART2;
    mp_src.sizes[1] = sizeof(MP_PART2) - 1;
    mp_src.index = 0;

    char *mp_etag = NULL;
    otsardb_store_result mp_rc;
    /* Each attempt must rewind the pull source: a failed attempt may have
     * consumed parts, and put_stream never partially re-reads them. */
    PROBE_RETRY(mp_rc, (mp_src.index = 0,
                        otsardb_store_put_stream(store,
                                               probe_key,
                                               mp_read,
                                               &mp_src,
                                               (uint64_t)((sizeof(MP_PART1) - 1) + (sizeof(MP_PART2) - 1)),
                                               &plain,
                                               &mp_etag)));

    if (mp_rc == OTSARDB_STORE_OK) {
        otsardb_store_object mp_obj = {0};
        PROBE_RETRY(mp_rc, otsardb_store_get(store, probe_key, &mp_obj));
        if (mp_rc == OTSARDB_STORE_OK) {
            static const unsigned char MP_FULL[] = "otsardb-multipart-probe-0123456789abcdef";
            if (object_equals(&mp_obj, MP_FULL, sizeof(MP_FULL) - 1) &&
                mp_etag && mp_etag[0] != '\0') {
                out_report->capabilities |= OTSARDB_STORE_CAP_MULTIPART;
            }
            otsardb_store_release_object(store, &mp_obj);
        }
        free(mp_etag);
    } else {
        free(mp_etag);
    }

    out_report->s3_core_ok =
        (out_report->capabilities & core_required) == core_required;
    out_report->s3_cas_ok =
        out_report->s3_core_ok &&
        (out_report->capabilities & cas_required) == cas_required;

    store->capabilities = out_report->capabilities;
    return OTSARDB_STORE_OK;
}

#include "replica.h"

#include "capability_probe.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "journal.h"
#include "lease.h"
#include "recovery.h"
#include "s3_object_store.h"
#include "segment.h"
#include "sha256.h"
#include "wal_publisher.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#else
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * â€” Model B synchronous dual publish.
 *
 * The replica destination is configured ONLY through the environment
 * (OTSARDB_S3_REPLICA_*). Everything below is a replica writer module over the
 * existing store API: open a second store instance (probe + capabilities,
 * fail-closed), replicate objects immutably (GET+compare, 
 * 0036/0065 semantics), write the per-commit ledger with CAS on both
 * destinations, CAS the replica HEAD (single global commit selection), and
 * create the one-time epoch record (If-None-Match; a 412 is handled as
 * "already created by an earlier session").
 *
 * The ledger + epoch protocol is ORIGINAL per
 * section 2: optimistic-concurrency
 * intent states ADAPTed for the per-commit ledger state machine, with
 * epoch fencing REIMPLEMENTed onto S3 CAS. The object-immutability helpers
 * are REIMPLEMENTed mirrors of commit_publish.c's immutable_put (same
 * GET+compare CAS semantics), kept here because
 * commit_publish.c is outside this module's file scope.
 * ------------------------------------------------------------------------- */

/* The env-driven store creation (otsardb_replica_open below) needs the S3
 * adapter (s3_object_store.cpp), which is OPTIONAL linkage: otsardb_core must
 * build and link in BOTH configurations (OTSARDB_ENABLE_S3=OFF core build and
 * the S3 build). The open path is therefore compiled only when the build
 * defines OTSARDB_ENABLE_S3 for otsardb_core (coordinator wiring:
 * `target_compile_definitions(otsardb_core PRIVATE OTSARDB_ENABLE_S3=1)` inside
 * the OTSARDB_ENABLE_S3 CMake branch; the otsardb executable already defines it
 * for its own translation units). Without the macro the open is a fail-
 * closed stub: a core build cannot reach a replica destination anyway (the
 * S3 native open lives in the S3 build), and a misconfigured environment
 * must never silently degrade to a single destination. */

/* One replica destination of the handle (the handle manages
 * a LIST of up to OTSARDB_REPLICA_MAX_DESTINATIONS destinations). The
 * per-destination epoch record (0077 promotion fence) lives here: the value
 * this session adopted at attach/open, re-verified before every
 * otsardb_replica_replicate_commit and raised by otsardb_replica_promote. */
typedef struct otsardb_replica_destination {
    otsardb_object_store *store; /* owned unless store_borrowed */
    otsardb_write_strategy strategy;
    int store_borrowed;
    int epoch_created;
    uint64_t epoch;
    char epoch_etag[128];
    int epoch_known;
} otsardb_replica_destination;

struct otsardb_replica {
    otsardb_replica_destination *destinations;
    size_t destination_count;
    size_t destination_capacity;
    char database_root[OTSARDB_KEY_MAX + 1];
    char database_id[OTSARDB_COMMIT_ID_MAX + 1];
    int primary_epoch_created;
    /* the index of the destination promoted by the last
     * otsardb_replica_promote (-1 = none). */
    int promoted_index;
};

/* ---- environment -------------------------------------------------------- */

static const char *env_nonempty(const char *name) {
    const char *value = getenv(name);
    return value && value[0] != '\0' ? value : NULL;
}

/* Variable sets: set 0 = OTSARDB_S3_REPLICA_* (0072),
 * set 1 = OTSARDB_S3_REPLICA2_*, set 2 = OTSARDB_S3_REPLICA3_*. */
static const char *const replica_env_prefixes[OTSARDB_REPLICA_MAX_DESTINATIONS] = {
    "OTSARDB_S3_REPLICA", "OTSARDB_S3_REPLICA2", "OTSARDB_S3_REPLICA3"};

static const char *replica_env_value(int var_set, const char *suffix,
                                     char *name_buf, size_t name_buf_size) {
    if (var_set < 0 || var_set >= OTSARDB_REPLICA_MAX_DESTINATIONS) return NULL;
    if (snprintf(name_buf, name_buf_size, "%s_%s",
                 replica_env_prefixes[var_set], suffix) >= (int)name_buf_size) {
        return NULL;
    }
    return env_nonempty(name_buf);
}

static int replica_var_set_configured(int var_set) {
    char buf[64];
    return replica_env_value(var_set, "ENDPOINT", buf, sizeof(buf)) != NULL ||
           replica_env_value(var_set, "REGION", buf, sizeof(buf)) != NULL ||
           replica_env_value(var_set, "ACCESS_KEY", buf, sizeof(buf)) != NULL ||
           replica_env_value(var_set, "SECRET_KEY", buf, sizeof(buf)) != NULL ||
           replica_env_value(var_set, "BUCKET", buf, sizeof(buf)) != NULL;
}

int otsardb_replica_configured(void) {
    /* ANY replica variable of ANY set means the operator intends dual/
     * multi-destination publish: the open path then validates completeness
     * and FAILS CLOSED instead of silently degrading to a single
     * destination. */
    for (int set = 0; set < OTSARDB_REPLICA_MAX_DESTINATIONS; ++set) {
        if (replica_var_set_configured(set)) return 1;
    }
    return 0;
}

/* OTSARDB_S3_PROMOTE=1 -> the session promotes the replica
 * destination to primary at open. Any other value (0, garbage, absent)
 * means no promotion (fail closed: a typo must never promote). */
int otsardb_replica_promote_requested(void) {
    const char *value = getenv("OTSARDB_S3_PROMOTE");
    if (!value || value[0] == '\0') return 0;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    return end != value && *end == '\0' && n != 0;
}

/* OTSARDB_S3_READ_FROM_REPLICA=1 -> the session opens as a
 * READ-ONLY reader over the replica destination (the recovery/read path
 * uses the replica store instead of the primary). Any other value (0,
 * garbage, absent) means a normal primary session (fail closed: a typo
 * must never switch the read path). */
int otsardb_replica_read_requested(void) {
    const char *value = getenv("OTSARDB_S3_READ_FROM_REPLICA");
    if (!value || value[0] == '\0') return 0;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    return end != value && *end == '\0' && n != 0;
}

/* OTSARDB_S3_QUORUM â€” the minimum number of destinations of
 * M (M = 1 primary + R destinations; the primary always counts as 1) that
 * must hold the COMMITTED ledger for the sync to ack. 0 / absent / garbage
 * = the safe default (EVERY destination must confirm â€” with R = 1 that is
 * the byte-identical 0072 Model B contract). A positive value is returned
 * verbatim; the effective clamp to 1..M happens in otsardb_replica_quorum. */
int otsardb_replica_quorum_requested(void) {
    const char *value = getenv("OTSARDB_S3_QUORUM");
    if (!value || value[0] == '\0') return 0;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0' || n <= 0) return 0;
    /* A saturated strtol (overflow, LONG_MAX on 32-bit Windows) must not
     * pass as a plausible quorum: treat INT_MAX-or-beyond as unset (all). */
    return n >= (long)INT_MAX ? 0 : (int)n;
}

int otsardb_replica_quorum(const otsardb_replica *replica) {
    if (!replica) return 1;
    size_t m = replica->destination_count + 1u; /* primary + destinations */
    if (m == 0) return 1;
    int requested = otsardb_replica_quorum_requested();
    if (requested <= 0) return (int)m;
    return requested >= (int)m ? (int)m : requested;
}

size_t otsardb_replica_destination_count(const otsardb_replica *replica) {
    return replica ? replica->destination_count : 0;
}

int otsardb_replica_promoted_index(const otsardb_replica *replica) {
    return replica ? replica->promoted_index : -1;
}

/* OTSARDB_LEASE_TTL (lease module policy): 0/absent/garbage -> the lease
 * default. The promotion liveness check uses the SAME operator knob as the
 * session's lease. */
static uint64_t replica_lease_ttl_env(void) {
    const char *value = getenv("OTSARDB_LEASE_TTL");
    if (!value || value[0] == '\0') return OTSARDB_LEASE_TTL_DEFAULT_SECS;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0' || n <= 0) return OTSARDB_LEASE_TTL_DEFAULT_SECS;
    return (uint64_t)n;
}

/* ---- small helpers ------------------------------------------------------ */

static int root_join(const char *root, const char *suffix, char *out, size_t out_size) {
    if (!root || !suffix || !out || out_size == 0) return 0;
    size_t len = strlen(root);
    int needs_slash = len > 0 && root[len - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", root, needs_slash ? "/" : "", suffix);
    return n > 0 && (size_t)n < out_size;
}

static void utc_now(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int json_extract_u64(const char *json, const char *name, uint64_t *out) {
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') ++p;
    char *end = NULL;
    unsigned long long value = strtoull(p, &end, 10);
    if (end == p) return 0;
    *out = (uint64_t)value;
    return 1;
}

static int json_extract_string(const char *json, const char *name, char *out, size_t out_size) {
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\":\"", name) >= (int)sizeof(pattern)) return 0;
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_size) return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int existing_object_matches(otsardb_object_store *store,
                                   const char *key,
                                   const void *data,
                                   size_t size) {
    otsardb_store_object existing = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &existing);
    if (rc == OTSARDB_STORE_NOT_FOUND) return 0;
    if (rc != OTSARDB_STORE_OK) return -1;
    int identical = existing.size == size && (size == 0 || memcmp(existing.data, data, size) == 0);
    otsardb_store_release_object(store, &existing);
    return identical ? 1 : 0;
}

/* Immutable content-addressed PUT, mirroring commit_publish.c's
 * immutable_put exactly (/0065 semantics): conditional create
 * when the store supports it, PRECONDITION_FAILED / TIMEOUT resolved by
 * GET + byte compare; Core-only stores get verify-then-write. */
static otsardb_journal_result replicate_object(otsardb_object_store *store,
                                             const char *key,
                                             const void *data,
                                             size_t size) {
    if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
        otsardb_store_put_options options = {0};
        options.if_none_match = 1;
        otsardb_store_result rc = otsardb_store_put(store, key, data, size, &options, NULL);
        if (rc == OTSARDB_STORE_OK) return OTSARDB_JOURNAL_OK;
        if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
            int matches = existing_object_matches(store, key, data, size);
            return matches < 0 ? OTSARDB_JOURNAL_ERROR : (matches ? OTSARDB_JOURNAL_OK
                                                                : OTSARDB_JOURNAL_CONFLICT);
        }
        if (rc == OTSARDB_STORE_TIMEOUT) {
            int matches = existing_object_matches(store, key, data, size);
            if (matches < 0) return OTSARDB_JOURNAL_ERROR;
            return matches ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_AMBIGUOUS;
        }
        if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
        return OTSARDB_JOURNAL_ERROR;
    }

    int matches = existing_object_matches(store, key, data, size);
    if (matches > 0) return OTSARDB_JOURNAL_OK;
    if (matches < 0) return OTSARDB_JOURNAL_ERROR;

    otsardb_store_put_options options = {0};
    otsardb_store_result rc = otsardb_store_put(store, key, data, size, &options, NULL);
    if (rc == OTSARDB_STORE_OK) return OTSARDB_JOURNAL_OK;
    if (rc == OTSARDB_STORE_TIMEOUT) {
        int resolved = existing_object_matches(store, key, data, size);
        if (resolved < 0) return OTSARDB_JOURNAL_ERROR;
        return resolved ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_AMBIGUOUS;
    }
    if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
    return OTSARDB_JOURNAL_ERROR;
}

/* ---- ledger -------------------------------------------------------------- */

static int ledger_encode(const otsardb_replica_ledger *ledger,
                         char **out_json,
                         size_t *out_size) {
    int needed = snprintf(NULL, 0,
                          "{\"commit_id\":\"%s\",\"generation\":%llu,"
                          "\"parent_generation\":%llu,\"manifest_sha256\":\"%s\","
                          "\"state\":\"%s\",\"primary_etag\":\"%s\","
                          "\"replica_etag\":\"%s\",\"model\":\"%s\","
                          "\"destination\":\"%s\",\"written_at_utc\":\"%s\"}\n",
                          ledger->commit_id,
                          (unsigned long long)ledger->generation,
                          (unsigned long long)ledger->parent_generation,
                          ledger->manifest_sha256,
                          ledger->state,
                          ledger->primary_etag,
                          ledger->replica_etag,
                          ledger->model,
                          ledger->destination,
                          ledger->written_at_utc);
    if (needed < 0) return 0;

    char *json = (char *)malloc((size_t)needed + 1);
    if (!json) return 0;

    int written = snprintf(json, (size_t)needed + 1,
                           "{\"commit_id\":\"%s\",\"generation\":%llu,"
                           "\"parent_generation\":%llu,\"manifest_sha256\":\"%s\","
                           "\"state\":\"%s\",\"primary_etag\":\"%s\","
                           "\"replica_etag\":\"%s\",\"model\":\"%s\","
                           "\"destination\":\"%s\",\"written_at_utc\":\"%s\"}\n",
                           ledger->commit_id,
                           (unsigned long long)ledger->generation,
                           (unsigned long long)ledger->parent_generation,
                           ledger->manifest_sha256,
                           ledger->state,
                           ledger->primary_etag,
                           ledger->replica_etag,
                           ledger->model,
                           ledger->destination,
                           ledger->written_at_utc);
    if (written != needed) {
        free(json);
        return 0;
    }
    *out_json = json;
    *out_size = (size_t)written;
    return 1;
}

static int ledger_decode(const char *json, size_t size, otsardb_replica_ledger *out) {
    if (!json || !out || size == 0) return 0;
    char *copy = (char *)malloc(size + 1);
    if (!copy) return 0;
    memcpy(copy, json, size);
    copy[size] = '\0';

    memset(out, 0, sizeof(*out));
    int ok = json_extract_string(copy, "commit_id", out->commit_id, sizeof(out->commit_id)) &&
             json_extract_u64(copy, "generation", &out->generation) &&
             json_extract_string(copy, "state", out->state, sizeof(out->state));
    free(copy);
    return ok;
}

static char *dup_string(const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, value, len);
    return copy;
}

/* CAS-guarded ledger write, idempotent under re-attempts:
 * - absent -> conditional create (If-None-Match); a 412 means the object
 * appeared concurrently -> re-read and resolve by commit identity;
 * - present with the same commit id and the SAME state -> OK (no-op);
 * - present with the same commit id but an EARLIER state (PREPARED/ABORTED
 * being advanced to COMMITTED) -> CAS update (If-Match the current etag);
 * - present with a DIFFERENT commit id -> fail closed (key collision by
 * construction; a foreign ledger means a misconfigured destination).
 * On stores without conditional support the write is a plain PUT after the
 * same read-side identity check (single-writer-per-destination, design
 * section 4 asymmetry rule). */
static otsardb_journal_result ledger_write_cas(otsardb_object_store *store,
                                             const char *key,
                                             const char *json,
                                             size_t size,
                                             const char *expected_commit_id,
                                             const char *expected_state,
                                             char **out_etag) {
    if (out_etag) *out_etag = NULL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        otsardb_store_object existing = {0};
        otsardb_store_result read_rc = otsardb_store_get(store, key, &existing);
        if (read_rc == OTSARDB_STORE_OK) {
            otsardb_replica_ledger current = {0};
            int decoded = ledger_decode((const char *)existing.data, existing.size, &current);
            if (!decoded || strcmp(current.commit_id, expected_commit_id) != 0) {
                otsardb_store_release_object(store, &existing);
                return OTSARDB_JOURNAL_CONFLICT;
            }
            if (strcmp(current.state, expected_state) == 0 ||
                strcmp(current.state, OTSARDB_REPLICA_STATE_COMMITTED) == 0) {
                /* Same state, or the state machine's terminal state: the
                 * record is final (COMMITTED is never regressed to PREPARED
                 * by an idempotent re-publish, and ABORTED never clobbers a
                 * COMMITTED record). */
                if (out_etag && existing.etag && existing.etag[0] != '\0') {
                    *out_etag = dup_string(existing.etag);
                }
                otsardb_store_release_object(store, &existing);
                return OTSARDB_JOURNAL_OK;
            }

            otsardb_store_put_options options = {0};
            if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_UPDATE) &&
                existing.etag && existing.etag[0] != '\0') {
                options.if_match_etag = existing.etag;
            }
            char *put_etag = NULL;
            otsardb_store_result put_rc = otsardb_store_put(store, key, json, size, &options, &put_etag);
            otsardb_store_release_object(store, &existing);
            if (put_rc == OTSARDB_STORE_OK) {
                if (out_etag) *out_etag = put_etag;
                else free(put_etag);
                return OTSARDB_JOURNAL_OK;
            }
            if (put_rc == OTSARDB_STORE_PRECONDITION_FAILED) {
                free(put_etag);
                continue; /* concurrent change: re-read and re-resolve */
            }
            if (put_rc == OTSARDB_STORE_TIMEOUT) {
                /* The ledger content is not byte-stable across attempts
                 * (written_at_utc): a timeout resolves by identity + state â€”
                 * the record either exists with the expected commit/state
                 * (OK) or is still absent (AMBIGUOUS, retried by the cycle). */
                otsardb_store_object after = {0};
                otsardb_store_result after_rc = otsardb_store_get(store, key, &after);
                if (after_rc == OTSARDB_STORE_OK) {
                    otsardb_replica_ledger resolved = {0};
                    int resolved_ok = ledger_decode((const char *)after.data,
                                                    after.size, &resolved);
                    int resolved_match = resolved_ok &&
                                         strcmp(resolved.commit_id, expected_commit_id) == 0 &&
                                         strcmp(resolved.state, expected_state) == 0;
                    otsardb_store_release_object(store, &after);
                    return resolved_match ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_AMBIGUOUS;
                }
                if (after_rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_JOURNAL_AMBIGUOUS;
                return OTSARDB_JOURNAL_ERROR;
            }
            free(put_etag);
            if (put_rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
            return OTSARDB_JOURNAL_ERROR;
        }
        if (read_rc == OTSARDB_STORE_NOT_FOUND) {
            otsardb_store_put_options options = {0};
            if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
                options.if_none_match = 1;
            }
            char *put_etag = NULL;
            otsardb_store_result put_rc = otsardb_store_put(store, key, json, size, &options, &put_etag);
            if (put_rc == OTSARDB_STORE_OK) {
                if (out_etag) *out_etag = put_etag;
                else free(put_etag);
                return OTSARDB_JOURNAL_OK;
            }
            if (put_rc == OTSARDB_STORE_PRECONDITION_FAILED) {
                free(put_etag);
                continue; /* object appeared concurrently: re-read */
            }
            if (put_rc == OTSARDB_STORE_TIMEOUT) {
                /* Identity+state resolution, as above (written_at_utc makes
                 * byte comparison wrong across attempts). */
                otsardb_store_object after = {0};
                otsardb_store_result after_rc = otsardb_store_get(store, key, &after);
                if (after_rc == OTSARDB_STORE_OK) {
                    otsardb_replica_ledger resolved = {0};
                    int resolved_ok = ledger_decode((const char *)after.data,
                                                    after.size, &resolved);
                    int resolved_match = resolved_ok &&
                                         strcmp(resolved.commit_id, expected_commit_id) == 0 &&
                                         strcmp(resolved.state, expected_state) == 0;
                    otsardb_store_release_object(store, &after);
                    return resolved_match ? OTSARDB_JOURNAL_OK : OTSARDB_JOURNAL_AMBIGUOUS;
                }
                if (after_rc == OTSARDB_STORE_NOT_FOUND) return OTSARDB_JOURNAL_AMBIGUOUS;
                return OTSARDB_JOURNAL_ERROR;
            }
            free(put_etag);
            if (put_rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
            return OTSARDB_JOURNAL_ERROR;
        }
        if (read_rc == OTSARDB_STORE_TIMEOUT) return OTSARDB_JOURNAL_AMBIGUOUS;
        if (read_rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
        return OTSARDB_JOURNAL_ERROR;
    }
    return OTSARDB_JOURNAL_ERROR;
}

/* ---- epoch (0072 creation + 0077 CAS raise / adoption / fence) ----------- */

/* v0 creation-only record (0072): epoch 1, immutable per destination in v0.
 * 0077 keeps this exact format for records created by attach/open and adds
 * the promoted variant below (epoch N+1, note updated). */
static int epoch_encode(char *out, size_t out_size, const char *owner,
                        uint64_t generation) {
    char stamp[32];
    utc_now(stamp, sizeof(stamp));
    int n = snprintf(out, out_size,
                     "{\"epoch\":1,\"owner\":\"%s\",\"generation\":%llu,"
                     "\"model\":\"B\",\"note\":\"v0 creation-only record; "
                     "promotion/roll-forward deferred (0072)\","
                     "\"written_at_utc\":\"%s\"}\n",
                     owner,
                     (unsigned long long)generation,
                     stamp);
    return n > 0 && (size_t)n < out_size;
}

/* promoted record â€” the CAS raise rewrites the epoch object
 * with epoch N+1 and the promotion note (design section 4). */
static int epoch_encode_promoted(char *out, size_t out_size, uint64_t epoch,
                                 const char *owner, uint64_t generation) {
    char stamp[32];
    utc_now(stamp, sizeof(stamp));
    int n = snprintf(out, out_size,
                     "{\"epoch\":%llu,\"owner\":\"%s\",\"generation\":%llu,"
                     "\"model\":\"B\",\"note\":\"promoted to primary (0077); "
                     "the old primary was offline per the operator contract\","
                     "\"written_at_utc\":\"%s\"}\n",
                     (unsigned long long)epoch,
                     owner,
                     (unsigned long long)generation,
                     stamp);
    return n > 0 && (size_t)n < out_size;
}

int otsardb_replica_epoch_read(otsardb_object_store *store,
                             const char *database_root,
                             otsardb_replica_epoch_view *out,
                             otsardb_journal_result *out_rc) {
    if (!store || !database_root || !out || !out_rc) return 0;
    *out_rc = OTSARDB_JOURNAL_ERROR;
    memset(out, 0, sizeof(*out));
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "replica/epoch.json", key, sizeof(key))) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) {
        /* A legitimate "never created" state (a fresh destination): the
         * view says exists=0 and the caller may create at epoch 1. */
        out->exists = 0;
        *out_rc = OTSARDB_JOURNAL_OK;
        return 1;
    }
    if (rc != OTSARDB_STORE_OK) {
        *out_rc = rc == OTSARDB_STORE_TIMEOUT ? OTSARDB_JOURNAL_AMBIGUOUS
                 : rc == OTSARDB_STORE_UNSUPPORTED ? OTSARDB_JOURNAL_UNSUPPORTED
                                                 : OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    char *copy = (char *)malloc(object.size + 1);
    if (!copy) {
        otsardb_store_release_object(store, &object);
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    memcpy(copy, object.data, object.size);
    copy[object.size] = '\0';
    int ok = json_extract_u64(copy, "epoch", &out->epoch);
    json_extract_u64(copy, "generation", &out->generation);
    json_extract_string(copy, "owner", out->owner, sizeof(out->owner));
    if (object.etag && object.etag[0]) {
        snprintf(out->etag, sizeof(out->etag), "%s", object.etag);
    }
    free(copy);
    otsardb_store_release_object(store, &object);
    if (!ok) {
        /* A foreign/corrupt record fails closed: the destination epoch must
         * be a number for the fence to be meaningful. */
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    out->exists = 1;
    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

/* One CAS raise attempt against an explicit view: If-Match the view's etag
 * (or If-None-Match when the view says the record is absent). Returns the
 * journal result only (no retry) â€” the drill path is deterministic. */
static otsardb_journal_result epoch_raise_attempt(otsardb_object_store *store,
                                                const char *key,
                                                const char *json,
                                                size_t size,
                                                const otsardb_replica_epoch_view *current,
                                                uint64_t *out_new_epoch) {
    otsardb_store_put_options options;
    memset(&options, 0, sizeof(options));
    if (current->exists) {
        if (!current->etag[0]) return OTSARDB_JOURNAL_ERROR;
        options.if_match_etag = current->etag;
    } else if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
        options.if_none_match = 1;
    }
    char *put_etag = NULL;
    otsardb_store_result rc = otsardb_store_put(store, key, json, size, &options, &put_etag);
    if (rc == OTSARDB_STORE_OK) {
        if (out_new_epoch) *out_new_epoch = 0; /* filled by the caller */
        free(put_etag);
        return OTSARDB_JOURNAL_OK;
    }
    free(put_etag);
    if (rc == OTSARDB_STORE_PRECONDITION_FAILED) return OTSARDB_JOURNAL_CONFLICT;
    if (rc == OTSARDB_STORE_TIMEOUT) return OTSARDB_JOURNAL_AMBIGUOUS;
    if (rc == OTSARDB_STORE_UNSUPPORTED) return OTSARDB_JOURNAL_UNSUPPORTED;
    return OTSARDB_JOURNAL_ERROR;
}

int otsardb_replica_epoch_raise(otsardb_object_store *store,
                              const char *database_root,
                              const char *owner,
                              const otsardb_replica_epoch_view *current,
                              uint64_t *out_new_epoch,
                              otsardb_journal_result *out_rc) {
    if (out_new_epoch) *out_new_epoch = 0;
    if (!store || !database_root || !owner || !owner[0] || !out_rc) {
        if (out_rc) *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    if (strchr(owner, '"')) {
        *out_rc = OTSARDB_JOURNAL_INVALID; /* would break the JSON object */
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "replica/epoch.json", key, sizeof(key))) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }

    if (current) {
        /* Verbatim-view raise: the deterministic split-brain drill. The CAS
         * either wins or reports CONFLICT â€” no re-read, no retry. */
        uint64_t new_epoch = current->exists ? current->epoch + 1 : 1;
        char json[512];
        if (!epoch_encode_promoted(json, sizeof(json), new_epoch, owner,
                                   current->generation)) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        otsardb_journal_result rc = epoch_raise_attempt(store, key, json, strlen(json),
                                                      current, NULL);
        if (rc == OTSARDB_JOURNAL_OK && out_new_epoch) *out_new_epoch = new_epoch;
        *out_rc = rc;
        return rc == OTSARDB_JOURNAL_OK;
    }

    /* Self-read raise (the single-call promote primitive): read, then CAS
     * N -> N+1 (create at 1 when absent); a concurrent 412 re-reads and
     * retries up to 3 attempts. */
    for (int attempt = 0; attempt < 3; ++attempt) {
        otsardb_replica_epoch_view view;
        otsardb_journal_result read_rc = OTSARDB_JOURNAL_ERROR;
        if (!otsardb_replica_epoch_read(store, database_root, &view, &read_rc)) {
            *out_rc = read_rc;
            return 0;
        }
        uint64_t new_epoch = view.exists ? view.epoch + 1 : 1;
        char json[512];
        if (!epoch_encode_promoted(json, sizeof(json), new_epoch, owner,
                                   view.generation)) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        otsardb_journal_result rc = epoch_raise_attempt(store, key, json, strlen(json),
                                                      &view, NULL);
        if (rc == OTSARDB_JOURNAL_OK) {
            if (out_new_epoch) *out_new_epoch = new_epoch;
            *out_rc = OTSARDB_JOURNAL_OK;
            return 1;
        }
        if (rc == OTSARDB_JOURNAL_CONFLICT) continue; /* concurrent change: re-read */
        if (rc == OTSARDB_JOURNAL_AMBIGUOUS) {
            /* Timeout: resolve by identity â€” the record either now carries
             * exactly the raised epoch (OK) or is still behind (AMBIGUOUS,
             * retried by the operator). A record BEYOND our raise means a
             * concurrent promotion won after our attempt â€” AMBIGUOUS, never
             * an adoption of the winner's epoch. */
            otsardb_replica_epoch_view after;
            otsardb_journal_result after_rc = OTSARDB_JOURNAL_ERROR;
            if (otsardb_replica_epoch_read(store, database_root, &after, &after_rc) &&
                after.exists && after.epoch == new_epoch) {
                if (out_new_epoch) *out_new_epoch = after.epoch;
                *out_rc = OTSARDB_JOURNAL_OK;
                return 1;
            }
            *out_rc = after_rc == OTSARDB_JOURNAL_OK ? OTSARDB_JOURNAL_AMBIGUOUS: after_rc;
            return 0;
        }
        *out_rc = rc;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_CONFLICT; /* three CAS attempts lost the race */
    return 0;
}

/* Create-or-adopt the epoch record of one destination (attach/open path).
 * The record is created ONCE per destination at epoch 1 (0072); a record
 * that already exists â€” an earlier session, or a promotion â€” is ADOPTED
 * verbatim (its epoch becomes the session's fence value, 0077). Returns 1
 * on success; 0 on store/format errors (fail closed). */
static int epoch_create_or_adopt(otsardb_object_store *store,
                                 const char *root,
                                 const char *database_id,
                                 uint64_t generation,
                                 uint64_t *out_epoch,
                                 char *out_etag,
                                 size_t out_etag_size,
                                 otsardb_journal_result *out_rc) {
    *out_rc = OTSARDB_JOURNAL_ERROR;
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(root, "replica/epoch.json", key, sizeof(key))) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_OK) {
        otsardb_replica_epoch_view view;
        char *copy = (char *)malloc(object.size + 1);
        if (!copy) {
            otsardb_store_release_object(store, &object);
            *out_rc = OTSARDB_JOURNAL_ERROR;
            return 0;
        }
        memcpy(copy, object.data, object.size);
        copy[object.size] = '\0';
        int ok = json_extract_u64(copy, "epoch", &view.epoch) &&
                 json_extract_u64(copy, "generation", &view.generation);
        json_extract_string(copy, "owner", view.owner, sizeof(view.owner));
        free(copy);
        if (!ok) {
            /* A foreign/corrupt epoch record fails the open: never adopt an
             * unparseable fence. */
            otsardb_store_release_object(store, &object);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        if (out_epoch) *out_epoch = view.epoch;
        if (out_etag && out_etag_size && object.etag && object.etag[0]) {
            snprintf(out_etag, out_etag_size, "%s", object.etag);
        }
        otsardb_store_release_object(store, &object);
        *out_rc = OTSARDB_JOURNAL_OK;
        return 1;
    }
    if (rc != OTSARDB_STORE_NOT_FOUND) {
        *out_rc = rc == OTSARDB_STORE_TIMEOUT ? OTSARDB_JOURNAL_AMBIGUOUS
                 : rc == OTSARDB_STORE_UNSUPPORTED ? OTSARDB_JOURNAL_UNSUPPORTED
                                                 : OTSARDB_JOURNAL_ERROR;
        return 0;
    }

    char json[512];
    if (!epoch_encode(json, sizeof(json), database_id, generation)) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    if (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE)) {
        otsardb_store_put_options options = {0};
        options.if_none_match = 1;
        char *put_etag = NULL;
        otsardb_store_result put_rc = otsardb_store_put(store, key, json, strlen(json),
                                                    &options, &put_etag);
        if (put_rc == OTSARDB_STORE_OK) {
            if (out_epoch) *out_epoch = 1;
            if (out_etag && out_etag_size && put_etag && put_etag[0]) {
                snprintf(out_etag, out_etag_size, "%s", put_etag);
            }
            free(put_etag);
            *out_rc = OTSARDB_JOURNAL_OK;
            return 1;
        }
        free(put_etag);
        if (put_rc == OTSARDB_STORE_PRECONDITION_FAILED) {
            /* A concurrent session created it between our read and write:
             * re-read and adopt (one retry, like the 0072 412-as-success
             * rule). */
            return epoch_create_or_adopt(store, root, database_id, generation,
                                         out_epoch, out_etag, out_etag_size, out_rc);
        }
        if (put_rc == OTSARDB_STORE_TIMEOUT) {
            otsardb_store_object after = {0};
            otsardb_store_result get_rc = otsardb_store_get(store, key, &after);
            if (get_rc == OTSARDB_STORE_OK) {
                otsardb_store_release_object(store, &after);
                *out_rc = OTSARDB_JOURNAL_AMBIGUOUS;
                return 0;
            }
            if (get_rc == OTSARDB_STORE_NOT_FOUND) {
                *out_rc = OTSARDB_JOURNAL_AMBIGUOUS;
                return 0;
            }
            *out_rc = OTSARDB_JOURNAL_ERROR;
            return 0;
        }
        if (put_rc == OTSARDB_STORE_UNSUPPORTED) {
            *out_rc = OTSARDB_JOURNAL_UNSUPPORTED;
            return 0;
        }
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }

    /* Core-only destination (no conditional create): plain PUT after the
     * read side confirmed absence (single-writer-per-destination, design
     * section 4 asymmetry rule). */
    otsardb_store_result put_rc = otsardb_store_put(store, key, json, strlen(json), NULL, NULL);
    if (put_rc == OTSARDB_STORE_OK) {
        if (out_epoch) *out_epoch = 1;
        *out_rc = OTSARDB_JOURNAL_OK;
        return 1;
    }
    if (put_rc == OTSARDB_STORE_TIMEOUT) {
        otsardb_store_object after = {0};
        otsardb_store_result get_rc = otsardb_store_get(store, key, &after);
        if (get_rc == OTSARDB_STORE_OK) {
            otsardb_store_release_object(store, &after);
            *out_rc = OTSARDB_JOURNAL_AMBIGUOUS;
            return 0;
        }
        if (get_rc == OTSARDB_STORE_NOT_FOUND) {
            *out_rc = OTSARDB_JOURNAL_AMBIGUOUS;
            return 0;
        }
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    if (put_rc == OTSARDB_STORE_UNSUPPORTED) {
        *out_rc = OTSARDB_JOURNAL_UNSUPPORTED;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    return 0;
}

/* ---- retried per-store calls (cycle) ---------------------- */

typedef struct replica_put_ctx {
    otsardb_object_store *store;
    const char *key;
    const void *data;
    size_t size;
} replica_put_ctx;

static int replica_put_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    replica_put_ctx *ctx = (replica_put_ctx *)opaque;
    otsardb_journal_result rc = replicate_object(ctx->store, ctx->key, ctx->data, ctx->size);
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK;
}

static int replica_retried_put(otsardb_object_store *store,
                               const char *key,
                               const void *data,
                               size_t size,
                               otsardb_journal_result *out_rc) {
    replica_put_ctx ctx;
    ctx.store = store;
    ctx.key = key;
    ctx.data = data;
    ctx.size = size;
    return otsardb_wal_publisher_retry_cycle(replica_put_attempt_once, &ctx, out_rc);
}

typedef struct replica_ledger_ctx {
    otsardb_object_store *store;
    const char *key;
    const char *json;
    size_t size;
    const char *commit_id;
    const char *state;
    char **out_etag;
} replica_ledger_ctx;

static int replica_ledger_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    replica_ledger_ctx *ctx = (replica_ledger_ctx *)opaque;
    otsardb_journal_result rc = ledger_write_cas(ctx->store, ctx->key, ctx->json, ctx->size,
                                               ctx->commit_id, ctx->state, ctx->out_etag);
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK;
}

static int replica_retried_ledger(otsardb_object_store *store,
                                  const char *key,
                                  const char *json,
                                  size_t size,
                                  const char *commit_id,
                                  const char *state,
                                  char **out_etag,
                                  otsardb_journal_result *out_rc) {
    replica_ledger_ctx ctx;
    ctx.store = store;
    ctx.key = key;
    ctx.json = json;
    ctx.size = size;
    ctx.commit_id = commit_id;
    ctx.state = state;
    ctx.out_etag = out_etag;
    return otsardb_wal_publisher_retry_cycle(replica_ledger_attempt_once, &ctx, out_rc);
}

typedef struct replica_head_read_ctx {
    otsardb_object_store *store;
    const char *root;
    otsardb_head *out_head;
    char **out_etag;
} replica_head_read_ctx;

static int replica_head_read_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    replica_head_read_ctx *ctx = (replica_head_read_ctx *)opaque;
    otsardb_journal_result rc = otsardb_journal_read_head(ctx->store, ctx->root,
                                                      ctx->out_head, ctx->out_etag);
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK || rc == OTSARDB_JOURNAL_NOT_FOUND;
}

static int replica_retried_head_read(otsardb_object_store *store,
                                     const char *root,
                                     otsardb_head *out_head,
                                     char **out_etag,
                                     otsardb_journal_result *out_rc) {
    replica_head_read_ctx ctx;
    ctx.store = store;
    ctx.root = root;
    ctx.out_head = out_head;
    ctx.out_etag = out_etag;
    return otsardb_wal_publisher_retry_cycle(replica_head_read_attempt_once, &ctx, out_rc);
}

typedef struct replica_head_publish_ctx {
    otsardb_object_store *store;
    const char *root;
    const otsardb_head *head;
    const char *expected_etag;
    int create_if_missing;
    char **out_etag;
} replica_head_publish_ctx;

static int replica_head_publish_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    replica_head_publish_ctx *ctx = (replica_head_publish_ctx *)opaque;
    otsardb_journal_result rc = otsardb_journal_publish_head(ctx->store, ctx->root, ctx->head,
                                                         ctx->expected_etag,
                                                         ctx->create_if_missing,
                                                         ctx->out_etag);
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK;
}

static int replica_retried_head_publish(otsardb_object_store *store,
                                        const char *root,
                                        const otsardb_head *head,
                                        const char *expected_etag,
                                        int create_if_missing,
                                        char **out_etag,
                                        otsardb_journal_result *out_rc) {
    replica_head_publish_ctx ctx;
    ctx.store = store;
    ctx.root = root;
    ctx.head = head;
    ctx.expected_etag = expected_etag;
    ctx.create_if_missing = create_if_missing;
    ctx.out_etag = out_etag;
    return otsardb_wal_publisher_retry_cycle(replica_head_publish_attempt_once, &ctx, out_rc);
}

/* ---- reconciliation / return-to-primary ------------------ */

#define OTSARDB_REPLICA_RECONCILE_MAX_COMMITS 64

typedef struct replica_gap_commit {
    char commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    char manifest_sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    uint64_t generation;
} replica_gap_commit;

/* Copy db.json to the destination when absent (the replay path; the normal
 * phase keeps its own inline block so the 0072 tested paths stay byte-for-
 * byte the same). */
static int replica_copy_metadata_if_absent(otsardb_replica_destination *dest,
                                           otsardb_object_store *primary_store,
                                           const char *root,
                                           otsardb_journal_result *out_rc) {
    char metadata_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(root, "db.json", metadata_key, sizeof(metadata_key))) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    otsardb_store_object existing = {0};
    otsardb_store_result rc = otsardb_store_get(dest->store, metadata_key, &existing);
    if (rc == OTSARDB_STORE_OK) {
        otsardb_store_release_object(dest->store, &existing);
        *out_rc = OTSARDB_JOURNAL_OK;
        return 1;
    }
    if (rc != OTSARDB_STORE_NOT_FOUND) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    otsardb_store_object source = {0};
    otsardb_store_result src_rc = otsardb_store_get(primary_store, metadata_key, &source);
    if (src_rc == OTSARDB_STORE_OK) {
        otsardb_journal_result put_rc = OTSARDB_JOURNAL_ERROR;
        int put_ok = replica_retried_put(dest->store, metadata_key,
                                         source.data, source.size, &put_rc);
        otsardb_store_release_object(primary_store, &source);
        *out_rc = put_rc;
        return put_ok;
    }
    if (src_rc == OTSARDB_STORE_NOT_FOUND) {
        *out_rc = OTSARDB_JOURNAL_OK; /* no metadata anywhere (fresh database) */
        return 1;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    return 0;
}

/* Full object key of one manifest segment ref: v3+ refs carry a SUFFIX
 * relative to the database root; legacy v1/v2 refs carry the absolute key. */
static int segment_full_key(const char *root, const char *ref_key,
                            char *out, size_t out_size) {
    size_t root_len = strlen(root);
    if (strncmp(ref_key, root, root_len) == 0) {
        int n = snprintf(out, out_size, "%s", ref_key);
        return n > 0 && (size_t)n < out_size;
    }
    return root_join(root, ref_key, out, out_size);
}

/* Walk the PRIMARY chain down from the commit's parent (the pre-publish
 * stash â€” the identity of the commit about to be dual-published) until the
 * replica's HEAD generation is reached, collecting the commits between the
 * replica HEAD and the parent (the replay gap, newest first). The replica
 * HEAD commit must be found on the chain at its generation: the replica
 * then lags on OUR chain and the gap is replayable. A walk that ends
 * elsewhere, a broken chain link, or a gap beyond
 * OTSARDB_REPLICA_RECONCILE_MAX_COMMITS fails closed (CONFLICT class) â€” a
 * foreign/ahead destination is never reconciled. */
static int replica_collect_gap(otsardb_object_store *primary_store,
                               const char *root,
                               const char *parent_commit_id,
                               const char *parent_manifest_sha256,
                               uint64_t parent_generation,
                               const otsardb_head *replica_head,
                               replica_gap_commit *gap,
                               size_t gap_cap,
                               size_t *out_count,
                               otsardb_journal_result *out_rc) {
    *out_count = 0;
    *out_rc = OTSARDB_JOURNAL_ERROR;
    if (!parent_commit_id || !parent_commit_id[0] || !replica_head ||
        replica_head->commit_id[0] == '\0' || parent_generation == 0) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    char walk_id[OTSARDB_COMMIT_ID_MAX + 1];
    char walk_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    uint64_t walk_gen = parent_generation;
    snprintf(walk_id, sizeof(walk_id), "%s", parent_commit_id);
    snprintf(walk_sha, sizeof(walk_sha), "%s",
             parent_manifest_sha256 ? parent_manifest_sha256 : "");
    size_t count = 0;
    while (walk_gen > replica_head->generation) {
        if (count >= gap_cap) {
            *out_rc = OTSARDB_JOURNAL_CONFLICT; /* gap beyond the replay bound */
            return 0;
        }
        char key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(key, sizeof(key), "%s/commits/%s.json", root, walk_id);
        if (n <= 0 || (size_t)n >= sizeof(key)) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        otsardb_store_object object = {0};
        otsardb_store_result rc = otsardb_store_get(primary_store, key, &object);
        if (rc != OTSARDB_STORE_OK) {
            *out_rc = rc == OTSARDB_STORE_NOT_FOUND ? OTSARDB_JOURNAL_NOT_FOUND
                                                  : OTSARDB_JOURNAL_ERROR;
            return 0;
        }
        char actual_sha[OTSARDB_SHA256_HEX_MAX + 1];
        if (!otsardb_sha256_hex_data(object.data, object.size, actual_sha) ||
            strcmp(actual_sha, walk_sha) != 0) {
            otsardb_store_release_object(primary_store, &object);
            *out_rc = OTSARDB_JOURNAL_INVALID; /* broken chain link */
            return 0;
        }
        otsardb_commit_manifest manifest = {0};
        int decoded = otsardb_commit_manifest_decode_json((const char *)object.data,
                                                        object.size, &manifest);
        otsardb_store_release_object(primary_store, &object);
        if (!decoded || manifest.generation != walk_gen ||
            manifest.parent_generation + 1u != walk_gen) {
            otsardb_commit_manifest_free(&manifest);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        snprintf(gap[count].commit_id, sizeof(gap[count].commit_id), "%s", walk_id);
        snprintf(gap[count].manifest_sha256, sizeof(gap[count].manifest_sha256),
                 "%s", walk_sha);
        gap[count].generation = walk_gen;
        ++count;
        walk_gen = manifest.parent_generation;
        snprintf(walk_id, sizeof(walk_id), "%s", manifest.parent_commit_id);
        snprintf(walk_sha, sizeof(walk_sha), "%s", manifest.parent_manifest_sha256);
        otsardb_commit_manifest_free(&manifest);
    }
    if (walk_gen != replica_head->generation ||
        strcmp(walk_id, replica_head->commit_id) != 0) {
        /* The replica HEAD is not an ancestor of the primary chain: a
         * foreign/ahead destination â€” fail closed, never reconcile. */
        *out_rc = OTSARDB_JOURNAL_CONFLICT;
        return 0;
    }
    *out_count = count;
    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

/* Replay ONE gap commit onto ONE destination: re-publish db.json (when
 * absent) + every segment + the manifest from the primary store
 * (content-addressed immutable PUTs, SHA-256/size verified against the
 * manifest), write the PREPARED ledger on the PRIMARY and the destination
 * (the primary's ABORTED record is advanced â€” a completing replay may only
 * move the state machine forward), CAS the destination HEAD to the commit,
 * then write the COMMITTED ledger on the PRIMARY and the destination (the
 * flip that completes the commit's global selection). `replica_head_etag`
 * is the destination's current HEAD etag; *out_new_etag receives the HEAD
 * etag after this replay step. */
static int replica_replay_gap_commit(otsardb_replica_destination *dest,
                                     otsardb_object_store *primary_store,
                                     const char *root,
                                     const char *database_id,
                                     const char *model,
                                     const replica_gap_commit *c,
                                     const char *replica_head_etag,
                                     const char *primary_head_etag,
                                     char **out_new_etag,
                                     otsardb_journal_result *out_rc) {
    if (out_new_etag) *out_new_etag = NULL;
    *out_rc = OTSARDB_JOURNAL_ERROR;
    if (!dest || !primary_store || !root || !c || !database_id) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }

    if (!replica_copy_metadata_if_absent(dest, primary_store, root, out_rc)) {
        return 0;
    }

    char manifest_key[OTSARDB_KEY_MAX + 1];
    {
        char suffix[OTSARDB_KEY_MAX + 1];
        int n = snprintf(suffix, sizeof(suffix), "commits/%s.json", c->commit_id);
        if (n <= 0 || (size_t)n >= sizeof(suffix) ||
            !root_join(root, suffix, manifest_key, sizeof(manifest_key))) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
    }
    otsardb_store_object manifest_object = {0};
    otsardb_store_result src_rc = otsardb_store_get(primary_store, manifest_key, &manifest_object);
    if (src_rc != OTSARDB_STORE_OK) {
        *out_rc = src_rc == OTSARDB_STORE_NOT_FOUND ? OTSARDB_JOURNAL_NOT_FOUND
                                                  : OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    char actual_sha[OTSARDB_SHA256_HEX_MAX + 1];
    if (!otsardb_sha256_hex_data(manifest_object.data, manifest_object.size, actual_sha) ||
        strcmp(actual_sha, c->manifest_sha256) != 0) {
        otsardb_store_release_object(primary_store, &manifest_object);
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    otsardb_commit_manifest manifest = {0};
    if (!otsardb_commit_manifest_decode_json((const char *)manifest_object.data,
                                           manifest_object.size, &manifest)) {
        otsardb_store_release_object(primary_store, &manifest_object);
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }

    for (uint32_t i = 0; i < manifest.segment_count; ++i) {
        const otsardb_manifest_segment_ref *ref = &manifest.segments[i];
        char segment_key[OTSARDB_KEY_MAX + 1];
        if (!segment_full_key(root, ref->key, segment_key, sizeof(segment_key))) {
            otsardb_commit_manifest_free(&manifest);
            otsardb_store_release_object(primary_store, &manifest_object);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        otsardb_store_object seg = {0};
        otsardb_store_result seg_rc = otsardb_store_get(primary_store, segment_key, &seg);
        if (seg_rc != OTSARDB_STORE_OK) {
            otsardb_commit_manifest_free(&manifest);
            otsardb_store_release_object(primary_store, &manifest_object);
            *out_rc = seg_rc == OTSARDB_STORE_NOT_FOUND ? OTSARDB_JOURNAL_NOT_FOUND
                                                      : OTSARDB_JOURNAL_ERROR;
            return 0;
        }
        char seg_sha[OTSARDB_SHA256_HEX_MAX + 1];
        int seg_ok = otsardb_sha256_hex_data(seg.data, seg.size, seg_sha) &&
                     strcmp(seg_sha, ref->sha256) == 0 &&
                     seg.size == (size_t)ref->size;
        if (!seg_ok) {
            otsardb_store_release_object(primary_store, &seg);
            otsardb_commit_manifest_free(&manifest);
            otsardb_store_release_object(primary_store, &manifest_object);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
        otsardb_journal_result put_rc = OTSARDB_JOURNAL_ERROR;
        int put_ok = replica_retried_put(dest->store, segment_key,
                                         seg.data, seg.size, &put_rc);
        otsardb_store_release_object(primary_store, &seg);
        if (!put_ok) {
            otsardb_commit_manifest_free(&manifest);
            otsardb_store_release_object(primary_store, &manifest_object);
            *out_rc = put_rc;
            return 0;
        }
    }

    {
        otsardb_journal_result put_rc = OTSARDB_JOURNAL_ERROR;
        int put_ok = replica_retried_put(dest->store, manifest_key,
                                         manifest_object.data, manifest_object.size,
                                         &put_rc);
        otsardb_commit_manifest_free(&manifest);
        otsardb_store_release_object(primary_store, &manifest_object);
        if (!put_ok) {
            *out_rc = put_rc;
            return 0;
        }
    }

    /* PREPARED ledger on both destinations (the intent record). */
    otsardb_replica_ledger ledger;
    memset(&ledger, 0, sizeof(ledger));
    snprintf(ledger.commit_id, sizeof(ledger.commit_id), "%s", c->commit_id);
    ledger.generation = c->generation;
    ledger.parent_generation = c->generation - 1u;
    snprintf(ledger.manifest_sha256, sizeof(ledger.manifest_sha256), "%s",
             c->manifest_sha256);
    snprintf(ledger.state, sizeof(ledger.state), "%s", OTSARDB_REPLICA_STATE_PREPARED);
    snprintf(ledger.primary_etag, sizeof(ledger.primary_etag), "%s",
             primary_head_etag ? primary_head_etag : "");
    snprintf(ledger.replica_etag, sizeof(ledger.replica_etag), "%s",
             replica_head_etag ? replica_head_etag : "");
    snprintf(ledger.model, sizeof(ledger.model), "%s", model);
    snprintf(ledger.destination, sizeof(ledger.destination), "primary");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));

    char *ledger_json = NULL;
    size_t ledger_size = 0;
    char ledger_key[OTSARDB_KEY_MAX + 1];
    {
        char suffix[OTSARDB_KEY_MAX + 1];
        int n = snprintf(suffix, sizeof(suffix), "replica/%s.json", c->commit_id);
        if (n <= 0 || (size_t)n >= sizeof(suffix) ||
            !root_join(root, suffix, ledger_key, sizeof(ledger_key))) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            return 0;
        }
    }
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    otsardb_journal_result ledger_rc = OTSARDB_JOURNAL_ERROR;
    if (!replica_retried_ledger(primary_store, ledger_key, ledger_json, ledger_size,
                                c->commit_id, OTSARDB_REPLICA_STATE_PREPARED, NULL, &ledger_rc)) {
        free(ledger_json);
        *out_rc = ledger_rc;
        return 0;
    }
    snprintf(ledger.destination, sizeof(ledger.destination), "replica");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    if (!replica_retried_ledger(dest->store, ledger_key, ledger_json, ledger_size,
                                c->commit_id, OTSARDB_REPLICA_STATE_PREPARED, NULL, &ledger_rc)) {
        free(ledger_json);
        *out_rc = ledger_rc;
        return 0;
    }
    free(ledger_json);
    ledger_json = NULL;

    /* HEAD CAS on the destination: this destination's HEAD advances to the
     * replayed commit id (the single global selection). */
    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = c->generation;
    snprintf(head.database_id, sizeof(head.database_id), "%s", database_id);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", c->commit_id);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s", manifest_key);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", c->manifest_sha256);
    char *new_etag = NULL;
    otsardb_journal_result head_pub_rc = OTSARDB_JOURNAL_ERROR;
    int head_pub_ok = replica_retried_head_publish(dest->store, root, &head,
                                                   replica_head_etag, 0, &new_etag,
                                                   &head_pub_rc);
    if (!head_pub_ok) {
        *out_rc = head_pub_rc;
        return 0;
    }

    /* COMMITTED ledger on both destinations â€” the replay completes the
     * commit's selection (the primary's ABORTED record flips here). */
    memset(&ledger, 0, sizeof(ledger));
    snprintf(ledger.commit_id, sizeof(ledger.commit_id), "%s", c->commit_id);
    ledger.generation = c->generation;
    ledger.parent_generation = c->generation - 1u;
    snprintf(ledger.manifest_sha256, sizeof(ledger.manifest_sha256), "%s",
             c->manifest_sha256);
    snprintf(ledger.state, sizeof(ledger.state), "%s", OTSARDB_REPLICA_STATE_COMMITTED);
    snprintf(ledger.primary_etag, sizeof(ledger.primary_etag), "%s",
             primary_head_etag ? primary_head_etag : "");
    snprintf(ledger.replica_etag, sizeof(ledger.replica_etag), "%s",
             new_etag ? new_etag : "");
    snprintf(ledger.model, sizeof(ledger.model), "%s", model);
    snprintf(ledger.destination, sizeof(ledger.destination), "primary");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        free(new_etag);
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    if (!replica_retried_ledger(primary_store, ledger_key, ledger_json, ledger_size,
                                c->commit_id, OTSARDB_REPLICA_STATE_COMMITTED, NULL, &ledger_rc)) {
        free(ledger_json);
        free(new_etag);
        *out_rc = ledger_rc;
        return 0;
    }
    snprintf(ledger.destination, sizeof(ledger.destination), "replica");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        free(ledger_json);
        free(new_etag);
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    if (!replica_retried_ledger(dest->store, ledger_key, ledger_json, ledger_size,
                                c->commit_id, OTSARDB_REPLICA_STATE_COMMITTED, NULL, &ledger_rc)) {
        free(ledger_json);
        free(new_etag);
        *out_rc = ledger_rc;
        return 0;
    }
    free(ledger_json);

    if (out_new_etag) *out_new_etag = new_etag;
    else free(new_etag);
    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

/* Replay the whole gap between the destination HEAD and the commit's parent,
 * oldest first (each HEAD CAS advances the destination one commit). On
 * success the destination HEAD equals the commit's parent and the caller
 * re-reads it and proceeds with the normal phase. */
static int replica_reconcile_gap(otsardb_replica_destination *dest,
                                 otsardb_object_store *primary_store,
                                 const char *root,
                                 const char *database_id,
                                 const char *model,
                                 const otsardb_head *replica_head,
                                 const char *replica_head_etag,
                                 const char *primary_head_etag,
                                 const char *parent_commit_id,
                                 const char *parent_manifest_sha256,
                                 uint64_t parent_generation,
                                 otsardb_journal_result *out_rc) {
    *out_rc = OTSARDB_JOURNAL_ERROR;
    replica_gap_commit gap[OTSARDB_REPLICA_RECONCILE_MAX_COMMITS];
    size_t gap_count = 0;
    if (!replica_collect_gap(primary_store, root,
                             parent_commit_id, parent_manifest_sha256, parent_generation,
                             replica_head, gap, OTSARDB_REPLICA_RECONCILE_MAX_COMMITS,
                             &gap_count, out_rc)) {
        return 0;
    }
    if (gap_count == 0) {
        *out_rc = OTSARDB_JOURNAL_OK; /* nothing to replay */
        return 1;
    }
    char *current_etag = dup_string(replica_head_etag ? replica_head_etag : "");
    if (!current_etag) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    for (size_t i = gap_count; i > 0; --i) {
        const replica_gap_commit *c = &gap[i - 1];
        char *new_etag = NULL;
        if (!replica_replay_gap_commit(dest, primary_store, root, database_id,
                                       model, c, current_etag, primary_head_etag,
                                       &new_etag, out_rc)) {
            free(current_etag);
            return 0;
        }
        free(current_etag);
        current_etag = new_etag ? new_etag : dup_string("");
        if (!current_etag) {
            *out_rc = OTSARDB_JOURNAL_ERROR;
            return 0;
        }
    }
    free(current_etag);
    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

/* ---- the replica phase --------------------------------------------------- */

/* Forward declaration (GCC 13 implicit-declaration error at the epoch-fence
 * call site below; the definition lives after otsardb_replica_replicate_commit
 * with the other epoch helpers). */
static int replica_epoch_verify(otsardb_replica_destination *dest,
                                const char *database_root,
                                otsardb_journal_result *out_rc);

/* The Model B per-destination phase (0072) for ONE destination of the
 * handle. Runs exactly the 0072 step sequence (epoch fence, HEAD read +
 * classification + 0077 gap reconciliation, metadata/segment/manifest
 * replication, PREPARED ledger on primary + destination, destination HEAD
 * CAS, COMMITTED ledger on primary + destination). Returns 1 when the
 * destination durably holds the commit; 0 with *out_rc carrying the
 * journal-level cause (the caller applies the quorum rules). */
static int replica_replicate_destination(otsardb_replica *replica,
                                         otsardb_replica_destination *dest,
                                         otsardb_object_store *primary_store,
                                         const char *database_root,
                                         const char *commit_id,
                                         uint64_t generation,
                                         uint64_t parent_generation,
                                         const char *parent_commit_id,
                                         const char *parent_manifest_sha256,
                                         const char *manifest_sha256,
                                         const char *primary_head_etag,
                                         const otsardb_segment_input *segments,
                                         uint32_t segment_count,
                                         const char *model,
                                         otsardb_journal_result *out_rc) {
    if (!replica || !dest || !primary_store || !database_root || !commit_id ||
        !manifest_sha256 || !out_rc) {
        return 0;
    }
    if (segment_count > 0 && !segments) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    otsardb_store_object object = {0};

    /* the promotion fence. The destination's epoch record
     * must still match this session's adopted epoch; a destination that was
     * promoted by another operator refuses every dual publish from the old
     * primary session with CONFLICT class (the publisher maps it to
     * SQLITE_BUSY and rolls back â€” the commit is not acked). */
    {
        otsardb_journal_result epoch_rc = OTSARDB_JOURNAL_ERROR;
        if (!replica_epoch_verify(dest, database_root, &epoch_rc)) {
            *out_rc = epoch_rc;
            return 0;
        }
    }

    /* 1. Read + classify the destination HEAD. The destination may only
     * advance by THIS writer (v0, single-writer-per-destination): a
     * destination HEAD that is neither the commit's parent, the commit
     * itself (a same-session retry), nor absent at genesis is a
     * lagging/ahead/foreign destination â€” reconciled when it lags on OUR
     * chain (promotion fence), fail closed otherwise (INCONCLUSIVE per
     * the commit is durable on the primary but the sync must not ack). */
    otsardb_head replica_head = {0};
    char *replica_head_etag = NULL;
    otsardb_journal_result head_rc = OTSARDB_JOURNAL_ERROR;
    int head_read_ok = replica_retried_head_read(dest->store, database_root,
                                                 &replica_head, &replica_head_etag, &head_rc);
    if (!head_read_ok) {
        *out_rc = head_rc;
        return 0;
    }

    int head_mode = -1; /* 0 = genesis create, 1 = CAS advance from parent,
                           2 = CAS re-apply (replica already at this commit) */
    const char *head_expected_etag = NULL;
    if (head_rc == OTSARDB_JOURNAL_NOT_FOUND) {
        if (parent_generation != 0) {
            *out_rc = OTSARDB_JOURNAL_CONFLICT; /* empty replica behind history */
            free(replica_head_etag);
            return 0;
        }
        head_mode = 0;
    } else {
        int is_self = strcmp(replica_head.commit_id, commit_id) == 0;
        int is_parent = parent_commit_id && parent_commit_id[0] != '\0' &&
                        replica_head.generation == parent_generation &&
                        strcmp(replica_head.commit_id, parent_commit_id) == 0;
        if (is_self) {
            head_mode = 2;
        } else if (is_parent) {
            head_mode = 1;
        } else {
            /* reconciliation / return-to-primary. A failed
             * dual publish (0072 failure contract) left the destination
             * behind the primary chain (e.g. after a session restart the
             * pending commit is never re-run). Replay the gap â€” re-publish
             * each gap commit's objects and flip the ledgers COMMITTED â€”
             * then proceed with this commit. A destination HEAD that is NOT
             * an ancestor of the primary chain (foreign/ahead) still fails
             * closed (CONFLICT class). */
            if (!replica_reconcile_gap(dest, primary_store, database_root,
                                       replica->database_id, model,
                                       &replica_head, replica_head_etag,
                                       primary_head_etag,
                                       parent_commit_id, parent_manifest_sha256,
                                       parent_generation, out_rc)) {
                free(replica_head_etag);
                return 0;
            }
            free(replica_head_etag);
            replica_head_etag = NULL;
            memset(&replica_head, 0, sizeof(replica_head));
            otsardb_journal_result re_rc = OTSARDB_JOURNAL_ERROR;
            if (!replica_retried_head_read(dest->store, database_root,
                                           &replica_head, &replica_head_etag, &re_rc)) {
                *out_rc = re_rc;
                return 0;
            }
            if (re_rc != OTSARDB_JOURNAL_OK) {
                *out_rc = re_rc;
                free(replica_head_etag);
                return 0;
            }
            /* After the replay the destination must sit exactly on the
             * commit's parent (or on the commit itself in a same-session
             * retry). */
            is_self = strcmp(replica_head.commit_id, commit_id) == 0;
            is_parent = parent_commit_id && parent_commit_id[0] != '\0' &&
                        replica_head.generation == parent_generation &&
                        strcmp(replica_head.commit_id, parent_commit_id) == 0;
            if (!is_self && !is_parent) {
                *out_rc = OTSARDB_JOURNAL_CONFLICT;
                free(replica_head_etag);
                return 0;
            }
            head_mode = is_self ? 2 : 1;
        }
        if (head_mode != 0) head_expected_etag = replica_head_etag;
    }

    /* 2. Replicate metadata.json (db.json) only when absent on the replica:
     * it is a per-database fixed object guarded by conditional create on the
     * primary; a stale foreign copy is caught by the chain/HEAD guards. */
    char metadata_key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, "db.json", metadata_key, sizeof(metadata_key))) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        free(replica_head_etag);
        return 0;
    }
    {
        otsardb_store_object existing = {0};
        otsardb_store_result rc = otsardb_store_get(dest->store, metadata_key, &existing);
        if (rc == OTSARDB_STORE_OK) {
            otsardb_store_release_object(dest->store, &existing);
        } else if (rc == OTSARDB_STORE_NOT_FOUND) {
            otsardb_store_object source = {0};
            otsardb_store_result src_rc = otsardb_store_get(primary_store, metadata_key, &source);
            if (src_rc == OTSARDB_STORE_OK) {
                otsardb_journal_result put_rc = OTSARDB_JOURNAL_ERROR;
                int put_ok = replica_retried_put(dest->store, metadata_key,
                                                 source.data, source.size, &put_rc);
                otsardb_store_release_object(primary_store, &source);
                if (!put_ok) {
                    *out_rc = put_rc;
                    free(replica_head_etag);
                    return 0;
                }
            } else if (src_rc != OTSARDB_STORE_NOT_FOUND) {
                *out_rc = OTSARDB_JOURNAL_ERROR;
                free(replica_head_etag);
                return 0;
            }
        } else {
            *out_rc = OTSARDB_JOURNAL_ERROR;
            free(replica_head_etag);
            return 0;
        }
    }

    /* 3. Replicate every segment: encoded exactly like commit_publish.c
     * (same encoder, same commit id, frame table on) so the object bytes
     * are byte-identical to the primary's â€” content-addressed keys make the
     * GET+compare idempotent. */
    for (uint32_t i = 0; i < segment_count; ++i) {
        if (!segments[i].payload && segments[i].payload_size != 0) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            free(replica_head_etag);
            return 0;
        }
        unsigned char *segment_data = NULL;
        size_t segment_size = 0;
        char segment_sha256[OTSARDB_SEGMENT_SHA256_HEX_SIZE + 1];
        if (!otsardb_segment_encode_with_frames(OTSARDB_SEGMENT_WAL_FRAMES,
                                              commit_id,
                                              segments[i].payload,
                                              segments[i].payload_size,
                                              &segment_data,
                                              &segment_size,
                                              segment_sha256,
                                              1,
                                              NULL)) {
            *out_rc = OTSARDB_JOURNAL_ERROR;
            free(replica_head_etag);
            return 0;
        }

        char segment_suffix[OTSARDB_KEY_MAX + 1];
        int n = snprintf(segment_suffix, sizeof(segment_suffix),
                         "segments/sha256/%.2s/%s.otsseg",
                         segment_sha256, segment_sha256);
        if (n <= 0 || (size_t)n >= sizeof(segment_suffix)) {
            free(segment_data);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            free(replica_head_etag);
            return 0;
        }
        char segment_key[OTSARDB_KEY_MAX + 1];
        if (!root_join(database_root, segment_suffix, segment_key, sizeof(segment_key))) {
            free(segment_data);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            free(replica_head_etag);
            return 0;
        }

        otsardb_journal_result put_rc = OTSARDB_JOURNAL_ERROR;
        int put_ok = replica_retried_put(dest->store, segment_key,
                                         segment_data, segment_size, &put_rc);
        free(segment_data);
        if (!put_ok) {
            *out_rc = put_rc;
            free(replica_head_etag);
            return 0;
        }
    }

    /* 4. Replicate the manifest: GET the EXACT published bytes from the
     * primary (the in-memory borrow-only result cannot re-encode the
     * frame_offsets table) and verify their SHA-256 against the head. */
    char manifest_key[OTSARDB_KEY_MAX + 1];
    {
        char suffix[OTSARDB_KEY_MAX + 1];
        int n = snprintf(suffix, sizeof(suffix), "commits/%s.json", commit_id);
        if (n <= 0 || (size_t)n >= sizeof(suffix) ||
            !root_join(database_root, suffix, manifest_key, sizeof(manifest_key))) {
            *out_rc = OTSARDB_JOURNAL_INVALID;
            free(replica_head_etag);
            return 0;
        }
    }
    {
        otsardb_store_result src_rc = otsardb_store_get(primary_store, manifest_key, &object);
        if (src_rc != OTSARDB_STORE_OK) {
            *out_rc = src_rc == OTSARDB_STORE_NOT_FOUND ? OTSARDB_JOURNAL_NOT_FOUND
                                                      : OTSARDB_JOURNAL_ERROR;
            free(replica_head_etag);
            return 0;
        }
        char actual_sha[OTSARDB_SHA256_HEX_MAX + 1];
        if (!otsardb_sha256_hex_data(object.data, object.size, actual_sha) ||
            strcmp(actual_sha, manifest_sha256) != 0) {
            otsardb_store_release_object(primary_store, &object);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            free(replica_head_etag);
            return 0;
        }
        otsardb_journal_result put_rc = OTSARDB_JOURNAL_ERROR;
        int put_ok = replica_retried_put(dest->store, manifest_key,
                                         object.data, object.size, &put_rc);
        otsardb_store_release_object(primary_store, &object);
        if (!put_ok) {
            *out_rc = put_rc;
            free(replica_head_etag);
            return 0;
        }
    }

    /* 5. PREPARED ledger entry on BOTH destinations (the design's intent
     * record; the decision point is reached when both hold it). */
    otsardb_replica_ledger ledger;
    memset(&ledger, 0, sizeof(ledger));
    snprintf(ledger.commit_id, sizeof(ledger.commit_id), "%s", commit_id);
    ledger.generation = generation;
    ledger.parent_generation = parent_generation;
    snprintf(ledger.manifest_sha256, sizeof(ledger.manifest_sha256), "%s", manifest_sha256);
    snprintf(ledger.state, sizeof(ledger.state), "%s", OTSARDB_REPLICA_STATE_PREPARED);
    snprintf(ledger.primary_etag, sizeof(ledger.primary_etag), "%s",
             primary_head_etag ? primary_head_etag : "");
    snprintf(ledger.replica_etag, sizeof(ledger.replica_etag), "%s",
             replica_head_etag ? replica_head_etag : "");
    snprintf(ledger.model, sizeof(ledger.model), "%s", model);
    snprintf(ledger.destination, sizeof(ledger.destination), "primary");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));

    char *ledger_json = NULL;
    size_t ledger_size = 0;
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        free(replica_head_etag);
        return 0;
    }

    char ledger_key[OTSARDB_KEY_MAX + 1];
    {
        char suffix[OTSARDB_KEY_MAX + 1];
        int n = snprintf(suffix, sizeof(suffix), "replica/%s.json", commit_id);
        if (n <= 0 || (size_t)n >= sizeof(suffix) ||
            !root_join(database_root, suffix, ledger_key, sizeof(ledger_key))) {
            free(ledger_json);
            *out_rc = OTSARDB_JOURNAL_INVALID;
            free(replica_head_etag);
            return 0;
        }
    }

    otsardb_journal_result ledger_rc = OTSARDB_JOURNAL_ERROR;
    if (!replica_retried_ledger(primary_store, ledger_key, ledger_json, ledger_size,
                                commit_id, OTSARDB_REPLICA_STATE_PREPARED, NULL, &ledger_rc)) {
        free(ledger_json);
        *out_rc = ledger_rc;
        free(replica_head_etag);
        return 0;
    }
    snprintf(ledger.destination, sizeof(ledger.destination), "replica");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        free(replica_head_etag);
        return 0;
    }
    if (!replica_retried_ledger(dest->store, ledger_key, ledger_json, ledger_size,
                                commit_id, OTSARDB_REPLICA_STATE_PREPARED, NULL, &ledger_rc)) {
        free(ledger_json);
        *out_rc = ledger_rc;
        free(replica_head_etag);
        return 0;
    }
    free(ledger_json);
    ledger_json = NULL;

    /* 6. HEAD CAS on the replica: every destination HEAD = the same commit
     * id (the single global selection of the 0069 design). If-Match the
     * replica's own current etag; If-None-Match at genesis. */
    otsardb_head head;
    memset(&head, 0, sizeof(head));
    head.format_version = 1;
    head.generation = generation;
    snprintf(head.database_id, sizeof(head.database_id), "%s", replica->database_id);
    snprintf(head.commit_id, sizeof(head.commit_id), "%s", commit_id);
    snprintf(head.commit_key, sizeof(head.commit_key), "%s", manifest_key);
    snprintf(head.commit_sha256, sizeof(head.commit_sha256), "%s", manifest_sha256);

    char *replica_head_new_etag = NULL;
    otsardb_journal_result head_pub_rc = OTSARDB_JOURNAL_ERROR;
    int head_pub_ok = replica_retried_head_publish(dest->store, database_root, &head,
                                                   head_expected_etag,
                                                   head_mode == 0 ? 1 : 0,
                                                   &replica_head_new_etag,
                                                   &head_pub_rc);
    free(replica_head_etag);
    replica_head_etag = NULL;
    if (!head_pub_ok) {
        *out_rc = head_pub_rc;
        return 0;
    }

    /* 7. COMMITTED ledger entry on BOTH destinations â€” the ack barrier. */
    memset(&ledger, 0, sizeof(ledger));
    snprintf(ledger.commit_id, sizeof(ledger.commit_id), "%s", commit_id);
    ledger.generation = generation;
    ledger.parent_generation = parent_generation;
    snprintf(ledger.manifest_sha256, sizeof(ledger.manifest_sha256), "%s", manifest_sha256);
    snprintf(ledger.state, sizeof(ledger.state), "%s", OTSARDB_REPLICA_STATE_COMMITTED);
    snprintf(ledger.primary_etag, sizeof(ledger.primary_etag), "%s",
             primary_head_etag ? primary_head_etag : "");
    snprintf(ledger.replica_etag, sizeof(ledger.replica_etag), "%s",
             replica_head_new_etag ? replica_head_new_etag : "");
    snprintf(ledger.model, sizeof(ledger.model), "%s", model);
    snprintf(ledger.destination, sizeof(ledger.destination), "primary");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));

    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        free(replica_head_new_etag);
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    if (!replica_retried_ledger(primary_store, ledger_key, ledger_json, ledger_size,
                                commit_id, OTSARDB_REPLICA_STATE_COMMITTED, NULL, &ledger_rc)) {
        free(ledger_json);
        free(replica_head_new_etag);
        *out_rc = ledger_rc;
        return 0;
    }
    snprintf(ledger.destination, sizeof(ledger.destination), "replica");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));
    if (!ledger_encode(&ledger, &ledger_json, &ledger_size)) {
        free(ledger_json);
        free(replica_head_new_etag);
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    if (!replica_retried_ledger(dest->store, ledger_key, ledger_json, ledger_size,
                                commit_id, OTSARDB_REPLICA_STATE_COMMITTED, NULL, &ledger_rc)) {
        free(ledger_json);
        free(replica_head_new_etag);
        *out_rc = ledger_rc;
        return 0;
    }
    free(ledger_json);
    free(replica_head_new_etag);

    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

/* ---- quorum decision (Model C, N-of-M) ------------------ */

/* The primary's OWN COMMITTED ledger must exist whenever the sync acks
 * (a commit is GLOBALLY SELECTED only when its ledger reads COMMITTED on
 * the required destinations; the primary always counts as one of them).
 * The normal phase writes it inside every successful destination's COMMITTED
 * step; the Q=1 quorum (primary-only durability) with EVERY destination
 * lagging reaches the quorum without any destination having written it, so
 * it is written here directly (absent -> COMMITTED is a legal transition of
 * the ledger state machine). Idempotent by design: on the normal paths the
 * record already exists COMMITTED and this call is a no-op (it is only
 * invoked when zero destinations succeeded). */
static int replica_ensure_primary_committed(otsardb_object_store *primary_store,
                                            const char *root,
                                            const char *commit_id,
                                            uint64_t generation,
                                            uint64_t parent_generation,
                                            const char *manifest_sha256,
                                            const char *primary_head_etag,
                                            otsardb_journal_result *out_rc) {
    otsardb_replica_ledger ledger;
    memset(&ledger, 0, sizeof(ledger));
    snprintf(ledger.commit_id, sizeof(ledger.commit_id), "%s", commit_id);
    ledger.generation = generation;
    ledger.parent_generation = parent_generation;
    snprintf(ledger.manifest_sha256, sizeof(ledger.manifest_sha256), "%s", manifest_sha256);
    snprintf(ledger.state, sizeof(ledger.state), "%s", OTSARDB_REPLICA_STATE_COMMITTED);
    snprintf(ledger.primary_etag, sizeof(ledger.primary_etag), "%s",
             primary_head_etag ? primary_head_etag : "");
    snprintf(ledger.model, sizeof(ledger.model), "C");
    snprintf(ledger.destination, sizeof(ledger.destination), "primary");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));

    char *json = NULL;
    size_t size = 0;
    if (!ledger_encode(&ledger, &json, &size)) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    char suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(suffix, sizeof(suffix), "replica/%s.json", commit_id);
    if (n <= 0 || (size_t)n >= sizeof(suffix)) {
        free(json);
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(root, suffix, key, sizeof(key))) {
        free(json);
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    int ok = replica_retried_ledger(primary_store, key, json, size,
                                    commit_id, OTSARDB_REPLICA_STATE_COMMITTED,
                                    NULL, out_rc);
    free(json);
    return ok;
}

/* â€” Model C quorum N-of-M. Runs the per-destination phase
 * (0072/0077, see replica_replicate_destination) over the handle's
 * destination list and decides the ACK at the quorum:
 *
 * - the PRIMARY always counts as 1 (its publish preceded the phase);
 * - Q = otsardb_replica_quorum(replica) of the M = 1 + R destinations must
 * hold every object + the COMMITTED ledger;
 * - a destination that fails with a TRANSPORT-class result (ERROR /
 * AMBIGUOUS after the 0065 budget) is absorbed while the quorum is
 * still reachable: it stays LAGGING (its HEAD behind) and the 0077
 * gap-replay catches it up on the next dual publish (best-effort);
 * - an AUTHORITATIVE result (CONFLICT / NOT_FOUND / INVALID /
 * UNSUPPORTED) on ANY destination fails the whole sync: a
 * foreign/fenced/broken destination can never be safely caught up and
 * must never be silently dropped from the durability set;
 * - the quorum met -> 1 (the sync acks; with lagging destinations a
 * stderr line reports the catch-up contract); the quorum missed -> 0
 * with the FIRST transport result in *out_rc (the publisher then marks
 * the primary ABORTED and rolls back â€” the exact 0072 failure contract).
 *
 * With R = 1 and no OTSARDB_S3_QUORUM env the effective quorum is 2-of-2:
 * the byte-identical 0072 Model B behavior (identical store call sequence
 * and put counts â€” the quorum bookkeeping adds no store calls on the
 * success path). */
int otsardb_replica_replicate_commit(otsardb_replica *replica,
                                   otsardb_object_store *primary_store,
                                   const char *database_root,
                                   const char *commit_id,
                                   uint64_t generation,
                                   uint64_t parent_generation,
                                   const char *parent_commit_id,
                                   const char *parent_manifest_sha256,
                                   const char *manifest_sha256,
                                   const char *primary_head_etag,
                                   const otsardb_segment_input *segments,
                                   uint32_t segment_count,
                                   otsardb_journal_result *out_rc) {
    if (!replica || !primary_store || !database_root || !commit_id ||
        !manifest_sha256 || !out_rc) {
        return 0;
    }
    if (segment_count > 0 && !segments) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    if (replica->destination_count == 0) {
        /* A handle with no destinations cannot reach any quorum beyond the
         * primary-only degenerate case; the caller failed to configure. */
        return 0;
    }

    const int quorum = otsardb_replica_quorum(replica);
    const char *model = replica->destination_count > 1u ? "C" : "B";
    int confirmed = 1; /* the primary always holds the commit here */
    int succeeded = 0;
    int have_transport_failure = 0;
    otsardb_journal_result first_transport = OTSARDB_JOURNAL_ERROR;

    for (size_t i = 0; i < replica->destination_count; ++i) {
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        if (replica_replicate_destination(replica, &replica->destinations[i],
                                          primary_store, database_root,
                                          commit_id, generation, parent_generation,
                                          parent_commit_id, parent_manifest_sha256,
                                          manifest_sha256, primary_head_etag,
                                          segments, segment_count, model, &rc)) {
            ++confirmed;
            ++succeeded;
        } else if (!otsardb_wal_publisher_rc_retryable(rc)) {
            /* Authoritative failure: never absorbed by the quorum. */
            *out_rc = rc;
            return 0;
        } else {
            if (!have_transport_failure) {
                first_transport = rc;
                have_transport_failure = 1;
            }
        }
    }

    if (confirmed >= quorum) {
        if (succeeded == 0 &&
            !replica_ensure_primary_committed(primary_store, database_root,
                                              commit_id, generation,
                                              parent_generation,
                                              manifest_sha256,
                                              primary_head_etag, out_rc)) {
            /* The ack barrier requires the primary's COMMITTED ledger; its
             * write failed -> no ack (the publisher rolls back and retries). */
            return 0;
        }
        if (succeeded < (int)replica->destination_count) {
            fprintf(stderr,
                    "otsardb: quorum ack: %d of %d destinations COMMITTED (quorum %d; "
                    "primary + %d replicas); %zu lagging destination(s) marked for "
                    "background catch-up (best-effort 0077 gap-replay on the next "
                    "dual publish)\n",
                    confirmed, (int)(replica->destination_count + 1u), quorum,
                    succeeded, replica->destination_count - (size_t)succeeded);
        }
        *out_rc = OTSARDB_JOURNAL_OK;
        return 1;
    }

    *out_rc = have_transport_failure ? first_transport: OTSARDB_JOURNAL_ERROR;
    return 0;
}

void otsardb_replica_mark_aborted(otsardb_object_store *primary_store,
                                const char *database_root,
                                const char *commit_id,
                                uint64_t generation,
                                uint64_t parent_generation,
                                const char *manifest_sha256,
                                const char *primary_head_etag) {
    if (!primary_store || !database_root || !commit_id || !manifest_sha256) return;

    otsardb_replica_ledger ledger;
    memset(&ledger, 0, sizeof(ledger));
    snprintf(ledger.commit_id, sizeof(ledger.commit_id), "%s", commit_id);
    ledger.generation = generation;
    ledger.parent_generation = parent_generation;
    snprintf(ledger.manifest_sha256, sizeof(ledger.manifest_sha256), "%s", manifest_sha256);
    snprintf(ledger.state, sizeof(ledger.state), "%s", OTSARDB_REPLICA_STATE_ABORTED);
    snprintf(ledger.primary_etag, sizeof(ledger.primary_etag), "%s",
             primary_head_etag ? primary_head_etag : "");
    snprintf(ledger.model, sizeof(ledger.model), "B");
    snprintf(ledger.destination, sizeof(ledger.destination), "primary");
    utc_now(ledger.written_at_utc, sizeof(ledger.written_at_utc));

    char *json = NULL;
    size_t size = 0;
    if (!ledger_encode(&ledger, &json, &size)) return;

    char suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(suffix, sizeof(suffix), "replica/%s.json", commit_id);
    if (n <= 0 || (size_t)n >= sizeof(suffix)) {
        free(json);
        return;
    }
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(database_root, suffix, key, sizeof(key))) {
        free(json);
        return;
    }

    /* Best-effort by contract: any failure is swallowed (the sync already
     * fails; the marker is a reconciliation aid for the next phase). */
    (void)ledger_write_cas(primary_store, key, json, size, commit_id,
                           OTSARDB_REPLICA_STATE_ABORTED, NULL);
    free(json);
}

/* ---- open / attach / close ---------------------------------------------- */

/* 0077: verify the destination's epoch record still matches this session's
 * adopted epoch before the replica phase runs â€” the promotion fence. A
 * destination whose record advanced (another operator promoted it) refuses
 * every dual publish from this session with CONFLICT class (SQLITE_BUSY):
 * the old primary session can never advance a promoted replica. A missing
 * record fails closed too (the invariant is "one epoch record per
 * destination, created at attach/open"; absence means deletion or a foreign
 * destination). */
static int replica_epoch_verify(otsardb_replica_destination *dest,
                                const char *database_root,
                                otsardb_journal_result *out_rc) {
    otsardb_replica_epoch_view view;
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    if (!otsardb_replica_epoch_read(dest->store, database_root, &view, &rc)) {
        *out_rc = rc == OTSARDB_JOURNAL_NOT_FOUND ? OTSARDB_JOURNAL_CONFLICT: rc;
        return 0;
    }
    if (!view.exists) {
        *out_rc = OTSARDB_JOURNAL_CONFLICT;
        return 0;
    }
    if (!dest->epoch_known || view.epoch != dest->epoch) {
        /* The destination epoch was raised by a promotion this session did
         * not perform â€” this writer is fenced. */
        *out_rc = OTSARDB_JOURNAL_CONFLICT;
        return 0;
    }
    if (view.etag[0]) {
        snprintf(dest->epoch_etag, sizeof(dest->epoch_etag), "%s", view.etag);
    }
    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

/* The session epoch the handle holds on its (promoted) destination: the
 * value the 0077 fence of that destination must match. With a destination
 * list (0091) the value of the PROMOTED destination is reported (or of the
 * first destination when nothing was promoted yet). */
uint64_t otsardb_replica_epoch(const otsardb_replica *replica) {
    if (!replica || replica->destination_count == 0) return 0;
    size_t index = 0;
    if (replica->promoted_index >= 0 &&
        (size_t)replica->promoted_index < replica->destination_count) {
        index = (size_t)replica->promoted_index;
    }
    return replica->destinations[index].epoch;
}

/* Append one destination to the handle's list. */
static int replica_destination_append(otsardb_replica *replica,
                                      otsardb_object_store *store,
                                      otsardb_write_strategy strategy,
                                      int borrowed) {
    if (replica->destination_count == replica->destination_capacity) {
        size_t next = replica->destination_capacity ? replica->destination_capacity * 2u
                                                    : 1u;
        otsardb_replica_destination *more =
            (otsardb_replica_destination *)realloc(replica->destinations,
                                                 next * sizeof(*more));
        if (!more) return 0;
        replica->destinations = more;
        replica->destination_capacity = next;
    }
    otsardb_replica_destination *dest = &replica->destinations[replica->destination_count];
    memset(dest, 0, sizeof(*dest));
    dest->store = store;
    dest->strategy = strategy;
    dest->store_borrowed = borrowed != 0;
    ++replica->destination_count;
    return 1;
}

/* Pop the last destination (attach failure cleanup). */
static void replica_destination_pop(otsardb_replica *replica) {
    if (replica->destination_count == 0) return;
    --replica->destination_count;
    replica->destinations[replica->destination_count].store = NULL;
}

static int replica_bootstrap_epoch(otsardb_replica *replica,
                                   otsardb_object_store *primary_store,
                                   uint64_t generation) {
    /* The epoch record is written once per destination on the PRIMARY and on
     * EVERY destination (design section 3/4: <root>/replica/epoch.json per
     * destination). Create-or-adopt: absent -> If-None-Match create at
     * epoch 1 (a 412 from a concurrent session re-reads and adopts); present
     * -> the record is ADOPTED verbatim (its epoch becomes this session's
     * fence, 0077). */
    if (!replica->primary_epoch_created) {
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        if (!epoch_create_or_adopt(primary_store, replica->database_root,
                                   replica->database_id, generation,
                                   NULL, NULL, 0, &rc)) {
            return 0;
        }
        replica->primary_epoch_created = 1;
    }
    for (size_t i = 0; i < replica->destination_count; ++i) {
        otsardb_replica_destination *dest = &replica->destinations[i];
        if (dest->epoch_created) continue;
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        if (!epoch_create_or_adopt(dest->store, replica->database_root,
                                   replica->database_id, generation,
                                   &dest->epoch, dest->epoch_etag,
                                   sizeof(dest->epoch_etag), &rc)) {
            return 0;
        }
        dest->epoch_created = 1;
        dest->epoch_known = 1;
    }
    return 1;
}

int otsardb_replica_attach(struct otsardb_wal_publisher *publisher,
                         otsardb_object_store *store,
                         otsardb_write_strategy strategy,
                         int borrowed) {
    if (!publisher || !store) return 0;

    otsardb_replica *replica = publisher->replica;
    if (!replica) {
        replica = (otsardb_replica *)calloc(1, sizeof(*replica));
        if (!replica) return 0;
        replica->promoted_index = -1;
        snprintf(replica->database_root, sizeof(replica->database_root), "%s",
                 publisher->database_root);
        snprintf(replica->database_id, sizeof(replica->database_id), "%s",
                 publisher->database_id);
        publisher->replica = replica;
    }

    if (!replica_destination_append(replica, store, strategy, borrowed != 0)) {
        if (replica->destination_count == 0) {
            free(replica);
            publisher->replica = NULL;
        }
        return 0;
    }
    if (!replica_bootstrap_epoch(replica, publisher->store,
                                 publisher->current_generation)) {
        replica_destination_pop(replica);
        if (replica->destination_count == 0) {
            free(replica);
            publisher->replica = NULL;
        }
        return 0;
    }
    return 1;
}

#if !defined(OTSARDB_ENABLE_S3)
otsardb_replica *otsardb_replica_open(otsardb_object_store *primary_store,
                                  const char *database_root,
                                  const char *database_id,
                                  char *err,
                                  size_t err_size) {
    (void)primary_store;
    (void)database_root;
    (void)database_id;
    if (err && err_size > 0) {
        snprintf(err, err_size,
                 "this build lacks the S3 adapter; the replica destination is "
                 "unavailable (fail-closed: no silent single-destination fallback)");
    }
    return NULL;
}

otsardb_object_store *otsardb_replica_open_reader(const char *database_root,
                                              const char *database_id,
                                              otsardb_store_probe_report *out_report,
                                              char *err,
                                              size_t err_size) {
    (void)database_root;
    (void)database_id;
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (err && err_size > 0) {
        snprintf(err, err_size,
                 "this build lacks the S3 adapter; the read-replica destination is "
                 "unavailable (fail-closed)");
    }
    return NULL;
}

otsardb_object_store *otsardb_replica_open_reader_at(int destination_index,
                                                 const char *database_root,
                                                 const char *database_id,
                                                 otsardb_store_probe_report *out_report,
                                                 char *err,
                                                 size_t err_size) {
    (void)destination_index;
    (void)database_root;
    (void)database_id;
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (err && err_size > 0) {
        snprintf(err, err_size,
                 "this build lacks the S3 adapter; the replica destination is "
                 "unavailable (fail-closed)");
    }
    return NULL;
}

int otsardb_replica_attach_env_others(struct otsardb_wal_publisher *publisher,
                                    int exclude_index,
                                    char *err,
                                    size_t err_size) {
    (void)publisher;
    (void)exclude_index;
    if (err && err_size > 0) {
        snprintf(err, err_size,
                 "this build lacks the S3 adapter; additional replica destinations "
                 "are unavailable (fail-closed)");
    }
    return 0;
}
#else
/* Shared env-driven replica store construction (
 * 0091 extends it to the REPLICA2_* / REPLICA3_* variable sets): parse the
 * environment of `var_set` (0 = OTSARDB_S3_REPLICA_*, 1 = OTSARDB_S3_REPLICA2_*,
 * 2 = OTSARDB_S3_REPLICA3_*), create the store instance through the S3 adapter
 * and run the capability probe (0035 retry policy; the 0043 verdict cache is
 * NOT consulted â€” the replica always probes so its verdict is authoritative
 * for this session). Fail-closed: any probe/store failure returns NULL with
 * a human message in `err`. On success the returned store is owned by the
 * caller; *out_report and *out_strategy receive the probe verdict and the
 * derived write strategy (CAS when the destination supports it, else the
 * design section 4 asymmetry rule: single-writer-per-destination). */
static otsardb_object_store *replica_store_create_from_env(int var_set,
                                                         const char *database_id,
                                                         otsardb_store_probe_report *out_report,
                                                         otsardb_write_strategy *out_strategy,
                                                         char *err,
                                                         size_t err_size) {
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (out_strategy) *out_strategy = OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;

    char name_buf[64];
    const char *endpoint = replica_env_value(var_set, "ENDPOINT", name_buf, sizeof(name_buf));
    const char *bucket = replica_env_value(var_set, "BUCKET", name_buf, sizeof(name_buf));
    if (!endpoint || !bucket) {
        if (err) {
            snprintf(err, err_size,
                     "replica environment incomplete: %s_ENDPOINT and %s_BUCKET are "
                     "required (fail-closed; no silent single-destination fallback)",
                     replica_env_prefixes[var_set], replica_env_prefixes[var_set]);
        }
        return NULL;
    }

    const char *region = replica_env_value(var_set, "REGION", name_buf, sizeof(name_buf));
    if (!region) region = "us-east-1";
    const char *access_key = replica_env_value(var_set, "ACCESS_KEY", name_buf, sizeof(name_buf));
    const char *secret_key = replica_env_value(var_set, "SECRET_KEY", name_buf, sizeof(name_buf));
    const char *prefix = env_nonempty("OTSARDB_S3_PREFIX");

    otsardb_s3_config config;
    memset(&config, 0, sizeof(config));
    config.endpoint = endpoint;
    config.region = region;
    config.bucket = bucket;
    config.access_key = access_key;
    config.secret_key = secret_key;
    config.prefix = prefix;
    config.use_https = 1;

    otsardb_object_store *store = otsardb_s3_store_create(&config);
    if (!store) {
        if (err) {
            snprintf(err, err_size,
                     "failed to initialize the replica S3 client (verify %s_* "
                     "endpoint, region, bucket and credentials); this build may lack the "
                     "S3 adapter",
                     replica_env_prefixes[var_set]);
        }
        return NULL;
    }

    /* Capability probe (retry policy; fail-closed). The
     * verdict cache lives in s3_database.cpp and is not
     * consulted: the replica always probes so its verdict is authoritative
     * for this session. */
    char probe_key[OTSARDB_KEY_MAX + 1];
    {
#if defined(_WIN32)
        int pid = _getpid();
#else
        int pid = (int)getpid();
#endif
        int n = snprintf(probe_key, sizeof(probe_key), "__otsardb_probe/%s/replica%d/p%d",
                         database_id, var_set, pid);
        if (n <= 0 || (size_t)n >= sizeof(probe_key)) {
            otsardb_s3_store_destroy(store);
            if (err) snprintf(err, err_size, "replica probe key too long");
            return NULL;
        }
    }
    otsardb_store_probe_report report;
    memset(&report, 0, sizeof(report));
    otsardb_store_result probe_rc = otsardb_store_probe_capabilities(store, probe_key, &report);
    if (probe_rc != OTSARDB_STORE_OK || !report.s3_core_ok) {
        otsardb_s3_store_destroy(store);
        if (err) {
            snprintf(err, err_size,
                     "replica endpoint failed the OtsarDB S3 Core capability probe "
                     "(fail-closed: dual publish cannot start)");
        }
        return NULL;
    }
    if (out_report) *out_report = report;
    if (out_strategy) {
        if (report.s3_cas_ok) {
            *out_strategy = OTSARDB_WRITE_STRATEGY_CAS;
        } else {
            /* Design section 4 asymmetry rule: non-CAS destinations are
             * written only by the current primary (single-writer per
             * destination); the epoch/ledger/HEAD conditionals are resolved
             * by GET+compare and the fence lives on the CAS-capable primary
             * side. */
            *out_strategy = OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;
        }
    }
    return store;
}

otsardb_object_store *otsardb_replica_open_reader(const char *database_root,
                                              const char *database_id,
                                              otsardb_store_probe_report *out_report,
                                              char *err,
                                              size_t err_size) {
    /* the READ path. Destination 0 is the default reader
     * destination (OTSARDB_S3_REPLICA_*), exactly as 0084 defined it. */
    return otsardb_replica_open_reader_at(0, database_root, database_id,
                                        out_report, err, err_size);
}

otsardb_object_store *otsardb_replica_open_reader_at(int destination_index,
                                                 const char *database_root,
                                                 const char *database_id,
                                                 otsardb_store_probe_report *out_report,
                                                 char *err,
                                                 size_t err_size) {
    (void)database_root;
    if (err && err_size > 0) err[0] = '\0';
    if (destination_index < 0 || destination_index >= OTSARDB_REPLICA_MAX_DESTINATIONS) {
        if (err) {
            snprintf(err, err_size,
                     "replica destination index %d is out of range (0..%d)",
                     destination_index, OTSARDB_REPLICA_MAX_DESTINATIONS - 1);
        }
        return NULL;
    }
    if (!replica_var_set_configured(destination_index)) {
        if (err) {
            snprintf(err, err_size,
                     "%s_* is not configured; replica destination %d is unavailable "
                     "(fail-closed)",
                     replica_env_prefixes[destination_index], destination_index);
        }
        return NULL;
    }
    /* the READ path. The replica destination is opened as a
     * plain store handle â€” no otsardb_replica struct, no epoch bootstrap, no
     * ledgers, no HEAD, no lease: a reader performs ZERO writes to the
     * database root (the probe verdict object is the only write, outside
     * the root, identical to any other open). */
    return replica_store_create_from_env(destination_index, database_id,
                                         out_report, NULL, err, err_size);
}

otsardb_replica *otsardb_replica_open(otsardb_object_store *primary_store,
                                  const char *database_root,
                                  const char *database_id,
                                  char *err,
                                  size_t err_size) {
    if (err && err_size > 0) err[0] = '\0';

    if (!replica_var_set_configured(0)) {
        if (err) {
            snprintf(err, err_size,
                     "replica environment incomplete: OTSARDB_S3_REPLICA_ENDPOINT and "
                     "OTSARDB_S3_REPLICA_BUCKET are required (fail-closed; no silent "
                     "single-destination fallback)");
        }
        return NULL;
    }

    otsardb_replica *replica = (otsardb_replica *)calloc(1, sizeof(*replica));
    if (!replica) {
        if (err) snprintf(err, err_size, "out of memory opening the replica");
        return NULL;
    }
    replica->promoted_index = -1;
    snprintf(replica->database_root, sizeof(replica->database_root), "%s", database_root);
    snprintf(replica->database_id, sizeof(replica->database_id), "%s", database_id);

    /* open EVERY configured destination set. Set 0
     * (OTSARDB_S3_REPLICA_*) is required (byte-identical 0072 rules); sets
     * 1/2 are optional but each must be COMPLETE when any of its variables
     * is set (fail closed â€” a partial OTSARDB_S3_REPLICA2_* config is an
     * operator error, never a silent single-destination fallback). */
    for (int set = 0; set < OTSARDB_REPLICA_MAX_DESTINATIONS; ++set) {
        if (!replica_var_set_configured(set)) continue;
        otsardb_write_strategy strategy = OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;
        otsardb_object_store *store = replica_store_create_from_env(set, database_id,
                                                                   NULL, &strategy,
                                                                   err, err_size);
        if (!store) {
            free(replica);
            return NULL;
        }
        if (!replica_destination_append(replica, store, strategy, 0)) {
            otsardb_s3_store_destroy(store);
            free(replica);
            if (err) snprintf(err, err_size, "out of memory opening the replica");
            return NULL;
        }
    }

    /* One-time epoch records on the primary and every destination; a
     * transient failure here fails the open (fail-closed) and the sync â€”
     * retried on the next sync. */
    if (!replica_bootstrap_epoch(replica, primary_store, 0)) {
        otsardb_replica_close(replica);
        if (err) {
            snprintf(err, err_size,
                     "failed to write the one-time epoch record on the replica "
                     "(<root>/replica/epoch.json); dual publish cannot start");
        }
        return NULL;
    }

    return replica;
}

/* open and attach every env-configured destination except
 * `exclude_index` as an OWNED replica destination of the publisher (the
 * promoted session's role-swap extension: after the swap, the old primary
 * (attached by the caller) AND the remaining destinations are replicated
 * to). Fail-closed: a destination that cannot be opened/probed fails the
 * whole open â€” the promoted session must not silently drop a destination
 * (a destination that dies AFTER the open is handled by the quorum
 * machinery instead). */
int otsardb_replica_attach_env_others(struct otsardb_wal_publisher *publisher,
                                    int exclude_index,
                                    char *err,
                                    size_t err_size) {
    if (err && err_size > 0) err[0] = '\0';
    if (!publisher) {
        if (err) snprintf(err, err_size, "attach_env_others: null publisher");
        return 0;
    }
    for (int set = 0; set < OTSARDB_REPLICA_MAX_DESTINATIONS; ++set) {
        if (set == exclude_index) continue;
        if (!replica_var_set_configured(set)) continue;
        otsardb_write_strategy strategy = OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;
        otsardb_object_store *store = replica_store_create_from_env(set,
                                                                   publisher->database_id,
                                                                   NULL, &strategy,
                                                                   err, err_size);
        if (!store) {
            if (err && err[0] == '\0') {
                snprintf(err, err_size,
                         "failed to open %s_* for the promoted session (fail-closed)",
                         replica_env_prefixes[set]);
            }
            return 0;
        }
        if (!otsardb_replica_attach(publisher, store, strategy, 0)) {
            otsardb_s3_store_destroy(store);
            if (err) {
                snprintf(err, err_size,
                         "failed to attach %s_* as a replica destination of the "
                         "promoted session (fail-closed)",
                         replica_env_prefixes[set]);
            }
            return 0;
        }
    }
    return 1;
}
#endif /* defined(OTSARDB_ENABLE_S3) */

void otsardb_replica_close(otsardb_replica *replica) {
    if (!replica) return;
    for (size_t i = 0; i < replica->destination_count; ++i) {
        otsardb_replica_destination *dest = &replica->destinations[i];
#if defined(OTSARDB_ENABLE_S3)
        if (dest->store && !dest->store_borrowed) {
            otsardb_s3_store_destroy(dest->store);
        }
#endif
        dest->store = NULL;
    }
    free(replica->destinations);
    free(replica);
}

/* ---- operator promotion ---------------------------------- */

/* The old primary must not be lease-held: read <root>/lease.json on its
 * store and judge expiry with the SAME predicate as the lease module
 * (TTL - skew margin from the store's Last-Modified; an unknown
 * Last-Modified presumes live â€” mirroring otsardb_lease_claim's takeover
 * rule). Returns 1 when promotion may proceed (no lease, or provably
 * expired); 0 with the refusal reason in `err` otherwise. */
static int replica_old_primary_releasable(otsardb_object_store *store,
                                          const char *root,
                                          uint64_t ttl_secs,
                                          char *err,
                                          size_t err_size) {
    char key[OTSARDB_KEY_MAX + 1];
    if (!root_join(root, "lease.json", key, sizeof(key))) {
        if (err) {
            snprintf(err, err_size,
                     "promotion: could not build the old primary lease key (invalid root)");
        }
        return 0;
    }
    otsardb_store_object object = {0};
    otsardb_store_result rc = otsardb_store_get(store, key, &object);
    if (rc == OTSARDB_STORE_NOT_FOUND) return 1; /* never claimed / cleanly released */
    if (rc != OTSARDB_STORE_OK) {
        if (err) {
            snprintf(err, err_size,
                     "promotion: could not read the old primary writer lease "
                     "(<root>/lease.json) â€” store error; promotion refused");
        }
        return 0;
    }
    int expired = 0;
    if (object.last_modified_secs > 0) {
        time_t now = time(NULL);
        int64_t age = (int64_t)now - object.last_modified_secs;
        if (age < 0) age = 0;
        expired = otsardb_lease_age_is_expired(ttl_secs, age);
    }
    otsardb_store_release_object(store, &object);
    if (!expired) {
        if (err) {
            snprintf(err, err_size,
                     "promotion refused: the old primary still holds a live writer lease "
                     "(<root>/lease.json). Promotion is an OFFLINE / DISASTER-RECOVERY "
                     "operator contract: stop the old primary (or wait OTSARDB_LEASE_TTL "
                     "for its lease to expire) and retry");
        }
        return 0;
    }
    return 1;
}

/* ---- promotion pick (newest COMMITTED destination) ------- */

/* The safe-last-commit of one destination: the max generation with a
 * COMMITTED ledger entry there (0069 decision point 5). Computed from the
 * destination's <root>/replica/ ledger listing (one-level list + GET + JSON
 * decode per entry, skipping the epoch record); stores without listing fall
 * back to the HEAD + its ledger record. Returns 1 on success (*out_safe =
 * 0 = no COMMITTED commit); 0 on store errors (fail closed).
 *
 * the callback BUFFERS the ledger keys and the GETs run
 * AFTER otsardb_store_list returns. The S3 adapter (s3_object_store.cpp)
 * holds the process-wide 0064 request mutex ACROSS the list callback, so
 * re-entering otsardb_store_get from the callback would self-deadlock there
 * (the 0099-observed OTSARDB_S3_PROMOTE hang). The pattern mirrors
 * lazy_hydration.c's own list-collect callback (no store calls inside). */
typedef struct replica_safe_scan_ctx {
    char **keys; /* buffered ledger keys (relative to <root>/replica/) */
    size_t count;
    size_t capacity;
    int failed;
    uint64_t safe; /* max COMMITTED generation, computed after the list */
} replica_safe_scan_ctx;

static int replica_safe_scan_cb(const char *key, void *opaque) {
    replica_safe_scan_ctx *ctx = (replica_safe_scan_ctx *)opaque;
    if (!key || key[0] == '\0') return 0;
    if (strcmp(key, "epoch.json") == 0) return 0; /* skip the epoch record */
    if (ctx->count >= 1000000u) { /* mirror the adapter's listing bound */
        ctx->failed = 1;
        return 1;
    }
    if (ctx->count == ctx->capacity) {
        size_t next_capacity = ctx->capacity ? ctx->capacity * 2u : 64u;
        char **next = (char **)realloc(ctx->keys, next_capacity * sizeof(*next));
        if (!next) {
            ctx->failed = 1;
            return 1;
        }
        ctx->keys = next;
        ctx->capacity = next_capacity;
    }
    size_t len = strlen(key);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) {
        ctx->failed = 1;
        return 1;
    }
    memcpy(copy, key, len + 1u);
    ctx->keys[ctx->count++] = copy;
    return 0;
}

static int destination_safe_last_commit(otsardb_object_store *store,
                                        const char *root,
                                        uint64_t *out_safe,
                                        otsardb_journal_result *out_rc) {
    *out_safe = 0;
    *out_rc = OTSARDB_JOURNAL_ERROR;
    if (!store || !root) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    char prefix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(prefix, sizeof(prefix), "%s/replica/", root);
    if (n <= 0 || (size_t)n >= sizeof(prefix)) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    replica_safe_scan_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    otsardb_store_result list_rc = otsardb_store_list(store, prefix,
                                                  replica_safe_scan_cb, &ctx);
    if (list_rc == OTSARDB_STORE_UNSUPPORTED) {
        /* Fallback: HEAD + its ledger record. */
        otsardb_head head;
        char *etag = NULL;
        otsardb_journal_result head_rc = otsardb_journal_read_head(store, root, &head, &etag);
        free(etag);
        if (head_rc == OTSARDB_JOURNAL_OK) {
            char ledger_key[OTSARDB_KEY_MAX + 1];
            int kn = snprintf(ledger_key, sizeof(ledger_key),
                              "%s/replica/%s.json", root, head.commit_id);
            if (kn <= 0 || (size_t)kn >= sizeof(ledger_key)) {
                *out_rc = OTSARDB_JOURNAL_INVALID;
                return 0;
            }
            otsardb_store_object object = {0};
            if (otsardb_store_get(store, ledger_key, &object) == OTSARDB_STORE_OK) {
                otsardb_replica_ledger ledger;
                if (ledger_decode((const char *)object.data, object.size, &ledger) &&
                    strcmp(ledger.state, OTSARDB_REPLICA_STATE_COMMITTED) == 0) {
                    ctx.safe = head.generation;
                }
                otsardb_store_release_object(store, &object);
            }
        } else if (head_rc != OTSARDB_JOURNAL_NOT_FOUND) {
            *out_rc = head_rc;
            return 0;
        }
        *out_rc = OTSARDB_JOURNAL_OK;
        *out_safe = ctx.safe;
        return 1;
    }
    if (list_rc != OTSARDB_STORE_OK || ctx.failed) {
        for (size_t i = 0; i < ctx.count; ++i) free(ctx.keys[i]);
        free(ctx.keys);
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    /* the ledger GETs run HERE, after the list returned —
     * never from inside the callback (the S3 adapter holds its request
     * mutex across the callback; re-entrant GETs would self-deadlock). */
    int store_error = 0;
    for (size_t i = 0; i < ctx.count && !store_error; ++i) {
        char full_key[OTSARDB_KEY_MAX + 1];
        int n = snprintf(full_key, sizeof(full_key), "%s/replica/%s",
                         root, ctx.keys[i]);
        if (n <= 0 || (size_t)n >= sizeof(full_key)) {
            store_error = 1;
            break;
        }
        otsardb_store_object object = {0};
        otsardb_store_result rc = otsardb_store_get(store, full_key, &object);
        if (rc != OTSARDB_STORE_OK) {
            if (rc != OTSARDB_STORE_NOT_FOUND) store_error = 1;
            continue;
        }
        otsardb_replica_ledger ledger;
        if (ledger_decode((const char *)object.data, object.size, &ledger) &&
            strcmp(ledger.state, OTSARDB_REPLICA_STATE_COMMITTED) == 0 &&
            ledger.generation > ctx.safe) {
            ctx.safe = ledger.generation;
        }
        otsardb_store_release_object(store, &object);
    }
    for (size_t i = 0; i < ctx.count; ++i) free(ctx.keys[i]);
    free(ctx.keys);
    if (store_error) {
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_OK;
    *out_safe = ctx.safe;
    return 1;
}

/* (REP-1): the HEAD generation of a destination store
 * (NOT_FOUND = 0, which is never a promotable safe ledger anyway). The
 * promotion selection needs it to prove the destination HEAD sits exactly
 * at its newest COMMITTED ledger. */
static int destination_head_generation(otsardb_object_store *store,
                                       const char *root,
                                       uint64_t *out_generation,
                                       otsardb_journal_result *out_rc) {
    *out_generation = 0;
    *out_rc = OTSARDB_JOURNAL_ERROR;
    if (!store || !root) {
        *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    otsardb_head head;
    char *etag = NULL;
    otsardb_journal_result rc = otsardb_journal_read_head(store, root, &head, &etag);
    free(etag);
    if (rc == OTSARDB_JOURNAL_OK) *out_generation = head.generation;
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK || rc == OTSARDB_JOURNAL_NOT_FOUND;
}

int otsardb_replica_promote(otsardb_replica *replica,
                          otsardb_object_store *old_primary_store,
                          const char *local_db_path,
                          uint64_t lease_ttl_secs,
                          char *err,
                          size_t err_size,
                          otsardb_journal_result *out_rc) {
    if (err && err_size > 0) err[0] = '\0';
    if (!replica || !old_primary_store || !local_db_path || !local_db_path[0] ||
        !out_rc) {
        if (out_rc) *out_rc = OTSARDB_JOURNAL_INVALID;
        return 0;
    }
    *out_rc = OTSARDB_JOURNAL_ERROR;
    uint64_t ttl = lease_ttl_secs == 0 ? replica_lease_ttl_env() : lease_ttl_secs;

    /* 1. The old primary must be offline: a live writer lease there means it
     * may still publish â€” promoting now would split the database. */
    if (!replica_old_primary_releasable(old_primary_store, replica->database_root,
                                        ttl, err, err_size)) {
        *out_rc = OTSARDB_JOURNAL_CONFLICT;
        return 0;
    }

    /* 1b. pick the destination with the NEWEST COMMITTED
     * ledger/HEAD (safe-last-commit; ties keep the earlier destination in
     * list order). Refuse when NO destination holds any COMMITTED commit â€”
     * there is nothing to promote (the operator must seed/reconcile first).
     * The old primary's safe head is NOT compared: in the offline /
     * disaster-recovery contract the old primary is down, and its data may
     * legitimately include unacked (INCONCLUSIVE) commits that never reached
     * any destination â€” the destination set is the recovery source.
     *
     * (REP-1): a destination is promotable ONLY when its
     * HEAD sits EXACTLY at its newest COMMITTED ledger. A crash between the
     * replica HEAD CAS and the COMMITTED ledger write (0072 phase order)
     * leaves a PREPARED-only head one generation ahead of the safe ledger;
     * recovery validates the chain, not the ledger, so recovering from that
     * HEAD would silently make a never-acked commit durable truth. Such a
     * destination is skipped; if every destination at the newest committed
     * state is stuck, the promotion is refused with a repair hint. */
    size_t best = (size_t)-1;
    uint64_t best_safe = 0;
    uint64_t max_committed = 0;
    int any_committed = 0;
    int stuck_head = 0;
    for (size_t i = 0; i < replica->destination_count; ++i) {
        uint64_t safe = 0;
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        if (!destination_safe_last_commit(replica->destinations[i].store,
                                          replica->database_root, &safe, &rc)) {
            if (err) {
                snprintf(err, err_size,
                         "promotion: could not read the COMMITTED ledgers of "
                         "destination %zu (<root>/replica/) â€” store error; "
                         "promotion refused",
                         i);
            }
            *out_rc = rc;
            return 0;
        }
        if (safe > 0) {
            any_committed = 1;
            if (safe > max_committed) max_committed = safe;
        }
        uint64_t head_gen = 0;
        otsardb_journal_result head_rc = OTSARDB_JOURNAL_ERROR;
        if (!destination_head_generation(replica->destinations[i].store,
                                         replica->database_root, &head_gen,
                                         &head_rc)) {
            if (err) {
                snprintf(err, err_size,
                         "promotion: could not read the HEAD of destination %zu "
                         "â€” store error; promotion refused",
                         i);
            }
            *out_rc = head_rc;
            return 0;
        }
        /* Ineligible: no COMMITTED ledger, or a PREPARED-only head (HEAD
         * generation above the safe ledger). A head BELOW the safe ledger
         * cannot occur (the ledger is written only after the destination
         * HEAD CAS for that commit) and is refused defensively too. */
        if (safe == 0 || head_gen != safe) {
            if (safe > 0) stuck_head = 1;
            continue;
        }
        if (safe > best_safe) {
            best_safe = safe;
            best = i;
        }
    }
    if (best == (size_t)-1) {
        if (any_committed && stuck_head) {
            if (err) {
                snprintf(err, err_size,
                         "promotion refused: the newest COMMITTED ledger of the "
                         "destination set is at generation %llu but every "
                         "destination's HEAD is ahead of it â€” a crash between the "
                         "replica HEAD CAS and the COMMITTED ledger write left a "
                         "PREPARED-only head commit. Promoting now would make a "
                         "never-acked commit durable truth; repair the "
                         "destination (complete or rewind its HEAD ledger) and "
                         "retry",
                         (unsigned long long)max_committed);
            }
        } else if (err) {
            snprintf(err, err_size,
                     "promotion refused: no replica destination holds any COMMITTED "
                     "commit (<root>/replica/ ledgers are all absent or behind). "
                     "Seed/reconcile a destination first, then promote");
        }
        *out_rc = OTSARDB_JOURNAL_CONFLICT;
        return 0;
    }
    otsardb_replica_destination *promoted = &replica->destinations[best];
    replica->promoted_index = (int)best;

    /* 2. Raise the picked destination's epoch by CAS (design section 4:
     * promotion = read current, write N+1 with If-Match; If-None-Match
     * create when absent). A lost CAS race (another promoter won) refuses
     * the promotion: two concurrent promotes cannot both win. */
    uint64_t new_epoch = 0;
    if (!otsardb_replica_epoch_raise(promoted->store, replica->database_root,
                                   replica->database_id, NULL, &new_epoch, out_rc)) {
        if (*out_rc == OTSARDB_JOURNAL_CONFLICT) {
            snprintf(err, err_size,
                     "promotion refused: another promotion won the epoch CAS "
                     "(<root>/replica/epoch.json moved since this attempt read it) â€” "
                     "split-brain refused; re-read the destination and retry");
        } else {
            snprintf(err, err_size,
                     "promotion: failed to raise the destination epoch record "
                     "(<root>/replica/epoch.json) â€” store error; nothing was changed");
        }
        return 0;
    }
    promoted->epoch = new_epoch;
    promoted->epoch_known = 1;
    /* Capture the record's new etag so the handle's fence state is exact. */
    {
        otsardb_replica_epoch_view view;
        otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
        if (otsardb_replica_epoch_read(promoted->store, replica->database_root,
                                     &view, &rc) &&
            view.etag[0]) {
            snprintf(promoted->epoch_etag, sizeof(promoted->epoch_etag), "%s", view.etag);
        }
    }

    /* 3. FULL recovery from the promoted store: validate the whole chain and
     * materialise the local execution image at the promoted HEAD. The former
     * destination is now primary. A recovery failure leaves the raised epoch
     * in place (monotonic â€” the operator re-raises on retry) and the OLD
     * primary session stays fenced, which is the safe direction. */
    if (!otsardb_recovery_restore_store(promoted->store, replica->database_root,
                                      local_db_path)) {
        if (err) {
            snprintf(err, err_size,
                     "promotion: full recovery from the promoted destination failed "
                     "(chain validation or materialisation error). The destination epoch "
                     "was raised (monotonic) and the old primary session is fenced; fix "
                     "the store and retry the promotion");
        }
        *out_rc = OTSARDB_JOURNAL_ERROR;
        return 0;
    }

    *out_rc = OTSARDB_JOURNAL_OK;
    return 1;
}

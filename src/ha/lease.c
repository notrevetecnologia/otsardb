#include "lease.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Phase-1 expiry model (design doc 0014, implementation note in 
 * 0027): liveness for the OWNER is judged from the local client clock
 * (`claimed_at` re-anchored by claim/adopt/renew). A COMPETITOR's takeover
 * additionally requires the store's Last-Modified (`otsardb_store_object
 * .last_modified_secs`) to prove expiry: Last-Modified + TTL - skew. When
 * the store does not expose it (0 = unknown), the lease is presumed live
 * and cannot be taken over. The CAS generation is the true fence in both
 * cases: every takeover and renewal is an If-Match overwrite, so two
 * writers can never both believe they hold the same generation.
 *
 * Clock-skew envelope: a takeover candidate whose clock
 * is ahead can take over while the holder's own clock still reports the
 * lease live; the lease-level liveness window is bounded by
 * OTSARDB_LEASE_SKEW_MARGIN_SECS + TTL/3. The data-loss window is zero:
 * the publish fence's generation anchor (wal_publisher.c) refuses a stale
 * base once a competitor has advanced HEAD, independent of clock skew.
 *
 * Thread safety (phase 2): the renew thread mutates
 * etag/claimed_at/lost while the publish fence reads lost/claimed_at via
 * otsardb_lease_is_live, so every field access is guarded by `lock`. Network
 * I/O happens outside the lock: renew snapshots its CAS inputs under the
 * lock, performs the store round trip unlocked, then commits the outcome
 * (etag/claimed_at or lost) under the lock. otsardb_lease_destroy must not
 * run while another thread may still touch the lease (join first). */

#if defined(_WIN32)
typedef CRITICAL_SECTION otsardb_lease_lock;
#else
typedef pthread_mutex_t otsardb_lease_lock;
#endif

struct otsardb_lease {
    otsardb_object_store *store;
    char key[OTSARDB_LEASE_KEY_MAX + 1];
    char owner[OTSARDB_LEASE_OWNER_MAX + 1];
    char address[OTSARDB_LEASE_ADDRESS_MAX + 1];
    uint64_t generation;
    char etag[128];
    uint64_t ttl_secs;
    int64_t claimed_at;
    int lost;
    otsardb_lease_lock lock;
};

static void lease_lock_init(otsardb_lease *lease) {
#if defined(_WIN32)
    InitializeCriticalSection(&lease->lock);
#else
    pthread_mutex_init(&lease->lock, NULL);
#endif
}

static void lease_lock_destroy(otsardb_lease *lease) {
#if defined(_WIN32)
    DeleteCriticalSection(&lease->lock);
#else
    pthread_mutex_destroy(&lease->lock);
#endif
}

static void lease_lock(otsardb_lease *lease) {
#if defined(_WIN32)
    EnterCriticalSection(&lease->lock);
#else
    pthread_mutex_lock(&lease->lock);
#endif
}

static void lease_unlock(otsardb_lease *lease) {
#if defined(_WIN32)
    LeaveCriticalSection(&lease->lock);
#else
    pthread_mutex_unlock(&lease->lock);
#endif
}

static int64_t clock_now(void) {
    return (int64_t)time(NULL);
}

/* Expired once the age reaches ttl - skew; the window is clamped at 0 so a
 * tiny TTL fails closed (never extends liveness). */
static int age_is_expired(int64_t age_secs, uint64_t ttl_secs) {
    int64_t window = (int64_t)ttl_secs - OTSARDB_LEASE_SKEW_MARGIN_SECS;
    if (window < 0) window = 0;
    return age_secs >= window;
}

int otsardb_lease_age_is_expired(uint64_t ttl_secs, int64_t age_secs) {
    return age_is_expired(age_secs, ttl_secs);
}

static size_t encode_lease_json(char *buffer,
                                size_t cap,
                                const char *owner,
                                uint64_t generation,
                                const char *address) {
    int written;
    if (address && address[0]) {
        written = snprintf(buffer,
                           cap,
                           "{\"owner\":\"%s\",\"generation\":%llu,\"address\":\"%s\"}",
                           owner,
                           (unsigned long long)generation,
                           address);
    } else {
        written = snprintf(buffer,
                           cap,
                           "{\"owner\":\"%s\",\"generation\":%llu}",
                           owner,
                           (unsigned long long)generation);
    }
    return written > 0 ? (size_t)written : 0;
}

/* Extract a quoted string field from `json` at `key`. Returns 1 on success
 * (empty string allowed), 0 when the key is absent or malformed. */
static int parse_lease_string_field(const char *json,
                                    size_t size,
                                    const char *key,
                                    char *out,
                                    size_t out_cap) {
    if (!json || !key || !out || !out_cap) return 0;
    out[0] = '\0';
    const char *found = strstr(json, key);
    if (!found) return 0;
    const char *value_open = strchr(found + strlen(key), '"');
    if (!value_open) return 0;
    const char *value_start = value_open + 1;
    const char *value_close = strchr(value_start, '"');
    if (!value_close || value_close >= json + size) return 0;
    size_t value_len = (size_t)(value_close - value_start);
    if (value_len >= out_cap) return 0;
    memcpy(out, value_start, value_len);
    out[value_len] = '\0';
    return 1;
}

static int parse_lease_json(const char *json,
                            size_t size,
                            char *owner,
                            size_t owner_cap,
                            uint64_t *generation,
                            char *address,
                            size_t address_cap) {
    if (!json || !owner || !generation) return 0;
    if (!parse_lease_string_field(json, size, "\"owner\"", owner, owner_cap) ||
        !owner[0]) {
        return 0;
    }
    const char *gen_key = strstr(json, "\"generation\"");
    if (!gen_key) return 0;

    const char *number = gen_key + strlen("\"generation\"");
    while (*number == ':' || *number == ' ' || *number == '\t' ||
           *number == '\r' || *number == '\n') {
        ++number;
    }
    char *end = NULL;
    unsigned long long parsed = strtoull(number, &end, 10);
    if (end == number) return 0;
    const char *tail = end;
    while (*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n') ++tail;
    if (*tail != '}' && *tail != ',' && *tail != '\0') return 0;
    *generation = (uint64_t)parsed;

    /* Optional address field: absent -> empty. */
    if (address && address_cap) {
        if (!parse_lease_string_field(json, size, "\"address\"", address, address_cap)) {
            address[0] = '\0';
        }
    }
    return 1;
}

static int lease_address_valid(const char *address) {
    if (!address) return 1;
    if (strchr(address, '"') || strchr(address, '\\')) return 0;
    return 1;
}

static void lease_set(otsardb_lease *lease,
                      const char *owner,
                      uint64_t generation,
                      const char *address,
                      const char *etag) {
    lease_lock(lease);
    snprintf(lease->owner, sizeof(lease->owner), "%s", owner);
    lease->generation = generation;
    snprintf(lease->address,
             sizeof(lease->address),
             "%s",
             address && lease_address_valid(address) ? address : "");
    snprintf(lease->etag, sizeof(lease->etag), "%s", etag ? etag : "");
    lease->claimed_at = clock_now();
    lease->lost = 0;
    lease_unlock(lease);
}

otsardb_lease *otsardb_lease_claim(otsardb_object_store *store,
                               const char *database_root,
                               const char *instance_id,
                               const char *address,
                               uint64_t ttl_secs,
                               uint64_t *epoch_out,
                               int *claim_state_out,
                               char *holder_out,
                               size_t holder_cap,
                               char *address_out,
                               size_t address_cap) {
    if (claim_state_out) *claim_state_out = OTSARDB_LEASE_ERROR;
    if (epoch_out) *epoch_out = 0;
    if (holder_out && holder_cap) holder_out[0] = '\0';
    if (address_out && address_cap) address_out[0] = '\0';

    if (!store || !database_root || !database_root[0] || !instance_id ||
        !instance_id[0] || strlen(database_root) >= OTSARDB_LEASE_KEY_MAX) {
        return NULL;
    }
    if (strchr(instance_id, '"') || strchr(instance_id, '\\') ||
        !lease_address_valid(address)) {
        /* would break the JSON object */
        return NULL;
    }

    otsardb_lease *lease = (otsardb_lease *)calloc(1, sizeof(*lease));
    if (!lease) return NULL;
    lease->store = store;
    lease->ttl_secs = ttl_secs == 0 ? OTSARDB_LEASE_TTL_DEFAULT_SECS : ttl_secs;
    snprintf(lease->key, sizeof(lease->key), "%s/lease.json", database_root);
    lease_lock_init(lease);

    int state = OTSARDB_LEASE_HELD;
    otsardb_lease *result = NULL;

    for (int attempt = 0; attempt < 3 && !result; ++attempt) {
        otsardb_store_object object = {0};
        otsardb_store_result rc = otsardb_store_get(store, lease->key, &object);

        if (rc == OTSARDB_STORE_NOT_FOUND) {
            /* Fresh database: create at generation 1 with If-None-Match:*.
             * A concurrent create fails the CAS and is re-read below. */
            char json[OTSARDB_LEASE_JSON_MAX];
            if (!encode_lease_json(json, sizeof(json), instance_id, 1, address)) {
                state = OTSARDB_LEASE_ERROR;
                break;
            }
            char *etag = NULL;
            otsardb_store_put_options options = {0};
            options.if_none_match = 1;
            rc = otsardb_store_put(store, lease->key, json, strlen(json), &options, &etag);
            if (rc == OTSARDB_STORE_OK) {
                lease_set(lease, instance_id, 1, address, etag);
                if (epoch_out) *epoch_out = 1;
                state = OTSARDB_LEASE_CLAIMED;
                result = lease;
            } else if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
                continue;
            } else {
                state = OTSARDB_LEASE_ERROR;
                break;
            }
            free(etag);
            continue;
        }

        if (rc != OTSARDB_STORE_OK) {
            state = OTSARDB_LEASE_ERROR;
            break;
        }

        char current_owner[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
        char current_address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
        uint64_t generation = 0;
        if (!parse_lease_json((const char *)object.data,
                              object.size,
                              current_owner,
                              sizeof(current_owner),
                              &generation,
                              current_address,
                              sizeof(current_address))) {
            otsardb_store_release_object(store, &object);
            state = OTSARDB_LEASE_ERROR;
            break;
        }
        if (holder_out && holder_cap) {
            snprintf(holder_out, holder_cap, "%s", current_owner);
        }
        if (address_out && address_cap) {
            snprintf(address_out, address_cap, "%s", current_address);
        }

        if (strcmp(current_owner, instance_id) == 0) {
            /* Same owner (reconnect/adoption): CAS re-stamp at the same
             * generation to re-anchor liveness. */
            char json[OTSARDB_LEASE_JSON_MAX];
            if (!encode_lease_json(json, sizeof(json), instance_id, generation, address)) {
                otsardb_store_release_object(store, &object);
                state = OTSARDB_LEASE_ERROR;
                break;
            }
            char *etag = NULL;
            otsardb_store_put_options options = {0};
            options.if_match_etag = object.etag;
            rc = otsardb_store_put(store, lease->key, json, strlen(json), &options, &etag);
            otsardb_store_release_object(store, &object);
            if (rc == OTSARDB_STORE_OK) {
                lease_set(lease, instance_id, generation, address, etag);
                if (epoch_out) *epoch_out = generation;
                state = OTSARDB_LEASE_CLAIMED;
                result = lease;
            } else if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
                continue;
            } else {
                state = OTSARDB_LEASE_ERROR;
                break;
            }
            free(etag);
            continue;
        }

        /* Different owner: takeover requires provable expiry. Without the
         * store's Last-Modified the lease is presumed live. */
        int expired = 0;
        if (object.last_modified_secs > 0) {
            int64_t age = clock_now() - object.last_modified_secs;
            if (age < 0) age = 0;
            expired = age_is_expired(age, lease->ttl_secs);
        }
        if (!expired) {
            otsardb_store_release_object(store, &object);
            state = OTSARDB_LEASE_HELD;
            break;
        }

        char json[OTSARDB_LEASE_JSON_MAX];
        if (!encode_lease_json(json, sizeof(json), instance_id, generation + 1, address)) {
            otsardb_store_release_object(store, &object);
            state = OTSARDB_LEASE_ERROR;
            break;
        }
        char *etag = NULL;
        otsardb_store_put_options options = {0};
        options.if_match_etag = object.etag;
        rc = otsardb_store_put(store, lease->key, json, strlen(json), &options, &etag);
        otsardb_store_release_object(store, &object);
        if (rc == OTSARDB_STORE_OK) {
            lease_set(lease, instance_id, generation + 1, address, etag);
            if (epoch_out) *epoch_out = generation + 1;
            state = OTSARDB_LEASE_CLAIMED;
            result = lease;
        } else if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
            /* Racing takeover won; re-read and re-evaluate. */
            continue;
        } else {
            state = OTSARDB_LEASE_ERROR;
            break;
        }
        free(etag);
    }

    if (!result) {
        if (claim_state_out) *claim_state_out = state;
        lease_lock_destroy(lease);
        free(lease);
        return NULL;
    }
    if (claim_state_out) *claim_state_out = OTSARDB_LEASE_CLAIMED;
    return result;
}

int otsardb_lease_renew(otsardb_lease *lease) {
    if (!lease) return 2;

    /* Snapshot the CAS inputs under the lock; the store round trip below
     * runs unlocked so a slow renew never blocks the publish fence. */
    char owner[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
    char address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
    char etag[128] = {0};
    uint64_t generation = 0;
    int lost = 0;
    lease_lock(lease);
    lost = lease->lost;
    if (!lost) {
        snprintf(owner, sizeof(owner), "%s", lease->owner);
        snprintf(address, sizeof(address), "%s", lease->address);
        snprintf(etag, sizeof(etag), "%s", lease->etag);
        generation = lease->generation;
    }
    lease_unlock(lease);
    if (lost) return 1;

    char json[OTSARDB_LEASE_JSON_MAX];
    if (!encode_lease_json(json, sizeof(json), owner, generation, address)) {
        return 2;
    }
    char *out_etag = NULL;
    otsardb_store_put_options options = {0};
    options.if_match_etag = etag;
    otsardb_store_result rc = otsardb_store_put(lease->store,
                                            lease->key,
                                            json,
                                            strlen(json),
                                            &options,
                                            &out_etag);
    if (rc == OTSARDB_STORE_OK) {
        lease_lock(lease);
        if (!lease->lost) {
            snprintf(lease->etag, sizeof(lease->etag), "%s", out_etag ? out_etag : "");
            lease->claimed_at = clock_now();
        }
        lease_unlock(lease);
        free(out_etag);
        return 0;
    }
    free(out_etag);
    if (rc == OTSARDB_STORE_PRECONDITION_FAILED) {
        lease_lock(lease);
        lease->lost = 1;
        lease_unlock(lease);
        return 1;
    }
    return 2;
}

int otsardb_lease_is_live(const otsardb_lease *lease) {
    if (!lease) return 0;
    int lost = 0;
    int64_t claimed_at = 0;
    uint64_t ttl_secs = OTSARDB_LEASE_TTL_DEFAULT_SECS;
    lease_lock((otsardb_lease *)lease);
    lost = lease->lost;
    claimed_at = lease->claimed_at;
    ttl_secs = lease->ttl_secs;
    lease_unlock((otsardb_lease *)lease);
    if (lost) return 0;
    int64_t age = clock_now() - claimed_at;
    if (age < 0) age = 0;
    return age_is_expired(age, ttl_secs) ? 0 : 1;
}

int otsardb_lease_address(const otsardb_lease *lease, char *out, size_t out_cap) {
    if (!lease || !out || !out_cap) return 0;
    out[0] = '\0';
    lease_lock((otsardb_lease *)lease);
    snprintf(out, out_cap, "%s", lease->address);
    lease_unlock((otsardb_lease *)lease);
    return out[0] != '\0';
}

void otsardb_lease_release(otsardb_lease *lease) {
    if (!lease) return;
    int lost = 0;
    int live = 0;
    lease_lock(lease);
    lost = lease->lost;
    if (!lost) {
        int64_t age = clock_now() - lease->claimed_at;
        if (age < 0) age = 0;
        live = age_is_expired(age, lease->ttl_secs) ? 0 : 1;
    }
    lease_unlock(lease);
    if (lost || !live) return;
    otsardb_store_delete(lease->store, lease->key);
    lease_lock(lease);
    lease->lost = 1;
    lease_unlock(lease);
}

void otsardb_lease_destroy(otsardb_lease *lease) {
    if (!lease) return;
    lease_lock_destroy(lease);
    free(lease);
}

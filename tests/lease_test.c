#include "fault_store.h"
#include "lease.h"
#include "memory_store.h"
#include "object_store.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#define otsardb_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#include <pthread.h>
#define otsardb_sleep_ms(ms) usleep((ms)*1000)
#endif

#if defined(_WIN32)
typedef uintptr_t otsardb_test_thread_t;
static int test_thread_start(otsardb_test_thread_t *thread,
                             unsigned int(__stdcall *fn)(void *),
                             void *arg) {
    *thread = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    return *thread != 0;
}
static int test_thread_join(otsardb_test_thread_t thread) {
    if (!thread) return 0;
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
    return 1;
}
#else
typedef pthread_t otsardb_test_thread_t;
static int test_thread_start(otsardb_test_thread_t *thread,
                             void *(*fn)(void *),
                             void *arg) {
    return pthread_create(thread, NULL, fn, arg) == 0;
}
static int test_thread_join(otsardb_test_thread_t thread) {
    return pthread_join(thread, NULL) == 0;
}
#endif

#define TEST_ROOT "lease-test"

static void assert_object_absent(otsardb_object_store *store, const char *key) {
    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, key, &object) == OTSARDB_STORE_NOT_FOUND);
}

static void rewrite_lease_object(otsardb_object_store *store,
                                 const char *key,
                                 const char *owner,
                                 uint64_t generation) {
    char json[128];
    snprintf(json, sizeof(json), "{\"owner\":\"%s\",\"generation\":%llu}",
             owner, (unsigned long long)generation);
    otsardb_store_put_options options = {0};
    assert(otsardb_store_put(store, key, json, strlen(json), &options, NULL) ==
           OTSARDB_STORE_OK);
}

/* Phase-2 renewal-thread worker: renews until the lease is lost or the
 * iteration budget is spent. Runs concurrently with is_live() reads from
 * the main thread (the publish-fence position). */
#if defined(_WIN32)
static unsigned int __stdcall renew_worker(void *arg) {
#else
static void *renew_worker(void *arg) {
#endif
    otsardb_lease *lease = (otsardb_lease *)arg;
    for (int i = 0; i < 25; ++i) {
        if (otsardb_lease_is_live(lease) == 0) break;
        int rc = otsardb_lease_renew(lease);
        assert(rc == 0 || rc == 1);
        if (rc == 1) break;
        otsardb_sleep_ms(10);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

int main(void) {
    otsardb_object_store *store = otsardb_test_memory_store_create();
    assert(store != NULL);

    /* --- claim when absent: create at generation 1 via If-None-Match --- */
    uint64_t epoch = 0;
    int state = OTSARDB_LEASE_ERROR;
    otsardb_lease *lease_a =
        otsardb_lease_claim(store, TEST_ROOT "/absent", "writer-A", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_a != NULL);
    assert(state == OTSARDB_LEASE_CLAIMED);
    assert(epoch == 1);
    assert(otsardb_lease_is_live(lease_a) == 1);

    otsardb_store_object object = {0};
    assert(otsardb_store_get(store, TEST_ROOT "/absent/lease.json", &object) ==
           OTSARDB_STORE_OK);
    assert(strstr((const char *)object.data, "\"owner\":\"writer-A\"") != NULL);
    assert(strstr((const char *)object.data, "\"generation\":1") != NULL);
    assert(object.last_modified_secs > 0);
    otsardb_store_release_object(store, &object);

    /* --- claim fails when held live: different owner, fresh lease --- */
    char holder[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
    epoch = 0;
    state = OTSARDB_LEASE_ERROR;
    otsardb_lease *lease_b =
        otsardb_lease_claim(store, TEST_ROOT "/absent", "writer-B", NULL, 10,
                          &epoch, &state, holder, sizeof(holder), NULL, 0);
    assert(lease_b == NULL);
    assert(state == OTSARDB_LEASE_HELD);
    assert(epoch == 0);
    assert(strcmp(holder, "writer-A") == 0);
    assert(otsardb_lease_is_live(lease_a) == 1);

    /* --- claim succeeds on expired: CAS overwrite at generation + 1 --- */
    otsardb_lease *lease_a2 =
        otsardb_lease_claim(store, TEST_ROOT "/expired", "writer-A", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_a2 != NULL);
    assert(state == OTSARDB_LEASE_CLAIMED);
    assert(epoch == 1);
    /* Age the object's Last-Modified past TTL (10) - skew (5) + margin. */
    otsardb_test_memory_store_age(store, TEST_ROOT "/expired/lease.json", 20);

    holder[0] = '\0';
    epoch = 0;
    state = OTSARDB_LEASE_ERROR;
    otsardb_lease *lease_b2 =
        otsardb_lease_claim(store, TEST_ROOT "/expired", "writer-B", NULL, 10,
                          &epoch, &state, holder, sizeof(holder), NULL, 0);
    assert(lease_b2 != NULL);
    assert(state == OTSARDB_LEASE_CLAIMED);
    assert(epoch == 2);
    assert(strcmp(holder, "writer-A") == 0); /* pre-takeover holder */
    assert(otsardb_lease_is_live(lease_b2) == 1);

    assert(otsardb_store_get(store, TEST_ROOT "/expired/lease.json", &object) ==
           OTSARDB_STORE_OK);
    assert(strstr((const char *)object.data, "\"owner\":\"writer-B\"") != NULL);
    assert(strstr((const char *)object.data, "\"generation\":2") != NULL);
    otsardb_store_release_object(store, &object);

    /* --- renew OK: CAS If-Match, same generation, re-anchors liveness --- */
    assert(otsardb_lease_renew(lease_b2) == 0);
    assert(otsardb_lease_is_live(lease_b2) == 1);
    assert(otsardb_store_get(store, TEST_ROOT "/expired/lease.json", &object) ==
           OTSARDB_STORE_OK);
    assert(strstr((const char *)object.data, "\"generation\":2") != NULL);
    otsardb_store_release_object(store, &object);

    /* --- renew 412 -> lost: the old owner lost the CAS race --- */
    assert(otsardb_lease_renew(lease_a2) == 1);
    assert(otsardb_lease_is_live(lease_a2) == 0);

    /* --- renew 412 -> lost: external rewrite of the lease object --- */
    otsardb_lease *lease_a3 =
        otsardb_lease_claim(store, TEST_ROOT "/rewrite", "writer-A", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_a3 != NULL);
    rewrite_lease_object(store, TEST_ROOT "/rewrite/lease.json", "writer-X", 9);
    assert(otsardb_lease_renew(lease_a3) == 1);
    assert(otsardb_lease_is_live(lease_a3) == 0);
    /* release of a lost lease must not touch the object */
    otsardb_lease_release(lease_a3);
    assert(otsardb_store_get(store, TEST_ROOT "/rewrite/lease.json", &object) ==
           OTSARDB_STORE_OK);
    otsardb_store_release_object(store, &object);

    /* --- is_live after expiry = 0 (local clock, TTL - skew) --- */
    otsardb_lease *lease_e =
        otsardb_lease_claim(store, TEST_ROOT "/expiry", "writer-E", NULL, 6,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_e != NULL);
    assert(otsardb_lease_is_live(lease_e) == 1);
    otsardb_sleep_ms(2500); /* TTL 6 - skew 5 = 1 s liveness window */
    assert(otsardb_lease_is_live(lease_e) == 0);

    /* --- release: live owner deletes the object; next claim starts at 1 --- */
    otsardb_lease *lease_f =
        otsardb_lease_claim(store, TEST_ROOT "/release", "writer-F", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_f != NULL);
    otsardb_lease_release(lease_f);
    otsardb_lease_destroy(lease_f);
    assert_object_absent(store, TEST_ROOT "/release/lease.json");

    otsardb_lease *lease_g =
        otsardb_lease_claim(store, TEST_ROOT "/release", "writer-G", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_g != NULL);
    assert(state == OTSARDB_LEASE_CLAIMED);
    assert(epoch == 1);

    /* --- fault injection: store errors are errors, not "held" --- */
    otsardb_object_store *inner = otsardb_test_memory_store_create();
    assert(inner != NULL);
    otsardb_object_store *fault = otsardb_test_fault_store_create(inner);
    assert(fault != NULL);

    /* GET failure during claim -> error */
    otsardb_test_fault_store_fail_get(fault, 1, OTSARDB_STORE_ERROR);
    state = OTSARDB_LEASE_ERROR;
    otsardb_lease *lease_err =
        otsardb_lease_claim(fault, TEST_ROOT "/fault", "writer-F", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_err == NULL);
    assert(state == OTSARDB_LEASE_ERROR);

    /* create PUT failure during claim -> error */
    otsardb_test_fault_store_fail_put(fault, 1, OTSARDB_STORE_ERROR, 0);
    state = OTSARDB_LEASE_ERROR;
    lease_err = otsardb_lease_claim(fault, TEST_ROOT "/fault", "writer-F", NULL, 10,
                                  &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_err == NULL);
    assert(state == OTSARDB_LEASE_ERROR);

    /* renew transport error -> 2, lease state unchanged (still live) */
    otsardb_test_fault_store_clear(fault);
    otsardb_lease *lease_ok =
        otsardb_lease_claim(fault, TEST_ROOT "/fault", "writer-F", NULL, 10,
                          &epoch, &state, NULL, 0, NULL, 0);
    assert(lease_ok != NULL);
    assert(otsardb_lease_is_live(lease_ok) == 1);
    otsardb_test_fault_store_fail_put(fault, 1, OTSARDB_STORE_ERROR, 0);
    assert(otsardb_lease_renew(lease_ok) == 2);
    assert(otsardb_lease_is_live(lease_ok) == 1);

    otsardb_lease_destroy(lease_ok);
    otsardb_test_fault_store_destroy(fault);
    otsardb_test_memory_store_destroy(inner);

    /* --- phase 2: multi-session automatic failover ---
     * Session A holds the lease; its renewal thread stops (crash). Session
     * B opens in candidate mode and its poller GETs the lease; while the
     * store's Last-Modified proves the lease still live, the poll returns
     * HELD; once aged past TTL - skew, the poll CAS-overwrites at
     * generation + 1 (takeover), B becomes live and A's next renew 412s
     * (A's publish fence flips to SQLITE_BUSY). */
    otsardb_object_store *failover_store = otsardb_test_memory_store_create();
    assert(failover_store != NULL);

    uint64_t f_epoch = 0;
    int f_state = OTSARDB_LEASE_ERROR;
    otsardb_lease *leader_a =
        otsardb_lease_claim(failover_store, TEST_ROOT "/failover", "writer-A", NULL, 10,
                          &f_epoch, &f_state, NULL, 0, NULL, 0);
    assert(leader_a != NULL);
    assert(f_state == OTSARDB_LEASE_CLAIMED);
    assert(f_epoch == 1);

    /* B's candidate poller: a live foreign lease is never claimed. */
    otsardb_lease *candidate_b = NULL;
    for (int poll = 0; poll < 3; ++poll) {
        candidate_b =
            otsardb_lease_claim(failover_store, TEST_ROOT "/failover", "writer-B", NULL, 10,
                              &f_epoch, &f_state, NULL, 0, NULL, 0);
        assert(candidate_b == NULL);
        assert(f_state == OTSARDB_LEASE_HELD);
        otsardb_sleep_ms(50); /* ~2 s poll cadence, scaled down for tests */
    }
    assert(otsardb_lease_is_live(leader_a) == 1);

    /* A stops renewing (crash). Age the lease object past TTL - skew. */
    otsardb_test_memory_store_age(failover_store, TEST_ROOT "/failover/lease.json", 20);

    /* B's next poll takes the lease over: CAS overwrite, generation + 1. */
    candidate_b =
        otsardb_lease_claim(failover_store, TEST_ROOT "/failover", "writer-B", NULL, 10,
                          &f_epoch, &f_state, NULL, 0, NULL, 0);
    assert(candidate_b != NULL);
    assert(f_state == OTSARDB_LEASE_CLAIMED);
    assert(f_epoch == 2);
    assert(otsardb_lease_is_live(candidate_b) == 1);

    /* A's own view of the world: if A were alive, its next renew 412s and
     * the publish fence (is_live) permanently flips closed. */
    assert(otsardb_lease_renew(leader_a) == 1);
    assert(otsardb_lease_is_live(leader_a) == 0);

    /* The store object now belongs to B at generation 2. */
    otsardb_store_object f_object = {0};
    assert(otsardb_store_get(failover_store, TEST_ROOT "/failover/lease.json",
                           &f_object) == OTSARDB_STORE_OK);
    assert(strstr((const char *)f_object.data, "\"owner\":\"writer-B\"") != NULL);
    assert(strstr((const char *)f_object.data, "\"generation\":2") != NULL);
    otsardb_store_release_object(failover_store, &f_object);

    /* B renews; A's fence stays closed even after B's renewal re-stamps. */
    assert(otsardb_lease_renew(candidate_b) == 0);
    assert(otsardb_lease_is_live(candidate_b) == 1);
    assert(otsardb_lease_is_live(leader_a) == 0);

    otsardb_lease_destroy(candidate_b);
    otsardb_lease_destroy(leader_a);
    otsardb_test_memory_store_destroy(failover_store);

    /* --- phase 2: renew thread vs fence thread-safety smoke test ---
     * A renewal thread mutates etag/claimed_at while the main thread
     * (publisher fence position) reads is_live concurrently. The store is
     * touched only by the renewal thread, so this exercises the lease
     * struct mutex without racing the memory store. */
    otsardb_object_store *thread_store = otsardb_test_memory_store_create();
    assert(thread_store != NULL);
    uint64_t t_epoch = 0;
    int t_state = OTSARDB_LEASE_ERROR;
    otsardb_lease *thread_lease =
        otsardb_lease_claim(thread_store, TEST_ROOT "/thread", "writer-T", NULL, 10,
                          &t_epoch, &t_state, NULL, 0, NULL, 0);
    assert(thread_lease != NULL);

    otsardb_test_thread_t renewer;
    assert(test_thread_start(&renewer, renew_worker, thread_lease) != 0);
    for (int i = 0; i < 60; ++i) {
        assert(otsardb_lease_is_live(thread_lease) == 0 ||
               otsardb_lease_is_live(thread_lease) == 1);
        otsardb_sleep_ms(5);
    }
    assert(test_thread_join(renewer) != 0);
    /* Renewals kept the lease live for the whole run. */
    assert(otsardb_lease_is_live(thread_lease) == 1);

    otsardb_lease_destroy(thread_lease);
    otsardb_test_memory_store_destroy(thread_store);

    /* --- phase 3: optional address field ---
     * The leader publishes "host:port" at claim time; a candidate's
     * failed claim reports it; renewals preserve it; old leases without
     * the field parse fine (address empty -> forwarding disabled). */
    otsardb_object_store *addr_store = otsardb_test_memory_store_create();
    assert(addr_store != NULL);

    char address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
    uint64_t a_epoch = 0;
    int a_state = OTSARDB_LEASE_ERROR;
    otsardb_lease *leader_l =
        otsardb_lease_claim(addr_store, TEST_ROOT "/address", "leader", "127.0.0.1:18080",
                          10, &a_epoch, &a_state, NULL, 0, NULL, 0);
    assert(leader_l != NULL);
    assert(a_state == OTSARDB_LEASE_CLAIMED);
    assert(otsardb_lease_address(leader_l, address, sizeof(address)) == 1);
    assert(strcmp(address, "127.0.0.1:18080") == 0);

    otsardb_store_object addr_object = {0};
    assert(otsardb_store_get(addr_store, TEST_ROOT "/address/lease.json", &addr_object) ==
           OTSARDB_STORE_OK);
    assert(strstr((const char *)addr_object.data,
                  "\"address\":\"127.0.0.1:18080\"") != NULL);
    otsardb_store_release_object(addr_store, &addr_object);

    /* Renewal preserves the published address. */
    assert(otsardb_lease_renew(leader_l) == 0);
    assert(otsardb_lease_address(leader_l, address, sizeof(address)) == 1);
    assert(strcmp(address, "127.0.0.1:18080") == 0);

    /* A HELD claim reports the holder's address to the candidate. */
    a_state = OTSARDB_LEASE_ERROR;
    char cand_address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
    otsardb_lease *candidate_l =
        otsardb_lease_claim(addr_store, TEST_ROOT "/address", "candidate", NULL,
                          10, &a_epoch, &a_state, NULL, 0, cand_address,
                          sizeof(cand_address));
    assert(candidate_l == NULL);
    assert(a_state == OTSARDB_LEASE_HELD);
    assert(strcmp(cand_address, "127.0.0.1:18080") == 0);

    /* Old lease objects without an address stay readable and report none. */
    rewrite_lease_object(addr_store, TEST_ROOT "/address/lease.json", "other", 9);
    a_state = OTSARDB_LEASE_ERROR;
    cand_address[0] = '\0';
    otsardb_lease *candidate_l2 =
        otsardb_lease_claim(addr_store, TEST_ROOT "/address", "candidate", NULL,
                          10, &a_epoch, &a_state, NULL, 0, cand_address,
                          sizeof(cand_address));
    assert(candidate_l2 == NULL);
    assert(a_state == OTSARDB_LEASE_HELD);
    assert(cand_address[0] == '\0');

    otsardb_lease_destroy(leader_l);
    otsardb_test_memory_store_destroy(addr_store);

    otsardb_lease_destroy(lease_a);
    otsardb_lease_destroy(lease_b2);
    otsardb_lease_destroy(lease_a2);
    otsardb_lease_destroy(lease_e);
    otsardb_lease_destroy(lease_g);
    otsardb_test_memory_store_destroy(store);

    puts("writer lease claim/renew/is_live/release semantics passed");
    return 0;
}

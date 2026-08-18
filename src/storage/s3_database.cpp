#include "s3_database.h"

#include "otsardb_internal.h"
#include "capability_probe.h"
#include "checkpoint_build.h"
#include "checkpoint_manifest.h"
#include "cipher.h"
#include "forward.h"
#include "gc.h"
#include "journal.h"
#include "lazy_hydration.h"
#include "lease.h"
#include "local_cache.h"
#include "local_vfs.h"
#include "recovery.h"
#include "replica.h"
#include "s3_object_store.h"
#include "s3_target.h"
#include "sha256.h"
#include "sqlite3.h"
#include "wal_publisher.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * (audit finding F04): cache directory permissions.
 *
 * The disposable execution-image dir (temp_directory_path()/otsardb by
 * default) holds the FULL unencrypted database image, and the probe-verdict
 * dir (/tmp/otsardb-probe-cache) is advisory but plantable. On POSIX
 * multi-user hosts the default umask left them world-readable (0755 under
 * umask 022), letting any local user list and read database images while a
 * session runs or after a crash.
 *
 * Fix: the dirs are created with mode 0700 and the mode is RE-APPLIED on
 * every open/save, so a pre-existing dir created by an older binary (0755)
 * is tightened instead of left loose. A chmod failure on POSIX fails closed:
 * make_execution_path refuses to place the image in an unhardened dir, and
 * the probe cache refuses to write a verdict into one.
 *
 * Windows keeps its default semantics: access is governed by the user's ACLs
 * on %TEMP%, mode bits are not applicable, and the helper only creates the
 * directory (create_directories is idempotent).
 * ------------------------------------------------------------------------- */

extern "C" int otsardb_cache_dir_apply_perms(const char *dir) {
    if (!dir || dir[0] == '\0') return 0;
    try {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return 0;
#if !defined(_WIN32)
        if (::chmod(dir, 0700) != 0) return 0;
#endif
        return 1;
    } catch (...) {
        return 0;
    }
}

namespace {

thread_local std::string g_last_error;
std::atomic<unsigned long long> g_session_counter{0};
std::mutex g_probe_mutex;

/* M2 phase-2 lease background thread: the holder renews
 * the lease every TTL/3 via CAS and demotes on CAS conflict / consecutive
 * transport errors; a non-holder (candidate) polls otsardb_lease_claim every
 * ~2 s and, when the store proves the foreign lease expired, takes it over,
 * re-runs recovery to HEAD and re-anchors the publisher generation fence.
 *
 * Lifetimes: the thread only touches the store, the local execution files
 * and the publisher; destroy_session stops and joins it BEFORE tearing any
 * of those down, so the thread can never leak or outlive its session. */
struct s3_database_session {
    otsardb_object_store *store = nullptr;
    otsardb_local_vfs_instance *vfs = nullptr;
    otsardb_wal_publisher publisher{};
    /* Current writer lease, or null while this session is a demoted /
     * candidate session (publish fence returns 0). Published by the lease
     * thread on takeover and by the open path; read by the fence. */
    std::atomic<otsardb_lease *> lease{nullptr};
    /* Leases retired by the lease thread (lost / self-demoted / failed
     * takeover). Freed only in destroy_session AFTER the thread is joined,
     * so a fence that already loaded the pointer never sees it freed. */
    std::vector<otsardb_lease *> retired_leases;
    /* Lazy hydration: when enabled (default), the cold-open
     * flow validates the manifest chain and materialises only the newest
     * checkpoint base image; every other segment stays lazy and is fetched,
     * verified and applied on VFS page-read misses. The hydrator is created
     * at open, rebuilt in place on lease takeover, and destroyed at close. */
    otsardb_lazy_hydrator *hydrator = nullptr;
    bool lazy_enabled = false;
    /* (S3-1/E4 follow-up): whether the lazy-hydration VFS
     * page hooks are installed on session->vfs. The hooks are installed at
     * open only when a hydrator exists then — a HEAD-less open creates
     * none — so the swap paths (takeover, FRESH rematerialisation) must
     * install them when they create the hydrator for a session that opened
     * headless, or every read would bypass hydration and serve the stale
     * pre-swap file. */
    int page_hooks_installed = 0;
    /* read-replica session (OTSARDB_S3_READ_FROM_REPLICA=1).
     * The session's recovery/read path uses the REPLICA destination store
     * (OTSARDB_S3_REPLICA_*) instead of the primary. The session is READ-ONLY:
     * it never claims a lease, never publishes (a permanently closed lease
     * fence refuses every publish with SQLITE_BUSY), and the SQLite
     * connection runs under PRAGMA query_only=ON so any DML fails with
     * SQLITE_READONLY. destroy_session skips the checkpoint/GC writes (a
     * reader must never write the store it reads from). */
    bool read_replica = false;
    /* session-level promotion (OTSARDB_S3_PROMOTE=1 at open,
     * 0077 Next item 2). `promoted` is set after the store swap; the
     * promoted destination becomes session->store (this session's primary)
     * and `old_primary_store` is the store the OLD primary wrote to, now
     * attached as this session's REPLICA destination (borrowed by the
     * publisher's replica handle — the session still owns and destroys it). */
    bool promoted = false;
    otsardb_object_store *old_primary_store = nullptr;
    /* at-rest segment encryption cipher, created at open
     * from OTSARDB_S3_ENC_KEY (64 hex chars). NULL = plaintext path (default).
     * Registered on the session thread (otsardb_cipher_thread_set) so the WAL
     * publisher's otsardb_publish_commit encrypts segments; passed explicitly
     * to the lazy/eager restore paths (including the lease thread takeover).
     * Destroyed (key zeroised) in destroy_session. */
    otsardb_cipher *cipher = nullptr;
    /* (FRESH consistency mode): OTSARDB_CONSISTENCY=fresh
     * installs a per-statement pre-exec hook (otsardb_exec) that reads the
     * remote HEAD once per statement and re-materialises the session image
     * when the generation advanced. fresh_anchor_generation is the
     * generation the CURRENT local image represents (open head, takeover
     * head, last fresh swap, or this session's own published commit) —
     * the comparison anchor of the check. snapshot (default) sessions have
     * fresh_mode == 0 and never install the hook (byte-identical behavior).
     * Atomic: written by the CLI thread (hook) AND the lease thread
     * (takeover). */
    int fresh_mode = 0;
    std::atomic<uint64_t> fresh_anchor_generation{0};
    /* (audit finding S3-2): the commit_id of the head the
     * anchor generation was set at. The anchor is the (generation,
     * commit_id) pair, NOT generation alone: a same-generation HEAD
     * replacement (different commit_id at the same generation — a restored
     * or corrupted single-writer store) must trigger re-materialisation,
     * or the session keeps silently serving the old branch's snapshot.
     * Guarded by sql_mutex exactly like fresh_anchor_generation (all reads
     * happen under the exec lock; all writes happen under sql_mutex). */
    char fresh_anchor_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
    /* Test/internal hook (otsardb_s3_open_with_store): the session BORROWS
     * a caller-provided store (memory/fault store tests). destroy_session
     * must not destroy a borrowed store — the caller owns its lifetime. */
    int store_borrowed = 0;
    std::string local_path;
    std::string instance_id;
    uint64_t lease_ttl_secs = 0;
    std::atomic<int> lease_stop{0};
    /* SQLite handle of the open database, used on takeover to reset the
     * page cache so pre-takeover cached pages cannot leak into commits. */
    otsardb *db = nullptr;
    /* M2 phase 3 HTTP write forwarding:
     * - forward_server: the leader's embedded HTTP server (only when the
     * session was opened with a forward_port); its thread is joined in
     * destroy_session BEFORE the lease thread and SQLite teardown.
     * - forward_port / forward_to: the CLI-supplied configuration.
     * - leader_address: the lease-derived "host:port" of the CURRENT
     * holder, refreshed by the lease thread on every candidate poll
     * (empty when this session is the leader or the holder publishes
     * no address). Guarded by leader_mutex.
     * - sql_mutex: serializes the forward server's sqlite3_exec against
     * nothing else today (the CLI is single-threaded), kept as
     * defensive serialization for the handler. */
    otsardb_forward_server *forward_server = nullptr;
    int forward_port = 0;
    std::string forward_to;
    std::mutex leader_mutex;
    std::string leader_address;
    /* (E4): RECURSIVE because otsardb_exec now holds this
     * lock around the whole statement (serialising it against the
     * lease-thread takeover and the forward-server handler) and the FRESH
     * pre-exec hook re-enters it for the re-materialisation swap. */
    std::recursive_mutex sql_mutex;
#if defined(_WIN32)
    uintptr_t lease_thread = 0;
#else
    pthread_t lease_thread{};
    bool lease_thread_running = false;
#endif
};

static void set_error(const std::string &message) {
    g_last_error = message;
}

/* M2 lease fence installed on the WAL publisher: a publish is refused with
 * SQLITE_BUSY unless this session currently holds a live lease. In
 * candidate mode (lease not yet claimed) or after demotion the fence is
 * permanently closed; the lease thread flips it open on takeover. */
static int session_lease_fence(void *ctx) {
    auto *session = static_cast<s3_database_session *>(ctx);
    if (!session) return 0;
    otsardb_lease *lease = session->lease.load(std::memory_order_acquire);
    return lease && otsardb_lease_is_live(lease) == 1;
}

/* (E4): otsardb_exec lock/unlock callbacks registered via
 * otsardb_internal_set_exec_lock. They take the SAME sql_mutex the lease
 * thread (takeover), the FRESH pre-exec hook and the forward server hold,
 * so a statement and an image swap can never interleave. */
static void session_exec_lock(void *ctx) {
    auto *session = static_cast<s3_database_session *>(ctx);
    if (session) session->sql_mutex.lock();
}

static void session_exec_unlock(void *ctx) {
    auto *session = static_cast<s3_database_session *>(ctx);
    if (session) session->sql_mutex.unlock();
}

/* Lazy-hydration VFS page hooks: the hydrate hook runs
 * before every main-db xRead and fails closed (SQLITE_IOERR_READ) on any
 * fetch/verify/apply failure; mark/truncate report SQLite's own main-db
 * writes so the hydrator never re-applies older segment data on top. */
static int session_page_hydrate(void *ctx, sqlite3_int64 offset, int amount) {
    auto *hydrator = static_cast<otsardb_lazy_hydrator *>(ctx);
    return otsardb_lazy_hydrate_bytes(hydrator, offset, amount) ? SQLITE_OK
                                                              : SQLITE_IOERR_READ;
}

static void session_page_mark(void *ctx, sqlite3_int64 offset, int amount) {
    otsardb_lazy_mark_bytes(static_cast<otsardb_lazy_hydrator *>(ctx), offset, amount);
}

static void session_page_truncate(void *ctx, sqlite3_int64 new_size) {
    otsardb_lazy_clear_beyond_bytes(static_cast<otsardb_lazy_hydrator *>(ctx), new_size);
}

/* install the lazy-hydration VFS page hooks when a hydrator
 * exists and they are not installed yet. A session that opened on a
 * HEAD-less store has NEITHER (the open-time hook installation is gated on
 * session->hydrator); the swap paths (lease takeover, FRESH
 * rematerialisation) create the hydrator later and must install the hooks
 * too, or every read bypasses hydration and serves the stale pre-swap
 * file. Mirrors the open-time installation (s3_database.cpp open tail).
 * Returns 1 on success (already installed counts). */
static int session_ensure_page_hooks(s3_database_session *session) {
    if (!session->hydrator || session->page_hooks_installed) return 1;
    if (otsardb_local_vfs_instance_set_page_hooks(session->vfs,
                                                 session_page_hydrate,
                                                 session_page_mark,
                                                 session_page_truncate,
                                                 session->hydrator) != SQLITE_OK) {
        return 0;
    }
    session->page_hooks_installed = 1;
    return 1;
}

/* "127.0.0.1:<port>" of this session's forward server, or "" when the
 * session runs no server (no forwarding published in the lease). */
static std::string session_forward_address(const s3_database_session *session) {
    if (session->forward_server) {
        char address[OTSARDB_LEASE_ADDRESS_MAX + 1];
        std::snprintf(address,
                      sizeof(address),
                      "127.0.0.1:%d",
                      otsardb_forward_server_port(session->forward_server));
        return address;
    }
    return "";
}

/* Forward server request context: accumulates rendered result rows. */
struct forward_row_ctx {
    char *out;
    size_t cap;
    size_t used;
    int rows;
};

static int forward_row_callback(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    auto *row_ctx = static_cast<forward_row_ctx *>(ctx);
    otsardb_forward_row_line(row_ctx->out, row_ctx->cap, &row_ctx->used, argc, argv);
    row_ctx->rows += 1;
    return 0;
}

/* M2 phase 3: the leader-side handler for POST /forward.
 * Executes the forwarded SQL on THIS session's own SQLite connection
 * (full WAL-publisher durability path). Refuses with 421 when the session
 * is not a live lease holder (demoted mid-request or candidate), so a
 * forwarder falls back to the local BUSY error instead of silently
 * executing against a stale snapshot. */
static int forward_server_handler(void *ctx,
                                  const char *sql,
                                  size_t sql_len,
                                  char *out,
                                  size_t cap) {
    auto *session = static_cast<s3_database_session *>(ctx);
    if (out) out[0] = '\0';
    if (!session) {
        if (out) std::snprintf(out, cap, "database not open");
        return 500;
    }
    std::lock_guard<std::recursive_mutex> lock(session->sql_mutex);
    otsardb_lease *lease = session->lease.load(std::memory_order_acquire);
    if (!lease || otsardb_lease_is_live(lease) != 1) {
        if (out) std::snprintf(out, cap, "not leader");
        return 421;
    }
    sqlite3 *sqlite = otsardb_internal_sqlite_handle(session->db);
    if (!sqlite) {
        if (out) std::snprintf(out, cap, "database not open");
        return 500;
    }
    std::string statement(sql, sql_len);
    forward_row_ctx row_ctx{out, cap, 0, 0};
    char *error = nullptr;
    int rc = sqlite3_exec(sqlite, statement.c_str(), forward_row_callback, &row_ctx, &error);
    if (rc != SQLITE_OK) {
        if (rc == SQLITE_BUSY || rc == SQLITE_BUSY_SNAPSHOT) {
            /* The lease was lost between the liveness check and the commit
             * (takeover raced the request): refuse like a demoted leader. */
            std::snprintf(out, cap, "not leader");
            sqlite3_free(error);
            return 421;
        }
        std::snprintf(out, cap, "%s", error ? error : "SQL error");
        sqlite3_free(error);
        return 500;
    }
    otsardb_forward_end_marker(out, cap, &row_ctx.used, static_cast<size_t>(row_ctx.rows));
    return 200;
}

/* Sleep in 100 ms slices, bailing out as soon as the stop flag is set.
 * The lease thread is joined with an infinite wait at session close, so a
 * single long sleep (TTL/3 = 10 s by default) would add ~10 s of latency
 * to every CLI invocation and every session close. */
static void lease_sleep_ms(int ms, const std::atomic<int> *stop) {
    if (ms < 0) ms = 0;
#if defined(_WIN32)
    const ULONGLONG deadline = GetTickCount64() + static_cast<DWORD>(ms);
    while (true) {
        if (stop && stop->load(std::memory_order_relaxed) != 0) return;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return;
        const ULONGLONG remaining = deadline - now;
        Sleep(static_cast<DWORD>(remaining < 100 ? remaining : 100));
    }
#else
    while (ms > 0) {
        if (stop && stop->load(std::memory_order_relaxed) != 0) return;
        const int chunk = ms > 100 ? 100 : ms;
        struct timespec ts = {};
        ts.tv_sec = chunk / 1000;
        ts.tv_nsec = static_cast<long>(chunk % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
        ms -= chunk;
    }
#endif
}

static void remove_execution_files(const std::string &base);
static void remove_execution_sidecars(const std::string &base);
#if !defined(_WIN32)
static int copy_file_over_in_place(const std::string &src, const std::string &dst);
#endif

/* Called by the lease thread right after a successful takeover claim:
 * discard the stale local execution image, re-materialise it at the
 * current remote HEAD, and re-anchor the publisher's 0024 generation
 * fence. The fence only flips live AFTER this returns, so no publish can
 * base pages on the pre-takeover state; any sync that slips in during
 * recovery still fails closed (stale local_image_generation -> BUSY).
 *
 * the whole sequence runs under sql_mutex so it cannot
 * interleave with a FRESH-mode per-statement re-materialisation (which
 * manipulates the same execution files, hydrator and publisher anchor)
 * or a forwarded statement; a successful takeover also re-anchors the
 * FRESH comparison anchor to the takeover head.
 *
 * (S3-1): the takeover now WAL-neutralises the connection
 * FIRST, exactly like the FRESH rematerialisation path: PRAGMA
 * wal_checkpoint(TRUNCATE) flushes this session's committed frames into
 * the base file that is about to be discarded and truncates the -wal, so
 * no stale pre-takeover frame can be replayed on top of the fresh base.
 * Without it, on Windows the -wal/-shm remove()s below fail on the open
 * handles, the pre-takeover WAL survives the image swap, and the open
 * SQLite connection can replay its old frames over the new image (a mixed
 * old-frames + new-base image served silently by the session after the
 * takeover). The two swap paths are now symmetric. */
static int session_takeover_commit(s3_database_session *session) {
    std::lock_guard<std::recursive_mutex> lock(session->sql_mutex);
    /* Neutralise the doomed WAL before the image swap (S3-1, same block as
     * session_fresh_rematerialize). Best-effort: with no open transaction
     * and a quiescent connection (the sql_mutex serialises against every
     * statement) this succeeds; a failure leaves the stale-WAL hazard
     * unchanged, so the restore below must still be treated as the
     * authoritative state (the restore path is unchanged). */
    sqlite3 *pre_swap_sqlite =
        session->db ? otsardb_internal_sqlite_handle(session->db) : nullptr;
    if (pre_swap_sqlite) {
        char *checkpoint_error = nullptr;
        sqlite3_exec(pre_swap_sqlite,
                     "PRAGMA wal_checkpoint(TRUNCATE);",
                     nullptr,
                     nullptr,
                     &checkpoint_error);
        sqlite3_free(checkpoint_error);
    }
    otsardb_head head{};
    char *head_etag = nullptr;
    otsardb_journal_result head_rc = otsardb_journal_read_head(session->store,
                                                           session->publisher.database_root,
                                                           &head,
                                                           &head_etag);
    std::free(head_etag);
    if (head_rc == OTSARDB_JOURNAL_OK) {
        if (std::strcmp(head.database_id, session->publisher.database_id) != 0) {
            std::fprintf(stderr,
                         "otsardb: lease takeover: remote database identity mismatch; "
                         "staying in candidate mode\n");
            return 0;
        }
        /* on POSIX keep the main image file (its inode is
         * the one the SQLite connection reads through) and only discard the
         * sidecars; the complete-image fallback is staged and copied over
         * the live file in place below. On Windows the pre-0127 behavior is
         * unchanged (the remove fails on the open handle; the restore is a
         * same-file rewrite). */
#if defined(_WIN32)
        remove_execution_files(session->local_path);
#else
        remove_execution_sidecars(session->local_path);
#endif
        int restore_ok = 0;
        if (session->lazy_enabled) {
            /* Lazy hydration: re-validate the chain at the takeover head and
             * rebuild the hydrator in place (the local image is discarded
             * and re-materialised from the newest checkpoint base; the page
             * hooks keep serving on-demand fetches from the NEW index). */
            restore_ok = otsardb_lazy_restore_store_ex(session->store,
                                                     session->publisher.database_root,
                                                     session->local_path.c_str(),
                                                     NULL,
                                                     &session->hydrator,
                                                     session->cipher);
        } else {
#if defined(_WIN32)
            restore_ok = otsardb_recovery_restore_store_ex(session->store,
                                                          session->publisher.database_root,
                                                          session->local_path.c_str(),
                                                          session->cipher);
#else
            /* the full restore internally remove()s its
             * destination — stage it, then copy the verified bytes over the
             * live file in place so the open connection's fd keeps reading
             * the swapped image. */
            const std::string stage_path = session->local_path + ".takeoverstage";
            std::error_code stage_ec;
            std::filesystem::remove(stage_path, stage_ec);
            restore_ok = otsardb_recovery_restore_store_ex(session->store,
                                                          session->publisher.database_root,
                                                          stage_path.c_str(),
                                                          session->cipher);
            if (restore_ok && !copy_file_over_in_place(stage_path, session->local_path)) {
                restore_ok = 0;
            }
            std::filesystem::remove(stage_path, stage_ec);
#endif
        }
        if (!restore_ok) {
            std::fprintf(stderr,
                         "otsardb: lease takeover: recovery to HEAD failed; staying "
                         "in candidate mode\n");
            return 0;
        }
        /* a session that opened HEAD-less has no hydration
         * hooks (the open-time installation is gated on an existing
         * hydrator); the takeover just created the hydrator, so install the
         * hooks now — otherwise every read bypasses hydration and serves
         * the stale pre-takeover file. A hook-install failure fails the
         * takeover closed (staying candidate is always safe). */
        if (!session_ensure_page_hooks(session)) {
            std::fprintf(stderr,
                         "otsardb: lease takeover: could not install the lazy-"
                         "hydration page hooks; staying in candidate mode\n");
            return 0;
        }
        otsardb_wal_publisher_set_local_image_generation(&session->publisher,
                                                       head.generation);
        if (session->fresh_mode) {
            session->fresh_anchor_generation.store(head.generation,
                                                   std::memory_order_relaxed);
            std::snprintf(session->fresh_anchor_commit_id,
                          sizeof(session->fresh_anchor_commit_id),
                          "%s",
                          head.commit_id);
        }
    } else if (head_rc != OTSARDB_JOURNAL_NOT_FOUND) {
        std::fprintf(stderr,
                     "otsardb: lease takeover: failed to read HEAD; staying in "
                     "candidate mode\n");
        return 0;
    }

    /* Drop SQLite's page cache so pages cached before the takeover cannot
     * be written into the next commit. Best-effort: SQLite only clears the
     * cache while no transaction is active (SQLITE_FCNTL_RESET_CACHE). */
    if (session->db) {
        sqlite3 *sqlite = otsardb_internal_sqlite_handle(session->db);
        if (sqlite) {
            int cache_rc =
                sqlite3_file_control(sqlite, "main", SQLITE_FCNTL_RESET_CACHE, nullptr);
            if (cache_rc != SQLITE_OK) {
                std::fprintf(stderr,
                             "otsardb: lease takeover: could not reset the SQLite page "
                             "cache (rc=%d); stale cached pages will surface "
                             "SQLITE_BUSY until the connection is reused\n",
                             cache_rc);
            }
        }
    }
    return 1;
}

/* Forward declarations of the env helpers (defined below the lease thread);
 * the FRESH-mode block uses them. */
static const char *env_nonempty(const char *name);
static bool env_true(const char *name);
static std::string cache_dir_path(void);
/* (E3): test-only fault hook for the FRESH page-cache reset.
 * The bundled SQLite amalgamation always reports SQLITE_OK for
 * SQLITE_FCNTL_RESET_CACHE, so the fail-closed reset branch is exercised
 * deterministically through this hook (OTSARDB_TEST_FAIL_RESET_CACHE=1). */
static bool fresh_test_fail_reset_cache(void);

/* ---------------------------------------------------------------------------
 * — FRESH consistency mode (OTSARDB_CONSISTENCY=fresh).
 *
 * The fresh pre-exec hook runs at the TOP of every otsardb_exec (installed
 * via otsardb_internal_set_pre_exec_hook; snapshot sessions never install
 * it, so their behavior is byte-identical). It performs ONE remote HEAD GET
 * per statement (bounded 0065 retry on transient failure, fail-closed on
 * final failure) and re-materialises the local image only when the remote
 * generation advanced beyond this session's anchor. The re-materialisation
 * reuses the SAME ladder as the open path (exact-match cache probe ->
 * /0097 delta over the cached image -> lazy/full fallback)
 * and the SAME takeover re-anchoring pattern (discard execution files,
 * restore into the SAME path the SQLite connection keeps open, re-anchor
 * the publisher's local-image generation fence, reset SQLite's page cache).
 * The connection is quiescent when the hook runs (no statement in flight;
 * an open transaction defers the check — the snapshot stays pinned inside
 * a transaction, preserving ACID).
 * ------------------------------------------------------------------------- */

/* Re-materialise the session's execution image at `head` (the takeover
 * pattern). Caller contract: sql_mutex held, connection quiescent, no
 * open transaction. Returns 1 on success; 0 leaves the session usable at
 * its previous generation (fail-closed: the caller aborts the statement). */
static int session_fresh_rematerialize(s3_database_session *session, const otsardb_head *head) {
    sqlite3 *sqlite = session->db ? otsardb_internal_sqlite_handle(session->db) : nullptr;
    if (!head || head->generation == 0) return 0;

    /* Neutralise the doomed WAL before the image swap: checkpoint(TRUNCATE)
     * flushes this session's committed frames into the base file that is
     * about to be discarded and truncates the -wal so no stale frame can be
     * replayed on top of the fresh image (the same hazard class the
     * takeover path documents; the truncate makes it impossible instead of
     * relying on frame-content coincidence). Best-effort: with no open
     * transaction and a quiescent connection this succeeds; a failure
     * leaves the session readable at the old generation (fail-closed
     * handled by the caller through the restore below). */
    if (sqlite) {
        char *checkpoint_error = nullptr;
        sqlite3_exec(sqlite,
                     "PRAGMA wal_checkpoint(TRUNCATE);",
                     nullptr,
                     nullptr,
                     &checkpoint_error);
        sqlite3_free(checkpoint_error);
    }

    /* on POSIX keep the main image file (its inode is the
     * one the SQLite connection reads through) and only discard the
     * sidecars. On Windows the pre-0127 behavior is unchanged (the remove
     * fails on the open handle; the restore is a same-file rewrite). */
#if defined(_WIN32)
    remove_execution_files(session->local_path);
#else
    remove_execution_sidecars(session->local_path);
#endif

    /* Materialisation ladder — the SAME order and SAME APIs as the open
     * path (otsardb_s3_open_target_ex): exact-match cache probe (cached
     * image already at head), delta over the cached image (0094/0097), then
     * lazy/full fallback. A complete image (probe/delta/full) needs no
     * hydrator; only the lazy path keeps (rebuilds) one.
     *
     * on POSIX the complete-image branches internally
     * remove()+rewrite their destination — they write a staging sidecar and
     * the verified bytes are then copied over the live file in place below.
     * The lazy branch writes the live path directly (inode-preserving). */
#if defined(_WIN32)
    const std::string swap_dest = session->local_path;
#else
    const std::string swap_dest = session->local_path + ".freshstage";
    std::error_code stage_ec;
    std::filesystem::remove(swap_dest, stage_ec);
#endif
    const char *reuse_env = env_nonempty("OTSARDB_CACHE_REUSE");
    bool cache_reuse = !reuse_env || env_true("OTSARDB_CACHE_REUSE");
    bool restore_ok = false;
    bool complete_image = false;

    if (cache_reuse) {
        otsardb_local_cache_state remote_state{};
        remote_state.generation = head->generation;
        std::snprintf(remote_state.commit_id,
                      sizeof(remote_state.commit_id),
                      "%s",
                      head->commit_id);
        std::snprintf(remote_state.commit_sha256,
                      sizeof(remote_state.commit_sha256),
                      "%s",
                      head->commit_sha256);
        restore_ok =
            otsardb_local_cache_probe(cache_dir_path().c_str(),
                                    session->publisher.database_id,
                                    &remote_state,
                                    swap_dest.c_str()) != 0;
        complete_image = restore_ok;
        if (!restore_ok) {
            otsardb_local_cache_state cached{};
            if (otsardb_local_cache_read_state(cache_dir_path().c_str(),
                                             session->publisher.database_id,
                                             &cached) != 0 &&
                cached.generation > 0 &&
                cached.generation < head->generation) {
                std::string cached_img = cache_dir_path() + "/otsardb-cache-v1/" +
                                         session->publisher.database_id + "/state.db";
                otsardb_head delta_head{};
                restore_ok =
                    otsardb_recovery_materialize_delta_ex(
                        session->store,
                        session->publisher.database_root,
                        cached_img.c_str(),
                        cached.generation,
                        cached.commit_sha256,
                        swap_dest.c_str(),
                        &delta_head,
                        session->cipher) != 0;
                complete_image = restore_ok;
            }
        }
    }
    if (!restore_ok) {
        if (session->lazy_enabled) {
            /* In-place hydrator rebuild (same struct, hooks stay valid).
             * Targets the LIVE path directly: the hydrator's file writes
             * preserve the inode the connection reads through. */
            restore_ok = otsardb_lazy_restore_store_ex(session->store,
                                                     session->publisher.database_root,
                                                     session->local_path.c_str(),
                                                     head,
                                                     &session->hydrator,
                                                     session->cipher) != 0;
        } else {
            restore_ok = otsardb_recovery_restore_store_ex(session->store,
                                                         session->publisher.database_root,
                                                         swap_dest.c_str(),
                                                         session->cipher) != 0;
            complete_image = restore_ok;
        }
    }
#if !defined(_WIN32)
    /* publish the staged complete image INTO the live file
     * (same inode). A copy failure fails the swap closed — the caller
     * aborts the statement and the session keeps serving the previous
     * generation. The lazy branch wrote the live file directly. */
    if (restore_ok && complete_image) {
        if (!copy_file_over_in_place(swap_dest, session->local_path)) {
            restore_ok = 0;
            complete_image = false;
        }
    }
    std::filesystem::remove(swap_dest, stage_ec);
#endif
    if (!restore_ok) return 0;

    /* a session that opened HEAD-less has no hydration
     * hooks; the lazy branch above just created the hydrator, so install
     * the hooks now (symmetric with the takeover path). A failure fails
     * the swap closed — the statement is aborted, never a stale read. */
    if (!session_ensure_page_hooks(session)) return 0;

    if (complete_image && session->hydrator) {
        /* A complete image is served with no hydration: retire the hydrator
         * and remove the VFS page hooks so no stale lazy index can apply
         * older segment data over the fresh image. */
        otsardb_lazy_destroy(session->hydrator);
        session->hydrator = nullptr;
        otsardb_local_vfs_instance_set_page_hooks(session->vfs,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                nullptr);
        session->page_hooks_installed = 0;
    }

    /* Takeover re-anchor: the local image now represents head.generation.
     * The publisher's cached HEAD (current_generation) is deliberately left
     * stale — the next write sync's refresh_head then adopts the chain
     * change with the full bookkeeping (page-map clear, local-image proof
     * drop, rolling-checksum resurrection). */
    otsardb_wal_publisher_set_local_image_generation(&session->publisher,
                                                   head->generation);

    if (sqlite) {
        int cache_rc =
            sqlite3_file_control(sqlite, "main", SQLITE_FCNTL_RESET_CACHE, nullptr);
        /* (E3): a page-cache reset that cannot be established
         * must NOT let the next statement run on a possibly-stale cache.
         * Fail the swap closed — the caller aborts the statement and the
         * session keeps its previous anchor (the next statement re-attempts
         * the re-materialisation). The bundled SQLite amalgamation returns
         * SQLITE_OK even when the reset silently no-ops inside a transaction,
         * so the fail-closed branch is additionally armed by the documented
         * test-only hook OTSARDB_TEST_FAIL_RESET_CACHE for the deterministic
         * regression (it must never be set in production). */
        if (cache_rc != SQLITE_OK || fresh_test_fail_reset_cache()) {
            std::fprintf(stderr,
                         "otsardb: fresh mode: could not reset the SQLite page cache "
                         "(rc=%d); failing the statement because the page cache may "
                         "still hold pages of the previous generation\n",
                         cache_rc);
            return 0;
        }
    }
    std::fprintf(stderr,
                 "otsardb: fresh mode: rematerialized at generation %llu (%s)\n",
                 static_cast<unsigned long long>(head->generation),
                 complete_image ? "complete image" : "lazy hydration");
    return 1;
}

/* One HEAD read attempt of the 0065 bounded retry cycle. */
struct fresh_head_attempt_ctx {
    s3_database_session *session;
    otsardb_head head;
    char *etag;
};

static int fresh_head_attempt_once(void *opaque, otsardb_journal_result *out_rc) {
    auto *ctx = static_cast<fresh_head_attempt_ctx *>(opaque);
    std::free(ctx->etag);
    ctx->etag = nullptr;
    otsardb_journal_result rc = otsardb_journal_read_head(ctx->session->store,
                                                       ctx->session->publisher.database_root,
                                                       &ctx->head,
                                                       &ctx->etag);
    *out_rc = rc;
    return rc == OTSARDB_JOURNAL_OK || rc == OTSARDB_JOURNAL_NOT_FOUND;
}

/* FRESH pre-exec hook: one HEAD GET per otsardb_exec. */
static int session_fresh_pre_exec(void *opaque, char **error_message) {
    auto *session = static_cast<s3_database_session *>(opaque);
    if (!session) return OTSARDB_ERROR;

    /* Inside an explicit transaction the snapshot stays pinned (ACID): the
     * swap must never interleave with an open transaction. The first
     * statement AFTER the transaction ends re-checks and adopts. */
    sqlite3 *sqlite = session->db ? otsardb_internal_sqlite_handle(session->db) : nullptr;
    if (sqlite && sqlite3_get_autocommit(sqlite) == 0) {
        return OTSARDB_OK;
    }

    fresh_head_attempt_ctx ctx{session, {}, nullptr};
    otsardb_journal_result rc = OTSARDB_JOURNAL_ERROR;
    otsardb_wal_publisher_retry_cycle(fresh_head_attempt_once, &ctx, &rc);
    std::free(ctx.etag);

    if (rc == OTSARDB_JOURNAL_NOT_FOUND) {
        /* No HEAD yet: nothing newer to adopt. */
        return OTSARDB_OK;
    }
    if (rc != OTSARDB_JOURNAL_OK) {
        /* Fail-closed: fresh mode promises the latest state; a HEAD read
         * that fails after the 0065 retry budget aborts the statement. */
        set_error("FRESH consistency mode: the remote HEAD could not be read after "
                  "the configured retry budget (OTSARDB_PUBLISH_RETRIES); the "
                  "statement was aborted because fresh mode promises the latest "
                  "state");
        if (error_message) *error_message = sqlite3_mprintf("%s", g_last_error.c_str());
        return OTSARDB_ERROR;
    }
    if (std::strcmp(ctx.head.database_id, session->publisher.database_id) != 0) {
        set_error("FRESH consistency mode: remote database identity mismatch; "
                  "the statement was aborted");
        if (error_message) *error_message = sqlite3_mprintf("%s", g_last_error.c_str());
        return OTSARDB_ERROR;
    }

    /* (S3-2): the anchor is the (generation, commit_id)
     * pair. A same-generation HEAD replacement (same generation, different
     * commit_id) must re-materialise — comparing generation alone would let
     * a restored/corrupted single-writer store's replaced head slip through
     * and the session would keep serving the old branch's snapshot. */
    const uint64_t anchor_generation =
        session->fresh_anchor_generation.load(std::memory_order_relaxed);
    if (ctx.head.generation == anchor_generation &&
        std::strcmp(ctx.head.commit_id, session->fresh_anchor_commit_id) == 0) {
        return OTSARDB_OK;
    }
    if (ctx.head.generation < anchor_generation) {
        /* Out-of-band HEAD regression (unsupported per CACHE_COHERENCE):
         * keep serving the pinned snapshot rather than chasing backwards. */
        return OTSARDB_OK;
    }

    /* The publisher already adopted this generation (this session's own
     * published commit: read-your-writes) — the local image + WAL contain
     * the state; adopt the anchor without re-materialising. 
     * (S3-2): the commit_id must match the publisher's cached head too — a
     * same-generation HEAD replacement would otherwise be swallowed here. */
    if (session->publisher.head_loaded &&
        session->publisher.current_generation == ctx.head.generation &&
        std::strcmp(session->publisher.current_commit_id, ctx.head.commit_id) == 0 &&
        session->publisher.local_image_generation_known &&
        session->publisher.local_image_generation == ctx.head.generation) {
        session->fresh_anchor_generation.store(ctx.head.generation,
                                               std::memory_order_relaxed);
        std::snprintf(session->fresh_anchor_commit_id,
                      sizeof(session->fresh_anchor_commit_id),
                      "%s",
                      ctx.head.commit_id);
        return OTSARDB_OK;
    }

    /* External advance: re-materialise (serialised against the lease-thread
     * takeover and the forward-server handler via sql_mutex). Recursive:
     * otsardb_exec already holds the lock for the whole statement (0134). */
    {
        std::lock_guard<std::recursive_mutex> lock(session->sql_mutex);
        /* Re-check under the lock: a takeover may have just rebuilt the
         * image at this very head. */
        if (ctx.head.generation ==
                session->fresh_anchor_generation.load(std::memory_order_relaxed) &&
            std::strcmp(ctx.head.commit_id, session->fresh_anchor_commit_id) == 0) {
            return OTSARDB_OK;
        }
        if (!session_fresh_rematerialize(session, &ctx.head)) {
            set_error("FRESH consistency mode: re-materialisation to the latest "
                      "generation failed; the statement was aborted (a wrong or "
                      "partial image is never served)");
            if (error_message) {
                *error_message = sqlite3_mprintf("%s", g_last_error.c_str());
            }
            return OTSARDB_ERROR;
        }
        session->fresh_anchor_generation.store(ctx.head.generation,
                                               std::memory_order_relaxed);
        std::snprintf(session->fresh_anchor_commit_id,
                      sizeof(session->fresh_anchor_commit_id),
                      "%s",
                      ctx.head.commit_id);
    }
    return OTSARDB_OK;
}

#if defined(_WIN32)
static unsigned int __stdcall lease_thread_main(void *arg) {
#else
static void *lease_thread_main(void *arg) {
#endif
    auto *session = static_cast<s3_database_session *>(arg);
    const int poll_interval_ms = 2000;
    int renew_interval_ms =
        session->lease_ttl_secs > 3 ? (int)(session->lease_ttl_secs / 3) : 1;
    renew_interval_ms *= 1000;
    if (renew_interval_ms < 1000) renew_interval_ms = 1000;
    int transport_errors = 0;

    while (session->lease_stop.load(std::memory_order_relaxed) == 0) {
        otsardb_lease *held = session->lease.load(std::memory_order_acquire);
        if (held) {
            /* Leader mode: renew via CAS If-Match at the same generation. */
            int rc = otsardb_lease_renew(held);
            if (rc == 1) {
                /* CAS conflict: another writer took the lease over. Demote
                 * permanently; the publish fence (is_live) is now closed. */
                std::fprintf(stderr,
                             "otsardb: writer lease lost (CAS conflict) for %s; "
                             "demoting this session to candidate\n",
                             session->instance_id.c_str());
                session->lease.store(nullptr, std::memory_order_release);
                session->retired_leases.push_back(held);
                transport_errors = 0;
                lease_sleep_ms(poll_interval_ms, &session->lease_stop);
            } else if (rc == 2) {
                /* Transport/store error: after 3 consecutive failures
                 * self-demote so a healthier writer can take over once the
                 * lease expires. */
                if (++transport_errors >= 3) {
                    std::fprintf(stderr,
                                 "otsardb: writer lease renewal failed with 3 "
                                 "consecutive transport errors for %s; "
                                 "self-demoting this session to candidate\n",
                                 session->instance_id.c_str());
                    session->lease.store(nullptr, std::memory_order_release);
                    session->retired_leases.push_back(held);
                    transport_errors = 0;
                    lease_sleep_ms(poll_interval_ms, &session->lease_stop);
                } else {
                    lease_sleep_ms(renew_interval_ms, &session->lease_stop);
                }
            } else {
                transport_errors = 0;
                lease_sleep_ms(renew_interval_ms, &session->lease_stop);
            }
        } else {
            /* Candidate mode: poll the lease. otsardb_lease_claim only
             * overwrites a foreign lease when the store proves it expired
             * (Last-Modified + TTL - skew); a live lease stays HELD. */
            uint64_t epoch = 0;
            int claim_state = OTSARDB_LEASE_ERROR;
            char holder[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
            char holder_address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
            std::string self_address = session_forward_address(session);
            otsardb_lease *claimed = otsardb_lease_claim(session->store,
                                                     session->publisher.database_root,
                                                     session->instance_id.c_str(),
                                                     self_address.empty() ? nullptr
                                                                          : self_address.c_str(),
                                                     session->lease_ttl_secs,
                                                     &epoch,
                                                     &claim_state,
                                                     holder,
                                                     sizeof(holder),
                                                     holder_address,
                                                     sizeof(holder_address));
            if (claimed) {
                std::fprintf(stderr,
                             "otsardb: writer lease takeover succeeded for %s at "
                             "generation %llu (previous owner %s); re-running "
                             "recovery to HEAD\n",
                             session->instance_id.c_str(),
                             static_cast<unsigned long long>(epoch),
                             holder[0] ? holder : "unknown");
                {
                    std::lock_guard<std::mutex> leader_lock(session->leader_mutex);
                    /* We ARE the leader now: no forwarding target. */
                    session->leader_address.clear();
                }
                if (session_takeover_commit(session)) {
                    session->lease.store(claimed, std::memory_order_release);
                    transport_errors = 0;
                    /* Fence now live: the next WAL sync publishes. */
                } else {
                    session->retired_leases.push_back(claimed);
                }
            } else {
                /* HELD (or transient ERROR): remember the holder's published
                 * forward address so the CLI can route writes to it. A poll
                 * that fails to read the lease clears the address (unknown
                 * leader -> fail closed, no forwarding). */
                std::lock_guard<std::mutex> leader_lock(session->leader_mutex);
                session->leader_address =
                    claim_state == OTSARDB_LEASE_HELD ? holder_address : "";
            }
            lease_sleep_ms(poll_interval_ms, &session->lease_stop);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return nullptr;
#endif
}

static void session_lease_thread_start(s3_database_session *session) {
#if defined(_WIN32)
    session->lease_thread =
        _beginthreadex(nullptr, 0, lease_thread_main, session, 0, nullptr);
    if (!session->lease_thread) {
        std::fprintf(stderr,
                     "otsardb: failed to start the writer lease thread for %s; "
                     "the lease will expire locally (fence to SQLITE_BUSY) "
                     "unless the session closes first\n",
                     session->instance_id.c_str());
    }
#else
    session->lease_thread_running =
        pthread_create(&session->lease_thread, nullptr, lease_thread_main, session) == 0;
    if (!session->lease_thread_running) {
        std::fprintf(stderr,
                     "otsardb: failed to start the writer lease thread for %s; "
                     "the lease will expire locally (fence to SQLITE_BUSY) "
                     "unless the session closes first\n",
                     session->instance_id.c_str());
    }
#endif
}

static void session_lease_thread_stop(s3_database_session *session) {
    session->lease_stop.store(1, std::memory_order_release);
#if defined(_WIN32)
    if (session->lease_thread) {
        WaitForSingleObject(reinterpret_cast<HANDLE>(session->lease_thread), INFINITE);
        CloseHandle(reinterpret_cast<HANDLE>(session->lease_thread));
        session->lease_thread = 0;
    }
#else
    if (session->lease_thread_running) {
        pthread_join(session->lease_thread, nullptr);
        session->lease_thread_running = false;
    }
#endif
}

static const char *env_nonempty(const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] != '\0' ? value : nullptr;
}

/* Parse a positive integer environment variable, falling back to
 * default_value when the variable is absent, unparsable or below 1. */
static int env_positive_int(const char *name, int default_value) {
    const char *value = env_nonempty(name);
    if (!value) return default_value;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 2147483647L) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

/* Parse a non-negative integer environment variable (0 allowed), falling
 * back to default_value when the variable is absent, unparsable or negative.
 * Used for thresholds whose default is 0. */
static int env_nonneg_int(const char *name, int default_value) {
    const char *value = env_nonempty(name);
    if (!value) return default_value;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 2147483647L) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

static const char *env_first(const char *primary, const char *fallback) {
    const char *value = env_nonempty(primary);
    return value ? value : env_nonempty(fallback);
}

static std::string lower_copy(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

static bool env_true(const char *name) {
    const char *value = env_nonempty(name);
    if (!value) return false;
    const std::string normalized = lower_copy(value);
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

/* (E3): test-only fault injection for the FRESH page-cache
 * reset (see the forward declaration above). OTSARDB_TEST_FAIL_RESET_CACHE
 * is never set by the engine or any production path; the deterministic
 * regression test uses it to force the fail-closed branch. */
static bool fresh_test_fail_reset_cache(void) {
    const char *value = env_nonempty("OTSARDB_TEST_FAIL_RESET_CACHE");
    if (!value) return false;
    const std::string normalized = lower_copy(value);
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

static std::string cache_dir_path() {
    const char *configured = env_nonempty("OTSARDB_CACHE_DIR");
    if (configured) return configured;
    try {
        return std::filesystem::temp_directory_path().string() + "\\otsardb";
    } catch (...) {
        return "";
    }
}

static unsigned long process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

static void remove_execution_files(const std::string &base) {
    std::error_code ignored;
    std::filesystem::remove(base, ignored);
    std::filesystem::remove(base + "-wal", ignored);
    std::filesystem::remove(base + "-shm", ignored);
}

/* ---------------------------------------------------------------------------
 * — POSIX inode-preserving execution-image swap.
 *
 * The two in-place image swaps (fresh-mode rematerialisation, lease
 * takeover) rewrite session->local_path while the SQLite connection keeps
 * the main database file OPEN. On Windows this has always been a same-file
 * truncate-rewrite: the ladder's remove() calls FAIL on the locked handle
 * (sharing violation), so every restore writes into the very file the
 * connection reads. On POSIX remove() succeeds and unlinks the open file:
 * the connection's fd keeps pointing at the ORPHANED inode and keeps
 * serving the pre-swap image — the observed "Windows passes, Linux serves
 * the old generation" divergence (/0080 class).
 *
 * Fix (POSIX only): the complete-image ladder branches (cache probe,
 * 0094/0097 delta, full restore) internally remove()+rewrite their
 * destination, so they are targeted at a staging sidecar; the VERIFIED
 * image is then copied OVER the live file in place (same inode, truncate
 * to the exact size + rewrite + fsync). The lazy branch preserves the
 * inode itself (lazy_truncate_file and the hydrator's page writes open the
 * path with "r+b"), so it keeps targeting the live file directly. Windows
 * keeps the exact pre-0127 behavior (no staging, no extra copy).
 * ------------------------------------------------------------------------- */

/* Remove only the -wal/-shm sidecars: they are re-created on demand and
 * the pre-swap WAL was neutralised by checkpoint(TRUNCATE). The main image
 * file must survive on POSIX (see above). */
static void remove_execution_sidecars(const std::string &base) {
    std::error_code ignored;
    std::filesystem::remove(base + "-wal", ignored);
    std::filesystem::remove(base + "-shm", ignored);
}

#if !defined(_WIN32)
/* Copy `src` over the EXISTING file at `dst` without replacing its inode
 * (truncate to the exact source size + rewrite + fsync). POSIX-only; used
 * only by the staged complete-image swap described above. */
static int copy_file_over_in_place(const std::string &src, const std::string &dst) {
    FILE *in = std::fopen(src.c_str(), "rb");
    if (!in) return 0;
    FILE *out = std::fopen(dst.c_str(), "r+b");
    if (!out) out = std::fopen(dst.c_str(), "w+b");
    if (!out) {
        std::fclose(in);
        return 0;
    }

    char buf[65536];
    int ok = 1;
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), in);
        if (n > 0 && std::fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
        if (n < sizeof(buf)) {
            if (std::ferror(in)) ok = 0;
            break;
        }
    }

    if (ok && std::fflush(out) != 0) ok = 0;
    if (ok) {
        /* The fresh image may be SMALLER than the pre-swap one: cut the tail
         * so the live file is exactly the verified image. */
        if (std::fseek(in, 0, SEEK_END) != 0) {
            ok = 0;
        } else {
            const long size = std::ftell(in);
            if (size < 0 ||
                ::ftruncate(::fileno(out), static_cast<off_t>(size)) != 0 ||
                ::fsync(::fileno(out)) != 0) {
                ok = 0;
            }
        }
    }
    std::fclose(in);
    std::fclose(out);
    return ok;
}
#endif

static bool make_execution_path(const char *database_id, std::string *out_path) {
    if (!database_id || !out_path) return false;
    try {
        const char *configured = env_nonempty("OTSARDB_CACHE_DIR");
        std::filesystem::path dir = configured
            ? std::filesystem::path(configured)
            : (std::filesystem::temp_directory_path() / "otsardb");
        /* (F04): create with mode 0700 on POSIX (re-applied
         * on every open so a pre-existing loose dir is tightened); fail
         * closed if the dir cannot be hardened. */
        if (!otsardb_cache_dir_apply_perms(dir.string().c_str())) return false;

        const unsigned long long session_id = ++g_session_counter;
        std::string filename = std::string(database_id) + "-" + std::to_string(process_id()) + "-" +
                               std::to_string(session_id) + ".db";
        *out_path = (dir / filename).string();
        return true;
    } catch (...) {
        return false;
    }
}

static int collect_single_text(void *ctx, int argc, char **argv, char **columns) {
    (void)columns;
    if (!ctx || argc != 1 || !argv || !argv[0]) return 1;
    *static_cast<std::string *>(ctx) = argv[0];
    return 0;
}

static int durability_authorizer(void *ctx,
                                 int action,
                                 const char *arg1,
                                 const char *arg2,
                                 const char *db_name,
                                 const char *trigger_name) {
    (void)ctx;
    (void)arg2;
    (void)db_name;
    (void)trigger_name;

    /* (F08 third recommendation): deny ATTACH outright. Every
     * ATTACH variant — a local file path, ':memory:', a named in-memory db,
     * an encrypted database, any aux name — reaches the authorizer as
     * SQLITE_ATTACH with arg1 = the target filename. The F08 Studio boundary
     * already restricts which targets the loopback API may open; ATTACH would
     * otherwise let any session read or write ANY local path the process user
     * can access through plain SQL, bypassing the s3:// default. DETACH
     * cannot create an aux database, so refusing ATTACH is the complete
     * boundary (there is nothing to detach once ATTACH is refused). */
    if (action == SQLITE_ATTACH) return SQLITE_DENY;

    if (action != SQLITE_PRAGMA || !arg1 || !arg2) return SQLITE_OK;
    const std::string pragma = lower_copy(arg1);
    if (pragma == "journal_mode" || pragma == "synchronous" || pragma == "wal_autocheckpoint") {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

static void destroy_session(void *ctx) {
    auto *session = static_cast<s3_database_session *>(ctx);
    if (!session) return;

    /* Stop and join the HTTP forward server FIRST: its handler executes SQL
     * on the same SQLite connection and reads the lease, both torn down
     * below. The stop unblocks the accept loop and joins the thread. */
    if (session->forward_server) {
        otsardb_forward_server_stop(session->forward_server);
        session->forward_server = nullptr;
    }

    /* Stop and join the writer lease thread FIRST: it uses the store, the
     * local execution files and the publisher, all torn down below. The
     * thread may be asleep for up to one poll interval (2 s), so a close
     * can block briefly; it can never outlive the session. */
    session_lease_thread_stop(session);

    /* Automatic threshold-based checkpointing: when a session closes and
     * either (a) the number of commits published since the last checkpoint
     * is at least OTSARDB_AUTO_CHECKPOINT_COMMITS (default 1000), or (b) the
     * WAL bytes read this session (the publisher's cheap per-session byte
     * counter, proxy for committed payload volume) reached
     * OTSARDB_AUTO_CHECKPOINT_BYTES (default 1 GiB), materialise a checkpoint
     * from the current state so future recovery skips old history. This is
     * best-effort: a failure only logs to stderr and must never break the
     * close. OTSARDB_AUTO_CHECKPOINT=0 disables it entirely. */
    const char *auto_checkpoint = env_nonempty("OTSARDB_AUTO_CHECKPOINT");
    bool checkpoint_built = false;
    /* A read-replica session never runs the checkpoint
     * builder: it would WRITE to the replica store this session reads from
     * (and its publisher never publishes, so the head/generation guards
     * below would already skip it — this is the explicit documented gate). */
    if (!session->read_replica &&
        (!auto_checkpoint || std::strcmp(auto_checkpoint, "0") != 0) &&
        session->publisher.head_loaded &&
        session->publisher.current_generation > 0) {
        const int commit_threshold = env_positive_int("OTSARDB_AUTO_CHECKPOINT_COMMITS", 1000);
        const int byte_threshold = env_nonneg_int("OTSARDB_AUTO_CHECKPOINT_BYTES",
                                                  1024 * 1024 * 1024);

        otsardb_checkpoint_pointer cp_ptr{};
        char *cp_etag = nullptr;
        uint64_t covered = 0;
        otsardb_checkpoint_ptr_result ptr_rc = otsardb_checkpoint_ptr_read(
            session->store, session->publisher.database_root, &cp_ptr, &cp_etag);
        std::free(cp_etag);
        if (ptr_rc == OTSARDB_CHECKPOINT_PTR_OK) {
            covered = cp_ptr.covered_generation;
        }

        const uint64_t generation = session->publisher.current_generation;
        const bool commit_trigger =
            generation >= covered + static_cast<uint64_t>(commit_threshold);
        const bool byte_trigger =
            session->publisher.wal_bytes_read_total >=
            static_cast<uint64_t>(byte_threshold);
        if (commit_trigger || byte_trigger) {
            if (otsardb_checkpoint_build(session->store,
                                       session->publisher.database_root,
                                       session->local_path.c_str())) {
                checkpoint_built = true;
            } else {
                std::fprintf(stderr,
                             "otsardb: automatic checkpoint failed at generation %llu "
                             "(best-effort; continuing session close)\n",
                             static_cast<unsigned long long>(generation));
            }
        }
    }

    /* GC after checkpoint: once the NEW checkpoint pointer
     * is on the store (checkpoint_build publishes it last), sweep the
     * history it superseded. Opt-in via OTSARDB_GC=1 (default off — never
     * delete anything unless explicitly enabled); OTSARDB_GC_KEEP_MINUTES
     * (default 0 = delete immediately) keeps a fresh checkpoint's history
     * for rollback. Best-effort: a failure only logs to stderr and never
     * breaks the close. */
    if (checkpoint_built && env_true("OTSARDB_GC")) {
        otsardb_gc_options gc_opts{};
        gc_opts.keep_secs =
            static_cast<uint64_t>(env_nonneg_int("OTSARDB_GC_KEEP_MINUTES", 0)) * 60ull;
        gc_opts.owner_expected = session->instance_id.c_str();
        const int deleted = otsardb_gc_sweep(session->store,
                                           session->publisher.database_root,
                                           &gc_opts);
        if (deleted < 0) {
            std::fprintf(stderr,
                         "otsardb: GC failed (best-effort; continuing session close)\n");
        } else {
            std::fprintf(stderr,
                         "otsardb: GC swept %d superseded object(s) at generation %llu\n",
                         deleted,
                         static_cast<unsigned long long>(session->publisher.current_generation));
        }
    }

    /* Persist the current database image and HEAD state to the local cache
     * before tearing down the session. The next open will skip recovery if
     * the remote HEAD hasn't changed.
     *
     * Lazy hydration: the disposable local image is only a
     * valid cache entry when it is COMPLETE — a partial image must never be
     * cached as authoritative (a later cache-reuse open would serve zero
     * pages). A successful checkpoint build above re-materialised the whole
     * image, so the hydrator is marked complete right after it. */
    if (checkpoint_built && session->hydrator) {
        otsardb_lazy_mark_all(session->hydrator);
    }
    if (session->publisher.head_loaded &&
        session->publisher.current_generation > 0 &&
        (!session->hydrator || otsardb_lazy_is_complete(session->hydrator))) {
        otsardb_local_cache_state state{};
        state.generation = session->publisher.current_generation;
        std::snprintf(state.commit_id,
                      sizeof(state.commit_id),
                      "%s",
                      session->publisher.current_commit_id);
        std::snprintf(state.commit_sha256,
                      sizeof(state.commit_sha256),
                      "%s",
                      session->publisher.current_manifest_sha256);
        otsardb_local_cache_save(cache_dir_path().c_str(),
                               session->publisher.database_id,
                               session->local_path.c_str(),
                               &state);
    }

    if (session->lease) {
        otsardb_lease_release(session->lease);
        otsardb_lease_destroy(session->lease);
        session->lease = nullptr;
    }
    /* Leases retired by the lease thread (lost / self-demoted / failed
     * takeover) are freed here, after the join above guarantees no fence
     * or thread can still touch them. */
    for (otsardb_lease *retired : session->retired_leases) {
        otsardb_lease_destroy(retired);
    }
    session->retired_leases.clear();

    if (session->vfs) {
        otsardb_local_vfs_instance_destroy(session->vfs);
        session->vfs = nullptr;
    }
    if (session->hydrator) {
        /* Destroyed after the VFS (no more page calls) and before the store
         * (no more fetches). */
        otsardb_lazy_destroy(session->hydrator);
        session->hydrator = nullptr;
    }
    /* /0089: release the publisher's replica destination
     * handle. For a dual-publish session this closes the lazily-opened
     * replica store (the pre-existing 0072 destroy wiring follow-up, now
     * landed); for a promoted session the attached handle is BORROWED (the
     * old primary store below is still owned and released by this session).
     * Must run before the stores are destroyed: the publisher is the last
     * user of the replica handle. */
    otsardb_wal_publisher_destroy(&session->publisher);
    /* a promoted session releases the OLD primary store
     * here — after the checkpoint/GC/cache-save steps above, which only
     * ever touch the promoted primary store (session->store). */
    if (session->old_primary_store) {
        otsardb_s3_store_destroy(session->old_primary_store);
        session->old_primary_store = nullptr;
    }
    if (session->store) {
        /* a session opened through the test hook
         * (otsardb_s3_open_with_store) BORROWS its store — the caller owns
         * its lifetime, never destroy it here. */
        if (!session->store_borrowed) {
            otsardb_s3_store_destroy(session->store);
        }
        session->store = nullptr;
    }
    /* unregister this session's cipher from the thread-local
     * publish registry (only when it is still the registered one — a later
     * session on this thread must not lose its own registration) and destroy
     * it (key material zeroised). */
    if (otsardb_cipher_thread_get() == session->cipher) {
        otsardb_cipher_thread_set(nullptr);
    }
    if (session->cipher) {
        otsardb_cipher_destroy(session->cipher);
        session->cipher = nullptr;
    }
    remove_execution_files(session->local_path);
    delete session;
}

/* ---------------------------------------------------------------------------
 * Capability-probe verdict cache .
 *
 * The probe costs ~11 sequential requests and its verdict (S3 Core / CAS /
 * multipart) is deployment-static, so it is cached per (endpoint, region,
 * access-key, prefix) in a shared temp-directory file. Every open after the
 * first per-endpoint open skips the probe entirely. The cache is advisory:
 * a missing/corrupt entry falls back to a real probe; OTSARDB_S3_PROBE_CACHE=0
 * always probes. Capability drift on a fixed endpoint degrades fail-closed
 * (CAS missing -> single-writer assertion required).
 * ------------------------------------------------------------------------- */

static std::string probe_cache_dir() {
#if defined(_WIN32)
    char buffer[MAX_PATH + 1] = {0};
    DWORD len = GetTempPathA(MAX_PATH, buffer);
    std::string dir = len > 0 ? std::string(buffer, len) : std::string(".");
#else
    const char *tmp = getenv("TMPDIR");
    std::string dir = (tmp && tmp[0]) ? tmp : "/tmp";
#endif
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir + "/otsardb-probe-cache";
}

static std::string probe_cache_path(const std::string &cache_key) {
    unsigned char digest[32];
    char hex[65] = {0};
    if (!otsardb_sha256(cache_key.data(), cache_key.size(), digest)) return "";
    for (int i = 0; i < 32; ++i) {
        static const char digits[] = "0123456789abcdef";
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    return probe_cache_dir() + "/" + hex + ".json";
}

/* Load a cached verdict. Returns OTSARDB_STORE_OK on hit (report filled),
 * OTSARDB_STORE_NOT_FOUND when no valid entry exists. */
static otsardb_store_result otsardb_probe_cache_load(const std::string &cache_key,
                                                 otsardb_store_probe_report *out_report) {
    if (!out_report) return OTSARDB_STORE_ERROR;
    std::string path = probe_cache_path(cache_key);
    if (path.empty()) return OTSARDB_STORE_ERROR;
    std::ifstream in(path, std::ios::binary);
    if (!in) return OTSARDB_STORE_NOT_FOUND;
    std::string json;
    json.reserve(256);
    std::string line;
    while (std::getline(in, line)) json += line;
    in.close();

    unsigned long long capabilities = 0;
    int core_ok = 0;
    int cas_ok = 0;
    int n = std::sscanf(json.c_str(),
                        "{\"capabilities\":%llu,\"s3_core_ok\":%d,\"s3_cas_ok\":%d}",
                        &capabilities, &core_ok, &cas_ok);
    if (n != 3) return OTSARDB_STORE_NOT_FOUND;
    if ((core_ok && (capabilities & OTSARDB_STORE_CAP_ETAG) == 0) ||
        (capabilities & OTSARDB_STORE_CAP_RANGE_GET) == 0 ||
        (capabilities & OTSARDB_STORE_CAP_READ_AFTER_WRITE) == 0) {
        return OTSARDB_STORE_NOT_FOUND; /* stale/corrupt verdict */
    }
    out_report->capabilities = (otsardb_store_capabilities)capabilities;
    out_report->s3_core_ok = core_ok;
    out_report->s3_cas_ok = cas_ok;
    return OTSARDB_STORE_OK;
}

/* Save a verdict (atomic tmp + rename; concurrent writers are benign). */
static void otsardb_probe_cache_save(const std::string &cache_key,
                                   const otsardb_store_probe_report *report) {
    std::string path = probe_cache_path(cache_key);
    if (path.empty() || !report) return;
    try {
        /* (F04): the probe dir is created with mode 0700 on
         * POSIX; an unhardened dir is refused (advisory verdicts must not be
         * planted world-readable in shared /tmp). */
        std::string parent = std::filesystem::path(path).parent_path().string();
        if (!otsardb_cache_dir_apply_perms(parent.c_str())) return;
        std::string tmp = path + ".tmp";
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) return;
        out << "{\"capabilities\":" << (unsigned long long)report->capabilities
            << ",\"s3_core_ok\":" << (report->s3_core_ok ? 1 : 0)
            << ",\"s3_cas_ok\":" << (report->s3_cas_ok ? 1 : 0) << "}\n";
        out.close();
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    } catch (...) {
        /* Best-effort: the next open just re-probes. */
    }
}

static bool configure_store(const otsardb_s3_target &target,
                            const char *database_id,
                            s3_database_session *session,
                            otsardb_store_probe_report *out_report) {
    const char *endpoint = env_nonempty("OTSARDB_S3_ENDPOINT");
    const char *region = env_first("OTSARDB_S3_REGION", "AWS_REGION");
    if (!region) region = env_nonempty("AWS_DEFAULT_REGION");
    if (!region) region = "us-east-1";

    const char *access_key = env_first("OTSARDB_S3_ACCESS_KEY", "AWS_ACCESS_KEY_ID");
    const char *secret_key = env_first("OTSARDB_S3_SECRET_KEY", "AWS_SECRET_ACCESS_KEY");
    const char *session_token = env_first("OTSARDB_S3_SESSION_TOKEN", "AWS_SESSION_TOKEN");
    const char *prefix = env_nonempty("OTSARDB_S3_PREFIX");

    if (!access_key || !access_key[0]) access_key = nullptr;
    if (!secret_key || !secret_key[0]) secret_key = nullptr;
    const char *effective_endpoint = endpoint ? endpoint : "https://s3.amazonaws.com";

    otsardb_s3_config config{};
    config.endpoint = effective_endpoint;
    config.region = region;
    config.bucket = target.bucket;
    config.access_key = access_key;
    config.secret_key = secret_key;
    config.session_token = session_token;
    config.prefix = prefix;
    config.use_https = 1;

    session->store = otsardb_s3_store_create(&config);
    if (!session->store) {
        set_error("failed to initialize the S3 client; verify endpoint, region and credentials");
        return false;
    }

    std::string probe_key = "__otsardb_probe/" + std::string(database_id) + "/p" + std::to_string(process_id());
    otsardb_store_probe_report report{};
    otsardb_store_result probe_rc;
    bool probed = false;

    /* Probe verdict cache: the capability probe costs ~11
     * sequential requests (~1-2 s on a high-RTT link) and its verdict is
     * deployment-static. The verdict is cached per (endpoint, region, access
     * key, prefix) in a shared temp-directory file, so every open after the
     * first skips the probe entirely. OTSARDB_S3_PROBE_CACHE=0 disables the
     * cache (always probe); a stale verdict is fail-closed in the
     * interesting direction (missing CAS -> single-writer required). */
    const char *probe_cache_env = env_nonempty("OTSARDB_S3_PROBE_CACHE");
    bool probe_cache = !probe_cache_env || env_true("OTSARDB_S3_PROBE_CACHE");
    std::string cache_key;
    if (probe_cache) {
        cache_key = std::string(effective_endpoint) + "|" + region + "|" +
                    (access_key ? access_key : "") + "|" + (prefix ? prefix : "");
        probe_rc = otsardb_probe_cache_load(cache_key, &report);
        if (probe_rc == OTSARDB_STORE_OK) {
            session->store->capabilities = report.capabilities;
            probed = true;
        }
    }
    if (!probed) {
        {
            std::lock_guard<std::mutex> lock(g_probe_mutex);
            probe_rc = otsardb_store_probe_capabilities(session->store,
                                                       probe_key.c_str(),
                                                       &report);
        }
        if (probe_rc == OTSARDB_STORE_OK && probe_cache) {
            otsardb_probe_cache_save(cache_key, &report);
        }
    }
    if (probe_rc != OTSARDB_STORE_OK || !report.s3_core_ok) {
        set_error("S3 endpoint failed the OtsarDB S3 Core capability probe (GET/PUT/ETag/Range/read-after-write required)");
        return false;
    }

    if (!report.s3_cas_ok && !env_true("OTSARDB_S3_SINGLE_WRITER")) {
        set_error("this S3 endpoint does not provide conditional PUT/CAS; native writes require OTSARDB_S3_SINGLE_WRITER=1 and an external guarantee that only one OtsarDB writer can publish this database at a time");
        return false;
    }

    if (out_report) *out_report = report;
    return true;
}

}  // namespace

extern "C" const char *otsardb_s3_last_error(void) {
    return g_last_error.c_str();
}

extern "C" int otsardb_s3_open_target(const char *target, otsardb **out_db) {
    return otsardb_s3_open_target_ex(target, nullptr, out_db);
}

/* shared open tail (declared before the entry points). */
static int open_session_common(s3_database_session *session,
                               const char *database_root,
                               const char *database_id,
                               const otsardb_store_probe_report *report,
                               const otsardb_s3_open_config *config,
                               otsardb **out_db);

extern "C" int otsardb_s3_leader_address(otsardb *db, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    auto *session = static_cast<s3_database_session *>(otsardb_internal_close_ctx(db));
    if (!session) return 0;
    /* This session IS the current leader: it executes locally, nothing to
     * forward. (A demoted leader falls through: no known leader yet.) */
    otsardb_lease *lease = session->lease.load(std::memory_order_acquire);
    if (lease && otsardb_lease_is_live(lease) == 1) return 0;
    std::string address;
    {
        std::lock_guard<std::mutex> lock(session->leader_mutex);
        /* Explicit CLI override wins over the lease-derived address. */
        address = !session->forward_to.empty() ? session->forward_to
                                               : session->leader_address;
    }
    if (address.empty()) return 0;
    std::snprintf(out, cap, "%s", address.c_str());
    return 1;
}

extern "C" int otsardb_s3_open_target_ex(const char *target,
                                       const otsardb_s3_open_config *config,
                                       otsardb **out_db) {
    g_last_error.clear();
    if (!out_db) {
        set_error("output OtsarDB handle is null");
        return OTSARDB_ERROR;
    }
    *out_db = nullptr;

    otsardb_s3_target parsed{};
    if (!otsardb_s3_target_parse(target, &parsed)) {
        set_error("invalid S3 target; expected s3://bucket/database-root");
        return OTSARDB_ERROR;
    }

    char database_id[OTSARDB_COMMIT_ID_MAX + 1] = {0};
    if (!otsardb_s3_target_database_id(&parsed, database_id)) {
        set_error("failed to derive OtsarDB database identity");
        return OTSARDB_ERROR;
    }

    auto *session = new (std::nothrow) s3_database_session();
    if (!session) {
        set_error("out of memory creating S3 database session");
        return OTSARDB_ERROR;
    }

    /* Lazy hydration gate: default ON. "0" (or false/off/no)
     * restores the pre-0043 full-materialisation path. */
    {
        const char *lazy_env = env_nonempty("OTSARDB_LAZY_HYDRATION");
        session->lazy_enabled = !lazy_env || env_true("OTSARDB_LAZY_HYDRATION");
    }

    /* — read-replica sessions (0069 "read-replica" decision
     * point): OTSARDB_S3_READ_FROM_REPLICA=1 opens a
     * READ-ONLY session whose recovery/read path uses the replica
     * destination (OTSARDB_S3_REPLICA_*, same database_root) instead of the
     * primary. The reader sees the newest commit that is COMMITTED on the
     * replica — always a valid committed state, never partial (the replica
     * HEAD advances only after every object of the commit is durable there;
     * an in-flight dual publish leaves the replica at its previous HEAD).
     * The session never claims a lease and never publishes. */
    const bool read_replica = otsardb_replica_read_requested() != 0;
    session->read_replica = read_replica;
    if (read_replica && otsardb_replica_promote_requested()) {
        set_error("OTSARDB_S3_READ_FROM_REPLICA and OTSARDB_S3_PROMOTE cannot be "
                  "combined: a read-replica session is read-only and can never "
                  "promote (promotion writes the epoch and runs recovery)");
        destroy_session(session);
        return OTSARDB_ERROR;
    }

    /* at-rest segment encryption gate. OTSARDB_S3_ENC_KEY set
     * (must be exactly 64 hex chars = 32 bytes) turns on AES-256-GCM
     * encryption of every published segment; unset = plaintext (default,
     * byte-identical pre-0079 behavior). Invalid key material and the
     * dual-publish combination fail the open closed — the replica path
     * (0072) re-encodes segments itself and cannot replicate encrypted
     * objects byte-identically. */
    {
        const char *enc_key = env_nonempty("OTSARDB_S3_ENC_KEY");
        if (enc_key) {
            if (!otsardb_cipher_valid_hex_key(enc_key)) {
                set_error("OTSARDB_S3_ENC_KEY must be exactly 64 hex characters (32 bytes); refusing to open");
                destroy_session(session);
                return OTSARDB_ERROR;
            }
            if (otsardb_replica_configured() && !read_replica) {
                set_error("OTSARDB_S3_ENC_KEY cannot be combined with OTSARDB_S3_REPLICA_* dual publish "
                          "(the replica path re-encodes segments and cannot "
                          "replicate encrypted objects byte-identically); refusing to open");
                destroy_session(session);
                return OTSARDB_ERROR;
            }
            /* A read-replica session MAY combine the key with
             * OTSARDB_S3_REPLICA_*: the replica variables are then the READ path
             * only (the cipher decrypts the replica's encrypted segments; the
             * publish path is dead, so nothing is ever re-encoded). */
            session->cipher = otsardb_cipher_create_from_hex(enc_key);
            if (!session->cipher) {
                set_error(std::string("failed to create the AES-256-GCM cipher: ") +
                          otsardb_cipher_last_error());
                destroy_session(session);
                return OTSARDB_ERROR;
            }
            otsardb_cipher_thread_set(session->cipher);
            std::fprintf(stderr,
                         "otsardb: at-rest segment encryption enabled (AES-256-GCM, "
                         "OTSARDB_S3_ENC_KEY)\n");
        }
    }

    otsardb_store_probe_report report{};
    if (read_replica) {
        /* the read-replica session's store IS the replica
         * destination. otsardb_replica_open_reader performs ZERO writes to the
         * database root (no epoch bootstrap, no ledgers, no HEAD, no lease);
         * the only write is the capability-probe verdict object, outside the
         * root, identical to any other open. */
        char open_err[512] = {0};
        session->store = otsardb_replica_open_reader(parsed.database_root,
                                                   database_id,
                                                   &report,
                                                   open_err,
                                                   sizeof(open_err));
        if (!session->store) {
            set_error(std::string("failed to open the read-replica destination: ") +
                      (open_err[0] ? open_err : "unknown error"));
            destroy_session(session);
            return OTSARDB_ERROR;
        }
    } else if (!configure_store(parsed, database_id, session, &report)) {
        destroy_session(session);
        return OTSARDB_ERROR;
    }

    if (!make_execution_path(database_id, &session->local_path)) {
        set_error("failed to create disposable OtsarDB local execution path");
        destroy_session(session);
        return OTSARDB_ERROR;
    }
    remove_execution_files(session->local_path);

    /* ---------------------------------------------------------------------
     * — session-level promotion (OTSARDB_S3_PROMOTE=1 at open;
     * 0077 Next item 2;). The operator promotes the
     * replica destination to PRIMARY:
     * 1. REFUSE while the CURRENT PRIMARY's writer lease is provably live:
     * otsardb_replica_promote reads <root>/lease.json on the old primary
     * store and judges expiry with the SAME predicate as the lease
     * module (TTL - skew from the store's Last-Modified; unknown =
     * presumed live) — the offline/disaster-recovery operator contract;
     * 2. run the 0077 promotion sequence: raise the destination epoch by
     * CAS (If-Match N -> N+1, If-None-Match create when absent); a lost
     * epoch CAS race fails the open with the clear "another promotion
     * won" message (two concurrent promotes cannot both win);
     * 3. SWAP the session's store to the destination: the promoted store
     * becomes session->store AND the publisher's store (the publisher
     * operates on the promoted store — the same init'd handle is
     * re-pointed, no re-init needed), the old primary store handle is
     * closed as the session's store but kept open and ATTACHED as the
     * session's REPLICA destination: the roles swap. After the swap the
     * normal open flow below (head read, recovery from the promoted
     * store, lease claim) runs unchanged on the promoted store.
     *
     * The promotion runs BEFORE any lease claim on the primary: a lease
     * this session claimed on the old primary store would make it provably
     * live and the promotion would refuse itself.
     * ------------------------------------------------------------------- */
    bool promoted = false;
    if (!read_replica && otsardb_replica_promote_requested()) {
        if (!otsardb_wal_publisher_init(&session->publisher,
                                      session->store,
                                      parsed.database_root,
                                      database_id,
                                      report.s3_cas_ok ? OTSARDB_WRITE_STRATEGY_CAS
                                                       : OTSARDB_WRITE_STRATEGY_SINGLE_WRITER)) {
            set_error("failed to initialize the OtsarDB WAL publisher for this S3 endpoint");
            destroy_session(session);
            return OTSARDB_ERROR;
        }

        char promote_err[512] = {0};
        uint64_t raised_epoch = 0;
        otsardb_journal_result promote_rc = OTSARDB_JOURNAL_ERROR;
        if (!otsardb_wal_publisher_promote(&session->publisher,
                                         session->local_path.c_str(),
                                         &raised_epoch,
                                         promote_err,
                                         sizeof(promote_err),
                                         &promote_rc)) {
            if (promote_rc == OTSARDB_JOURNAL_CONFLICT) {
                std::string message =
                    promote_err[0]
                        ? promote_err
                        : "the old primary is still authoritative (its writer "
                          "lease is live) or another session promoted this "
                          "destination first (the epoch CAS was lost). Promotion "
                          "is an OFFLINE / DISASTER-RECOVERY operator contract: "
                          "stop the old primary (or wait OTSARDB_LEASE_TTL for its "
                          "lease to expire) and retry";
                if (message.rfind("promotion refused", 0) != 0) {
                    message = "promotion refused: " + message;
                }
                set_error(message);
            } else {
                set_error(std::string("promotion failed: ") +
                          (promote_err[0] ? promote_err
                                          : "store error during promotion; nothing was "
                                            "changed"));
            }
            destroy_session(session);
            return OTSARDB_ERROR;
        }

        /* report which env destination was promoted (the
         * NEWEST COMMITTED one). The session reopens THAT destination as
         * its primary store below (open_reader_at reads the
         * OTSARDB_S3_REPLICA_* / REPLICA2_* / REPLICA3_* env set of the
         * promoted index). */
        const int promoted_index = otsardb_replica_promoted_index(session->publisher.replica);
        if (promoted_index < 0) {
            set_error("promotion failed: the replica handle did not report a "
                      "promoted destination index");
            destroy_session(session);
            return OTSARDB_ERROR;
        }

        /* Store swap (0077's documented session-side follow-up). The promote
         * path opened the replica destination as a second store inside the
         * publisher's replica handle; release that handle (it never touches
         * the destination after the swap — the session owns the promoted
         * store below) and open the destination once more as a plain store
         * handle via the same env-driven open the reader path uses (zero
         * writes to the database root: the epoch is already raised). */
        otsardb_object_store *old_primary = session->store;
        otsardb_replica_close(session->publisher.replica);
        session->publisher.replica = nullptr;
        char open_err[512] = {0};
        otsardb_object_store *promoted_store =
            otsardb_replica_open_reader_at(promoted_index,
                                         parsed.database_root,
                                         database_id,
                                         &report,
                                         open_err,
                                         sizeof(open_err));
        if (!promoted_store) {
            set_error(std::string("promotion: failed to open the promoted destination "
                                  "as the session's primary store: ") +
                      (open_err[0] ? open_err : "unknown error"));
            destroy_session(session);
            return OTSARDB_ERROR;
        }
        session->store = promoted_store;
        session->publisher.store = promoted_store;
        session->publisher.strategy =
            report.s3_cas_ok ? OTSARDB_WRITE_STRATEGY_CAS : OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;
        /* Role swap: the OLD primary store becomes this session's REPLICA
         * destination. The attach bootstraps the epoch records by
         * create-or-adopt (verbatim adoption never rewrites): the old
         * primary's record stays at 1 (this session's fence on it) and the
         * promoted store's raised record is adopted as-is. Borrowed: the
         * replica never destroys the old primary store — the session owns
         * it and releases it in destroy_session. */
        if (!otsardb_replica_attach(&session->publisher,
                                  old_primary,
                                  report.s3_cas_ok ? OTSARDB_WRITE_STRATEGY_CAS
                                                   : OTSARDB_WRITE_STRATEGY_SINGLE_WRITER,
                                  1)) {
            set_error("promotion: failed to attach the old primary store as this "
                      "session's replica destination");
            destroy_session(session);
            return OTSARDB_ERROR;
        }
        /* the role-swap semantics extend to ALL configured
         * destinations — every env destination except the promoted one stays
         * (or re-enters) as a replica destination of the promoted session.
         * Fail-closed: a remaining destination that cannot be opened fails
         * the promoted open (a destination that dies AFTER the open is
         * handled by the OTSARDB_S3_QUORUM machinery instead). */
        if (!otsardb_replica_attach_env_others(&session->publisher,
                                             promoted_index,
                                             open_err,
                                             sizeof(open_err))) {
            set_error(std::string("promotion: failed to attach the remaining "
                                  "replica destinations: ") +
                      (open_err[0] ? open_err : "unknown error"));
            destroy_session(session);
            return OTSARDB_ERROR;
        }
        promoted = true;
        session->promoted = true;
        session->old_primary_store = old_primary;
        std::fprintf(stderr,
                     "otsardb: promotion complete: destination %d epoch raised to %llu; "
                     "the promoted store is now this session's primary and the old "
                     "primary store (plus every other configured destination) is this "
                     "session's replica destination (roles swapped)\n",
                     promoted_index,
                     static_cast<unsigned long long>(raised_epoch));
    }

    return open_session_common(session, parsed.database_root, database_id, &report,
                               config, out_db);
}

/* shared open tail (head read -> materialisation ->
 * publisher/lease -> VFS -> SQLite open -> setup -> hook install). Used by
 * both otsardb_s3_open_target_ex (env-configured store) and the test hook
 * otsardb_s3_open_with_store (borrowed store). Precondition: the session
 * exists with a store, local_path is allocated and cleaned. */
static int open_session_common(s3_database_session *session,
                               const char *database_root,
                               const char *database_id,
                               const otsardb_store_probe_report *report,
                               const otsardb_s3_open_config *config,
                               otsardb **out_db) {
    /* consistency mode gate. snapshot (default/absent) =
     * pinned-at-open (byte-identical pre-0123 behavior); fresh = per-
     * statement HEAD check + re-materialisation. An unknown value fails
     * the open (an operator typo must not silently degrade). */
    {
        const char *consistency = env_nonempty("OTSARDB_CONSISTENCY");
        if (consistency) {
            const std::string mode = lower_copy(consistency);
            if (mode == "fresh") {
                session->fresh_mode = 1;
            } else if (mode != "snapshot") {
                set_error("OTSARDB_CONSISTENCY must be 'snapshot' or 'fresh' (got '" +
                          std::string(consistency) + "'); refusing to open");
                destroy_session(session);
                return OTSARDB_ERROR;
            }
        }
    }

    otsardb_head head{};
    char *head_etag = nullptr;
    otsardb_journal_result head_rc = otsardb_journal_read_head(session->store,
                                                           database_root,
                                                           &head,
                                                           &head_etag);
    std::free(head_etag);
    bool recovered_from_cache = false;
    if (head_rc == OTSARDB_JOURNAL_OK) {
        if (std::strcmp(head.database_id, database_id) != 0) {
            set_error("remote OtsarDB database identity does not match the requested S3 target");
            destroy_session(session);
            return OTSARDB_ERROR;
        }

        /* When local cache reuse is enabled (it is, unless OTSARDB_CACHE_REUSE
         * is explicitly 0/false/off/no), try to reuse the previously saved
         * database image so we skip the full manifest-chain recovery. Any
         * remote HEAD change invalidates the cache. */
        const char *reuse_env = env_nonempty("OTSARDB_CACHE_REUSE");
        bool cache_reuse = !reuse_env ||
                           env_true("OTSARDB_CACHE_REUSE");
        if (cache_reuse) {
            otsardb_local_cache_state remote_state{};
            remote_state.generation = head.generation;
            std::snprintf(remote_state.commit_id,
                          sizeof(remote_state.commit_id),
                          "%s",
                          head.commit_id);
            std::snprintf(remote_state.commit_sha256,
                          sizeof(remote_state.commit_sha256),
                          "%s",
                          head.commit_sha256);
            recovered_from_cache =
                otsardb_local_cache_probe(cache_dir_path().c_str(),
                                        database_id,
                                        &remote_state,
                                        session->local_path.c_str()) != 0;
        }

        if (!recovered_from_cache) {
            bool restore_ok = false;
            /* delta materialization: when a cached image at
             * generation G exists but the remote HEAD is G+k, apply only the
             * newer segments on top of the cached image instead of the full
             * chain (26.5x fewer bytes measured). Any mismatch falls back to
             * the full path; the delta never produces a wrong image.
             *
             * (session wiring defects fixed here):
             * 1. otsardb_local_cache_read_state returns 1 on SUCCESS (a valid
             * cache entry exists) and 0 on failure — the pre-0097 code
             * compared == 0, so the delta branch never ran with a valid
             * cache (the wrong constant also masked the absent-cache
             * case into an attempt that failed closed to the full path).
             * 2. The base image is the PERSISTENT cached image file
             * (<cache_dir>/otsardb-cache-v1/<database_id>/state.db — the
             * exact layout otsardb_local_cache_save/probe/read_state use).
             * The delta reads it and publishes the new image to this
             * session's execution file; the cache file is NEVER modified
             * in place (the pre-0097 wiring passed the session path as
             * BOTH base and destination, which the delta API rejects as
             * base==dest — combined with defect 1, the delta never ran).
             * 3. The delta API returns 1 on success (0 on any mismatch) —
             * the pre-0097 check `== OTSARDB_JOURNAL_OK` (0) inverted the
             * outcome and would have re-materialised even a valid delta.
             * 4. The lazy/full fallback below runs ONLY when restore_ok is
             * still false: a successful delta has already produced the
             * complete image at head and must not be overwritten by a
             * full restore (the pre-0097 if/else entered the full path
             * even after a delta success). */
            if (cache_reuse) {
                otsardb_local_cache_state cached{};
                if (otsardb_local_cache_read_state(cache_dir_path().c_str(),
                                                 database_id,
                                                 &cached) != 0 &&
                    cached.generation > 0 &&
                    cached.generation < head.generation) {
                    std::string cached_img = cache_dir_path() + "/otsardb-cache-v1/" +
                                             database_id + "/state.db";
                    restore_ok =
                        otsardb_recovery_materialize_delta_ex(
                            session->store,
                            database_root,
                            cached_img.c_str(),
                            cached.generation,
                            cached.commit_sha256,
                            session->local_path.c_str(),
                            &head,
                            session->cipher) != 0;
                    if (restore_ok) {
                        /* Always-on one-line diagnostic (
                         * decision): proves the delta path ran and records
                         * the applied generation range and the produced
                         * image size. Segment count / fetched bytes are not
                         * exposed by the (read-only) delta API; they are
                         * measured at the library level. */
                        std::error_code diag_ec;
                        const uint64_t image_bytes =
                            static_cast<uint64_t>(std::filesystem::file_size(
                                session->local_path, diag_ec));
                        std::fprintf(stderr,
                                     "otsardb: delta materialization applied "
                                     "generations %llu..%llu (%llu generation(s), "
                                     "image %llu bytes)\n",
                                     static_cast<unsigned long long>(cached.generation + 1),
                                     static_cast<unsigned long long>(head.generation),
                                     static_cast<unsigned long long>(head.generation -
                                                                      cached.generation),
                                     static_cast<unsigned long long>(image_bytes));
                    }
                }
            }
            if (!restore_ok) {
                if (session->lazy_enabled) {
                    /* Lazy hydration: validate the chain and
                     * materialise only the newest checkpoint base image. The
                     * head read above is reused; the file is pre-sized and every
                     * other segment is fetched on VFS page-read misses. */
                    restore_ok = otsardb_lazy_restore_store_ex(session->store,
                                                             database_root,
                                                             session->local_path.c_str(),
                                                             &head,
                                                             &session->hydrator,
                                                             session->cipher) != 0;
                } else {
                    restore_ok = otsardb_recovery_restore_store_ex(session->store,
                                                                 database_root,
                                                                 session->local_path.c_str(),
                                                                 session->cipher) != 0;
                }
            }
            if (!restore_ok) {
                set_error("failed to reconstruct the authoritative OtsarDB state from S3");
                destroy_session(session);
                return OTSARDB_ERROR;
            }
        }
    } else if (head_rc != OTSARDB_JOURNAL_NOT_FOUND) {
        set_error("failed to read OtsarDB HEAD from S3");
        destroy_session(session);
        return OTSARDB_ERROR;
    }

    const otsardb_write_strategy strategy = report->s3_cas_ok
        ? OTSARDB_WRITE_STRATEGY_CAS
        : OTSARDB_WRITE_STRATEGY_SINGLE_WRITER;
    /* a promoted session initialized the publisher in the
     * promote branch above (the promote protocol runs on the old primary
     * store first, then the publisher's store pointer follows the swap). */
    if (!session->promoted &&
        !otsardb_wal_publisher_init(&session->publisher,
                                  session->store,
                                  database_root,
                                  database_id,
                                  strategy)) {
        set_error("failed to initialize the OtsarDB WAL publisher for this S3 endpoint");
        destroy_session(session);
        return OTSARDB_ERROR;
    }

    /* Anchor the publisher to the generation of the materialised local
     * image. A remote HEAD that has since advanced means another writer
     * committed while we were open â€” writes must fail with BUSY instead of
     * silently basing new pages on a stale snapshot. */
    if (head_rc == OTSARDB_JOURNAL_OK) {
        otsardb_wal_publisher_set_local_image_generation(&session->publisher,
                                                       head.generation);
        /* FRESH sessions compare the remote HEAD against
         * this anchor once per statement. */
        if (session->fresh_mode) {
            session->fresh_anchor_generation.store(head.generation,
                                                   std::memory_order_relaxed);
            std::snprintf(session->fresh_anchor_commit_id,
                          sizeof(session->fresh_anchor_commit_id),
                          "%s",
                          head.commit_id);
        }
    } else {
        /* (F3): a HEAD-less open (the first-writer / genesis
         * case, head_rc == OTSARDB_JOURNAL_NOT_FOUND) MUST still anchor the
         * 0024 generation fence: the local image IS the empty database at
         * generation 0. Without the anchor local_image_generation_known
         * stays 0 and the fence is permanently off for the session — after
         * a foreign takeover advances the head, the session's stale-base
         * commit could win the HEAD CAS and silently overwrite the winner's
         * acknowledged pages (the strongest theoretical lost-update path in
         * the failure-mode audit). Anchored at 0 the fence trips on ANY
         * foreign genesis. */
        otsardb_wal_publisher_set_local_image_generation(&session->publisher, 0);
        if (session->fresh_mode) {
            session->fresh_anchor_generation.store(0, std::memory_order_relaxed);
            session->fresh_anchor_commit_id[0] = '\0';
        }
    }

    /* M2 writer lease (phase 2): with CAS capabilities
     * (and without an explicit single-writer assertion) exactly one session
     * may publish commits at a time. Claim at open:
     * - CLAIMED: this session is the leader; the fence is live and the
     * background thread renews the lease every TTL/3.
     * - HELD: another live writer owns the lease. Phase 1 failed the open;
     * phase 2 opens in CANDIDATE mode instead: every publish is fenced
     * to SQLITE_BUSY and the background thread polls the lease and takes
     * it over once the store proves it expired (automatic failover).
     * - ERROR: the lease state is unknown (store failure); fail the open
     * so the caller can retry, as in phase 1.
     *
     * A read-replica session NEVER claims a lease: it is a
     * read-only session (like --health) and must not race the writer. Its
     * publish fence is installed below and permanently closed. */
    if (strategy == OTSARDB_WRITE_STRATEGY_CAS && !env_true("OTSARDB_S3_SINGLE_WRITER") &&
        !session->read_replica) {
        char instance_id[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
        std::snprintf(instance_id,
                      sizeof(instance_id),
                      "otsardb-%lu-%llu",
                      process_id(),
                      static_cast<unsigned long long>(++g_session_counter));
        session->instance_id = instance_id;

        /* M2 phase 3: start the HTTP forward server BEFORE
         * the first claim so its address is published in the lease object at
         * creation/takeover time. Loopback-only: the endpoint executes
         * arbitrary SQL with no authentication, so it must never be exposed
         * beyond the leader's own host (documented v1 safety boundary). A
         * failed start only warns: the session still works locally, it just
         * never publishes an address (forwarders keep getting BUSY). */
        if (config && config->forward_port > 0) {
            session->forward_port = config->forward_port;
            if (config->forward_to && config->forward_to[0]) {
                session->forward_to = config->forward_to;
            }
            char forward_error[256] = {0};
            session->forward_server = otsardb_forward_server_start(
                "127.0.0.1",
                session->forward_port,
                forward_server_handler,
                session,
                forward_error,
                sizeof(forward_error));
            if (session->forward_server) {
                std::fprintf(stderr,
                             "otsardb: forward server listening on 127.0.0.1:%d\n",
                             otsardb_forward_server_port(session->forward_server));
            } else {
                std::fprintf(stderr,
                             "otsardb: failed to start the forward server on port %d: %s; "
                             "candidate sessions will not be able to forward writes to "
                             "this leader\n",
                             session->forward_port,
                             forward_error[0] ? forward_error : "unknown error");
            }
        }

        const int configured_ttl = env_positive_int("OTSARDB_LEASE_TTL",
                                                    OTSARDB_LEASE_TTL_DEFAULT_SECS);
        uint64_t ttl = configured_ttl > 0
            ? static_cast<uint64_t>(configured_ttl)
            : OTSARDB_LEASE_TTL_DEFAULT_SECS;
        if (ttl < OTSARDB_LEASE_TTL_MIN_SECS) ttl = OTSARDB_LEASE_TTL_MIN_SECS;
        session->lease_ttl_secs = ttl;

        char holder[OTSARDB_LEASE_OWNER_MAX + 1] = {0};
        char holder_address[OTSARDB_LEASE_ADDRESS_MAX + 1] = {0};
        uint64_t epoch = 0;
        int claim_state = OTSARDB_LEASE_ERROR;
        std::string self_address = session_forward_address(session);
        otsardb_lease *claimed = otsardb_lease_claim(session->store,
                                                 database_root,
                                                 instance_id,
                                                 self_address.empty() ? nullptr
                                                                      : self_address.c_str(),
                                                 ttl,
                                                 &epoch,
                                                 &claim_state,
                                                 holder,
                                                 sizeof(holder),
                                                 holder_address,
                                                 sizeof(holder_address));
        if (claim_state != OTSARDB_LEASE_CLAIMED && claim_state != OTSARDB_LEASE_HELD) {
            set_error("failed to claim the OtsarDB writer lease; retry after the "
                      "current holder's lease expires, or set "
                      "OTSARDB_S3_SINGLE_WRITER=1 to assert a single writer");
            destroy_session(session);
            return OTSARDB_ERROR;
        }
        if (claimed) {
            session->lease.store(claimed, std::memory_order_release);
            std::fprintf(stderr,
                         "otsardb: writer lease acquired for %s at generation %llu "
                         "by %s (ttl %llu s)%s%s\n",
                         database_root,
                         static_cast<unsigned long long>(epoch),
                         instance_id,
                         static_cast<unsigned long long>(ttl),
                         self_address.empty() ? "" : "; forward address ",
                         self_address.empty() ? "" : self_address.c_str());
        } else if (claim_state == OTSARDB_LEASE_HELD) {
            std::fprintf(stderr,
                         "otsardb: database %s is leased by another writer (owner %s); "
                         "opening in candidate mode â€” publishes return SQLITE_BUSY "
                         "until this session takes the lease over after expiry\n",
                         database_root,
                         holder[0] ? holder : "unknown");
            /* Seed the forwarding target from the lease the very first poll
             * would otherwise wait ~2 s to learn. */
            std::lock_guard<std::mutex> leader_lock(session->leader_mutex);
            session->leader_address = holder_address;
            if (holder_address[0]) {
                std::fprintf(stderr,
                             "otsardb: forwarding write SQL to the leader at %s\n",
                             holder_address);
            }
        } else {
            std::fprintf(stderr,
                         "otsardb: lease claim for %s returned unexpected state %d "
                         "(instance %s); opening without a lease (candidate mode)\n",
                         database_root, claim_state, instance_id);
        }
        otsardb_wal_publisher_set_lease_fence(&session->publisher,
                                            session_lease_fence,
                                            session);
        /* Background lease thread: renews while leader, polls and takes
         * over while candidate. Starts after the claim so its first poll
         * cannot race the open-time claim. SINGLE_WRITER sessions (no
         * lease) do not get a thread. */
        session_lease_thread_start(session);
    } else if (session->read_replica) {
        /* a read-replica session never claims a lease, so
         * the lease pointer stays null and this fence is PERMANENTLY
         * closed — no code path can ever publish to the replica store (any
         * publish attempt fails with SQLITE_BUSY). Defense-in-depth behind
         * PRAGMA query_only=ON (SQLITE_READONLY on every DML). */
        otsardb_wal_publisher_set_lease_fence(&session->publisher,
                                            session_lease_fence,
                                            session);
    }

    if (otsardb_local_vfs_instance_create(otsardb_wal_publisher_sync_reader,
                                        &session->publisher,
                                        &session->vfs) != SQLITE_OK) {
        set_error("failed to create the database-owned OtsarDB VFS instance");
        destroy_session(session);
        return OTSARDB_ERROR;
    }

    /* Lazy hydration: install the page hooks on the VFS so
     * main-db reads hydrate missing pages on demand. Without a hydrator
     * (cache reuse, or OTSARDB_LAZY_HYDRATION=0) no hooks are installed and
     * the VFS behaves exactly as before. */
    if (session->hydrator) {
        if (otsardb_local_vfs_instance_set_page_hooks(session->vfs,
                                                    session_page_hydrate,
                                                    session_page_mark,
                                                    session_page_truncate,
                                                    session->hydrator) != SQLITE_OK) {
            set_error("failed to install the lazy-hydration page hooks");
            destroy_session(session);
            return OTSARDB_ERROR;
        }
        session->page_hooks_installed = 1;
    }

    otsardb *db = nullptr;
    int open_rc = otsardb_open_with_vfs_owned(session->local_path.c_str(),
                                            otsardb_local_vfs_instance_name(session->vfs),
                                            destroy_session,
                                            session,
                                            &db);
    if (open_rc != OTSARDB_OK) {
        set_error("failed to open the disposable OtsarDB execution image");
        destroy_session(session);
        return open_rc;
    }
    session->db = db;

    /* (E4): serialise every statement on this connection
     * against the lease-thread takeover and the forward-server handler via
     * the session's sql_mutex. Without it a takeover/rematerialisation
     * could rewrite the execution image under an in-flight statement (a
     * long SELECT during lazy hydration reads a mixed old/new image or
     * fails with IOERR). Registered BEFORE the open-time setup execs below
     * so no exec can race the first lease-thread poll. */
    otsardb_internal_set_exec_lock(db,
                                 session_exec_lock,
                                 session_exec_unlock,
                                 session);

    char *sqlite_error = nullptr;
    int setup_rc = OTSARDB_ERROR;
    if (session->read_replica) {
        /* the read-replica session is READ-ONLY. The
         * journal_mode/synchronous/wal_autocheckpoint setup would WRITE to
         * the local image and is skipped; PRAGMA query_only=ON makes every
         * DML statement fail with SQLITE_READONLY ("attempt to write a
         * readonly database") while reads (including lazy hydration over
         * the replica store) work normally. */
        setup_rc = otsardb_exec(db,
                              "PRAGMA query_only=ON;",
                              nullptr,
                              nullptr,
                              &sqlite_error);
    } else {
        setup_rc = otsardb_exec(db,
                              "PRAGMA journal_mode=WAL;"
                              "PRAGMA synchronous=FULL;"
                              "PRAGMA wal_autocheckpoint=0;",
                              nullptr,
                              nullptr,
                              &sqlite_error);
    }
    if (setup_rc != OTSARDB_OK) {
        set_error(std::string(session->read_replica
                                  ? "failed to configure the read-only session: "
                                  : "failed to configure OtsarDB WAL durability: ") +
                  (sqlite_error ? sqlite_error : otsardb_errmsg(db)));
        otsardb_free(sqlite_error);
        otsardb_close(db);
        return OTSARDB_ERROR;
    }
    otsardb_free(sqlite_error);

    if (!session->read_replica) {
        std::string journal_mode;
        sqlite_error = nullptr;
        if (otsardb_exec(db,
                       "PRAGMA journal_mode;",
                       collect_single_text,
                       &journal_mode,
                       &sqlite_error) != OTSARDB_OK ||
            lower_copy(journal_mode) != "wal") {
            set_error(std::string("OtsarDB S3 requires WAL mode: ") +
                      (sqlite_error ? sqlite_error : "journal_mode did not become WAL"));
            otsardb_free(sqlite_error);
            otsardb_close(db);
            return OTSARDB_ERROR;
        }
        otsardb_free(sqlite_error);
    }

    sqlite3 *sqlite = otsardb_internal_sqlite_handle(db);
    if (!sqlite || sqlite3_set_authorizer(sqlite, durability_authorizer, session) != SQLITE_OK) {
        set_error("failed to install OtsarDB S3 durability guard");
        otsardb_close(db);
        return OTSARDB_ERROR;
    }

    /* (FRESH consistency mode): install the per-statement
     * pre-exec hook AFTER the open-time setup execs above (they must not
     * trigger a freshness check). Snapshot sessions never install it. */
    if (session->fresh_mode) {
        if (otsardb_internal_set_pre_exec_hook(db, session_fresh_pre_exec, session) !=
            OTSARDB_OK) {
            set_error("failed to install the FRESH consistency pre-exec hook");
            otsardb_close(db);
            return OTSARDB_ERROR;
        }
    }

    *out_db = db;
    return OTSARDB_OK;
}

/* — deterministic test hook. Opens a session over a
 * CALLER-PROVIDED object store (memory/fault stores in tests) instead of
 * building one from OTSARDB_S3_* env. The session BORROWS the store: it
 * is never destroyed by the session and must outlive it. Everything after
 * store construction is the same open_session_common path the real open
 * uses (head read, materialisation ladder, publisher, VFS, setup, FRESH
 * hook when OTSARDB_CONSISTENCY=fresh). Not part of the public contract. */
extern "C" int otsardb_s3_open_with_store(otsardb_object_store *store,
                                          int s3_cas_ok,
                                          const char *database_root,
                                          const char *database_id,
                                          const otsardb_s3_open_config *config,
                                          otsardb **out_db) {
    g_last_error.clear();
    if (!out_db) {
        set_error("output OtsarDB handle is null");
        return OTSARDB_ERROR;
    }
    *out_db = nullptr;
    if (!store || !database_root || !database_id || !database_id[0]) {
        set_error("otsardb_s3_open_with_store requires a store, database root and id");
        return OTSARDB_ERROR;
    }

    auto *session = new (std::nothrow) s3_database_session();
    if (!session) {
        set_error("out of memory creating S3 database session");
        return OTSARDB_ERROR;
    }
    session->store = store;
    session->store_borrowed = 1;

    {
        const char *lazy_env = env_nonempty("OTSARDB_LAZY_HYDRATION");
        session->lazy_enabled = !lazy_env || env_true("OTSARDB_LAZY_HYDRATION");
    }

    if (!make_execution_path(database_id, &session->local_path)) {
        set_error("failed to create disposable OtsarDB local execution path");
        destroy_session(session);
        return OTSARDB_ERROR;
    }
    remove_execution_files(session->local_path);

    otsardb_store_probe_report report{};
    report.s3_core_ok = 1;
    report.s3_cas_ok = s3_cas_ok;
    /* The borrowed store already carries its capabilities (memory_store). */
    if (store->capabilities & OTSARDB_STORE_CAP_ETAG) {
        report.capabilities = store->capabilities;
    }

    return open_session_common(session, database_root, database_id, &report, config,
                               out_db);
}

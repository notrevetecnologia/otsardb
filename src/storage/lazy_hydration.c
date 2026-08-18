#include "lazy_hydration.h"

#include "checkpoint_manifest.h"
#include "cipher.h"
#include "commit_manifest.h"
#include "db_metadata.h"
#include "recovery.h"
#include "segment.h"
#include "sha256.h"
#include "wal_batch.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define lazy_fileno _fileno
#define lazy_ftruncate _chsize_s
#else
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#define lazy_fileno fileno
#endif

/* ---------------------------------------------------------------------------
 * Gated timing diagnostics (OTSARDB_S3_TIMING=1 -> "otsardb: timing ..." on
 * stderr). Test harnesses filter "otsardb:" lines, so these never pollute
 * query output.
 * ------------------------------------------------------------------------- */

int otsardb_lazy_timing_enabled(void) {
    const char *value = getenv("OTSARDB_S3_TIMING");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

void otsardb_lazy_timing_note(const char *label, double milliseconds) {
    if (!otsardb_lazy_timing_enabled()) return;
    fprintf(stderr, "otsardb: timing %s %.1f ms\n", label, milliseconds);
}

/* ---------------------------------------------------------------------------
 * Per-read-path counters
 * ------------------------------------------------------------------------- */

/* OTSARDB_S3_READ_STATS=1 -> "otsardb: read-stats ..." at session close. */
static int lazy_read_stats_enabled(void) {
    const char *value = getenv("OTSARDB_S3_READ_STATS");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

int otsardb_lazy_hydration_stats_format(const otsardb_lazy_hydration_stats *stats,
                                      char *buf,
                                      size_t buf_size) {
    if (!stats) return 0;
    return snprintf(buf,
                    buf_size,
                    "otsardb: read-stats total_gets=%llu range_gets=%llu "
                    "bytes_fetched=%llu demand_misses=%llu prefetch_fetches=%llu "
                    "prefetch_hits=%llu coalesced_runs=%llu "
                    "sequential_streaks=%llu hydration_ms=%llu\n",
                    (unsigned long long)stats->total_gets,
                    (unsigned long long)stats->range_gets,
                    (unsigned long long)stats->bytes_fetched,
                    (unsigned long long)stats->demand_misses,
                    (unsigned long long)stats->prefetch_fetches,
                    (unsigned long long)stats->prefetch_hits,
                    (unsigned long long)stats->coalesced_runs,
                    (unsigned long long)stats->sequential_streaks,
                    (unsigned long long)stats->hydration_ms);
}

void otsardb_lazy_hydration_stats_print(const otsardb_lazy_hydration_stats *stats) {
    char line[512];
    if (otsardb_lazy_hydration_stats_format(stats, line, sizeof(line)) > 0) {
        fputs(line, stderr);
    }
}

/* the RAM tier line (kept separate from the canonical 0082
 * line, whose exact bytes the 0082 format test pins). */
int otsardb_lazy_hydration_stats_format_ram(const otsardb_lazy_hydration_stats *stats,
                                          char *buf,
                                          size_t buf_size) {
    if (!stats) return 0;
    return snprintf(buf,
                    buf_size,
                    "otsardb: read-stats ram_hits=%llu ram_misses=%llu\n",
                    (unsigned long long)stats->ram_hits,
                    (unsigned long long)stats->ram_misses);
}

/* ---------------------------------------------------------------------------
 * Range-GET coalescing window: pages of the SAME segment
 * whose frame blocks lie within OTSARDB_S3_COALESCE_MAX bytes of each other
 * are fetched in one byte-range GET covering both runs. Default 4096;
 * 0 = strict 0059 behavior (only consecutive page numbers merge).
 * ------------------------------------------------------------------------- */

#define LAZY_COALESCE_DEFAULT 4096u
#define LAZY_COALESCE_MAX 0x7FFFFFFF

static uint64_t lazy_coalesce_max(void) {
    const char *value = getenv("OTSARDB_S3_COALESCE_MAX");
    if (!value || value[0] == '\0') return LAZY_COALESCE_DEFAULT;
    char *end = NULL;
    long long n = strtoll(value, &end, 10);
    if (end == value || *end != '\0' || n < 0) return LAZY_COALESCE_DEFAULT;
    if (n == 0) return 0;
    if (n > LAZY_COALESCE_MAX) n = LAZY_COALESCE_MAX;
    return (uint64_t)n;
}

/* ---------------------------------------------------------------------------
 * RAM page cache
 *
 * A session-scoped in-memory tier holding hydrated page bytes
 * (page_no -> page_size bytes) with strict LRU eviction under
 * OTSARDB_S3_RAM_CACHE_MB (default 64, 0 = off). Every cached byte was
 * verified at fetch time (chain-authenticated manifest + per-block
 * structural validation + CRC-32 on the frame path; exact size + SHA-256 on
 * the whole-object path): the cache holds verified bytes only. The demand
 * path resolves every page against the RAM tier BEFORE the bitmap — a hit
 * skips the S3 fetch AND the decode, and the page is written into the local
 * image only when the bitmap says it is not yet materialised.
 *
 * Boundary (documented in the header): the cache serves the HYDRATION
 * layer's internal page resolution only. Pages are still materialised into
 * the local image before the VFS reads them (local_vfs is unchanged; the
 * VFS hook API has no page-bytes-out channel).
 *
 * All cache access happens under the hydrator mutex (the demand path, the
 * prefetch worker apply, the whole-object apply and the mark/rebuild paths
 * all hold it), so the tier needs no lock of its own.
 * ------------------------------------------------------------------------- */

#define LAZY_RAM_CACHE_DEFAULT_MB 64u
#define LAZY_RAM_CACHE_MAX_MB 65536u
/* Slot-pool cap (bookkeeping bound for absurd budgets); a budget above the
 * pool capacity simply evicts the LRU slot on store. */
#define LAZY_RAM_CACHE_POOL_MAX 4194304u

typedef struct lazy_cache_entry {
    uint32_t page_no;      /* 0 = free slot */
    unsigned char *data;   /* page_size bytes, owned */
    uint32_t hash_next;    /* hash chain, index+1 encoding (0 = none) */
    uint32_t lru_prev;     /* doubly-linked LRU list, index+1 (0 = none) */
    uint32_t lru_next;
} lazy_cache_entry;

typedef struct lazy_ram_cache {
    lazy_cache_entry *entries;
    size_t entry_count;    /* slot pool size */
    uint32_t *hash;        /* bucket heads (index+1, 0 = empty) */
    size_t hash_buckets;   /* power of two */
    uint64_t budget_bytes;
    uint64_t used_bytes;
    uint32_t lru_head;     /* MRU, index+1 (0 = empty) */
    uint32_t lru_tail;     /* LRU */
    uint32_t free_head;    /* free-slot list head, index+1 (0 = none) */
    uint32_t count;
} lazy_ram_cache;


#if defined(_WIN32)
static double monotonic_ms(void) {
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
#endif

/* ---------------------------------------------------------------------------
 * Small mutex wrapper (Windows CRITICAL_SECTION / POSIX pthread). Serialises
 * hydrator state (bitmap, segment index, failure flag) between the SQLite
 * thread and the lease-takeover rebuild.
 * ------------------------------------------------------------------------- */

typedef struct lazy_mutex {
#if defined(_WIN32)
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mutex;
#endif
} lazy_mutex;

static void lazy_mutex_init(lazy_mutex *m) {
#if defined(_WIN32)
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->mutex, NULL);
#endif
}

static void lazy_mutex_lock(lazy_mutex *m) {
#if defined(_WIN32)
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mutex);
#endif
}

static void lazy_mutex_unlock(lazy_mutex *m) {
#if defined(_WIN32)
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mutex);
#endif
}

static void lazy_mutex_destroy(lazy_mutex *m) {
#if defined(_WIN32)
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->mutex);
#endif
}

typedef struct lazy_cond {
#if defined(_WIN32)
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cond;
#endif
} lazy_cond;

static void lazy_cond_init(lazy_cond *c) {
#if defined(_WIN32)
    InitializeConditionVariable(&c->cv);
#else
    pthread_cond_init(&c->cond, NULL);
#endif
}

static void lazy_cond_wait(lazy_cond *c, lazy_mutex *m) {
#if defined(_WIN32)
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
#else
    pthread_cond_wait(&c->cond, &m->mutex);
#endif
}

static void lazy_cond_broadcast(lazy_cond *c) {
#if defined(_WIN32)
    WakeAllConditionVariable(&c->cv);
#else
    pthread_cond_broadcast(&c->cond);
#endif
}

static void lazy_cond_destroy(lazy_cond *c) {
#if defined(_WIN32)
    (void)c;
#else
    pthread_cond_destroy(&c->cond);
#endif
}

/* ---------------------------------------------------------------------------
 * Hydrator state
 * ------------------------------------------------------------------------- */

typedef struct lazy_unknown_ref {
    size_t manifest_index;
    uint32_t segment_index;
    char key[OTSARDB_MANIFEST_SEGMENT_KEY_MAX + 1];
    char sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    uint64_t size;
    int fetched;
} lazy_unknown_ref;

/* One prefetch task per KNOWN segment, newest-first (the same iteration
 * order as lazy_resolve_page). Workers fetch (GET + SHA-256/size verify)
 * and store the verified bytes; the requesting thread applies tasks in
 * ordinal order so an older segment can never overwrite a newer one. */
typedef struct lazy_task_ref {
    int format_version;                 /* manifest format version */
    char commit_id[OTSARDB_MANIFEST_ID_MAX + 1];
    char absolute_key[OTSARDB_KEY_MAX + 1];
    char sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    uint64_t size;
    uint32_t page_size;
    /* position of this segment in the hydrator's manifest
     * array (the deep copies own the pages/frame_offsets arrays). When
     * has_frame_offsets the segment's frames are fetched with byte-range
     * GETs on the requesting thread and the prefetch pool must never touch
     * the task. */
    size_t manifest_index;
    uint32_t segment_index;
    int has_frame_offsets;
} lazy_task_ref;

#define LAZY_TASK_IDLE 0
#define LAZY_TASK_FETCHING 1
#define LAZY_TASK_FETCHED 2
#define LAZY_TASK_APPLIED 3
#define LAZY_TASK_FAILED 4

typedef struct lazy_seg_task {
    lazy_task_ref ref;
    int state;                     /* LAZY_TASK_* */
    unsigned char *verified_bytes; /* owned by the task while fetched */
    size_t verified_size;
} lazy_seg_task;

/* Worker pool: W parallel fetchers (OTSARDB_LAZY_PARALLEL,
 * default 4, clamped 1..8) started on the first page miss. All pool state is
 * guarded by the hydrator mutex; the requesting thread waits on the condvar
 * for the task it needs and applies tasks strictly in ordinal order. */
#define LAZY_POOL_MAX 8u
#define LAZY_POOL_DEFAULT 4u

/* Sequential read-ahead . In the miss path, when recent
 * demand misses are consecutive page numbers (the sequential-scan
 * signature), the pool prefetches the NEXT K pages of the same segment in
 * the background: one coalesced byte-range GET per contiguous run of pages
 * that exist in the claiming segment's page set (resolved via the page
 * index) and are not yet materialised. K = OTSARDB_LAZY_PREFETCH (default 8,
 * clamped 1..64; 0 or negative = off). Prefetch failures are dropped
 * silently (the next explicit miss re-fetches); only demand-fetch failures
 * are sticky. Whole-object (non-frame-offset) segments are already eagerly
 * fetched by the pool in ordinal order, so prefetch jobs only target
 * frame-offset segments (serviced inline on demand). Random access never
 * builds a streak and never schedules a job. */
#define LAZY_PREFETCH_DEFAULT 8
#define LAZY_PREFETCH_MAX 64
#define LAZY_PREFETCH_JOB_MAX 64
#define LAZY_PREFETCH_MIN_STREAK 2

#define LAZY_PREFETCH_PENDING 0
#define LAZY_PREFETCH_FETCHING 1
#define LAZY_PREFETCH_DONE 2
#define LAZY_PREFETCH_DROPPED 3

/* One job = one contiguous run of pages [first_page, last_page] whose
 * blocks are consecutive in the owning segment (coalesced into a single
 * range GET). Workers apply the fetched blocks on the requesting thread's
 * behalf, re-checking the bitmap and newest-claimant rules at apply time. */
typedef struct lazy_prefetch_job {
    size_t ordinal;          /* h->tasks index of the owning segment */
    uint32_t first_page;
    uint32_t last_page;
    int state;               /* LAZY_PREFETCH_* */
} lazy_prefetch_job;

struct otsardb_lazy_hydrator {
    otsardb_object_store *store;
    char database_root[OTSARDB_KEY_MAX + 1];
    char database_path[1024];

    uint32_t page_size;
    uint64_t database_size_pages;
    uint64_t covered_size_pages; /* 0 = no checkpoint base */

    /* (LH-2): number of pages in [1, covered_size_pages]
     * (clamped to database_size_pages) that NO segment newer than the
     * checkpoint claims. Those pages are materialised by the checkpoint base
     * image (its bytes are their newest state) but are deliberately not
     * pre-marked, so completeness accounting must count them explicitly.
     * Computed once at restore, after lazy_build_tasks. */
    uint64_t base_complete_pages;

    /* Validated manifests, NEWEST first. Each owns its pages/frame_offsets
     * arrays (otsardb_commit_manifest_free at destroy). */
    otsardb_commit_manifest *manifests;
    size_t manifest_count;

    /* parsed "segment_enc" sets, one per manifest (aligned
     * by index with h->manifests). All fields are inline arrays — no owned
     * pointers. */
    otsardb_segment_enc_set *manifest_enc;

    /* cipher used to decrypt encrypted segments on fetch
     * (borrowed from the session; NULL = plaintext path). */
    const otsardb_cipher *cipher;

    /* Segments whose page set is unknown (v1-v3, or v4 with empty pages):
     * fetched in full the first time any page miss cannot be resolved. */
    lazy_unknown_ref *unknown_segments;
    size_t unknown_count;

    unsigned char *bitmap;       /* 1 bit per page, 0 = not materialised */
    size_t bitmap_bytes;
    uint64_t marked_pages;       /* count of set bits (completeness probe) */

    int unknown_fetched;         /* unknown segments fetched at least once */
    int fetch_failed;            /* sticky fail-closed flag */
    uint64_t generation;         /* bumped on every (re)build; waiters on a
                                    stale generation fail closed */

    /* Prefetch pool. */
    lazy_seg_task *tasks;
    size_t task_count;
    size_t next_apply;           /* first task not yet applied (ordinal) */
    int pool_started;
    int pool_workers;
    int pool_stop;
#if defined(_WIN32)
    uintptr_t pool_threads[LAZY_POOL_MAX];
#else
    pthread_t pool_threads[LAZY_POOL_MAX];
#endif
    lazy_cond cond;
    lazy_mutex mutex;

    /* Sequential read-ahead . Guarded by the hydrator mutex
     * like every other pool field. */
    uint32_t last_miss_page;   /* last demand-miss page (0 = none yet) */
    uint32_t seq_streak;       /* consecutive sequential demand misses */
    lazy_prefetch_job *prefetch_jobs;
    size_t prefetch_job_count;
    size_t prefetch_job_capacity;

    /* Per-read-path counters. Bumped with
     * relaxed atomics from every thread (demand, prefetch pool, coalesced
     * GET workers) so no lock is needed on the hot fetch paths; read at
     * session close (pool joined) or via otsardb_lazy_hydration_stats_get.
     * Never reset by lazy_release_state: they are session-scoped and
     * survive an in-place rebuild (lease takeover). */
    otsardb_lazy_hydration_stats stats;

    /* RAM page cache. All
     * access happens under the hydrator mutex (every fetch/apply/mark path
     * holds it). Cleared by lazy_release_state: cached bytes are only valid
     * for the hydrator's restore-time head state. */
    lazy_ram_cache cache;
};
static uint64_t lazy_ram_cache_mb(void) {
    const char *value = getenv("OTSARDB_S3_RAM_CACHE_MB");
    if (!value || value[0] == '\0') return LAZY_RAM_CACHE_DEFAULT_MB;
    char *end = NULL;
    long long n = strtoll(value, &end, 10);
    if (end == value || *end != '\0' || n < 0) return LAZY_RAM_CACHE_DEFAULT_MB;
    if (n == 0) return 0; /* 0 = off */
    if (n > (long long)LAZY_RAM_CACHE_MAX_MB) n = (long long)LAZY_RAM_CACHE_MAX_MB;
    return (uint64_t)n;
}

static uint32_t lazy_cache_hash(const lazy_ram_cache *c, uint32_t page_no) {
    uint64_t x = (uint64_t)page_no * 2654435761ull;
    return (uint32_t)(x >> 32) & (uint32_t)(c->hash_buckets - 1u);
}

/* Initialise the tier (restore time, hydrator mutex held, page_size known).
 * Allocation failure disables the tier gracefully (budget = 0). */
static void lazy_cache_init(otsardb_lazy_hydrator *h) {
    if (h->cache.entries) return;
    uint64_t mb = lazy_ram_cache_mb();
    if (mb == 0 || h->page_size == 0) return;
    h->cache.budget_bytes = mb * 1024ull * 1024ull;
    uint64_t want = h->cache.budget_bytes / (uint64_t)h->page_size + 1u;
    if (want > LAZY_RAM_CACHE_POOL_MAX) want = LAZY_RAM_CACHE_POOL_MAX;
    size_t pool = (size_t)want;
    lazy_cache_entry *entries =
        (lazy_cache_entry *)calloc(pool ? pool : 1u, sizeof(*entries));
    size_t buckets = 1u;
    while (buckets < pool * 2u && buckets <= (1u << 22u)) buckets <<= 1u;
    uint32_t *hash = (uint32_t *)calloc(buckets, sizeof(*hash));
    if (!entries || !hash) {
        free(entries);
        free(hash);
        h->cache.budget_bytes = 0;
        fprintf(stderr,
                "otsardb: lazy hydration: RAM page cache disabled "
                "(allocation failure)\n");
        return;
    }
    h->cache.entries = entries;
    h->cache.entry_count = pool;
    h->cache.hash = hash;
    h->cache.hash_buckets = buckets;
    h->cache.free_head = 1u; /* first free slot: index 0 */
    for (size_t i = 0; i < pool; ++i) {
        entries[i].hash_next = (uint32_t)(i + 1u < pool ? i + 2u : 0u);
    }
    if (otsardb_lazy_timing_enabled()) {
        fprintf(stderr,
                "otsardb: timing lazy RAM cache budget=%llu bytes pool=%zu\n",
                (unsigned long long)h->cache.budget_bytes, pool);
    }
}

/* Free everything (rebuild/destroy; also the mark_all invalidation). */
static void lazy_cache_clear(otsardb_lazy_hydrator *h) {
    lazy_ram_cache *c = &h->cache;
    if (c->entries) {
        for (size_t i = 0; i < c->entry_count; ++i) free(c->entries[i].data);
    }
    free(c->entries);
    free(c->hash);
    memset(c, 0, sizeof(*c));
}

/* Evict the LRU entry; returns the recycled slot (index+1) or 0. */
static uint32_t lazy_cache_evict_lru(otsardb_lazy_hydrator *h) {
    lazy_ram_cache *c = &h->cache;
    uint32_t slot = c->lru_tail;
    if (slot == 0) return 0;
    lazy_cache_entry *e = &c->entries[slot - 1u];
    c->lru_tail = e->lru_prev;
    if (c->lru_tail) {
        c->entries[c->lru_tail - 1u].lru_next = 0;
    } else {
        c->lru_head = 0;
    }
    uint32_t bucket = lazy_cache_hash(c, e->page_no);
    uint32_t *link = &c->hash[bucket];
    while (*link) {
        if (*link == slot) {
            *link = e->hash_next;
            break;
        }
        link = &c->entries[*link - 1u].hash_next;
    }
    free(e->data);
    e->data = NULL;
    e->page_no = 0;
    e->lru_prev = 0;
    e->lru_next = 0;
    /* Recycle the slot onto the free list (hash_next is the free link). */
    e->hash_next = c->free_head;
    c->free_head = slot;
    if (c->used_bytes >= (uint64_t)h->page_size) {
        c->used_bytes -= (uint64_t)h->page_size;
    } else {
        c->used_bytes = 0;
    }
    --c->count;
    return slot;
}

/* Look up one page; on a hit `out` receives the bytes and the entry moves
 * to the MRU end. Returns 1 on hit, 0 on miss (or tier disabled). */
static int lazy_cache_lookup(otsardb_lazy_hydrator *h,
                             uint32_t page_no,
                             const unsigned char **out) {
    lazy_ram_cache *c = &h->cache;
    if (c->budget_bytes == 0 || !c->hash) return 0;
    uint32_t slot = c->hash[lazy_cache_hash(c, page_no)];
    while (slot) {
        lazy_cache_entry *e = &c->entries[slot - 1u];
        if (e->page_no == page_no) {
            if (out) *out = e->data;
            if (c->lru_head != slot) {
                if (e->lru_prev) {
                    c->entries[e->lru_prev - 1u].lru_next = e->lru_next;
                } else {
                    c->lru_head = e->lru_next;
                }
                if (e->lru_next) {
                    c->entries[e->lru_next - 1u].lru_prev = e->lru_prev;
                } else {
                    c->lru_tail = e->lru_prev;
                }
                e->lru_prev = 0;
                e->lru_next = c->lru_head;
                if (c->lru_head) c->entries[c->lru_head - 1u].lru_prev = slot;
                c->lru_head = slot;
                if (c->lru_tail == 0) c->lru_tail = slot;
            }
            return 1;
        }
        slot = e->hash_next;
    }
    return 0;
}

/* Presence probe without an LRU touch (prefetch scheduling filter). */
static int lazy_cache_contains(otsardb_lazy_hydrator *h, uint32_t page_no) {
    lazy_ram_cache *c = &h->cache;
    if (c->budget_bytes == 0 || !c->hash) return 0;
    uint32_t slot = c->hash[lazy_cache_hash(c, page_no)];
    while (slot) {
        if (c->entries[slot - 1u].page_no == page_no) return 1;
        slot = c->entries[slot - 1u].hash_next;
    }
    return 0;
}

/* Store one verified page (page_size bytes) into the tier. Evicts strict
 * LRU until the budget fits; a single page larger than the budget is still
 * stored (soft bound). Re-storing an existing page replaces its bytes and
 * touches it (the newest claimant never changes mid-session). */
static void lazy_cache_store(otsardb_lazy_hydrator *h,
                             uint32_t page_no,
                             const unsigned char *data) {
    lazy_ram_cache *c = &h->cache;
    if (c->budget_bytes == 0 || !c->hash || !data || page_no == 0) return;
    uint32_t bucket = lazy_cache_hash(c, page_no);
    uint32_t slot = c->hash[bucket];
    while (slot) {
        if (c->entries[slot - 1u].page_no == page_no) {
            memcpy(c->entries[slot - 1u].data, data, h->page_size);
            lazy_cache_lookup(h, page_no, NULL);
            return;
        }
        slot = c->entries[slot - 1u].hash_next;
    }
    while (c->count > 0 &&
           c->used_bytes + (uint64_t)h->page_size > c->budget_bytes) {
        lazy_cache_evict_lru(h);
    }
    /* A single page larger than the whole budget is still stored (the
     * budget is a soft bound); when no free slot exists (pool capped below
     * the budget), evict the LRU to recycle one. */
    if (c->free_head == 0 && c->lru_tail != 0) {
        lazy_cache_evict_lru(h);
    }
    if (c->free_head == 0) return;
    slot = c->free_head;
    c->free_head = c->entries[slot - 1u].hash_next;
    lazy_cache_entry *e = &c->entries[slot - 1u];
    e->page_no = page_no;
    e->data = (unsigned char *)malloc(h->page_size ? h->page_size : 1u);
    if (!e->data) {
        e->page_no = 0;
        e->hash_next = c->free_head;
        c->free_head = slot;
        return;
    }
    memcpy(e->data, data, h->page_size);
    e->hash_next = c->hash[bucket];
    c->hash[bucket] = slot;
    e->lru_prev = 0;
    e->lru_next = c->lru_head;
    if (c->lru_head) c->entries[c->lru_head - 1u].lru_prev = slot;
    c->lru_head = slot;
    if (c->lru_tail == 0) c->lru_tail = slot;
    c->used_bytes += (uint64_t)h->page_size;
    ++c->count;
}

/* Remove one page (SQLite wrote the image — the cached copy is superseded). */
static void lazy_cache_remove(otsardb_lazy_hydrator *h, uint32_t page_no) {
    lazy_ram_cache *c = &h->cache;
    if (c->budget_bytes == 0 || !c->hash || page_no == 0) return;
    uint32_t bucket = lazy_cache_hash(c, page_no);
    uint32_t *link = &c->hash[bucket];
    while (*link) {
        uint32_t slot = *link;
        lazy_cache_entry *e = &c->entries[slot - 1u];
        if (e->page_no == page_no) {
            *link = e->hash_next;
            if (e->lru_prev) {
                c->entries[e->lru_prev - 1u].lru_next = e->lru_next;
            } else {
                c->lru_head = e->lru_next;
            }
            if (e->lru_next) {
                c->entries[e->lru_next - 1u].lru_prev = e->lru_prev;
            } else {
                c->lru_tail = e->lru_prev;
            }
            free(e->data);
            e->data = NULL;
            e->page_no = 0;
            e->lru_prev = 0;
            e->lru_next = 0;
            e->hash_next = c->free_head;
            c->free_head = slot;
            if (c->used_bytes >= (uint64_t)h->page_size) {
                c->used_bytes -= (uint64_t)h->page_size;
            } else {
                c->used_bytes = 0;
            }
            --c->count;
            return;
        }
        link = &e->hash_next;
    }
}

/* Write one page's bytes into the local image at the page's offset. */
static int lazy_write_page_file(otsardb_lazy_hydrator *h,
                                uint32_t page_no,
                                const unsigned char *page) {
    uint64_t offset = ((uint64_t)page_no - 1u) * (uint64_t)h->page_size;
    FILE *file = NULL;
    if (offset <= INT64_MAX && (file = fopen(h->database_path, "r+b")) != NULL) {
#if defined(_WIN32)
        int seek_ok = _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
        int seek_ok = fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
        int write_ok =
            seek_ok && fwrite(page, 1, h->page_size, file) == h->page_size;
        int close_ok = fclose(file) == 0;
        return write_ok && close_ok;
    }
    if (file) fclose(file);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Atomic counter add . InterlockedExchangeAdd64 on Windows,
 * __atomic_fetch_add elsewhere. The stats struct is public (header); the
 * counters are uint64_t members, 8-byte aligned on every supported target.
 * ------------------------------------------------------------------------- */

#if defined(_WIN32)
static void lazy_stats_add64(uint64_t *dst, uint64_t value) {
    if (value) {
        InterlockedExchangeAdd64((volatile LONG64 *)dst, (LONG64)value);
    }
}
#else
static void lazy_stats_add64(uint64_t *dst, uint64_t value) {
    if (value) {
        __atomic_fetch_add((volatile unsigned long long *)dst,
                           (unsigned long long)value, __ATOMIC_RELAXED);
    }
}
#endif

/* Snapshot the session counters (the struct definition lives above). */
int otsardb_lazy_hydration_stats_get(const otsardb_lazy_hydrator *h,
                                   otsardb_lazy_hydration_stats *out) {
    if (!h || !out) return 0;
    *out = h->stats;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Bitmap helpers (page numbers are 1-based; page 0 is never stored)
 * ------------------------------------------------------------------------- */

static void bitmap_set(otsardb_lazy_hydrator *h, uint32_t page_no) {
    if (page_no == 0 || (uint64_t)page_no > h->database_size_pages) return;
    size_t bit = (size_t)page_no - 1u;
    if (!(h->bitmap[bit >> 3] & (unsigned char)(1u << (bit & 7u)))) {
        h->bitmap[bit >> 3] |= (unsigned char)(1u << (bit & 7u));
        ++h->marked_pages;
    }
}

static int bitmap_get(const otsardb_lazy_hydrator *h, uint32_t page_no) {
    if (page_no == 0 || (uint64_t)page_no > h->database_size_pages) return 0;
    size_t bit = (size_t)page_no - 1u;
    return (h->bitmap[bit >> 3] & (unsigned char)(1u << (bit & 7u))) != 0;
}

static void bitmap_clear_range(otsardb_lazy_hydrator *h, uint64_t first, uint64_t last) {
    if (first == 0) first = 1;
    if (last > h->database_size_pages) last = h->database_size_pages;
    for (uint64_t p = first; p <= last; ++p) {
        size_t bit = (size_t)p - 1u;
        if (h->bitmap[bit >> 3] & (unsigned char)(1u << (bit & 7u))) {
            h->bitmap[bit >> 3] &= (unsigned char)~(1u << (bit & 7u));
            --h->marked_pages;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Object key handling (mirrors recovery.c)
 * ------------------------------------------------------------------------- */

static int lazy_root_join(const char *root, const char *suffix, char *out, size_t out_size) {
    if (!root || !suffix || !out || out_size == 0) return 0;
    size_t root_len = strlen(root);
    int needs_slash = root_len > 0 && root[root_len - 1] != '/';
    int n = snprintf(out, out_size, "%s%s%s", root, needs_slash ? "/" : "", suffix);
    return n > 0 && (size_t)n < out_size;
}

static int segment_absolute_key(const otsardb_lazy_hydrator *h,
                                uint32_t format_version,
                                const char *key,
                                char *out,
                                size_t out_size) {
    if (format_version >= 3) {
        return lazy_root_join(h->database_root, key, out, out_size);
    }
    /* Legacy v1/v2 flat segment_key is already an absolute object key. */
    size_t len = strlen(key);
    if (len >= out_size) return 0;
    memcpy(out, key, len + 1u);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Manifest deep copy (a visitor manifest is transient)
 * ------------------------------------------------------------------------- */

static int manifest_deep_copy(const otsardb_commit_manifest *src,
                              otsardb_commit_manifest *dst) {
    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, sizeof(*dst));
    for (uint32_t i = 0; i < src->segment_count; ++i) {
        dst->segments[i].pages = NULL;
        dst->segments[i].frame_offsets = NULL;
        if (src->segments[i].page_count > 0) {
            size_t bytes = (size_t)src->segments[i].page_count * sizeof(uint32_t);
            dst->segments[i].pages = (uint32_t *)malloc(bytes);
            if (!dst->segments[i].pages) {
                otsardb_commit_manifest_free(dst);
                return 0;
            }
            memcpy(dst->segments[i].pages, src->segments[i].pages, bytes);
            if (src->segments[i].frame_offsets) {
                dst->segments[i].frame_offsets =
                    (uint64_t *)malloc((size_t)src->segments[i].page_count * sizeof(uint64_t));
                if (!dst->segments[i].frame_offsets) {
                    otsardb_commit_manifest_free(dst);
                    return 0;
                }
                memcpy(dst->segments[i].frame_offsets,
                       src->segments[i].frame_offsets,
                       (size_t)src->segments[i].page_count * sizeof(uint64_t));
            }
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Chain walk visitor: keep a deep copy, collect unknown-page segments
 * ------------------------------------------------------------------------- */

typedef struct lazy_collect_ctx {
    otsardb_lazy_hydrator *hydrator;
    uint64_t covered_generation;
    int ok;
} lazy_collect_ctx;

static int lazy_chain_visitor(void *ctx,
                              const char *manifest_key,
                              const char *expected_sha256,
                              const otsardb_commit_manifest *manifest,
                              const otsardb_segment_enc_set *enc) {
    (void)manifest_key;
    (void)expected_sha256;
    lazy_collect_ctx *collect = (lazy_collect_ctx *)ctx;
    otsardb_lazy_hydrator *h = collect->hydrator;

    /* Manifests at or below the checkpoint's covered generation are fully
     * covered by the base image: their segments are never needed. */
    int covered = collect->covered_generation != 0 &&
                  manifest->generation <= collect->covered_generation;

    otsardb_commit_manifest copy;
    if (!manifest_deep_copy(manifest, &copy)) {
        collect->ok = 0;
        return 0;
    }

    if (covered) {
        /* Drop segment data the base image already covers. */
        for (uint32_t i = 0; i < copy.segment_count; ++i) {
            free(copy.segments[i].pages);
            copy.segments[i].pages = NULL;
            free(copy.segments[i].frame_offsets);
            copy.segments[i].frame_offsets = NULL;
            copy.segments[i].page_count = 0;
        }
    } else {
        for (uint32_t i = 0; i < copy.segment_count; ++i) {
            if (copy.segments[i].page_count == 0) {
                otsardb_manifest_segment_ref *ref = &copy.segments[i];
                lazy_unknown_ref *grown = (lazy_unknown_ref *)realloc(
                    h->unknown_segments,
                    (h->unknown_count + 1u) * sizeof(*h->unknown_segments));
                if (!grown) {
                    otsardb_commit_manifest_free(&copy);
                    collect->ok = 0;
                    return 0;
                }
                h->unknown_segments = grown;
                lazy_unknown_ref *u = &h->unknown_segments[h->unknown_count];
                memset(u, 0, sizeof(*u));
                u->manifest_index = h->manifest_count;
                u->segment_index = i;
                snprintf(u->key, sizeof(u->key), "%s", ref->key);
                snprintf(u->sha256, sizeof(u->sha256), "%s", ref->sha256);
                u->size = ref->size;
                ++h->unknown_count;
            }
        }
    }

    otsardb_commit_manifest *grown = (otsardb_commit_manifest *)realloc(
        h->manifests, (h->manifest_count + 1u) * sizeof(*h->manifests));
    if (!grown) {
        otsardb_commit_manifest_free(&copy);
        collect->ok = 0;
        return 0;
    }
    otsardb_segment_enc_set *enc_grown = (otsardb_segment_enc_set *)realloc(
        h->manifest_enc, (h->manifest_count + 1u) * sizeof(*h->manifest_enc));
    if (!enc_grown) {
        otsardb_commit_manifest_free(&copy);
        collect->ok = 0;
        return 0;
    }
    h->manifests = grown;
    h->manifests[h->manifest_count] = copy;
    h->manifest_enc = enc_grown;
    if (enc) {
        h->manifest_enc[h->manifest_count] = *enc;
    } else {
        memset(&h->manifest_enc[h->manifest_count], 0, sizeof(h->manifest_enc[0]));
    }
    ++h->manifest_count;
    if (otsardb_lazy_timing_enabled()) {
        fprintf(stderr, "otsardb: timing lazy collect gen=%llu count=%zu\n",
                (unsigned long long)manifest->generation, h->manifest_count);
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Parallel chain fetch 
 *
 * The manifest chain keys are deterministic when every commit of the chain
 * shares the same WAL salts: commit_id is "w<salt1><salt2>-<index>" with
 * index == generation. When the head's commit_id parses and every fetched
 * manifest confirms the pattern (generation == index, parent ids match the
 * deterministic neighbours, hash links verify), the whole chain is fetched
 * in parallel windows and validated with EXACTLY the same checks as the
 * sequential walk. Any deviation falls back to otsardb_recovery_walk_chain.
 * ------------------------------------------------------------------------- */

#define LAZY_PARALLEL_CHAIN_MAX 16384u
#define LAZY_PARALLEL_CHAIN_WINDOW 16u

typedef struct lazy_chain_slot {
    char key[OTSARDB_KEY_MAX + 1];
    otsardb_commit_manifest manifest; /* decoded, zeroed when !ok */
    otsardb_segment_enc_set enc;      /* parsed "segment_enc" */
    char sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    int ok; /* fetched and decoded */
} lazy_chain_slot;

typedef struct lazy_chain_job {
    otsardb_object_store *store;
    lazy_chain_slot *slot;
} lazy_chain_job;

#if defined(_WIN32)
static unsigned __stdcall lazy_chain_worker(void *opaque)
#else
static void *lazy_chain_worker(void *opaque)
#endif
{
    lazy_chain_job *job = (lazy_chain_job *)opaque;
    otsardb_store_object object = {0};
    job->slot->ok = 0;
    if (otsardb_store_get(job->store, job->slot->key, &object) == OTSARDB_STORE_OK &&
        otsardb_sha256_hex_data(object.data, object.size, job->slot->sha256) &&
        otsardb_commit_manifest_decode_json((const char *)object.data,
                                          object.size,
                                          &job->slot->manifest) &&
        otsardb_segment_enc_parse_json((const char *)object.data,
                                     object.size,
                                     &job->slot->enc)) {
        job->slot->ok = 1;
    }
    otsardb_store_release_object(job->store, &object);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* Fast path: fetch every manifest in parallel, validate the chain
 * deterministically, feed the visitor newest-first. Returns 1 when the fast
 * path fully replaced the sequential walk; 0 to fall back. */
static int lazy_try_parallel_chain(otsardb_object_store *store,
                                   const char *database_root,
                                   const otsardb_db_metadata *metadata,
                                   const otsardb_head *head,
                                   otsardb_chain_visitor visitor,
                                   void *visitor_ctx) {
    unsigned int salt1 = 0;
    unsigned int salt2 = 0;
    unsigned int head_index = 0;
    char trailing = '\0';
    int matched = sscanf(head->commit_id, "w%8x%8x-%8x%c", &salt1, &salt2, &head_index, &trailing);
    if (matched != 3) return 0;
    if (head->generation == 0 || head->generation > LAZY_PARALLEL_CHAIN_MAX ||
        head_index != (unsigned int)head->generation) {
        return 0;
    }

    const uint64_t count = head->generation;
    lazy_chain_slot *slots = (lazy_chain_slot *)calloc((size_t)count, sizeof(*slots));
    if (!slots) return 0;

    for (uint64_t k = 1; k <= count; ++k) {
        int n = snprintf(slots[k - 1].key,
                         sizeof(slots[k - 1].key),
                         "%s/commits/w%08x%08x-%08x.json",
                         database_root, salt1, salt2, (unsigned int)k);
        if (n <= 0 || (size_t)n >= sizeof(slots[k - 1].key)) {
            free(slots);
            return 0;
        }
    }

    int ok = 1;
    int visitor_aborted = 0;
    lazy_chain_job jobs[LAZY_PARALLEL_CHAIN_WINDOW];
#if defined(_WIN32)
    uintptr_t handles[LAZY_PARALLEL_CHAIN_WINDOW];
#else
    pthread_t handles[LAZY_PARALLEL_CHAIN_WINDOW];
#endif
    for (uint64_t start = 1; start <= count && ok; start += LAZY_PARALLEL_CHAIN_WINDOW) {
        uint64_t window = count - start + 1u;
        if (window > LAZY_PARALLEL_CHAIN_WINDOW) window = LAZY_PARALLEL_CHAIN_WINDOW;
        for (uint64_t i = 0; i < window; ++i) {
            jobs[i].store = store;
            jobs[i].slot = &slots[start - 1u + i];
#if defined(_WIN32)
            handles[i] = _beginthreadex(NULL, 0, lazy_chain_worker, &jobs[i], 0, NULL);
            if (handles[i] == 0) {
                jobs[i].slot->ok = 0;
                handles[i] = 0;
            }
#else
            if (pthread_create(&handles[i], NULL, lazy_chain_worker, &jobs[i]) != 0) {
                jobs[i].slot->ok = 0;
            }
#endif
        }
        for (uint64_t i = 0; i < window; ++i) {
#if defined(_WIN32)
            if (handles[i]) {
                WaitForSingleObject((HANDLE)handles[i], INFINITE);
                CloseHandle((HANDLE)handles[i]);
            }
#else
            pthread_join(handles[i], NULL);
#endif
            if (!jobs[i].slot->ok) ok = 0;
        }
    }

    /* Validate the deterministic chain (identical checks to the sequential
     * walk: hash links, generation/commit-id continuity, database/page
     * anchor, genesis rules). */
    for (uint64_t k = 1; k <= count && ok; ++k) {
        lazy_chain_slot *slot = &slots[k - 1];
        if (!slot->ok) {
            ok = 0;
            break;
        }
        char expected_id[OTSARDB_COMMIT_ID_MAX + 1];
        int n = snprintf(expected_id,
                         sizeof(expected_id),
                         "w%08x%08x-%08x",
                         salt1, salt2, (unsigned int)k);
        if (n <= 0 || (size_t)n >= sizeof(expected_id) ||
            slot->manifest.generation != k ||
            strcmp(slot->manifest.commit_id, expected_id) != 0 ||
            slot->manifest.parent_generation + 1u != slot->manifest.generation ||
            strcmp(slot->manifest.database_id, metadata->database_id) != 0 ||
            slot->manifest.page_size != metadata->page_size) {
            ok = 0;
            break;
        }
        if (k == 1) {
            if (slot->manifest.parent_commit_id[0] != '\0' ||
                slot->manifest.parent_manifest_sha256[0] != '\0') {
                ok = 0;
            }
            break;
        }
        char parent_expected[OTSARDB_COMMIT_ID_MAX + 1];
        int pn = snprintf(parent_expected,
                          sizeof(parent_expected),
                          "w%08x%08x-%08x",
                          salt1, salt2, (unsigned int)(k - 1));
        if (pn <= 0 || (size_t)pn >= sizeof(parent_expected) ||
            strcmp(slot->manifest.parent_commit_id, parent_expected) != 0) {
            ok = 0;
            break;
        }
    }
    /* Hash links (the sha of manifest k must equal manifest k+1's
     * parent_manifest_sha256, or head's commit_sha256 for k == count). */
    for (uint64_t k = count; k >= 1 && ok; --k) {
        const char *expected = NULL;
        if (k == count) {
            expected = head->commit_sha256;
        } else {
            if (slots[k].manifest.format_version < 2 ||
                strlen(slots[k].manifest.parent_manifest_sha256) !=
                    OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
                ok = 0;
                break;
            }
            expected = slots[k].manifest.parent_manifest_sha256;
        }
        if (strcmp(slots[k - 1].sha256, expected) != 0) {
            ok = 0;
            break;
        }
    }

    if (ok) {
        /* Feed the visitor newest-first, like the sequential walk. A visitor
         * abort is NOT a fallback condition (the hydrator state is already
         * partially populated): report it so the restore fails cleanly. */
        for (uint64_t k = count; k >= 1 && ok; --k) {
            const char *expected = k == count
                                       ? head->commit_sha256
                                       : slots[k].manifest.parent_manifest_sha256;
            if (visitor &&
                !visitor(visitor_ctx, slots[k - 1].key, expected, &slots[k - 1].manifest,
                         &slots[k - 1].enc)) {
                ok = 0;
                visitor_aborted = 1;
            }
        }
    }

    for (uint64_t k = 1; k <= count; ++k) {
        otsardb_commit_manifest_free(&slots[k - 1].manifest);
    }
    free(slots);
    return visitor_aborted ? -1 : ok;
}

/* ---------------------------------------------------------------------------
 * List-based parallel chain fetch 
 *
 * The salt-based fast path only covers single-WAL chains. For multi-salt
 * chains (e.g. one CLI session per commit, each with its own WAL salts) the
 * manifest keys are NOT derivable, but they ARE discoverable: list the
 * "<root>/commits/" prefix (one request), fetch every manifest in parallel,
 * then validate the chain by following parent links through the fetched set
 * with the SAME checks as the sequential walk. The listing is only a
 * discovery mechanism â€” the chain is defined and authenticated by HEAD and
 * the parent hash links, never by the listing.
 * ------------------------------------------------------------------------- */

typedef struct lazy_list_slot {
    char key[OTSARDB_KEY_MAX + 1];
    otsardb_commit_manifest manifest; /* decoded, zeroed when !ok */
    otsardb_segment_enc_set enc;      /* parsed "segment_enc";
                                       layout must match lazy_chain_slot (the
                                       list path casts slots to it) */
    char sha256[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
    int ok;
} lazy_list_slot;

typedef struct lazy_list_collect {
    char **keys;
    size_t count;
    size_t capacity;
    int failed;
} lazy_list_collect;

static int lazy_list_callback(const char *key, void *ctx) {
    lazy_list_collect *collect = (lazy_list_collect *)ctx;
    if (!key || key[0] == '\0') return 0;
    if (collect->count >= 1000000u) {
        collect->failed = 1;
        return 1;
    }
    if (collect->count == collect->capacity) {
        size_t next_capacity = collect->capacity ? collect->capacity * 2u : 64u;
        char **next = (char **)realloc(collect->keys, next_capacity * sizeof(*next));
        if (!next) {
            collect->failed = 1;
            return 1;
        }
        collect->keys = next;
        collect->capacity = next_capacity;
    }
    size_t len = strlen(key);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) {
        collect->failed = 1;
        return 1;
    }
    memcpy(copy, key, len + 1u);
    collect->keys[collect->count++] = copy;
    return 0;
}

typedef struct lazy_list_cmp_ctx {
    lazy_list_slot *slots;
} lazy_list_cmp_ctx;

static int lazy_list_cmp_impl(const lazy_list_slot *slots, const size_t *a, const size_t *b) {
    return strcmp(slots[*a].key, slots[*b].key);
}

/* Portable bottom-up merge sort over the size_t index array. Replaces
 * qsort_r/qsort_s, whose signatures differ between MSVC, glibc and BSD/macOS
 * (and glibc's needs _GNU_SOURCE) — this has zero platform dependencies and
 * is deterministic. Stable O(n log n); buf must hold n * sizeof(size_t). */
static void lazy_list_sort(size_t *order, size_t n,
                           const lazy_list_slot *slots, size_t *buf) {
    if (n < 2) return;
    size_t width = 1;
    while (width < n) {
        size_t left = 0;
        while (left < n) {
            size_t mid = left + width;
            if (mid >= n) break;
            size_t right = mid + width;
            if (right > n) right = n;
            size_t i = left, j = mid, k = left;
            while (i < mid && j < right) {
                if (lazy_list_cmp_impl(slots, &order[i], &order[j]) <= 0)
                    buf[k++] = order[i++];
                else
                    buf[k++] = order[j++];
            }
            while (i < mid) buf[k++] = order[i++];
            while (j < right) buf[k++] = order[j++];
            for (size_t x = left; x < right; ++x) order[x] = buf[x];
            left = right;
        }
        width <<= 1;
    }
}

/* "<root>/commits/<commit_id>.json" (mirrors recovery's manifest keys). */
static int build_lazy_manifest_key(const char *database_root,
                                   const char *commit_id,
                                   char *out,
                                   size_t out_size) {
    char suffix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(suffix, sizeof(suffix), "commits/%s.json", commit_id);
    if (n <= 0 || (size_t)n >= sizeof(suffix)) return 0;
    return lazy_root_join(database_root, suffix, out, out_size);
}

/* Returns 1 when the list-based fast path fully validated the chain and fed
 * the visitor; 0 to fall back to the sequential walk; -1 when the visitor
 * aborted (the restore must fail, never fall back). */
static int lazy_try_list_chain(otsardb_object_store *store,
                               const char *database_root,
                               const otsardb_db_metadata *metadata,
                               const otsardb_head *head,
                               otsardb_chain_visitor visitor,
                               void *visitor_ctx) {
    char list_prefix[OTSARDB_KEY_MAX + 1];
    int n = snprintf(list_prefix, sizeof(list_prefix), "%s/commits/", database_root);
    if (n <= 0 || (size_t)n >= sizeof(list_prefix)) return 0;

    lazy_list_collect collect;
    memset(&collect, 0, sizeof(collect));
    otsardb_store_result list_rc = otsardb_store_list(store, list_prefix,
                                                  lazy_list_callback, &collect);
    if (list_rc != OTSARDB_STORE_OK || collect.failed) {
        for (size_t i = 0; i < collect.count; ++i) free(collect.keys[i]);
        free(collect.keys);
        return 0;
    }
    if (collect.count == 0) {
        free(collect.keys);
        return 0;
    }

    /* Fetch and decode every listed manifest in parallel windows. */
    lazy_list_slot *slots = (lazy_list_slot *)calloc(collect.count, sizeof(*slots));
    if (!slots) {
        for (size_t i = 0; i < collect.count; ++i) free(collect.keys[i]);
        free(collect.keys);
        return 0;
    }
    int all_fetched = 1;
    for (size_t i = 0; i < collect.count; ++i) {
        /* Listed keys are relative to the list prefix ("<root>/commits/"):
         * re-join them into full logical keys. */
        int n = snprintf(slots[i].key, sizeof(slots[i].key), "%s%s",
                         list_prefix, collect.keys[i]);
        if (n <= 0 || (size_t)n >= sizeof(slots[i].key)) {
            all_fetched = 0;
            break;
        }
    }
    if (all_fetched) {
        lazy_chain_job jobs[LAZY_PARALLEL_CHAIN_WINDOW];
#if defined(_WIN32)
        uintptr_t handles[LAZY_PARALLEL_CHAIN_WINDOW];
#else
        pthread_t handles[LAZY_PARALLEL_CHAIN_WINDOW];
#endif
        for (size_t start = 0; start < collect.count && all_fetched;
             start += LAZY_PARALLEL_CHAIN_WINDOW) {
            size_t window = collect.count - start;
            if (window > LAZY_PARALLEL_CHAIN_WINDOW) window = LAZY_PARALLEL_CHAIN_WINDOW;
            for (size_t i = 0; i < window; ++i) {
                jobs[i].store = store;
                jobs[i].slot = (lazy_chain_slot *)&slots[start + i];
#if defined(_WIN32)
                handles[i] = _beginthreadex(NULL, 0, lazy_chain_worker, &jobs[i], 0, NULL);
                if (handles[i] == 0) {
                    slots[start + i].ok = 0;
                    handles[i] = 0;
                }
#else
                if (pthread_create(&handles[i], NULL, lazy_chain_worker, &jobs[i]) != 0) {
                    slots[start + i].ok = 0;
                }
#endif
            }
            for (size_t i = 0; i < window; ++i) {
#if defined(_WIN32)
                if (handles[i]) {
                    WaitForSingleObject((HANDLE)handles[i], INFINITE);
                    CloseHandle((HANDLE)handles[i]);
                }
#else
                pthread_join(handles[i], NULL);
#endif
                if (!slots[start + i].ok) all_fetched = 0;
            }
        }
    }

    /* Sorted key index for O(log N) chain traversal. */
    size_t *order = NULL;
    if (all_fetched) {
        order = (size_t *)malloc(collect.count * sizeof(*order));
        if (!order) all_fetched = 0;
        else {
            size_t *sort_buf = (size_t *)malloc(collect.count * sizeof(*sort_buf));
            if (!sort_buf) {
                free(order);
                order = NULL;
                all_fetched = 0;
            } else {
                for (size_t i = 0; i < collect.count; ++i) order[i] = i;
                lazy_list_sort(order, collect.count, slots, sort_buf);
                free(sort_buf);
            }
        }
    }

    int ok = all_fetched;
    int visitor_aborted = 0;
    otsardb_commit_manifest *chain = NULL;
    const char **chain_keys = NULL;
    const char **chain_shas = NULL;
    otsardb_segment_enc_set *chain_encs = NULL;
    size_t chain_count = 0;

    if (ok) {
        /* Traverse the chain from HEAD through the fetched set. */
        char current_key[OTSARDB_KEY_MAX + 1];
        char expected_sha[OTSARDB_MANIFEST_SHA256_HEX_SIZE + 1];
        char expected_commit_id[OTSARDB_COMMIT_ID_MAX + 1];
        snprintf(current_key, sizeof(current_key), "%s", head->commit_key);
        snprintf(expected_sha, sizeof(expected_sha), "%s", head->commit_sha256);
        snprintf(expected_commit_id, sizeof(expected_commit_id), "%s", head->commit_id);
        uint64_t generation = head->generation;

        chain = (otsardb_commit_manifest *)calloc((size_t)generation + 1u, sizeof(*chain));
        chain_keys = (const char **)calloc((size_t)generation + 1u, sizeof(*chain_keys));
        chain_shas = (const char **)calloc((size_t)generation + 1u, sizeof(*chain_shas));
        chain_encs = (otsardb_segment_enc_set *)calloc((size_t)generation + 1u,
                                                     sizeof(*chain_encs));
        if (!chain || !chain_keys || !chain_shas || !chain_encs) {
            ok = 0;
        }

        while (ok && generation > 0) {
            /* Binary search the current key in the fetched set. */
            size_t lo = 0;
            size_t hi = collect.count;
            size_t found = SIZE_MAX;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2u;
                int cmp = strcmp(slots[order[mid]].key, current_key);
                if (cmp < 0) {
                    lo = mid + 1u;
                } else {
                    hi = mid;
                }
            }
            if (lo < collect.count && strcmp(slots[order[lo]].key, current_key) == 0) {
                found = order[lo];
            }
            if (found == SIZE_MAX || !slots[found].ok ||
                strcmp(slots[found].sha256, expected_sha) != 0) {
                ok = 0;
                break;
            }
            otsardb_commit_manifest *m = &slots[found].manifest;
            if (m->generation != generation ||
                strcmp(m->commit_id, expected_commit_id) != 0 ||
                strcmp(m->database_id, metadata->database_id) != 0 ||
                m->page_size != metadata->page_size ||
                m->parent_generation + 1u != m->generation) {
                ok = 0;
                break;
            }
            chain[chain_count] = *m;
            chain_keys[chain_count] = slots[found].key;
            chain_shas[chain_count] = slots[found].sha256;
            chain_encs[chain_count] = slots[found].enc;
            ++chain_count;

            if (m->parent_generation == 0) {
                if (m->parent_commit_id[0] != '\0' ||
                    (m->format_version >= 2 && m->parent_manifest_sha256[0] != '\0')) {
                    ok = 0;
                }
                break;
            }
            if (m->parent_commit_id[0] == '\0') {
                ok = 0;
                break;
            }
            generation = m->parent_generation;
            snprintf(expected_commit_id, sizeof(expected_commit_id), "%s", m->parent_commit_id);
            if (m->format_version >= 2) {
                if (strlen(m->parent_manifest_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
                    ok = 0;
                    break;
                }
                snprintf(expected_sha, sizeof(expected_sha), "%s", m->parent_manifest_sha256);
            } else {
                expected_sha[0] = '\0';
            }
            if (!build_lazy_manifest_key(database_root,
                                         expected_commit_id,
                                         current_key,
                                         sizeof(current_key))) {
                ok = 0;
                break;
            }
        }

        /* The chain must contain exactly head.generation validated manifests
         * (the traversal breaks at the genesis manifest). */
        if (ok && chain_count != head->generation) ok = 0;
    }

    if (ok) {
        /* Feed the visitor newest-first (chain was collected newest-first). */
        for (size_t i = 0; i < chain_count && ok; ++i) {
            if (visitor &&
                !visitor(visitor_ctx, chain_keys[i], chain_shas[i], &chain[i],
                         &chain_encs[i])) {
                ok = 0;
                visitor_aborted = 1;
            }
        }
    }

    free(chain);
    free(chain_keys);
    free(chain_shas);
    free(chain_encs);
    free(order);
    for (size_t i = 0; i < collect.count; ++i) {
        otsardb_commit_manifest_free(&slots[i].manifest);
        free(collect.keys[i]);
    }
    free(slots);
    free(collect.keys);
    return visitor_aborted ? -1 : ok;
}

/* ---------------------------------------------------------------------------
 * Restore (validate chain + base image + file sizing)
 * ------------------------------------------------------------------------- */

static int lazy_truncate_file(const char *path, uint64_t bytes) {
    if (bytes > INT64_MAX) return 0;
    FILE *file = fopen(path, "r+b");
    if (!file) file = fopen(path, "w+b");
    if (!file) return 0;
#if defined(_WIN32)
    int ok = _chsize_s(_fileno(file), (__int64)bytes) == 0;
#else
    int ok = ftruncate(fileno(file), (off_t)bytes) == 0;
#endif
    int close_ok = fclose(file) == 0;
    return ok && close_ok;
}

/* Release every owned structure of a hydrator, keeping the struct itself
 * (used by the in-place rebuild path). The prefetch pool must already be
 * shut down and joined (lazy_pool_shutdown) â€” callers do that first. */
static void lazy_release_state(otsardb_lazy_hydrator *h) {
    for (size_t i = 0; i < h->manifest_count; ++i) {
        otsardb_commit_manifest_free(&h->manifests[i]);
    }
    free(h->manifests);
    free(h->manifest_enc);
    free(h->unknown_segments);
    free(h->bitmap);
    free(h->prefetch_jobs);
    for (size_t i = 0; i < h->task_count; ++i) {
        free(h->tasks[i].verified_bytes);
    }
    free(h->tasks);
    h->manifests = NULL;
    h->manifest_count = 0;
    h->manifest_enc = NULL;
    h->unknown_segments = NULL;
    h->unknown_count = 0;
    h->bitmap = NULL;
    h->bitmap_bytes = 0;
    h->marked_pages = 0;
    h->page_size = 0;
    h->database_size_pages = 0;
    h->covered_size_pages = 0;
    h->base_complete_pages = 0;
    h->unknown_fetched = 0;
    h->fetch_failed = 0;
    h->tasks = NULL;
    h->task_count = 0;
    h->next_apply = 0;
    h->pool_started = 0;
    h->pool_stop = 0;
    h->last_miss_page = 0;
    h->seq_streak = 0;
    h->prefetch_jobs = NULL;
    h->prefetch_job_count = 0;
    h->prefetch_job_capacity = 0;
    /* cached bytes are only valid for the hydrator's
     * restore-time head state — a rebuild (lease takeover) must never serve
     * bytes of a superseded head. */
    lazy_cache_clear(h);
}

/* ---------------------------------------------------------------------------
 * Prefetch pool 
 * ------------------------------------------------------------------------- */

static int lazy_pool_parallelism(void) {
    const char *value = getenv("OTSARDB_LAZY_PARALLEL");
    if (!value || value[0] == '\0') return LAZY_POOL_DEFAULT;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0') return LAZY_POOL_DEFAULT;
    if (n < 1) return 1;
    if (n > LAZY_POOL_MAX) return LAZY_POOL_MAX;
    return (int)n;
}

/* Stop and join every pool worker. MUST be called without the hydrator
 * mutex held (workers wait on it). */
static void lazy_pool_shutdown(otsardb_lazy_hydrator *h) {
    if (!h->pool_started || h->pool_workers == 0) {
        h->pool_stop = 1;
        return;
    }
    lazy_mutex_lock(&h->mutex);
    h->pool_stop = 1;
    lazy_cond_broadcast(&h->cond);
    lazy_mutex_unlock(&h->mutex);
    for (int i = 0; i < h->pool_workers; ++i) {
#if defined(_WIN32)
        if (h->pool_threads[i]) {
            WaitForSingleObject((HANDLE)h->pool_threads[i], INFINITE);
            CloseHandle((HANDLE)h->pool_threads[i]);
            h->pool_threads[i] = 0;
        }
#else
        pthread_join(h->pool_threads[i], NULL);
#endif
    }
    h->pool_workers = 0;
    h->pool_started = 0;
}

/* ---------------------------------------------------------------------------
 * Segment fetch + apply (whole object, verified)
 * ------------------------------------------------------------------------- */

/* Returns the ordinal of the NEWEST segment whose v4 page set contains
 * page_no, or SIZE_MAX when no known segment claims it (the caller then
 * consults the checkpoint base coverage / unknown segments before failing
 * closed). Ordinals match h->tasks ordering (newest-first). */
static size_t lazy_resolve_page(const otsardb_lazy_hydrator *h, uint32_t page_no) {
    size_t ordinal = 0;
    for (size_t m = 0; m < h->manifest_count; ++m) {
        const otsardb_commit_manifest *manifest = &h->manifests[m];
        for (uint32_t s = 0; s < manifest->segment_count; ++s) {
            const otsardb_manifest_segment_ref *ref = &manifest->segments[s];
            if (ref->page_count == 0 || ref->pages == NULL) continue;
            uint32_t lo = 0;
            uint32_t hi = ref->page_count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2u;
                if (ref->pages[mid] < page_no) {
                    lo = mid + 1u;
                } else {
                    hi = mid;
                }
            }
            if (lo < ref->page_count && ref->pages[lo] == page_no) {
                return ordinal;
            }
            ++ordinal;
        }
    }
    return SIZE_MAX;
}

/* True when a page with no known segment claimant is covered by the
 * checkpoint base image (which was fully materialised at restore). */
static int lazy_covered_by_checkpoint(const otsardb_lazy_hydrator *h,
                                      uint32_t page_no) {
    return h->covered_size_pages != 0 &&
           (uint64_t)page_no <= h->covered_size_pages;
}

/* ---------------------------------------------------------------------------
 * Sequential read-ahead 
 *
 * Index-leaf lookahead prefetch: detect a +1-page trend (consecutive demand
 * misses), look ahead 8 pages, and fetch the predicted pages together with
 * the requested one, as REIMPLEMENTED OtsarDB-native C, simplified to
 * sequential-only detection and background fetch through the existing 0043
 * prefetch pool: the demand path never blocks on a prediction, and a wrong
 * prediction is harmless (apply-time bitmap re-check). K is
 * OTSARDB_LAZY_PREFETCH; the max lookahead is 8.
 * ------------------------------------------------------------------------- */

static int lazy_prefetch_depth(void) {
    const char *value = getenv("OTSARDB_LAZY_PREFETCH");
    if (!value || value[0] == '\0') return LAZY_PREFETCH_DEFAULT;
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0') return LAZY_PREFETCH_DEFAULT;
    if (n < 1) return 0; /* 0 (or negative) disables prefetch */
    if (n > LAZY_PREFETCH_MAX) return LAZY_PREFETCH_MAX;
    return (int)n;
}

/* Append one prefetch job, dropping silently when the queue is full or the
 * window overlaps a job already pending/fetching (the next sequential miss
 * schedules the NEXT window anyway). Caller holds the hydrator mutex. */
static void lazy_prefetch_job_enqueue(otsardb_lazy_hydrator *h,
                                      size_t ordinal,
                                      uint32_t first_page,
                                      uint32_t last_page) {
    for (size_t i = 0; i < h->prefetch_job_count; ++i) {
        lazy_prefetch_job *job = &h->prefetch_jobs[i];
        if ((job->state == LAZY_PREFETCH_PENDING ||
             job->state == LAZY_PREFETCH_FETCHING) &&
            job->ordinal == ordinal &&
            job->first_page <= last_page && first_page <= job->last_page) {
            return;
        }
    }
    if (h->prefetch_job_count >= LAZY_PREFETCH_JOB_MAX) return;
    if (h->prefetch_job_count == h->prefetch_job_capacity) {
        size_t next_capacity =
            h->prefetch_job_capacity ? h->prefetch_job_capacity * 2u : 8u;
        lazy_prefetch_job *grown = (lazy_prefetch_job *)realloc(
            h->prefetch_jobs, next_capacity * sizeof(*grown));
        if (!grown) return; /* drop silently */
        h->prefetch_jobs = grown;
        h->prefetch_job_capacity = next_capacity;
    }
    lazy_prefetch_job *job = &h->prefetch_jobs[h->prefetch_job_count++];
    job->ordinal = ordinal;
    job->first_page = first_page;
    job->last_page = last_page;
    job->state = LAZY_PREFETCH_PENDING;
    /* Wake the pool: a worker may be sleeping on the condvar. */
    lazy_cond_broadcast(&h->cond);
    if (otsardb_lazy_timing_enabled()) {
        fprintf(stderr,
                "otsardb: timing lazy prefetch scheduled ordinal=%zu pages=%u..%u\n",
                ordinal, first_page, last_page);
    }
}

/* True when an ACTIVE prefetch job (pending or in flight) covers page_no of
 * the segment at `ordinal`. Caller holds the hydrator mutex. */
static int lazy_prefetch_covers(const otsardb_lazy_hydrator *h,
                                size_t ordinal,
                                uint32_t page_no) {
    for (size_t i = 0; i < h->prefetch_job_count; ++i) {
        const lazy_prefetch_job *job = &h->prefetch_jobs[i];
        if (job->ordinal == ordinal &&
            (job->state == LAZY_PREFETCH_PENDING ||
             job->state == LAZY_PREFETCH_FETCHING) &&
            job->first_page <= page_no && page_no <= job->last_page) {
            return 1;
        }
    }
    return 0;
}

/* True when a COMPLETED prefetch job (prefetch_hits
 * provenance) covers page_no of the segment at `ordinal`. Jobs never leave
 * the array (bounded at LAZY_PREFETCH_JOB_MAX), so a DONE window is the
 * deterministic record that the page was materialised by prefetch. Caller
 * holds the hydrator mutex. */
static int lazy_prefetch_job_done(const otsardb_lazy_hydrator *h,
                                  size_t ordinal,
                                  uint32_t page_no) {
    for (size_t i = 0; i < h->prefetch_job_count; ++i) {
        const lazy_prefetch_job *job = &h->prefetch_jobs[i];
        if (job->state == LAZY_PREFETCH_DONE && job->ordinal == ordinal &&
            job->first_page <= page_no && page_no <= job->last_page) {
            return 1;
        }
    }
    return 0;
}

/* Wait until no active prefetch job covers (ordinal, page_no) — the worker
 * broadcasts on every job completion/drop and on pool shutdown. The demand
 * thread pays exactly the wire cost its own GET would have cost, and a
 * stuck window can never wedge the reader: the wait also ends on
 * fetch_failed / pool_stop / generation change, after which the caller
 * falls back to a direct demand fetch. Caller holds the hydrator mutex. */
static void lazy_wait_prefetch_job(otsardb_lazy_hydrator *h,
                                   size_t ordinal,
                                   uint32_t page_no,
                                   uint64_t generation) {
    while (lazy_prefetch_covers(h, ordinal, page_no) &&
           !h->fetch_failed && !h->pool_stop && generation == h->generation) {
        lazy_cond_wait(&h->cond, &h->mutex);
    }
}

/* Schedule a prefetch window [page_no+1, page_no+K] for the segment that
 * claims page_no. Only pages that exist in that segment's page set (the
 * segment is their newest claimant via lazy_resolve_page) and are not yet
 * materialised are included; contiguous survivors become one coalesced
 * range-GET job per run. Frame-offset segments only: the pool already
 * fetches whole-object segments eagerly. Caller holds the mutex. */
static void lazy_prefetch_schedule(otsardb_lazy_hydrator *h, uint32_t page_no) {
    int depth = lazy_prefetch_depth();
    if (depth <= 0) return;
    if (h->pool_workers == 0) return; /* sync mode: no worker runs jobs */
    if (page_no == UINT32_MAX) return;
    size_t ordinal = lazy_resolve_page(h, page_no);
    if (ordinal == SIZE_MAX) return;
    lazy_seg_task *task = &h->tasks[ordinal];
    if (!task->ref.has_frame_offsets) return;
    if (!otsardb_store_supports(h->store, OTSARDB_STORE_CAP_RANGE_GET)) return;
    /* the streak detector fired a schedule — one trigger
     * per schedule decision (a window may still be dropped by job dedup or
     * find nothing left to prefetch). */
    lazy_stats_add64(&h->stats.sequential_streaks, 1);

    uint32_t first = page_no + 1u;
    uint64_t last64 = (uint64_t)page_no + (uint64_t)depth;
    if (last64 > h->database_size_pages) last64 = h->database_size_pages;
    if ((uint64_t)first > last64) return;
    uint32_t last = (uint32_t)last64;

    uint32_t p = first;
    while (p <= last) {
        /* cached pages are skipped like materialised ones —
         * the RAM tier would serve them without a GET, so a window must not
         * fetch them redundantly. */
        if (bitmap_get(h, p) || lazy_cache_contains(h, p) ||
            lazy_resolve_page(h, p) != ordinal) {
            ++p;
            continue;
        }
        uint32_t run_start = p;
        while (p < last) {
            uint32_t next = p + 1u;
            if (bitmap_get(h, next) || lazy_cache_contains(h, next) ||
                lazy_resolve_page(h, next) != ordinal) {
                break;
            }
            ++p;
        }
        lazy_prefetch_job_enqueue(h, ordinal, run_start, p);
        ++p;
    }
}

/* Record a demand miss and detect the sequential-scan signature: two
 * consecutive miss page numbers. Once detected, every following miss in the
 * run schedules the next window. Caller holds the mutex. */
static void lazy_note_miss(otsardb_lazy_hydrator *h, uint32_t page_no) {
    if (page_no == 0) return;
    if (h->last_miss_page != 0 && page_no == h->last_miss_page + 1u) {
        ++h->seq_streak;
    } else {
        h->seq_streak = 1;
    }
    h->last_miss_page = page_no;
    if (h->seq_streak >= LAZY_PREFETCH_MIN_STREAK) {
        lazy_prefetch_schedule(h, page_no);
    }
}

/* resolve one page against the RAM tier. Returns 1 when the
 * page needs no store fetch: a hit either (a) found the page already
 * materialised in the local image (bitmap set — the RAM tier reports the
 * read; the local image stays the source of truth for the VFS), or (b)
 * materialised it from the cache (bitmap clear — the verified cached bytes
 * are written into the local image and marked). Returns 0 on a cache miss
 * (the caller falls through to the fetch paths); a hit whose image write
 * fails degrades to 0 so the ordinary demand fetch (which fails closed)
 * takes over. Counters are bumped only when the tier is enabled. */
static int lazy_cache_resolve_page(otsardb_lazy_hydrator *h, uint32_t page_no) {
    if (h->cache.budget_bytes == 0 || !h->cache.hash) return 0;
    const unsigned char *cached = NULL;
    if (!lazy_cache_lookup(h, page_no, &cached)) {
        lazy_stats_add64(&h->stats.ram_misses, 1);
        return 0;
    }
    lazy_stats_add64(&h->stats.ram_hits, 1);
    if (bitmap_get(h, page_no)) {
        /* provenance: a demand read of a page a completed
         * prefetch job materialised is a prefetch hit (kept for cache hits). */
        size_t ordinal = lazy_resolve_page(h, page_no);
        if (ordinal != SIZE_MAX && lazy_prefetch_job_done(h, ordinal, page_no)) {
            lazy_stats_add64(&h->stats.prefetch_hits, 1);
        }
        return 1;
    }
    if (!lazy_write_page_file(h, page_no, cached)) return 0;
    bitmap_set(h, page_no);
    lazy_stats_add64(&h->stats.demand_misses, 1);
    lazy_note_miss(h, page_no);
    return 1;
}

/* Execute one prefetch job (pool worker, hydrator mutex NOT held): one
 * coalesced byte-range GET for the run, then decode/validate every block
 * and apply it under the mutex, skipping pages the demand path or SQLite
 * materialised meanwhile and re-checking the newest-claimant rule. ANY
 * failure (fetch, short slice, decode, CRC/page mismatch) drops the job
 * silently — prefetch never poisons the sticky demand-failure flag; the
 * next explicit miss simply re-fetches. */
static int lazy_prefetch_fetch_run(otsardb_lazy_hydrator *h,
                                   size_t ordinal,
                                   uint32_t first_page,
                                   uint32_t last_page) {
    lazy_seg_task *task = &h->tasks[ordinal];
    if (!task->ref.has_frame_offsets) return 0;
    const otsardb_commit_manifest *manifest =
        &h->manifests[task->ref.manifest_index];
    const otsardb_manifest_segment_ref *ref =
        &manifest->segments[task->ref.segment_index];
    if (!ref->frame_offsets || ref->page_count == 0) return 0;
    uint32_t run_len = last_page - first_page + 1u;

    /* The run is consecutive page numbers: locate its first page in the
     * sorted page array (block indices are then consecutive too). */
    uint32_t lo = 0;
    uint32_t hi = ref->page_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (ref->pages[mid] < first_page) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo >= ref->page_count || ref->pages[lo] != first_page ||
        (uint64_t)lo + run_len > ref->page_count) {
        return 0;
    }
    for (uint32_t b = 0; b < run_len; ++b) {
        if (ref->pages[(size_t)lo + b] != first_page + b) return 0;
    }

    uint64_t range_start = ref->frame_offsets[lo];
    uint64_t range_end = (uint64_t)lo + run_len < ref->page_count
                             ? ref->frame_offsets[(size_t)lo + run_len]
                             : ref->size;
    if (range_start >= range_end || range_end > ref->size ||
        range_end - range_start > SIZE_MAX) {
        return 0;
    }
    size_t span = (size_t)(range_end - range_start);

    otsardb_store_object slice = {0};
    otsardb_store_result rc = otsardb_store_get_range(h->store,
                                                  task->ref.absolute_key,
                                                  (size_t)range_start,
                                                  span,
                                                  &slice);
    /* count every attempted GET and its fetched bytes. */
    lazy_stats_add64(&h->stats.total_gets, 1);
    lazy_stats_add64(&h->stats.range_gets, 1);
    if (rc == OTSARDB_STORE_OK && slice.size != span) rc = OTSARDB_STORE_ERROR;
    if (rc != OTSARDB_STORE_OK) {
        otsardb_store_release_object(h->store, &slice);
        if (otsardb_lazy_timing_enabled()) {
            fprintf(stderr,
                    "otsardb: timing lazy prefetch GET %s failed rc=%d (dropped)\n",
                    task->ref.absolute_key, (int)rc);
        }
        return 0;
    }
    lazy_stats_add64(&h->stats.bytes_fetched, slice.size);
    if (run_len > 1u) lazy_stats_add64(&h->stats.coalesced_runs, 1);
    if (otsardb_lazy_timing_enabled()) {
        fprintf(stderr,
                "otsardb: timing lazy prefetch GET %s off=%llu len=%zu\n",
                task->ref.absolute_key, (unsigned long long)range_start, span);
    }

    lazy_mutex_lock(&h->mutex);
    size_t pos = 0;
    int ok = 1;
    for (uint32_t b = 0; b < run_len && ok; ++b) {
        uint32_t page_no = first_page + b;
        if (h->fetch_failed) { /* a demand failure wins: stop silently */
            ok = 0;
            break;
        }
        /* (LH-1): decode and consume EVERY block of the run,
         * even when the page is already materialised or no longer claimed by
         * this segment. Skipping a block without consuming it desyncs the
         * slice: the next decode then reads the skipped block's header, the
         * positional check fails, and the WHOLE prefetch job is dropped
         * (silent throughput loss). Only the WRITE is skipped. */
        unsigned char *frame = NULL;
        size_t frame_size = 0;
        uint32_t frame_page = 0;
        if (!otsardb_segment_frame_decode(slice.data, slice.size, &pos,
                                        &frame, &frame_size, &frame_page)) {
            ok = 0;
            break;
        }
        if (frame_size != 8u + (size_t)h->page_size ||
            frame_page != page_no) {
            free(frame);
            ok = 0;
            break;
        }
        if (bitmap_get(h, page_no)) {      /* already materialised: no write */
            free(frame);
            continue;
        }
        if (lazy_resolve_page(h, page_no) != ordinal) { /* superseded: no write */
            free(frame);
            continue;
        }
        uint64_t offset = ((uint64_t)page_no - 1u) * (uint64_t)h->page_size;
        FILE *file = NULL;
        if (offset <= INT64_MAX && (file = fopen(h->database_path, "r+b")) != NULL) {
#if defined(_WIN32)
            int seek_ok = _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
            int seek_ok = fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
            if (seek_ok &&
                fwrite(frame + 8u, 1, h->page_size, file) == h->page_size) {
                if (fclose(file) == 0) {
                    bitmap_set(h, page_no);
                    /* page served by a prefetch job. */
                    lazy_stats_add64(&h->stats.prefetch_fetches, 1);
                    /* prefetch fills the RAM tier too. */
                    lazy_cache_store(h, page_no, frame + 8u);
                } else {
                    ok = 0;
                }
            } else {
                fclose(file);
                ok = 0;
            }
        } else {
            if (file) fclose(file);
            ok = 0;
        }
        free(frame);
    }
    if (ok && pos != slice.size) ok = 0;
    lazy_mutex_unlock(&h->mutex);
    otsardb_store_release_object(h->store, &slice);
    return ok;
}

/* Fetch step (workers, no mutex held): GET the object and verify its exact
 * size and SHA-256 against the chain-authenticated manifest reference. The
 * verified bytes are stored in the task for the (ordered) apply step. */
static int lazy_task_fetch(otsardb_lazy_hydrator *h, lazy_seg_task *task) {
    otsardb_store_object object = {0};
    if (otsardb_store_get(h->store, task->ref.absolute_key, &object) != OTSARDB_STORE_OK) {
        return 0;
    }
    char actual_sha256[OTSARDB_SHA256_HEX_SIZE + 1];
    int ok = object.size == task->ref.size &&
             otsardb_sha256_hex_data(object.data, object.size, actual_sha256) &&
             strcmp(actual_sha256, task->ref.sha256) == 0;
    if (ok) {
        task->verified_bytes = (unsigned char *)malloc(object.size ? object.size : 1u);
        if (!task->verified_bytes) {
            ok = 0;
        } else {
            if (object.size) memcpy(task->verified_bytes, object.data, object.size);
            task->verified_size = object.size;
        }
    }
    otsardb_store_release_object(h->store, &object);
    return ok;
}

/* ---------------------------------------------------------------------------
 * segment envelope decryption (fail closed)
 * ------------------------------------------------------------------------- */

/* AAD = "<absolute object key>\n<commit_id>" (malloc'd). */
static int lazy_aad_build(const char *absolute_key,
                          const char *commit_id,
                          char **out_aad,
                          size_t *out_aad_len) {
    size_t key_len = strlen(absolute_key);
    size_t id_len = strlen(commit_id);
    if (key_len > SIZE_MAX - id_len - 2u) return 0;
    size_t total = key_len + id_len + 2u;
    char *aad = (char *)malloc(total);
    if (!aad) return 0;
    memcpy(aad, absolute_key, key_len);
    aad[key_len] = '\n';
    memcpy(aad + key_len + 1u, commit_id, id_len);
    aad[key_len + 1u + id_len] = '\0';
    *out_aad = aad;
    *out_aad_len = total - 1u;
    return 1;
}

/* Decrypt an encrypted segment envelope into the plaintext .otsseg bytes.
 * Returns 1 on success (*out owns the plaintext); 0 fails closed (missing
 * cipher or tag verification failure). */
static int lazy_decrypt_envelope(const otsardb_lazy_hydrator *h,
                                 const char *absolute_key,
                                 const char *commit_id,
                                 const unsigned char *envelope,
                                 size_t envelope_size,
                                 unsigned char **out,
                                 size_t *out_size) {
    if (!h->cipher) {
        fprintf(stderr,
                "otsardb: lazy hydration: segment %s is encrypted (%s) but no "
                "OTSARDB_S3_ENC_KEY is configured; failing closed\n",
                absolute_key, OTSARDB_CIPHER_ALG_NAME);
        return 0;
    }
    char *aad = NULL;
    size_t aad_len = 0;
    if (!lazy_aad_build(absolute_key, commit_id, &aad, &aad_len)) return 0;
    int ok = otsardb_cipher_decrypt(h->cipher,
                                  envelope,
                                  envelope_size,
                                  aad,
                                  aad_len,
                                  out,
                                  out_size);
    free(aad);
    if (!ok) {
        fprintf(stderr,
                "otsardb: lazy hydration: segment %s failed AES-256-GCM tag "
                "verification (%s); wrong key or tampered object — failing "
                "closed\n",
                absolute_key, otsardb_cipher_last_error());
    }
    return ok;
}

/* The parsed "segment_enc" entry for (manifest_index, segment_index), or
 * NULL when the manifest is plaintext. */
static const otsardb_segment_enc_entry *lazy_segment_enc_entry(
    const otsardb_lazy_hydrator *h,
    size_t manifest_index,
    uint32_t segment_index) {
    if (!h->manifest_enc || manifest_index >= h->manifest_count) return NULL;
    const otsardb_segment_enc_set *enc = &h->manifest_enc[manifest_index];
    if (!enc->present || segment_index >= enc->count) return NULL;
    return enc->entries[segment_index].present ? &enc->entries[segment_index] : NULL;
}

/* Apply step (requesting thread, hydrator mutex held): decode the verified
 * object (decrypting the envelope first when the manifest marks it
 * encrypted), decompress, and write every frame of the batch into the local
 * image, marking pages the segment owns. */
static int lazy_task_apply(otsardb_lazy_hydrator *h,
                           lazy_seg_task *task,
                           size_t ordinal) {
    const lazy_task_ref *ref = &task->ref;
    if (!task->verified_bytes) return 0;

    const otsardb_segment_enc_entry *entry =
        lazy_segment_enc_entry(h, ref->manifest_index, ref->segment_index);
    unsigned char *decrypted = NULL;
    size_t decrypted_size = 0;
    const unsigned char *segment_bytes = task->verified_bytes;
    size_t segment_size = task->verified_size;
    if (entry) {
        if (!lazy_decrypt_envelope(h,
                                   ref->absolute_key,
                                   ref->commit_id,
                                   task->verified_bytes,
                                   task->verified_size,
                                   &decrypted,
                                   &decrypted_size)) {
            return 0;
        }
        segment_bytes = decrypted;
        segment_size = decrypted_size;
    }

    otsardb_segment_view segment;
    int decode_ok = otsardb_segment_decode(segment_bytes, segment_size, &segment);
    free(decrypted);
    if (!decode_ok || segment.kind != OTSARDB_SEGMENT_WAL_FRAMES ||
        strcmp(segment.commit_id, ref->commit_id) != 0) {
        return 0;
    }

    int ok = 1;
    void *decompressed = NULL;
    size_t decompressed_size = 0;
    const unsigned char *payload = segment.payload;
    size_t payload_size = segment.payload_size;
    if (segment.compressed) {
        ok = otsardb_segment_decompress(&segment, &decompressed, &decompressed_size);
        payload = (const unsigned char *)decompressed;
        payload_size = decompressed_size;
    }
    if (ok) {
        otsardb_wal_batch_view batch;
        ok = otsardb_wal_batch_decode(payload, payload_size, &batch) &&
             batch.page_size == ref->page_size;
        if (ok) {
            if (otsardb_lazy_timing_enabled()) {
                fprintf(stderr,
                        "otsardb: timing lazy applied segment %s frames=%u pagesize=%u\n",
                        ref->absolute_key, batch.frame_count, batch.page_size);
            }
            FILE *file = fopen(h->database_path, "r+b");
            if (!file) {
                ok = 0;
            } else {
                for (uint32_t i = 0; i < batch.frame_count && ok; ++i) {
                    uint32_t page_no = 0;
                    const unsigned char *page = NULL;
                    if (!otsardb_wal_batch_frame(&batch, i, &page_no, &page)) {
                        ok = 0;
                        break;
                    }
                    /* Only the page's NEWEST claimant may write it: tasks
                     * are applied newest-first, so a later (older) segment
                     * must never overwrite a newer value. */
                    if (lazy_resolve_page(h, page_no) != ordinal) {
                        continue;
                    }
                    uint64_t offset = ((uint64_t)page_no - 1u) * (uint64_t)batch.page_size;
                    if (offset > INT64_MAX) {
                        ok = 0;
                        break;
                    }
#if defined(_WIN32)
                    if (_fseeki64(file, (__int64)offset, SEEK_SET) != 0) {
#else
                    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
#endif
                        ok = 0;
                        break;
                    }
                    if (fwrite(page, 1, batch.page_size, file) != batch.page_size) {
                        ok = 0;
                        break;
                    }
                    /* count real demand misses for the
                     * sequential read-ahead streak. they
                     * are the demand-path miss counter. */
                    int was_miss = !bitmap_get(h, page_no);
                    bitmap_set(h, page_no);
                    if (was_miss) {
                        lazy_stats_add64(&h->stats.demand_misses, 1);
                        lazy_note_miss(h, page_no);
                        /* whole-object applies fill the RAM
                         * tier too (only fresh materialisations: a page the
                         * session already wrote may be superseded). */
                        lazy_cache_store(h, page_no, page);
                    }
                }
                if (fclose(file) != 0 && ok) ok = 0;
            }
        }
    }
    free(decompressed);
    free(task->verified_bytes);
    task->verified_bytes = NULL;
    task->verified_size = 0;
    return ok;
}

#if defined(_WIN32)
static unsigned __stdcall lazy_pool_worker_main(void *opaque)
#else
static void *lazy_pool_worker_main(void *opaque)
#endif
{
    otsardb_lazy_hydrator *h = (otsardb_lazy_hydrator *)opaque;
    for (;;) {
        lazy_mutex_lock(&h->mutex);
        if (h->pool_stop || h->fetch_failed) {
            lazy_mutex_unlock(&h->mutex);
            break;
        }
        /* Sequential read-ahead jobs first: small range
         * GETs serving the live scan; a job is copied out (ordinal + page
         * window) before the mutex is released because a concurrent enqueue
         * may realloc the job array while the fetch is in flight. */
        size_t job_idx = SIZE_MAX;
        for (size_t i = 0; i < h->prefetch_job_count; ++i) {
            if (h->prefetch_jobs[i].state == LAZY_PREFETCH_PENDING) {
                job_idx = i;
                break;
            }
        }
        if (job_idx != SIZE_MAX) {
            const size_t job_ordinal = h->prefetch_jobs[job_idx].ordinal;
            const uint32_t job_first = h->prefetch_jobs[job_idx].first_page;
            const uint32_t job_last = h->prefetch_jobs[job_idx].last_page;
            h->prefetch_jobs[job_idx].state = LAZY_PREFETCH_FETCHING;
            lazy_mutex_unlock(&h->mutex);

            int job_ok = lazy_prefetch_fetch_run(h, job_ordinal,
                                                 job_first, job_last);

            lazy_mutex_lock(&h->mutex);
            h->prefetch_jobs[job_idx].state =
                job_ok ? LAZY_PREFETCH_DONE : LAZY_PREFETCH_DROPPED;
            lazy_cond_broadcast(&h->cond);
            lazy_mutex_unlock(&h->mutex);
            continue;
        }
        size_t idx = SIZE_MAX;
        for (size_t i = 0; i < h->task_count; ++i) {
            if (h->tasks[i].state == LAZY_TASK_IDLE &&
                !h->tasks[i].ref.has_frame_offsets) {
                idx = i;
                break;
            }
        }
        if (idx == SIZE_MAX) {
            /* Nothing left to fetch: sleep until new work or shutdown. */
            lazy_cond_wait(&h->cond, &h->mutex);
            lazy_mutex_unlock(&h->mutex);
            continue;
        }
        h->tasks[idx].state = LAZY_TASK_FETCHING;
        lazy_mutex_unlock(&h->mutex);

        int ok = lazy_task_fetch(h, &h->tasks[idx]);

        lazy_mutex_lock(&h->mutex);
        /* count the whole-object GET (attempted) and the
         * verified bytes (success only). */
        lazy_stats_add64(&h->stats.total_gets, 1);
        if (ok) lazy_stats_add64(&h->stats.bytes_fetched, h->tasks[idx].verified_size);
        if (ok) {
            h->tasks[idx].state = LAZY_TASK_FETCHED;
        } else {
            h->tasks[idx].state = LAZY_TASK_FAILED;
            h->fetch_failed = 1;
        }
        lazy_cond_broadcast(&h->cond);
        lazy_mutex_unlock(&h->mutex);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* Spawn the prefetch workers on the first page miss. Called with the
 * hydrator mutex held; the workers block on it until released. */
static void lazy_pool_start(otsardb_lazy_hydrator *h) {
    h->pool_started = 1;
    int workers = h->task_count < 2 ? 1 : lazy_pool_parallelism();
    for (int i = 0; i < workers; ++i) {
#if defined(_WIN32)
        h->pool_threads[i] = _beginthreadex(NULL, 0, lazy_pool_worker_main, h, 0, NULL);
        if (h->pool_threads[i] == 0) {
            h->pool_threads[i] = 0;
            break;
        }
        ++h->pool_workers;
#else
        if (pthread_create(&h->pool_threads[i], NULL, lazy_pool_worker_main, h) != 0) {
            break;
        }
        ++h->pool_workers;
#endif
    }
}

/* Build the prefetch task array (newest-first, one task per segment with a
 * known page set). Called from restore with the mutex held. Segments whose
 * manifest records frame_offsets are serviced by inline
 * byte-range GETs when the store supports range reads; the pool only fetches
 * whole objects (v1-v3 / v4 without offsets, or stores without RANGE_GET). */
static void lazy_build_tasks(otsardb_lazy_hydrator *h) {
    size_t count = 0;
    for (size_t m = 0; m < h->manifest_count; ++m) {
        const otsardb_commit_manifest *manifest = &h->manifests[m];
        for (uint32_t s = 0; s < manifest->segment_count; ++s) {
            if (manifest->segments[s].page_count > 0) ++count;
        }
    }
    if (count == 0) return;
    h->tasks = (lazy_seg_task *)calloc(count, sizeof(*h->tasks));
    if (!h->tasks) {
        h->fetch_failed = 1;
        return;
    }
    h->task_count = count;
    const int store_range_gets =
        otsardb_store_supports(h->store, OTSARDB_STORE_CAP_RANGE_GET);
    size_t ordinal = 0;
    for (size_t m = 0; m < h->manifest_count; ++m) {
        const otsardb_commit_manifest *manifest = &h->manifests[m];
        const otsardb_segment_enc_set *enc =
            h->manifest_enc ? &h->manifest_enc[m] : NULL;
        for (uint32_t s = 0; s < manifest->segment_count; ++s) {
            const otsardb_manifest_segment_ref *ref = &manifest->segments[s];
            if (ref->page_count == 0) continue;
            lazy_seg_task *task = &h->tasks[ordinal];
            task->ref.format_version = (int)manifest->format_version;
            snprintf(task->ref.commit_id,
                     sizeof(task->ref.commit_id),
                     "%s",
                     manifest->commit_id);
            snprintf(task->ref.sha256,
                     sizeof(task->ref.sha256),
                     "%s",
                     ref->sha256);
            task->ref.size = ref->size;
            task->ref.page_size = manifest->page_size;
            task->ref.manifest_index = m;
            task->ref.segment_index = s;
            /* an encrypted segment can never be served from
             * byte-range slices (GCM authenticates the whole ciphertext), so
             * frame offsets are ignored and the whole object is fetched. */
            const int segment_encrypted =
                enc && enc->present && s < enc->count && enc->entries[s].present;
            task->ref.has_frame_offsets =
                !segment_encrypted && ref->frame_offsets != NULL && store_range_gets;
            segment_absolute_key(h,
                                 manifest->format_version,
                                 ref->key,
                                 task->ref.absolute_key,
                                 sizeof(task->ref.absolute_key));
            ++ordinal;
        }
    }
}

/* (LH-2): count the pages in [1, limit] that are claimed by
 * NO task — i.e. by no segment newer than the checkpoint's covered
 * generation. Those pages are materialised by the checkpoint base image and
 * their bytes are the newest state, so completeness accounting must count
 * them without requiring a bitmap mark. Task page arrays are sorted and
 * strictly increasing (manifest validation); a k-way merge emits values in
 * ascending order, so every emitted value is distinct and no per-page bitmap
 * is needed. Caller holds the hydrator mutex (restore time). */
static uint64_t lazy_count_covered_unclaimed(const otsardb_lazy_hydrator *h,
                                             uint64_t limit) {
    if (h->task_count == 0 || limit == 0) return limit;
    size_t *cursor = (size_t *)malloc(h->task_count * sizeof(*cursor));
    if (!cursor) return limit; /* OOM: conservative (count nothing extra) */
    const otsardb_manifest_segment_ref **refs =
        (const otsardb_manifest_segment_ref **)malloc(h->task_count * sizeof(*refs));
    if (!refs) {
        free((void *)cursor);
        return limit;
    }
    size_t active = 0;
    for (size_t o = 0; o < h->task_count; ++o) {
        const lazy_seg_task *task = &h->tasks[o];
        const otsardb_commit_manifest *manifest =
            &h->manifests[task->ref.manifest_index];
        const otsardb_manifest_segment_ref *ref =
            &manifest->segments[task->ref.segment_index];
        if (ref->page_count == 0 || ref->pages == NULL) continue;
        refs[active] = ref;
        cursor[active] = 0;
        ++active;
    }
    uint64_t claimed = 0;
    uint64_t previous = 0; /* strictly-increasing emitted values */
    for (;;) {
        uint64_t best = UINT64_MAX;
        int best_set = 0;
        for (size_t i = 0; i < active; ++i) {
            const otsardb_manifest_segment_ref *ref = refs[i];
            if (cursor[i] >= ref->page_count) continue;
            uint64_t v = ref->pages[cursor[i]];
            if (!best_set || v < best) {
                best = v;
                best_set = 1;
            }
        }
        if (!best_set || best > limit) break;
        if (best > previous) {
            ++claimed;
            previous = best;
        }
        for (size_t i = 0; i < active; ++i) {
            const otsardb_manifest_segment_ref *ref = refs[i];
            if (cursor[i] < ref->page_count &&
                (uint64_t)ref->pages[cursor[i]] == best) {
                ++cursor[i];
            }
        }
    }
    free((void *)refs);
    free((void *)cursor);
    uint64_t unclaimed = limit - claimed;
    return unclaimed;
}

/* Fetch every unknown-page segment once (fail-closed: any failure aborts).
 * Unknown segments are always OLDER than v4 known segments in practice
 * (v1-v3 chains, or the legacy v4-empty single-segment path), so their
 * frames are applied only to pages that are not yet materialised. Runs on
 * the requesting thread with the hydrator mutex held. */
static int lazy_fetch_unknown_segments(otsardb_lazy_hydrator *h) {
    if (h->unknown_fetched) return 1;
    for (size_t i = 0; i < h->unknown_count; ++i) {
        lazy_unknown_ref *u = &h->unknown_segments[i];
        if (u->fetched) continue;
        const otsardb_commit_manifest *manifest = &h->manifests[u->manifest_index];
        const otsardb_manifest_segment_ref *ref = &manifest->segments[u->segment_index];
        char absolute_key[OTSARDB_KEY_MAX + 1];
        if (!segment_absolute_key(h,
                                  manifest->format_version,
                                  ref->key,
                                  absolute_key,
                                  sizeof(absolute_key))) {
            return 0;
        }
        otsardb_store_object object = {0};
        /* count every attempted segment GET. */
        lazy_stats_add64(&h->stats.total_gets, 1);
        if (otsardb_store_get(h->store, absolute_key, &object) != OTSARDB_STORE_OK) {
            return 0;
        }
        lazy_stats_add64(&h->stats.bytes_fetched, object.size);
        char actual_sha256[OTSARDB_SHA256_HEX_SIZE + 1];
        int ok = object.size == ref->size &&
                 otsardb_sha256_hex_data(object.data, object.size, actual_sha256) &&
                 strcmp(actual_sha256, ref->sha256) == 0;
        if (ok) {
            /* encrypted envelope first (tag-verified, fail
             * closed), then the plaintext segment decode. */
            const otsardb_segment_enc_entry *entry =
                lazy_segment_enc_entry(h, u->manifest_index, u->segment_index);
            unsigned char *decrypted = NULL;
            size_t decrypted_size = 0;
            const unsigned char *segment_bytes = object.data;
            size_t segment_size = object.size;
            if (entry) {
                ok = lazy_decrypt_envelope(h,
                                           absolute_key,
                                           manifest->commit_id,
                                           (const unsigned char *)object.data,
                                           object.size,
                                           &decrypted,
                                           &decrypted_size);
                segment_bytes = decrypted;
                segment_size = decrypted_size;
            }
            otsardb_segment_view segment;
            if (ok) {
                ok = otsardb_segment_decode(segment_bytes, segment_size, &segment) &&
                     segment.kind == OTSARDB_SEGMENT_WAL_FRAMES &&
                     strcmp(segment.commit_id, manifest->commit_id) == 0;
            }
            if (ok) {
                void *decompressed = NULL;
                size_t decompressed_size = 0;
                const unsigned char *payload = segment.payload;
                size_t payload_size = segment.payload_size;
                if (segment.compressed) {
                    ok = otsardb_segment_decompress(&segment, &decompressed, &decompressed_size);
                    payload = (const unsigned char *)decompressed;
                    payload_size = decompressed_size;
                }
                if (ok) {
                    otsardb_wal_batch_view batch;
                    ok = otsardb_wal_batch_decode(payload, payload_size, &batch) &&
                         batch.page_size == manifest->page_size;
                    if (ok) {
                        FILE *file = fopen(h->database_path, "r+b");
                        if (!file) {
                            ok = 0;
                        } else {
                            for (uint32_t f = 0; f < batch.frame_count && ok; ++f) {
                                uint32_t page_no = 0;
                                const unsigned char *page = NULL;
                                if (!otsardb_wal_batch_frame(&batch, f, &page_no, &page)) {
                                    ok = 0;
                                    break;
                                }
                                if (bitmap_get(h, page_no)) continue; /* newer data wins */
                                uint64_t offset =
                                    ((uint64_t)page_no - 1u) * (uint64_t)batch.page_size;
                                if (offset > INT64_MAX) {
                                    ok = 0;
                                    break;
                                }
#if defined(_WIN32)
                                if (_fseeki64(file, (__int64)offset, SEEK_SET) != 0) {
#else
                                if (fseeko(file, (off_t)offset, SEEK_SET) != 0) {
#endif
                                    ok = 0;
                                    break;
                                }
                                if (fwrite(page, 1, batch.page_size, file) != batch.page_size) {
                                    ok = 0;
                                    break;
                                }
                                /* sequential read-ahead
                                 * streak (page was unmaterialised by
                                 * construction of this path). 
                                 * 0082: demand-path miss counter. */
                                bitmap_set(h, page_no);
                                lazy_stats_add64(&h->stats.demand_misses, 1);
                                lazy_note_miss(h, page_no);
                                /* cache verified bytes. */
                                lazy_cache_store(h, page_no, page);
                            }
                            if (fclose(file) != 0 && ok) ok = 0;
                        }
                    }
                }
                free(decompressed);
            }
            free(decrypted);
        }
        otsardb_store_release_object(h->store, &object);
        if (!ok) return 0;
        u->fetched = 1;
    }
    h->unknown_fetched = 1;
    return 1;
}

/* Wait until task `idx` is fetched (workers) or fetch it inline (sync mode
 * when no worker could be spawned). Returns 1 when the task is fetchable or
 * applied, 0 on failure/generation change. The hydrator mutex is held. */
static int lazy_wait_task_fetched(otsardb_lazy_hydrator *h,
                                  size_t idx,
                                  uint64_t generation,
                                  int sync_mode) {
    if (sync_mode) {
        if (h->tasks[idx].state == LAZY_TASK_IDLE ||
            h->tasks[idx].state == LAZY_TASK_FETCHING) {
            if (!lazy_task_fetch(h, &h->tasks[idx])) {
                lazy_stats_add64(&h->stats.total_gets, 1);
                h->tasks[idx].state = LAZY_TASK_FAILED;
                h->fetch_failed = 1;
                return 0;
            }
            /* count the whole-object GET (attempted) and
             * the verified bytes (success only). */
            lazy_stats_add64(&h->stats.total_gets, 1);
            lazy_stats_add64(&h->stats.bytes_fetched, h->tasks[idx].verified_size);
            h->tasks[idx].state = LAZY_TASK_FETCHED;
        }
        return h->tasks[idx].state == LAZY_TASK_FETCHED ||
               h->tasks[idx].state == LAZY_TASK_APPLIED;
    }
    while (h->tasks[idx].state != LAZY_TASK_FETCHED &&
           h->tasks[idx].state != LAZY_TASK_APPLIED &&
           !h->fetch_failed && !h->pool_stop &&
           generation == h->generation) {
        lazy_cond_wait(&h->cond, &h->mutex);
    }
    return !h->fetch_failed && generation == h->generation && !h->pool_stop &&
           (h->tasks[idx].state == LAZY_TASK_FETCHED ||
            h->tasks[idx].state == LAZY_TASK_APPLIED);
}

/* ---------------------------------------------------------------------------
 * Inline frame-range hydration 
 *
 * Segments whose manifest records frame_offsets are fetched with byte-range
 * GETs: exactly the blocks holding the needed pages. Blocks are contiguous
 * (one per unique page, sorted page order), so consecutive page numbers are
 * coalesced into a single range GET. Every fetched block is self-validated
 * (exact range size, block header fields, orig_len == 8 + page_size, CRC-32
 * of the decompressed frame, page number must equal the manifest's page).
 *
 * Integrity: a byte-range slice cannot be SHA-256-verified against the
 * manifest (the hash covers the whole object). A sliced fetch inherits trust
 * from the chain-authenticated manifest (the object is content-addressed and
 * immutable) plus the structural validation above; the FULL object hash is
 * still enforced on every whole-object download (v1-v3 fallback, stores
 * without RANGE_GET). SQLite's own header/page-1 checks cross-check the
 * first page where available. Any deviation fails closed (sticky).
 * ------------------------------------------------------------------------- */

typedef struct lazy_offset_req {
    size_t ordinal;
    uint32_t page_no;
} lazy_offset_req;

static int lazy_offset_req_grow(lazy_offset_req **reqs,
                                size_t *count,
                                size_t *capacity,
                                size_t ordinal,
                                uint32_t page_no) {
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2u : 64u;
        lazy_offset_req *next = (lazy_offset_req *)realloc(
            *reqs, next_capacity * sizeof(**reqs));
        if (!next) return 0;
        *reqs = next;
        *capacity = next_capacity;
    }
    (*reqs)[*count].ordinal = ordinal;
    (*reqs)[*count].page_no = page_no;
    ++*count;
    return 1;
}

/* One fetch unit of the demand path . A run covers
 * `page_idx_count` REQUESTED pages of the claiming segment at
 * [page_idx_start, page_idx_start + page_idx_count) in the sorted manifest
 * page array. The fetched byte range [range_start, range_end) covers page
 * indices [covered_lo, covered_hi). After an OTSARDB_S3_COALESCE_MAX byte-
 * window merge the requested set is the convex hull of the merged runs
 * (covered_lo == page_idx_start, covered_hi == page_idx_start +
 * page_idx_count); gap frames inside a merged slice are validated but only
 * applied where legal (bitmap + newest-claimant checks at apply time). */
typedef struct lazy_fetch_run {
    size_t ordinal;
    size_t page_idx_start;
    size_t page_idx_count;
    size_t covered_lo;
    size_t covered_hi;
    uint64_t range_start;
    uint64_t range_end;
    char key[OTSARDB_KEY_MAX + 1];
    otsardb_store_object slice;
    otsardb_store_result rc;
} lazy_fetch_run;

/* One parallel-fetch worker: static partition — worker `worker_index` takes
 * runs worker_index, worker_index + workers, ... (no shared state, no
 * locks). The key and range bounds are precomputed on the requesting thread
 * before the workers start (workers never touch hydrator state). */
typedef struct lazy_run_fetch_ctx {
    otsardb_object_store *store;
    lazy_fetch_run *runs;
    size_t run_count;
    size_t workers;
    size_t worker_index;
} lazy_run_fetch_ctx;

#if defined(_WIN32)
static unsigned __stdcall lazy_run_fetch_worker(void *opaque)
#else
static void *lazy_run_fetch_worker(void *opaque)
#endif
{
    lazy_run_fetch_ctx *ctx = (lazy_run_fetch_ctx *)opaque;
    for (size_t i = ctx->worker_index; i < ctx->run_count;
         i += ctx->workers) {
        lazy_fetch_run *run = &ctx->runs[i];
        if (run->range_start >= run->range_end ||
            run->range_end - run->range_start > SIZE_MAX) {
            run->rc = OTSARDB_STORE_ERROR;
            continue;
        }
        size_t span = (size_t)(run->range_end - run->range_start);
        otsardb_store_result rc = otsardb_store_get_range(ctx->store,
                                                      run->key,
                                                      (size_t)run->range_start,
                                                      span,
                                                      &run->slice);
        if (rc == OTSARDB_STORE_OK && run->slice.size != span) rc = OTSARDB_STORE_ERROR;
        run->rc = rc;
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* prefetch_aware: when 1, pages covered by an active
 * prefetch window are NOT collected — the worker is already fetching them
 * and the outer loop waits on the window instead (see
 * otsardb_lazy_hydrate_range). When 0 (demand-only recovery after a dropped
 * window) coverage is ignored and the page is fetched directly.
 *
 * coalescing refinement + parallel issue. Consecutive page
 * numbers of one segment already merge into one run (0059); runs of the
 * SAME segment whose blocks lie within OTSARDB_S3_COALESCE_MAX bytes of each
 * other are merged into ONE byte-range GET covering both runs. All runs of
 * one hydrate call are fetched in parallel by up to OTSARDB_LAZY_PARALLEL
 * ephemeral workers (the 0043 pool is busy with whole-object fetches and
 * must keep serving its own queue), then decoded/applied in order on the
 * requesting thread. Gap frames (inside a merged slice but outside the
 * requested page set) are structurally validated and skipped — the bitmap
 * and newest-claimant rules decide every write, so a gap page is applied
 * only when it is still unmaterialised and this segment is still its newest
 * claimant (equivalent to the prefetch apply rule). */
static int lazy_hydrate_frames_inline(otsardb_lazy_hydrator *h,
                                      uint32_t first_page,
                                      uint32_t last_page,
                                      int prefetch_aware) {
    if (!otsardb_store_supports(h->store, OTSARDB_STORE_CAP_RANGE_GET)) return 1;
    if (h->task_count == 0) return 1;
    const double lazy_t0 = monotonic_ms();

    /* Phase A: collect every unmaterialised page owned by an offset segment. */
    lazy_offset_req *reqs = NULL;
    size_t req_count = 0;
    size_t req_capacity = 0;
    int ok = 1;
    for (uint32_t page = first_page; page <= last_page && ok; ++page) {
        /* the RAM tier is the first resolution stop — a hit
         * skips the store GET and the decode entirely (the bytes were
         * verified when cached). */
        if (lazy_cache_resolve_page(h, page)) continue;
        if (bitmap_get(h, page)) {
            /* a demand read of a page a completed prefetch
             * job materialised is a prefetch hit. */
            size_t hit_ordinal = lazy_resolve_page(h, page);
            if (hit_ordinal != SIZE_MAX &&
                lazy_prefetch_job_done(h, hit_ordinal, page)) {
                lazy_stats_add64(&h->stats.prefetch_hits, 1);
            }
            continue;
        }
        size_t ordinal = lazy_resolve_page(h, page);
        if (ordinal == SIZE_MAX) continue;
        lazy_seg_task *task = &h->tasks[ordinal];
        if (!task->ref.has_frame_offsets) continue;
        if (prefetch_aware && lazy_prefetch_covers(h, ordinal, page)) continue;
        if (!lazy_offset_req_grow(&reqs, &req_count, &req_capacity, ordinal, page)) {
            ok = 0;
            break;
        }
    }
    if (ok && req_count == 0) {

        free(reqs);
        return 1;
    }

    /* Phase B: per ordinal, coalesce consecutive page numbers (their blocks
     * are contiguous) into one run — the 0059 per-frame-run merging. */
    lazy_fetch_run *runs = NULL;
    size_t run_count = 0;
    size_t run_capacity = 0;
    size_t i = 0;
    while (i < req_count && ok) {
        size_t ordinal = reqs[i].ordinal;
        lazy_seg_task *task = &h->tasks[ordinal];
        const otsardb_commit_manifest *manifest =
            &h->manifests[task->ref.manifest_index];
        const otsardb_manifest_segment_ref *ref =
            &manifest->segments[task->ref.segment_index];
        if (!ref->frame_offsets || ref->page_count == 0) {
            ok = 0;
            break;
        }

        /* Run: same ordinal, consecutive page numbers. */
        size_t start = i;
        while (i + 1u < req_count &&
               reqs[i + 1u].ordinal == ordinal &&
               reqs[i + 1u].page_no == reqs[i].page_no + 1u) {
            ++i;
        }
        size_t run_len = i - start + 1u;

        /* Map the run's first page to its index in the sorted manifest
         * pages array; consecutive page numbers mean consecutive indices. */
        const uint32_t *pages = ref->pages;
        uint32_t lo = 0;
        uint32_t hi = ref->page_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (pages[mid] < reqs[start].page_no) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo >= ref->page_count || pages[lo] != reqs[start].page_no ||
            (uint64_t)lo + run_len > ref->page_count) {
            ok = 0;
            break;
        }
        uint32_t k = lo;

        uint64_t range_start = ref->frame_offsets[k];
        uint64_t range_end = (uint64_t)k + run_len < ref->page_count
                                 ? ref->frame_offsets[(size_t)k + run_len]
                                 : ref->size;
        if (range_start >= range_end || range_end > ref->size) {
            ok = 0;
            break;
        }

        if (run_count == run_capacity) {
            size_t next_capacity = run_capacity ? run_capacity * 2u : 16u;
            lazy_fetch_run *grown =
                (lazy_fetch_run *)realloc(runs, next_capacity * sizeof(*grown));
            if (!grown) {
                ok = 0;
                break;
            }
            runs = grown;
            run_capacity = next_capacity;
        }
        lazy_fetch_run *run = &runs[run_count];
        memset(run, 0, sizeof(*run));
        run->ordinal = ordinal;
        run->page_idx_start = k;
        run->page_idx_count = run_len;
        run->covered_lo = k;
        run->covered_hi = (size_t)k + run_len;
        run->range_start = range_start;
        run->range_end = range_end;
        run->rc = OTSARDB_STORE_OK;
        ++run_count;
        ++i;
    }

    /* Phase B2: extend the merge window. Runs of the SAME
     * segment whose blocks lie within OTSARDB_S3_COALESCE_MAX bytes of each
     * other are fetched in one range covering both runs (transitively).
     * Runs are processed in page order, so per ordinal a single "open"
     * merged run suffices: once a run fails to merge, no later run of that
     * ordinal can reach the open one (page order => increasing offsets). */
    if (ok) {
        uint64_t coalesce_max = lazy_coalesce_max();
        lazy_fetch_run *open = NULL;
        size_t open_count = 0;
        size_t open_capacity = 0;
        lazy_fetch_run *final_runs = NULL;
        size_t final_count = 0;
        size_t final_capacity = 0;
        for (size_t r = 0; r < run_count; ++r) {
            lazy_fetch_run *run = &runs[r];
            size_t slot = SIZE_MAX;
            for (size_t o = 0; o < open_count; ++o) {
                if (open[o].ordinal == run->ordinal) {
                    slot = o;
                    break;
                }
            }
            int merged = 0;
            if (slot != SIZE_MAX && coalesce_max > 0 &&
                run->range_start >= open[slot].range_end &&
                run->range_start - open[slot].range_end <= coalesce_max) {
                /* Same segment, blocks within the byte window: one range
                 * covering both runs. The requested page set becomes the
                 * convex hull [first run start, merged run end) — the gap
                 * frames inside it are validated and applied only where
                 * legal (bitmap + newest-claimant checks at apply time). */
                open[slot].range_end = run->range_end;
                open[slot].covered_hi = run->page_idx_start + run->page_idx_count;
                open[slot].page_idx_count =
                    run->page_idx_start + run->page_idx_count -
                    open[slot].page_idx_start;
                merged = 1;
            }
            if (!merged) {
                /* Close the open run (append to the final list) and open a
                 * fresh one for this run. */
                if (slot != SIZE_MAX) {
                    if (final_count == final_capacity) {
                        size_t next_capacity =
                            final_capacity ? final_capacity * 2u : 16u;
                        lazy_fetch_run *grown = (lazy_fetch_run *)realloc(
                            final_runs, next_capacity * sizeof(*grown));
                        if (!grown) {
                            ok = 0;
                            break;
                        }
                        final_runs = grown;
                        final_capacity = next_capacity;
                    }
                    final_runs[final_count++] = open[slot];
                    if (open_count > 1u) {
                        open[slot] = open[open_count - 1u];
                    }
                    --open_count;
                }
                if (open_count == open_capacity) {
                    size_t next_capacity = open_capacity ? open_capacity * 2u : 8u;
                    lazy_fetch_run *grown = (lazy_fetch_run *)realloc(
                        open, next_capacity * sizeof(*grown));
                    if (!grown) {
                        ok = 0;
                        break;
                    }
                    open = grown;
                    open_capacity = next_capacity;
                }
                open[open_count++] = *run;
            }
        }
        if (ok) {
            for (size_t o = 0; o < open_count; ++o) {
                if (final_count == final_capacity) {
                    size_t next_capacity = final_capacity ? final_capacity * 2u : 16u;
                    lazy_fetch_run *grown = (lazy_fetch_run *)realloc(
                        final_runs, next_capacity * sizeof(*grown));
                    if (!grown) {
                        ok = 0;
                        break;
                    }
                    final_runs = grown;
                    final_capacity = next_capacity;
                }
                final_runs[final_count++] = open[o];
            }
        }
        free(open);
        free(runs);
        runs = final_runs;
        run_count = final_count;
    }

    /* Phase C: fetch every run in parallel (up to OTSARDB_LAZY_PARALLEL
     * ephemeral workers). The key for the range GET is taken from the
     * segment task, which is stable (tasks are never rebuilt mid-session). */
    if (ok && run_count > 0) {
        lazy_run_fetch_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.store = h->store;
        ctx.runs = runs;
        ctx.run_count = run_count;
        ctx.workers = (size_t)lazy_pool_parallelism();
        if (ctx.workers > run_count) ctx.workers = run_count;
        for (size_t r = 0; r < run_count; ++r) {
            lazy_fetch_run *run = &runs[r];
            const lazy_seg_task *task = &h->tasks[run->ordinal];
            const otsardb_commit_manifest *manifest =
                &h->manifests[task->ref.manifest_index];
            const otsardb_manifest_segment_ref *ref =
                &manifest->segments[task->ref.segment_index];
            if (run->range_start >= run->range_end ||
                run->range_end > ref->size ||
                run->range_end - run->range_start > SIZE_MAX) {
                run->rc = OTSARDB_STORE_ERROR;
                ok = 0;
            } else {
                snprintf(run->key, sizeof(run->key), "%s", task->ref.absolute_key);
            }
        }
        if (ok) {
            /* One per-worker ctx copy: the static partition is
             * worker_index-based, so each worker (including worker 0 on the
             * requesting thread) gets its own index. The runs array is
             * shared but the partitions write disjoint entries. */
            lazy_run_fetch_ctx worker_ctx[LAZY_POOL_MAX];
            for (size_t w = 0; w < ctx.workers; ++w) {
                worker_ctx[w] = ctx;
                worker_ctx[w].worker_index = w;
            }
            if (ctx.workers > 1) {
                size_t spawned = 0;
#if defined(_WIN32)
                uintptr_t handles[LAZY_POOL_MAX];
                for (size_t w = 1; w < ctx.workers; ++w) {
                    handles[w] = _beginthreadex(NULL, 0, lazy_run_fetch_worker,
                                                &worker_ctx[w], 0, NULL);
                    if (handles[w] == 0) {
                        handles[w] = 0;
                    } else {
                        ++spawned;
                    }
                }
#else
                pthread_t handles[LAZY_POOL_MAX];
                for (size_t w = 1; w < ctx.workers; ++w) {
                    if (pthread_create(&handles[w], NULL,
                                       lazy_run_fetch_worker, &worker_ctx[w]) == 0) {
                        ++spawned;
                    }
                }
#endif
                /* Worker 0 always runs on the requesting thread. A failed
                 * spawn leaves its partition's runs unfetched; they are
                 * fetched inline below (sequential fallback — correctness
                 * never depends on thread creation). */
                lazy_run_fetch_worker(&worker_ctx[0]);
                for (size_t w = 1; w < ctx.workers; ++w) {
#if defined(_WIN32)
                    if (handles[w]) {
                        WaitForSingleObject((HANDLE)handles[w], INFINITE);
                        CloseHandle((HANDLE)handles[w]);
                    }
#else
                    if (w <= spawned) pthread_join(handles[w], NULL);
#endif
                }
            } else {
                lazy_run_fetch_worker(&worker_ctx[0]);
            }
        }
        /* Sequential fallback for any run a failed worker spawn left
         * unfetched (rc still OK, no slice yet). */
        if (ok) {
            for (size_t r = 0; r < run_count; ++r) {
                lazy_fetch_run *run = &runs[r];
                if (run->rc == OTSARDB_STORE_OK && !run->slice.data) {
                    size_t span = (size_t)(run->range_end - run->range_start);
                    otsardb_store_result rc = otsardb_store_get_range(h->store,
                                                                  run->key,
                                                                  (size_t)run->range_start,
                                                                  span,
                                                                  &run->slice);
                    if (rc == OTSARDB_STORE_OK && run->slice.size != span) {
                        rc = OTSARDB_STORE_ERROR;
                    }
                    run->rc = rc;
                }
            }
        }
        /* count every attempted demand range GET, the bytes
         * received on success and the coalesced runs (>1 page per GET). */
        for (size_t r = 0; r < run_count; ++r) {
            lazy_fetch_run *run = &runs[r];
            lazy_stats_add64(&h->stats.total_gets, 1);
            lazy_stats_add64(&h->stats.range_gets, 1);
            if (run->rc == OTSARDB_STORE_OK) {
                lazy_stats_add64(&h->stats.bytes_fetched, run->slice.size);
                if (run->covered_hi - run->covered_lo > 1u) {
                    lazy_stats_add64(&h->stats.coalesced_runs, 1);
                }
                if (otsardb_lazy_timing_enabled()) {
                    fprintf(stderr,
                            "otsardb: timing lazy range GET %s off=%llu len=%zu "
                            "at %.0f ms\n",
                            run->key, (unsigned long long)run->range_start,
                            run->slice.size, monotonic_ms() - lazy_t0);
                }
            } else {
                if (otsardb_lazy_timing_enabled()) {
                    fprintf(stderr,
                            "otsardb: timing lazy range GET %s failed rc=%d\n",
                            run->key, (int)run->rc);
                }
                ok = 0;
            }
        }
        if (otsardb_lazy_timing_enabled()) {
            fprintf(stderr,
                    "otsardb: timing lazy inline runs=%zu workers=%zu at %.0f ms\n",
                    run_count, ctx.workers, monotonic_ms() - lazy_t0);
        }
    }

    /* Phase D: decode + validate + apply every run in order (requesting
     * thread, hydrator mutex held). A slice's b-th frame must be the block
     * of page pages[covered_lo + b] — the strict 0059 positional check now
     * also covers the gap bytes of a merged slice. A frame whose page is
     * outside the requested set (a gap page) is still validated but only
     * applied when it is unmaterialised and this segment is still its
     * newest claimant (bitmap + claimant rules decide every write). */
    for (size_t r = 0; r < run_count && ok; ++r) {
        lazy_fetch_run *run = &runs[r];
        if (run->rc != OTSARDB_STORE_OK) {
            if (otsardb_lazy_timing_enabled()) {
                fprintf(stderr,
                        "otsardb: timing lazy range GET %s failed rc=%d\n",
                        run->key, (int)run->rc);
            }
            ok = 0;
            break;
        }
        const size_t covered_count = run->covered_hi - run->covered_lo;
        const lazy_seg_task *task = &h->tasks[run->ordinal];
        const otsardb_commit_manifest *manifest =
            &h->manifests[task->ref.manifest_index];
        const otsardb_manifest_segment_ref *ref =
            &manifest->segments[task->ref.segment_index];
        const uint32_t *pages = ref->pages;
        const size_t req_lo = run->page_idx_start;
        const size_t req_hi = run->page_idx_start + run->page_idx_count;
        size_t pos = 0;
        for (size_t b = 0; b < covered_count && ok; ++b) {
            unsigned char *frame = NULL;
            size_t frame_size = 0;
            uint32_t frame_page = 0;
            if (!otsardb_segment_frame_decode(run->slice.data, run->slice.size, &pos,
                                            &frame, &frame_size, &frame_page)) {
                ok = 0;
                break;
            }
            if (frame_size != 8u + (size_t)h->page_size ||
                frame_page != pages[run->covered_lo + b]) {
                free(frame);
                ok = 0;
                break;
            }
            /* Apply only requested pages, and only when the page is still
             * unmaterialised and this segment is still its newest claimant
             * (bitmap + claimant rules decide every write; for a merged
             * slice the gap frames fail these checks and are skipped). */
            const size_t idx = run->covered_lo + b;
            if (idx >= req_lo && idx < req_hi && !bitmap_get(h, frame_page) &&
                lazy_resolve_page(h, frame_page) == run->ordinal) {
                uint64_t offset = ((uint64_t)frame_page - 1u) * (uint64_t)h->page_size;
                FILE *file = NULL;
                if (offset <= INT64_MAX &&
                    (file = fopen(h->database_path, "r+b")) != NULL) {
#if defined(_WIN32)
                    int seek_ok = _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
                    int seek_ok = fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
                    if (seek_ok &&
                        fwrite(frame + 8u, 1, h->page_size, file) == h->page_size) {
                        if (fclose(file) == 0) {
                            bitmap_set(h, frame_page);
                            /* demand-path miss. */
                            lazy_stats_add64(&h->stats.demand_misses, 1);
                            /* sequential read-ahead streak
                             * (every inline-applied page was a demand miss). */
                            lazy_note_miss(h, frame_page);
                            /* cache the verified bytes. */
                            lazy_cache_store(h, frame_page, frame + 8u);
                        } else {
                            ok = 0;
                        }
                    } else {
                        fclose(file);
                        ok = 0;
                    }
                } else {
                    if (file) fclose(file);
                    ok = 0;
                }
            }
            free(frame);
        }
        if (ok && pos != run->slice.size) ok = 0;
    }

    /* Release every slice (also the ones after a fail-closed break). */
    for (size_t r = 0; r < run_count; ++r) {
        if (runs[r].slice.data) {
            otsardb_store_release_object(h->store, &runs[r].slice);
        }
    }

    free(runs);
    free(reqs);
    return ok;
}

int otsardb_lazy_hydrate_range(otsardb_lazy_hydrator *h,
                             uint32_t first_page,
                             uint32_t last_page) {
    if (!h) return 0;
    if (first_page == 0) first_page = 1;
    if (first_page > last_page) return 1;

    lazy_mutex_lock(&h->mutex);
    if (h->fetch_failed) {
        lazy_mutex_unlock(&h->mutex);
        return 0;
    }
    /* (LH-3) defense in depth: the restore path rejects
     * database_size_pages >= 2^32-1 (uint32 page numbering), but a
     * mis-constructed hydrator must fail closed here too — the uint32 clamp
     * would otherwise skip pages past the wrap and, at UINT32_MAX, the
     * `page <= last_page` loop below would wrap and never terminate. */
    if (h->database_size_pages >= (uint64_t)UINT32_MAX) {
        lazy_mutex_unlock(&h->mutex);
        return 0;
    }
    if ((uint64_t)last_page > h->database_size_pages) {
        last_page = (uint32_t)h->database_size_pages;
    }
    if (!h->pool_started && h->task_count > 0) lazy_pool_start(h);
    const int sync_mode = h->pool_workers == 0;
    const uint64_t generation = h->generation;
    const double t_start = monotonic_ms();

    /* materialise pages owned by frame-offset segments with
     * byte-range GETs first (they are never fetched whole by the pool). Any
     * failure here fails closed; the whole-fetch loop below re-checks the
     * bitmap so these pages are never fetched twice. pages
     * covered by an in-flight prefetch window are skipped here (the worker
     * fetches them) and waited on below. */
    int ok = lazy_hydrate_frames_inline(h, first_page, last_page, 1);

    for (uint32_t page = first_page; page <= last_page && ok; ++page) {
        /* stores without RANGE_GET never run the inline
         * pass (Phase A returns early), so the RAM tier is resolved here —
         * whole-object pages were cached at apply time. With RANGE_GET the
         * inline pass already resolved the whole range against the tier. */
        if (!otsardb_store_supports(h->store, OTSARDB_STORE_CAP_RANGE_GET) &&
            lazy_cache_resolve_page(h, page)) {
            continue;
        }
        if (bitmap_get(h, page)) continue;

        size_t ordinal = lazy_resolve_page(h, page);
        if (ordinal == SIZE_MAX) {
            if (!lazy_fetch_unknown_segments(h)) {
                ok = 0;
                break;
            }
            /* The unknown segments were applied and marked every page they
             * own; re-check before failing closed. */
            if (bitmap_get(h, page)) continue;
            if (lazy_resolve_page(h, page) == SIZE_MAX) {
                if (lazy_covered_by_checkpoint(h, page)) {
                    /* The page lives in the fully-materialised checkpoint
                     * base image. */
                    bitmap_set(h, page);
                    /* the page was a demand miss served by
                     * the base image (no fetch). */
                    lazy_stats_add64(&h->stats.demand_misses, 1);
                    /* a checkpoint-covered page is a miss
                     * (it was unmaterialised); it resolves to no segment, so
                     * no prefetch is scheduled, but the streak keeps the
                     * sequential detection continuous across the boundary. */
                    lazy_note_miss(h, page);
                    continue;
                }
                /* Fail closed: no authenticated segment claims this page. */
                fprintf(stderr,
                        "otsardb: lazy hydration: page %u is claimed by no segment "
                        "of the validated chain\n",
                        page);
                ok = 0;
                break;
            }
            ordinal = lazy_resolve_page(h, page);
        }

        /* Frame-offset segments are only serviced by the inline path; if a
         * page still resolves to one here, the inline pass left it
         * unmaterialised because an active prefetch window covers it (
         * 0074). Wait for the window (wire cost of one range GET, no
         * redundant demand fetch), then re-check: a completed window marks the
         * page; a dropped window falls back to a direct demand fetch. Fail
         * closed only when the page is still missing after that — a wrong or
         * partial page is never served. */
        if (h->tasks[ordinal].ref.has_frame_offsets) {
            if (lazy_prefetch_covers(h, ordinal, page)) {
                lazy_wait_prefetch_job(h, ordinal, page, generation);
            }
            if (bitmap_get(h, page)) {
                /* the page was materialised by the window
                 * we waited on — a prefetch hit. */
                if (lazy_prefetch_job_done(h, ordinal, page)) {
                    lazy_stats_add64(&h->stats.prefetch_hits, 1);
                }
                continue;
            }
            if (lazy_prefetch_covers(h, ordinal, page)) {
                /* Still covered (overlapping schedule): demand must never
                 * wedge on a stuck window — fetch the page directly. */
                if (!lazy_hydrate_frames_inline(h, page, page, 0)) {
                    ok = 0;
                    break;
                }
                if (bitmap_get(h, page)) continue;
            }
            ok = 0;
            break;
        }

        /* Wait for the owning task to be fetched (the workers drain the
         * queue in ordinal order), then apply every pending task up to and
         * including it in strict ordinal order. */
        if (!lazy_wait_task_fetched(h, ordinal, generation, sync_mode)) {
            ok = 0;
            break;
        }
        while (h->next_apply <= ordinal && ok) {
            size_t i = h->next_apply;
            /* Frame-offset segments are applied inline (never by the pool):
             * their ordinals advance without a fetch/apply. */
            if (h->tasks[i].ref.has_frame_offsets) {
                ++h->next_apply;
                continue;
            }
            if (h->tasks[i].state == LAZY_TASK_IDLE ||
                h->tasks[i].state == LAZY_TASK_FETCHING) {
                if (!lazy_wait_task_fetched(h, i, generation, sync_mode)) {
                    ok = 0;
                    break;
                }
            }
            if (h->tasks[i].state == LAZY_TASK_FAILED) {
                ok = 0;
                break;
            }
            if (h->tasks[i].state == LAZY_TASK_FETCHED) {
                if (!lazy_task_apply(h, &h->tasks[i], i)) {
                    fprintf(stderr,
                            "otsardb: lazy hydration: failed to decode/apply segment %s "
                            "(page %u); failing closed\n",
                            h->tasks[i].ref.absolute_key, page);
                    ok = 0;
                    break;
                }
                h->tasks[i].state = LAZY_TASK_APPLIED;
            }
            ++h->next_apply;
        }
    }

    if (!ok) h->fetch_failed = 1;
    /* hydration wall time (demand path, incl. prefetch
     * waits). */

    lazy_stats_add64(&h->stats.hydration_ms, (uint64_t)(monotonic_ms() - t_start));
    lazy_mutex_unlock(&h->mutex);
    return ok;
}

int otsardb_lazy_restore_store_ex(otsardb_object_store *store,
                                const char *database_root,
                                const char *database_path,
                                const otsardb_head *head_in,
                                otsardb_lazy_hydrator **out_hydrator,
                                const otsardb_cipher *cipher) {
    if (!store || !database_root || !database_path || !out_hydrator) return 0;

    int created = 0;
    otsardb_lazy_hydrator *h = *out_hydrator;
    if (!h) {
        h = (otsardb_lazy_hydrator *)calloc(1, sizeof(*h));
        if (!h) return 0;
        created = 1;
        lazy_mutex_init(&h->mutex);
        lazy_cond_init(&h->cond);
        lazy_mutex_lock(&h->mutex);
    } else {
        /* In-place rebuild (lease takeover): stop and join the prefetch
         * workers FIRST (they wait on the mutex), then take the mutex for
         * the whole rebuild so a concurrent page hydrate can never touch a
         * half-replaced task/manifest array. The old generation's waiters
         * observe the bumped generation and fail closed. */
        lazy_pool_shutdown(h);
        lazy_mutex_lock(&h->mutex);
        lazy_release_state(h);
    }
    *out_hydrator = h;
    ++h->generation;

    h->store = store;
    h->cipher = cipher;
    snprintf(h->database_root, sizeof(h->database_root), "%s", database_root);
    snprintf(h->database_path, sizeof(h->database_path), "%s", database_path);

    double t0 = monotonic_ms();
    int ok = 1;

    /* 1. Metadata (page_size / database_id anchor). */
    otsardb_db_metadata metadata;
    if (otsardb_db_metadata_read(store, database_root, &metadata) != OTSARDB_METADATA_OK) {
        ok = 0;
        goto fail;
    }

    /* 2. HEAD (reuse the caller's read when available). */
    otsardb_head head;
    char *head_etag = NULL;
    if (head_in) {
        head = *head_in;
    } else if (otsardb_journal_read_head(store, database_root, &head, &head_etag) !=
               OTSARDB_JOURNAL_OK) {
        free(head_etag);
        ok = 0;
        goto fail;
    }
    free(head_etag);
    if (strcmp(head.database_id, metadata.database_id) != 0 || head.generation == 0 ||
        strlen(head.commit_sha256) != OTSARDB_MANIFEST_SHA256_HEX_SIZE) {
        ok = 0;
        goto fail;
    }

    otsardb_lazy_timing_note("lazy head+metadata", monotonic_ms() - t0);

    /* 3. Checkpoint base image (best-effort: any failure degrades to a full
     * chain walk; the pointer's sha256 and the manifest's hash are verified
     * exactly like checkpoint-accelerated recovery). */
    otsardb_checkpoint_pointer cp_ptr;
    char *cp_etag = NULL;
    uint64_t covered_generation = 0;
    if (otsardb_checkpoint_ptr_read(store, database_root, &cp_ptr, &cp_etag) ==
            OTSARDB_CHECKPOINT_PTR_OK &&
        cp_ptr.covered_generation > 0 &&
        cp_ptr.covered_generation <= head.generation &&
        cp_ptr.checkpoint_key[0] != '\0') {
        free(cp_etag);

        otsardb_store_object cp_obj = {0};
        if (otsardb_store_get(store, cp_ptr.checkpoint_key, &cp_obj) == OTSARDB_STORE_OK) {
            char cp_sha[OTSARDB_CHECKPOINT_SHA256_HEX_SIZE + 1];
            otsardb_checkpoint_manifest cp;
            if (otsardb_sha256_hex_data(cp_obj.data, cp_obj.size, cp_sha) &&
                strcmp(cp_sha, cp_ptr.checkpoint_sha256) == 0 &&
                otsardb_checkpoint_manifest_decode((const char *)cp_obj.data,
                                                 cp_obj.size, &cp)) {
                otsardb_store_release_object(store, &cp_obj);
                if (strcmp(cp.database_id, metadata.database_id) == 0 &&
                    cp.page_size == metadata.page_size &&
                    cp.database_size_pages > 0 &&
                    otsardb_recovery_apply_checkpoint_pages(store,
                                                          database_root,
                                                          &cp,
                                                          database_path)) {
                    covered_generation = cp.covered_generation;
                    h->covered_size_pages = cp.database_size_pages;
                }
            } else {
                otsardb_store_release_object(store, &cp_obj);
            }
        }
    } else {
        free(cp_etag);
    }
    otsardb_lazy_timing_note("lazy checkpoint", monotonic_ms() - t0);

    /* 4. Validate the chain and collect the lazy segment index. When no
     * checkpoint is involved, a parallel deterministic fast path fetches
     * every manifest in windows and validates with the same checks; any
     * deviation (multi-writer salts, legacy v1, missing objects) falls back
     * to the sequential walk. The walk stops at the checkpoint's covered
     * generation (inclusive); without a checkpoint the whole chain is
     * validated down to genesis. */
    lazy_collect_ctx collect;
    memset(&collect, 0, sizeof(collect));
    collect.hydrator = h;
    collect.covered_generation = covered_generation;
    collect.ok = 1;

    {
        const char *parallel_env = getenv("OTSARDB_LAZY_CHAIN_PARALLEL");
        int parallel_ok = !parallel_env || strcmp(parallel_env, "0") != 0;
        int fast_path_done = covered_generation == 0 && parallel_ok &&
                             lazy_try_parallel_chain(store,
                                                     database_root,
                                                     &metadata,
                                                     &head,
                                                     lazy_chain_visitor,
                                                     &collect) > 0;
        if (!fast_path_done && collect.ok && parallel_ok) {
            /* Multi-salt chains (one WAL per writer session): discover every
             * manifest via a single list request, fetch them in parallel,
             * and validate the chain by parent links through the set. */
            int list_result = lazy_try_list_chain(store,
                                                  database_root,
                                                  &metadata,
                                                  &head,
                                                  lazy_chain_visitor,
                                                  &collect);
            if (list_result < 0) {
                ok = 0;
                goto fail;
            }
            fast_path_done = list_result > 0;
        }
        if (!fast_path_done && collect.ok &&
            (!otsardb_recovery_walk_chain(store,
                                        database_root,
                                        &metadata,
                                        &head,
                                        covered_generation,
                                        lazy_chain_visitor,
                                        &collect) ||
             !collect.ok ||
             h->manifest_count == 0)) {
            ok = 0;
            goto fail;
        }
        if (!collect.ok) {
            /* A fast path's visitor aborted (OOM): fail, never fall back
             * onto an already-partially-populated hydrator. */
            ok = 0;
            goto fail;
        }
    }

    /* 5. Size the image from the HEAD manifest (manifests[0]). */
    const otsardb_commit_manifest *head_manifest = &h->manifests[0];
    if (head_manifest->database_size_pages == 0) {
        ok = 0;
        goto fail;
    }
    /* (LH-3): page numbers are uint32_t throughout the lazy
     * hydrator (bitmap index, hydrate loops, frame page fields). A database
     * with >= 2^32 pages (>= 16 TiB at 4 KiB) cannot be served here:
     * truncating to uint32 silently skips every page beyond the wrap and, at
     * exactly UINT32_MAX, the `page <= last_page` loops never terminate
     * (++page wraps to 0). Fail closed at open instead of hydrating a wrong
     * or stuck image. Non-lazy (full-recovery) sessions are unaffected. */
    if (head_manifest->database_size_pages >= (uint64_t)UINT32_MAX) {
        fprintf(stderr,
                "otsardb: lazy hydration refuses a database of %llu pages "
                "(>= 2^32-1): page numbering exceeds the uint32_t range of the "
                "lazy hydrator\n",
                (unsigned long long)head_manifest->database_size_pages);
        ok = 0;
        goto fail;
    }
    h->page_size = head_manifest->page_size;
    h->database_size_pages = head_manifest->database_size_pages;

    /* initialise the RAM page cache (OTSARDB_S3_RAM_CACHE_MB,
     * default 64, 0 = off). Allocation failure disables it gracefully. */
    lazy_cache_init(h);

    size_t bitmap_bytes = (size_t)((h->database_size_pages + 7u) / 8u);
    h->bitmap = (unsigned char *)calloc(bitmap_bytes ? bitmap_bytes : 1u, 1u);
    if (!h->bitmap) {
        ok = 0;
        goto fail;
    }
    h->bitmap_bytes = bitmap_bytes;
    h->marked_pages = 0;

    /* Pages covered by the checkpoint base are NOT pre-marked: pages touched
     * by segments newer than the covered generation must still resolve to
     * those segments. Unclaimed pages inside the covered range are marked
     * on demand by otsardb_lazy_hydrate_range (the base image is the newest
     * state for them). */

    /* 5b. Build the prefetch task array (one task per segment with a known
     * page set, newest-first). */
    lazy_build_tasks(h);
    if (h->fetch_failed) {
        ok = 0;
        goto fail;
    }

    /* (LH-2): count the checkpoint-base pages that are
     * materialised without a bitmap mark (covered by the base and claimed by
     * NO newer segment). is_complete must count them, otherwise a session
     * that opens over a checkpoint base never reports the image complete
     * until every page is touched and the close-time cache save silently
     * never happens (next open pays a full recovery). Only pages whose base
     * bytes are genuinely the newest state are counted: pages claimed by a
     * newer segment (and every page beyond the covered range) still require
     * a bitmap mark, so a partial image is never cached as authoritative. */
    {
        uint64_t limit = h->covered_size_pages;
        if (limit > h->database_size_pages) limit = h->database_size_pages;
        h->base_complete_pages =
            limit == 0 ? 0 : lazy_count_covered_unclaimed(h, limit);
    }

    /* 6. Create/truncate the local image at the full head size so SQLite
     * reads inside the header-declared range never go short. */
    uint64_t file_bytes = h->database_size_pages * (uint64_t)h->page_size;
    if (file_bytes == 0 || !lazy_truncate_file(database_path, file_bytes)) {
        ok = 0;
        goto fail;
    }
    otsardb_lazy_timing_note("lazy file init", monotonic_ms() - t0);

    lazy_mutex_unlock(&h->mutex);
    return 1;

fail:
    otsardb_lazy_timing_note("lazy restore FAILED", monotonic_ms() - t0);
    if (created) {
        lazy_mutex_unlock(&h->mutex);
        otsardb_lazy_destroy(h);
        *out_hydrator = NULL;
    } else {
        /* In-place rebuild failure: leave the struct valid but poisoned so
         * any later hydrate fails closed instead of serving stale data. */
        lazy_release_state(h);
        h->fetch_failed = 1;
        lazy_mutex_unlock(&h->mutex);
    }
    return 0;
}

int otsardb_lazy_restore_store(otsardb_object_store *store,
                             const char *database_root,
                             const char *database_path,
                             const otsardb_head *head_in,
                             otsardb_lazy_hydrator **out_hydrator) {
    /* Legacy entry point: falls back to the thread-local session cipher (set
     * by s3_database.cpp at open), which is NULL in every pre-0079 caller —
     * the plaintext path is byte-identical. */
    return otsardb_lazy_restore_store_ex(store, database_root, database_path, head_in,
                                       out_hydrator, otsardb_cipher_thread_get());
}

int otsardb_lazy_hydrate_bytes(otsardb_lazy_hydrator *h,
                             long long offset,
                             int amount) {
    if (!h || amount <= 0 || offset < 0 || h->page_size == 0) return 0;
    uint64_t start_page = (uint64_t)offset / (uint64_t)h->page_size + 1u;
    uint64_t end_byte = (uint64_t)offset + (uint64_t)amount - 1u;
    uint64_t end_page = end_byte / (uint64_t)h->page_size + 1u;
    if (end_page > UINT32_MAX) end_page = UINT32_MAX;
    return otsardb_lazy_hydrate_range(h, (uint32_t)start_page, (uint32_t)end_page);
}

int otsardb_lazy_is_complete(const otsardb_lazy_hydrator *h) {
    if (!h) return 0;
    /* (LH-2): pages covered by the checkpoint base that no
     * newer segment claims are materialised (the base image was fully applied
     * at restore) but are NOT bitmap-marked — they must be counted here, or
     * an image opened over a checkpoint base is never complete until every
     * page is touched and the close-time cache save silently never happens.
     * base_complete_pages excludes every page claimed by a newer segment, so
     * the gate never accepts a partial image. */
    return h->marked_pages + h->base_complete_pages >= h->database_size_pages;
}

void otsardb_lazy_mark_range(otsardb_lazy_hydrator *h, uint32_t first_page, uint32_t last_page) {
    if (!h) return;
    lazy_mutex_lock(&h->mutex);
    if (first_page == 0) first_page = 1;
    for (uint32_t p = first_page; p <= last_page; ++p) {
        bitmap_set(h, p);
        /* SQLite wrote the page — the cached copy is
         * superseded and must never be served after a bitmap clear. */
        lazy_cache_remove(h, p);
    }
    lazy_mutex_unlock(&h->mutex);
}

void otsardb_lazy_mark_bytes(otsardb_lazy_hydrator *h,
                           long long offset,
                           int amount) {
    if (!h || amount <= 0 || offset < 0 || h->page_size == 0) return;
    uint64_t start_page = (uint64_t)offset / (uint64_t)h->page_size + 1u;
    uint64_t end_page = ((uint64_t)offset + (uint64_t)amount - 1u) /
                            (uint64_t)h->page_size +
                        1u;
    if (end_page > UINT32_MAX) end_page = UINT32_MAX;
    otsardb_lazy_mark_range(h, (uint32_t)start_page, (uint32_t)end_page);
}

void otsardb_lazy_mark_all(otsardb_lazy_hydrator *h) {
    if (!h) return;
    lazy_mutex_lock(&h->mutex);
    if (h->bitmap && h->bitmap_bytes > 0) {
        memset(h->bitmap, 0xff, h->bitmap_bytes);
        h->marked_pages = h->database_size_pages;
    }
    /* the whole image was (re)materialised with the newest
     * state — cached bytes may be superseded by session writes. */
    lazy_cache_clear(h);
    lazy_mutex_unlock(&h->mutex);
}

void otsardb_lazy_clear_beyond(otsardb_lazy_hydrator *h, uint32_t page_count) {
    if (!h) return;
    lazy_mutex_lock(&h->mutex);
    if ((uint64_t)page_count < h->database_size_pages) {
        bitmap_clear_range(h, (uint64_t)page_count + 1u, h->database_size_pages);
    }
    lazy_mutex_unlock(&h->mutex);
}

void otsardb_lazy_clear_beyond_bytes(otsardb_lazy_hydrator *h, long long bytes) {
    if (!h || bytes < 0 || h->page_size == 0) return;
    uint64_t pages = (uint64_t)bytes / (uint64_t)h->page_size;
    otsardb_lazy_clear_beyond(h, pages > UINT32_MAX ? UINT32_MAX : (uint32_t)pages);
}

void otsardb_lazy_destroy(otsardb_lazy_hydrator *h) {
    if (!h) return;
    lazy_pool_shutdown(h);
    /* OTSARDB_S3_READ_STATS=1 prints the
     * canonical per-session line at session close. The pool is joined, so
     * the counters are stable; the mutex is still alive here. */
    if (lazy_read_stats_enabled()) {
        otsardb_lazy_hydration_stats stats;
        otsardb_lazy_hydration_stats_get(h, &stats);
        otsardb_lazy_hydration_stats_print(&stats);
        /* the RAM tier counters ride on a second line so
         * the canonical 0082 line stays byte-identical (its format test
         * pins the exact bytes). */
        char ram_line[128];
        if (otsardb_lazy_hydration_stats_format_ram(&stats,
                                                  ram_line,
                                                  sizeof(ram_line)) > 0) {
            fputs(ram_line, stderr);
        }
    }
    lazy_release_state(h);
    lazy_cond_destroy(&h->cond);
    lazy_mutex_destroy(&h->mutex);
    free(h);
}

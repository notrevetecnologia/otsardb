#include "s3_object_store.h"

/* Threading follows the project convention (s3_database.cpp lease thread,
 * forward.c server thread): _beginthreadex + WaitForSingleObject on
 * Windows, pthread on POSIX (GitHub Actions S3 workflow builds Linux). */
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <miniocpp/client.h>
#include <miniocpp/providers.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

/* miniocpp attaches one process-wide CURLSH
 * (miniocpp/http.cc GlobalCurlShare) that shares the CONNECTION POOL
 * (CURL_LOCK_DATA_CONNECT) to every request. libcurl documents the
 * shared connection pool as NOT thread-safe in the share API (see
 * https://curl.se/libcurl/c/threadsafe.html), and the windowed parallel
 * part uploads drove 4-16 concurrent transfers through
 * that shared pool. Observed result: intermittent 0xC0000005 in
 * libcurl!url_match_destination (url.c ~977/989) with zero output at
 * ~1.1% of otsardb-s3-wal-e2e runs - a torn/NULL read of a pooled
 * connection's fields while another worker thread concurrently attached,
 * reused or tore down a connection in the same shared bundle (2 full
 * dumps captured, identical signature, faulting offset 0x6c18b in
 * libcurl.dll 8.21.0). Every miniocpp Client call in this translation
 * unit now runs under this mutex, so no two threads are ever inside
 * libcurl's transfer machinery at the same time and the shared pool is
 * only accessed from one thread - curl's supported single-threaded use.
 * The windowed buffering of (calling thread reads the
 * pull source; workers only upload) and the multipart wire format are
 * unchanged; only the wire-level concurrency of UploadPart calls is
 * serialized. */
std::mutex g_s3_request_mutex;

struct s3_context {
    std::string bucket;
    std::string prefix;
    std::string region;
    std::unique_ptr<minio::s3::BaseUrl> base_url;
    std::unique_ptr<minio::creds::Provider> provider;
    std::unique_ptr<minio::s3::Client> client;
};

static std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static otsardb_store_result map_error(const std::string &message) {
    std::string lower = lower_copy(message);
    if (lower.find("nosuchkey") != std::string::npos ||
        lower.find("object does not exist") != std::string::npos ||
        lower.find("status code 404") != std::string::npos) {
        return OTSARDB_STORE_NOT_FOUND;
    }
    if (lower.find("preconditionfailed") != std::string::npos ||
        lower.find("precondition failed") != std::string::npos ||
        lower.find("status code 412") != std::string::npos) {
        return OTSARDB_STORE_PRECONDITION_FAILED;
    }
    if (lower.find("timeout") != std::string::npos ||
        lower.find("timed out") != std::string::npos) {
        return OTSARDB_STORE_TIMEOUT;
    }
    if (lower.find("not implemented") != std::string::npos ||
        lower.find("methodnotallowed") != std::string::npos ||
        lower.find("status code 501") != std::string::npos) {
        return OTSARDB_STORE_UNSUPPORTED;
    }
    return OTSARDB_STORE_ERROR;
}

static char *dup_cstr(const std::string &value) {
    char *copy = static_cast<char *>(std::malloc(value.size() + 1));
    if (!copy) return nullptr;
    std::memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    return copy;
}

static std::string canonical_etag(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

static std::string etag_header_value(const char *etag) {
    if (!etag || etag[0] == '\0') return {};
    std::string value = canonical_etag(etag);
    return "\"" + value + "\"";
}

static std::string object_name(const s3_context *ctx, const char *key) {
    std::string clean_key = key ? key : "";
    while (!clean_key.empty() && clean_key.front() == '/') clean_key.erase(clean_key.begin());
    if (ctx->prefix.empty()) return clean_key;
    if (clean_key.empty()) return ctx->prefix;
    return ctx->prefix + "/" + clean_key;
}

template <typename ResponseT>
static std::string response_etag(const ResponseT &response) {
    std::string etag = response.etag;
    if (etag.empty()) etag = response.headers.GetFront("ETag");
    return canonical_etag(etag);
}

/* Parse an RFC 7231 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") into unix
 * seconds; 0 when absent/unparsable. Feeds otsardb_store_object
 * .last_modified_secs so the M2 writer lease can judge a foreign lease's
 * expiry from the server clock (lease design). */
static int64_t http_date_to_unix_seconds(const std::string &value) {
    if (value.empty()) return 0;
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");
    if (stream.fail()) return 0;
    tm.tm_isdst = 0;
#if defined(_WIN32)
    return static_cast<int64_t>(_mkgmtime(&tm));
#else
    return static_cast<int64_t>(timegm(&tm));
#endif
}

static otsardb_store_result fill_object(const std::string &data,
                                      const std::string &etag,
                                      otsardb_store_object *out) {
    if (!out) return OTSARDB_STORE_ERROR;
    std::memset(out, 0, sizeof(*out));

    out->data = static_cast<unsigned char *>(std::malloc(data.empty() ? 1 : data.size()));
    if (!out->data) return OTSARDB_STORE_ERROR;
    if (!data.empty()) std::memcpy(out->data, data.data(), data.size());
    out->size = data.size();

    out->etag = dup_cstr(etag);
    if (!out->etag) {
        std::free(out->data);
        std::memset(out, 0, sizeof(*out));
        return OTSARDB_STORE_ERROR;
    }
    return OTSARDB_STORE_OK;
}

static otsardb_store_result s3_get_common(otsardb_object_store *store,
                                        const char *key,
                                        int use_range,
                                        size_t offset,
                                        size_t length,
                                        otsardb_store_object *out) {
    if (!store || !key || !out) return OTSARDB_STORE_ERROR;
    auto *ctx = static_cast<s3_context *>(store->context);

    minio::s3::GetObjectArgs args;
    args.bucket = ctx->bucket;
    args.object = object_name(ctx, key);
    args.region = ctx->region;

    size_t range_offset = offset;
    size_t range_length = length;
    if (use_range) {
        args.offset = &range_offset;
        args.length = &range_length;
    }

    std::string body;
    /* (security audit finding F06): the GET body was
     * accumulated with no size cap, so a malicious store streaming an
     * arbitrarily large object could exhaust memory. Accumulate at most
     * OTSARDB_S3_GET_MAX_BYTES per request and fail closed beyond it: the
     * datafunc returns false, which aborts the transfer, so the oversized
     * body is never buffered. miniocpp treats a false datafunc as "stop
     * reading" and may still report success with the truncated body, so the
     * cap flag is checked after the request and turned into an explicit
     * error — a truncated object is never returned as OTSARDB_STORE_OK.
     * The cap applies to whole-object and range GETs alike (a store that
     * ignores the requested range is caught the same way). */
    size_t get_accumulated = 0;
    int get_capped = 0;
    args.datafunc = [&body, &get_accumulated, &get_capped](
                        minio::http::DataFunctionArgs chunk) -> bool {
        const size_t chunk_size = chunk.datachunk.size();
        if (chunk_size > OTSARDB_S3_GET_MAX_BYTES - get_accumulated) {
            std::fprintf(stderr,
                         "otsardb: GET aborted: response exceeds the %llu-byte "
                         "object cap (%llu + %llu bytes)\n",
                         (unsigned long long)OTSARDB_S3_GET_MAX_BYTES,
                         (unsigned long long)get_accumulated,
                         (unsigned long long)chunk_size);
            get_capped = 1;
            return false;
        }
        get_accumulated += chunk_size;
        body.append(chunk.datachunk);
        return true;
    };

    std::lock_guard<std::mutex> lock(g_s3_request_mutex);
    minio::s3::GetObjectResponse response = ctx->client->GetObject(args);
    if (!response) return map_error(response.Error().String());
    if (get_capped) {
        /* Fail closed: the transfer was aborted at the cap, yet miniocpp
         * reported the partial body as success — never hand a truncated
         * object to a consumer. */
        return OTSARDB_STORE_ERROR;
    }

    otsardb_store_result rc = fill_object(body, response_etag(response), out);
    if (rc == OTSARDB_STORE_OK) {
        out->last_modified_secs =
            http_date_to_unix_seconds(response.headers.GetFront("Last-Modified"));
    }
    return rc;
}

static otsardb_store_result s3_get(otsardb_object_store *store,
                                 const char *key,
                                 otsardb_store_object *out) {
    return s3_get_common(store, key, 0, 0, 0, out);
}

static otsardb_store_result s3_get_range(otsardb_object_store *store,
                                       const char *key,
                                       size_t offset,
                                       size_t length,
                                       otsardb_store_object *out) {
    if (length == 0) return OTSARDB_STORE_ERROR;
    return s3_get_common(store, key, 1, offset, length, out);
}

/* OTSARDB_S3_MP_MIN: minimum payload size for routing a
 * single `otsardb_store_put` through the parallel multipart machinery
 * (`s3_put_stream`). Default 8 MiB: below two 5 MiB parts
 * the multipart path's extra Create/Complete round trips cost more than a
 * single PUT saves, so small objects (manifests, HEAD, lease, probe
 * objects, small segments) keep the current single-PUT behavior. 0
 * disables routing entirely (current PUT for every payload); invalid
 * values fall back to the default. Read once per put call. */
static uint64_t mp_min_threshold() {
    const char *value = std::getenv("OTSARDB_S3_MP_MIN");
    if (!value || value[0] == '\0') return 8u * 1024u * 1024u;
    char *end = nullptr;
    long long n = std::strtoll(value, &end, 10);
    if (end == value || *end != '\0' || n < 0) return 8u * 1024u * 1024u;
    return static_cast<uint64_t>(n);
}

/* Pull-source adapter over one contiguous in-memory buffer, used to route
 * large `otsardb_store_put` payloads through the multipart machinery. The
 * source is only ever read on the calling thread (the worker threads of
 * the windowed part upload never invoke the pull callback), so no locking
 * is needed. */
struct memory_source {
    const unsigned char *data;
    size_t size;
    size_t pos;
};

static long memory_source_read(void *ctx, void *buffer, size_t max_size) {
    memory_source *src = static_cast<memory_source *>(ctx);
    if (!src || !buffer || src->pos >= src->size) return 0;
    size_t n = src->size - src->pos;
    if (n > max_size) n = max_size;
    std::memcpy(buffer, src->data + src->pos, n);
    src->pos += n;
    return static_cast<long>(n);
}

/* Defined below ; forward-declared so s3_put can route
 * large payloads through the windowed parallel multipart upload. */
static otsardb_store_result s3_put_stream(otsardb_object_store *store,
                                        const char *key,
                                        otsardb_store_source_read source_read,
                                        void *source_ctx,
                                        uint64_t expected_size,
                                        const otsardb_store_put_options *options,
                                        char **out_etag);

static otsardb_store_result s3_put(otsardb_object_store *store,
                                 const char *key,
                                 const void *data,
                                 size_t size,
                                 const otsardb_store_put_options *options,
                                 char **out_etag) {
    if (!store || !key || key[0] == '\0' || (!data && size != 0)) return OTSARDB_STORE_ERROR;
    if (out_etag) *out_etag = nullptr;
    auto *ctx = static_cast<s3_context *>(store->context);

    /* probe-gated multipart routing. When the capability
     * probe reported OTSARDB_STORE_CAP_MULTIPART and the payload is at least
     * OTSARDB_S3_MP_MIN bytes, the single PUT is replaced by the windowed
     * parallel multipart path (s3_put_stream): the caller's
     * buffer is streamed through a pull source on the calling thread, parts
     * are uploaded by concurrent workers, and the complete-response ETag is
     * returned. This is what puts the segment uploads of the commit publish
     * path (commit_publish.c immutable_put -> otsardb_store_put) on the
     * parallel multipart wire format while the store lacks the capability -
     * or the payload is below the threshold - the current single PUT below
     * is unchanged (fallback). Failure semantics are unchanged: the same
     * otsardb_store_result codes (PRECONDITION_FAILED, TIMEOUT, ERROR,
     * UNSUPPORTED) propagate exactly as the single PUT would. Memory bound:
     * the caller's buffer plus one window of buffered parts
     * (MP_PARALLEL x 5 MiB, up to 16 x 5 MiB at the clamp ceiling). */
    if (size > 0 &&
        otsardb_store_supports(store, OTSARDB_STORE_CAP_MULTIPART)) {
        uint64_t mp_min = mp_min_threshold();
        if (mp_min > 0 && static_cast<uint64_t>(size) >= mp_min) {
            memory_source source = {
                static_cast<const unsigned char *>(data),
                size,
                0,
            };
            return s3_put_stream(store,
                                 key,
                                 memory_source_read,
                                 &source,
                                 static_cast<uint64_t>(size),
                                 options,
                                 out_etag);
        }
    }

    if (size > static_cast<size_t>(LONG_MAX)) return OTSARDB_STORE_UNSUPPORTED;
    std::string payload;
    if (size) payload.assign(static_cast<const char *>(data), size);
    std::istringstream stream(payload);

    minio::s3::PutObjectArgs args(stream, static_cast<long>(size), 0);
    args.bucket = ctx->bucket;
    args.object = object_name(ctx, key);
    args.region = ctx->region;
    args.content_type = "application/octet-stream";

    if (options) {
        if (options->if_none_match) {
            args.extra_headers.Add("If-None-Match", "*");
        }
        if (options->if_match_etag) {
            args.extra_headers.Add("If-Match", etag_header_value(options->if_match_etag));
        }
    }

    std::lock_guard<std::mutex> lock(g_s3_request_mutex);
    minio::s3::PutObjectResponse response = ctx->client->PutObject(args);
    if (!response) return map_error(response.Error().String());

    if (out_etag) {
        std::string etag = response_etag(response);
        *out_etag = dup_cstr(etag);
        if (!*out_etag) return OTSARDB_STORE_ERROR;
    }
    return OTSARDB_STORE_OK;
}

static void s3_release(otsardb_object_store *store, otsardb_store_object *object) {
    (void)store;
    if (!object) return;
    std::free(object->data);
    std::free(object->etag);
    std::memset(object, 0, sizeof(*object));
}

static otsardb_store_result s3_delete_obj(otsardb_object_store *store, const char *key) {
    if (!store || !key || key[0] == '\0') return OTSARDB_STORE_ERROR;
    auto *ctx = static_cast<s3_context *>(store->context);

    minio::s3::RemoveObjectArgs args;
    args.bucket = ctx->bucket;
    args.object = object_name(ctx, key);
    args.region = ctx->region;

    std::lock_guard<std::mutex> lock(g_s3_request_mutex);
    minio::s3::RemoveObjectResponse response = ctx->client->RemoveObject(args);
    if (!response) return map_error(response.Error().String());
    return OTSARDB_STORE_OK;
}

/* List object keys under a logical prefix: used to discover
 * every commit manifest of a database root so the chain can be fetched in
 * parallel. The miniocpp iterator paginates internally (continuation
 * tokens); keys are returned WITHOUT the global OTSARDB_S3_PREFIX so
 * consumers see logical keys. A failed listing iterates zero items and the
 * caller falls back to the sequential chain walk (correct, just slower). */
static otsardb_store_result s3_list_objects(otsardb_object_store *store,
                                          const char *prefix,
                                          otsardb_store_list_callback callback,
                                          void *ctx) {
    if (!store || !callback) return OTSARDB_STORE_ERROR;
    auto *s3 = static_cast<s3_context *>(store->context);

    std::string logical_prefix = prefix ? prefix : "";
    std::string physical_prefix = object_name(s3, logical_prefix.c_str());
    if (!physical_prefix.empty() && physical_prefix.back() != '/') {
        physical_prefix += "/";
    }
    const size_t strip = physical_prefix.size();

    minio::s3::ListObjectsArgs args;
    args.bucket = s3->bucket;
    args.region = s3->region;
    args.prefix = physical_prefix;
    args.use_url_encoding_type = true;

    /* The result iterator paginates lazily (network round trips on ++),
     * so the whole iteration runs under the request mutex. */
    std::lock_guard<std::mutex> lock(g_s3_request_mutex);
    minio::s3::ListObjectsResult result = s3->client->ListObjects(args);
    size_t total = 0;
    while (result) {
        /* Strip the physical prefix; an empty remainder means the prefix
         * itself ("folder" marker) — skip it. */
        const std::string &name = (*result).name;
        std::string logical = name.size() > strip ? name.substr(strip) : std::string();
        if (!logical.empty()) {
            if (callback(logical.c_str(), ctx) != 0) return OTSARDB_STORE_OK;
            if (++total > 1000000u) return OTSARDB_STORE_ERROR;
        }
        ++result;
    }
    return OTSARDB_STORE_OK;
}

/* OTSARDB_S3_MP_PARALLEL: how many multipart parts are
 * uploaded concurrently per window. Default 4; clamped to [1, 16]. Read
 * once per put_stream call. */
static int multipart_parallelism() {
    const char *value = std::getenv("OTSARDB_S3_MP_PARALLEL");
    if (!value || value[0] == '\0') return 4;
    char *end = nullptr;
    long n = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return 4;
    if (n < 1) return 1;
    if (n > 16) return 16;
    return static_cast<int>(n);
}

/* One parallel part-upload job . The worker performs a
 * single UploadPart call and records the part ETag or the raw error
 * message. Workers NEVER touch the caller's pull callback: every part's
 * bytes are fully buffered on the calling thread before the window
 * fan-out. */
struct part_upload_job {
    s3_context *ctx = nullptr;
    std::string object;
    std::string upload_id;
    unsigned int part_number = 0;
    const std::string *data = nullptr;
    std::string etag;
    std::string error;
};

#if defined(_WIN32)
static unsigned __stdcall part_upload_thread_main(void *opaque)
#else
static void *part_upload_thread_main(void *opaque)
#endif
{
    auto *job = static_cast<part_upload_job *>(opaque);
    try {
        minio::s3::UploadPartArgs part_args;
        part_args.bucket = job->ctx->bucket;
        part_args.object = job->object;
        part_args.region = job->ctx->region;
        part_args.upload_id = job->upload_id;
        part_args.part_number = job->part_number;
        part_args.buf = nullptr;
        part_args.data = std::string_view(
            reinterpret_cast<const char *>(job->data->data()), job->data->size());
        part_args.part_size = job->data->size();

        /* serialized with every other miniocpp call in
         * the process - see g_s3_request_mutex comment. */
        std::lock_guard<std::mutex> lock(g_s3_request_mutex);
        minio::s3::UploadPartResponse part_resp =
            job->ctx->client->UploadPart(part_args);
        if (!part_resp) {
            job->error = part_resp.Error().String();
            std::fprintf(stderr,
                         "otsardb: multipart part %u failed: %s\n",
                         job->part_number, job->error.c_str());
        } else {
            job->etag = response_etag(part_resp);
        }
    } catch (const std::exception &ex) {
        job->error = ex.what();
        std::fprintf(stderr,
                     "otsardb: multipart part %u exception: %s\n",
                     job->part_number, job->error.c_str());
    } catch (...) {
        job->error = "unknown exception in multipart part worker";
        std::fprintf(stderr,
                     "otsardb: multipart part %u unknown exception\n",
                     job->part_number);
    }
#if defined(_WIN32)
    return 0;
#else
    return nullptr;
#endif
}

#if defined(_WIN32)
typedef uintptr_t otsardb_thread_t;
static int mp_thread_create(otsardb_thread_t *out, part_upload_job *job) {
    uintptr_t handle = _beginthreadex(nullptr, 0, part_upload_thread_main, job, 0, nullptr);
    if (handle == 0) return -1;
    *out = handle;
    return 0;
}
static void mp_thread_join(otsardb_thread_t thread) {
    if (thread == 0) return;
    WaitForSingleObject(reinterpret_cast<HANDLE>(thread), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(thread));
}
#else
typedef pthread_t otsardb_thread_t;
static int mp_thread_create(otsardb_thread_t *out, part_upload_job *job) {
    return pthread_create(out, nullptr, part_upload_thread_main, job) == 0 ? 0 : -1;
}
static void mp_thread_join(otsardb_thread_t thread) { pthread_join(thread, nullptr); }
#endif

/* Reads exactly one multipart part (up to `want` bytes) from the pull
 * callback on the CALLING thread. Preserves the sequential adapter's read
 * semantics: the callback is polled until the part is full or returns 0
 * (source exhausted mid-part). Returns:
 * 1 part payload produced (data holds 1..want bytes),
 * 0 source exhausted with zero bytes for this part,
 * -1 source reported an error. */
static long read_part_payload(otsardb_store_source_read source_read,
                              void *source_ctx,
                              size_t want,
                              std::string *out) {
    out->clear();
    out->reserve(want);
    size_t total = 0;
    while (total < want) {
        size_t chunk = want - total;
        out->resize(total + chunk);
        long n = source_read(source_ctx, &(*out)[total], chunk);
        if (n < 0) return -1;
        if (n == 0) {
            out->resize(total);
            break;
        }
        total += static_cast<size_t>(n);
    }
    out->resize(total);
    return total == 0 ? 0 : 1;
}

static void abort_multipart_upload(s3_context *ctx,
                                   const std::string &obj_name,
                                   const std::string &upload_id) {
    minio::s3::AbortMultipartUploadArgs abort_args;
    abort_args.bucket = ctx->bucket;
    abort_args.region = ctx->region;
    abort_args.object = obj_name;
    abort_args.upload_id = upload_id;
    std::lock_guard<std::mutex> lock(g_s3_request_mutex);
    ctx->client->AbortMultipartUpload(abort_args);
}

static otsardb_store_result s3_put_stream(otsardb_object_store *store,
                                        const char *key,
                                        otsardb_store_source_read source_read,
                                        void *source_ctx,
                                        uint64_t expected_size,
                                        const otsardb_store_put_options *options,
                                        char **out_etag) {
    if (!store || !key || key[0] == '\0' || !source_read) return OTSARDB_STORE_ERROR;
    if (out_etag) *out_etag = nullptr;
    auto *ctx = static_cast<s3_context *>(store->context);

    if (expected_size == 0) {
        std::string buffer;
        buffer.reserve(1024 * 1024);
        const size_t chunk = 256u * 1024u;
        while (true) {
            size_t offset = buffer.size();
            buffer.resize(offset + chunk);
            long n = source_read(source_ctx, &buffer[offset], chunk);
            if (n < 0) return OTSARDB_STORE_ERROR;
            if (n == 0) {
                buffer.resize(offset);
                break;
            }
            buffer.resize(offset + static_cast<size_t>(n));
        }
        return s3_put(store, key, buffer.data(), buffer.size(), options, out_etag);
    }

    std::string obj_name = object_name(ctx, key);

    minio::s3::CreateMultipartUploadArgs create_args;
    create_args.bucket = ctx->bucket;
    create_args.object = obj_name;
    create_args.region = ctx->region;
    if (options) {
        if (options->if_none_match) {
            create_args.extra_headers.Add("If-None-Match", "*");
        }
        if (options->if_match_etag) {
            create_args.extra_headers.Add("If-Match", etag_header_value(options->if_match_etag));
        }
    }

    minio::s3::CreateMultipartUploadResponse create_resp = [&]() {
        std::lock_guard<std::mutex> lock(g_s3_request_mutex);
        return ctx->client->CreateMultipartUpload(create_args);
    }();
    if (!create_resp) {
        std::fprintf(stderr,
                     "otsardb: create multipart failed: %s\n",
                     create_resp.Error().String().c_str());
        return map_error(create_resp.Error().String());
    }
    std::string upload_id = create_resp.upload_id;

    const int parallel = multipart_parallelism();

    /* parts are uploaded in windows of `parallel` parts.
     * Each window's payloads are fully read from the pull callback on the
     * CALLING thread (workers never call it), then the window fans out to
     * one worker thread per part. Every spawned worker is joined before any
     * result is inspected and before any early return, so no thread can
     * outlive this call on any path. The first failing part aborts the
     * upload (best-effort AbortMultipartUpload) and propagates its error;
     * in-flight UploadPart calls of the window are allowed to finish first
     * and their parts are discarded with the abort. Memory is bounded by
     * window_size * part_size. */
    std::list<minio::s3::Part> parts;
    unsigned int part_number = 0;
    uint64_t remaining = expected_size;

    std::vector<std::string> payloads;    /* owns part bytes until join    */
    std::vector<part_upload_job> jobs;    /* one job per part in the window */
    std::vector<otsardb_thread_t> threads;  /* worker handles                 */
    std::vector<int> spawned;             /* which workers actually ran     */
    payloads.reserve(static_cast<size_t>(parallel));
    jobs.reserve(static_cast<size_t>(parallel));
    threads.reserve(static_cast<size_t>(parallel));
    spawned.reserve(static_cast<size_t>(parallel));

    while (remaining > 0 || part_number == 0) {
        /* Read up to `parallel` parts from the source (caller thread only). */
        payloads.clear();
        while (static_cast<int>(payloads.size()) < parallel &&
               (remaining > 0 || part_number == 0)) {
            size_t want = static_cast<size_t>(
                std::min<uint64_t>(remaining, minio::utils::kMinPartSize));
            if (want == 0) break;
            std::string payload;
            long rc = read_part_payload(source_read, source_ctx, want, &payload);
            if (rc < 0) {
                /* No worker is in flight yet; abort and report the error. */
                abort_multipart_upload(ctx, obj_name, upload_id);
                return OTSARDB_STORE_ERROR;
            }
            if (rc == 0) break; /* source exhausted before this part */
            remaining -= static_cast<uint64_t>(payload.size());
            part_number++;
            payloads.push_back(std::move(payload));
        }
        if (payloads.empty()) break; /* nothing left to upload */

        /* Fan-out: one worker per part of this window. A failed spawn marks
         * the job with an error so the window still aborts below. */
        const size_t window = payloads.size();
        jobs.resize(window);
        threads.assign(window, 0);
        spawned.assign(window, 0);
        for (size_t i = 0; i < window; ++i) {
            part_upload_job &job = jobs[i];
            job.ctx = ctx;
            job.object = obj_name;
            job.upload_id = upload_id;
            job.part_number = static_cast<unsigned int>(part_number) -
                              static_cast<unsigned int>(window) + 1u +
                              static_cast<unsigned int>(i);
            job.data = &payloads[i];
            job.etag.clear();
            job.error.clear();
            if (mp_thread_create(&threads[i], &job) == 0) {
                spawned[i] = 1;
            } else {
                job.error = "failed to spawn multipart part worker";
            }
        }

        /* Join EVERY spawned worker before inspecting any result. */
        for (size_t i = 0; i < window; ++i) {
            if (spawned[i]) mp_thread_join(threads[i]);
        }

        std::string first_error;
        for (size_t i = 0; i < window; ++i) {
            if (!jobs[i].error.empty()) {
                first_error = jobs[i].error;
                break;
            }
        }
        if (!first_error.empty()) {
            std::fprintf(stderr,
                         "otsardb: multipart window %u abort: %s\n",
                         part_number, first_error.c_str());
            abort_multipart_upload(ctx, obj_name, upload_id);
            return map_error(first_error);
        }

        for (size_t i = 0; i < window; ++i) {
            parts.emplace_back(jobs[i].part_number, jobs[i].etag);
        }
    }

    if (parts.empty()) {
        std::fprintf(stderr, "otsardb: multipart upload produced no parts\n");
        abort_multipart_upload(ctx, obj_name, upload_id);
        return OTSARDB_STORE_ERROR;
    }

    minio::s3::CompleteMultipartUploadArgs complete_args;
    complete_args.bucket = ctx->bucket;
    complete_args.object = obj_name;
    complete_args.region = ctx->region;
    complete_args.upload_id = upload_id;
    complete_args.parts = std::move(parts);

    minio::s3::CompleteMultipartUploadResponse complete_resp = [&]() {
        std::lock_guard<std::mutex> lock(g_s3_request_mutex);
        return ctx->client->CompleteMultipartUpload(complete_args);
    }();
    if (!complete_resp) {
        std::fprintf(stderr,
                     "otsardb: multipart complete failed: %s\n",
                     complete_resp.Error().String().c_str());
        abort_multipart_upload(ctx, obj_name, upload_id);
        return map_error(complete_resp.Error().String());
    }

    if (out_etag) {
        std::string etag = response_etag(complete_resp);
        *out_etag = dup_cstr(etag);
        if (!*out_etag) return OTSARDB_STORE_ERROR;
    }
    return OTSARDB_STORE_OK;
}

static const otsardb_object_store_ops S3_OPS = {
    s3_get,
    s3_get_range,
    s3_put,
    s3_put_stream,
    s3_release,
    s3_delete_obj,
    s3_list_objects,
};

static int parse_endpoint(const otsardb_s3_config *config,
                          std::string *out_host,
                          bool *out_https) {
    if (!config || !config->endpoint || !out_host || !out_https) return 0;
    std::string endpoint = config->endpoint;
    bool https = config->use_https != 0;

    const std::string https_prefix = "https://";
    const std::string http_prefix = "http://";
    if (endpoint.rfind(https_prefix, 0) == 0) {
        https = true;
        endpoint.erase(0, https_prefix.size());
    } else if (endpoint.rfind(http_prefix, 0) == 0) {
        https = false;
        endpoint.erase(0, http_prefix.size());
    }

    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    if (endpoint.empty() || endpoint.find('/') != std::string::npos) return 0;

    *out_host = endpoint;
    *out_https = https;
    return 1;
}

static std::string normalize_prefix(const char *prefix) {
    std::string value = prefix ? prefix : "";
    while (!value.empty() && value.front() == '/') value.erase(value.begin());
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

}  // namespace

extern "C" otsardb_object_store *otsardb_s3_store_create(const otsardb_s3_config *config) {
    if (!config || !config->bucket || config->bucket[0] == '\0') {
        return nullptr;
    }

    std::string host;
    bool https = true;
    if (!parse_endpoint(config, &host, &https)) return nullptr;

    auto *store = static_cast<otsardb_object_store *>(std::calloc(1, sizeof(otsardb_object_store)));
    auto *ctx = new (std::nothrow) s3_context();
    if (!store || !ctx) {
        std::free(store);
        delete ctx;
        return nullptr;
    }

    int use_explicit = config->access_key && config->access_key[0] != '\0' &&
                       config->secret_key && config->secret_key[0] != '\0';

    try {
        ctx->bucket = config->bucket;
        ctx->prefix = normalize_prefix(config->prefix);
        ctx->region = config->region ? config->region : "";
        ctx->base_url = std::make_unique<minio::s3::BaseUrl>(host, https, ctx->region);
        if (!static_cast<bool>(*ctx->base_url)) {
            std::free(store);
            delete ctx;
            return nullptr;
        }

        if (use_explicit) {
            ctx->provider = std::make_unique<minio::creds::StaticProvider>(
                config->access_key,
                config->secret_key,
                config->session_token ? config->session_token : "");
        } else {
            /* No explicit credentials �?" resolve from the environment, the
             * AWS config file, or the AWS instance profile / IAM role.
             * The chain tries each source in SDK-conventional order. */
            std::list<minio::creds::Provider *> chain;
            auto env_aws = new minio::creds::EnvAwsProvider();
            auto env_minio = new minio::creds::EnvMinioProvider();
            auto config_prov = new minio::creds::AwsConfigProvider();
            chain.push_back(env_aws);
            chain.push_back(env_minio);
            chain.push_back(config_prov);
            /* IAM instance-profile resolution (IMDS) is now OPT-IN via
             * OTSARDB_S3_ENABLE_IMDS=1: miniocpp's IamAwsProvider has no
             * bounded timeout and hangs indefinitely on Linux when no
             * credentials exist (observed >=284 s in the 0075 POSIX
             * health run vs ~10 s on Windows). Static keys, OTSARDB_S3_* /
             * AWS_* env and the AWS config file are still resolved
             * automatically (chain, updated in a later fix). */
            const char *imds_env = std::getenv("OTSARDB_S3_ENABLE_IMDS");
            const bool imds_enabled =
                imds_env &&
                (std::strcmp(imds_env, "1") == 0 ||
                 std::strcmp(imds_env, "true") == 0 ||
                 std::strcmp(imds_env, "yes") == 0 ||
                 std::strcmp(imds_env, "on") == 0);
            if (imds_enabled) {
                auto iam = new minio::creds::IamAwsProvider(minio::http::Url());
                chain.push_back(iam);
            }
            ctx->provider = std::make_unique<minio::creds::ChainedProvider>(std::move(chain));
        }

        ctx->client = std::make_unique<minio::s3::Client>(*ctx->base_url, ctx->provider.get());
        ctx->client->SetAppInfo("otsardb", "0.1.0-dev");
    } catch (...) {
        std::free(store);
        delete ctx;
        return nullptr;
    }

    store->ops = &S3_OPS;
    store->context = ctx;
    store->capabilities = 0;
    return store;
}

extern "C" void otsardb_s3_store_destroy(otsardb_object_store *store) {
    if (!store) return;
    delete static_cast<s3_context *>(store->context);
    std::free(store);
}

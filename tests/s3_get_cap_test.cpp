/* s3_get_cap_test.cpp — regression tests for the failure-mode finding
 * security audit finding F06 (GET body accumulated with no size cap).
 *
 * The S3 store datafunc appended every chunk of a GET response without a
 * bound, so a malicious/buggy store streaming an arbitrarily large object
 * could exhaust memory (defense-in-depth — manifests are otherwise
 * hash-verified). caps the accumulated bytes per request at
 * OTSARDB_S3_GET_MAX_BYTES (64 MiB) and fails closed beyond it: the
 * datafunc aborts the transfer, so the oversized body is never buffered.
 *
 * Discrimination: this test drives the REAL s3_object_store adapter against
 * a loopback HTTP server that behaves like an S3 store. A small object GETs
 * normally; an object larger than the cap must fail closed (never return
 * OK with the full body) AND the server must observe the client aborting
 * before the whole body was streamed (early termination, not a full
 * buffered read). Before the huge GET returned OK with the
 * full body in memory — the test FAILS on that behavior.
 *
 * Deterministic, no external MinIO needed: the fake store is a socket server
 * in this process.
 *
 * Wiring (coordinator): NOT applied here; CMakeLists.txt is
 * out of scope for this change set):
 * add_executable(otsardb_s3_get_cap_test
 * tests/s3_get_cap_test.cpp)
 * target_include_directories(otsardb_s3_get_cap_test
 * PRIVATE include src/core src/crypto src/storage)
 * target_link_libraries(otsardb_s3_get_cap_test
 * PRIVATE otsardb_core otsardb_s3_store)
 * add_test(NAME otsardb_s3_get_cap
 * COMMAND otsardb_s3_get_cap_test)
 * set_tests_properties(otsardb_s3_get_cap
 * PROPERTIES TIMEOUT 600)
 */
#include "object_store.h"
#include "s3_object_store.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_CLOSE close
#define SOL_SOCKET 1
#endif

namespace {

const size_t kSmallBody = 4096;
const uint64_t kCap = OTSARDB_S3_GET_MAX_BYTES;
const uint64_t kHugeBody = kCap + (4u * 1024u * 1024u);

std::atomic<int> g_mode{0};          /* 0 = small body, 1 = huge body */
std::atomic<uint64_t> g_huge_sent{0}; /* bytes actually written for the huge */
std::atomic<bool> g_stop{false};

#if defined(_WIN32)
typedef SOCKET socket_t;
#define SOCKET_CLOSE closesocket
#else
typedef int socket_t;
#endif

void send_all(socket_t sock, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = static_cast<int>(::send(sock, data + off,
                                        static_cast<int>(len - off), 0));
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

/* One HTTP request: read the request line + headers, then reply with a body
 * of the configured size. Tracks how many bytes of the huge body actually
 * reached the wire before the client aborted. */
void handle_request(socket_t sock) {
    char buf[4096];
    std::string headers;
    while (headers.find("\r\n\r\n") == std::string::npos) {
        int n = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
        if (n <= 0) return;
        headers.append(buf, static_cast<size_t>(n));
        if (headers.size() > 65536u) return;
    }

    const uint64_t body_size = g_mode.load() ? kHugeBody : kSmallBody;
    std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(body_size) +
                       "\r\nConnection: close\r\n\r\n";
    send_all(sock, head.data(), head.size());

    const size_t chunk = 64u * 1024u;
    char *fill = new char[chunk];
    std::memset(fill, 'A', chunk);
    uint64_t sent = 0;
    while (sent < body_size) {
        size_t n = std::min<uint64_t>(chunk, body_size - sent);
        int r = static_cast<int>(::send(sock, fill, static_cast<int>(n), 0));
        if (r <= 0) break;
        sent += static_cast<size_t>(r);
    }
    delete[] fill;
    g_huge_sent.store(sent);
}

void server_main(int port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(listener, SOL_SOCKET, 1 /* SO_REUSEADDR */,
               reinterpret_cast<const char *>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        SOCKET_CLOSE(listener);
        return;
    }
    if (::listen(listener, 8) != 0) {
        SOCKET_CLOSE(listener);
        return;
    }

    while (!g_stop.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        timeval tv{0, 200000};
        int sel = ::select(static_cast<int>(listener) + 1, &fds, nullptr,
                           nullptr, &tv);
        if (sel <= 0) continue;
        socket_t c = ::accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        handle_request(c);
        SOCKET_CLOSE(c);
    }
    SOCKET_CLOSE(listener);
}

int pick_free_port(void) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        SOCKET_CLOSE(s);
        return 0;
    }
    sockaddr_in got{};
    socklen_t len = sizeof(got);
    if (::getsockname(s, reinterpret_cast<sockaddr *>(&got), &len) != 0) {
        SOCKET_CLOSE(s);
        return 0;
    }
    int port = static_cast<int>(ntohs(got.sin_port));
    SOCKET_CLOSE(s);
    return port;
}

}  // namespace

int main(void) {
    const int port = pick_free_port();
    assert(port > 0);

    g_mode.store(0);
    std::thread server(server_main, port);

    std::string endpoint = "http://127.0.0.1:" + std::to_string(port);
    otsardb_s3_config config{};
    config.endpoint = endpoint.c_str();
    config.region = "us-east-1";
    config.bucket = "cap-bucket";
    config.access_key = "test-key";
    config.secret_key = "test-secret";
    config.prefix = "";
    config.use_https = 0;

    otsardb_object_store *store = otsardb_s3_store_create(&config);
    if (!store) {
        std::fprintf(stderr, "otsardb_s3_store_create failed\n");
        g_stop.store(true);
        server.join();
        return 1;
    }

    /* 1. A small object still GETs normally (the cap must not break
     * legitimate traffic). */
    otsardb_store_object obj{};
    otsardb_store_result rc = otsardb_store_get(store, "small", &obj);
    if (rc != OTSARDB_STORE_OK) {
        std::fprintf(stderr, "small GET failed: rc=%d\n", (int)rc);
        otsardb_s3_store_destroy(store);
        g_stop.store(true);
        server.join();
        return 1;
    }
    assert(obj.size == kSmallBody);
    otsardb_store_release_object(store, &obj);

    /* 2. An object larger than the cap must fail closed: no OK result, and
     * the transfer must abort before the full body is buffered. */
    g_mode.store(1);
    rc = otsardb_store_get(store, "huge", &obj);
    if (rc == OTSARDB_STORE_OK) {
        std::fprintf(stderr,
                     "huge GET unexpectedly succeeded (size=%zu); the size cap "
                     "is missing\n",
                     obj.size);
        otsardb_store_release_object(store, &obj);
        otsardb_s3_store_destroy(store);
        g_stop.store(true);
        server.join();
        return 1;
    }
    const uint64_t sent = g_huge_sent.load();
    if (sent >= kHugeBody) {
        std::fprintf(stderr,
                     "server streamed the FULL %llu-byte body (%llu sent); the "
                     "client did not abort early\n",
                     (unsigned long long)kHugeBody, (unsigned long long)sent);
        otsardb_s3_store_destroy(store);
        g_stop.store(true);
        server.join();
        return 1;
    }

    otsardb_s3_store_destroy(store);
    g_stop.store(true);
    server.join();

    std::printf("s3 GET object cap regression tests passed (F06): small GET "
                "OK; %llu-byte object refused, transfer aborted after %llu "
                "bytes\n",
                (unsigned long long)kHugeBody, (unsigned long long)sent);
    return 0;
}

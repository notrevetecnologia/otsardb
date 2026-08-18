#include "capability_probe.h"
#include "s3_object_store.h"

#include <cstdlib>
#include <iostream>
#include <string>

static const char *required_env(const char *name) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') {
        std::cerr << "missing required environment variable: " << name << "\n";
        return nullptr;
    }
    return value;
}

static const char *optional_env(const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] != '\0' ? value : nullptr;
}

int main() {
    const char *endpoint = required_env("OTSARDB_S3_ENDPOINT");
    const char *bucket = required_env("OTSARDB_S3_BUCKET");
    if (!endpoint || !bucket) return 2;

    otsardb_s3_config config = {};
    config.endpoint = endpoint;
    config.region = std::getenv("OTSARDB_S3_REGION");
    config.bucket = bucket;
    config.access_key = optional_env("OTSARDB_S3_ACCESS_KEY");
    config.secret_key = optional_env("OTSARDB_S3_SECRET_KEY");
    config.session_token = std::getenv("OTSARDB_S3_SESSION_TOKEN");
    config.prefix = std::getenv("OTSARDB_S3_PREFIX");
    config.use_https = 1;

    otsardb_object_store *store = otsardb_s3_store_create(&config);
    if (!store) {
        std::cerr << "unable to create OtsarDB S3 client\n";
        return 3;
    }

    std::string probe_key = "__otsardb_probe__/capabilities-v1";
    if (const char *custom_key = std::getenv("OTSARDB_S3_PROBE_KEY")) {
        if (custom_key[0] != '\0') probe_key = custom_key;
    }

    otsardb_store_probe_report report = {};
    otsardb_store_result rc = otsardb_store_probe_capabilities(store, probe_key.c_str(), &report);
    if (rc != OTSARDB_STORE_OK) {
        std::cerr << "S3 capability probe failed with OtsarDB store result " << static_cast<int>(rc) << "\n";
        otsardb_s3_store_destroy(store);
        return 4;
    }

    std::cout << "S3 Core: " << (report.s3_core_ok ? "yes" : "no") << "\n";
    std::cout << "S3 CAS: " << (report.s3_cas_ok ? "yes" : "no") << "\n";
    std::cout << "ETag: "
              << (otsardb_store_supports(store, OTSARDB_STORE_CAP_ETAG) ? "yes" : "no") << "\n";
    std::cout << "Range GET: "
              << (otsardb_store_supports(store, OTSARDB_STORE_CAP_RANGE_GET) ? "yes" : "no") << "\n";
    std::cout << "Read-after-write: "
              << (otsardb_store_supports(store, OTSARDB_STORE_CAP_READ_AFTER_WRITE) ? "yes" : "no") << "\n";
    std::cout << "If-None-Match: "
              << (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_CREATE) ? "yes" : "no") << "\n";
    std::cout << "If-Match: "
              << (otsardb_store_supports(store, OTSARDB_STORE_CAP_CONDITIONAL_UPDATE) ? "yes" : "no") << "\n";
    std::cout << "Multipart: "
              << (otsardb_store_supports(store, OTSARDB_STORE_CAP_MULTIPART) ? "yes" : "no") << "\n";

    if (report.s3_cas_ok) {
        std::cout << "OtsarDB write strategy: CAS\n";
    } else if (report.s3_core_ok) {
        std::cout << "OtsarDB write strategy: SINGLE_WRITER\n";
    } else {
        std::cout << "OtsarDB write strategy: UNSUPPORTED_FOR_WRITES\n";
    }

    otsardb_s3_store_destroy(store);
    return report.s3_core_ok ? 0 : 5;
}

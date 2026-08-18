#include "sha256.h"

#include <openssl/evp.h>

int otsardb_sha256(const void *data, size_t size, unsigned char out[OTSARDB_SHA256_SIZE]) {
    if ((!data && size != 0) || !out) return 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;

    unsigned int digest_size = 0;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, data, size) == 1 &&
             EVP_DigestFinal_ex(ctx, out, &digest_size) == 1 &&
             digest_size == OTSARDB_SHA256_SIZE;

    EVP_MD_CTX_free(ctx);
    return ok;
}

void otsardb_sha256_hex(const unsigned char digest[OTSARDB_SHA256_SIZE], char out[OTSARDB_SHA256_HEX_SIZE + 1]) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < OTSARDB_SHA256_SIZE; ++i) {
        out[i * 2] = HEX[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX[digest[i] & 0x0f];
    }
    out[OTSARDB_SHA256_HEX_SIZE] = '\0';
}

int otsardb_sha256_hex_data(const void *data, size_t size, char out[OTSARDB_SHA256_HEX_SIZE + 1]) {
    unsigned char digest[OTSARDB_SHA256_SIZE];
    if (!otsardb_sha256(data, size, digest)) return 0;
    otsardb_sha256_hex(digest, out);
    return 1;
}

#include "checksum.h"

#include <stdlib.h>
#include <string.h>

/* Largest page size the engine guarantees. accumulate() guards against
 * larger sizes so the on-stack buffer cannot overflow. */
#define OTSARDB_CHECKSUM_MAX_PAGE 65536

/* CRC-64-ISO: reflected algorithm with nominal polynomial
 * 0xD800000000000000. The implementation pre-inverts the state and the
 * result, so the effective init is 0xFFFFFFFFFFFFFFFF and the output is
 * inverted. Check value: crc64_iso("123456789") == 0xb90956c775a41001. */
static uint64_t crc64_iso(const unsigned char *data, size_t len) {
    uint64_t crc = 0xFFFFFFFFFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xD800000000000000u : 0u);
        }
    }
    return ~crc;
}

static void put_u64_be(unsigned char *out, uint64_t v) {
    out[0] = (unsigned char)(v >> 56);
    out[1] = (unsigned char)(v >> 48);
    out[2] = (unsigned char)(v >> 40);
    out[3] = (unsigned char)(v >> 32);
    out[4] = (unsigned char)(v >> 24);
    out[5] = (unsigned char)(v >> 16);
    out[6] = (unsigned char)(v >> 8);
    out[7] = (unsigned char)v;
}

uint64_t otsardb_checksum_accumulate(uint64_t current,
                                   uint32_t page_no,
                                   const void *page,
                                   size_t page_size) {
    unsigned char buf[8 + OTSARDB_CHECKSUM_MAX_PAGE];
    uint64_t crc;

    if (page == NULL || page_size > OTSARDB_CHECKSUM_MAX_PAGE) {
        return current;
    }

    put_u64_be(buf, (uint64_t)page_no);
    memcpy(buf + 8, page, page_size);
    crc = crc64_iso(buf, 8 + page_size);
    return current ^ crc;
}

uint64_t otsardb_checksum_compute(const void *pages,
                                size_t page_size,
                                uint32_t count) {
    const unsigned char *p = (const unsigned char *)pages;
    uint64_t c = 0;
    uint32_t i;

    if (pages == NULL) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        c = otsardb_checksum_accumulate(c, i + 1, p + (size_t)i * page_size, page_size);
    }
    return c;
}

uint64_t otsardb_checksum_file(FILE *file,
                             uint32_t page_size,
                             uint64_t page_count) {
    unsigned char *buf;
    uint64_t c = 0;
    uint64_t i;

    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return 0;
    }

    buf = (unsigned char *)malloc(page_size > 0 ? page_size : 1);
    if (buf == NULL) {
        return 0;
    }

    for (i = 0; i < page_count; ++i) {
        size_t n = fread(buf, 1, page_size, file);
        if (n < page_size) {
            memset(buf + n, 0, page_size - n);
        }
        c = otsardb_checksum_accumulate(c, (uint32_t)(i + 1), buf, page_size);
    }

    free(buf);
    return c;
}

void otsardb_checksum_hex(uint64_t checksum, char out[OTSARDB_CHECKSUM_HEX_SIZE + 1]) {
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < OTSARDB_CHECKSUM_HEX_SIZE; ++i) {
        out[OTSARDB_CHECKSUM_HEX_SIZE - 1 - i] = digits[checksum & 0xF];
        checksum >>= 4;
    }
    out[OTSARDB_CHECKSUM_HEX_SIZE] = '\0';
}

int otsardb_checksum_parse(const char *hex, uint64_t *out) {
    uint64_t v = 0;
    int i;

    if (hex == NULL || out == NULL) {
        return 0;
    }
    for (i = 0; i < OTSARDB_CHECKSUM_HEX_SIZE; ++i) {
        char c = hex[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            return 0;
        }
        v = (v << 4) | d;
    }
    if (hex[OTSARDB_CHECKSUM_HEX_SIZE] != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

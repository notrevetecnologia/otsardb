#include "checksum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Replica of the CRC-64-ISO used by checksum.c (Go hash/crc64 semantics:
 * effective init 0xFFFFFFFFFFFFFFFF and final inversion); used only to
 * validate the algorithm against Go's crc64.ISO check value. */
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

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__);     \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void fill_pages(unsigned char *pages, size_t page_size, uint32_t count) {
    uint32_t i;
    size_t j;
    for (i = 0; i < count; ++i) {
        for (j = 0; j < page_size; ++j) {
            pages[(size_t)i * page_size + j] = (unsigned char)(i * 31 + j * 7);
        }
    }
}

int main(void) {
    enum { PAGE_SIZE = 64, COUNT = 4 };
    unsigned char pages[PAGE_SIZE * COUNT];
    unsigned char new_pages[PAGE_SIZE * COUNT];
    uint64_t c, c2, new_c, parsed;
    char hex[OTSARDB_CHECKSUM_HEX_SIZE + 1];
    FILE *f;

    fill_pages(pages, PAGE_SIZE, COUNT);

    /* 1. Determinism: two full computations over the same buffer agree. */
    c = otsardb_checksum_compute(pages, PAGE_SIZE, COUNT);
    c2 = otsardb_checksum_compute(pages, PAGE_SIZE, COUNT);
    CHECK(c == c2);

    /* 2. Rolling property: replace page 2 (1-indexed) and roll the checksum
     * in O(1) with two accumulates (XOR old out, XOR new in). */
    memcpy(new_pages, pages, sizeof(pages));
    for (size_t j = 0; j < PAGE_SIZE; ++j) {
        new_pages[PAGE_SIZE + j] ^= 0xA5;
    }
    new_c = otsardb_checksum_accumulate(
        otsardb_checksum_accumulate(c, 2, pages + PAGE_SIZE, PAGE_SIZE),
        2, new_pages + PAGE_SIZE, PAGE_SIZE);
    CHECK(new_c == otsardb_checksum_compute(new_pages, PAGE_SIZE, COUNT));

    /* 2b. Guard: oversized page leaves the checksum unchanged. */
    CHECK(otsardb_checksum_accumulate(c, 1, pages, 70000) == c);

    /* 3. Hex round-trip (lowercase encode, mixed-case parse). */
    otsardb_checksum_hex(c, hex);
    CHECK(strlen(hex) == OTSARDB_CHECKSUM_HEX_SIZE);
    CHECK(otsardb_checksum_parse(hex, &parsed) == 1);
    CHECK(parsed == c);

    otsardb_checksum_hex(parsed, hex);
    for (size_t j = 0; j < OTSARDB_CHECKSUM_HEX_SIZE; ++j) {
        if (hex[j] >= 'a' && hex[j] <= 'f') {
            hex[j] = (char)(hex[j] - 'a' + 'A');
        }
    }
    CHECK(otsardb_checksum_parse(hex, &parsed) == 1);
    CHECK(parsed == c);
    CHECK(otsardb_checksum_parse("xyz", &parsed) == 0);
    CHECK(otsardb_checksum_parse("0000000000000000", &parsed) == 1);
    CHECK(otsardb_checksum_parse("b90956c775a4100", &parsed) == 0);   /* 15 chars */
    CHECK(otsardb_checksum_parse("b90956c775a41001ff", &parsed) == 0); /* 18 chars */

    /* 4. File variant: full file equals buffer compute; truncated last page
     * (half missing) matches a zero-filled expectation. */
    f = tmpfile();
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(fwrite(pages, 1, sizeof(pages), f) == sizeof(pages));
        fflush(f);
        CHECK(otsardb_checksum_file(f, PAGE_SIZE, COUNT) == c);
        fclose(f);
    }

    f = tmpfile();
    CHECK(f != NULL);
    if (f != NULL) {
        size_t trunc_len = sizeof(pages) - PAGE_SIZE / 2;
        unsigned char expected[PAGE_SIZE * COUNT];

        CHECK(fwrite(pages, 1, trunc_len, f) == trunc_len);
        fflush(f);

        memcpy(expected, pages, sizeof(expected));
        memset(expected + trunc_len, 0, PAGE_SIZE / 2);
        CHECK(otsardb_checksum_file(f, PAGE_SIZE, COUNT) ==
              otsardb_checksum_compute(expected, PAGE_SIZE, COUNT));
        fclose(f);
    }

    /* 5. Known vector: CRC-64/ISO check value of "123456789" is
     * 0xb90956c775a41001 (Go crc64.ISO). */
    CHECK(crc64_iso((const unsigned char *)"123456789", 9) ==
          UINT64_C(0xb90956c775a41001));

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("checksum tests passed\n");
    return 0;
}

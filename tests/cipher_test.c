/* cipher_test.c — AES-256-GCM cipher wrapper unit tests .
 *
 * Deterministic, no store, no network. Exercises the two-phase encrypt API
 * (stream pass for the content-addressed key derivation + authenticated pass
 * with the real AAD), authenticated decrypt (wrong key / wrong AAD / tampered
 * bytes all fail closed), the hex-key contract, the thread-local registry and
 * the manifest "segment_enc" parser.
 *
 * Wiring (coordinator, listed in):
 * add_executable(otsardb_cipher_test tests/cipher_test.c src/crypto/cipher.c)
 * target_include_directories(otsardb_cipher_test PRIVATE src/crypto)
 * target_link_libraries(otsardb_cipher_test PRIVATE OpenSSL::Crypto)
 * add_test(NAME otsardb_cipher COMMAND otsardb_cipher_test)
 */
#include "cipher.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char KEY_A_HEX[] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char KEY_B_HEX[] =
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

static void test_hex_key_contract(void) {
    assert(otsardb_cipher_valid_hex_key(KEY_A_HEX));
    assert(otsardb_cipher_valid_hex_key(KEY_B_HEX));
    assert(!otsardb_cipher_valid_hex_key(NULL));
    assert(!otsardb_cipher_valid_hex_key(""));
    assert(!otsardb_cipher_valid_hex_key("0123456789abcdef"));          /* 32 chars */
    assert(!otsardb_cipher_valid_hex_key("zz02030405060708090a0b0c0d0e0f10"
                                       "1112131415161718191a1b1c1d1e1f00")); /* non-hex */
    assert(!otsardb_cipher_valid_hex_key("000102030405060708090a0b0c0d0e0f"
                                       "101112131415161718191a1b1c1d1e1f0")); /* 63 chars */
    assert(otsardb_cipher_create_from_hex(KEY_A_HEX) != NULL);
    assert(otsardb_cipher_create_from_hex("not-a-key") == NULL);
    assert(otsardb_cipher_create(NULL) == NULL);
}

static void test_round_trip(void) {
    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);

    const unsigned char plaintext[] =
        "the quick brown fox jumps over the lazy dog 0123456789";
    const char aad[] = "s3://bucket/root/segments/sha256/ab/abc.otsseg\ncommit-42";

    unsigned char iv[OTSARDB_CIPHER_IV_BYTES];
    unsigned char *stream = NULL;
    size_t stream_size = 0;
    assert(otsardb_cipher_encrypt_stream(cipher, plaintext, sizeof(plaintext),
                                       iv, &stream, &stream_size));
    assert(stream_size == sizeof(plaintext));

    unsigned char tag[OTSARDB_CIPHER_TAG_BYTES];
    unsigned char *envelope = NULL;
    size_t envelope_size = 0;
    assert(otsardb_cipher_encrypt_with_aad(cipher, iv, aad, strlen(aad),
                                         plaintext, sizeof(plaintext),
                                         &envelope, &envelope_size, tag));
    assert(envelope_size == sizeof(plaintext) + OTSARDB_CIPHER_OVERHEAD);
    assert(memcmp(envelope, iv, OTSARDB_CIPHER_IV_BYTES) == 0);
    /* The authenticated pass produces the same stream bytes as phase 1. */
    assert(memcmp(envelope + OTSARDB_CIPHER_IV_BYTES, stream, stream_size) == 0);
    assert(memcmp(envelope + OTSARDB_CIPHER_IV_BYTES + stream_size,
                  tag, OTSARDB_CIPHER_TAG_BYTES) == 0);

    unsigned char *decrypted = NULL;
    size_t decrypted_size = 0;
    assert(otsardb_cipher_decrypt(cipher, envelope, envelope_size,
                                aad, strlen(aad), &decrypted, &decrypted_size));
    assert(decrypted_size == sizeof(plaintext));
    assert(memcmp(decrypted, plaintext, sizeof(plaintext)) == 0);

    free(decrypted);
    free(envelope);
    free(stream);
    otsardb_cipher_destroy(cipher);
}

static void test_fails_closed(void) {
    const unsigned char plaintext[] = "attack at dawn; hold the line; 12345";
    const char aad[] = "s3://bucket/root/segments/sha256/ab/abc.otsseg\ncommit-42";
    const char wrong_aad[] = "s3://other-bucket/root/segments/sha256/ab/abc.otsseg\ncommit-42";

    otsardb_cipher *cipher_a = otsardb_cipher_create_from_hex(KEY_A_HEX);
    otsardb_cipher *cipher_b = otsardb_cipher_create_from_hex(KEY_B_HEX);
    assert(cipher_a && cipher_b);

    unsigned char iv[OTSARDB_CIPHER_IV_BYTES];
    unsigned char *stream = NULL;
    size_t stream_size = 0;
    assert(otsardb_cipher_encrypt_stream(cipher_a, plaintext, sizeof(plaintext),
                                       iv, &stream, &stream_size));
    free(stream);

    unsigned char *envelope = NULL;
    size_t envelope_size = 0;
    assert(otsardb_cipher_encrypt_with_aad(cipher_a, iv, aad, strlen(aad),
                                         plaintext, sizeof(plaintext),
                                         &envelope, &envelope_size, NULL));
    assert(envelope_size == sizeof(plaintext) + OTSARDB_CIPHER_OVERHEAD);

    /* Wrong key fails. */
    unsigned char *out = NULL;
    size_t out_size = 0;
    assert(!otsardb_cipher_decrypt(cipher_b, envelope, envelope_size,
                                 aad, strlen(aad), &out, &out_size));
    assert(out == NULL);

    /* Wrong AAD fails (same key, different object identity). */
    assert(!otsardb_cipher_decrypt(cipher_a, envelope, envelope_size,
                                 wrong_aad, strlen(wrong_aad), &out, &out_size));
    assert(out == NULL);

    /* Tampered ciphertext byte fails (GCM tag). */
    unsigned char *tampered = (unsigned char *)malloc(envelope_size);
    assert(tampered);
    memcpy(tampered, envelope, envelope_size);
    tampered[OTSARDB_CIPHER_IV_BYTES + 5] ^= 0x01;
    assert(!otsardb_cipher_decrypt(cipher_a, tampered, envelope_size,
                                 aad, strlen(aad), &out, &out_size));
    assert(out == NULL);

    /* Tampered tag fails. */
    memcpy(tampered, envelope, envelope_size);
    tampered[envelope_size - 1] ^= 0x80;
    assert(!otsardb_cipher_decrypt(cipher_a, tampered, envelope_size,
                                 aad, strlen(aad), &out, &out_size));
    assert(out == NULL);

    /* Short input fails. */
    assert(!otsardb_cipher_decrypt(cipher_a, envelope, OTSARDB_CIPHER_OVERHEAD - 1,
                                 aad, strlen(aad), &out, &out_size));
    assert(!otsardb_cipher_decrypt(cipher_a, envelope, 0,
                                 aad, strlen(aad), &out, &out_size));

    free(tampered);
    free(envelope);
    otsardb_cipher_destroy(cipher_a);
    otsardb_cipher_destroy(cipher_b);
}

static void test_iv_uniqueness(void) {
    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);
    const unsigned char plaintext[] = "same plaintext both times";
    const char aad[] = "s3://b/r/segments/sha256/ab/abc.otsseg\nc1";

    unsigned char iv1[OTSARDB_CIPHER_IV_BYTES];
    unsigned char iv2[OTSARDB_CIPHER_IV_BYTES];
    unsigned char *s1 = NULL;
    unsigned char *s2 = NULL;
    size_t n1 = 0;
    size_t n2 = 0;
    assert(otsardb_cipher_encrypt_stream(cipher, plaintext, sizeof(plaintext),
                                       iv1, &s1, &n1));
    assert(otsardb_cipher_encrypt_stream(cipher, plaintext, sizeof(plaintext),
                                       iv2, &s2, &n2));
    /* Random IVs: two independent encrypts must never produce the same
     * envelope (a fresh IV per object is the GCM uniqueness requirement). */
    assert(memcmp(iv1, iv2, OTSARDB_CIPHER_IV_BYTES) != 0);
    assert(memcmp(s1, s2, n1) != 0);

    /* Each decrypts back under its own IV/AAD. */
    unsigned char *e1 = NULL;
    size_t e1_size = 0;
    unsigned char *e2 = NULL;
    size_t e2_size = 0;
    assert(otsardb_cipher_encrypt_with_aad(cipher, iv1, aad, strlen(aad),
                                         plaintext, sizeof(plaintext),
                                         &e1, &e1_size, NULL));
    assert(otsardb_cipher_encrypt_with_aad(cipher, iv2, aad, strlen(aad),
                                         plaintext, sizeof(plaintext),
                                         &e2, &e2_size, NULL));
    assert(e1_size == e2_size);
    assert(memcmp(e1, e2, e1_size) != 0);

    unsigned char *out = NULL;
    size_t out_size = 0;
    assert(otsardb_cipher_decrypt(cipher, e1, e1_size, aad, strlen(aad),
                                &out, &out_size));
    assert(out_size == sizeof(plaintext) && memcmp(out, plaintext, out_size) == 0);
    free(out);
    free(e1);
    free(e2);
    free(s1);
    free(s2);
    otsardb_cipher_destroy(cipher);
}

static void test_thread_registry(void) {
    otsardb_cipher *cipher = otsardb_cipher_create_from_hex(KEY_A_HEX);
    assert(cipher != NULL);
    assert(otsardb_cipher_thread_get() == NULL);
    otsardb_cipher_thread_set(cipher);
    assert(otsardb_cipher_thread_get() == cipher);
    otsardb_cipher_thread_set(NULL);
    assert(otsardb_cipher_thread_get() == NULL);
    otsardb_cipher_destroy(cipher);
}

static void test_segment_enc_parse(void) {
    /* Plaintext manifest: absent field -> present == 0. */
    const char plain_manifest[] =
        "{\"otsardb_format\":4,\"database_id\":\"d\",\"generation\":1,"
        "\"commit_id\":\"c1\",\"parent_generation\":0,\"parent_commit_id\":\"\","
        "\"parent_manifest_sha256\":\"\",\"page_size\":4096,"
        "\"database_size_pages\":2,\"segments\":[{\"key\":\"segments/sha256/ab/"
        "abcdef.otsseg\",\"sha256\":\"0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef\",\"size\":100,\"pages\":[]}]}\n";
    otsardb_segment_enc_set enc;
    assert(otsardb_segment_enc_parse_json(plain_manifest, strlen(plain_manifest), &enc));
    assert(!enc.present);
    assert(enc.count == 0);

    /* Encrypted manifest: two entries, second one plaintext ({}). */
    const char enc_manifest[] =
        "{\"otsardb_format\":4,\"database_id\":\"d\",\"generation\":1,"
        "\"commit_id\":\"c1\",\"parent_generation\":0,\"parent_commit_id\":\"\","
        "\"parent_manifest_sha256\":\"\",\"page_size\":4096,"
        "\"database_size_pages\":2,\"segments\":["
        "{\"key\":\"segments/sha256/ab/aaaaaaaa.otsseg\",\"sha256\":"
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"size\":128,\"pages\":[1,2]},"
        "{\"key\":\"segments/sha256/ab/bbbbbbbb.otsseg\",\"sha256\":"
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"size\":64,\"pages\":[3]}],"
        "\"segment_enc\":[{\"alg\":\"AES-256-GCM\",\"iv\":"
        "\"00112233445566778899aabb\",\"tag\":"
        "\"cafebabecafebabecafebabecafebabe\"},{}]}\n";
    assert(otsardb_segment_enc_parse_json(enc_manifest, strlen(enc_manifest), &enc));
    assert(enc.present);
    assert(enc.count == 2);
    assert(enc.entries[0].present);
    assert(strcmp(enc.entries[0].alg, "AES-256-GCM") == 0);
    assert(memcmp(enc.entries[0].iv,
                  "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb", 12) == 0);
    assert(memcmp(enc.entries[0].tag,
                  "\xca\xfe\xba\xbe\xca\xfe\xba\xbe\xca\xfe\xba\xbe\xca\xfe\xba\xbe", 16) == 0);
    assert(!enc.entries[1].present);

    /* Malformed: unknown algorithm. */
    const char bad_alg[] =
        "{\"otsardb_format\":4,\"database_id\":\"d\",\"generation\":1,"
        "\"commit_id\":\"c1\",\"parent_generation\":0,\"parent_commit_id\":\"\","
        "\"parent_manifest_sha256\":\"\",\"page_size\":4096,"
        "\"database_size_pages\":2,\"segments\":[],"
        "\"segment_enc\":[{\"alg\":\"AES-256-CTR\",\"iv\":"
        "\"00112233445566778899aabb\",\"tag\":"
        "\"cafebabecafebabecafebabecafebabe\"}]}\n";
    assert(!otsardb_segment_enc_parse_json(bad_alg, strlen(bad_alg), &enc));

    /* Malformed: wrong iv length. */
    const char bad_iv[] =
        "{\"otsardb_format\":4,\"database_id\":\"d\",\"generation\":1,"
        "\"commit_id\":\"c1\",\"parent_generation\":0,\"parent_commit_id\":\"\","
        "\"parent_manifest_sha256\":\"\",\"page_size\":4096,"
        "\"database_size_pages\":2,\"segments\":[],"
        "\"segment_enc\":[{\"alg\":\"AES-256-GCM\",\"iv\":\"00\",\"tag\":"
        "\"cafebabecafebabecafebabecafebabe\"}]}\n";
    assert(!otsardb_segment_enc_parse_json(bad_iv, strlen(bad_iv), &enc));
}

int main(void) {
    test_hex_key_contract();
    test_round_trip();
    test_fails_closed();
    test_iv_uniqueness();
    test_thread_registry();
    test_segment_enc_parse();
    printf("otsardb_cipher_test: ALL PASS\n");
    return 0;
}

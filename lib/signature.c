#include "signature.h"
#include "mem.h"
#include <stdbool.h>
#include <string.h>
#include <mbedtls/asn1.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/md.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/rsa.h>

static bool signature_spki(const uint8_t* key, size_t size) {
    unsigned char* cursor = (unsigned char*)key;
    const unsigned char* end = key + size;
    size_t length = 0;
    // require one complete SPKI envelope, not a private key or a valid key plus trailing data.
    return mbedtls_asn1_get_tag(&cursor, end, &length, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) == 0 &&
        length == (size_t)(end - cursor) &&
        mbedtls_asn1_get_tag(&cursor, end, &length, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) == 0;
}

static int signature_public_key(mbedtls_pk_context* context, SignatureScheme scheme,
        const uint8_t* key, size_t size, bool* raw) {
    static const char begin[] = "-----BEGIN PUBLIC KEY-----";
    static const char end[] = "-----END PUBLIC KEY-----";
    *raw = scheme == SIGNATURE_ECDSA_P256_SHA256 && size == 65 && key[0] == 4;
    if (*raw) {
        int result = mbedtls_pk_setup(context, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        if (result) return result;
        mbedtls_ecp_keypair* ec = mbedtls_pk_ec(*context);
        result = mbedtls_ecp_group_load(&ec->MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
        if (!result) result = mbedtls_ecp_point_read_binary(&ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q), key, size);
        if (!result) result = mbedtls_ecp_check_pubkey(&ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q));
        return result;
    }
    if (size < sizeof(begin) - 1 || memcmp(key, begin, sizeof(begin) - 1)) {
        if (!signature_spki(key, size)) return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
        return mbedtls_pk_parse_public_key(context, key, size);
    }
    if (memchr(key, 0, size)) return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    unsigned char* text = mem_alloc(size + 1, MEM_CAT_UNKNOWN);
    if (!text) return MBEDTLS_ERR_PK_ALLOC_FAILED;
    memcpy(text, key, size);
    text[size] = 0;
    mbedtls_pem_context pem;
    mbedtls_pem_init(&pem);
    size_t used = 0, der_size = 0;
    int result = mbedtls_pem_read_buffer(&pem, begin, end, text, NULL, 0, &used);
    if (!result) {
        for (size_t i = used; i < size; ++i)
            if (!strchr(" \t\r\n", text[i])) { result = MBEDTLS_ERR_PK_KEY_INVALID_FORMAT; break; }
    }
    const unsigned char* der = result ? NULL : mbedtls_pem_get_buffer(&pem, &der_size);
    if (!result && (!der || !signature_spki(der, der_size))) result = MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    if (!result) result = mbedtls_pk_parse_public_key(context, der, der_size);
    mbedtls_pem_free(&pem);
    mem_free(text);
    return result;
}

static int signature_p256_raw(mbedtls_ecp_keypair* ec, const uint8_t* digest,
        const uint8_t* signature, size_t size) {
    if (size != 64) return MBEDTLS_ERR_ECP_VERIFY_FAILED;
    mbedtls_mpi r, s, alternate;
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&alternate);
    int result = mbedtls_mpi_read_binary(&r, signature, 32);
    if (!result) result = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    // the raw SEC1/P1363 profile has a canonical low-S encoding; DER callers keep standard ECDSA semantics.
    if (!result && (mbedtls_mpi_cmp_int(&s, 0) <= 0 ||
            mbedtls_mpi_cmp_mpi(&s, &ec->MBEDTLS_PRIVATE(grp).N) >= 0)) result = MBEDTLS_ERR_ECP_VERIFY_FAILED;
    if (!result) result = mbedtls_mpi_sub_mpi(&alternate, &ec->MBEDTLS_PRIVATE(grp).N, &s);
    if (!result && mbedtls_mpi_cmp_mpi(&s, &alternate) > 0) result = MBEDTLS_ERR_ECP_VERIFY_FAILED;
    if (!result) result = mbedtls_ecdsa_verify(&ec->MBEDTLS_PRIVATE(grp), digest, 32, &ec->MBEDTLS_PRIVATE(Q), &r, &s);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_mpi_free(&alternate);
    return result;
}

SignatureResult signature_verify(SignatureScheme scheme, const uint8_t* key, size_t key_size,
        const uint8_t* message, size_t message_size, const uint8_t* signature, size_t signature_size) {
    static const uint8_t empty = 0;
    if (!key || !key_size || key_size > 64 * 1024 || (!message && message_size) ||
            !signature || !signature_size || signature_size > 1024 ||
            (scheme != SIGNATURE_RSA_PSS_SHA256 && scheme != SIGNATURE_ECDSA_P256_SHA256)) return SIGNATURE_INVALID;
    mbedtls_pk_context context;
    mbedtls_pk_init(&context);
    bool raw = false;
    int status = signature_public_key(&context, scheme, key, key_size, &raw);
    SignatureResult result = status == MBEDTLS_ERR_PK_ALLOC_FAILED ? SIGNATURE_ERROR : SIGNATURE_INVALID;
    uint8_t digest[32] = {0};
    if (!status) {
        const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!md || mbedtls_md(md, message ? message : &empty, message_size, digest)) result = SIGNATURE_ERROR;
        else if (scheme == SIGNATURE_RSA_PSS_SHA256 && mbedtls_pk_can_do(&context, MBEDTLS_PK_RSA) &&
                mbedtls_pk_get_bitlen(&context) >= 2048 && mbedtls_pk_get_bitlen(&context) <= 8192 &&
                signature_size == mbedtls_pk_get_len(&context)) {
            status = mbedtls_rsa_rsassa_pss_verify_ext(mbedtls_pk_rsa(context), MBEDTLS_MD_SHA256,
                sizeof(digest), digest, MBEDTLS_MD_SHA256, 32, signature);
            result = status ? SIGNATURE_INVALID : SIGNATURE_VALID;
        } else if (scheme == SIGNATURE_ECDSA_P256_SHA256 && mbedtls_pk_can_do(&context, MBEDTLS_PK_ECDSA) &&
                mbedtls_pk_ec(context)->MBEDTLS_PRIVATE(grp).id == MBEDTLS_ECP_DP_SECP256R1) {
            status = raw ? signature_p256_raw(mbedtls_pk_ec(context), digest, signature, signature_size) :
                mbedtls_pk_verify(&context, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature, signature_size);
            result = status ? SIGNATURE_INVALID : SIGNATURE_VALID;
        }
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_pk_free(&context);
    return result;
}

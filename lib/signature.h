#pragma once
#ifndef LAMBDA_API_ONLY
#define LAMBDA_API_ONLY
#define LAMBDA_API_ONLY_LOCAL
#endif
#include "../lambda/lambda.h"
#ifdef LAMBDA_API_ONLY_LOCAL
#undef LAMBDA_API_ONLY_LOCAL
#undef LAMBDA_API_ONLY
#endif
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum SignatureScheme {
    SIGNATURE_ED25519 = 0, /* reserved legacy value; unsupported by this provider */
    SIGNATURE_RSA_PSS_SHA256 = 1, SIGNATURE_ECDSA_P256_SHA256 = 2
} SignatureScheme;
typedef enum SignatureResult { SIGNATURE_VALID = 1, SIGNATURE_INVALID = 0, SIGNATURE_ERROR = -1 } SignatureResult;
// mbedCrypto only. RSA/P-256 public keys use strict PEM/DER SPKI and DER
// ECDSA signatures. Raw 65-byte SEC1 P-256 keys use canonical low-S r||s.
// No key files, ambient providers, modules or credentials are consulted.
LAMBDA_LIB_API SignatureResult signature_verify(SignatureScheme scheme,
    const uint8_t* key, size_t key_size, const uint8_t* message, size_t message_size,
    const uint8_t* signature, size_t signature_size);
#ifdef __cplusplus
}
#endif

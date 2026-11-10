#ifndef RA_MBEDTLS_VITA_CONFIG_H
#define RA_MBEDTLS_VITA_CONFIG_H

/* Minimal mbedTLS configuration for the on-device token vault.
 * Only AES-GCM, SHA-256 and HKDF are required. Do not enable PSA, TLS,
 * X.509, RSA, ECP, filesystem entropy or timing modules here: they inflate
 * the Adrenaline user module beyond the Vita module limit. */

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C

#endif /* RA_MBEDTLS_VITA_CONFIG_H */

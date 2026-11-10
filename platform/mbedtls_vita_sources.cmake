set(RA_MBEDTLS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/mbedtls")
set(RA_MBEDTLS_VITA_INCLUDE_DIRS
    "${RA_MBEDTLS_ROOT}/include"
    "${RA_MBEDTLS_ROOT}/library"
)

set(RA_MBEDTLS_VITA_SOURCES
    "${RA_MBEDTLS_ROOT}/library/aes.c"
    "${RA_MBEDTLS_ROOT}/library/cipher.c"
    "${RA_MBEDTLS_ROOT}/library/cipher_wrap.c"
    "${RA_MBEDTLS_ROOT}/library/constant_time.c"
    "${RA_MBEDTLS_ROOT}/library/gcm.c"
    "${RA_MBEDTLS_ROOT}/library/hkdf.c"
    "${RA_MBEDTLS_ROOT}/library/md.c"
    "${RA_MBEDTLS_ROOT}/library/platform.c"
    "${RA_MBEDTLS_ROOT}/library/platform_util.c"
    "${RA_MBEDTLS_ROOT}/library/sha256.c"
)

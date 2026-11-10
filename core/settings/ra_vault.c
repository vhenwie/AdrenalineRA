/* Device-bound encrypted credential vault. */

#include "ra_vault.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#else
#include <stdio.h>
#endif

#define MAGIC "RAV1"
#define AAD_SIZE (4 + RA_VAULT_SALT_SIZE + RA_VAULT_NONCE_SIZE + 4)
#define HEADER_SIZE (AAD_SIZE + RA_VAULT_TAG_SIZE)
#define MAX_PLAIN ((size_t)(2 + RA_VAULT_MAX_USERNAME + 2 + RA_VAULT_MAX_TOKEN))

static const unsigned char k_info[] = "ra-vita-vault-v1";
static const unsigned char k_secret[] = "ra-vita-vault-secret-v1";

static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)(value & 0xFF);
  p[1] = (unsigned char)((value >> 8) & 0xFF);
  p[2] = (unsigned char)((value >> 16) & 0xFF);
  p[3] = (unsigned char)((value >> 24) & 0xFF);
}

static uint16_t read_le16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_le16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)(value & 0xFF);
  p[1] = (unsigned char)((value >> 8) & 0xFF);
}

static int random_bytes(unsigned char *out, size_t length,
                        const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE]) {
  size_t i;
  unsigned long long state;

#if defined(__vita__)
  if (sceKernelGetRandomNumber(out, length) >= 0)
    return 0;
#else
  {
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom != NULL) {
      size_t got = fread(out, 1, length, urandom);
      fclose(urandom);
      if (got == length)
        return 0;
    }
  }
#endif

  state = (unsigned long long)(uintptr_t)device_id;
  state ^= (unsigned long long)time(NULL);
#if defined(__vita__)
  state ^= (unsigned long long)sceKernelGetSystemTimeWide();
#endif
  for (i = 0; i < length; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    out[i] = (unsigned char)(state >> 32);
  }
  return 0;
}

static int derive_key(const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
                      const unsigned char salt[RA_VAULT_SALT_SIZE],
                      unsigned char key[RA_VAULT_KEY_SIZE]) {
  unsigned char ikm[RA_VAULT_DEVICE_ID_SIZE + sizeof(k_secret) - 1];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  if (md == NULL)
    return -1;
  memcpy(ikm, device_id, RA_VAULT_DEVICE_ID_SIZE);
  memcpy(ikm + RA_VAULT_DEVICE_ID_SIZE, k_secret, sizeof(k_secret) - 1);
  return mbedtls_hkdf(md, salt, RA_VAULT_SALT_SIZE, ikm, sizeof(ikm),
                      k_info, sizeof(k_info) - 1, key, RA_VAULT_KEY_SIZE);
}

static int write_file(const char *path, const unsigned char *data,
                      size_t length) {
#if defined(__vita__)
  int fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  int written;

  if (fd < 0)
    return -1;
  written = sceIoWrite(fd, data, (SceSize)length);
  sceIoClose(fd);
  return written == (int)length ? 0 : -1;
#else
  FILE *file = fopen(path, "wb");
  size_t written;

  if (file == NULL)
    return -1;
  written = fwrite(data, 1, length, file);
  fclose(file);
  return written == length ? 0 : -1;
#endif
}

static int read_file(const char *path, unsigned char *data, size_t capacity,
                     size_t *length) {
#if defined(__vita__)
  int fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  int total = 0;
  int got;

  if (fd < 0)
    return -1;
  while ((size_t)total < capacity) {
    got = sceIoRead(fd, data + total, (SceSize)(capacity - (size_t)total));
    if (got <= 0)
      break;
    total += got;
  }
  sceIoClose(fd);
  *length = (size_t)total;
  return total > 0 ? 0 : -1;
#else
  FILE *file = fopen(path, "rb");
  size_t total;

  if (file == NULL)
    return -1;
  total = fread(data, 1, capacity, file);
  fclose(file);
  *length = total;
  return total > 0 ? 0 : -1;
#endif
}

static int vault_seal(unsigned char *blob, const unsigned char *salt,
                      const unsigned char *nonce, const unsigned char *key,
                      const unsigned char *plain, size_t plain_len) {
  unsigned char aad[AAD_SIZE];
  unsigned char tag[RA_VAULT_TAG_SIZE];
  mbedtls_gcm_context gcm;

  memcpy(aad, MAGIC, 4);
  memcpy(aad + 4, salt, RA_VAULT_SALT_SIZE);
  memcpy(aad + 4 + RA_VAULT_SALT_SIZE, nonce, RA_VAULT_NONCE_SIZE);
  write_le32(aad + 4 + RA_VAULT_SALT_SIZE + RA_VAULT_NONCE_SIZE,
             (uint32_t)plain_len);

  mbedtls_gcm_init(&gcm);
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) {
    mbedtls_gcm_free(&gcm);
    return -1;
  }
  memcpy(blob, aad, sizeof(aad));
  if (mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain_len,
                                nonce, RA_VAULT_NONCE_SIZE, aad, sizeof(aad),
                                plain, blob + HEADER_SIZE, sizeof(tag), tag) != 0) {
    mbedtls_gcm_free(&gcm);
    return -1;
  }
  memcpy(blob + AAD_SIZE, tag, sizeof(tag));
  mbedtls_gcm_free(&gcm);
  return 0;
}

int ra_vault_save(const char *path,
                  const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
                  const ra_vault_credentials_t *credentials) {
  unsigned char salt[RA_VAULT_SALT_SIZE];
  unsigned char nonce[RA_VAULT_NONCE_SIZE];
  unsigned char key[RA_VAULT_KEY_SIZE];
  unsigned char *plain = NULL;
  unsigned char *blob = NULL;
  size_t username_len;
  size_t token_len;
  size_t plain_len;
  int result = -1;

  if (path == NULL || device_id == NULL || credentials == NULL)
    return -1;
  username_len = strlen(credentials->username);
  token_len = strlen(credentials->token);
  if (username_len == 0 || username_len >= RA_VAULT_MAX_USERNAME ||
      token_len == 0 || token_len >= RA_VAULT_MAX_TOKEN)
    return -1;

  plain_len = 2 + username_len + 2 + token_len;
  if (plain_len > MAX_PLAIN)
    return -1;
  plain = (unsigned char *)malloc(plain_len);
  blob = (unsigned char *)malloc(HEADER_SIZE + plain_len);
  if (plain == NULL || blob == NULL)
    goto cleanup;

  write_le16(plain, (uint16_t)username_len);
  memcpy(plain + 2, credentials->username, username_len);
  write_le16(plain + 2 + username_len, (uint16_t)token_len);
  memcpy(plain + 2 + username_len + 2, credentials->token, token_len);

  if (random_bytes(salt, sizeof(salt), device_id) != 0 ||
      random_bytes(nonce, sizeof(nonce), device_id) != 0 ||
      derive_key(device_id, salt, key) != 0)
    goto cleanup;

  if (vault_seal(blob, salt, nonce, key, plain, plain_len) != 0)
    goto cleanup;
  result = write_file(path, blob, HEADER_SIZE + plain_len);

cleanup:
  memset(key, 0, sizeof(key));
  if (plain != NULL) {
    memset(plain, 0, plain_len);
    free(plain);
  }
  free(blob);
  return result;
}

static int vault_open_blob(const unsigned char *blob, size_t blob_len,
    unsigned char *salt, unsigned char *nonce, unsigned char *aad,
    size_t *plain_len) {
  if (blob_len < HEADER_SIZE || memcmp(blob, MAGIC, 4) != 0)
    return -1;

  memcpy(aad, blob, AAD_SIZE);
  memcpy(salt, blob + 4, RA_VAULT_SALT_SIZE);
  memcpy(nonce, blob + 4 + RA_VAULT_SALT_SIZE, RA_VAULT_NONCE_SIZE);
  *plain_len = read_le32(blob + 4 + RA_VAULT_SALT_SIZE + RA_VAULT_NONCE_SIZE);
  if (*plain_len == 0 || *plain_len > MAX_PLAIN ||
      blob_len != HEADER_SIZE + *plain_len)
    return -1;
  return 0;
}

static int vault_decrypt(const unsigned char *blob, const unsigned char *aad,
    const unsigned char *nonce, const unsigned char *key,
    unsigned char *plain, size_t plain_len) {
  mbedtls_gcm_context gcm;

  mbedtls_gcm_init(&gcm);
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) {
    mbedtls_gcm_free(&gcm);
    return -1;
  }
  if (mbedtls_gcm_auth_decrypt(&gcm, plain_len, nonce, RA_VAULT_NONCE_SIZE,
                               aad, AAD_SIZE, blob + AAD_SIZE,
                               RA_VAULT_TAG_SIZE, blob + HEADER_SIZE,
                               plain) != 0) {
    mbedtls_gcm_free(&gcm);
    return -1;
  }
  mbedtls_gcm_free(&gcm);
  return 0;
}

static int vault_decode(const unsigned char *plain, size_t plain_len,
    ra_vault_credentials_t *credentials) {
  uint16_t username_len;
  uint16_t token_len;

  if (plain_len < 4)
    return -1;
  username_len = read_le16(plain);
  if (username_len == 0 || username_len >= RA_VAULT_MAX_USERNAME ||
      (size_t)2 + username_len + 2 > plain_len)
    return -1;
  token_len = read_le16(plain + 2 + username_len);
  if (token_len == 0 || token_len >= RA_VAULT_MAX_TOKEN ||
      (size_t)2 + username_len + 2 + token_len != plain_len)
    return -1;

  memcpy(credentials->username, plain + 2, username_len);
  credentials->username[username_len] = '\0';
  memcpy(credentials->token, plain + 2 + username_len + 2, token_len);
  credentials->token[token_len] = '\0';
  return 0;
}

int ra_vault_load(const char *path,
                  const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
                  ra_vault_credentials_t *credentials) {
  unsigned char salt[RA_VAULT_SALT_SIZE];
  unsigned char nonce[RA_VAULT_NONCE_SIZE];
  unsigned char key[RA_VAULT_KEY_SIZE];
  unsigned char aad[AAD_SIZE];
  unsigned char *blob = NULL;
  unsigned char *plain = NULL;
  size_t blob_len = 0;
  size_t plain_len = 0;
  int result = -1;

  if (path == NULL || device_id == NULL || credentials == NULL)
    return -1;
  ra_vault_clear(credentials);
  blob = (unsigned char *)malloc(HEADER_SIZE + MAX_PLAIN);
  if (blob == NULL)
    return -1;

  if (read_file(path, blob, HEADER_SIZE + MAX_PLAIN, &blob_len) != 0)
    goto cleanup;
  if (vault_open_blob(blob, blob_len, salt, nonce, aad, &plain_len) != 0)
    goto cleanup;

  plain = (unsigned char *)malloc(plain_len);
  if (plain == NULL || derive_key(device_id, salt, key) != 0)
    goto cleanup;

  if (vault_decrypt(blob, aad, nonce, key, plain, plain_len) != 0)
    goto cleanup;
  if (vault_decode(plain, plain_len, credentials) != 0)
    goto cleanup;
  result = 0;

cleanup:
  memset(key, 0, sizeof(key));
  if (plain != NULL) {
    memset(plain, 0, plain_len);
    free(plain);
  }
  if (blob != NULL) {
    memset(blob, 0, HEADER_SIZE + MAX_PLAIN);
    free(blob);
  }
  if (result != 0)
    ra_vault_clear(credentials);
  return result;
}

void ra_vault_clear(ra_vault_credentials_t *credentials) {
  if (credentials != NULL) {
    volatile unsigned char *p = (volatile unsigned char *)credentials;
    size_t i;

    for (i = 0; i < sizeof(*credentials); ++i)
      p[i] = 0;
  }
}

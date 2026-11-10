/*
 * ra_vault.h -- device-bound encrypted credential vault.
 */

#ifndef RA_VAULT_H
#define RA_VAULT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_VAULT_DEVICE_ID_SIZE 16
#define RA_VAULT_SALT_SIZE 16
#define RA_VAULT_NONCE_SIZE 12
#define RA_VAULT_KEY_SIZE 32
#define RA_VAULT_TAG_SIZE 16
#define RA_VAULT_MAX_USERNAME 64
#define RA_VAULT_MAX_TOKEN 4096

typedef struct {
  char username[RA_VAULT_MAX_USERNAME];
  char token[RA_VAULT_MAX_TOKEN];
} ra_vault_credentials_t;

/* Encrypts username/token and writes a self-contained ra.cfg file.
 * The token is never written in plaintext. */
int ra_vault_save(const char *path,
                  const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
                  const ra_vault_credentials_t *credentials);

/* Loads and authenticates ra.cfg. Returns non-zero on malformed/tampered data. */
int ra_vault_load(const char *path,
                  const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
                  ra_vault_credentials_t *credentials);

void ra_vault_clear(ra_vault_credentials_t *credentials);

#ifdef __cplusplus
}
#endif

#endif /* RA_VAULT_H */

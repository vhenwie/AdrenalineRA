#include "ra_credentials.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/openpsid.h>

#include <stdio.h>
#include <string.h>

#include "ra_settings.h"
#include "ra_vault.h"

static int dir_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

static void ensure_dir(const char *path) {
  if (!dir_exists(path))
    sceIoMkdir(path, 0777);
}

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source != NULL ? source : "");
}

void ra_credentials_clear_files(void) {
  sceIoRemove(RA_CREDENTIALS_UR0);
  sceIoRemove(RA_CREDENTIALS_UX0);
  sceIoRemove(RA_SETTINGS_POLICY_PATH ".vanilla");
}

int ra_credentials_exists(void) {
  SceIoStat stat;

  return sceIoGetstat(RA_CREDENTIALS_UR0, &stat) >= 0 ||
    sceIoGetstat(RA_CREDENTIALS_UX0, &stat) >= 0;
}

int ra_credentials_load(char *username, size_t username_capacity,
            char *token, size_t token_capacity) {
  static const char *const paths[] = {
    RA_CREDENTIALS_UR0,
    RA_CREDENTIALS_UX0
  };
  SceKernelOpenPsId psid;
  ra_vault_credentials_t credentials;
  size_t i;

  if (username != NULL && username_capacity > 0)
    username[0] = '\0';
  if (token != NULL && token_capacity > 0)
    token[0] = '\0';
  if (sceKernelGetOpenPsId(&psid) < 0)
    return 0;

  for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    memset(&credentials, 0, sizeof(credentials));
    if (ra_vault_load(paths[i], (const uint8_t*)psid.id, &credentials) == 0) {
      copy_text(username, username_capacity, credentials.username);
      copy_text(token, token_capacity, credentials.token);
      ra_vault_clear(&credentials);
      return username == NULL || username_capacity == 0 ||
        username[0] != '\0';
    }
  }
  return 0;
}

int ra_credentials_save(const char *username, const char *token,
            char *error, size_t error_capacity) {
  SceKernelOpenPsId psid;
  ra_vault_credentials_t credentials;
  int result;

  if (error != NULL && error_capacity > 0)
    error[0] = '\0';
  if (username == NULL || username[0] == '\0' ||
      token == NULL || token[0] == '\0') {
    copy_text(error, error_capacity, "username and token are required");
    return -1;
  }

  ensure_dir("ur0:data");
  ensure_dir("ur0:data/AdrenalineRA");
  ensure_dir("ux0:data");
  ensure_dir(RA_SETTINGS_DIR);

  if (sceKernelGetOpenPsId(&psid) < 0) {
    copy_text(error, error_capacity, "could not read the console ID");
    return -1;
  }

  memset(&credentials, 0, sizeof(credentials));
  copy_text(credentials.username, sizeof(credentials.username), username);
  copy_text(credentials.token, sizeof(credentials.token), token);
  result = ra_vault_save(RA_CREDENTIALS_UR0, (const uint8_t*)psid.id,
            &credentials);
  ra_vault_clear(&credentials);
  if (result != 0) {
    copy_text(error, error_capacity, "could not save the encrypted vault");
    return -1;
  }
  return 0;
}

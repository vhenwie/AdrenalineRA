#ifndef RA_MENU_ACCOUNT_H
#define RA_MENU_ACCOUNT_H

#include <stdint.h>
#include "ra_settings.h"
#include "ra_vault.h"

#define RA_MENU_PASSWORD_MAX 256

typedef struct {
  char username[RA_VAULT_MAX_USERNAME];
  char account[RA_VAULT_MAX_USERNAME];
  char password[RA_MENU_PASSWORD_MAX];
  char status[192];
  int signed_in;
  int has_vault;
  unsigned score;
  int score_valid;
  uint8_t session_mode;
  uint8_t session_mode_applied;
} RaMenuAccountState;

RaMenuAccountState *ra_menu_account(void);
void ra_menu_account_update_status(void);
void ra_menu_build_account(void);
int ra_edit_username(void);
int ra_edit_password(void);
int ra_signin(void);
int ra_sign_out(void);
int ra_menu_signed_in(void);

#endif

#include "ra_menu_account.h"
#include "ra_menu_internal.h"
#include <stdio.h>
#include "ra_menu_entries.h"
#include "ra_menu_settings.h"
#include "ra_menu_nav.h"
#include "menu.h"
#include "ra_credentials.h"
#include "ra_vault.h"
#include "ra_runtime.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "ra_sound.h"
#include "ra_menu_worker.h"

static RaMenuAccountState g_state;
static char g_username_row[96];
static char g_password_row[96];

#define RA_SIGN_OUT_HOLD_MS 900

RaMenuAccountState *ra_menu_account(void) { return &g_state; }

void ra_menu_account_update_status(void) {
  if (g_state.signed_in && g_state.account[0] != '\0') {
    if (g_state.score_valid)
      snprintf(g_state.status, sizeof(g_state.status), "Signed in as %s (%u %s)",
          g_state.account, g_state.score, ra_menu_points_word_lower(g_state.score));
    else
      snprintf(g_state.status, sizeof(g_state.status), "Signed in as %s", g_state.account);
  } else {
    ra_menu_copy_text(g_state.status, sizeof(g_state.status), "Not signed in");
  }
  g_state.status[80] = '\0';
}


int ra_edit_username(void) {
  int result;

  if (ra_menu_busy_or_message())
    return 0;

  result = ra_menu_prompt("RetroAchievements username", g_state.username,
          sizeof(g_state.username), 0);
  if (result > 0) {
    ra_menu_set_message(WHITE, "Username updated.");
  } else if (result < 0) {
    ra_menu_show_prompt_error();
  }
  return 0;
}

int ra_edit_password(void) {
  int result;

  if (ra_menu_busy_or_message())
    return 0;

  result = ra_menu_prompt("RetroAchievements password", g_state.password,
          sizeof(g_state.password), 1);
  if (result > 0) {
    ra_menu_set_message(WHITE, "Password entered for this sign-in only.");
  } else if (result < 0) {
    ra_menu_show_prompt_error();
  }
  return 0;
}

int ra_signin(void) {
  if (ra_menu_busy_or_message())
    return 0;

  if (g_state.username[0] == '\0' || g_state.password[0] == '\0') {
    ra_menu_set_message(ORANGE, "Enter both a username and a password first.");
    return 0;
  }

  if (ra_worker_start_signin(g_state.username, g_state.password) != 0) {
    ra_menu_zero_memory(g_state.password, sizeof(g_state.password));
    ra_menu_set_message(RED, "Could not start the sign-in worker.");
    return 0;
  }

  ra_menu_zero_memory(g_state.password, sizeof(g_state.password));
  ra_menu_set_message(AZURE, "Signing in...");
  ra_menu_log("sign in requested user='%s'", g_state.username);
  return 0;
}

int ra_sign_out(void) {
  if (ra_menu_busy_or_message())
    return 0;

  ra_credentials_clear_files();
  ra_menu_zero_memory(g_state.password, sizeof(g_state.password));
  /* Tell the client too, not just our own state. */
  ra_runtime_logout();
  g_state.username[0] = '\0';
  g_state.account[0] = '\0';
  g_state.signed_in = 0;
  g_state.has_vault = 0;
  g_state.score = 0;
  g_state.score_valid = 0;
  ra_menu_settings()->ra_enabled = 0;
  ra_settings_normalize(ra_menu_settings(), 0);
  if (ra_menu_save_settings() == 0)
    ra_menu_set_message(GREEN, "Signed out. Achievements disabled.");
  ra_menu_log("signed out; preferences preserved");
  ra_menu_account_update_status();
  return 0;
}


void ra_menu_build_account(void) {
  if (g_state.signed_in) {
    snprintf(g_username_row, sizeof(g_username_row), "Username: %s",
        g_state.account);
    ra_menu_add_text(g_username_row, LITEGRAY);
  } else {
    snprintf(g_username_row, sizeof(g_username_row), "Username: %s",
        g_state.username[0] != '\0' ? g_state.username : "(not set)");
    ra_menu_add_callback(g_username_row, ra_edit_username, WHITE);
    if (g_state.password[0] != '\0')
      ra_menu_copy_text(g_password_row, sizeof(g_password_row),
          "Password: ********");
    else
      ra_menu_copy_text(g_password_row, sizeof(g_password_row), "Password");
    ra_menu_add_callback(g_password_row, ra_edit_password, WHITE);
  }

  if (g_state.signed_in)
    ra_menu_add_hold_callback("Sign Out", ra_sign_out, YELLOW, RA_SIGN_OUT_HOLD_MS);
  else
    ra_menu_add_callback("Sign In", ra_signin, GREEN);

  ra_menu_add_text("--- Settings ---", LITEGRAY);
  ra_menu_add_option("Enable Achievements", &ra_menu_settings()->ra_enabled, ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  if (ra_menu_settings()->ra_enabled) {
    ra_menu_add_option("Enable Hardcore Mode", &ra_menu_settings()->hardcore,
        ra_menu_yes_no_options,
        RA_MENU_YES_NO_COUNT);
    /* One row for the two engine flags. Spectator and Encore are independent
         * boolean settings in rcheevos (PCSX2 and DuckStation expose both), but
         * Spectator + Encore is inert: spectator skips `start_session`, so there is
         * no earned state for Encore to reset and no submission for it to replay.
         * Presenting the three meaningful states as one value keeps the inert
         * combination unreachable without coupling the flags themselves. */
    g_state.session_mode = (uint8_t)ra_session_mode_value(ra_menu_settings()->spectator,
                            ra_menu_settings()->encore);
    g_state.session_mode_applied = g_state.session_mode;
    ra_menu_add_option("Session Mode", &g_state.session_mode, ra_menu_session_mode_options,
        RA_MENU_SESSION_MODE_COUNT);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}



int ra_menu_signed_in(void) { return ra_menu_account()->signed_in; }

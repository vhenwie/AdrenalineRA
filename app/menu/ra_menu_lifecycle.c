#include "ra_menu_lifecycle.h"
#include "ra_menu_internal.h"
#include "ra_menu_account.h"
#include "ra_menu_entries_cache.h"
#include "ra_menu_game.h"
#include "ra_menu_achievements.h"
#include "ra_menu_settings.h"
#include "ra_menu_nav.h"
#include "ra_menu_worker.h"
#include "ra_menu_entries.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

#include "ra_menu.h"
#include "ra_credentials.h"
#include "ra_ime.h"
#include "ra_settings.h"
#include "ra_sound.h"
#include "ra_notify.h"
#include "ra_log.h"
#include "ra_logqueue.h"

static int g_loaded;
static char g_mode_titleid[16];
static int g_last_menu_open;
static unsigned long long g_consumed_us;

void ra_menu_ensure_loaded(void) {
  char token[RA_VAULT_MAX_TOKEN];

  if (g_loaded)
    return;

  ra_menu_achievements_defaults();
  ra_settings_load(ra_menu_settings());
  memset(token, 0, sizeof(token));
  ra_menu_account()->signed_in = ra_credentials_load(ra_menu_account()->username, sizeof(ra_menu_account()->username), token,
                  sizeof(token));
  ra_menu_zero_memory(token, sizeof(token));
  ra_menu_copy_text(ra_menu_account()->account, sizeof(ra_menu_account()->account),
      ra_menu_account()->signed_in ? ra_menu_account()->username : "");
  ra_menu_account()->has_vault = ra_credentials_exists(); {
    SceIoStat policy_stat;
    int have_policy =
      sceIoGetstat(RA_SETTINGS_POLICY_PATH, &policy_stat) >= 0;

    ra_settings_normalize(ra_menu_settings(), ra_menu_account()->signed_in);
    if (have_policy &&
      ra_settings_auth_state() != RA_SETTINGS_AUTH_INVALID) {
      /* Normalize old files too, but never discard hidden preferences. */
      ra_settings_save(ra_menu_settings());
    }
  }

  ra_notify_set_position(ra_menu_settings()->appearance_anchor,
            ra_menu_settings()->appearance_padding_auto,
            ra_menu_settings()->appearance_padding_h,
            ra_menu_settings()->appearance_padding_v);
  ra_sound_set_options(ra_menu_settings()->sound_effects,
            ra_menu_settings()->sound_message,
            ra_menu_settings()->sound_unlock,
            ra_menu_settings()->sound_lboard_submit);

  ra_menu_account_update_status();
  ra_ime_preload();
  g_loaded = 1;
}


static void apply_worker_result(const RaJobSnapshot *snapshot) {
  if (snapshot->state != RA_JOB_DONE_OK &&
    snapshot->state != RA_JOB_DONE_ERROR)
    return;

  /* A finished job can change sign-in state, the subset list or the verify
     * page, so the cached rows must be rebuilt on the next draw. */
  ra_menu_invalidate_entries();

  if (snapshot->state == RA_JOB_DONE_ERROR) {
    ra_menu_set_message(RED, "%s", snapshot->message);
    ra_menu_log("job kind=%d failed: %s", (int)snapshot->kind,
        snapshot->message);
    return;
  }

  switch (snapshot->kind) {
  case RA_JOB_SIGN_IN:
    if (snapshot->username[0] != '\0')
      ra_menu_copy_text(ra_menu_account()->account, sizeof(ra_menu_account()->account), snapshot->username);
    ra_menu_copy_text(ra_menu_account()->username, sizeof(ra_menu_account()->username), ra_menu_account()->account);
    ra_menu_account()->signed_in = ra_menu_account()->account[0] != '\0';
    ra_menu_account()->has_vault = ra_menu_account()->signed_in;
    ra_menu_settings()->ra_enabled = 1;
    ra_menu_settings()->hardcore = 1;
    ra_settings_normalize(ra_menu_settings(), ra_menu_account()->signed_in);
    if (ra_settings_save(ra_menu_settings()) != 0)
      ra_menu_set_message(RED, "Signed in, but RA_POLICY.TXT could not be written.");
    else
      ra_menu_set_message(GREEN, "%s", snapshot->message);
    ra_menu_account()->score_valid = 0;
    ra_menu_account()->score = 0;
    ra_menu_log("sign in ok user='%s'", ra_menu_account()->account);
    break;

  case RA_JOB_VERIFY_NETWORK:
    ra_menu_set_message(GREEN, "%s", snapshot->message);
    ra_menu_log("network verify ok");
    break;

  case RA_JOB_VERIFY_ACCOUNT:
    if (snapshot->username[0] != '\0') {
      ra_menu_copy_text(ra_menu_account()->account, sizeof(ra_menu_account()->account), snapshot->username);
      ra_menu_copy_text(ra_menu_account()->username, sizeof(ra_menu_account()->username), ra_menu_account()->account);
    }
    ra_menu_account()->score = snapshot->score;
    ra_menu_account()->score_valid = snapshot->score_valid;
    ra_menu_set_message(GREEN, "%s", snapshot->message);
    ra_menu_log("account verify ok user='%s' score=%u", ra_menu_account()->account, ra_menu_account()->score);
    break;

  default:
    ra_menu_set_message(WHITE, "%s", snapshot->message);
    break;
  }

  ra_menu_account_update_status();
}

void ra_menu_tick(const SceAdrenaline *adrenaline) {
  static int last_hardcore = -1;
  RaRuntimeInfo info;
  RaJobSnapshot snapshot;
  int xmb;

  ra_menu_ensure_loaded();
  memset(&info, 0, sizeof(info));
  ra_runtime_copy_info(&info);
  if (info.hardcore != last_hardcore) {
    last_hardcore = info.hardcore;
    ra_menu_invalidate_entries();
    if (!info.hardcore &&
        (ra_menu_page() == RA_PAGE_LEADERBOARDS ||
         ra_menu_page() == RA_PAGE_LEADERBOARD_ENTRIES)) {
      ra_menu_set_page(RA_PAGE_GAME);
      ra_menu_invalidate_entries();
    }
    ra_menu_reload_settings();
  }

  xmb = adrenaline != NULL &&
    strcmp(adrenaline->title, "XMB\xE2\x84\xA2") == 0;
  if (xmb != ra_menu_is_xmb()) {
    ra_menu_set_xmb(xmb);
    ra_menu_zero_memory(ra_menu_account()->password, sizeof(ra_menu_account()->password));
    ra_menu_set_message(0, NULL);
  }

  if (adrenaline != NULL) {
    ra_menu_set_game_name(adrenaline->title);
    ra_menu_set_titleid(adrenaline->titleid);
    if (strcmp(g_mode_titleid, ra_menu_titleid()) != 0) {
      ra_menu_copy_text(g_mode_titleid, sizeof(g_mode_titleid), ra_menu_titleid());
      ra_menu_reload_settings();
    }
    ra_menu_sync_achievement_ui();
  }

  /* VSH can toggle hardcore/spectator/encore while a game is running. The
     * menu process may not share the changed RaSettings object, so refresh
     * from RA_POLICY.TXT when the menu opens. */
  if (menu_open != g_last_menu_open) {
    g_last_menu_open = menu_open;
    if (menu_open)
      ra_menu_reload_settings();
  }

  ra_worker_snapshot(&snapshot);
  if ((snapshot.state == RA_JOB_DONE_OK ||
    snapshot.state == RA_JOB_DONE_ERROR) &&
    snapshot.finished_us != g_consumed_us) {
    g_consumed_us = snapshot.finished_us;
    apply_worker_result(&snapshot);
    ra_worker_consume();
  }

  ra_menu_account_update_status();
}

int ra_menu_busy_or_message(void) {
  if (ra_worker_busy()) {
    ra_menu_set_message(ORANGE, "A network operation is already running.");
    return 1;
  }
  return 0;
}

static int open_account(void) { return ra_menu_open_page(RA_PAGE_ACCOUNT); }
static int open_notifications(void) { return ra_menu_open_page(RA_PAGE_NOTIFICATIONS); }
static int open_options(void) { return ra_menu_open_page(RA_PAGE_OPTIONS); }

void ra_menu_build_root(void) {
  ra_menu_add_callback("Account >", open_account, WHITE);
  if (ra_menu_settings()->ra_enabled) {
    ra_menu_add_callback("Notifications >", open_notifications, WHITE);
    ra_menu_add_callback("Options >", open_options, WHITE);
  }
}

void ra_menu_log(const char *format, ...) {
  char buffer[256];
  char line[300];
  SceDateTime now;
  SceUID fd;
  va_list args;
  int prefix;
  int length;

  if (!ra_log_enabled())
    return;

  prefix = 0;
  if (sceRtcGetCurrentClockLocalTime(&now) == 0) {
    prefix = snprintf(buffer, sizeof(buffer),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            (unsigned)now.year, (unsigned)now.month,
            (unsigned)now.day, (unsigned)now.hour,
            (unsigned)now.minute, (unsigned)now.second,
            (unsigned)(now.microsecond / 1000));
  } else {
    prefix = snprintf(buffer, sizeof(buffer), "[t=%llu] ",
            (unsigned long long)sceKernelGetSystemTimeWide());
  }
  if (prefix < 0)
    prefix = 0;

  va_start(args, format);
  vsnprintf(buffer + prefix, sizeof(buffer) - (size_t)prefix, format, args);
  va_end(args);

  length = snprintf(line, sizeof(line), "%s\n", buffer);
  ra_logq_line(line, length);
  fd = sceIoOpen(RA_LOG_DIR "/RA_SETUP.log",
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, line, (SceSize)length);
    sceIoClose(fd);
  }
}




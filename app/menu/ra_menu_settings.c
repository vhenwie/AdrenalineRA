#include "ra_menu_settings.h"

static RaSettings g_settings;

RaSettings *ra_menu_settings(void) { return &g_settings; }

#include <stdio.h>

#include "ra_runtime.h"
#include "ra_menu_entries.h"
#include "ra_menu_internal.h"
#include "ra_menu.h"
#include "ra_menu_nav.h"

#include "menu.h"
#include "ra_menu_worker.h"
#include "ra_menu_account.h"
#include "ra_menu_entries_cache.h"
#include "ra_notify.h"
#include "ra_log.h"
#include "ra_runtime.h"
#include "ra_sound.h"
#include "ra_gate.h"

static int open_banner_options(void);
static int open_active_badge_options(void);
static int open_sound_effects(void);
static int open_developer_features(void);


static int leaderboards_active(void) {
  return ra_menu_hardcore_active();
}

void ra_menu_build_notifications(void) {
  ra_menu_add_option("Enable Notifications", &ra_menu_settings()->notifications,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  /* the master for the live challenge badge tiles,
     * directly under the notification master. It no longer gates badge art on
     * banners (that is always on) - only the Active Challenge badges. */
  ra_menu_add_option("Enable Active Badges", &ra_menu_settings()->badges, ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  if (leaderboards_active()) {
    ra_menu_add_option("Enable Leaderboard Tracking", &ra_menu_settings()->leaderboards,
        ra_menu_leaderboard_options,
        RA_MENU_LEADERBOARD_COUNT);
  }
  if (ra_menu_settings()->notifications) {
    ra_menu_add_text("--- Notifications ---", LITEGRAY);
    ra_menu_add_option("Startup Summary", &ra_menu_settings()->visibility_summary,
        ra_menu_summary_options,
        RA_MENU_SUMMARY_COUNT);
    ra_menu_add_option("Unlock Notifications", &ra_menu_settings()->visibility_unlock,
        ra_menu_yes_no_options,
        RA_MENU_YES_NO_COUNT);
    ra_menu_add_option("Mastery Notifications", &ra_menu_settings()->visibility_mastery,
        ra_menu_yes_no_options,
        RA_MENU_YES_NO_COUNT);
    ra_menu_add_option("Progress Indicator", &ra_menu_settings()->visibility_progress,
        ra_menu_yes_no_options,
        RA_MENU_YES_NO_COUNT);
    if (leaderboards_active() && ra_menu_settings()->leaderboards != 0) {
      ra_menu_add_option("Leaderboard Start Messages",
          &ra_menu_settings()->visibility_lboard_start, ra_menu_yes_no_options,
          RA_MENU_YES_NO_COUNT);
      ra_menu_add_option("Leaderboard Submit Messages",
          &ra_menu_settings()->visibility_lboard_submit, ra_menu_yes_no_options,
          RA_MENU_YES_NO_COUNT);
      ra_menu_add_option("Leaderboard Failed Messages",
          &ra_menu_settings()->visibility_lboard_cancel, ra_menu_yes_no_options,
          RA_MENU_YES_NO_COUNT);
      ra_menu_add_option("Leaderboard Trackers",
          &ra_menu_settings()->visibility_lboard_trackers, ra_menu_yes_no_options,
          RA_MENU_YES_NO_COUNT);
    }
    ra_menu_add_option("Login Messages", &ra_menu_settings()->visibility_login,
        ra_menu_yes_no_options,
        RA_MENU_YES_NO_COUNT);
    ra_menu_add_option("Verbose Messages", &ra_menu_settings()->visibility_verbose,
        ra_menu_yes_no_options,
        RA_MENU_YES_NO_COUNT);
    ra_menu_add_callback("Banner Options >", open_banner_options, WHITE);
    /* Hidden while the badges it shapes are off. */
    if (ra_menu_settings()->badges)
      ra_menu_add_callback("Active Badge Options >",
            open_active_badge_options, WHITE);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

/* Banner geometry only. The live-indicator rows moved to Active Badge Options
 *; this page is now purely about the notification
 * cards. */
void ra_menu_build_banner_options(void) {
  ra_menu_add_option("Position", &ra_menu_settings()->appearance_anchor, ra_menu_anchor_options,
      RA_MENU_ANCHOR_COUNT);
  ra_menu_add_option("Aligned Padding", &ra_menu_settings()->appearance_padding_auto,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  if (!ra_menu_settings()->appearance_padding_auto) {
    ra_menu_add_option("Manual Horizontal Padding",
        &ra_menu_settings()->appearance_padding_h, ra_menu_padding_options,
        RA_MENU_PADDING_COUNT);
    ra_menu_add_option("Manual Vertical Padding",
        &ra_menu_settings()->appearance_padding_v, ra_menu_padding_options,
        RA_MENU_PADDING_COUNT);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

/* The live-indicator options, renamed and on their own page. The caller hides the row while Enable Active Badges is off. The
 * names are the player: Position / Size / Progress Values. */
void ra_menu_build_active_badge_options(void) {
  MenuEntry *entry;

  entry = ra_menu_add_option("Position", &ra_menu_settings()->indicator_anchor, ra_menu_anchor_options,
          RA_MENU_ANCHOR_COUNT);
  if (entry != NULL)
    entry->tooltip = "Anchor for the active challenge badge tiles.";
  entry = ra_menu_add_option("Size", &ra_menu_settings()->indicator_size,
          ra_menu_indicator_size_options,
          RA_MENU_INDICATOR_SIZE_COUNT);
  if (entry != NULL)
    entry->tooltip = "Size of the active challenge badge tiles.";
  /* The tile is the badge; this only decides whether the "12/30" caption is
     * drawn next to it, so it can be turned off without losing the art. */
  entry = ra_menu_add_option("Progress Values", &ra_menu_settings()->indicator_values,
          ra_menu_yes_no_options,
          RA_MENU_YES_NO_COUNT);
  if (entry != NULL)
    entry->tooltip = "Show the progress value next to an active badge.";
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

void ra_menu_build_options(void) {
  /* No section divider here any more: the page is
     * four rows and a heading was noise. Rich Presence has no row: it is always on. */
  ra_menu_add_option("Enable Next Achievements", &ra_menu_settings()->next_achievement,
      ra_menu_next_achievement_options,
      RA_MENU_NEXT_ACHIEVEMENT_COUNT);
  /* `Achievement Badges` moved to Notifications as `Enable Active Badges`
     *: banner art is always on, so the only thing
     * left to switch is the active challenge badges. */
  ra_menu_add_option("Automatic Screenshots", &ra_menu_settings()->auto_screenshot,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  ra_menu_add_callback("Sound Effects >", open_sound_effects, WHITE);
  ra_menu_add_callback("Developer Features >", open_developer_features, WHITE);
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

void ra_menu_build_sound_effects(void) {
  ra_menu_add_option("Enable Sound Effects", &ra_menu_settings()->sound_effects,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  if (ra_menu_settings()->sound_effects) {
    ra_menu_add_option("Notification Sound", &ra_menu_settings()->sound_message,
        ra_menu_sound_options,
        RA_MENU_SOUND_COUNT);
    ra_menu_add_option("Achievement Unlock Sound", &ra_menu_settings()->sound_unlock,
        ra_menu_sound_options,
        RA_MENU_SOUND_COUNT);
    if (leaderboards_active())
      ra_menu_add_option("Leaderboard Submit Sound",
          &ra_menu_settings()->sound_lboard_submit, ra_menu_sound_options,
          RA_MENU_SOUND_COUNT);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

void ra_menu_build_developer_features(void) {
  ra_menu_add_option("Test Unofficial Achievements", &ra_menu_settings()->test_unofficial,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  /* "Enable Save State in Hardcore Mode" was removed in : the RA rules
     * require savestates to be disabled in hardcore, and the option was a port of
     * PPSSPP's developer escape hatch rather than a supported feature. */
  ra_menu_add_option("Enable Logging", &ra_menu_settings()->logging, ra_menu_no_yes_options,
      RA_MENU_NO_YES_COUNT);
  /* Background art prefetch : on by default. It warms only near-unlock
     * art in RAM and never writes ux0 while the game runs. Turning it off gives
     * reference-style fetch-at-unlock behaviour. */ {
    MenuEntry *entry = ra_menu_add_option("Enable Art Prefetch", &ra_menu_settings()->badge_prefetch,
                  ra_menu_no_yes_options,
                  RA_MENU_NO_YES_COUNT);
    if (entry != NULL)
      entry->tooltip = "Warm badge art for near-unlock achievements while you play. "
              "Off fetches art when the achievement unlocks.";
  }
  /* Offline queue has no row: it is always on and replayed at session start. */
  /* There is no clock row any more : the runtime scopes the boost to the windows
     * where the emulated world is paused and never touches gameplay, so there is nothing
     * for the player to decide and nothing Hardcore has to refuse. */
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}



static int open_banner_options(void) {
  return ra_menu_open_page(RA_PAGE_BANNER_OPTIONS);
}

static int open_active_badge_options(void) {
  return ra_menu_open_page(RA_PAGE_ACTIVE_BADGE_OPTIONS);
}

static int open_sound_effects(void) {
  return ra_menu_open_page(RA_PAGE_SOUND_EFFECTS);
}

static int open_developer_features(void) {
  return ra_menu_open_page(RA_PAGE_DEVELOPER_FEATURES);
}

void ra_menu_reload_settings(void) {
  ra_settings_load(ra_menu_settings());
  /* Everything shown in the menu derives from settings, so drop the cached rows. */
  ra_menu_invalidate_entries();
  ra_settings_normalize(ra_menu_settings(), ra_menu_account()->signed_in);
  ra_notify_set_position(ra_menu_settings()->appearance_anchor,
            ra_menu_settings()->appearance_padding_auto,
            ra_menu_settings()->appearance_padding_h,
            ra_menu_settings()->appearance_padding_v);
  ra_sound_set_options(ra_menu_settings()->sound_effects,
            ra_menu_settings()->sound_message,
            ra_menu_settings()->sound_unlock,
            ra_menu_settings()->sound_lboard_submit);
}

int ra_menu_save_settings(void) {
  ra_settings_normalize(ra_menu_settings(), ra_menu_account()->signed_in);
  if (ra_settings_save(ra_menu_settings()) != 0) {
    ra_menu_set_message(RED, "Could not write RA_POLICY.TXT.");
    return -1;
  }
  ra_publish_gate();
  ra_notify_set_position(ra_menu_settings()->appearance_anchor,
            ra_menu_settings()->appearance_padding_auto,
            ra_menu_settings()->appearance_padding_h,
            ra_menu_settings()->appearance_padding_v);
  ra_sound_set_options(ra_menu_settings()->sound_effects,
            ra_menu_settings()->sound_message,
            ra_menu_settings()->sound_unlock,
            ra_menu_settings()->sound_lboard_submit);
  ra_menu_account_update_status();
  return 0;
}

int ra_menu_hardcore_active(void) {
  ra_menu_ensure_loaded();
  return ra_settings_hardcore_effective(&g_settings, ra_menu_account()->signed_in);
}

int ra_menu_states_mode(void) {
  ra_menu_ensure_loaded();
  /* Hardcore: saving and deleting states is allowed for debugging; loading is
   * refused by the request handler. */
  return ra_settings_hardcore_effective(&g_settings, ra_menu_account()->signed_in) ? 2 : 0;
}

int ra_menu_is_busy(void) {
  return ra_worker_busy();
}

void ra_menu_options_changed(void) {
  /* Rows are built from the settings, so a change here invalidates the cache. */
  ra_menu_invalidate_entries();

  /* Session Mode is presentation over two engine flags: map the row back to
     * `spectator`/`encore` before persisting. Only act when the row actually
     * changed, so an unrelated option edit cannot clobber a reloaded value. */
  if (ra_menu_account()->session_mode > 2)
    ra_menu_account()->session_mode = 0;
  if (ra_menu_account()->session_mode != ra_menu_account()->session_mode_applied) {
    int spectator = 0;
    int encore = 0;

    ra_session_mode_flags(ra_menu_account()->session_mode, &spectator, &encore);
    ra_menu_settings()->spectator = (uint8_t)spectator;
    ra_menu_settings()->encore = (uint8_t)encore;
    ra_menu_account()->session_mode_applied = ra_menu_account()->session_mode;
  }

  /* Persist first: the runtime reloads settings from disk when it sees the
     * dirty flag. Signalling before the write can make it reload the old
     * values and clear rich presence permanently. */
  if (ra_menu_save_settings() == 0) {
    ra_log_set_enabled(ra_menu_settings()->logging);
    ra_menu_log("settings changed ra=%d hardcore=%d notifications=%d",
        ra_menu_settings()->ra_enabled, ra_menu_settings()->hardcore,
        ra_menu_settings()->notifications);
    ra_runtime_settings_changed();
  }
}

char *ra_menu_yes_no_options[] = { "No", "Yes" };
char *ra_menu_session_mode_options[] = { "Standard", "Spectator", "Encore" };
char *ra_menu_no_yes_options[] = { "No", "Yes" };
char *ra_menu_leaderboard_options[] = { "Off", "Trackers", "Notifications", "All" };
char *ra_menu_summary_options[] = { "Off", "All games", "Games with achievements" };
/* Enable Next Achievements: No / 1 / 3. */
char *ra_menu_next_achievement_options[] = { "No", "1", "3" };
char *ra_menu_sound_options[] = { "No", "Yes", "Override" };
char *ra_menu_anchor_options[] = {
  "Top left", "Top center", "Top right",
  "Bottom left", "Bottom center", "Bottom right"
};
char *ra_menu_padding_options[] = { "0", "2", "4", "8", "16" };
/* Live indicator tile size: 36 / 48 / 60 px. */
char *ra_menu_indicator_size_options[] = { "Small", "Standard", "Large" };

void ra_menu_format_mode(char *destination, size_t capacity,
                         const RaRuntimeInfo *info, const RaSettings *settings,
                         int signed_in) {
  const char *mode = "Standard";
  int hardcore;
  int spectator;
  int encore;
  int value;

  if (destination == NULL || capacity == 0)
    return;

  /* One line, one shape: `Session Mode: Hardcore (Standard)`, `Session Mode:
     * Casual (Spectator)`, `Session Mode: Hardcore (Encore)`. The parenthesis
     * carries which of the three session modes is selected (they are mutually
     * exclusive; an old settings file with both flags set resolves the way the
     * `Session Mode` row does), so the line never reads as a slash-separated
     * stack that could mean "both are on". */
  hardcore = info->ready ? info->hardcore
            : ra_settings_hardcore_effective(&(*settings),
                              signed_in);
  spectator = info->ready ? info->spectator != 0 : (*settings).spectator != 0;
  encore = info->ready ? info->encore != 0 : (*settings).encore != 0;

  value = ra_session_mode_value(spectator, encore);
  if (value == 1)
    mode = "Spectator";
  else if (value == 2)
    mode = "Encore";

  snprintf(destination, capacity, "Session Mode: %s (%s)%s",
      hardcore ? "Hardcore" : "Casual", mode,
      info->unsupported_platform ? " (UP)" : "");
}

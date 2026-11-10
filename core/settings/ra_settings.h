#ifndef RA_SETTINGS_H
#define RA_SETTINGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_SETTINGS_DIR "ux0:data/AdrenalineRA"
#define RA_SETTINGS_POLICY_PATH RA_SETTINGS_DIR "/RA_POLICY.TXT"
#define RA_SETTINGS_BIN_UR0 "ur0:data/AdrenalineRA/ra_settings.bin"
#define RA_SETTINGS_BIN_UX0 "ux0:data/AdrenalineRA/ra_settings.bin"
#define POLICY_VERSION 8

typedef struct {
  uint8_t ra_enabled;
  uint8_t hardcore;
  uint8_t notifications;
  /* How many `Next:` rows the achievement page shows, as an option index:
     * 0 = off, 1 = one row, 2 = three rows . rcheevos hands the
     * suggestions out one at a time, so the row count is our choice. */
  uint8_t next_achievement;
  uint8_t leaderboards;

  uint8_t badges;
  /* Background badge art cache : fetch art for the set's achievements while
     * the player plays, so an unlock card finds a texture already built. On by default;
     * the switch exists so the player can turn the background I/O off without a new
     * build if it ever disturbs a title. */
  uint8_t badge_prefetch;
  uint8_t test_unofficial;
  uint8_t auto_screenshot;
  uint8_t spectator;
  uint8_t encore;

  uint8_t sound_effects;
  uint8_t sound_message;
  uint8_t sound_unlock;
  uint8_t sound_lboard_submit;

  uint8_t appearance_anchor;
  uint8_t appearance_padding_auto;
  uint8_t appearance_padding_h;
  uint8_t appearance_padding_v;
  /* Anchor for the live indicator rows (challenge/progress/tracker). */
  uint8_t indicator_anchor;
  /* Live indicator tile size: 0 small (36 px), 1 standard (48 px), 2 large. */
  uint8_t indicator_size;
  /* The in-game CPU clock is no longer a setting: it is scoped by the runtime to the
     * windows where the emulated world is paused (load, menu, exit) and never touches
     * gameplay T1). The `cpu_boost` field was removed in ; an old
     * `cpu_boost=` line in an existing settings file is ignored by the parser. */
  /* Numeric caption beside a badge tile ("12/30"): 0 badge only, 1 show it.
     * The tile itself is governed by the visibility rows above - this only
     * decides the text, so "badges only" never means "no badge". */
  uint8_t indicator_values;

  uint8_t visibility_summary;
  uint8_t visibility_unlock;
  uint8_t visibility_mastery;
  uint8_t visibility_challenge;
  uint8_t visibility_progress;
  uint8_t visibility_lboard_start;
  uint8_t visibility_lboard_submit;
  uint8_t visibility_lboard_cancel;
  uint8_t visibility_lboard_trackers;
  uint8_t visibility_login;
  uint8_t visibility_verbose;

  /* Master logging switch : No by default. Gates every log writer,
   * including the stable setup/preload files and the queued session log. */
  uint8_t logging;

} RaSettings;

typedef enum {
  RA_SETTINGS_AUTH_UNKNOWN = 0,
  RA_SETTINGS_AUTH_DEFAULTS,
  RA_SETTINGS_AUTH_LEGACY,
  RA_SETTINGS_AUTH_VALID,
  RA_SETTINGS_AUTH_INVALID
} RaSettingsAuthState;

void ra_settings_defaults(RaSettings *settings);
void ra_settings_load(RaSettings *settings);

/* Reads only the master logging preference from the policy text, without
 * allocating or verifying the signature. Used at module_start before the heap
 * path is ready; the authoritative load still runs later. */
int ra_settings_logging_pref(void);
int ra_settings_save(const RaSettings *settings);

/* Last load/save authentication result, for the dev Verify page. */
RaSettingsAuthState ra_settings_auth_state(void);

/* Copies the canonical Vita-side signed policy mirror to a PSP-visible path.
 * systemctrl verifies the signature before using it for hardcore plugin gating;
 * the authenticated binary record remains the authority. */
int ra_settings_copy_to(const char *destination);

/* Enforces cross-setting rules. Preferences are deliberately preserved when a
 * feature is disabled or hidden. */
void ra_settings_normalize(RaSettings *settings, int signed_in);

/* Hardcore is only effective when RA, the setting and a signed-in vault agree. */
int ra_settings_hardcore_effective(const RaSettings *settings, int signed_in);

/* Session Mode maps to two engine flags: 0 Standard, 1 Spectator, 2 Encore. */
int ra_session_mode_value(int spectator, int encore);
void ra_session_mode_flags(int mode, int *spectator, int *encore);

#ifdef __cplusplus
}
#endif

#endif /* RA_SETTINGS_H */

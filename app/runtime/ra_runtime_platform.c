#include "ra_runtime_platform.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_core.h"

#include "ra_notify.h"
#include "ra_server.h"
#include "ra_settings.h"
#include "ra_credentials.h"
#include "ra_log.h"
#include "ra_sound.h"
#include "ra_badge.h"
#include "ra_notify.h"
#include "ra_runtime.h"
#include "ra_client.h"
#include "ra_session.h"
#include "rc_client.h"
#include "ra_gate.h"
#include "main.h"

RaSettings *ra_runtime_settings(void);
int ra_runtime_info_ready(void);
void ra_runtime_mark_dirty(void);
int ra_runtime_game_unsupported(void);

void ra_runtime_apply_submission_policy(void) {
  static int logged_state = -1;
  int blocked = (*ra_runtime_settings()).encore != 0;

  /* Defense in depth: rcheevos only gates leaderboard submissions on
     * spectator mode, so Encore would otherwise post a replayed run's score. */
  ra_server_set_leaderboard_submits_blocked(blocked);
  if (blocked != logged_state) {
    ra_runtime_log("runtime: leaderboard submissions %s (encore=%d)\n",
          blocked ? "blocked" : "enabled", blocked);
    logged_state = blocked;
  }
}
void ra_runtime_push_up_banner(void) {
  if ((*ra_runtime_settings()).notifications)
    ra_notify_push(RA_NOTIFY_ERROR, 0, "Unsupported platform (UP)",
        "UP: Hardcore has been toggled/downgraded to Casual");
}


static volatile int g_settings_dirty;

int ra_runtime_settings_dirty(void) { return g_settings_dirty; }
void ra_runtime_mark_settings_dirty(void) { g_settings_dirty = 1; }
void ra_runtime_clear_settings_dirty(void) { g_settings_dirty = 0; }

void ra_runtime_apply_client_settings(void);

void ra_runtime_load_settings(void) {
  ra_settings_load(ra_runtime_settings());
  ra_settings_normalize(ra_runtime_settings(), ra_credentials_exists());
  ra_log_set_enabled(ra_runtime_settings()->logging);
  ra_runtime_apply_submission_policy();
  /* RP and the offline queue are mandatory (compliance) and no longer settings:
   * force them here so a stale or edited policy file cannot disable either. */
  ra_server_set_rich_presence_enabled(1);
  ra_sound_set_options(ra_runtime_settings()->sound_effects,
            ra_runtime_settings()->sound_message,
            ra_runtime_settings()->sound_unlock,
            ra_runtime_settings()->sound_lboard_submit);
  ra_notify_set_position(ra_runtime_settings()->appearance_anchor,
            ra_runtime_settings()->appearance_padding_auto,
            ra_runtime_settings()->appearance_padding_h,
            ra_runtime_settings()->appearance_padding_v);
  ra_notify_set_indicator_position(ra_runtime_settings()->indicator_anchor);
  ra_notify_set_indicator_size(ra_runtime_settings()->indicator_size);
  /* A live indicator always owns the frame now: the `Live Indicator Frame` setting is
     * gone , so nothing to apply here. */
  /* Badge art on banners is always on: the worker
     * must fetch art even when the active-challenge tiles are switched off. */
  ra_badge_set_enabled(1);
  ra_runtime_apply_client_settings();
  ra_runtime_clear_settings_dirty();
}

static void client_apply_spectator(rc_client_t *rc, ra_client_t *client,
                                   int ready) {
  ra_client_set_spectator_mode(client, ra_runtime_settings()->spectator != 0);
  ra_runtime_mark_dirty();
  if (ready &&
      rc_client_get_spectator_mode_enabled(rc) !=
        (ra_runtime_settings()->spectator != 0)) {
    ra_runtime_log("runtime: spectator change deferred to next game (current=%d desired=%d)\n",
        rc_client_get_spectator_mode_enabled(rc) != 0,
        ra_runtime_settings()->spectator != 0);
  }
}

static void client_apply_encore(rc_client_t *rc, int ready) {
  if (ready && rc_client_get_encore_mode_enabled(rc) !=
      (ra_runtime_settings()->encore != 0)) {
    ra_runtime_log("runtime: encore change applies next game (current=%d desired=%d)\n",
        rc_client_get_encore_mode_enabled(rc) != 0,
        ra_runtime_settings()->encore != 0);
  }
  if (!ready)
    rc_client_set_encore_mode_enabled(rc, ra_runtime_settings()->encore != 0);
}

static void client_apply_unofficial(rc_client_t *rc, int ready) {
  int desired = ra_runtime_settings()->test_unofficial != 0;

  if (!ready)
    rc_client_set_unofficial_enabled(rc, desired);
  else if (rc_client_get_unofficial_enabled(rc) != desired)
    ra_runtime_log("runtime: unofficial change applies next game (current=%d desired=%d)\n",
        rc_client_get_unofficial_enabled(rc) != 0, desired);
}

static void client_apply_hardcore(rc_client_t *rc, int ready) {
  int desired = ra_settings_hardcore_effective(ra_runtime_settings(), 1);
  int current = rc_client_get_hardcore_enabled(rc) != 0;

  if (ra_runtime_game_unsupported())
    desired = 0;
  if (desired == current)
    return;
  if (desired == 0) {
    rc_client_set_hardcore_enabled(rc, 0);
    ra_runtime_log("runtime: hardcore disabled for this session\n");
    ra_runtime_mark_dirty();
  } else if (!ready) {
    rc_client_set_hardcore_enabled(rc, 1);
    ra_runtime_mark_dirty();
  } else {
    ra_runtime_log("runtime: hardcore enable ignored while a session is live "
                   "(settings change is XMB-only)\n");
  }
}

void ra_runtime_apply_client_settings(void) {
  ra_session_t *session;
  ra_client_t *client;
  rc_client_t *rc;
  int ready;

  ra_runtime_apply_submission_policy();
  client = ra_runtime_client_acquire(&session);
  if (client == NULL)
    return;
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_client_release(session);
    return;
  }
  ra_runtime_lock();
  ready = ra_runtime_info_ready();
  ra_runtime_unlock();
  client_apply_spectator(rc, client, ready);
  client_apply_encore(rc, ready);
  client_apply_unofficial(rc, ready);
  client_apply_hardcore(rc, ready);
  ra_runtime_client_release(session);
}

void ra_runtime_set_unsupported(void);

int ra_runtime_downgrade_platform(void) {
  int was_hardcore = ra_settings_hardcore_effective(&(*ra_runtime_settings()), 1);
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc = client != NULL ? ra_client_get_rc_client(client) : NULL;

  (*ra_runtime_settings()).hardcore = 0;
  ra_settings_save(&(*ra_runtime_settings()));
  ra_publish_gate();
  ra_runtime_apply_submission_policy();
  if (rc != NULL)
    rc_client_set_hardcore_enabled(rc, 0);
  if (client != NULL)
    ra_runtime_client_release(session);
  ra_runtime_set_unsupported();
  ra_notify_indicator_clear_kind(RA_INDICATOR_TRACKER);
  ra_runtime_log("runtime: unsupported platform; %s\n",
        was_hardcore ? "downgraded to Casual" : "already Casual");
  return was_hardcore;
}

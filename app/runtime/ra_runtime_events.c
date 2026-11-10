#include "ra_runtime_events_lb.h"
#include "ra_runtime_events.h"

#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

#include "ra_badge.h"
#include "ra_client.h"
#include "ra_http.h"
#include "ra_notify.h"
#include "ra_offline.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_core.h"
#include "ra_runtime_lb_service.h"
#include "ra_runtime_tracker.h"
#include "ra_shot.h"
#include "ra_settings.h"

static void events_push_unlock_card(const rc_client_achievement_t *achievement) {
  char badge_key[RA_BADGE_KEY_MAX];
  char badge_url[RA_BADGE_URL_MAX];
  char title[RA_RUNTIME_TITLE_MAX];
  char subtitle[RA_RUNTIME_DESCRIPTION_MAX];
  int has_badge = 0;

  badge_key[0] = '\0';
  badge_url[0] = '\0';
  if (ra_runtime_badge_key(achievement, badge_key, sizeof(badge_key),
            badge_url, sizeof(badge_url))) {
    ra_badge_request(badge_key, badge_url, 1);
    has_badge = 1;
  }
  snprintf(title, sizeof(title), "%s", achievement->title);
  snprintf(subtitle, sizeof(subtitle), "%s%s", g_rt.game_title,
      g_rt.settings.spectator ? "  (Spectator)" : "");
  ra_notify_push_achievement(RA_NOTIFY_UNLOCK, achievement->id,
              (int)achievement->points, title, subtitle,
              has_badge ? badge_key : NULL);
  if (g_rt.settings.auto_screenshot && !g_rt.settings.spectator)
    ra_shot_request(g_rt.game_title, title);
  ra_runtime_prefetch_next_badge(achievement);
}

static void events_handle_achievement_triggered(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  const rc_client_achievement_t *achievement = NULL;

  do {
    achievement = event->achievement;
    /* A trigger raised after the game left (or while a disc/hold makes memory
     * reads unavailable) is a false positive from the zero-filled reads above -
     * not a real unlock. Ignore it instead of showing a card and submitting it. */
    if (!g_rt.in_game || ra_runtime_eval_suspended()) {
      ra_runtime_log("runtime: achievement %u ignored (evaluation suspended)\n",
            achievement != NULL ? (unsigned)achievement->id : 0u);
      break;
    }
    if (achievement != NULL) {
      /* The list must reflect an unlock even when the banner is gated. */
      g_rt.dirty = 1;
      ra_runtime_log("runtime: achievement %u triggered notifications=%d unlock=%d spectator=%d\n",
            (unsigned)achievement->id,
            g_rt.settings.notifications != 0,
            g_rt.settings.visibility_unlock != 0,
            g_rt.settings.spectator != 0);
    }
    if (!g_rt.settings.notifications || !g_rt.settings.visibility_unlock)
      break;
    if (achievement == NULL || achievement->title == NULL)
      break;
    if (achievement->id >= RA_RUNTIME_WARNING_ACHIEVEMENT_ID) {
      ra_runtime_log("runtime: ignored warning achievement %u\n",
            (unsigned)achievement->id);
      break;
    }
    events_push_unlock_card(achievement);
  } while (0);
}
static void events_handle_achievement_challenge_indicator_show(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  char badge_key[RA_BADGE_KEY_MAX];
  char badge_url[RA_BADGE_URL_MAX];

  badge_key[0] = '\0';
  badge_url[0] = '\0';

  do {
    /* Track primed achievements for the RA tab even when the indicator row is
         * off: the list answers "what is live", which is not a notification. */
    ra_runtime_active_challenge_add(event->achievement);
    if (g_rt.settings.notifications && g_rt.settings.badges &&
      event->achievement != NULL && event->achievement->title != NULL) {
            int tile_badge = 0;

      badge_key[0] = '\0';
      badge_url[0] = '\0';
      if (ra_runtime_badge_key(event->achievement, badge_key,
                sizeof(badge_key), badge_url,
                sizeof(badge_url))) {
        ra_badge_request(badge_key, badge_url, 1);
        tile_badge = 1;
      }
      ra_notify_indicator_set_detail(
        RA_INDICATOR_CHALLENGE, event->achievement->id,
        event->achievement->title, NULL,
        tile_badge ? badge_key : NULL);
      ra_runtime_log("runtime: indicator +challenge id=%u '%s'\n",
            (unsigned)event->achievement->id,
            event->achievement->title);
    } else if (event->achievement != NULL) {
      /* Name the gate: a gated indicator otherwise leaves no trace. */
      ra_runtime_log("runtime: indicator challenge gated id=%u "
            "notifications=%d active_badges=%d\n",
            (unsigned)event->achievement->id,
            g_rt.settings.notifications != 0,
            g_rt.settings.badges != 0);
    }
  } while (0);
}
static void events_handle_achievement_challenge_indicator_hide(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    ra_runtime_active_challenge_remove(
      event->achievement != NULL ? event->achievement->id : 0);
    if (event->achievement != NULL) {
      ra_notify_indicator_clear(RA_INDICATOR_CHALLENGE,
                  event->achievement->id);
      ra_runtime_log("runtime: indicator -challenge id=%u\n",
            (unsigned)event->achievement->id);
    }
  } while (0);
}
static int events_tile_badge(const rc_client_achievement_t *achievement,
                             char *badge_key, size_t badge_key_size,
                             char *badge_url, size_t badge_url_size) {
  int tile_badge = 0;

  badge_key[0] = '\0';
  badge_url[0] = '\0';
  if (achievement != NULL &&
    ra_runtime_badge_key(achievement, badge_key, badge_key_size,
            badge_url, badge_url_size)) {
    ra_badge_request(badge_key, badge_url, 1);
    tile_badge = 1;
  } else if (achievement != NULL) {
    /* A tile with no key draws the glyph forever. */
    ra_runtime_log("runtime: tile badge key unavailable achiev=%u name=%s url=%s\n",
          (unsigned)achievement->id,
          achievement->badge_name != NULL ? achievement->badge_name : "(null)",
          achievement->badge_url != NULL ? "set" : "(null)");
  }
  return tile_badge;
}

static void events_handle_achievement_progress_indicator_show(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  char badge_key[RA_BADGE_KEY_MAX];
  char badge_url[RA_BADGE_URL_MAX];
  int tile_badge;

  do {
    tile_badge = events_tile_badge(event->achievement, badge_key,
                     sizeof(badge_key), badge_url, sizeof(badge_url));

    /* A primed achievement can also report measured progress: keep the copy
         * the RA tab lists in step with the value the tile shows. */
    ra_runtime_active_challenge_refresh(event->achievement);

    if (g_rt.settings.notifications && g_rt.settings.visibility_progress &&
      event->achievement != NULL && event->achievement->title != NULL) {
            ra_notify_indicator_set_detail(
        RA_INDICATOR_PROGRESS, event->achievement->id,
        event->achievement->title,
        g_rt.settings.indicator_values
          ? event->achievement->measured_progress
          : NULL,
        tile_badge ? badge_key : NULL);
      if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW)
        ra_runtime_log("runtime: indicator +progress id=%u '%s' "
              "measured='%s'%s\n",
              (unsigned)event->achievement->id,
              event->achievement->title,
              event->achievement->measured_progress,
              tile_badge ? " (badge)" : " (no badge)");
    } else if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW &&
        event->achievement != NULL) {
      /* SHOW only: UPDATE repeats while the tracker stays alive. */
      ra_runtime_log("runtime: indicator progress gated id=%u "
            "notifications=%d visibility=%d\n",
            (unsigned)event->achievement->id,
            g_rt.settings.notifications != 0,
            g_rt.settings.visibility_progress != 0);
    }
    /* Only SHOW needs a snapshot rebuild; rebuilding on every progress tick stole
     * CPU from the emulated game. */
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW)
      g_rt.dirty = 1;
  } while (0);
}
static void events_handle_achievement_progress_indicator_hide(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    /* Clear by kind, not by id. rcheevos raises this from its own 2 s timer
         * with no achievement attached (`src/rc_client.c:6109-6126`), and there
         * is only one progress tracker anyway, so an id match never fired and
         * the tile stayed up until the game exited (found ). The
         * action-driven HIDE does carry an achievement, but clearing the kind
         * covers both. */
    ra_notify_indicator_clear_kind(RA_INDICATOR_PROGRESS);
    ra_runtime_log("runtime: indicator -progress\n");
  } while (0);
}
static void events_handle_reset(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    /* A hardcore change (or another reset trigger) recreates the client
         * state; processing stays disabled until rc_client_reset is called. */
    ra_runtime_log("runtime: reset event; resetting client state\n");
    rc_client_reset(rc);
  } while (0);
}
static void events_handle_game_completed(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    if (g_rt.settings.notifications && g_rt.settings.visibility_mastery)
      ra_notify_push(RA_NOTIFY_MASTERY, 0, g_rt.game_title,
            "All achievements completed");
  } while (0);
}
static void events_handle_subset_completed(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    if (g_rt.settings.notifications && g_rt.settings.visibility_mastery)
      ra_notify_push(RA_NOTIFY_MASTERY, 0, g_rt.game_title,
            "Subset completed");
  } while (0);
}
/* One "connection is back" card per outage. Several independent layers can
 * notice the same recovery: rcheevos' RECONNECTED event, the HTTP failure
 * streak, the offline-queue replay, and the slow-load retry. They all call this
 * and only the first one is shown. */
static int g_reconnect_notice_shown;

void ra_runtime_reconnect_reset(void) {
  g_reconnect_notice_shown = 0;
}

void ra_runtime_reconnect_note(int synced) {
  char line[96];

  if (g_reconnect_notice_shown)
    return;
  /* Wait for the replay count when submissions are still queued: the count is
   * more useful than a generic "reachable again" card. */
  if (synced <= 0 && ra_offline_queue_pending() > 0)
    return;
  g_reconnect_notice_shown = 1;
  if (synced > 0)
    snprintf(line, sizeof(line), "%d pending submission(s) synced", synced);
  else
    snprintf(line, sizeof(line), "RetroAchievements is reachable again.");
  ra_notify_push(RA_NOTIFY_INFO, 0, "Reconnected", line);
}

static void events_handle_disconnected(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  ra_runtime_reconnect_reset();

  do {
    /* Network loss during a live session: keep running and report the pending submission in
 * the status row; the banner is delayed so a retry that succeeds shows nothing. */
    ra_runtime_lock();
    ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status),
        "Offline - syncs when the connection returns");
    ra_runtime_unlock();
    /* Only promise/withhold when a session actually exists : during a load
         * the client can report a pending request while the session banner has not even
         * been drawn, which is how the boot burst (connection error -> disconnected ->
         * reconnected -> session banner) happened on a multi-disc PBP. */
    if (g_rt.info.ready) {
      ra_runtime_offline_arm();
    }
    ra_runtime_log("runtime: pending unlock; offline banner %s (queue on)\n",
          g_rt.info.ready ? "armed for 5000 ms" : "skipped, session not ready");
  } while (0);
}
static void events_handle_reconnected(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    ra_runtime_lock();
    ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status), "RetroAchievements ready");
    ra_runtime_unlock();
    ra_runtime_log("runtime: network restored; session continues\n");
    /* Only answer a banner the player actually saw. A retry that lands inside the
         * delay window produces no offline card and no "restored" card either. */
    ra_runtime_offline_cancel();
    if (ra_runtime_offline_take_shown())
      ra_runtime_reconnect_note(0);
  } while (0);
}
static void events_handle_server_error(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    if (g_rt.settings.notifications && g_rt.settings.visibility_verbose &&
      event->server_error != NULL)
      ra_notify_push(RA_NOTIFY_ERROR, 0, "RetroAchievements",
            event->server_error->error_message != NULL
              ? event->server_error->error_message
              : "Server error");
  } while (0);
}


static int events_is_leaderboard(const rc_client_event_t *event) {
  switch (event->type) {
  case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
  case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
  case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
  case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
    return 1;
  default:
    return 0;
  }
}

static int events_should_dispatch(const rc_client_event_t *event) {
  if (event == NULL)
    return 0;

  /* Teardown flushes rcheevos' pending queue; the list still refreshes. */
  if (g_rt.stop) {
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED)
      g_rt.dirty = 1;
    return 0;
  }
  if (!g_rt.info.hardcore && events_is_leaderboard(event))
    return 0;
  /* Keep indicator/tracker state alive across the PS window: rcheevos does not
   * re-send SHOW for something it already considers active. */
  if (g_rt.shell_transition_until_us != 0 &&
    (unsigned long long)sceKernelGetSystemTimeWide() <
      g_rt.shell_transition_until_us) {
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
    case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
    case RC_CLIENT_EVENT_GAME_COMPLETED:
    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
      break;
    default:
      if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED)
        g_rt.dirty = 1;
      return 0;
    }
  }

  /* A trigger that races a transition must refresh the list but show no card. */
  if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED &&
    !ra_runtime_game_memory_valid()) {
    ra_runtime_log("runtime: dropped unlock event outside a running game\n");
    g_rt.dirty = 1;
    return 0;
  }
  return 1;
}

/* Achievement events: unlocks and the two overlay indicators. */
static void events_dispatch_achievement(const rc_client_event_t *event,
                                        rc_client_t *rc) {
  switch (event->type) {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
    events_handle_achievement_triggered(event, rc);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
    events_handle_achievement_challenge_indicator_show(event, rc);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
    events_handle_achievement_challenge_indicator_hide(event, rc);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
    events_handle_achievement_progress_indicator_show(event, rc);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
    events_handle_achievement_progress_indicator_hide(event, rc);
    break;
  default:
    break;
  }
}

/* Leaderboard trackers, scoreboards and submissions. */

/* Session lifecycle outside achievements and leaderboards. */
static void events_dispatch_session(const rc_client_event_t *event,
                                    rc_client_t *rc) {
  switch (event->type) {
  case RC_CLIENT_EVENT_RESET:
    events_handle_reset(event, rc);
    break;
  case RC_CLIENT_EVENT_GAME_COMPLETED:
    events_handle_game_completed(event, rc);
    break;
  case RC_CLIENT_EVENT_SUBSET_COMPLETED:
    events_handle_subset_completed(event, rc);
    break;
  case RC_CLIENT_EVENT_DISCONNECTED:
    events_handle_disconnected(event, rc);
    break;
  case RC_CLIENT_EVENT_RECONNECTED:
    events_handle_reconnected(event, rc);
    break;
  case RC_CLIENT_EVENT_SERVER_ERROR:
    events_handle_server_error(event, rc);
    break;
  default:
    break;
  }
}

void ra_runtime_events_dispatch(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  if (!events_should_dispatch(event))
    return;

  switch (event->type) {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
    events_dispatch_achievement(event, rc);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
  case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
  case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
  case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
  case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    ra_runtime_events_dispatch_leaderboard(event, rc);
    break;
  case RC_CLIENT_EVENT_RESET:
  case RC_CLIENT_EVENT_GAME_COMPLETED:
  case RC_CLIENT_EVENT_SUBSET_COMPLETED:
  case RC_CLIENT_EVENT_DISCONNECTED:
  case RC_CLIENT_EVENT_RECONNECTED:
  case RC_CLIENT_EVENT_SERVER_ERROR:
    events_dispatch_session(event, rc);
    break;
  default:
    break;
  }
}

/* IDs rejected by the validator are remembered briefly so the later
 * LEADERBOARD_SCOREBOARD event does not show a result card for a submission
 * that never left the device. */
int ra_runtime_leaderboard_validate(uint32_t id, int score) {
  int ok = ra_runtime_leaderboard_validate_impl(id, score);

  if (!ok) {
    g_rt.rejected_lb_id = id;
    g_rt.rejected_lb_until =
      (unsigned long long)sceKernelGetSystemTimeWide() + 10000000ULL;
  }
  return ok;
}

void ra_runtime_fail(const char *status);
void ra_runtime_mark_game_unsupported(void);
void ra_runtime_set_ready(void);

void ra_runtime_status(int result, const char *error_message, void *userdata) {
  (void)userdata;
  if (result != RC_OK) {
    char text[RA_RUNTIME_STATUS_MAX];
    snprintf(text, sizeof(text), "%s",
        error_message != NULL ? error_message : "RetroAchievements error");
    if (result == RC_NO_GAME_LOADED ||
      (error_message != NULL && strcmp(error_message, "Unknown game") == 0)) {
      /* An unknown hash is terminal. Retrying cannot make the game
             * appear in the database and produced the Signing/Inactive loop. */
      ra_runtime_mark_game_unsupported();
      ra_runtime_fail("RetroAchievements does not support this game");
      return;
    }
    ra_runtime_log("runtime: load failed result=%d message='%s'\n", result,
          text);
    ra_runtime_log("runtime: load request='r=%s' http=%d body=%u "
          "content_length=%u stage=%d err=0x%08X net=0x%08X\n",
          ra_http_get_last_request(), ra_http_get_last_status(),
          (unsigned)ra_http_get_last_body_length(),
          (unsigned)ra_http_get_last_content_length(),
          ra_http_get_last_stage(), (unsigned)ra_http_get_last_error(),
          (unsigned)ra_http_get_last_net_state());
    ra_runtime_log("runtime: load request body='%s'\n",
          ra_http_get_last_request_body());
    ra_runtime_log("runtime: response head='%s'\n",
          ra_http_get_last_body_prefix());
    ra_runtime_log("runtime: response tail='%s'\n",
          ra_http_get_last_body_tail());
    ra_runtime_fail(text);
    return;
  }
  ra_runtime_set_ready();
}

#include "ra_runtime_events_lb.h"

#include "ra_runtime_events.h"
#include <stdio.h>
#include <psp2/kernel/processmgr.h>
#include "ra_badge.h"
#include "ra_client.h"
#include "ra_notify.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_core.h"
#include "ra_runtime_lb_service.h"
#include "ra_runtime_tracker.h"
#include "ra_shot.h"
#include "ra_settings.h"

static void events_handle_leaderboard_tracker_show(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    /* rcheevos raises UPDATE on every frame, and the display string is its own
         * buffer: copy once, and only when something changed. An empty display or a
         * zero id cannot be drawn and cannot be hidden by id later, so drop those
         *. */
    if (event->leaderboard_tracker == NULL ||
      event->leaderboard_tracker->id == 0 ||
      event->leaderboard_tracker->display == NULL ||
      event->leaderboard_tracker->display[0] == '\0') {
      if (event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW)
        ra_runtime_log("runtime: tracker ignored (id=%u display=%s)\n",
              (unsigned)(event->leaderboard_tracker != NULL
                    ? event->leaderboard_tracker->id : 0u),
              (event->leaderboard_tracker != NULL &&
              event->leaderboard_tracker->display != NULL)
                ? event->leaderboard_tracker->display : "null");
      break;
    }
    if (g_rt.settings.leaderboards != 0 && g_rt.settings.notifications &&
      g_rt.settings.visibility_lboard_trackers) {
      if (ra_runtime_tracker_update(event->leaderboard_tracker->id,
                      event->leaderboard_tracker->display)) {
        ra_notify_indicator_set_detail(
          RA_INDICATOR_TRACKER, event->leaderboard_tracker->id,
          event->leaderboard_tracker->display, NULL, NULL);
      }
      if (event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW)
        ra_runtime_log("runtime: indicator +tracker id=%u '%s'\n",
              (unsigned)event->leaderboard_tracker->id,
              event->leaderboard_tracker->display);
    } else if (event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW) {
      ra_runtime_log("runtime: indicator tracker gated leaderboards=%d "
            "notifications=%d visibility=%d\n",
            g_rt.settings.leaderboards != 0,
            g_rt.settings.notifications != 0,
            g_rt.settings.visibility_lboard_trackers != 0);
    }
  } while (0);
}

static void events_handle_leaderboard_tracker_hide(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;

  do {
    if (event->leaderboard_tracker != NULL) {
      ra_notify_indicator_clear(RA_INDICATOR_TRACKER,
                  event->leaderboard_tracker->id);
      ra_runtime_tracker_forget(event->leaderboard_tracker->id);
      ra_runtime_log("runtime: indicator -tracker id=%u\n",
            (unsigned)event->leaderboard_tracker->id);
    }
  } while (0);
}

static void events_handle_leaderboard_scoreboard(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  char subtitle[RA_RUNTIME_DESCRIPTION_MAX];

  do {
    if (!g_rt.in_game || ra_runtime_eval_suspended()) {
      ra_runtime_log("runtime: leaderboard scoreboard ignored (evaluation suspended)\n");
      break;
    }
    if (event->leaderboard_scoreboard != NULL &&
      event->leaderboard_scoreboard->leaderboard_id == g_rt.rejected_lb_id &&
      (unsigned long long)sceKernelGetSystemTimeWide() < g_rt.rejected_lb_until) {
      ra_runtime_log("runtime: leaderboard scoreboard ignored (submission rejected)\n");
      break;
    }
    if (g_rt.settings.leaderboards != 0 && g_rt.settings.notifications &&
      g_rt.settings.visibility_lboard_submit &&
      event->leaderboard_scoreboard != NULL) {
      snprintf(subtitle, sizeof(subtitle), "Rank %u of %u (best %s)",
          (unsigned)event->leaderboard_scoreboard->new_rank,
          (unsigned)event->leaderboard_scoreboard->num_entries,
          event->leaderboard_scoreboard->best_score);
      ra_notify_push(RA_NOTIFY_INFO,
            event->leaderboard_scoreboard->leaderboard_id,
            "Leaderboard", subtitle);
    }
  } while (0);
}

static void events_handle_leaderboard_started(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  const rc_client_leaderboard_t *leaderboard = NULL;

  do {
    if (!g_rt.in_game || ra_runtime_eval_suspended()) {
      ra_runtime_log("runtime: leaderboard start ignored (evaluation suspended)\n");
      break;
    }
    leaderboard = event->leaderboard;
    if (leaderboard == NULL || leaderboard->title == NULL ||
      leaderboard->title[0] == '\0') {
      ra_runtime_log("runtime: leaderboard start ignored (id=%u no title)\n",
            (unsigned)(leaderboard != NULL ? leaderboard->id : 0u));
      break;
    }
    /* Log the event before any setting can swallow it: "no card appeared" used
         * to be indistinguishable from "the game never started a leaderboard", and
         * that ambiguity cost a debugging round (). */
    ra_runtime_log("runtime: leaderboard +start id=%u '%s'\n",
          (unsigned)leaderboard->id, leaderboard->title);
    if (g_rt.settings.leaderboards != 0 && g_rt.settings.notifications &&
      g_rt.settings.visibility_lboard_start) {
      ra_notify_push(RA_NOTIFY_LB_START, leaderboard->id,
            leaderboard->title, "Leaderboard attempt started");
    } else {
      ra_runtime_log("runtime: leaderboard start gated leaderboards=%d "
            "notifications=%d visibility=%d\n",
            g_rt.settings.leaderboards != 0,
            g_rt.settings.notifications != 0,
            g_rt.settings.visibility_lboard_start != 0);
    }
  } while (0);
}

static void events_handle_leaderboard_submitted(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  const rc_client_leaderboard_t *leaderboard = NULL;

  do {
    if (!g_rt.in_game || ra_runtime_eval_suspended()) {
      ra_runtime_log("runtime: leaderboard result ignored (evaluation suspended)\n");
      break;
    }
    leaderboard = event->leaderboard;
    if (g_rt.settings.encore) {
      /* The submission never left the device; do not claim otherwise. */
      ra_runtime_log("runtime: leaderboard %u submission suppressed (encore)\n",
            leaderboard != NULL ? (unsigned)leaderboard->id : 0u);
      break;
    }
    if (leaderboard != NULL)
      ra_runtime_log("runtime: leaderboard result id=%u '%s'\n",
            (unsigned)leaderboard->id,
            leaderboard->title != NULL ? leaderboard->title : "");
    if (leaderboard != NULL && leaderboard->id == g_rt.rejected_lb_id &&
      (unsigned long long)sceKernelGetSystemTimeWide() <
        g_rt.rejected_lb_until) {
      ra_runtime_log("runtime: rejected leaderboard %u result card suppressed\n",
            (unsigned)leaderboard->id);
      break;
    }
    if (g_rt.settings.leaderboards != 0 && g_rt.settings.notifications &&
      g_rt.settings.visibility_lboard_submit && leaderboard != NULL)
      ra_notify_push(RA_NOTIFY_LB_SUBMIT, leaderboard->id,
            leaderboard->title, "Leaderboard attempt submitted");
    else
      ra_runtime_log("runtime: leaderboard result gated leaderboards=%d "
            "notifications=%d visibility=%d\n",
            g_rt.settings.leaderboards != 0,
            g_rt.settings.notifications != 0,
            g_rt.settings.visibility_lboard_submit != 0);
  } while (0);
}

static void events_handle_leaderboard_failed(const rc_client_event_t *event, rc_client_t *rc) {
  (void)rc;
  const rc_client_leaderboard_t *leaderboard = NULL;

  do {
    if (!g_rt.in_game || ra_runtime_eval_suspended()) {
      ra_runtime_log("runtime: leaderboard failure ignored (evaluation suspended)\n");
      break;
    }
    leaderboard = event->leaderboard;
    if (leaderboard != NULL)
      ra_runtime_log("runtime: leaderboard failed id=%u\n",
            (unsigned)leaderboard->id);
    if (g_rt.settings.leaderboards != 0 && g_rt.settings.notifications &&
      g_rt.settings.visibility_lboard_cancel && leaderboard != NULL)
      ra_notify_push(RA_NOTIFY_LB_FAIL, leaderboard->id,
            leaderboard->title, "Leaderboard attempt failed");
    else
      ra_runtime_log("runtime: leaderboard failure gated leaderboards=%d "
            "notifications=%d visibility=%d\n",
            g_rt.settings.leaderboards != 0,
            g_rt.settings.notifications != 0,
            g_rt.settings.visibility_lboard_cancel != 0);
  } while (0);
}

void ra_runtime_events_dispatch_leaderboard(const rc_client_event_t *event,
                                        rc_client_t *rc) {
  switch (event->type) {
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
    events_handle_leaderboard_tracker_show(event, rc);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
    events_handle_leaderboard_tracker_hide(event, rc);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
    events_handle_leaderboard_scoreboard(event, rc);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
    events_handle_leaderboard_started(event, rc);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
    events_handle_leaderboard_submitted(event, rc);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    events_handle_leaderboard_failed(event, rc);
    break;
  default:
    break;
  }
}

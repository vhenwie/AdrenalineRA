#include "ra_runtime_internal.h"

#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/openpsid.h>
#include <psp2/power.h>
#include <psp2/display.h>

#include "main.h"
#include "ra_arena.h"
#include "ra_badge.h"
#include "ra_client.h"
#include "ra_credentials.h"
#include "ra_hash.h"
#include "ra_http.h"
#include "ra_log.h"
#include "ra_netqueue.h"
#include "ra_notify.h"
#include "ra_offline.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_cards.h"
#include "ra_runtime_clock.h"
#include "ra_runtime_boot_auth.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_tracker.h"
#include "ra_runtime_core.h"
#include "ra_runtime_diag.h"
#include "ra_runtime_disc.h"
#include "ra_runtime_events.h"
#include "ra_runtime_hold.h"
#include "ra_runtime_lb_service.h"
#include "ra_runtime_lifecycle.h"
#include "ra_runtime_load.h"
#include "ra_runtime_memory.h"
#include "ra_runtime_path.h"
#include "ra_runtime_platform.h"
#include "ra_runtime_plugins.h"
#include "ra_server.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "ra_shot.h"


static int runtime_start_session(ra_hash_result_t *hash) {
  if (!ra_runtime_session_prepare(hash))
    return 0;
  if (!ra_runtime_session_load(hash))
    return 0;
  return 1;
}

/* Logic frame gate: evaluate while the menu is closed and no identity hold is
 * up (a host-visible identity change is the one window where RAM may already
 * belong to the shell). */
static void runtime_eval_frame(void) {
  unsigned long long tick_now =
    (unsigned long long)sceKernelGetSystemTimeWide();
  int held = ra_runtime_eval_hold_until() != 0 && tick_now < ra_runtime_eval_hold_until();
  int evaluate = !held &&
    (!g_rt.paused ||
    ra_runtime_lb_state()->state == RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING);
  ra_session_t *session = ra_runtime_session_acquire();

  if (evaluate) {
    unsigned long long eval_started_us =
      (unsigned long long)sceKernelGetSystemTimeWide();
    unsigned long long eval_us;

    ra_session_do_frame(session);
    eval_us = (unsigned long long)sceKernelGetSystemTimeWide() -
        eval_started_us;
    g_rt.eval_sum_us += eval_us;
    ++g_rt.eval_count;
    if (eval_us > g_rt.eval_max_us)
      g_rt.eval_max_us = eval_us;
  } else {
    ra_session_idle(session);
  }
  ra_session_release(session);
}

/* Heartbeat: ~10 s at the 16 ms cadence. */
static void runtime_heartbeat(unsigned int *eval_polls) {
  if (++(*eval_polls) >= 600) {
    unsigned int eval_divisor = g_rt.eval_count != 0 ? g_rt.eval_count : 1;
    unsigned int refresh_divisor =
      g_rt.refresh_count != 0 ? g_rt.refresh_count : 1;

    *eval_polls = 0;
    ra_runtime_log("runtime: eval paused=%d mem=%d susp=%d running=%d "
          "ingame=%d held=%d\n",
          g_rt.paused, ra_runtime_game_memory_valid(), g_rt.suspended, g_rt.running,
          g_rt.in_game,
          ra_runtime_eval_hold_until() != 0 &&
            (unsigned long long)sceKernelGetSystemTimeWide() <
              ra_runtime_eval_hold_until());
    /* What our thread costs the console: one logic frame per poll and the
           * snapshot rebuilds in between. This is the measurement that turns "the
           * game stutters" into a number. */
    ra_runtime_log("runtime: cost frame=%u avg=%llu us max=%llu us "
          "refresh=%u avg=%llu us max=%llu us arm=%u gpu=%u\n",
          g_rt.eval_count, g_rt.eval_sum_us / eval_divisor,
          g_rt.eval_max_us, g_rt.refresh_count,
          g_rt.refresh_sum_us / refresh_divisor, g_rt.refresh_max_us,
          (unsigned)scePowerGetArmClockFrequency(),
          (unsigned)scePowerGetGpuClockFrequency());
    g_rt.eval_sum_us = 0;
    g_rt.eval_max_us = 0;
    g_rt.eval_count = 0;
    g_rt.refresh_sum_us = 0;
    g_rt.refresh_max_us = 0;
    g_rt.refresh_count = 0;
  }
}

static void runtime_refresh_if_dirty(void) {
  if (!g_rt.dirty)
    return;
  {
    unsigned long long refresh_now =
      (unsigned long long)sceKernelGetSystemTimeWide();

    /* Refresh is immediate while the menu or a dialog is open, otherwise at most once per
 * 5 s: it costs 78-243 ms on a large set and only the menu reads it. */
    if (g_rt.paused ||
      refresh_now - g_rt.last_refresh_us >= RA_RUNTIME_REFRESH_DEFER_US) {
      g_rt.last_refresh_us = refresh_now;
      ra_runtime_refresh();
    }
  }
}

static void runtime_presence_tick(void) {
  if ((unsigned long long)sceKernelGetSystemTimeWide() -
      g_rt.last_presence_check_us >= 1000000ULL) {
    g_rt.last_presence_check_us =
      (unsigned long long)sceKernelGetSystemTimeWide();
    ra_runtime_update_rich_presence();
  }
}

/* The menu and the game runtime can be separate address spaces, so the dirty
 * flag is not always visible here. Poll the policy as a fallback; a stale
 * Hardcore setting would keep leaderboards alive after the user downgraded. */
static int runtime_settings_ready(void) {
  static unsigned long long last_poll_us;
  unsigned long long now;

  if (!ra_runtime_settings_dirty()) {
    now = (unsigned long long)sceKernelGetSystemTimeWide();
    if (last_poll_us != 0 && now - last_poll_us < 2000000)
      return 1;
    last_poll_us = now;
  }
  ra_runtime_load_settings();
  return g_rt.settings.ra_enabled != 0;
}

/* One pass of the session loop. Returns 0 when the session must end (the
 * setting turned RA off). */
static int runtime_iteration(unsigned int *eval_polls) {
  if (!runtime_settings_ready())
    return 0;
  ra_runtime_lb_service();
  ra_runtime_warm_tick();
  runtime_eval_frame();

  runtime_heartbeat(eval_polls);
  runtime_refresh_if_dirty();
  runtime_presence_tick();
  ra_runtime_clock_tick();
  /* One evaluation per emulated frame, matching PPSSPP's rc_client_do_frame
   * cadence. A 16 ms timer can miss a one-frame leaderboard start transition;
   * the vblank wait also keeps the runtime off the emulator's frame clock. */
  if (g_rt.in_game)
    sceDisplayWaitVblankStart();
  else
    sceKernelDelayThread(16 * 1000);
  return 1;
}

static void runtime_teardown(void) {
  ra_shot_flush_copies();
  ra_badge_stop();
  /* The session can end without the exit path; release the same holds here. */
  ra_runtime_hold_release_all();
  /* The clock is not ours to keep: hand it back before the session ends. */
  ra_runtime_clock_release("session-end");

  /* Stop accepting work from rcheevos before anything is torn down. The line it
     * logs carries `delivered=`/`inline=`: with the queue working, `inline=0`. */
  ra_netqueue_stop();

  /* The warm-up list holds pointers into the client's current game. Drop it
     * before the game is unloaded or the client destroyed; the reuse path keeps
     * the client, so a stale list would dereference freed achievements. */
  if (g_rt.warm_list != NULL) {
    ra_session_t *warm_session;
    ra_client_t *warm_client = ra_runtime_client_acquire(&warm_session);

    rc_client_destroy_achievement_list(g_rt.warm_list);
    g_rt.warm_list = NULL;
    if (warm_client != NULL)
      ra_runtime_client_release(warm_session);
  }

  if (g_rt.reuse_session && g_rt.session != NULL) {
    ra_runtime_flush_netqueue();
    ra_session_unload_game(g_rt.session);
    ra_runtime_log("runtime: session retained for next game\n");
  } else {
    ra_runtime_destroy_session();
    ra_http_set_session_active(0);
  }
  ra_runtime_lb_reset();
  ra_runtime_set_game_memory_valid(0);
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  g_rt.reuse_session = 0;
  ra_runtime_lock();
  g_rt.info.running = 0;
  g_rt.info.ready = 0;
  g_rt.info.unsupported = 0;
  g_rt.info.unsupported_platform = 0;
  g_rt.platform_downgraded = 0;
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status), "Session stopped");
  ra_runtime_unlock();
}

void ra_runtime_run(void) {
  ra_hash_result_t hash;
  unsigned int eval_polls = 0;

  if (!runtime_start_session(&hash))
    return;

  while (!g_rt.stop) {
    if (!runtime_iteration(&eval_polls))
      break;
  }

  runtime_teardown();
}

/* Book-keeping after a session loop returns: retry the load, or stop retrying
 * once the attempts or the launch policy say so. */
static void runtime_after_run(void) {
  int running;

  ra_runtime_lock();
  running = g_rt.info.running;
  ra_runtime_unlock();
  if (!running) {
    if (g_rt.game_unsupported || g_rt.offline_at_launch) {
      /* Terminal for this game: an unsupported hash, or no network at
         * launch (the offline policy says do not create a session even if
         * the link returns). No retry loop, no repeated holds. */
      g_rt.attempted = 1;
    }
    else {
      ++g_rt.load_failures;
      if (g_rt.load_failures >= RA_RUNTIME_LOAD_ATTEMPTS) {
        g_rt.attempted = 1; /* stop retrying until the game changes */
        ra_runtime_log("runtime: %d load attempts failed; automatic retry "
              "stopped (relaunch the game to try again)\n",
              g_rt.load_failures);
      }
      else {
        unsigned long long now =
          (unsigned long long)sceKernelGetSystemTimeWide();
        unsigned int delay_s = (unsigned int)(
          g_retry_delays_us[g_rt.load_failures - 1] / 1000000ULL);

        g_rt.attempted = 0;
        g_rt.retry_at_us = now + g_retry_delays_us[g_rt.load_failures - 1];
        ra_runtime_log("runtime: load attempt %d failed; next attempt in "
              "%u s\n", g_rt.load_failures, delay_s);
      }
    }
  }
}

int ra_runtime_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  for (;;) {
    sceKernelWaitSema(g_rt.start_sema, 1, NULL);
    if (g_rt.boot_auth_requested) {
      g_rt.boot_auth_requested = 0;
      ra_runtime_boot_auth();
      g_rt.boot_auth_done = 1;
      /* The boot login may have consumed the start signal; start_pending is authoritative. */
      if (!g_rt.start_pending)
        continue;
    }
    if (!g_rt.start_pending)
      continue; /* stale wake: no request pending */
    g_rt.start_pending = 0;
    g_rt.running = 1;
    if (!g_rt.stop)
      ra_runtime_run();
    g_rt.running = 0;
    if (!g_rt.stop)
      runtime_after_run();
  }
  return 0;
}

/* Standby and power events: hold the session across suspend and rebase every
 * time-based gate on resume. */
static void runtime_suspend(const char *source) {
  if (g_rt.suspended)
    return;
  g_rt.suspended = 1;
  /* Downloads cannot finish and a card frozen mid-animation looks broken. */
  ra_badge_stop();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  ra_runtime_log("runtime: suspend (%s); session held\n", source);
}

void ra_runtime_resume(const char *source) {
  g_rt.suspended = 0;
  /* The frozen interval was never measured: clear every gate so it cannot
     * expire a grace window, force a restart check, or read as a lost game. */
  g_rt.no_game_since_us = 0;
  g_rt.no_game_stopped = 0;
  g_rt.last_start_check_us = 0;
  g_rt.last_presence_check_us = 0;
  g_rt.memory_refusals = 0;
  g_rt.dirty = 1;
  ra_badge_init();
  ra_runtime_log("runtime: resume (%s); timers rebased\n", source);
}

static int runtime_power_callback(int notify_id, int notify_count, int notify_arg,
                void *arg) {
  const unsigned int suspend_bits =
    SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND;
  const unsigned int resume_bits =
    SCE_POWER_CB_SYSTEM_RESUMING | SCE_POWER_CB_SYSTEM_RESUME |
    SCE_POWER_CB_APP_RESUMING | SCE_POWER_CB_APP_RESUME;

  (void)notify_count;
  (void)notify_arg;
  (void)arg;

  /* Logged once per distinct id: firmware raises 0xFFFFFFFF ("power info changed")
     * many times per session, and a repeated line per event buried the transitions that
     * matter. */ {
    static unsigned int last_logged_id = 0xDEADBEEFu;

    if ((unsigned)notify_id != last_logged_id) {
      last_logged_id = (unsigned)notify_id;
      ra_runtime_log("runtime: power event 0x%08X\n", (unsigned)notify_id);
    }
  }

  /* Firmware reports 0xFFFFFFFF for "power info changed". Treating that as a
     * suspend stopped the badge worker and cleared cards mid-game, so only an
     * explicit suspend/resume bit acts, and only while a session is running. */
  if ((unsigned)notify_id == 0xFFFFFFFFu || notify_id == 0)
    return 0;

  if ((unsigned)notify_id & suspend_bits) {
    if (g_rt.running)
      runtime_suspend("power");
  } else if ((unsigned)notify_id & resume_bits) {
    ra_runtime_resume("power");
  }
  return 0;
}

void ra_runtime_install_power_hook(void) {
  if (g_rt.power_hook_requested)
    return;
  g_rt.power_hook_requested = 1;

  g_rt.power_callback = sceKernelCreateCallback("ra_power", 0,
                      runtime_power_callback, NULL);
  if (g_rt.power_callback < 0) {
    ra_runtime_log("runtime: power callback create failed (0x%08X)\n",
          (unsigned)g_rt.power_callback);
    g_rt.power_callback = -1;
    return;
  }
  if (scePowerRegisterCallback(g_rt.power_callback) < 0) {
    ra_runtime_log("runtime: power callback register failed\n");
    sceKernelDeleteCallback(g_rt.power_callback);
    g_rt.power_callback = -1;
    return;
  }
  ra_runtime_log("runtime: power callback registered\n");
}

/* Release the power hook before pspemu unloads; the service would keep a stale pointer. */

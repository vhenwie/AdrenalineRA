#include "ra_runtime_tick.h"

#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <stdio.h>

#include "ra_badge.h"
#include "ra_credentials.h"
#include "ra_http.h"
#include "ra_netqueue.h"
#include "ra_notify.h"
#include "ra_offline.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_cards.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_core.h"
#include "ra_runtime_disc.h"
#include "ra_runtime_hashlib.h"
#include "ra_runtime_lb_service.h"
#include "ra_runtime_lifecycle.h"
#include "ra_runtime_memory.h"
#include "ra_settings.h"

static void tick_disc_housekeeping(unsigned long long now_us) {
  (void)now_us;
  ra_runtime_maybe_fetch_hash_library();
  ra_runtime_maybe_replay_offline();
  ra_runtime_ensure_badge_cache();
  ra_runtime_disc_watch();
  /* A settings close asks for a re-check, and it is done here and not on pspemu's
     * thread ( - see ra_runtime_official_settings_closed()). */
  if (g_rt.disc_after_settings != 0) {
    g_rt.disc_after_settings = 0;
    ra_runtime_log("runtime: settings closed; re-checking the disc on the runtime "
          "thread\n");
    ra_runtime_disc_check();
  }
  /* An unresolved disc switch (no network) retries with the hash it already has. */
  ra_runtime_disc_retry();
  /* No periodic PS1 fingerprint hash: it reads/hashes the PBP on ux0 (~300 ms
   * on FFVII) and the frame thread may never block on storage. The serial watch
   * and the settings-driven disc check cover real disc changes. */
  /* A refused media switch means another game: restart on that container. */
  if (g_rt.restart_disc_pending != 0) {
    int restart_disc = g_rt.restart_disc_pending;

    g_rt.restart_disc_pending = 0;
    ra_runtime_disc_restart(restart_disc);
  }
  /* A successful media switch queues the disc card from the worker; push it here, on
     * the runtime thread, where the title and the notify queue are owned. */
  if (g_rt.disc_banner_pending != 0) {
    int banner_disc = g_rt.disc_banner_pending;

    g_rt.disc_banner_pending = 0;
    ra_runtime_push_disc_banner(banner_disc);
  }
}
static void tick_offline_notices(unsigned long long now_us) {
  /* The delayed offline card : only fires when the pending submission is
     * still pending after the delay, so a retry that lands first is never announced as
     * an outage. */
  if (ra_runtime_offline_due()) {
    ra_runtime_log("runtime: unlock still pending after %u ms; telling the player\n",
          (unsigned)(RA_OFFLINE_BANNER_DELAY_US / 1000ULL));
    if (g_rt.settings.notifications && g_rt.settings.visibility_login) {
      ra_notify_push(RA_NOTIFY_ERROR, 0, "RetroAchievements",
            "Wait to sync, or screenshot your achievement list");
    }
  }
  /* The worker cannot push a notification, so the replay result is announced here,
     * once per replay : "Reconnected - N pending submissions synced". */ {
    int synced = ra_netqueue_take_replay_count();

    if (synced > 0) {
      ra_runtime_log("runtime: reconnected; %d pending submission(s) synced\n",
            synced);
      ra_runtime_reconnect_note(synced);
    }
  }

  /* A tick can only run while we are awake, so a stale suspend flag - the boot
     * transition raises SUSPEND without a matching RESUME - must not freeze
     * evaluation for the whole session. */
  if (g_rt.suspended)
    ra_runtime_resume("tick");

  
  if (g_rt.last_tick_us != 0 && now_us > g_rt.last_tick_us &&
    now_us - g_rt.last_tick_us > RA_RUNTIME_TIME_JUMP_US) {
    g_rt.no_game_since_us = 0;
    g_rt.no_game_stopped = 0;
    g_rt.last_start_check_us = 0;
    g_rt.last_presence_check_us = 0;
    g_rt.memory_refusals = 0;
    ra_runtime_log("runtime: time jump %lu ms; timers rebased\n",
          (unsigned long)((now_us - g_rt.last_tick_us) / 1000ULL));
  }
}
static void tick_state_changed(const SceAdrenaline *adrenaline, int in_shell,
                               unsigned long long now_us, unsigned int fingerprint) {
  int ready_game = g_rt.info.ready && !in_shell && adrenaline != NULL &&
    adrenaline->app_type == SCE_APP_TYPE_GAME;

  g_rt.state_fingerprint = fingerprint;
  if (!ready_game) {
    ra_runtime_set_eval_hold_until(now_us + RA_RUNTIME_STATE_HOLD_US);
    ra_runtime_log("runtime: game state changed; evaluation held %u ms "
          "(shell=%d app=0x%X pops=%d ps1=%d title='%.20s' id='%.12s')\n",
          (unsigned)(RA_RUNTIME_STATE_HOLD_US / 1000ULL), in_shell,
          adrenaline != NULL ? (unsigned)adrenaline->app_type : 0u,
          adrenaline != NULL ? adrenaline->pops_mode : 0,
          g_rt.title_is_pops,
          adrenaline != NULL ? adrenaline->title : "",
          adrenaline != NULL ? adrenaline->titleid : "");
  } else {
    ra_runtime_log("runtime: game state changed in a ready game; "
          "evaluation stays continuous for leaderboard transitions\n");
  }
}

static int tick_identity(const SceAdrenaline *adrenaline, unsigned long long now_us) {
  int game;
    int has_identity = adrenaline != NULL &&
      (adrenaline->titleid[0] != '\0' ||
      adrenaline->filename[0] != '\0');
    int identity_known = g_rt.game_titleid[0] != '\0';
    int in_shell = ra_runtime_state_in_shell(adrenaline);

  g_rt.in_shell = in_shell;

    /* Latch the first pops_mode sighting for this title (see g_rt.title_is_pops):
         * a later flap to 0 must not switch the display path on. The shell owns
         * the screen again once the game is closed, so clear it there. */
    if (adrenaline != NULL && !in_shell && adrenaline->pops_mode)
      g_rt.title_is_pops = 1;
    else if (in_shell)
      g_rt.title_is_pops = 0;

    /* POPS only becomes readable once the boot payload is in PS1 RAM; until
         * then every read is refused (see read_psp_memory). */
    if (g_rt.title_is_pops)
      ra_runtime_memory_gate_tick();

    unsigned int fingerprint = ra_runtime_state_fingerprint(adrenaline, in_shell);

    /* A game closes here (PS Button > Exit to XMB): the pspemu and this process
         * keep running, only the game is gone, and the RAM the triggers read is
         * already the shell's. Hold evaluation across every identity change so the
         * transition frame is never evaluated, then let the shell state below stop
         * it for good. */
    if (fingerprint != g_rt.state_fingerprint)
      tick_state_changed(adrenaline, in_shell, now_us, fingerprint);

    /* Adrenaline's own menu treats SCE_APP_TYPE_GAME and SCE_APP_TYPE_APP as
         * "a game is running" (menu.c). The shell also blanks titleid/filename
         * mid-game for multi-EBOOT titles, so a known identity keeps the runtime
         * in game through that flap instead of expiring the session every grace
         * period. Requiring GAME alone made the runtime refuse every memory read
         * while the game was actually running. */
    game = !in_shell && adrenaline != NULL &&
      (adrenaline->app_type == SCE_APP_TYPE_GAME ||
        adrenaline->app_type == SCE_APP_TYPE_APP ||
        /* POPS reports its own app type (0x300); without it a PS1 title
                 * never reached the session start and no hash was ever logged. */
        adrenaline->app_type == SCE_APP_TYPE_POPS) &&
      (RA_ENABLE_POPS || !g_rt.title_is_pops) &&
      (has_identity || identity_known);
  return game;
}
static void tick_auto_resume(unsigned long long now_us, int game) {
  if (g_rt.auto_resume_pending != 0) {
    int resume_ready = game && !g_rt.paused &&
            g_rt.disc_after_settings == 0 && g_rt.restart_disc_pending == 0;

    if (resume_ready && now_us >= g_rt.auto_resume_after_us) {
      if (++g_rt.auto_resume_frames >= RA_RUNTIME_AUTO_RESUME_FRAMES) {
        g_rt.auto_resume_pending = 0;
        g_rt.auto_resume_frames = 0;
        ra_runtime_log("runtime: game visible again after the LiveArea; "
              "restarting the session\n");
        if (ra_runtime_request_start() < 0)
          ra_runtime_log("runtime: session restart refused; the menu's "
                "Start session row is the fallback\n");
        /* Coming back from the LiveArea: the frame loop must re-point pspemu's framebuffer. */
        g_rt.display_reassert = 1;
      }
    } else {
      g_rt.auto_resume_frames = 0;
    }
  }
}
static int tick_same_game(const SceAdrenaline *adrenaline) {
  return g_rt.game_titleid[0] != '\0' && adrenaline != NULL &&
    ((adrenaline->titleid[0] != '\0' &&
    strcmp(g_rt.game_titleid, adrenaline->titleid) == 0) ||
    (adrenaline->filename[0] != '\0' && g_rt.game_filename[0] != '\0' &&
    strcmp(g_rt.game_filename, adrenaline->filename) == 0));
}
static void tick_no_game_shell(const SceAdrenaline *adrenaline, int paused,
                               unsigned long long now_us) {
    
    int same_game =
      g_rt.game_titleid[0] != '\0' && adrenaline != NULL &&
      ((adrenaline->titleid[0] != '\0' &&
      strcmp(g_rt.game_titleid, adrenaline->titleid) == 0) ||
      (adrenaline->filename[0] != '\0' && g_rt.game_filename[0] != '\0' &&
      strcmp(g_rt.game_filename, adrenaline->filename) == 0));

    /* Reads stay valid through a shell flap (the identity is still known and
         * the RAM is still pspemu's). Only a real loss - grace expiry, an
         * explicit exit, suspend or background - refuses them.
         * Evaluation must also continue through a flap, otherwise a title whose
         * shell state is unstable can never unlock anything; it pauses only for
         * the menu or when no session is loaded at all. */
    /* Only the menu / common dialog pauses evaluation. A shell flap is not a
         * reason to stop: the verified build evaluated straight through it. */
    g_rt.paused = paused != 0;

    if (ra_runtime_state_in_shell(adrenaline)) {
      /* The game is closed and the pspemu XMB owns the screen. Nothing in
             * that RAM is the game's, and nothing may be drawn over the shell. */
      g_rt.paused = 1;
      ra_runtime_set_game_memory_valid(0);
      g_rt.memory_refusals = 0;
      if (g_rt.in_game) {
        g_rt.in_game = 0;
        ra_badge_stop();
        ra_notify_clear();
        ra_runtime_active_challenges_reset();
        ra_runtime_log("runtime: game closed; shell owns the screen; "
              "session held\n");
      }
    }


    if (g_rt.no_game_since_us == 0) {
      g_rt.no_game_since_us = now_us;
      ra_runtime_log("runtime: game state lost; holding session "
            "(identity %s app=0x%X pops=%d title='%.20s')\n",
            same_game ? "kept" : "gone",
            adrenaline != NULL ? (unsigned)adrenaline->app_type : 0u,
            adrenaline != NULL ? adrenaline->pops_mode : 0,
            adrenaline != NULL ? adrenaline->title : "");
    }
}
static void tick_stop_lost_session(void) {
  if (ra_runtime_game_memory_valid() && g_rt.memory_refusals > 0)
    ra_runtime_log("runtime: memory guard refused %u read(s)\n",
                   g_rt.memory_refusals);
  ra_runtime_set_game_memory_valid(0);
  g_rt.memory_refusals = 0;
  g_rt.in_game = 0;
  if (g_rt.running || g_rt.start_pending)
    ra_runtime_request_stop();
  ra_badge_stop();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  g_rt.attempted = 0;
  ra_runtime_reset_per_session_state("game gone");
  if (g_rt.net_failed != 0)
    ra_runtime_log("runtime: session ended; clearing the network-failure "
                   "state so the next session starts clean\n");
  g_rt.net_failed = 0;
  if (g_rt.forced_disc_index != 0) {
    ra_runtime_log("runtime: session ended; clearing the forced container %d "
                   "(it belonged to '%s')\n",
                   g_rt.forced_disc_index + 1, g_rt.forced_disc_path);
    g_rt.forced_disc_index = 0;
    g_rt.forced_disc_path[0] = '\0';
  }
  if (g_rt.offline_at_launch != 0)
    ra_runtime_log("runtime: session ended; clearing the offline-at-launch "
                   "latch for the next session\n");
  g_rt.offline_at_launch = 0;
  g_rt.load_failures = 0;
  g_rt.ps1_loaded = 0;
  g_rt.game_titleid[0] = '\0';
  g_rt.game_filename[0] = '\0';
  g_rt.game_title[0] = '\0';
  g_rt.last_start_check_us = 0;
  g_rt.no_game_stopped = 1;
  ra_runtime_lb_reset();
  ra_runtime_log("runtime: game state lost for grace period; session stopped\n");
}

static void tick_no_game_grace(const SceAdrenaline *adrenaline,
                                unsigned long long now_us) {
  int same_game = tick_same_game(adrenaline);

  if (!same_game)
    g_rt.paused = 1;
  if (same_game && now_us - g_rt.no_game_since_us < RA_RUNTIME_TRANSITION_HOLD_US)
    return;
  if (now_us - g_rt.no_game_since_us >= RA_RUNTIME_NO_GAME_GRACE_US &&
      !g_rt.no_game_stopped)
    tick_stop_lost_session();
}
static void tick_game_reset(int paused) {
  g_rt.no_game_since_us = 0;
  g_rt.no_game_stopped = 0;
  ra_runtime_set_game_memory_valid(1);
  g_rt.memory_refusals = 0;
  g_rt.paused = paused != 0;
}
static int tick_game_latch(const SceAdrenaline *adrenaline, int paused) {
  int changed = 0;
  if (!g_rt.in_game) {
    g_rt.in_game = 1;
    ra_runtime_log("runtime: in game (title='%.20s' id='%.12s' app=0x%X "
          "ps1=%d attempted=%d paused=%d frames=%u)\n",
          adrenaline->title, adrenaline->titleid,
          (unsigned)adrenaline->app_type, g_rt.title_is_pops,
          g_rt.attempted, paused, (unsigned)g_rt.frames);
  }

  changed = 0;
  if (adrenaline->titleid[0] != '\0') {
    if (g_rt.game_titleid[0] == '\0') {
      ra_runtime_copy_text(g_rt.game_titleid, sizeof(g_rt.game_titleid), adrenaline->titleid);
    } else if (strcmp(g_rt.game_titleid, adrenaline->titleid) != 0) {
      changed = 1;
      /* A different game is starting: the previous session's published
             * data must not stay readable while the new one is still loading,
             * otherwise its achievements show under the new game's title. */
      ra_runtime_lock();
      g_rt.achievement_count = 0;
      g_rt.subset_count = 0;
      g_rt.leaderboard_count = 0;
      g_rt.next_achievement_count = 0;
      g_rt.game_title[0] = '\0';
      ra_runtime_unlock();
    }
  }
  return changed;
}
static void tick_game_refresh_path(const SceAdrenaline *adrenaline) {
    /* A transition can temporarily publish an empty titleid/filename, or
         * change the inner EBOOT title while the RA identity is unchanged.
         * Refresh only the display/path fields that are valid. */
    if (adrenaline->filename[0] != '\0') {
      /* A disc swap keeps the titleid and changes the file : the
             * identity test above cannot see it, so it is caught here, before the
             * path is replaced and the old name is lost. */
      if (g_rt.game_filename[0] != '\0' &&
        strcmp(g_rt.game_filename, adrenaline->filename) != 0)
        ra_runtime_disc_changed(adrenaline->filename);
      ra_runtime_copy_text(g_rt.game_filename, sizeof(g_rt.game_filename), adrenaline->filename);
    }
    if (adrenaline->title[0] != '\0') {
      ra_runtime_copy_text(g_rt.game_title, sizeof(g_rt.game_title), adrenaline->title);
      ra_runtime_lock();
      ra_runtime_copy_text(g_rt.info.game_title, sizeof(g_rt.info.game_title), g_rt.game_title);
      ra_runtime_unlock();
    }
}
static void tick_game_changed(const SceAdrenaline *adrenaline) {
    if (g_rt.running || g_rt.start_pending) {
      g_rt.reuse_session = 1;
      g_rt.stop = 1;
    }
    ra_badge_stop();
    ra_notify_clear();
    ra_runtime_active_challenges_reset();
    ra_runtime_copy_text(g_rt.game_titleid, sizeof(g_rt.game_titleid), adrenaline->titleid);
    ra_runtime_copy_text(g_rt.game_filename, sizeof(g_rt.game_filename), adrenaline->filename);
    ra_runtime_copy_text(g_rt.game_title, sizeof(g_rt.game_title), adrenaline->title);
    g_rt.frames = 0;
    g_rt.attempted = 0;
    g_rt.offline_at_launch = 0;
    g_rt.game_unsupported = 0;
    
    ra_runtime_reset_per_session_state("new game");
    /* A new title starts at its own container 0. */
    g_rt.forced_disc_index = 0;
    g_rt.forced_disc_path[0] = '\0';
    g_rt.load_failures = 0;
    g_rt.game_is_pops = 0;
    g_rt.title_is_pops = 0;
    g_rt.ps1_loaded = 0;
    g_rt.ps1_entry_tried = 0;
    g_rt.retry_at_us = 0;
    g_rt.last_start_check_us = 0;
    /* A new game starts with no disc history and no library answer. */
    ra_runtime_hashlib_reset();
    g_rt.disc_tried[0] = '\0';
    g_rt.disc_hash[0] = '\0';
    ra_runtime_lb_reset();
}
static void tick_start_retry(unsigned long long now_us) {

  if (!g_rt.attempted && !g_rt.offline_at_launch && !g_rt.game_unsupported &&
    g_rt.frames > RA_RUNTIME_START_FRAMES) {
    RaSettings settings;
    unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

    if (now - g_rt.last_start_check_us < RA_RUNTIME_RETRY_US)
      return;
    if (g_rt.retry_at_us != 0 && now < g_rt.retry_at_us)
      return; /* a failed attempt is waiting out its backoff */
    /* Never restart a load while the menu is open.
         *
         * The retry takes the world pause, and the menu already owns one while it is up.
         * `pentazemin` cannot be paused twice, so a retry that fires under an open menu
         * is how the emulated PSP ended up frozen. The retry simply waits for the menu to
         * close - it is a backoff, not a deadline. */
    /* Never restart a load while the menu is open  - **except for a
         * PS1 title**. POPS never claims pentazemin's world pause (see
         * runtime_update_world_pause), so a load under an open menu cannot
         * deadlock, and deferring it left the RA tab with no "Loading
         * achievements..." state while the game sat at its title screen. */
    if (g_rt.paused && !g_rt.title_is_pops) {
      if (!g_rt.retry_deferred_logged) {
        g_rt.retry_deferred_logged = 1;
        ra_runtime_log("runtime: retry deferred; the menu owns the world pause\n");
      }
      return;
    }
    g_rt.retry_deferred_logged = 0;
    g_rt.last_start_check_us = now;
    ra_settings_load(&settings);
    ra_settings_normalize(&settings, ra_credentials_exists());
    if (settings.ra_enabled && ra_credentials_exists()) {
      ra_runtime_request_start();
    } else {
      static int logged_disabled;
      if (!logged_disabled) {
        logged_disabled = 1;
        ra_runtime_log("runtime: session start skipped (ra=%d credentials=%d)\n",
              settings.ra_enabled, ra_credentials_exists());
      }
    }
  }
}

void ra_runtime_tick(const SceAdrenaline *adrenaline, int paused) {
  unsigned long long now_us;
  int game;
  int changed;

  ra_runtime_init();
  ++g_rt.frames;
  ++g_rt.frames_since_ready;
  now_us = (unsigned long long)sceKernelGetSystemTimeWide();
  tick_disc_housekeeping(now_us);
  tick_offline_notices(now_us);
  g_rt.last_tick_us = now_us;

  game = tick_identity(adrenaline, now_us);
  tick_auto_resume(now_us, game);
  if (!game) {
    tick_no_game_shell(adrenaline, paused, now_us);
    tick_no_game_grace(adrenaline, now_us);
    return;
  }

  tick_game_reset(paused);
  changed = tick_game_latch(adrenaline, paused);
  if (!changed)
    tick_game_refresh_path(adrenaline);
  if (changed)
    tick_game_changed(adrenaline);
  g_rt.paused = paused != 0;
  tick_start_retry(now_us);
}



/* Multi-disc: a swap keeps the titleid and changes the file, so change-media is
 * attempted on suspicion; the hash library only rejects a swap that would move the
 * session to a different game. */
/* State for the block below is declared at the top of this file (the tick reads it). */


/* Last time the link was queried, for the throttle above. */
void ra_runtime_maybe_replay_offline(void) {
  unsigned long long now_us = (unsigned long long)sceKernelGetSystemTimeWide();
  int up;

  
  if (!g_rt.in_game || !ra_runtime_session_ready())
    return;

  /* Throttle link checks; reading the queue file every tick stalls. */
  if (g_rt.last_link_check_us != 0 && now_us > g_rt.last_link_check_us &&
    now_us - g_rt.last_link_check_us < RA_LINK_CHECK_US)
    return;
  g_rt.last_link_check_us = now_us;

  if (ra_offline_queue_pending() <= 0)
    return;

  up = ra_http_link_up();
  if (up && !g_rt.link_was_up) {
    g_rt.link_was_up = 1;
    ra_runtime_log("runtime: link up mid-game; replaying pending submissions\n");
    ra_netqueue_replay_offline();
    return;
  }
  if (!up) {
    ra_runtime_reconnect_reset();
    g_rt.link_was_up = 0;
  }
}

static unsigned long long g_at_us;
static int g_shown;

void ra_runtime_offline_arm(void) {
  g_at_us = (unsigned long long)sceKernelGetSystemTimeWide() +
    RA_OFFLINE_BANNER_DELAY_US;
}

void ra_runtime_offline_cancel(void) { g_at_us = 0; }

int ra_runtime_offline_due(void) {
  if (g_at_us == 0 ||
    (unsigned long long)sceKernelGetSystemTimeWide() < g_at_us)
    return 0;
  g_at_us = 0;
  g_shown = 1;
  return 1;
}

int ra_runtime_offline_take_shown(void) {
  int shown = g_shown;
  g_shown = 0;
  return shown;
}

void ra_runtime_offline_reset(void) { g_at_us = 0; g_shown = 0; }


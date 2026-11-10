#include "ra_runtime_internal.h"
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/openpsid.h>
#include <psp2/power.h>

#include "ra_badge.h"
#include "ra_client.h"
#include "main.h"
#include "ra_credentials.h"
#include "ra_hash.h"
#include "ra_http.h"
#include "ra_netqueue.h"
#include "ra_notify.h"
#include "ra_offline.h"
#include "ra_runtime_cards.h"
#include "ra_runtime_core.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_diag.h"
#include "ra_runtime_disc_serial.h"
#include "ra_runtime_events.h"
#include "ra_runtime_hold.h"
#include "ra_runtime_load.h"
#include "ra_runtime_memory.h"
#include "ra_runtime_path.h"
#include "ra_runtime_platform.h"
#include "ra_runtime_plugins.h"
#include "ra_server.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "ra_shot.h"

void ra_runtime_fail_status(const char *status) {
  /* Never leave the game frozen if the session could not load. */
  ra_runtime_release_world();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  ra_badge_stop();
  ra_runtime_set_game_memory_valid(0);
  ra_http_set_session_active(0);
  ra_runtime_lock();
  g_rt.info.running = 0;
  g_rt.info.ready = 0;
  g_rt.info.unsupported = 0;
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status), status);
  ra_runtime_unlock();
  ra_runtime_log("runtime: %s\n", status);
}


/* Session card image: the game's badge, fetched through the same cache. The key
 * is handed to the card itself, so the draw thread swaps the image in as soon as
 * the PNG is on disk. */

static int runtime_pbp_disc_count(void) {
  char serial[16];
  int i;

  if (g_rt.game_filename[0] == '\0')
    return 0;
  for (i = 0; i < 8; ++i) {
    if (!ra_runtime_disc_pbp_serial(g_rt.game_filename, i, serial, sizeof(serial)))
      break;
  }
  return i;
}

/* Which container POPS has mounted right now, 0-based, or -1 when it is not readable yet. */
static int runtime_live_disc_index(void) {
  char live[16];
  char serial[16];
  int i;

  if (g_rt.game_filename[0] == '\0' || !ra_runtime_disc_live_serial(live, sizeof(live)))
    return -1;
  for (i = 0; i < 8; ++i) {
    if (!ra_runtime_disc_pbp_serial(g_rt.game_filename, i, serial, sizeof(serial)))
      break;
    if (strcmp(serial, live) == 0)
      return i;
  }
  return -1;
}


void ra_runtime_push_session_start(void) {
  int disc = 0;

  /* A single-disc POPS title has no disc worth naming, so it keeps the plain session
     * banner; a multi-disc one names the disc that is actually mounted. */
  if (g_rt.game_is_pops && runtime_pbp_disc_count() > 1) {
    int live = runtime_live_disc_index();

    if (live >= 0)
      disc = live + 1;
  }
  ra_runtime_announced_disc_set(disc);
  ra_runtime_push_session_card(disc);
}



/* A destroyed session is retired until the next frame-thread init, so a raw
 * getter that was already in flight cannot touch freed memory. */
static ra_session_t *g_retired_session;

/* The deferred queue must never outlive the client it answers for: a request that
 * finishes after `ra_client_destroy()` would invoke rcheevos' callback on freed
 * state. Every teardown goes through here. */
ra_session_t *ra_runtime_session_acquire(void) {
  ra_session_t *session;

  ra_runtime_lock();
  session = g_rt.session;
  if (session != NULL)
    ra_session_retain(session);
  ra_runtime_unlock();
  return session;
}

static int runtime_offline_credentials(char *username, size_t username_capacity,
                                       char *token, size_t token_capacity) {
  ra_session_t *session = ra_runtime_session_acquire();
  ra_vault_credentials_t credentials;
  int copied;

  if (session == NULL)
    return 0;
  copied = ra_session_copy_credentials(session, &credentials);
  ra_session_release(session);
  if (!copied)
    return 0;
  snprintf(username, username_capacity, "%s", credentials.username);
  snprintf(token, token_capacity, "%s", credentials.token);
  ra_vault_clear(&credentials);
  return 1;
}

void ra_runtime_destroy_session(void) {
  ra_session_t *session;

  ra_runtime_lock();
  session = g_rt.session;
  g_rt.session = NULL;
  ra_runtime_unlock();
  if (session == NULL)
    return;
  ra_netqueue_stop();
  if (g_retired_session != NULL)
    ra_session_release(g_retired_session);
  g_retired_session = session;
}

/* Drops deferred answers around an unload (a callback must not run against a game
 * that is being unloaded), then re-arms the queue so the next load is asynchronous
 * too. */
void ra_runtime_flush_netqueue(void) {
  ra_netqueue_stop();
  ra_netqueue_init();
}

/* One reload when the load ran while memory was unreadable: rcheevos permanently
 * disables achievements and leaderboards whose memref failed validation. */

/* Badge warm-up: ACTIVE_CHALLENGE and ALMOST_THERE art only, held in RAM, one request
 * per second, paused while a card is visible or the world is held. */

/* Done on the runtime thread because rc_client is not safe to query from another
 * thread. */


/* Identity/build diagnostics: pspemu anchors, the retired cache purge and the
 * start heap headroom. */
static void prepare_log_identity(void) {
  ra_runtime_log("runtime: pspemu text=0x%08X nid=0x%08X session=%d\n", (unsigned)text_addr,
        (unsigned)module_nid, g_rt.session_ordinal);
  /* Delete what the retired achievement cache left on the card (sets payloads and
     * the hash cache). Done before the hash is computed, so a stale hash entry can
     * never be read on this launch either. */
  ra_server_purge_caches();
  /* Resolves a core dump without guesswork: module base = this anchor minus the
     * same symbol's address in the unstripped ELF (nm -n | grep runtime_run). */
  ra_runtime_log("runtime: module anchor=%08x\n",
        (unsigned)(uintptr_t)&ra_runtime_run);
  /* Heap headroom before anything is allocated, to separate "heap not raised"
     * from "heap raised but already consumed". */
  ra_http_probe_heap();
  ra_runtime_log("runtime: heap at start=%u KiB\n", ra_http_get_heap_probe_kib());
}

/* Name the gates so a silent session can be told from "never primed". */
static void prepare_log_settings(void) {
  ra_runtime_log("runtime: settings ra=%d notifications=%d unlock_visibility=%d "
        "hardcore=%d spectator=%d encore=%d active_badges=%d "
        "indicators=progress:%d trackers:%d values:%d "
        "cpu=%s anchor=%d size=%d "
        "leaderboards=%d lboard=start:%d submit:%d cancel:%d trackers:%d\n",
        g_rt.settings.ra_enabled != 0,
        g_rt.settings.notifications != 0,
        g_rt.settings.visibility_unlock != 0,
        ra_settings_hardcore_effective(&g_rt.settings, 1) != 0,
        g_rt.settings.spectator != 0,
        g_rt.settings.encore != 0,
        g_rt.settings.badges != 0,
        g_rt.settings.visibility_progress != 0,
        g_rt.settings.visibility_lboard_trackers != 0,
        g_rt.settings.indicator_values != 0,
        "scoped444",
        (int)g_rt.settings.indicator_anchor,
        (int)g_rt.settings.indicator_size,
        (int)g_rt.settings.leaderboards,
        g_rt.settings.visibility_lboard_start != 0,
        g_rt.settings.visibility_lboard_submit != 0,
        g_rt.settings.visibility_lboard_cancel != 0,
        g_rt.settings.visibility_lboard_trackers != 0);
}

/* Settings, credentials and the PSP memory mapping. Returns 0 when a gate failed. */
static int prepare_gates(void) {
  if (!g_rt.settings.ra_enabled) {
    ra_runtime_fail_status("RetroAchievements disabled");
    return 0;
  }
  if (!ra_credentials_exists()) {
    ra_runtime_fail_status("No saved RetroAchievements account");
    return 0;
  }
  if (!ra_runtime_memory_map()) {
    ra_runtime_fail_status("Could not map PSP memory");
    return 0;
  }
  return 1;
}

/* Maps the game image and resolves its path. */
static int prepare_game_image(char *game_path, size_t capacity) {
  /* Reads are valid for the whole session: pspemu owns this RAM until the
     * session is unloaded, whatever the shell app type does in the meantime. */
  ra_runtime_set_game_memory_valid(1);
  g_rt.memory_refusals = 0;
  g_rt.memory_zero_fills = 0;
  if (!ra_runtime_resolve_game_path(g_rt.game_filename, g_rt.game_titleid, game_path,
            capacity)) {
    ra_runtime_fail_status("Game image could not be found");
    return 0;
  }
  ra_runtime_log("runtime: game path='%s' titleid='%s'\n", game_path,
        g_rt.game_titleid);
  return 1;
}

/* POPS detection and the client identity it selects. */
static void prepare_detect_pops(const char *game_path) {
  /* A POPS title keeps its disc inside the EBOOT's PSISOIMG0000 container, so
     * the PSP hasher (which reads the ISO9660 volume directly) cannot see it and
     * rc_hash_psx has to walk the container through the same cdreader. Probing
     * costs one open and reads only the PBP header chain. */
  g_rt.game_is_pops = RA_ENABLE_POPS ? ra_hash_probe_ps1(game_path) : 0;
  g_rt.last_title_is_pops = g_rt.game_is_pops;
  /* The client identity is fixed before the session starts. */
  ra_http_set_console_identity(g_rt.game_is_pops, NULL);
}

/* A restart must hash the container that is live. The forced index is consumed
 * once, and only by the file it was set for, so it cannot leak to the next game. */
static int prepare_forced_index(const char *game_path) {
  if (g_rt.forced_disc_index != 0 && g_rt.forced_disc_path[0] != '\0' &&
    strcmp(g_rt.game_filename, g_rt.forced_disc_path) == 0) {
    return g_rt.forced_disc_index;
  }
  if (g_rt.forced_disc_index != 0) {
    ra_runtime_log("runtime: forced container %d dropped; it belongs to '%s', this "
          "game is '%s'\n",
          g_rt.forced_disc_index + 1, g_rt.forced_disc_path,
          g_rt.game_filename);
    g_rt.forced_disc_index = 0;
    g_rt.forced_disc_path[0] = '\0';
  }
  return 0;
}

/* Hashes the live container, retrying container 0 once when a forced container
 * was used and failed. */
static int prepare_compute_hash(const char *game_path, int forced_index,
                                ra_hash_result_t *hash) {
  int hash_ok;

  ra_hash_set_disc_index(forced_index);
  hash_ok = g_rt.game_is_pops
        ? ra_hash_ps1(game_path, NULL, hash)
        : ra_hash_game(game_path, NULL, hash);
  if (forced_index != 0)
    ra_runtime_log("runtime: hashed forced container %d for the restart\n",
          forced_index + 1);
  ra_hash_set_disc_index(0);
  if (!hash_ok) {
    /* Self-healing fallback: if the hash used a forced container, try the file's
     * own container 0 once before counting a failure. */
    if (forced_index != 0) {
      ra_runtime_log("runtime: forced container %d failed; hashing container 0 "
            "instead\n", forced_index + 1);
      g_rt.forced_disc_index = 0;
      g_rt.forced_disc_path[0] = '\0';
      ra_hash_set_disc_index(0);
      hash_ok = g_rt.game_is_pops
            ? ra_hash_ps1(game_path, NULL, hash)
            : ra_hash_game(game_path, NULL, hash);
      if (hash_ok)
        ra_runtime_log("runtime: container 0 hash succeeded after the forced "
              "container was dropped\n");
    }
  }
  return hash_ok;
}

/* Failure path of the hash: logs the step and asks for a status line. Returns 0. */
static int prepare_hash_outcome(int hash_ok) {
  if (hash_ok)
    return 1;
  /* Never retry the same wrong container: the index died with this attempt. */
  if (g_rt.forced_disc_index != 0) {
    ra_runtime_log("runtime: forced container %d cleared after the failed hash\n",
          g_rt.forced_disc_index + 1);
    g_rt.forced_disc_index = 0;
    g_rt.forced_disc_path[0] = '\0';
  }
  /* `ra_hash_last_error()` names the failing step (container block,
     * ISO9660 lookup, short read); without it a POPS hash failure is a
     * single opaque line. */
  ra_runtime_log("runtime: %s hash failed: %s (image %s, block %s)\n",
        g_rt.game_is_pops ? "PS1 disc" : "Game",
        ra_hash_last_error(), ra_hash_pbp_geometry(),
        ra_hash_last_block_error());
  ra_runtime_fail_status(g_rt.game_is_pops ? "PS1 disc hash failed"
                : "Game hash failed");
  return 0;
}

static int prepare_hash(const char *game_path, ra_hash_result_t *hash) {
  int forced_index = prepare_forced_index(game_path);
  int hash_ok = prepare_compute_hash(game_path, forced_index, hash);

  return prepare_hash_outcome(hash_ok);
}

/* Hash log, the stored session hash, and the POPS boot-payload gate. */
static void prepare_after_hash(const ra_hash_result_t *hash) {
  ra_runtime_log("runtime: hash=%s cache=%d ps1=%d in %llu ms\n", hash->hash,
        hash->from_cache, g_rt.game_is_pops,
        (unsigned long long)((sceKernelGetSystemTimeWide() - g_rt.started_us) /
                  1000ULL));
  /* Kept for the deferred reload: the runtime loop can fire it long after this load. */
  snprintf(g_rt.session_hash, sizeof(g_rt.session_hash), "%s", hash->hash);

  /* Hold the world from here until the session is live. The menu suppresses its
     * own pause while this is active (ra_runtime_holds_world()). */

  /* POPS: wait for the PS1 boot payload before the hold, never while holding, because a
   * held world freezes the PS1. */
  if (g_rt.game_is_pops && !g_rt.ps1_loaded) {
    unsigned waited = 0;

    ra_runtime_log("runtime: waiting for the PS1 boot payload before holding the "
          "world\n");
    while (!g_rt.ps1_loaded && waited < RA_RUNTIME_PS1_GATE_WAIT_MS && !g_rt.stop) {
      ra_runtime_memory_gate_tick();
      if (g_rt.ps1_loaded)
        break;
      sceKernelDelayThread(10 * 1000);
      waited += 10;
    }
    ra_runtime_log("runtime: PS1 gate %s before load (waited=%u ms)\n",
          g_rt.ps1_loaded ? "open" : "still closed", waited);
  }
}

/* Offline policy, decided once per game. Returns 0 when the launch had no link. */
static int prepare_offline_policy(void) {
  g_rt.load_started_us = (unsigned long long)sceKernelGetSystemTimeWide();

  /* Offline policy, decided once per game: no link at launch keeps RetroAchievements
 * unavailable for the whole session; a link at launch survives a later drop. */
  /* The offline latch is per attempt: a relaunch of the same title re-reads the link
   * instead of inheriting a dead state. */
  if (g_rt.offline_at_launch != 0) {
    g_rt.offline_at_launch = 0;
    ra_runtime_log("runtime: new session attempt; network policy re-evaluated "
          "(offline latch cleared)\n");
  }
  if (!ra_http_link_up()) {
    /* No link at launch: RetroAchievements stays unavailable for this session and is
     * not retried mid-game. */
    g_rt.offline_at_launch = 1;
    g_rt.attempted = 1;
    ra_runtime_log("runtime: no network at launch; RetroAchievements stays "
          "unavailable for this game session\n");
    ra_runtime_fail_status("RetroAchievements unavailable: no network at launch");
    return 0;
  }
  return 1;
}

int ra_runtime_session_prepare(ra_hash_result_t *hash) {
  char game_path[RA_HASH_PATH_MAX];

  ra_runtime_load_settings();
  if (g_rt.stop)
    return 0;
  /* Idempotent: a reused session (g_rt.reuse_session) has no live queue after the
     * previous run stopped one, and every path into a session needs it. */
  ra_netqueue_init();
  g_rt.detected_plugins = ra_runtime_scan_cheat_plugins();
  g_rt.booted_with_hardcore =
    ra_settings_hardcore_effective(&g_rt.settings, 1) != 0;
  if (g_rt.detected_plugins > 0 && g_rt.booted_with_hardcore)
    ra_runtime_log("runtime: %d cheat plugin(s) installed; Hardcore policy is "
          "`plugins=none`, so they are not loaded (a leaderboard "
          "submission is still accepted)\n", g_rt.detected_plugins);
  else if (g_rt.detected_plugins > 0)
    ra_runtime_log("runtime: %d cheat plugin(s) installed; Hardcore is off, so "
          "they may load\n", g_rt.detected_plugins);
  g_rt.started_us = (unsigned long long)sceKernelGetSystemTimeWide();
  g_rt.progress_logged = 0;
  
  ++g_rt.session_ordinal;
  ra_server_reset_platform_warning();
  ra_runtime_log("runtime: startup begin build=%s session=%d\n", RA_RUNTIME_BUILD_TAG,
        g_rt.session_ordinal);
  
  prepare_log_identity();
  prepare_log_settings();
  if (!prepare_gates())
    return 0;
  if (!prepare_game_image(game_path, sizeof(game_path)))
    return 0;
  prepare_detect_pops(game_path);
  if (!prepare_hash(game_path, hash))
    return 0;
  prepare_after_hash(hash);
  if (!prepare_offline_policy())
    return 0;

  if (g_rt.load_failures == 0)
    ra_runtime_hold_world();
  ra_server_set_parse_hooks(ra_runtime_parse_hold, ra_runtime_parse_release);


  return 1;
}

void ra_runtime_release_power_hook(void) {
  int callback = g_rt.power_callback;

  if (callback < 0)
    return;
  g_rt.power_callback = -1;
  g_rt.power_hook_requested = 0;
  if (scePowerUnregisterCallback(callback) < 0)
    ra_runtime_log("runtime: power callback unregister failed\n");
  if (sceKernelDeleteCallback(callback) < 0)
    ra_runtime_log("runtime: power callback delete failed\n");
  else
    ra_runtime_log("runtime: power callback released\n");
}

void ra_runtime_init(void) {
  RaRuntimeInfo initial;

  if (g_retired_session != NULL) {
    ra_session_release(g_retired_session);
    g_retired_session = NULL;
  }
  ra_runtime_hold_init();
  ra_offline_set_credentials_provider(runtime_offline_credentials);
  if (g_rt.start_sema < 0)
    g_rt.start_sema = sceKernelCreateSema("ra_runtime_start", 0, 0, 1, NULL);
  /* Reject invalid leaderboard values before they can reach the network. */
  ra_server_set_leaderboard_validator(ra_runtime_leaderboard_validate);

  if (g_rt.info.status[0] == '\0') {
    memset(&initial, 0, sizeof(initial));
    ra_runtime_copy_text(initial.status, sizeof(initial.status), "Idle");
    ra_runtime_info_set(&initial);
  }

  if (g_rt.thread < 0 && g_rt.start_sema >= 0) {
    g_rt.thread = sceKernelCreateThread("ra_runtime", ra_runtime_thread,
                    RA_RUNTIME_PRIORITY, RA_RUNTIME_STACK,
                    0, 0, NULL);
    if (g_rt.thread >= 0)
      sceKernelStartThread(g_rt.thread, 0, NULL);
  }

  ra_runtime_install_power_hook();
  ra_shot_init();
}

void ra_runtime_boot(void) {
  ra_runtime_init();
  if (g_rt.boot_auth_done || g_rt.boot_auth_requested)
    return;
  g_rt.boot_auth_requested = 1;
  if (g_rt.start_sema >= 0)
    sceKernelSignalSema(g_rt.start_sema, 1);
}

int ra_runtime_request_start(void) {
  ra_runtime_init();
  if (g_rt.start_sema < 0 || g_rt.thread < 0) {
    ra_runtime_log("runtime: start refused (sema=0x%08X thread=0x%08X)\n",
          (unsigned)g_rt.start_sema, (unsigned)g_rt.thread);
    return -1;
  }
  if (g_rt.running || g_rt.start_pending) {
    /* Re-arm the wake. The start semaphore has a max count of 1, so a signal
         * can be consumed by the boot-auth branch and leave `g_rt.start_pending` set
         * with nothing waiting on it. The worker ignores a wake with no pending
         * request, so an extra signal is harmless. */
    if (g_rt.start_pending && !g_rt.running) {
      static int rearmed;
      if (!rearmed) {
        rearmed = 1;
        ra_runtime_log("runtime: start signal re-armed (pending was set "
              "without a worker wake)\n");
      }
      sceKernelSignalSema(g_rt.start_sema, 1);
    }
    return 0;
  }

  /* The ePSP mapping helper is called from the draw thread; the worker only
     * reads through the pointer after this succeeds. */
  if (!ra_runtime_memory_map()) {
    ra_runtime_fail_status("Could not map PSP memory");
    return -1;
  }

  g_rt.attempted = 1;
  g_rt.game_unsupported = 0;
  g_rt.stop = 0;
  
  g_rt.retry_at_us = 0;
  g_rt.start_pending = 1;
  ra_runtime_lock();
  g_rt.info.ready = 0;
  g_rt.info.unsupported = 0;
  g_rt.info.unsupported_platform = 0;
  g_rt.platform_downgraded = 0;
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status), "Starting session...");
  ra_runtime_unlock();
  sceKernelSignalSema(g_rt.start_sema, 1);
  return 0;
}

void ra_runtime_request_stop(void) {
  /* Refuse reads immediately: the worker may still be between frames. */
  ra_runtime_set_game_memory_valid(0);
  g_rt.paused = 1;
  g_rt.reuse_session = 0;
  g_rt.stop = 1;
}


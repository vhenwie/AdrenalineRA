#include "ra_runtime_lifecycle.h"

#include <string.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "ra_arena.h"
#include "ra_badge.h"
#include "ra_client.h"
#include "ra_credentials.h"
#include "ra_http.h"
#include "ra_notify.h"
#include "ra_offline.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_clock.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_lb_service.h"
#include "ra_shot.h"
#include "ra_runtime_core.h"
#include "ra_runtime_diag.h"
#include "ra_runtime_hold.h"
#include "ra_runtime_load.h"
#include "ra_runtime_memory.h"
#include "ra_runtime_plugins.h"
#include "ra_session.h"
#include "ra_settings.h"

void ra_runtime_shell_transition(void) {
  /* Five seconds covers the PS window, the shell repaint, and the menu/exit
   * decision. Events raised in that window are UI artifacts, not gameplay. */
  g_rt.shell_transition_until_us =
    (unsigned long long)sceKernelGetSystemTimeWide() + 5000000ULL;
}

void ra_runtime_shell_transition_clear(void) {
  g_rt.shell_transition_until_us = 0;
}

/* PS double press: stop the session and restart it from the tick once the game is
 * visible again, with the resume guarded by delay, frame and disc conditions. */
/* Set when a resume has been issued; consumed by the frame loop, which is the only
 * place that can re-point pspemu's display. */

/* Long enough for the shell's LiveArea animation and pspemu's own pause to settle. */
/* Frames of "game is up, shell is down" before the restart is issued (~0.5 s). */

/* Per-session reset: every static that only means something inside one session is
 * cleared here, so the next game cannot inherit it. Credentials, settings, the session
 * ordinal and the badge cache are process state and stay. */
void ra_runtime_reset_per_session_state(const char *why) {
  char cleared[192];
  size_t at = 0;

  cleared[0] = '\0';
#define RA_GUARD_CLEAR_INT(field)                                                 \
  do {                                                                          \
    if ((field) != 0) {                                                       \
      if (at + 26 < sizeof(cleared))                                        \
        at += (size_t)snprintf(cleared + at, sizeof(cleared) - at, "%s ", \
                  #field);                                   \
      (field) = 0;                                                          \
    }                                                                         \
  } while (0)
#define RA_GUARD_CLEAR_STR(field)                                                 \
  do {                                                                          \
    if ((field)[0] != '\0') {                                                 \
      if (at + 26 < sizeof(cleared))                                        \
        at += (size_t)snprintf(cleared + at, sizeof(cleared) - at, "%s ", \
                  #field);                                   \
      (field)[0] = '\0';                                                    \
    }                                                                         \
  } while (0)

  /* Load and network policy. */
  RA_GUARD_CLEAR_INT(g_rt.attempted);
  RA_GUARD_CLEAR_INT(g_rt.offline_at_launch);
  RA_GUARD_CLEAR_INT(g_rt.game_unsupported);
  RA_GUARD_CLEAR_INT(g_rt.load_failures);
  RA_GUARD_CLEAR_INT(g_rt.net_failed);
  /* Disc identity: a forced index, a remembered serial and the media state all
     describe this disc in this PBP. None of them survives a game. */
  RA_GUARD_CLEAR_INT(g_rt.forced_disc_index);
  RA_GUARD_CLEAR_STR(g_rt.forced_disc_path);
  RA_GUARD_CLEAR_STR(g_rt.last_live_serial);
  RA_GUARD_CLEAR_INT(g_rt.media_failures);
  RA_GUARD_CLEAR_INT(g_rt.restart_disc_pending);
  /* `g_rt.disc_banner_pending` is declared further down and is consumed by the tick on
     * the next frame, so it clears itself; listing it here would need a forward
     * declaration for a one-frame flag. */

  if (g_rt.media_state != RA_MEDIA_UNKNOWN) {
    if (at + 18 < sizeof(cleared))
      at += (size_t)snprintf(cleared + at, sizeof(cleared) - at,
                "g_rt.media_state ");
    g_rt.media_state = RA_MEDIA_UNKNOWN;
  }
  ra_runtime_offline_reset();

#undef RA_GUARD_CLEAR_INT
#undef RA_GUARD_CLEAR_STR

  if (cleared[0] != '\0')
    ra_runtime_log("runtime: per-session state reset (%s): %s\n", why, cleared);
}

/* Drives the eject/re-insert transition from the L+R+START harness. Returns 1 when
 * the media is now treated as removed. */
int ra_runtime_debug_media_toggle(void) {
  ra_runtime_init();
  g_rt.dev_media_forced_blank = g_rt.dev_media_forced_blank ? 0 : 1;
  ra_runtime_log("dev: media forced %s by the L+R+START harness\n",
        g_rt.dev_media_forced_blank ? "blank (eject path)"
                    : "readable (re-insert path)");
  return g_rt.dev_media_forced_blank;
}

void ra_runtime_background_stop(void) {
  ra_runtime_init();

  ra_runtime_log("runtime: PS double press; leaving for the LiveArea and stopping the "
        "session (it restarts when the game is visible again)\n");
  ra_runtime_request_stop();
  ra_badge_stop();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  /* Clear the previous session's lists so every view rebuilds. */
  ++g_rt.data_generation;
  g_rt.auto_resume_pending = 1;
  g_rt.auto_resume_frames = 0;
  g_rt.auto_resume_after_us =
    (unsigned long long)sceKernelGetSystemTimeWide() +
    RA_RUNTIME_AUTO_RESUME_DELAY_US;
}

/* Consumed once by the frame loop after a resume to re-point the framebuffer. */
int ra_runtime_consume_display_reassert(void) {
  int wanted = g_rt.display_reassert;

  g_rt.display_reassert = 0;
  return wanted;
}

/* Abort an in-flight game load . Called when the game is lost or the
 * user signs out, so a completion callback cannot land on a client that has been
 * reset underneath it. */
void ra_runtime_abort_load(void) {
  ra_session_t *session = ra_runtime_session_acquire();

  if (session == NULL)
    return;
  ra_session_abort_load(session);
  ra_session_release(session);
  ra_runtime_log("runtime: game load aborted\n");
}

/* Sign-out path : tell the client, not just our own state. Without
 * this the client keeps the user identity until the process ends. */
void ra_runtime_logout(void) {
  ra_session_t *session = ra_runtime_session_acquire();

  if (session == NULL)
    return;
  ra_session_logout(session);
  ra_session_release(session);
  ra_runtime_log("runtime: client logged out\n");
}

/* rcheevos asks clients not to freeze emulation immediately after an unlock; this
 * reports how many frames it still wants . 0 = safe to pause. */
int ra_runtime_can_pause(uint32_t *frames_remaining) {
  ra_session_t *session;
  ra_client_t *client;
  rc_client_t *rc;
  int can_pause;

  if (frames_remaining != NULL)
    *frames_remaining = 0;
  client = ra_runtime_client_acquire(&session);
  if (client == NULL)
    return 1;
  rc = ra_client_get_rc_client(client);
  can_pause = rc == NULL ? 1 : rc_client_can_pause(rc, frames_remaining);
  ra_runtime_client_release(session);
  return can_pause;
}

void ra_runtime_background(void) {
  ra_runtime_init();

  /* The host app is returning to its own LiveArea. Adrenaline only relaunches
     * itself there (menu.c double-PS handler) and does not terminate pspemu, so
     * the game can still be alive when the user comes back. Pause instead of
     * ending the session: tracking resumes on the next tick that sees the game,
     * and if pspemu really went away the identity clears and the normal grace
     * stops the session. */
  g_rt.paused = 1;
  ra_runtime_set_game_memory_valid(0);
  ra_badge_stop();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  ra_runtime_log("runtime: app backgrounded; session paused\n");
}

/* A state was loaded: rcheevos' measured values belong to the old timeline, so
 * reset the runtime exactly like PCSX2 does on state load. */
void ra_runtime_state_loaded(void) {
  ra_session_t *session;

  ra_runtime_init();
  session = ra_runtime_session_acquire();
  if (session == NULL)
    return;
  ra_session_reset(session);
  ra_session_release(session);
  g_rt.dirty = 1;
  ra_runtime_log("runtime: state loaded; achievement runtime reset\n");
}

static void leave_release_world(void) {
  int held_load = g_rt.pause_load;
  int held_menu = g_rt.pause_menu;

  g_rt.pause_load = 0;
  g_rt.pause_menu = 0;
  ra_runtime_log("runtime: exit: releasing world (load=%d menu=%d paused=%d)\n",
                 held_load, held_menu, g_rt.world_paused);
  ra_runtime_update_world_pause("exit");
  ra_runtime_clock_release("exit");
  ra_runtime_release_power_hook();
  ra_badge_shutdown();
}

static void leave_stop_and_wait(unsigned long long now_us) {
  int i;

  g_rt.no_game_since_us = now_us - RA_RUNTIME_NO_GAME_GRACE_US;
  g_rt.no_game_stopped = 1;
  g_rt.paused = 1;
  ra_runtime_set_game_memory_valid(0);
  g_rt.reuse_session = 0;
  g_rt.stop = 1;
  for (i = 0; i < 200 && (g_rt.running || g_rt.start_pending); ++i)
    sceKernelDelayThread(10 * 1000);
  if (g_rt.running || g_rt.start_pending)
    ra_runtime_log("runtime: exit flush timed out after 2000 ms; session stopped\n");
}

static void leave_clear_session(void) {
  ra_shot_flush_copies();
  ra_badge_stop();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  g_rt.attempted = 0;
  g_rt.game_unsupported = 0;
  g_rt.frames = 0;
  g_rt.last_start_check_us = 0;
  g_rt.game_titleid[0] = '\0';
  g_rt.game_filename[0] = '\0';
  g_rt.game_title[0] = '\0';
  ra_runtime_lb_reset();
}

static void leave_clear_info(void) {
  g_rt.info.running = 0;
  g_rt.info.ready = 0;
  g_rt.info.unsupported = 0;
  g_rt.info.unsupported_platform = 0;
  g_rt.info.hardcore = 0;
  g_rt.platform_downgraded = 0;
  g_rt.info.game_id = 0;
  g_rt.info.achievement_count = 0;
  g_rt.info.unlocked_count = 0;
  g_rt.info.points_total = 0;
  g_rt.info.points_unlocked = 0;
  g_rt.info.game_title[0] = '\0';
  g_rt.info.rich_presence[0] = '\0';
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status), "Not in game");
  g_rt.achievement_count = 0;
  g_rt.next_achievement_count = 0;
}

void ra_runtime_leave_game(void) {
  unsigned long long now_us;

  ra_runtime_init();
  now_us = (unsigned long long)sceKernelGetSystemTimeWide();
  leave_release_world();
  leave_stop_and_wait(now_us);
  leave_clear_session();
  ra_runtime_lock();
  leave_clear_info();
  ra_runtime_unlock();
  g_rt.dirty = 0;
  ra_runtime_log("runtime: exit to LiveArea; session stopped immediately\n");
}

void ra_runtime_settings_changed(void) {
  ra_runtime_mark_settings_dirty();
}

/* The pspemu shell publishes its own title as "XMB" whenever no game is loaded -
 * menu.c uses the same test to draw "XMB" in the Adrenaline menu. In that state the
 * emulated RAM belongs to the shell, not to a game, so nothing may be evaluated
 * against it. Checked as well as the app type because the app type flaps during
 * gameplay on multi-EBOOT titles and cannot be trusted alone. */
int ra_runtime_state_in_shell(const SceAdrenaline *adrenaline) {
  return adrenaline != NULL && strcmp(adrenaline->title, "XMB\xE2\x84\xA2") == 0;
}

/* FNV-1a over the identity fields a tick can observe. A change means "something
 * about which game is running just changed" - the only moment the emulated RAM can
 * stop belonging to the game between two ticks. */
unsigned int ra_runtime_state_fingerprint(const SceAdrenaline *adrenaline,
                      int in_shell) {
  unsigned int fp = 2166136261u;

  if (adrenaline == NULL)
    return 0;
  fp = (fp ^ (unsigned int)adrenaline->app_type) * 16777619u;
  fp = (fp ^ (unsigned int)g_rt.title_is_pops) * 16777619u;
  fp = (fp ^ (unsigned int)(in_shell != 0)) * 16777619u;
  fp = (fp ^ (unsigned int)(unsigned char)adrenaline->titleid[0]) * 16777619u;
  fp = (fp ^ (unsigned int)(unsigned char)adrenaline->titleid[1]) * 16777619u;
  fp = (fp ^ (unsigned int)(unsigned char)adrenaline->filename[0]) * 16777619u;
  fp = (fp ^ (unsigned int)(unsigned char)adrenaline->title[0]) * 16777619u;
  return fp;
}

/* Drawn only while a game owns the screen. */
int ra_runtime_overlay_visible(void) {
  return g_rt.in_game && ra_runtime_game_memory_valid();
}

/* 1 when the running title is a PS1 game.
 *
 * Reads the latch, not `adrenaline->pops_mode`: POPS drops that flag back to 0
 * mid-game, and menu.c uses this to keep the direct sharp-scale display path
 * off for PS1 titles (it would otherwise take the framebuffer over and repaint
 * the Vita system overlay away). */
int ra_runtime_title_is_pops(void) {
  return g_rt.title_is_pops;
}

/* The live title latch drops at the XMB transition, but the exit hand-back still
 * needs to know which surface pspemu used. Updated only when a game is probed. */
int ra_runtime_last_title_was_pops(void) {
  return g_rt.last_title_is_pops;
}


/* Badge cache set-up is finished the first time the client publishes a game id
 * . On a POPS title `g_rt.info.game_id` can still be 0 when the load path reaches
 * `ra_badge_set_game()`, and that call early-returns for 0 - leaving the worker writing
 * into `badges/0/`, where five failed writes trip the circuit breaker and the whole
 * session runs with no art. That is the "active badges have no images" report; this runs
 * once per game id and logs the state, so the next POPS log names which of the two it was. */
void ra_runtime_ensure_badge_cache(void) {
  static uint32_t badge_ready_id;
  uint32_t id;

  if (!ra_runtime_session_ready())
    return;
  ra_runtime_lock();
  id = g_rt.info.game_id;
  ra_runtime_unlock();
  if (id == 0 || id == badge_ready_id)
    return;

  badge_ready_id = id;
  ra_badge_set_game(id);
  ra_runtime_prefetch_game_badge();
  ra_runtime_prefetch_likely_badge();
  
  ra_runtime_log("runtime: badge cache organised game_id=%u dir=%d enabled=%d "
        "stalled=%d worker=%d\n", (unsigned)id, ra_badge_cache_dir_ok(),
        ra_badge_enabled(), ra_badge_stalled(), ra_badge_worker_alive());
}

/* The live disc serial lives in PSP RAM at 0x09E80400, not in PS1 address space;
 * see ra_runtime_memory_read_psp(). */

/* Session counter, 1 for the first session in this process. */
int ra_runtime_session_ordinal(void) {
  return g_rt.session_ordinal;
}

/* Only records the request: the tick performs the check on the thread that owns the
 * disc state. */

void ra_runtime_fail(const char *status) { ra_runtime_fail_status(status); }

#include "ra_runtime_hold.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "main.h"
#include "ra_client.h"
#include "ra_runtime_clock.h"
#include "ra_runtime_internal.h"

static SceUID g_hold_lock = -1;

void ra_runtime_hold_init(void) {
  if (g_hold_lock < 0)
    g_hold_lock = sceKernelCreateSema("ra_hold", 0, 1, 1, NULL);
}

static void hold_lock(void) {
  if (g_hold_lock >= 0)
    sceKernelWaitSema(g_hold_lock, 1, NULL);
}

static void hold_unlock(void) {
  if (g_hold_lock >= 0)
    sceKernelSignalSema(g_hold_lock, 1);
}

int ra_runtime_holds_world(void) {
  return g_rt.world_paused;
}

/* Defined below; the first world pause starts it, and the budget tick is shared with the
 * load wait loop. Declared here because the budget tick calls it. */
static int hold_watchdog_thread(SceSize args, void *argp);
void ra_runtime_update_world_pause(const char *reason);

/* Release the load claim when the budget is spent: the parse thread enforces it, the
 * watchdog covers a hold that outlives the loop. */
void ra_runtime_hold_budget_tick(void) {
  if (!g_rt.pause_load || g_rt.hold_started_us == 0)
    return;
  if ((unsigned long long)sceKernelGetSystemTimeWide() - g_rt.hold_started_us <
    (unsigned long long)RA_RUNTIME_HOLD_BUDGET_MS * 1000ULL)
    return;

  ra_runtime_log("runtime: load window budget %u ms reached; game released "
        "(load continues in the background)\n",
        (unsigned)RA_RUNTIME_HOLD_BUDGET_MS);
  /* Only the load's claim: if the menu also holds the world the pause must stay up
     * . The clock scope pops with it, or the parse would keep 444 while the
     * player is playing. */
  g_rt.pause_load = 0;
  ra_runtime_clock_leave(RA_CLOCK_SCOPE_LOAD, "budget");
  ra_runtime_update_world_pause("budget");
}

/* The only sender of PAUSE_WORLD / RESUME_WORLD. One claim per owner, one PAUSE on the
 * first claim, one RESUME when the last is dropped: pentazemin's pause is not re-entrant. */
static void hold_start_watchdog(void) {
  if (g_rt.hold_watchdog_thread >= 0)
    return;
  g_rt.hold_watchdog_thread = sceKernelCreateThread("ra_hold_guard",
          hold_watchdog_thread, 0x10000120, 0x4000, 0, 0, NULL);
  if (g_rt.hold_watchdog_thread < 0)
    g_rt.hold_watchdog_thread = sceKernelCreateThread("ra_hold_guard",
            hold_watchdog_thread, 0x10000100, 0x4000, 0, 0, NULL);
  if (g_rt.hold_watchdog_thread >= 0) {
    sceKernelStartThread(g_rt.hold_watchdog_thread, 0, NULL);
    ra_runtime_log("runtime: hold watchdog started thid=0x%08X\n",
          (unsigned)g_rt.hold_watchdog_thread);
  } else {
    ra_runtime_log("runtime: hold watchdog failed (0x%08X); the load loop "
          "enforces the budget itself\n",
          (unsigned)g_rt.hold_watchdog_thread);
  }
}

void ra_runtime_update_world_pause(const char *reason) {
  int want = g_rt.pause_load || g_rt.pause_menu;

  hold_lock();
  /* POPS is not pentazemin's world: menu.c pauses a PS1 title through
 * ScePspemuPausePops(), so the runtime never claims the world for it. */
  if (g_rt.game_is_pops || g_rt.title_is_pops) {
    g_rt.pause_load = 0;
    g_rt.pause_menu = 0;
    hold_unlock();
    return;
  }

  if (want == g_rt.world_paused) {
    hold_unlock();
    return;
  }

  
  if (want && !g_rt.in_game && !g_rt.running) {
    static int logged;

    if (!logged) {
      logged = 1;
      ra_runtime_log("runtime: world pause not sent (reason=%s, no emulated world yet)\n",
            reason != NULL ? reason : "?");
    }
    g_rt.pause_load = 0;
    g_rt.pause_menu = 0;
    hold_unlock();
    return;
  }

  g_rt.world_paused = want;
  if (want) {
    g_rt.hold_started_us = (unsigned long long)sceKernelGetSystemTimeWide();
    SendAdrenalineRequest(ADRENALINE_PSP_CMD_PAUSE_WORLD);
    hold_start_watchdog();
    ra_runtime_log("runtime: world pause on (reason=%s load=%d menu=%d)\n",
          reason != NULL ? reason : "?", g_rt.pause_load, g_rt.pause_menu);
  } else {
    SendAdrenalineRequest(ADRENALINE_PSP_CMD_RESUME_WORLD);
    ra_runtime_log("runtime: world pause off (reason=%s load=%d menu=%d)\n",
          reason != NULL ? reason : "?", g_rt.pause_load, g_rt.pause_menu);
  }
  hold_unlock();
}

/* The menu's claim. Called from menu.c instead of it sending its own PAUSE/RESUME, so
 * the two owners cannot overlap. */
void ra_runtime_pause_for_menu(void) {
  if (g_rt.pause_menu)
    return;
  g_rt.pause_menu = 1;
  ra_runtime_update_world_pause("menu-open");
  /* The clock scope is taken after the pause is asserted, so `ra_runtime_holds_world()`
     * is already true T1). */
  ra_runtime_clock_enter(RA_CLOCK_SCOPE_MENU, "menu");
}

void ra_runtime_resume_for_menu(void) {
  if (!g_rt.pause_menu)
    return;
  g_rt.pause_menu = 0;
  ra_runtime_clock_leave(RA_CLOCK_SCOPE_MENU, "menu-close");
  ra_runtime_update_world_pause("menu-close");
}

static int hold_watchdog_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  for (;;) {
    ra_runtime_hold_budget_tick();
    sceKernelDelayThread(50 * 1000);
  }
  return 0;
}

void ra_runtime_hold_world(void) {
  if (g_rt.pause_load)
    return;
  g_rt.pause_load = 1;
  ra_runtime_log("runtime: game held while the session loads\n");
  ra_runtime_update_world_pause("load");
  /* The parse is the heaviest work we do, and it is safe to speed up here because the
     * emulated world is paused T1). */
  ra_runtime_clock_enter(RA_CLOCK_SCOPE_LOAD, "load");
}

void ra_runtime_release_world(void) {
  if (!g_rt.pause_load)
    return;
  g_rt.pause_load = 0;
  ra_runtime_log("runtime: releasing game\n");
  ra_runtime_clock_leave(RA_CLOCK_SCOPE_LOAD, "load-done");
  ra_runtime_update_world_pause("load-done");
  ra_runtime_log("runtime: game resumed\n");
}

/* Stop-path cleanup: a session can end without the exit path, so the same holds are
 * released here. */
void ra_runtime_hold_release_all(void) {
  int held_load = g_rt.pause_load;
  int held_menu = g_rt.pause_menu;

  g_rt.pause_load = 0;
  g_rt.pause_menu = 0;
  ra_runtime_log("runtime: session stop cleanup (load=%d menu=%d paused=%d)\n",
        held_load, held_menu, g_rt.world_paused);
  if (g_rt.memory_zero_fills > 0) {
    /* Expected during an evaluation hold: reads return zeros, not an error. */
    ra_runtime_log("runtime: %u read(s) served as zeros while evaluation was "
          "suspended this session\n", g_rt.memory_zero_fills);
  }
  ra_runtime_update_world_pause("session-stop");
  /* The badge worker is deliberately *not *shut down here: a join would be abandoned
     * while a fetch is in flight, and an abandoned worker plus the next session's worker
     * would both drain one queue. It stays parked (`ra_badge_stop()` above) and
     * `ra_badge_init()` resumes it for the next session. */
}

static volatile unsigned long long g_eval_hold_until_us;
static volatile int g_game_memory_valid;
static volatile int g_disc_switch_hold;

int ra_runtime_game_memory_valid(void) { return g_game_memory_valid; }
void ra_runtime_set_game_memory_valid(int valid) { g_game_memory_valid = valid != 0; }
int ra_runtime_disc_switch_hold(void) { return g_disc_switch_hold; }
unsigned long long ra_runtime_eval_hold_until(void) { return g_eval_hold_until_us; }
void ra_runtime_set_eval_hold_until(unsigned long long until) {
  g_eval_hold_until_us = until;
}

int ra_runtime_eval_suspended(void) {
  unsigned long long now_us = (unsigned long long)sceKernelGetSystemTimeWide();

  if (!ra_runtime_game_memory_valid())
    return 1;
  if (ra_runtime_disc_switch_hold())
    return 1;
  if (ra_runtime_eval_hold_until() != 0 && now_us < ra_runtime_eval_hold_until())
    return 1;
  return 0;
}
void ra_runtime_set_eval_hold(int hold) {
  ra_session_t *session;

  g_disc_switch_hold = hold != 0;
  session = ra_runtime_session_acquire();
  if (session != NULL) {
    ra_client_set_eval_hold(ra_session_get_client(session),
                g_disc_switch_hold);
    ra_session_release(session);
  }
}

/* Split of the load cost: transport, cache and parse. */


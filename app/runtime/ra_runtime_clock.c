#include "ra_runtime_clock.h"
#include "ra_runtime_internal.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "ra_runtime.h"

int ra_runtime_in_game(void);

#define RA_RUNTIME_CLOCK_SCOPED_MHZ 444
#define RA_RUNTIME_CLOCK_REASSERT_US 250000ULL
#define RA_RUNTIME_CLOCK_SCOPE_MAX_US 5000000ULL

static int g_clock_scope;              /* bitmask of active claims */
static int g_clock_applied;            /* the scoped clock is currently set */
static int g_clock_base_mhz;           /* what to hand back on release */
static unsigned long long g_clock_scope_started_us;
static unsigned long long g_clock_last_assert_us;
static int g_clock_reasserts;          /* pspemu took the clock back N times this scope */
static int g_clock_refused_logged;

/* The clock scope is always on: it only runs while no game code executes. */
int ra_runtime_clock_allowed(void) {
  return ra_runtime_in_game();
}

void ra_runtime_clock_apply(const char *reason) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  int current = scePowerGetArmClockFrequency();

  if (!g_clock_applied)
    g_clock_base_mhz = current;
  if (current == RA_RUNTIME_CLOCK_SCOPED_MHZ) {
    /* already there: pspemu has not taken it back yet */
    g_clock_last_assert_us = now;
    g_clock_applied = 1;
    return;
  }

  if (scePowerSetArmClockFrequency(RA_RUNTIME_CLOCK_SCOPED_MHZ) < 0) {
    ra_runtime_log("runtime: clock scope refused by the system (was %d MHz)\n", current);
    return;
  }

  g_clock_last_assert_us = now;
  if (!g_clock_applied) {
    g_clock_applied = 1;
    ra_runtime_log("runtime: clock scope %d MHz (reason=%s was=%d)\n",
          RA_RUNTIME_CLOCK_SCOPED_MHZ, reason != NULL ? reason : "?",
          current);
  } else if (++g_clock_reasserts == 1) {
    /* once per scope is enough to show that pspemu re-asserts its own clock */
    ra_runtime_log("runtime: clock scope re-applied after pspemu reset it (was %d MHz)\n",
          current);
  }
}

void ra_runtime_clock_release(const char *reason) {
  int back;

  g_clock_scope = 0;
  if (!g_clock_applied)
    return;

  g_clock_applied = 0;
  back = g_clock_base_mhz > 0 ? g_clock_base_mhz : 333;
  if (scePowerGetArmClockFrequency() != back)
    (void)scePowerSetArmClockFrequency(back);
  ra_runtime_log("runtime: clock scope released (reason=%s back=%d)\n",
        reason != NULL ? reason : "?", back);
}

/* Claims are only taken while the emulated world is paused. If a caller gets this wrong the
 * request is dropped and logged, so the clock can never be raised under live gameplay. */
void ra_runtime_clock_enter(int scope, const char *reason) {
  if (g_clock_scope & scope)
    return;

  if (!ra_runtime_clock_allowed())
    return;

  if (!ra_runtime_holds_world()) {
    if (!g_clock_refused_logged) {
      g_clock_refused_logged = 1;
      ra_runtime_log("runtime: clock scope refused (reason=%s, the world is running)\n",
            reason != NULL ? reason : "?");
    }
    return;
  }

  if (g_clock_scope == 0) {
    g_clock_scope_started_us = (unsigned long long)sceKernelGetSystemTimeWide();
    g_clock_reasserts = 0;
  }
  g_clock_scope |= scope;
  ra_runtime_clock_apply(reason);
}

void ra_runtime_clock_leave(int scope, const char *reason) {
  if (!(g_clock_scope & scope))
    return;

  g_clock_scope &= ~scope;
  if (g_clock_scope == 0)
    ra_runtime_clock_release(reason);
}

/* Called once per frame from `runtime_run`. Three jobs: hand the clock back if the world is
 * running again, re-assert it while pspemu keeps resetting it, and refuse to hold it forever. */
void ra_runtime_clock_tick(void) {
  unsigned long long now;

  if (g_clock_scope == 0)
    return;

  now = (unsigned long long)sceKernelGetSystemTimeWide();
  if (now - g_clock_scope_started_us >= RA_RUNTIME_CLOCK_SCOPE_MAX_US) {
    ra_runtime_log("runtime: clock scope held for %u ms; releasing\n",
          (unsigned)(RA_RUNTIME_CLOCK_SCOPE_MAX_US / 1000ULL));
    ra_runtime_clock_release("cap");
    return;
  }

  if (!ra_runtime_holds_world()) {
    ra_runtime_log("runtime: clock scope released (reason=world-running)\n");
    ra_runtime_clock_release("world-running");
    return;
  }

  if (now - g_clock_last_assert_us >= RA_RUNTIME_CLOCK_REASSERT_US)
    ra_runtime_clock_apply("reassert");
}


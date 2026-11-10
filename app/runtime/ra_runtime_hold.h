#ifndef RA_RUNTIME_HOLD_H
#define RA_RUNTIME_HOLD_H

/* One PAUSE_WORLD per owner (load, menu); RESUME_WORLD is sent when the last claim
 * is dropped. POPS sessions are never claimed here: menu.c owns ScePspemuPausePops. */
void ra_runtime_hold_world(void);
void ra_runtime_release_world(void);
void ra_runtime_hold_budget_tick(void);
void ra_runtime_update_world_pause(const char *reason);

/* Drops every hold and reports one line. Runs on the session stop path. */
void ra_runtime_hold_release_all(void);

#endif /* RA_RUNTIME_HOLD_H */

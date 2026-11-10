#ifndef RA_RUNTIME_CLOCK_H
#define RA_RUNTIME_CLOCK_H

enum {
  RA_CLOCK_SCOPE_LOAD = 1,
  RA_CLOCK_SCOPE_MENU = 2
};

void ra_runtime_clock_enter(int scope, const char *reason);
void ra_runtime_clock_leave(int scope, const char *reason);
void ra_runtime_clock_tick(void);
void ra_runtime_clock_release(const char *reason);
int ra_runtime_clock_allowed(void);
int ra_runtime_in_game(void);

#endif

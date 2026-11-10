#ifndef RA_RUNTIME_LIFECYCLE_H
#define RA_RUNTIME_LIFECYCLE_H

#include "ra_runtime.h"

/* Tick-side identity helpers. */
int ra_runtime_state_in_shell(const SceAdrenaline *adrenaline);
unsigned int ra_runtime_state_fingerprint(const SceAdrenaline *adrenaline,
                                          int in_shell);
void ra_runtime_reset_per_session_state(const char *why);

/* Prepares the running game's badge key and prefetches its art. */
void ra_runtime_ensure_badge_cache(void);

#endif /* RA_RUNTIME_LIFECYCLE_H */

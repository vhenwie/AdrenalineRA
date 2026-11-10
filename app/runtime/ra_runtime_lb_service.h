#ifndef RA_RUNTIME_LB_SERVICE_H
#define RA_RUNTIME_LB_SERVICE_H

#include <stddef.h>
#include "ra_runtime.h"

void ra_runtime_lb_reset(void);
/* Score validator shared with the rcheevos event handler. */
int ra_runtime_leaderboard_validate_impl(uint32_t id, int score);
void ra_runtime_lb_service(void);
int ra_runtime_leaderboard_entries_state(void);
int ra_runtime_copy_leaderboard_entries_error(char *out, size_t capacity);
int ra_runtime_copy_leaderboard_entry_at(int index, RaLeaderboardEntryView *out);

#endif

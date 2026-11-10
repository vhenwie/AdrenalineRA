#ifndef RA_RUNTIME_EVENTS_H
#define RA_RUNTIME_EVENTS_H

#include <stdint.h>

#include "rc_client.h"

/* rcheevos event handler installed on the session client. */
void ra_runtime_events_dispatch(const rc_client_event_t *event, rc_client_t *rc);

/* rcheevos leaderboard submission validator. Returns 0 to reject a score. */
int ra_runtime_leaderboard_validate(uint32_t id, int score);

#endif /* RA_RUNTIME_EVENTS_H */

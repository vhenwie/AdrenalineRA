#ifndef RA_RUNTIME_EVENTS_LB_H
#define RA_RUNTIME_EVENTS_LB_H

#include "rc_client.h"

/* Leaderboard events: tracking indicators, start, submit, failure and scoreboard. */
void ra_runtime_events_dispatch_leaderboard(const rc_client_event_t *event,
                                            rc_client_t *rc);

#endif /* RA_RUNTIME_EVENTS_LB_H */

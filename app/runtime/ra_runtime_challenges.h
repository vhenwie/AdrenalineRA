#ifndef RA_RUNTIME_CHALLENGES_H
#define RA_RUNTIME_CHALLENGES_H

#include <stdint.h>
#include "rc_client.h"
#include "ra_runtime.h"

void ra_runtime_active_challenge_add(const rc_client_achievement_t *achievement);
void ra_runtime_active_challenge_remove(uint32_t id);
void ra_runtime_active_challenge_refresh(const rc_client_achievement_t *achievement);
void ra_runtime_active_challenges_reset(void);
void ra_runtime_mark_data_changed(void);
int ra_runtime_active_challenge_count(void);
int ra_runtime_copy_active_challenges(RaActiveChallengeView *out, int max,
                                      int *total);

#endif

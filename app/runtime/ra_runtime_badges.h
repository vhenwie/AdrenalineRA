#ifndef RA_RUNTIME_BADGES_H
#define RA_RUNTIME_BADGES_H

#include <stddef.h>
#include "rc_client.h"

int ra_runtime_badge_key(const rc_client_achievement_t *achievement,
                         char *key, size_t key_capacity, char *url,
                         size_t url_capacity);
void ra_runtime_prefetch_badge_for(const rc_client_achievement_t *achievement,
                                   int high_priority);
void ra_runtime_prefetch_likely_badge(void);
void ra_runtime_prefetch_next_badge(const rc_client_achievement_t *achievement);

#endif

#ifndef RA_BADGE_STATE_H
#define RA_BADGE_STATE_H

#include <stddef.h>
#include <stdint.h>

void ra_badge_set_game(uint32_t game_id);
int ra_badge_path(const char *key, char *out, size_t capacity);
int ra_badge_cache_dir_ok(void);

#endif

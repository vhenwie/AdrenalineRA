#ifndef RA_RUNTIME_HASHLIB_H
#define RA_RUNTIME_HASHLIB_H

#include <stdint.h>

int ra_runtime_hash_library_game_id(const char *hash, uint32_t *game_id);
void ra_runtime_maybe_fetch_hash_library(void);
void ra_runtime_hashlib_reset(void);
int ra_runtime_hash_library_count(void);

#endif

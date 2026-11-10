#ifndef RA_BADGE_TEXTURE_H
#define RA_BADGE_TEXTURE_H

#include <vita2d.h>

int ra_badge_texture_memory_cached(const char *key);
void ra_badge_prime_queue(const char *key);
void ra_badge_frame_begin(void);
vita2d_texture *ra_badge_texture(const char *key);
void ra_badge_prime_step(void);
void ra_badge_log(const char *format, ...);

#endif

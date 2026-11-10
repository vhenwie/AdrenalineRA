/*
 * ra_badge_key.h -- filesystem-safe badge cache keys.
 */

#ifndef RA_BADGE_KEY_H
#define RA_BADGE_KEY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_BADGE_KEY_MAX 16

/* Builds a filesystem-safe badge cache key from an rcheevos badge name,
 * falling back to the PNG filename in the badge URL. Returns 1 on success. */
int ra_badge_make_key(const char *badge_name, const char *url, char *out,
                      size_t capacity);

/* Returns 1 when a cache key is safe to use as a filename. */
int ra_badge_key_valid(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* RA_BADGE_KEY_H */

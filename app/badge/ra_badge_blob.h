#ifndef RA_BADGE_BLOB_H
#define RA_BADGE_BLOB_H

#include <stddef.h>
#include "ra_badge.h"

#define RA_BADGE_BLOB_MAX 12

typedef struct {
  char key[RA_BADGE_KEY_MAX];
  unsigned char *data;
  size_t size;
} RaBadgeBlob;

void ra_badge_lock(void);
void ra_badge_unlock(void);
int ra_badge_blob_store(const char *key, const unsigned char *data, size_t size);
int ra_badge_blob_take(const char *key, unsigned char **data, size_t *size);
int ra_badge_blob_has(const char *key);

#endif

#include "ra_badge_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

static RaBadgeBlob g_blobs[RA_BADGE_BLOB_MAX];
static SceUID g_lock = -1;

void ra_badge_lock(void) {
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_badge", 0, 1, 1, NULL);
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

void ra_badge_unlock(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

int ra_badge_blob_store(const char *key, const unsigned char *data, size_t size) {
  unsigned char *copy;
  int slot = -1;
  int i;

  if (key == NULL || data == NULL || size == 0)
    return 0;
  copy = (unsigned char *)malloc(size);
  if (copy == NULL)
    return 0;
  memcpy(copy, data, size);
  ra_badge_lock();
  for (i = 0; i < RA_BADGE_BLOB_MAX; ++i) {
    if (g_blobs[i].data == NULL) { slot = i; break; }
  }
  if (slot < 0) { slot = 0; free(g_blobs[slot].data); }
  snprintf(g_blobs[slot].key, sizeof(g_blobs[slot].key), "%s", key);
  g_blobs[slot].data = copy;
  g_blobs[slot].size = size;
  ra_badge_unlock();
  return 1;
}

int ra_badge_blob_take(const char *key, unsigned char **data, size_t *size) {
  int i;

  if (key == NULL || data == NULL || size == NULL)
    return 0;
  ra_badge_lock();
  for (i = 0; i < RA_BADGE_BLOB_MAX; ++i) {
    if (g_blobs[i].data != NULL && strcmp(g_blobs[i].key, key) == 0) {
      *data = g_blobs[i].data;
      *size = g_blobs[i].size;
      g_blobs[i].data = NULL;
      g_blobs[i].size = 0;
      g_blobs[i].key[0] = '\0';
      ra_badge_unlock();
      return 1;
    }
  }
  ra_badge_unlock();
  return 0;
}

int ra_badge_blob_has(const char *key) {
  int i, found = 0;

  ra_badge_lock();
  for (i = 0; i < RA_BADGE_BLOB_MAX; ++i) {
    if (g_blobs[i].data != NULL && strcmp(g_blobs[i].key, key) == 0) {
      found = 1;
      break;
    }
  }
  ra_badge_unlock();
  return found;
}

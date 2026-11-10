#include "ra_badge_state.h"
#include "ra_badge_internal.h"
#include "ra_badge.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

static uint32_t g_game_id;

void ra_badge_set_game(uint32_t game_id) {
  if (g_game_id == game_id)
    return;
  g_game_id = game_id;
  /* The LRU is keyed by badge name, which is unique per achievement, so stale
     * entries simply age out rather than being freed from an arbitrary thread. */ {
    char folder[80];

    sceIoMkdir(RA_BADGE_DIR, 0777);
    snprintf(folder, sizeof(folder), "%s/%u", RA_BADGE_DIR,
        (unsigned)game_id);
    sceIoMkdir(folder, 0777);
  }
}

int ra_badge_path(const char *key, char *out, size_t capacity) {
  if (!ra_badge_key_valid(key) || out == NULL || capacity == 0)
    return 0;
  if (snprintf(out, capacity, "%s/%u/%s.png", RA_BADGE_DIR,
        (unsigned)g_game_id, key) >= (int)capacity)
    return 0;
  return 1;
}

int ra_badge_cache_dir_ok(void) {
  SceIoStat stat;

  if (sceIoGetstat(RA_BADGE_DIR, &stat) < 0)
    return 0;
  return (stat.st_mode & SCE_S_IFDIR) != 0;
}

#define RA_BADGE_MAX_BYTES (128 * 1024)

int ra_badge_exists(const char *key) {
  char path[96];
  SceIoStat stat;

  if (!ra_badge_path(key, path, sizeof(path)))
    return 0;
  return sceIoGetstat(path, &stat) >= 0 && stat.st_size > 0 &&
    stat.st_size <= RA_BADGE_MAX_BYTES;
}
int ra_badge_cached(const char *key) {
  if (!ra_badge_enabled() || !ra_badge_key_valid(key))
    return 0;
  return ra_badge_exists(key);
}

#define RA_BADGE_MISS_SLOTS 16
#define RA_BADGE_MISS_TTL_US (3ULL * 1000ULL * 1000ULL)
static struct {
  char key[RA_BADGE_KEY_MAX];
  unsigned long long until_us;
} g_badge_miss[RA_BADGE_MISS_SLOTS];
static int g_badge_miss_next;

int ra_badge_missing_recently(const char *key) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  int i;

  for (i = 0; i < RA_BADGE_MISS_SLOTS; ++i) {
    if (g_badge_miss[i].key[0] != '\0' &&
      strcmp(g_badge_miss[i].key, key) == 0)
      return now < g_badge_miss[i].until_us;
  }
  return 0;
}

void ra_badge_note_missing(const char *key) {
  snprintf(g_badge_miss[g_badge_miss_next].key, RA_BADGE_KEY_MAX, "%s", key);
  g_badge_miss[g_badge_miss_next].until_us =
    (unsigned long long)sceKernelGetSystemTimeWide() + RA_BADGE_MISS_TTL_US;
  g_badge_miss_next = (g_badge_miss_next + 1) % RA_BADGE_MISS_SLOTS;
}

/* A fetched PNG invalidates a negative-cache entry left by an earlier miss. */
void ra_badge_note_present(const char *key) {
  int i;

  for (i = 0; i < RA_BADGE_MISS_SLOTS; ++i) {
    if (g_badge_miss[i].key[0] != '\0' &&
      strcmp(g_badge_miss[i].key, key) == 0)
      g_badge_miss[i].until_us = 0;
  }
}

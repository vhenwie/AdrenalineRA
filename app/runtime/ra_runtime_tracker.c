#include "ra_runtime_tracker.h"
#include "ra_runtime_core.h"

#include <stdint.h>
#include <string.h>

#define RA_TRACKER_CACHE_MAX 8
#define RA_TRACKER_TEXT_MAX 32

static struct {
  uint32_t id;
  char text[RA_TRACKER_TEXT_MAX];
  int used;
} g_tracker_cache[RA_TRACKER_CACHE_MAX];

/* 1 when the value differs from the one already on screen (or is new), so the
 * caller should forward it; 0 when it is the same text and can be skipped. */
int ra_runtime_tracker_update(uint32_t id, const char *display) {
  int free_slot = -1;
  int i;

  if (id == 0 || display == NULL)
    return 0;
  for (i = 0; i < RA_TRACKER_CACHE_MAX; ++i) {
    if (!g_tracker_cache[i].used) {
      if (free_slot < 0)
        free_slot = i;
      continue;
    }
    if (g_tracker_cache[i].id == id) {
      if (strcmp(g_tracker_cache[i].text, display) == 0)
        return 0;
      ra_runtime_copy_text(g_tracker_cache[i].text, sizeof(g_tracker_cache[i].text),
          display);
      return 1;
    }
  }
  if (free_slot < 0)
    free_slot = 0;
  g_tracker_cache[free_slot].id = id;
  g_tracker_cache[free_slot].used = 1;
  ra_runtime_copy_text(g_tracker_cache[free_slot].text,
      sizeof(g_tracker_cache[free_slot].text), display);
  return 1;
}

/* Drops the remembered value for one tracker, so a later SHOW with the same text is
 * not mistaken for a repeat. */
void ra_runtime_tracker_forget(uint32_t id) {
  int i;

  for (i = 0; i < RA_TRACKER_CACHE_MAX; ++i) {
    if (g_tracker_cache[i].used && g_tracker_cache[i].id == id)
      memset(&g_tracker_cache[i], 0, sizeof(g_tracker_cache[i]));
  }
}

void ra_runtime_tracker_reset(void) {
  memset(g_tracker_cache, 0, sizeof(g_tracker_cache));
}


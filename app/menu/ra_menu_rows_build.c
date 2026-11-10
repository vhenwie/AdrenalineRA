#include "ra_menu_rows_build.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "ra_menu_internal.h"
#include "ra_menu_achievements.h"
#include "ra_menu_entries.h"
#include "ra_menu_rows.h"
#include "ra_menu_settings.h"

#define RA_ACHIEVEMENT_ROW_CAPACITY RA_RUNTIME_MAX_ACHIEVEMENTS

static char g_achievement_rows[RA_ACHIEVEMENT_ROW_CAPACITY][160];
static char g_achievement_tooltips[RA_ACHIEVEMENT_ROW_CAPACITY][RA_RUNTIME_DESCRIPTION_MAX];

static uint32_t achievement_view_fingerprint(const RaAchievementView *view) {
  const unsigned char *bytes;
  uint32_t hash = 2166136261u;
  size_t i;

#define RA_HASH_BYTES(pointer, length)                                     \
  do {                                                                   \
    bytes = (const unsigned char*)(pointer);                           \
    for (i = 0; i < (length); ++i) {                                   \
      hash ^= bytes[i];                                              \
      hash *= 16777619u;                                             \
    }                                                                  \
  } while (0)

  RA_HASH_BYTES(&view->points, sizeof(view->points));
  RA_HASH_BYTES(&view->unlocked, sizeof(view->unlocked));
  RA_HASH_BYTES(&view->unlocked_hardcore, sizeof(view->unlocked_hardcore));
  RA_HASH_BYTES(&view->type, sizeof(view->type));
  RA_HASH_BYTES(&view->unofficial, sizeof(view->unofficial));
  RA_HASH_BYTES(&view->subset_id, sizeof(view->subset_id));
  RA_HASH_BYTES(view->title, sizeof(view->title));
  RA_HASH_BYTES(view->description, sizeof(view->description));
  RA_HASH_BYTES(view->progress, sizeof(view->progress));

#undef RA_HASH_BYTES

  return hash;
}

void ra_menu_add_achievement_row(const RaAchievementView *view, int slot) {
  static uint32_t built_fingerprint[RA_ACHIEVEMENT_ROW_CAPACITY];
  static unsigned char built_valid[RA_ACHIEVEMENT_ROW_CAPACITY];
  uint32_t fingerprint;
  uint32_t color;

  if (slot < 0 || slot >= RA_ACHIEVEMENT_ROW_CAPACITY)
    return;

  /* Gold marks a hardcore unlock; a casual-only unlock stays green. */
  color = view->unlocked
        ? (view->unlocked_hardcore ? GOLD : GREEN)
        : WHITE;

  fingerprint = achievement_view_fingerprint(view);
  if (built_valid[slot] && built_fingerprint[slot] == fingerprint) {
    ra_menu_set_prefix_marker(view->type, view->unofficial);
    ra_menu_add_callback_arg(g_achievement_rows[slot],
              ra_menu_achievements_toggle_pin,
              (void *)(uintptr_t)view->id, color,
              g_achievement_tooltips[slot]);
    return;
  }
  built_fingerprint[slot] = fingerprint;
  built_valid[slot] = 1;

  /* Marker, title, measured progress; unlock state stays in the colour. */
  ra_menu_format_achievement(g_achievement_rows[slot],
              sizeof(g_achievement_rows[slot]), view->type,
              view->unofficial, view->title, view->progress);
  ra_menu_format_tip(g_achievement_tooltips[slot],
              sizeof(g_achievement_tooltips[slot]), view->type,
              view->unofficial, view->description, view->points);
  ra_menu_set_prefix_marker(view->type, view->unofficial);
  ra_menu_add_callback_arg(g_achievement_rows[slot],
            ra_menu_achievements_toggle_pin,
            (void *)(uintptr_t)view->id, color,
            g_achievement_tooltips[slot]);
}

void ra_menu_copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  if (source == NULL)
    source = "";
  snprintf(destination, capacity, "%s", source);
}
void ra_menu_zero_memory(void *data, size_t length) {
  volatile unsigned char *bytes = (volatile unsigned char*)data;
  while (length-- > 0)
        *bytes++ = 0;
}

const char *ra_menu_points_word(unsigned int points) {
  return points == 1 ? "Point" : "Points";
}
const char *ra_menu_points_word_lower(unsigned int points) {
  return points == 1 ? "point" : "points";
}

#define RA_FIT_TEXT_CACHE 256

void ra_menu_fit_text(char *text, size_t capacity, float max_width,
          float scale) {
  static char cached_input[RA_FIT_TEXT_CACHE];
  static char cached_output[RA_FIT_TEXT_CACHE];
  static float cached_scale;
  static float cached_max_width;

  if (text == NULL || capacity == 0)
    return;

  if (cached_scale == scale && cached_max_width == max_width &&
    strcmp(cached_input, text) == 0) {
    snprintf(text, capacity, "%s", cached_output);
    return;
  }

  snprintf(cached_input, sizeof(cached_input), "%s", text);
  while (strlen(text) > 1 &&
    vita2d_pgf_text_width(font, scale, text) > max_width)
    text[strlen(text) - 1] = '\0';
  snprintf(cached_output, sizeof(cached_output), "%s", text);
  cached_scale = scale;
  cached_max_width = max_width;
}

static char g_next_line[RA_RUNTIME_NEXT_MAX][176];
static char g_next_tip[RA_RUNTIME_NEXT_MAX][200];

void ra_menu_achievement_add_next_rows(const RaRuntimeInfo *info) {
  static RaNextAchievementView next[RA_RUNTIME_NEXT_MAX];
  static const char *const labels[RA_RUNTIME_NEXT_MAX] = {
    "Next", "Then", "Afterward"
  };
  int want;
  int got;
  int shown = 0;
  int n;

  if (!info->ready || ra_menu_settings()->next_achievement <= 0)
    return;
  want = ra_menu_settings()->next_achievement >= 2 ? RA_RUNTIME_NEXT_MAX : 1;
  got = ra_runtime_next_achievements(next, want);

  /* Two dividers: without them three extra rows
       * read as part of the summary line above. The rows reuse the achievement
       * page's own naming - type marker first, points only in the tooltip - so
       * nothing new has to be learned to read them. */
  for (n = 0; n < got; ++n) {
    if (next[n].title[0] == '\0')
      continue;
    if (shown == 0)
      ra_menu_add_text("--- Next Achievements ---", LITEGRAY);
    ra_menu_format_next(g_next_line[n], sizeof(g_next_line[n]), labels[n],
            next[n].type, next[n].unofficial, next[n].title);
    ra_menu_format_tip(g_next_tip[n], sizeof(g_next_tip[n]),
              next[n].type, next[n].unofficial,
              next[n].description, next[n].points);
    ra_menu_set_prefix_marker(next[n].type, next[n].unofficial);
    ra_menu_add_text_tip(g_next_line[n], WHITE, g_next_tip[n]);
    ++shown;
  }
  if (shown > 0)
    ra_menu_add_text("--- Achievements ---", LITEGRAY);
}

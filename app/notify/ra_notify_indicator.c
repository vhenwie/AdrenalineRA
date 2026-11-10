#include "ra_notify_indicator.h"
#include <stdio.h>
#include "ra_notify.h"
#include "ra_notify_gfx.h"
#include "ra_notify_internal.h"

#include <string.h>

#include <psp2/kernel/processmgr.h>

#include <vita2d.h>

#include "menu.h"
#include "ra_badge.h"
#include "ra_text.h"

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source != NULL ? source : "");
}


#define RA_INDICATOR_MAX 6
#define RA_INDICATOR_TEXT_MAX 80
#define RA_INDICATOR_PROGRESS_MAX 24
/* PPSSPP keeps its OSD entries for 0.1 s in / 0.25 s out (Common/System/OSD.h:
 * FadeinTime/FadeoutTime); matching those makes the tiles enter and leave at the
 * same speed as a PPSSPP session rather than a beat slower. */
#define RA_INDICATOR_FADE_IN_US 100000ULL
#define RA_INDICATOR_FADE_OUT_US 250000ULL
#define RA_INDICATOR_TILE_SMALL 36.0f
#define RA_INDICATOR_TILE_STANDARD 48.0f
#define RA_INDICATOR_TILE_LARGE 60.0f
#define RA_INDICATOR_GAP 6.0f
#define RA_INDICATOR_EDGE_PAD 12.0f
#define RA_INDICATOR_CAPTION_SCALE 0.92f
#define RA_INDICATOR_CAPTION_MAX_W 170.0f
#define RA_INDICATOR_CAPTION_PAD 4.0f
#define RA_INDICATOR_TRACKER_H 30.0f
#define RA_INDICATOR_TRACKER_TEXT_MAX_W 150.0f

#define RA_INDICATOR_QUALIFY_US 150000ULL
/* Slide distance for the reveal: the tile (and its caption) starts this far
 * "outside" the anchored edge and settles onto it over the fade-in, which is what
 * makes a corner badge noticed without making it loud (DuckStation animates the
 * box the same way). */
#define RA_INDICATOR_SLIDE_PX 6.0f

typedef struct {
  int kind;
  uint32_t id;
  int used;
  /* 1 while the state holds, 0 while fading out. The entry is freed by the
     * draw once the fade-out has finished. */
  int active;
  unsigned long long change_us;
  /* When the fade-in may start; anything before this is drawn as alpha 0. */
  unsigned long long reveal_us;
  char text[RA_INDICATOR_TEXT_MAX];
  char progress[RA_INDICATOR_PROGRESS_MAX];
  char badge_key[RA_BADGE_KEY_MAX];
} RaIndicator;

static RaIndicator g_indicators[RA_INDICATOR_MAX];
static int g_indicator_anchor = 5;
static int g_indicator_size = 1;

static float indicator_alpha(const RaIndicator *indicator,
              unsigned long long now);

void ra_notify_set_indicator_position(int anchor) {
  g_indicator_anchor = (anchor >= 0 && anchor <= 5) ? anchor : 5;
}

void ra_notify_set_indicator_size(int size) {
  g_indicator_size = (size >= 0 && size <= 2) ? size : 1;
}

void ra_notify_indicator_set(int kind, uint32_t id, const char *text) {
  ra_notify_indicator_set_detail(kind, id, text, NULL, NULL);
}

/* Rejects an unknown kind and an indicator with nothing to show. */
static int indicator_set_valid(int kind, const char *text, const char *progress,
    const char *badge_key) {
  if (kind < RA_INDICATOR_CHALLENGE || kind > RA_INDICATOR_TRACKER)
    return 0;
  if ((text == NULL || text[0] == '\0') &&
    (progress == NULL || progress[0] == '\0') &&
    (badge_key == NULL || badge_key[0] == '\0'))
    return 0;
  return 1;
}

/* Caller holds the notify lock. Picks the slot to fill: the singleton PROGRESS
 * entry first, then a same-(kind,id) entry, then a free slot, then one that is
 * already fading out. */
static int indicator_pick_slot(int kind, uint32_t id) {
  int free_slot = -1;
  int fading_slot = -1;
  int slot = -1;
  int i;

  /* A progress indicator is a singleton, like PPSSPP's: it keeps one
     * `ACHIEVEMENT_PROGRESS` entry and a new achievement *takes that entry over*
     * (`Common/System/OSD.cpp:151-183` - "there can only be one of these at a
     * time"), so a busy game cannot fill the row with badges. Reusing the slot
     * also keeps its fade state, so the artwork swaps at full opacity instead of
     * pulsing when one measured value hands over to another. */
  if (kind == RA_INDICATOR_PROGRESS) {
    for (i = 0; i < RA_INDICATOR_MAX; ++i) {
      if (!g_indicators[i].used || g_indicators[i].kind != kind)
        continue;
      if (slot < 0)
        slot = i;
      else
        memset(&g_indicators[i], 0, sizeof(g_indicators[i]));
    }
  }

  if (slot < 0) {
    for (i = 0; i < RA_INDICATOR_MAX; ++i) {
      if (g_indicators[i].used && g_indicators[i].kind == kind &&
        g_indicators[i].id == id) {
        slot = i;
        break;
      }
      if (!g_indicators[i].used && free_slot < 0)
        free_slot = i;
      else if (g_indicators[i].used && !g_indicators[i].active &&
          fading_slot < 0)
        fading_slot = i;
    }
    if (slot < 0)
      slot = (free_slot >= 0) ? free_slot
                  : (fading_slot >= 0 ? fading_slot : 0);
  }
  return slot;
}

/* Caller holds the notify lock. Only a new or ended indicator restarts the fade. */
static void indicator_store(int slot, int kind, uint32_t id, const char *text,
    const char *progress, const char *badge_key, unsigned long long now) {
  if (!g_indicators[slot].used || !g_indicators[slot].active) {
    g_indicators[slot].change_us = now;
    g_indicators[slot].reveal_us = now + RA_INDICATOR_QUALIFY_US;
  }
  g_indicators[slot].used = 1;
  g_indicators[slot].active = 1;
  g_indicators[slot].kind = kind;
  g_indicators[slot].id = id;
  copy_text(g_indicators[slot].text, sizeof(g_indicators[slot].text), text);
  copy_text(g_indicators[slot].progress,
      sizeof(g_indicators[slot].progress), progress);
  copy_text(g_indicators[slot].badge_key,
      sizeof(g_indicators[slot].badge_key), badge_key);
}

void ra_notify_indicator_set_detail(int kind, uint32_t id, const char *text,
                  const char *progress, const char *badge_key) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  int slot;

  if (!indicator_set_valid(kind, text, progress, badge_key))
    return;

  ra_notify_lock();
  slot = indicator_pick_slot(kind, id);
  indicator_store(slot, kind, id, text, progress, badge_key, now);
  ra_notify_unlock();
}

void ra_notify_indicator_clear(int kind, uint32_t id) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  int i;

  ra_notify_lock();
  for (i = 0; i < RA_INDICATOR_MAX; ++i) {
    if (g_indicators[i].used && g_indicators[i].active &&
      g_indicators[i].kind == kind && g_indicators[i].id == id) {
      /* Fade out instead of vanishing: PPSSPP sets an end time and PCSX2
             * fades for 0.5 s. The draw frees the slot when the fade ends. */
      g_indicators[i].active = 0;
      g_indicators[i].change_us = now;
    }
  }
  ra_notify_unlock();
}

/* Clear every live indicator of one kind. Needed because rcheevos raises
 * PROGRESS_INDICATOR_HIDE from its own 2 s timer with no achievement attached
 * (third_party/rcheevos/src/rc_client.c:6109-6126), and there is only ever one
 * progress tracker anyway, so an id match would never fire. */
void ra_notify_indicator_clear_kind(int kind) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  int i;

  ra_notify_lock();
  for (i = 0; i < RA_INDICATOR_MAX; ++i) {
    if (g_indicators[i].used && g_indicators[i].active &&
      g_indicators[i].kind == kind) {
      g_indicators[i].active = 0;
      g_indicators[i].change_us = now;
    }
  }
  ra_notify_unlock();
}







/* Free indicator slots whose fade-out has finished. Caller holds the lock. */
void ra_notify_indicator_expire(unsigned long long now) {
  int i;

  for (i = 0; i < RA_INDICATOR_MAX; ++i) {
    if (g_indicators[i].used && !g_indicators[i].active &&
      now - g_indicators[i].change_us >= RA_INDICATOR_FADE_OUT_US)
      memset(&g_indicators[i], 0, sizeof(g_indicators[i]));
  }
}

int ra_notify_indicator_visible(unsigned long long now) {
  int i;

  for (i = 0; i < RA_INDICATOR_MAX; ++i) {
    if (g_indicators[i].used &&
      indicator_alpha(&g_indicators[i], now) > 0.0f)
      return 1;
  }
  return 0;
}



int ra_notify_indicator_count(void) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  int count = 0;
  int i;

  ra_notify_lock();
  for (i = 0; i < RA_INDICATOR_MAX; ++i) {
    if (g_indicators[i].used &&
      indicator_alpha(&g_indicators[i], now) > 0.0f)
      ++count;
  }
  ra_notify_unlock();
  return count;
}

static uint32_t indicator_accent(int kind) {
  switch (kind) {
  case RA_INDICATOR_CHALLENGE:
    return 0xFF3A3AE8;
  case RA_INDICATOR_PROGRESS:
    return 0xFFE8B045;
  case RA_INDICATOR_TRACKER:
  default:
    return 0xFFC868BA;
  }
}

static float indicator_tile_size(void) {
  switch (g_indicator_size) {
  case 0:
    return RA_INDICATOR_TILE_SMALL;
  case 2:
    return RA_INDICATOR_TILE_LARGE;
  case 1:
  default:
    return RA_INDICATOR_TILE_STANDARD;
  }
}

static float indicator_alpha(const RaIndicator *indicator,
              unsigned long long now) {
  unsigned long long elapsed;
  unsigned long long start;

  if (indicator->active) {
    /* Not qualified yet: nothing is drawn, so a tile that exists for less
         * than RA_INDICATOR_QUALIFY_US never reaches the screen. */
    start = indicator->reveal_us;
    if (now < start)
      return 0.0f;
    elapsed = now - start;
    if (elapsed >= RA_INDICATOR_FADE_IN_US)
      return 1.0f;
    return (float)elapsed / (float)RA_INDICATOR_FADE_IN_US;
  }
  elapsed = now - indicator->change_us;
  if (elapsed >= RA_INDICATOR_FADE_OUT_US)
    return 0.0f;
  return 1.0f - (float)elapsed / (float)RA_INDICATOR_FADE_OUT_US;
}


static void draw_indicator_tracker_glyph(float x, float y, float size,
                    uint32_t accent, float alpha);

static void draw_indicator_tile(const RaIndicator *indicator, float x, float y,
                float size, float alpha) {
  uint32_t accent = indicator_accent(indicator->kind);
  uint32_t frame = COLOR_ALPHA(accent, (uint32_t)(0xFF * alpha));
  vita2d_texture *badge = NULL;

  vita2d_draw_rectangle(x + 2.0f, y + 2.0f, size, size,
            COLOR_ALPHA(BLACK, (uint32_t)(0x60 * alpha)));
  vita2d_draw_rectangle(x, y, size, size,
            COLOR_ALPHA(0xFF141414, (uint32_t)(0xC8 * alpha)));
  vita2d_draw_rectangle(x, y, size, 2.0f, frame);
  vita2d_draw_rectangle(x, y + size - 2.0f, size, 2.0f, frame);
  vita2d_draw_rectangle(x, y, 2.0f, size, frame);
  vita2d_draw_rectangle(x + size - 2.0f, y, 2.0f, size, frame);

  if (indicator->badge_key[0] != '\0')
    badge = ra_badge_texture(indicator->badge_key);
  if (badge != NULL) {
    float width = (float)vita2d_texture_get_width(badge);
    float height = (float)vita2d_texture_get_height(badge);

    if (width > 0.0f && height > 0.0f) {
      const float inset = 3.0f;
      float inner = size - inset * 2.0f;

      vita2d_draw_texture_tint_scale(
        badge, x + inset, y + inset, inner / width, inner / height,
        COLOR_ALPHA(WHITE, (uint32_t)(0xFF * alpha)));
      return;
    }
  }

  vita2d_draw_rectangle(x + 7.0f, y + 7.0f, size - 14.0f, size - 14.0f,
            COLOR_ALPHA(accent, (uint32_t)(0x34 * alpha)));
  if (indicator->kind == RA_INDICATOR_TRACKER) {
    draw_indicator_tracker_glyph(x, y, size, accent, alpha);
    return;
  } {
    const char *mark = "RA";
    float width = vita2d_pgf_text_width(font, 0.86f, mark);

    ra_notify_draw_text_shadow(x + (size - width) * 0.5f, y + (size - 18.0f) * 0.5f,
            COLOR_ALPHA(accent, (uint32_t)(0xFF * alpha)), 0.86f,
            mark);
  }
}


static const char *indicator_caption(const RaIndicator *indicator) {
  if (indicator->progress[0] != '\0')
    return indicator->progress;
  if (indicator->kind == RA_INDICATOR_TRACKER && indicator->text[0] != '\0')
    return indicator->text;
  return NULL;
}

/* Leaderboard tracker inside a tile: there is no badge art for a leaderboard, so
 * the tile carries a stopwatch dial and the value goes in the caption beside it,
 * exactly where a challenge puts its measurement. */
static void draw_indicator_tracker_glyph(float x, float y, float size,
                    uint32_t accent, float alpha) {
  float cx = x + size * 0.5f;
  float cy = y + size * 0.5f + 2.0f;
  float radius = size * 0.24f;

  vita2d_draw_fill_circle(cx, cy, radius,
              COLOR_ALPHA(accent, (uint32_t)(0xC0 * alpha)));
  vita2d_draw_rectangle(cx - radius * 0.22f, cy - radius - 5.0f,
            radius * 0.44f, 5.0f,
            COLOR_ALPHA(accent, (uint32_t)(0xC0 * alpha)));
}

/* Copies the live indicators into `snapshot` under the lock, expiring slots whose
 * fade-out has finished. Returns how many are drawable. */
static int indicator_snapshot(unsigned long long now, RaIndicator *snapshot,
    float *alpha_values, float *slides) {
  int count = 0;
  int i;

  ra_notify_lock();
  for (i = 0; i < RA_INDICATOR_MAX; ++i) {
    float alpha;

    if (!g_indicators[i].used)
      continue;
    alpha = indicator_alpha(&g_indicators[i], now);
    if (alpha <= 0.0f) {
      /* Either the fade-out finished (free the slot) or the entry has not
             * qualified yet (keep it: it may still earn its place). */
      if (!g_indicators[i].active)
        memset(&g_indicators[i], 0, sizeof(g_indicators[i]));
      continue;
    }
    snapshot[count] = g_indicators[i];
    alpha_values[count] = alpha;
    slides[count] = 0.0f;
    if (g_indicators[i].active && now >= g_indicators[i].reveal_us &&
      now - g_indicators[i].reveal_us < RA_INDICATOR_FADE_IN_US) {
      float progress =
        (float)(now - g_indicators[i].reveal_us) /
        (float)RA_INDICATOR_FADE_IN_US;
      slides[count] = RA_INDICATOR_SLIDE_PX * (1.0f - progress);
    }
    ++count;
  }
  ra_notify_unlock();
  return count;
}

/* Draws leaderboard trackers after the achievement tiles. */
static void indicator_sort_trackers_first(RaIndicator *snapshot,
    float *alpha_values, float *slides, int count) {
  int a;
  int b;

  for (a = 0; a < count; ++a) {
    for (b = a + 1; b < count; ++b) {
      if (snapshot[a].kind != RA_INDICATOR_TRACKER &&
          snapshot[b].kind == RA_INDICATOR_TRACKER) {
        RaIndicator swap_indicator = snapshot[a];
        float swap_float = alpha_values[a];

        snapshot[a] = snapshot[b];
        snapshot[b] = swap_indicator;
        alpha_values[a] = alpha_values[b];
        alpha_values[b] = swap_float;
        swap_float = slides[a];
        slides[a] = slides[b];
        slides[b] = swap_float;
      }
    }
  }
}

/* Measure first: a row is centred or right-aligned as a whole. Every
 * indicator is a tile now - a tracker differs only in its glyph and in where
 * its caption comes from (see indicator_caption). Returns the row width. */
static float indicator_measure(const RaIndicator *snapshot, int count,
    float *widths, float tile) {
  float tile_row_w = 0.0f;
  int tiles = 0;
  int trackers = 0;
  int i;

  for (i = 0; i < count; ++i) {
    const char *caption = indicator_caption(&snapshot[i]);

    widths[i] = tile;
    if (caption != NULL && caption[0] != '\0') {
      char text[RA_INDICATOR_PROGRESS_MAX];
      float caption_width;

      copy_text(text, sizeof(text), caption);
      ra_notify_fit_text(text, sizeof(text), RA_INDICATOR_CAPTION_MAX_W,
          RA_INDICATOR_CAPTION_SCALE);
      caption_width = vita2d_pgf_text_width(
        font, RA_INDICATOR_CAPTION_SCALE, text);
      widths[i] += RA_INDICATOR_GAP + caption_width +
            2.0f * RA_INDICATOR_CAPTION_PAD;
    }
    tile_row_w += widths[i] + RA_INDICATOR_GAP;
    ++tiles;
    if (snapshot[i].kind == RA_INDICATOR_TRACKER)
      ++trackers; /* logging only: there is no separate tracker row any more */
  }
  if (tile_row_w > 0.0f)
    tile_row_w -= RA_INDICATOR_GAP;
  return tile_row_w;
}

/* Caption box beside one tile, when the indicator has one. */
static void indicator_draw_caption(const RaIndicator *indicator, float x,
    float y, float tile, float alpha) {
  const char *source = indicator_caption(indicator);
  char caption[RA_INDICATOR_TEXT_MAX];
  float caption_x;
  float caption_w;

  if (source == NULL || source[0] == '\0')
    return;
  caption_x = x + tile + RA_INDICATOR_GAP;
  copy_text(caption, sizeof(caption), source);
  ra_notify_fit_text(caption, sizeof(caption),
      RA_INDICATOR_CAPTION_MAX_W,
      RA_INDICATOR_CAPTION_SCALE);
  caption_w = vita2d_pgf_text_width(
    font, RA_INDICATOR_CAPTION_SCALE, caption);
  vita2d_draw_rectangle(
    caption_x - RA_INDICATOR_CAPTION_PAD,
    y + 6.0f,
    caption_w + 2.0f * RA_INDICATOR_CAPTION_PAD,
    tile - 12.0f,
    COLOR_ALPHA(0xFF141414,
          (uint32_t)(0xB0 * alpha)));
  ra_notify_draw_text_shadow(
    caption_x, y + (tile - 18.0f) * 0.5f,
    COLOR_ALPHA(indicator_accent(indicator->kind),
          (uint32_t)(0xFF * alpha)),
    RA_INDICATOR_CAPTION_SCALE, caption);
}

/* Places the measured row at the anchored edge and draws every tile + caption. */
static void indicator_draw_row(RaIndicator *snapshot, const float *alpha_values,
    const float *slides, const float *widths, int count, float tile_row_w,
    float tile) {
  int top = (g_indicator_anchor <= 2);
  int align = g_indicator_anchor % 3; /* 0 left, 1 center, 2 right */
  float tile_row_x;
  float x;
  float y;
  int i;

  if (align == 0) {
    tile_row_x = RA_INDICATOR_EDGE_PAD;
  } else if (align == 1) {
    tile_row_x = (SCREEN_WIDTH - tile_row_w) * 0.5f;
  } else {
    /* Right anchors: the whole row ends at the edge. Laying it out from this
       * x keeps leaderboards first (leftmost) and stops a caption box from
       * being placed against the edge before its tile. */
    tile_row_x = SCREEN_WIDTH - RA_INDICATOR_EDGE_PAD - tile_row_w;
  }
  if (top)
    y = RA_INDICATOR_EDGE_PAD;
  else
    y = SCREEN_HEIGHT - RA_INDICATOR_EDGE_PAD - tile;

  x = tile_row_x;
  for (i = 0; i < count; ++i) {
    float y_off;

    /* Reveal: come in from the anchored edge (top rows from above, bottom
           * rows from below) while the fade runs. */
    y_off = top ? -slides[i] : slides[i];
    draw_indicator_tile(&snapshot[i], x, y + y_off, tile,
              alpha_values[i]);
    indicator_draw_caption(&snapshot[i], x, y + y_off, tile,
        alpha_values[i]);
    x += widths[i] + RA_INDICATOR_GAP;
  }
}

void ra_notify_indicator_draw(void) {
  RaIndicator snapshot[RA_INDICATOR_MAX];
  float alpha_values[RA_INDICATOR_MAX];
  float slides[RA_INDICATOR_MAX];
  float widths[RA_INDICATOR_MAX];
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
  float tile = indicator_tile_size();
  float tile_row_w;
  int count;

  count = indicator_snapshot(now, snapshot, alpha_values, slides);
  indicator_sort_trackers_first(snapshot, alpha_values, slides, count);
  if (count == 0)
    return;

  tile_row_w = indicator_measure(snapshot, count, widths, tile);
  indicator_draw_row(snapshot, alpha_values, slides, widths, count,
      tile_row_w, tile);
}


void ra_notify_indicator_reset(void) {
  memset(g_indicators, 0, sizeof(g_indicators));
}

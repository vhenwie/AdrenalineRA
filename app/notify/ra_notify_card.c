#include "ra_notify_card.h"
#include "ra_notify.h"
#include "ra_notify_queue.h"
#include "ra_notify_indicator.h"
#include "ra_notify_gfx.h"
#include "ra_notify_internal.h"

#include <stdio.h>
#include <string.h>

#include <vita2d.h>

#include "menu.h"
#include "ra_badge.h"
#include "ra_runtime.h"

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source != NULL ? source : "");
}


#define RA_NOTIFY_CARD_W 720.0f
#define RA_NOTIFY_CARD_HEIGHT 96.0f
#define RA_NOTIFY_CARD_MIN_W 360.0f
#define RA_NOTIFY_CARD_MIN_H 84.0f
#define RA_NOTIFY_ENTER_US 180000ULL
#define RA_NOTIFY_EXIT_US 350000ULL

typedef struct {
  RaNotifyItem item;
  char title[RA_NOTIFY_TITLE_MAX];
  char subtitle[RA_NOTIFY_SUBTITLE_MAX];
  char points[24];
  uint32_t accent;
  uint32_t surface;
  float card_w;
  float card_h;
  float title_y;
  float points_y;
  float x;
  float y;
  float alpha;
} RaNotifyDraw;

static void notify_measure(RaNotifyDraw *d) {
  RaNotifyItem item = d->item;
  char title[RA_NOTIFY_TITLE_MAX];
  char subtitle[RA_NOTIFY_SUBTITLE_MAX];
  char points[24];
  float title_w;
  float subtitle_w;
  float points_w;
  float needed;
  float second;
  float text_width;
  float card_w;
  float card_h;

  copy_text(title, sizeof(title), item.title);
  copy_text(subtitle, sizeof(subtitle), item.subtitle);
  if (item.points > 0)
    snprintf(points, sizeof(points), "+%d", item.points);
  else
    points[0] = '\0';
  title_w = vita2d_pgf_text_width(font, 1.0f, title);
  subtitle_w = subtitle[0] != '\0'
    ? vita2d_pgf_text_width(font, 0.86f, subtitle) : 0.0f;
  points_w = points[0] != '\0'
    ? vita2d_pgf_text_width(font, 1.0f, points) : 0.0f;
  needed = title_w;
  second = subtitle_w + (points_w > 0.0f ? points_w + 16.0f : 0.0f);
  if (second > needed)
    needed = second;
  card_w = 94.0f + needed + 18.0f;
  if (card_w < RA_NOTIFY_CARD_MIN_W)
    card_w = RA_NOTIFY_CARD_MIN_W;
  if (card_w > RA_NOTIFY_CARD_W)
    card_w = RA_NOTIFY_CARD_W;
  if (subtitle[0] == '\0')
    card_h = RA_NOTIFY_CARD_MIN_H;
  else if (title_w > 420.0f || subtitle_w > 420.0f)
    card_h = RA_NOTIFY_CARD_HEIGHT;
  else
    card_h = RA_NOTIFY_CARD_MIN_H;
  text_width = card_w - 94.0f - 18.0f;
  if (text_width < 100.0f)
    text_width = 100.0f;
  ra_notify_fit_text(title, sizeof(title),
      text_width - (points_w > 0.0f ? points_w + 16.0f : 0.0f), 1.0f);
  ra_notify_fit_text(subtitle, sizeof(subtitle),
      text_width - (points_w > 0.0f ? points_w + 16.0f : 0.0f), 0.86f);
  d->accent = ra_notify_accent_for(item.kind);
  d->surface = 0xFF212121;
  d->card_w = card_w;
  d->card_h = card_h;
  d->title_y = subtitle[0] != '\0' ? 18.0f : 28.0f;
  d->points_y = subtitle[0] != '\0' ? 55.0f : 30.0f;
  copy_text(d->title, sizeof(d->title), title);
  copy_text(d->subtitle, sizeof(d->subtitle), subtitle);
  copy_text(d->points, sizeof(d->points), points);
}

static void notify_anchor(RaNotifyDraw *d) {
  static const float pad_values[] = { 0.0f, 2.0f, 4.0f, 8.0f, 16.0f };
  float margin_h = ra_notify_padding_auto()
    ? 16.0f
    : pad_values[ra_notify_padding_h() >= 0 && ra_notify_padding_h() <= 4 ? ra_notify_padding_h() : 2];
  float margin_v = ra_notify_padding_auto()
    ? 16.0f
    : pad_values[ra_notify_padding_v() >= 0 && ra_notify_padding_v() <= 4 ? ra_notify_padding_v() : 2];

  switch (ra_notify_anchor()) {
  case 0:
  case 3:
    d->x = margin_h;
    break;
  case 1:
  case 4:
    d->x = (SCREEN_WIDTH - d->card_w) * 0.5f;
    break;
  default:
    d->x = SCREEN_WIDTH - d->card_w - margin_h;
    break;
  }
  switch (ra_notify_anchor()) {
  case 3:
  case 4:
  case 5:
    d->y = SCREEN_HEIGHT - d->card_h - margin_v;
    break;
  default:
    d->y = margin_v;
    break;
  }
}

static void notify_animate(RaNotifyDraw *d, unsigned long long now) {
  float enter = (float)(uint32_t)(now - d->item.start_us) / 180000.0f;
  float leave = (float)(uint32_t)(d->item.end_us - now) / 350000.0f;
  float slide;

  if (enter < 0.0f) enter = 0.0f;
  if (enter > 1.0f) enter = 1.0f;
  if (leave < 0.0f) leave = 0.0f;
  if (leave > 1.0f) leave = 1.0f;
  d->alpha = enter < leave ? enter : leave;
  slide = (1.0f - enter) * 24.0f;
  if (ra_notify_anchor() == 3 || ra_notify_anchor() == 4 || ra_notify_anchor() == 5)
    d->y += slide;
  else if (ra_notify_anchor() != 1 && ra_notify_anchor() != 4)
    d->y -= slide;
  if (ra_notify_anchor() == 0 || ra_notify_anchor() == 3)
    d->x -= slide;
  else if (ra_notify_anchor() == 2 || ra_notify_anchor() == 5)
    d->x += slide;
}

static void notify_position(RaNotifyDraw *d, unsigned long long now) {
  notify_anchor(d);
  notify_animate(d, now);
}

static void notify_prepare(RaNotifyDraw *d, unsigned long long now) {
  notify_measure(d);
  notify_position(d, now);
}

static void notify_render(const RaNotifyDraw *d, vita2d_texture *badge) {
  vita2d_draw_rectangle(d->x + 4.0f, d->y + 4.0f, d->card_w, d->card_h,
            COLOR_ALPHA(BLACK, (uint32_t)(0x60 * d->alpha)));
  vita2d_draw_rectangle(d->x, d->y, d->card_w, d->card_h,
            COLOR_ALPHA(0xFF484848, (uint32_t)(0xFF * d->alpha)));
  vita2d_draw_rectangle(d->x + 1.0f, d->y + 1.0f, d->card_w - 2.0f,
            d->card_h - 2.0f,
            COLOR_ALPHA(d->surface, (uint32_t)(0xF2 * d->alpha)));
  vita2d_draw_rectangle(d->x, d->y, 4.0f, d->card_h,
            COLOR_ALPHA(d->accent, (uint32_t)(0xFF * d->alpha)));

  vita2d_draw_rectangle(d->x + 14.0f, d->y + 16.0f, 64.0f, 64.0f,
            COLOR_ALPHA(d->accent, (uint32_t)(0x28 * d->alpha)));
  vita2d_draw_rectangle(d->x + 14.0f, d->y + 16.0f, 64.0f, 2.0f,
            COLOR_ALPHA(d->accent, (uint32_t)(0xB0 * d->alpha)));
  if (badge != NULL) {
    float badge_w = (float)vita2d_texture_get_width(badge);
    float badge_h = (float)vita2d_texture_get_height(badge);
    if (badge_w > 0.0f && badge_h > 0.0f) {
      vita2d_draw_texture_tint_scale(
        badge, d->x + 14.0f, d->y + 16.0f, 64.0f / badge_w, 64.0f / badge_h,
        COLOR_ALPHA(WHITE, (uint32_t)(0xFF * d->alpha)));
    }
  } else {
    ra_notify_draw_glyph(d->x + 14.0f, d->y + 16.0f, d->item.kind,
        COLOR_ALPHA(d->accent, (uint32_t)(0xFF * d->alpha)), d->alpha);
  }

  ra_notify_draw_text_shadow(d->x + 94.0f, d->y + d->title_y,
          COLOR_ALPHA(WHITE, (uint32_t)(0xFF * d->alpha)), 1.0f, d->title);
  if (d->subtitle[0] != '\0') {
    ra_notify_draw_text_shadow(d->x + 94.0f, d->y + 52.0f,
            COLOR_ALPHA(0xFFD0D0D0, (uint32_t)(0xFF * d->alpha)),
            0.86f, d->subtitle);
  }
  if (d->points[0] != '\0') {
    float width = vita2d_pgf_text_width(font, 1.0f, d->points);
    ra_notify_draw_text_shadow(d->x + d->card_w - 18.0f - width, d->y + d->points_y,
            COLOR_ALPHA(d->accent, (uint32_t)(0xFF * d->alpha)), 1.0f,
            d->points);
  }
}

void ra_notify_draw(void) {
  RaNotifyDraw d;
  vita2d_texture *badge;
  unsigned long long now;

  if (font == NULL)
    return;

  /* Never draw a card over the XMB. A leaderboard result queued just before
   * the game closed can otherwise survive into the shell for one frame. */
  if (ra_runtime_in_shell()) {
    ra_notify_clear();
    return;
  }

  ra_badge_frame_begin();
  ra_badge_prime_step();
  ra_notify_indicator_draw();

  if (!ra_notify_take_current(&d.item, &now))
    return;

  notify_prepare(&d, now);
  badge = ra_badge_texture(d.item.badge_key);
  if (d.item.kind == RA_NOTIFY_INFO || d.item.kind == RA_NOTIFY_ERROR)
    badge = NULL;
  notify_render(&d, badge);
}

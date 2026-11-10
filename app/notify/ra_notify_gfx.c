#include "ra_notify_gfx.h"
#include "ra_notify.h"

#include <string.h>

#include "menu.h"
#include "ra_text.h"

void ra_notify_fit_text(char *text, size_t capacity, float max_width,
          float scale) {
  size_t length = strlen(text);

  if (font == NULL || capacity == 0)
    return;
  if (vita2d_pgf_text_width(font, scale, text) <= max_width)
    return;
  while (length > 3) {
    text[length - 1] = '\0';
    --length;
    if (vita2d_pgf_text_width(font, scale, text) <= max_width)
      break;
  }
  if (length > 3 && capacity >= 5) {
    text[length - 3] = '.';
    text[length - 2] = '.';
    text[length - 1] = '.';
    text[length] = '\0';
  }
}

void ra_notify_draw_text_shadow(float x, float y, uint32_t color, float scale,
              const char *text) {
  pgf_draw_text(x + 1.0f, y + 1.0f, COLOR_ALPHA(BLACK, 0x90), scale, text);
  pgf_draw_text(x, y, color, scale, text);
}

/* RetroAchievements mark drawn from primitives (no official artwork); used for client
 * messages and as the badge fallback. */
void ra_notify_draw_ra_mark(float x, float y, uint32_t color, float alpha,
            int alert) {
  const float plate_x = x + 6.0f;
  const float plate_y = y + 8.0f;
  const float plate_w = 52.0f;
  const float plate_h = 46.0f;
  uint32_t frame = COLOR_ALPHA(color, (uint32_t)(0xFF * alpha));
  float text_w;

  if (font == NULL)
    return;

  vita2d_draw_rectangle(plate_x, plate_y, plate_w, plate_h,
            COLOR_ALPHA(0xFF141414, (uint32_t)(0xB4 * alpha)));
  vita2d_draw_rectangle(plate_x, plate_y, plate_w, 2.0f, frame);
  vita2d_draw_rectangle(plate_x, plate_y + plate_h - 2.0f, plate_w, 2.0f,
            frame);
  vita2d_draw_rectangle(plate_x, plate_y, 2.0f, plate_h, frame);
  vita2d_draw_rectangle(plate_x + plate_w - 2.0f, plate_y, 2.0f, plate_h,
            frame);

  text_w = vita2d_pgf_text_width(font, 1.1f, "RA");
  pgf_draw_text(plate_x + (plate_w - text_w) * 0.5f, plate_y + 15.0f,
        frame, 1.1f, "RA");

  if (alert) {
    /* Errors keep a marker: the plate says where it came from, the dot says
         * something went wrong. */
    vita2d_draw_rectangle(plate_x + plate_w - 11.0f, plate_y + plate_h - 13.0f,
              7.0f, 7.0f, frame);
  }
}

void ra_notify_draw_glyph(float x, float y, int kind, uint32_t accent, float alpha) {
  switch (kind) {
  case RA_NOTIFY_UNLOCK:
  case RA_NOTIFY_MASTERY:
    vita2d_draw_fill_circle(x + 32.0f, y + 23.0f, 13.0f, accent);
    vita2d_draw_rectangle(x + 29.0f, y + 34.0f, 6.0f, 11.0f, accent);
    vita2d_draw_rectangle(x + 21.0f, y + 45.0f, 22.0f, 5.0f, accent);
    break;
  case RA_NOTIFY_LB_START:
  case RA_NOTIFY_LB_SUBMIT:
  case RA_NOTIFY_LB_FAIL:
    vita2d_draw_rectangle(x + 19.0f, y + 33.0f, 7.0f, 16.0f, accent);
    vita2d_draw_rectangle(x + 29.0f, y + 24.0f, 7.0f, 25.0f, accent);
    vita2d_draw_rectangle(x + 39.0f, y + 17.0f, 7.0f, 32.0f, accent);
    break;
  case RA_NOTIFY_ERROR:
    ra_notify_draw_ra_mark(x, y, WHITE, alpha, 1);
    break;
  case RA_NOTIFY_SESSION:
  case RA_NOTIFY_INFO:
  default:
    ra_notify_draw_ra_mark(x, y, WHITE, alpha, 0);
    break;
  }
}

/* Live indicators are drawn as badge tiles plus tracker boxes; the layout body
 * follows the helpers below. */


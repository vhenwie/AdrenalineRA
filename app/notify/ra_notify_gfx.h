#ifndef RA_NOTIFY_GFX_H
#define RA_NOTIFY_GFX_H

#include <stddef.h>
#include <stdint.h>
#include "ra_notify.h"

void ra_notify_fit_text(char *text, size_t capacity, float max_width, float scale);
void ra_notify_draw_text_shadow(float x, float y, uint32_t color, float scale,
                                const char *text);
void ra_notify_draw_ra_mark(float x, float y, uint32_t color, float alpha,
                            int alert);
void ra_notify_draw_glyph(float x, float y, int kind, uint32_t accent, float alpha);

#endif

/* ASCII-only PGF text helpers. */

#ifndef RA_TEXT_H
#define RA_TEXT_H

#include <stdint.h>
#include <vita2d.h>

/* Call once after the PGF font exists. Returns 0 if pgf is NULL. */
int ra_text_init(vita2d_pgf *pgf);
int ra_text_ready(void);

/* Width and draw helpers. Non-ASCII codepoints are removed. */
int ra_text_width(vita2d_pgf *pgf, float scale, const char *text);
float ra_text_draw(vita2d_pgf *pgf, float x, float y, uint32_t color,
                   float scale, const char *text);

#endif

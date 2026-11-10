/* ASCII-only PGF text. Non-ASCII codepoints are dropped. */

#include "ra_text.h"

#define RUN_MAX 96

static int g_ready;

static unsigned int next_codepoint(const char **cursor) {
  const unsigned char *s = (const unsigned char *)*cursor;
  unsigned int cp;
  int extra;
  int i;

  if (*s < 0x80) {
    *cursor += 1;
    return *s;
  }
  if ((*s & 0xE0) == 0xC0) {
    cp = *s & 0x1F;
    extra = 1;
  } else if ((*s & 0xF0) == 0xE0) {
    cp = *s & 0x0F;
    extra = 2;
  } else if ((*s & 0xF8) == 0xF0) {
    cp = *s & 0x07;
    extra = 3;
  } else {
    *cursor += 1;
    return *s;
  }
  for (i = 1; i <= extra; ++i) {
    if ((s[i] & 0xC0) != 0x80) {
      *cursor += 1;
      return *s;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *cursor += extra + 1;
  return cp;
}

int ra_text_init(vita2d_pgf *pgf) {
  if (pgf == NULL)
    return 0;
  g_ready = 1;
  return 1;
}

int ra_text_ready(void) {
  return g_ready != 0;
}

int ra_text_width(vita2d_pgf *pgf, float scale, const char *text) {
  char run[RUN_MAX];
  size_t run_len = 0;
  const char *cursor = text;
  float width = 0.0f;

  if (pgf == NULL || text == NULL)
    return 0;
  if (!ra_text_ready())
    return vita2d_pgf_text_width(pgf, scale, text);

  while (*cursor != '\0') {
    const char *start = cursor;
    unsigned int cp = next_codepoint(&cursor);

    if (cp >= 0x80)
      continue;
    while (start < cursor) {
      if (run_len + 1 >= sizeof(run)) {
        run[run_len] = '\0';
        width += (float)vita2d_pgf_text_width(pgf, scale, run);
        run_len = 0;
      }
      run[run_len++] = *start++;
    }
  }
  if (run_len > 0) {
    run[run_len] = '\0';
    width += (float)vita2d_pgf_text_width(pgf, scale, run);
  }
  return (int)(width + 0.5f);
}

float ra_text_draw(vita2d_pgf *pgf, float x, float y, uint32_t color,
                   float scale, const char *text) {
  char run[RUN_MAX];
  size_t run_len = 0;
  const char *cursor = text;
  float pen = x;

  if (pgf == NULL || text == NULL)
    return x;
  if (!ra_text_ready()) {
    vita2d_pgf_draw_text(pgf, (int)x, (int)y, color, scale, text);
    return x + (float)vita2d_pgf_text_width(pgf, scale, text);
  }

  while (*cursor != '\0') {
    const char *start = cursor;
    unsigned int cp = next_codepoint(&cursor);

    if (cp >= 0x80)
      continue;
    while (start < cursor) {
      if (run_len + 1 >= sizeof(run)) {
        run[run_len] = '\0';
        vita2d_pgf_draw_text(pgf, (int)pen, (int)y, color, scale, run);
        pen += (float)vita2d_pgf_text_width(pgf, scale, run);
        run_len = 0;
      }
      run[run_len++] = *start++;
    }
  }
  if (run_len > 0) {
    run[run_len] = '\0';
    vita2d_pgf_draw_text(pgf, (int)pen, (int)y, color, scale, run);
    pen += (float)vita2d_pgf_text_width(pgf, scale, run);
  }
  return pen;
}

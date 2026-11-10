#include "ra_menu_rows.h"

#include <stdio.h>
#include <string.h>

#include <vita2d.h>

#include "menu.h"
#include "rc_client.h"
#include "ra_menu_internal.h"

int ra_menu_progress_percent(const char *progress) {
  unsigned int have = 0;
  unsigned int need = 0;

  if (progress == NULL || progress[0] == '\0')
    return -1;
  if (sscanf(progress, "%u/%u", &have, &need) == 2 && need > 0 &&
    have <= need)
    return (int)((have * 100u) / need);
  return -1;
}

/* Cheap fingerprint of everything that changes a rendered achievement row.
 *
 * `add_achievement_row()` runs for every visible row on every rebuild, and a
 * rebuild re-runs `fit_text()` (a PGF measurement of up to 128 characters).
 * At 376 rows that spike was visible as a hitch twice a second, even when no
 * achievement had changed. Unchanged rows now reuse what was already rendered. */
void ra_menu_type_labels(uint8_t type, int unofficial,
                  const char **marker, const char **prefix) {
  /* Never merged: `[MU]` is not a marker (player correction ).
     * A typed achievement keeps its single letter, `[M]`/`[P]`/`[W]`, and an
     * unofficial one carries a separate `[U]`. The tooltip always spells the
     * type out and appends `[Unofficial]`. */
  switch (type) {
  case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
        *marker = unofficial ? "[M] [U] " : "[M] ";
        *prefix = unofficial ? "[Missable] [Unofficial] " : "[Missable] ";
    break;
  case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
        *marker = unofficial ? "[P] [U] " : "[P] ";
        *prefix = unofficial ? "[Progression] [Unofficial] " : "[Progression] ";
    break;
  case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
        *marker = unofficial ? "[W] [U] " : "[W] ";
        *prefix = unofficial ? "[Win Condition] [Unofficial] "
              : "[Win Condition] ";
    break;
  default:
        *marker = unofficial ? "[U] " : "";
        *prefix = unofficial ? "[Unofficial] " : "";
    break;
  }
}

/* Fit `prefix + title + tail` into `max_width` by shortening the title, never the tail:
 * the tail carries the measurement and its closing parenthesis. */
void ra_menu_fit_row(char *out, size_t capacity, const char *prefix,
              const char *title, const char *tail,
              float max_width) {
  char trimmed[96];
  size_t title_length;
  size_t length;
  float available;
  float ellipsis_width;

  if (out == NULL || capacity == 0)
    return;
  if (prefix == NULL)
    prefix = "";
  if (title == NULL)
    title = "";
  if (tail == NULL)
    tail = "";

  available = max_width -
        vita2d_pgf_text_width(font, FONT_SIZE, prefix) -
        vita2d_pgf_text_width(font, FONT_SIZE, tail);
  if (available < 0.0f)
    available = 0.0f;

  snprintf(trimmed, sizeof(trimmed), "%s", title);
  title_length = strlen(trimmed);
  length = title_length;
  while (length > 0 &&
    vita2d_pgf_text_width(font, FONT_SIZE, trimmed) > available)
    trimmed[--length] = '\0';

  if (length < title_length) {
    ellipsis_width = vita2d_pgf_text_width(font, FONT_SIZE, "...");
    while (length > 0 &&
      vita2d_pgf_text_width(font, FONT_SIZE, trimmed) + ellipsis_width >
        available)
      trimmed[--length] = '\0';
    if (length + 3 < sizeof(trimmed)) {
      trimmed[length++] = '.';
      trimmed[length++] = '.';
      trimmed[length++] = '.';
      trimmed[length] = '\0';
    }
  }

  snprintf(out, capacity, "%s%s%s", prefix, trimmed, tail);
}

/* `<marker><title>  <progress> (<pct>%)` - the achievement row format. The
 * progress and its percentage appear only when rcheevos reports a measurement,
 * because neither can be inferred from anything else on the screen. */
void ra_menu_format_achievement(char *out, size_t capacity, uint8_t type,
                int unofficial, const char *title,
                const char *progress) {
  const char *marker;
  const char *prefix;
  char tail[48];
  int percent;

  ra_menu_type_labels(type, unofficial, &marker, &prefix);
  (void)prefix;
  if (title == NULL)
    title = "";
  if (progress == NULL)
    progress = "";
  percent = ra_menu_progress_percent(progress);
  if (progress[0] != '\0') {
    if (percent >= 0)
      snprintf(tail, sizeof(tail), "  %s (%d%%)", progress, percent);
    else
      snprintf(tail, sizeof(tail), "  %s", progress);
  } else {
    tail[0] = '\0';
  }
  ra_menu_fit_row(out, capacity, marker, title, tail, 760.0f);
}



/* Active Challenges row: the achievement page's own naming (type marker + title),
 * then the live measurement in parentheses:
 * `[M] Title (7/20)`. The percentage stays on the achievement's own row; repeating
 * it here would make the challenge read as two progress values. */
void ra_menu_format_challenge(char *out, size_t capacity, uint8_t type,
                int unofficial, const char *title,
                const char *progress) {
  const char *marker;
  const char *prefix;
  char tail[40];

  ra_menu_type_labels(type, unofficial, &marker, &prefix);
  (void)prefix;
  if (title == NULL)
    title = "";
  if (progress == NULL)
    progress = "";
  if (progress[0] != '\0')
    snprintf(tail, sizeof(tail), " (%s)", progress);
  else
    tail[0] = '\0';
  ra_menu_fit_row(out, capacity, marker, title, tail, 760.0f);
}

/* `Next: [M] Title` - the achievement page's own naming (type marker + title) with
 * the label in front. Points live in the tooltip, exactly like every achievement row,
 * so a suggestion reads like the list it points into instead of a second style. */
void ra_menu_format_next(char *out, size_t capacity, const char *label,
              uint8_t type, int unofficial, const char *title) {
  const char *marker;
  const char *prefix;
  char lead[40];

  ra_menu_type_labels(type, unofficial, &marker, &prefix);
  (void)prefix;
  if (marker[0] != '\0')
    snprintf(lead, sizeof(lead), "%s: %s", label, marker);
  else
    snprintf(lead, sizeof(lead), "%s: ", label);
  ra_menu_fit_row(out, capacity, lead, title != NULL ? title : "", "", 760.0f);
}

/* `[Type] <description> (<points> Points)` - the achievement tooltip format.
 * The score follows the description in parentheses. */
void ra_menu_format_tip(char *out, size_t capacity, uint8_t type,
                int unofficial, const char *description,
                uint32_t points) {
  const char *marker;
  const char *prefix;

  ra_menu_type_labels(type, unofficial, &marker, &prefix);
  (void)marker;
  if (description == NULL)
    description = "";
  snprintf(out, capacity, "%s%s (%u %s)", prefix, description,
      (unsigned)points, ra_menu_points_word((unsigned)points));
}


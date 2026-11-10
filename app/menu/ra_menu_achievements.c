#include "ra_menu_achievements.h"
#include "ra_menu_entries.h"
#include "ra_menu_rows.h"
#include "ra_menu_internal.h"
#include "ra_menu_rows_build.h"
#include "ra_menu_entries_cache.h"
#include "ra_server.h"
#include "ra_menu_settings.h"
#include "ra_menu_account.h"
#include "ra_menu_nav.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "menu.h"
#include "rc_client.h"
#include "ra_menu_nav.h"
#include "ra_runtime.h"
#include "ra_settings.h"

#define RA_ACHIEVEMENT_ROW_CAPACITY RA_RUNTIME_MAX_ACHIEVEMENTS
#define RA_PIN_MAX 24
#define RA_FILTER_COUNT 6

static uint32_t g_achievement_subset;
static char g_titleid[16];
static char g_ui_titleid[16];
static int g_unlocked_open = 1;
static int g_locked_open = 1;
static char g_unlocked_header[64];
static char g_locked_header[64];

#define RA_PIN_MAX 24
static uint32_t g_pins[RA_PIN_MAX];
static int g_pin_count;
static int g_pinned_open = 1;
static char g_pinned_header[64];
static char g_achievement_summary[160];
/* `Next: <title> (12 pts)` rows plus their description tooltips. One buffer each,
 * because the menu keeps pointers to the text it is handed. rcheevos answers one
 * suggestion per call, so the rows come from a walk of the locked bucket
 * (`ra_runtime_next_achievements`), first row `Next:`, the rest `Then:`. */
/* Category filter: Triangle cycles All / Standard / Missable / Progression /
 * Win Condition / Unofficial, remembered per game. Pinned rows are exempt. */
#define RA_FILTER_COUNT 6
static uint8_t g_achievement_filter;
static char g_filter_line[96];
static char g_achievement_status[160];
void ra_menu_set_achievement_subset(uint32_t id) {
  g_achievement_subset = id;
}

int ra_open_achievements(void) {
  if (ra_runtime_subset_count() > 0)
    return ra_menu_open_page(RA_PAGE_ACHIEVEMENT_SETS);
  g_achievement_subset = 0;
  return ra_menu_open_page(RA_PAGE_ACHIEVEMENTS);
}
static void achievement_ui_path(char *path, size_t capacity) {
  char safe[16];
  size_t i;
  size_t j = 0;

  for (i = 0; i < sizeof(safe) - 1 && g_titleid[i] != '\0'; ++i) {
    char c = g_titleid[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9'))
      safe[j++] = c;
  }
  safe[j] = '\0';
  if (safe[0] == '\0')
    ra_menu_copy_text(safe, sizeof(safe), "XMB");
  sceIoMkdir("ux0:data/AdrenalineRA/ui", 0777);
  snprintf(path, capacity, "ux0:data/AdrenalineRA/ui/%s.cfg", safe);
}

static void achievement_ui_defaults(void) {
  g_unlocked_open = 1;
  g_locked_open = 1;
  g_pinned_open = 1;
  g_pin_count = 0;
  g_achievement_filter = 0;
  memset(g_pins, 0, sizeof(g_pins));
}

/* `filter=<0..4>`: the category the player last looked at for this game.
   * Anything out of range falls back to All rather than hiding every row. */
static void achievement_ui_parse_filter(const char *text) {
  const char *cursor = strstr(text, "filter=");

  if (cursor != NULL) {
    unsigned int value = 0;

    cursor += 7;
    while (*cursor >= '0' && *cursor <= '9') {
      value = value * 10u + (unsigned int)(*cursor - '0');
      ++cursor;
    }
    g_achievement_filter = (uint8_t)(value < RA_FILTER_COUNT ? value : 0);
  }
}

/* One `pin=<id>` line per pinned achievement. `strstr` cannot confuse this
   * with `pinned_open=` above: that word never contains "pin=" (it reads
   * "pinne..."). Ids are read digit by digit, so a truncated file simply pins
   * fewer rows instead of mis-reading one. */
static void achievement_ui_parse_pins(const char *text) {
  const char *cursor = text;

  while ((cursor = strstr(cursor, "pin=")) != NULL) {
    unsigned int id = 0;

    cursor += 4;
    while (*cursor >= '0' && *cursor <= '9') {
      id = id * 10u + (unsigned int)(*cursor - '0');
      ++cursor;
    }
    if (id == 0)
      continue;
    if (g_pin_count >= RA_PIN_MAX)
      break;
    g_pins[g_pin_count++] = (uint32_t)id;
  }
}

void ra_menu_sync_achievement_ui(void) {
  char path[128];
  char text[768];
  SceUID fd;
  int length;

  if (strcmp(g_ui_titleid, g_titleid) == 0)
    return;
  ra_menu_copy_text(g_ui_titleid, sizeof(g_ui_titleid), g_titleid);
  achievement_ui_defaults();
  g_achievement_subset = 0;
  achievement_ui_path(path, sizeof(path));
  fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return;
  length = (int)sceIoRead(fd, text, sizeof(text) - 1);
  sceIoClose(fd);
  if (length <= 0)
    return;
  text[length] = '\0';
  if (strstr(text, "unlocked_open=0") != NULL)
    g_unlocked_open = 0;
  if (strstr(text, "locked_open=0") != NULL)
    g_locked_open = 0;
  if (strstr(text, "pinned_open=0") != NULL)
    g_pinned_open = 0;
  achievement_ui_parse_filter(text);
  achievement_ui_parse_pins(text);
}

static void save_achievement_ui(void) {
  char path[128];
  char text[512];
  size_t length;
  SceUID fd;
  int i;

  achievement_ui_path(path, sizeof(path));
  length = (size_t)snprintf(text, sizeof(text),
              "unlocked_open=%d\nlocked_open=%d\n"
              "pinned_open=%d\n",
              g_unlocked_open ? 1 : 0, g_locked_open ? 1 : 0,
              g_pinned_open ? 1 : 0);
  /* `filter=` is written only when it is not the default: a game without the line
     * is a game with no filter, which is the shipped behaviour . Absence
     * means All, so a config from an older build (or none at all) still reads as
     * "no filter" instead of pinning a value the player never chose. */
  if (g_achievement_filter != 0 && length + 16 < sizeof(text))
    length += (size_t)snprintf(text + length, sizeof(text) - length,
                "filter=%d\n", (int)g_achievement_filter);
  for (i = 0; i < g_pin_count && length + 16 < sizeof(text); ++i)
    length += (size_t)snprintf(text + length, sizeof(text) - length,
                "pin=%u\n", (unsigned)g_pins[i]);
  fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return;
  sceIoWrite(fd, text, strlen(text));
  sceIoClose(fd);
}

static int ra_pin_find(uint32_t id) {
  int i;

  for (i = 0; i < g_pin_count; ++i) {
    if (g_pins[i] == id)
      return i;
  }
  return -1;
}

static void ra_pin_toggle(uint32_t id) {
  int index;

  if (id == 0)
    return;
  index = ra_pin_find(id);
  if (index >= 0) {
    memmove(&g_pins[index], &g_pins[index + 1],
        sizeof(g_pins[0]) * (size_t)(g_pin_count - index - 1));
    --g_pin_count;
  } else if (g_pin_count < RA_PIN_MAX) {
    g_pins[g_pin_count++] = id;
  } else {
    return;
  }
  save_achievement_ui();
  ra_menu_invalidate_entries();
}

/* Row action: selecting an achievement pins it to the top, selecting it again
 * removes the pin. The id travels as the row's
 * `callback_data`, so no global "current row" state is needed. */
int ra_menu_achievements_toggle_pin(void *data) {
  ra_pin_toggle((uint32_t)(uintptr_t)data);
  return 0;
}

static int ra_toggle_pinned(void) {
  g_pinned_open = !g_pinned_open;
  ra_menu_invalidate_entries();
  save_achievement_ui();
  return 0;
}

static int ra_toggle_unlocked(void) {
  g_unlocked_open = !g_unlocked_open;
  ra_menu_invalidate_entries();
  save_achievement_ui();
  return 0;
}

static int ra_toggle_locked(void) {
  g_locked_open = !g_locked_open;
  ra_menu_invalidate_entries();
  save_achievement_ui();
  return 0;
}

int ra_menu_achievements_page_active(void) {
  return ra_menu_page() == RA_PAGE_ACHIEVEMENTS;
}

/* Filter mask: bit 0 is always set (All), the rest follow g_filter_names. Built from the
 * snapshot so a filter never lands on an empty list. */
static unsigned g_filter_mask;


int ra_menu_filter_cycle(void) {
  unsigned mask;
  unsigned next;

  if (!ra_menu_achievements_page_active())
    return 0;

  /* Only cycle through All plus the types this set actually has . With no
     * mask yet (page not built) fall back to the full cycle. */
  mask = g_filter_mask != 0 ? g_filter_mask : ((1u << RA_FILTER_COUNT) - 1u);
  mask |= 1u; /* All is never skipped */
  for (next = (unsigned)g_achievement_filter; ; ) {
    next = (next + 1u) % (unsigned)RA_FILTER_COUNT;
    if (mask & (1u << next))
      break;
  }
  g_achievement_filter = (uint8_t)next;
  save_achievement_ui();
  ra_menu_invalidate_entries();
  return 1;
}

static RaAchievementView g_achievement_cache[RA_ACHIEVEMENT_ROW_CAPACITY];

/* Which filter types the set being displayed can actually show . Bit 0 is
 * always set (All); the rest follow `g_filter_names` (1 Standard, 2 Missable, 3
 * Progression, 4 Win Condition, 5 Unofficial). Built from the same snapshot that fills
 * the rows, so Triangle never lands on a type whose list would be empty. */
static void achievement_filter_mask_from_cache(int total, uint32_t subset) {
  unsigned mask = 1u;
  int i;

  for (i = 0; i < total; ++i) {
    const RaAchievementView *view = &g_achievement_cache[i];

    if (view->subset_id != subset)
      continue;
    switch (view->type) {
    case RC_CLIENT_ACHIEVEMENT_TYPE_STANDARD:
      mask |= 1u << 1;
      break;
    case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
      mask |= 1u << 2;
      break;
    case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
      mask |= 1u << 3;
      break;
    case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
      mask |= 1u << 4;
      break;
    default:
      break;
    }
    if (view->unofficial)
      mask |= 1u << 5;
  }
  g_filter_mask = mask;
}


/* Category filter: Triangle cycles All / Standard / Missable / Progression /
 * Win Condition / Unofficial, remembered per game. Pinned rows are exempt. */
static const char *const g_filter_names[RA_FILTER_COUNT] = {
  "All", "Standard", "Missable", "Progression", "Win Condition", "Unofficial"
};

static int achievement_matches_filter(const RaAchievementView *view) {
  switch (g_achievement_filter) {
  case 1:
    return view->type == RC_CLIENT_ACHIEVEMENT_TYPE_STANDARD;
  case 2:
    return view->type == RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE;
  case 3:
    return view->type == RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION;
  case 4:
    return view->type == RC_CLIENT_ACHIEVEMENT_TYPE_WIN;
  case 5:
    return view->unofficial != 0;
  default:
    return 1;
  }
}

/* Rows for one lock state in rcheevos order; pinned rows are emitted by their own
 * section. */
static void emit_achievement_list(int unlocked, int *slot, int total) {
  int i;

  for (i = 0; i < total; ++i) {
    RaAchievementView *view = &g_achievement_cache[i];

    if (view->subset_id != g_achievement_subset)
      continue;
    if ((view->unlocked ? 1 : 0) != unlocked)
      continue;
    if (!achievement_matches_filter(view))
      continue;
    if (ra_pin_find(view->id) >= 0)
      continue;
    ra_menu_add_achievement_row(view, (*slot)++);
  }
}

/* How many pins belong to the set being displayed. Kept separate from
 * emit_pinned_list so the header can be added first. */
static int count_pinned_rows(int total) {
  int count = 0;
  int i;

  for (i = 0; i < g_pin_count; ++i) {
    int a;

    for (a = 0; a < total; ++a) {
      if (g_achievement_cache[a].id != g_pins[i])
        continue;
      if (g_achievement_cache[a].subset_id == g_achievement_subset)
        ++count;
      break;
    }
  }
  return count;
}

/* The Pinned section, in pin order (most recently pinned last) so the list does
 * not reshuffle when an achievement unlocks. Only pins that belong to the set
 * being displayed are listed. */
static int emit_pinned_list(int *slot, int total) {
  int emitted = 0;
  int i;

  for (i = 0; i < g_pin_count; ++i) {
    int a;

    for (a = 0; a < total; ++a) {
      RaAchievementView *view = &g_achievement_cache[a];

      if (view->id != g_pins[i])
        continue;
      if (view->subset_id != g_achievement_subset)
        break;
      ra_menu_add_achievement_row(view, (*slot)++);
      ++emitted;
      break;
    }
  }
  return emitted;
}

/* Snapshot the runtime and the per-game UI state, then build the filter mask from
 * that same snapshot. */
static void achievement_build_begin(RaRuntimeInfo *info, int *total) {
  ra_menu_sync_achievement_ui();
  memset(info, 0, sizeof(*info));
  ra_runtime_copy_info(info);
  ra_runtime_copy_achievements(0, g_achievement_cache,
                RA_ACHIEVEMENT_ROW_CAPACITY, total);
  
  achievement_filter_mask_from_cache(*total, g_achievement_subset);
  if (g_achievement_filter != 0 &&
    (g_filter_mask & (1u << g_achievement_filter)) == 0)
    g_achievement_filter = 0;
}

/* Everything below is scoped to the set being displayed. Using the runtime
   * totals here made every set report the whole game: the subset showed the
   * base set's 148 rows and 1604 points instead of its own 228 and 1520. */
static void achievement_count_visible(int total, int *unlocked, int *locked,
    unsigned int *points_total, unsigned int *points_unlocked) {
  int i;

  for (i = 0; i < total; ++i) {
    if (g_achievement_cache[i].subset_id != g_achievement_subset)
      continue;
    /* Hidden rows do not count: a summary that says 12/40 above nine visible
         * rows is a bug report waiting to happen. Pins are counted here (they are
         * still part of the set) even though the Pinned section renders them. */
    if (!achievement_matches_filter(&g_achievement_cache[i]))
      continue;
    *points_total += (unsigned int)g_achievement_cache[i].points;
    if (g_achievement_cache[i].unlocked) {
      ++(*unlocked);
      *points_unlocked += (unsigned int)g_achievement_cache[i].points;
    }
    else {
      ++(*locked);
    }
  }
}

/* Summary line (or the not-running status) with the filter row under it. */
static void achievement_add_summary(const RaRuntimeInfo *info, int unlocked,
    int locked, unsigned int points_total, unsigned int points_unlocked) {
  if (info->ready) {
    snprintf(g_achievement_summary, sizeof(g_achievement_summary),
        "Achievements: %u/%u  Points: %u/%u",
        (unsigned)unlocked, (unsigned)(unlocked + locked),
        points_unlocked, points_total);
    ra_menu_add_text_tip(g_achievement_summary, info->hardcore ? ORANGE : GREEN,
          "D-pad Up/Down scrolls the achievement list. "
          "Select an achievement to pin or unpin it. "
          "Triangle cycles the category filter.");
    if (g_achievement_filter != 0) {
      /* No `(Triangle)` here: the summary's tooltip already teaches the key,
             * and repeating it on every filtered page was noise. The row is coloured
             * instead, so "a filter is active" is visible without reading it. */
      snprintf(g_filter_line, sizeof(g_filter_line),
          "Filter: %s - %u shown",
          g_filter_names[g_achievement_filter],
          (unsigned)(unlocked + locked));
      ra_menu_add_text_tip(g_filter_line, RA_COLOR_TOGGLE_FILTER,
            "Triangle cycles All / Standard / Missable / Progression / "
            "Win Condition / Unofficial; the choice is kept for this "
            "game.");
    }
  } else {
    ra_menu_copy_text(g_achievement_status, sizeof(g_achievement_status),
        info->status[0] != '\0' ? info->status
                    : "RetroAchievements is not running.");
    ra_menu_add_text(g_achievement_status, ORANGE);
  }
}

/* The suggestion rows sit under the summary : they answer "what
   * now" without scrolling a 148-row list, and they are dropped entirely when
   * rcheevos has nothing locked left to point at. `Next:` is the first, `Then:`
   * the following ones, because rcheevos walks the bucket one answer at a time
   * and never hands out a list. */

/* Pinned first, and only when the current set
   * has at least one pinned achievement: an empty section is noise. The count
   * comes first because the header has to be added before the rows it names. */
static void achievement_add_pinned_section(int *slot, int total) {
  int pinned = count_pinned_rows(total);

  if (pinned > 0) {
    snprintf(g_pinned_header, sizeof(g_pinned_header),
        "%s Pinned (%d)", g_pinned_open ? "[-]" : "[+]", pinned);
    ra_menu_add_callback(g_pinned_header, ra_toggle_pinned,
          ra_menu_toggle_color("Pinned", g_pinned_open ? 1 : 0));
    if (g_pinned_open)
      emit_pinned_list(slot, total);
  }
}

/* Locked first, then Unlocked. */
static void achievement_add_lock_sections(int *slot, int total, int unlocked,
    int locked) {
  snprintf(g_locked_header, sizeof(g_locked_header),
      "%s Locked (%d)", g_locked_open ? "[-]" : "[+]", locked);
  ra_menu_add_callback(g_locked_header, ra_toggle_locked,
        ra_menu_toggle_color("Locked", g_locked_open ? 1 : 0));
  if (g_locked_open)
    emit_achievement_list(0, slot, total);

  snprintf(g_unlocked_header, sizeof(g_unlocked_header),
      "%s Unlocked (%d)", g_unlocked_open ? "[-]" : "[+]", unlocked);
  ra_menu_add_callback(g_unlocked_header, ra_toggle_unlocked,
        ra_menu_toggle_color("Unlocked", g_unlocked_open ? 1 : 0));
  if (g_unlocked_open)
    emit_achievement_list(1, slot, total);
}

void ra_menu_build_achievements(void) {
  RaRuntimeInfo info;
  int slot = 0;
  int unlocked = 0;
  int locked = 0;
  int total = 0;
  unsigned int points_total = 0;
  unsigned int points_unlocked = 0;

  achievement_build_begin(&info, &total);
  if (info.ready && total == 0) {
    ra_menu_add_text("No achievements are available for this set.", LITEGRAY);
    ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
    return;
  }
  achievement_count_visible(total, &unlocked, &locked, &points_total,
      &points_unlocked);
  achievement_add_summary(&info, unlocked, locked, points_total,
      points_unlocked);
  ra_menu_achievement_add_next_rows(&info);
  achievement_add_pinned_section(&slot, total);
  achievement_add_lock_sections(&slot, total, unlocked, locked);

  if (info.ready && unlocked + locked == 0 && g_achievement_filter == 0)
    ra_menu_add_text("No achievements are available for this set.", LITEGRAY);
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}




void ra_menu_set_titleid(const char *titleid) {
  ra_menu_copy_text(g_titleid, sizeof(g_titleid), titleid);
}

const char *ra_menu_titleid(void) { return g_titleid; }

void ra_menu_achievements_defaults(void) {
  g_unlocked_open = 1;
  g_locked_open = 1;
  g_pinned_open = 1;
  g_pin_count = 0;
  g_achievement_filter = 0;
  memset(g_pins, 0, sizeof(g_pins));
}

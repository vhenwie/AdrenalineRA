#include "ra_menu_game.h"
#include "ra_menu_internal.h"
#include "ra_menu_entries.h"
#include "ra_menu_rows.h"
#include "ra_menu_entries_cache.h"
#include "ra_menu_settings.h"
#include "ra_menu_account.h"
#include "ra_menu_nav.h"
#include "ra_menu_leaderboards.h"

#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "ra_http.h"
#include "ra_runtime.h"

static int g_challenge_scroll;
static int g_challenge_first_row = -1;
static int g_challenge_rows;
static char g_game[96];
static char g_game_line[160];
static char g_session_line[160];
static char g_hardcore_line[128];
static char g_presence_line[160];

static int open_achievements(void) {
  return ra_menu_open_page(ra_runtime_subset_count() > 0 ? RA_PAGE_ACHIEVEMENT_SETS
                                                          : RA_PAGE_ACHIEVEMENTS);
}
static int open_leaderboards(void) { return ra_menu_open_page(RA_PAGE_LEADERBOARDS); }
static int open_game_options(void) { return ra_menu_open_page(RA_PAGE_GAME_OPTIONS); }

void ra_menu_add_active_challenges(void) {
  static char rows[RA_MENU_VISIBLE_ROWS][RA_RUNTIME_TITLE_MAX + 64];
  static char tips[RA_MENU_VISIBLE_ROWS][RA_RUNTIME_DESCRIPTION_MAX + 64];
  RaActiveChallengeView list[RA_RUNTIME_MAX_ACTIVE_CHALLENGES];
  int total = 0;
  int rows_left;
  int count;
  int i;

  count = ra_runtime_copy_active_challenges(
    list, RA_RUNTIME_MAX_ACTIVE_CHALLENGES, &total);
  g_challenge_first_row = -1;
  g_challenge_rows = 0;
  if (count <= 0)
    return;

  ra_menu_add_text("--- Active Challenges ---", LITEGRAY);

  rows_left = RA_MENU_VISIBLE_ROWS - ra_menu_entry_count();
  if (rows_left <= 0)
    return;

  /* Keep the window inside the list after a challenge ends. */
  if (g_challenge_scroll > count - rows_left)
    g_challenge_scroll = count - rows_left;
  if (g_challenge_scroll < 0)
    g_challenge_scroll = 0;

  if (rows_left > count)
    rows_left = count;
  for (i = 0; i < rows_left; ++i) {
    const RaActiveChallengeView *view = &list[g_challenge_scroll + i];

    ra_menu_format_challenge(rows[i], sizeof(rows[i]), view->type,
              view->unofficial, view->title, view->progress);
    ra_menu_format_tip(tips[i], sizeof(tips[i]), view->type,
              view->unofficial, view->description, view->points);
    if (g_challenge_first_row < 0)
      g_challenge_first_row = ra_menu_entry_count();

    ra_menu_set_prefix_marker(view->type, view->unofficial);
    ra_menu_add_text_tip(rows[i], WHITE, tips[i]);
    ++g_challenge_rows;
  }
}

/* Move the Active Challenges window instead of the cursor when the player walks
 * past either end of it. Returns 1 when the move was consumed, so menu.c leaves
 * `menu_sel` alone and the rows above the divider never move. */
int ra_menu_nav(int direction, int selected) {
  int total;
  int last_row;

  if (!(ra_menu_page() == RA_PAGE_GAME || (ra_menu_page() == RA_PAGE_ROOT && !ra_menu_is_xmb())) ||
    g_challenge_rows <= 0 || g_challenge_first_row < 0)
    return 0;

  total = ra_runtime_active_challenge_count();
  if (total <= g_challenge_rows)
    return 0; /* everything fits: nothing to scroll */

  last_row = g_challenge_first_row + g_challenge_rows - 1;

  if (direction > 0 && selected >= last_row &&
    g_challenge_scroll + g_challenge_rows < total) {
    ++g_challenge_scroll;
    ra_menu_invalidate_entries();
    return 1;
  }
  if (direction < 0 && selected <= g_challenge_first_row &&
    g_challenge_scroll > 0) {
    --g_challenge_scroll;
    ra_menu_invalidate_entries();
    return 1;
  }
  return 0;
}

/* Gate + snapshot: offline, signed-out and unsupported games finish the page here.
 * Returns 1 when the page has already been filled. */
static int game_page_gate(RaRuntimeInfo *info) {
  if (!ra_menu_settings()->ra_enabled || !ra_menu_account()->signed_in) {
    ra_menu_add_text("RetroAchievements is disabled.", ORANGE);
    ra_menu_add_text("Enable RetroAchievements in Adrenaline menu to use.",
        LITEGRAY);
    return 1;
  }

  if (ra_runtime_offline_at_launch()) {
    ra_menu_add_text("RetroAchievements unavailable.", ORANGE);
    ra_menu_add_text("This game started without a network connection. "
        "Restart the game to connect.", LITEGRAY);
    return 1;
  }

  if (!ra_http_link_up()) {
    ra_menu_add_text("RetroAchievements unavailable.", ORANGE);
    ra_menu_add_text("No network connection. Reconnect, then reopen this menu.",
        LITEGRAY);
    return 1;
  }

  ra_runtime_copy_info(info);

  /* an unsupported game gets the reason and nothing
     * else - no Game/RA State/rich-presence rows, no Achievements/Leaderboards/
     * Options navigation, no Active Challenges list. */
  if (info->unsupported) {
    ra_menu_add_text("RetroAchievements does not support this game.", ORANGE);
    ra_menu_add_text("Achievements and leaderboards are unavailable here.", LITEGRAY);
    ra_menu_add_text("This game is not in the RetroAchievements database.", LITEGRAY);
    return 1;
  }
  return 0;
}

/* Game row, plus the loading report that replaces the rest of the page while the
 * session starts. Returns 1 when the loading report was the whole page. */
static int game_page_add_heading(const RaRuntimeInfo *info) {
  const char *game_title = info->game_title[0] != '\0' ? info->game_title
        : (g_game[0] != '\0' ? g_game : "XMB");

  snprintf(g_game_line, sizeof(g_game_line), "Game: %s (%u)",
      game_title, (unsigned)info->game_id);

  /* While the session loads, this page is a progress report and nothing else
     *: offering Achievements/Leaderboards/Options here
     * would open pages a half-loaded client cannot fill, and the player needs to know
     * that the banner is still coming. The rows reappear on their own when
     * `info.ready` flips, because the entry list rebuilds on the runtime generation
     * and on RA_ENTRIES_REFRESH_US. */
  if (info->running && !info->ready && !info->unsupported) {
    ra_menu_add_text(g_game_line, WHITE);
    ra_menu_add_text("Loading achievements...", LITEGRAY);
    ra_menu_add_text("Wait at the game screen for the session banner.", LITEGRAY);
    return 1;
  }
  return 0;
}

/* Session state row and the hardcore/enforcement row. */
static void game_page_build_session(const RaRuntimeInfo *info) {
  const char *state;

  if (info->ready)
    state = "Ready";
  else if (info->running)
    state = info->status[0] != '\0' ? info->status : "Starting";
  else if (!ra_menu_account()->signed_in)
    state = "Inactive - signed out";
  else if (!ra_menu_settings()->ra_enabled)
    state = "Inactive - achievements off";
  else
    state = "Inactive";
  snprintf(g_session_line, sizeof(g_session_line),
      "Session State: %s (%u/%u Achievements)", state,
      (unsigned)info->unlocked_count, (unsigned)info->achievement_count);

  if (ra_runtime_network_slow()) {
    size_t used = strlen(g_session_line);

    if (used < sizeof(g_session_line))
      snprintf(g_session_line + used, sizeof(g_session_line) - used,
          " - slow network");
  }
  ra_menu_format_mode(g_hardcore_line, sizeof(g_hardcore_line), info,
      ra_menu_settings(), ra_menu_signed_in());
}

/* Rich-presence row, with the ASCII fold used only by the marquee fallback. */
static void game_page_add_presence(const RaRuntimeInfo *info) {
  static char folded[256];
  static int fold_logged;

  ra_menu_copy_text(g_presence_line, sizeof(g_presence_line),
      info->rich_presence[0] != '\0' ? info->rich_presence
                      : "Not available");

  /* The row draws the presence string as written; ra_text renders the eight
         * documented symbols and draws `_` for anything else. The folded string is
         * only the marquee fallback, because the scroller slices by bytes and
         * would cut a multi-byte symbol in half. */
  ra_menu_presence_fold(folded, sizeof(folded), g_presence_line);
  if (ra_menu_presence_dropped() != 0 && !fold_logged) {
    fold_logged = 1;
    ra_menu_log("status: %u symbol(s) folded to ASCII for the marquee\n",
        ra_menu_presence_dropped());
  }
  ra_menu_add_scrolling_text("Status:", g_presence_line,
                folded[0] != '\0' ? folded
                        : "Not available",
                LITEGRAY);
}

void ra_menu_build_game(void) {
  RaRuntimeInfo info;

  if (game_page_gate(&info))
    return;
  if (game_page_add_heading(&info))
    return;

  game_page_build_session(&info);

  ra_menu_add_text(g_game_line, WHITE);
  game_page_add_presence(&info);
  ra_menu_add_text(g_session_line, LITEGRAY);
  ra_menu_add_text(g_hardcore_line, LITEGRAY);
  if (ra_menu_settings()->ra_enabled) {
    ra_menu_add_callback("Achievements >", open_achievements, WHITE);
    if (info.hardcore)
      ra_menu_add_callback("Leaderboards >", open_leaderboards, WHITE);
  }
  ra_menu_add_callback("Options >", open_game_options, WHITE);
  ra_menu_add_active_challenges();
}



void ra_menu_set_game_name(const char *name) {
  ra_menu_copy_text(g_game, sizeof(g_game), name);
}

void ra_menu_reset_active_challenges(void) {
  g_challenge_scroll = 0;
  g_challenge_first_row = -1;
  g_challenge_rows = 0;
}

static int open_banner_options(void) { return ra_menu_open_page(RA_PAGE_BANNER_OPTIONS); }
static int open_active_badge_options(void) { return ra_menu_open_page(RA_PAGE_ACTIVE_BADGE_OPTIONS); }

void ra_menu_build_game_options(void) {
  ra_menu_add_option("Enable Notifications", &ra_menu_settings()->notifications,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  /* Rich Presence has no row: it is always on and always shown in Status. */
  ra_menu_add_option("Enable Next Achievements", &ra_menu_settings()->next_achievement,
      ra_menu_next_achievement_options,
      RA_MENU_NEXT_ACHIEVEMENT_COUNT);
  ra_menu_add_option("Enable Sound Effects", &ra_menu_settings()->sound_effects,
      ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);
  ra_menu_add_option("Enable Active Badges", &ra_menu_settings()->badges, ra_menu_yes_no_options,
      RA_MENU_YES_NO_COUNT);

  if (ra_menu_settings()->notifications) {
    ra_menu_add_callback("Banner Options >", open_banner_options, WHITE);
    if (ra_menu_settings()->badges)
      ra_menu_add_callback("Active Badge Options >",
            open_active_badge_options, WHITE);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

static unsigned int g_dropped;

unsigned int ra_menu_presence_dropped(void) { return g_dropped; }

void ra_menu_presence_fold(char *out, size_t capacity, const char *in) {
  size_t used = 0;
  size_t i = 0;

  if (out == NULL || capacity == 0)
    return;
  out[0] = '\0';
  if (in == NULL)
    return;

  while (in[i] != '\0' && used + 1 < capacity) {
    unsigned char c = (unsigned char)in[i];

    if (c < 0x80) {
      out[used++] = (char)c;
      ++i;
    } else if (c == 0xE2 && (unsigned char)in[i + 1] == 0x80 &&
               (unsigned char)in[i + 2] != '\0') {
      unsigned char t = (unsigned char)in[i + 2];

      if (t == 0x93 || t == 0x94)            /* en/em dash */
        out[used++] = '-';
      else if (t == 0x98 || t == 0x99)       /* curly apostrophe */
        out[used++] = '\'';
      else if (t == 0x9C || t == 0x9D)       /* curly quotes */
        out[used++] = '"';
      else if (t == 0xA6 && used + 3 < capacity) { /* ellipsis */
        out[used++] = '.';
        out[used++] = '.';
        out[used++] = '.';
      } else {
        ++g_dropped;
      }
      i += 3;
    } else {
      /* Skip the whole sequence, but never past the terminator. */
      size_t skip = 1;
      size_t j;

      if ((c & 0xE0) == 0xC0)
        skip = 2;
      else if ((c & 0xF0) == 0xE0)
        skip = 3;
      else if ((c & 0xF8) == 0xF0)
        skip = 4;
      for (j = 1; j < skip; ++j) {
        if (in[i + j] == '\0') {
          skip = j;
          break;
        }
      }
      i += skip;
      ++g_dropped;
    }
  }
  out[used] = '\0';
}

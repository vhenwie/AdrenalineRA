#include "ra_menu_leaderboards.h"
#include "ra_menu_account.h"
#include "ra_menu_entries.h"
#include "ra_menu_nav.h"
#include "ra_menu_settings.h"
#include "ra_menu_internal.h"
#include "ra_menu_rows.h"

#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "rc_client.h"
#include "ra_runtime.h"
#include "ra_settings.h"

static char g_leaderboard_rows[RA_RUNTIME_MAX_LEADERBOARDS][128];
static char g_leaderboard_tips[RA_RUNTIME_MAX_LEADERBOARDS][RA_RUNTIME_DESCRIPTION_MAX + 24];
static char g_leaderboard_summary[48];
static char g_leaderboard_entry_title[RA_RUNTIME_TITLE_MAX];
static char g_leaderboard_entry_summary[64];
static char g_leaderboard_entry_rows[RA_RUNTIME_MAX_LEADERBOARD_ENTRIES][128];
static char g_leaderboard_entry_tips
    [RA_RUNTIME_MAX_LEADERBOARD_ENTRIES][RA_RUNTIME_DESCRIPTION_MAX];
/* Static so a menu row can point at them: `ra_menu_add_text()` stores the pointer and the page is
 * drawn after the builder returns, so a stack buffer would dangle (a disc-based title: the
 * error row rendered as one glyph that changed every frame). */
static char g_lb_page_status[RA_RUNTIME_STATUS_MAX];
static char g_lb_page_error[RA_RUNTIME_STATUS_MAX];
static void leaderboard_tip(const RaLeaderboardView *view, char *out, size_t capacity) {
  ra_menu_copy_text(out, capacity, view->description[0] != '\0'
                ? view->description
                : "Leaderboard details.");
}

void ra_menu_build_leaderboards(void) {
  RaRuntimeInfo info;
  RaLeaderboardView view;
  int count;
  int i;

  ra_runtime_copy_info(&info);
  if (!info.ready) {
    /* `info` is a local copy, so the row must not point into it. */
    ra_menu_copy_text(g_lb_page_status, sizeof(g_lb_page_status), info.status);
    ra_menu_add_text(g_lb_page_status[0] != '\0' ? g_lb_page_status
                      : "RetroAchievements is not running.",
        ORANGE);
    ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
    return;
  }

  if (!info.hardcore) {
    ra_menu_add_text("Leaderboards require Hardcore Mode.", LITEGRAY);
    ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
    return;
  }

  count = ra_runtime_leaderboard_count();
  snprintf(g_leaderboard_summary, sizeof(g_leaderboard_summary),
      "Leaderboards (%d)", count);
  ra_menu_add_text(g_leaderboard_summary, WHITE);

  if (count == 0) {
    ra_menu_add_text("No leaderboards are available for this game.", LITEGRAY);
    ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
    return;
  }

  for (i = 0; i < count && i < RA_RUNTIME_MAX_LEADERBOARDS; ++i) {
    uint32_t color;

    if (!ra_runtime_copy_leaderboard_at(i, &view))
      continue;
    
    snprintf(g_leaderboard_rows[i], sizeof(g_leaderboard_rows[i]), "%s",
        view.title[0] != '\0' ? view.title : "Leaderboard");
    leaderboard_tip(&view, g_leaderboard_tips[i],
            sizeof(g_leaderboard_tips[i]));
    color = view.state == RC_CLIENT_LEADERBOARD_STATE_TRACKING
          ? GREEN
          : (view.state == RC_CLIENT_LEADERBOARD_STATE_ACTIVE
            ? WHITE
            : LITEGRAY);
    ra_menu_add_callback_arg(g_leaderboard_rows[i], ra_open_leaderboard_entries,
            (void*)(uintptr_t)view.id, color,
            g_leaderboard_tips[i]);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

int ra_open_leaderboard_entries(void *data) {
  uint32_t id = (uint32_t)(uintptr_t)data;
  RaLeaderboardView view;
  int count;
  int i;

  if (id == 0)
    return 0;

  g_leaderboard_entry_title[0] = '\0';
  count = ra_runtime_leaderboard_count();
  for (i = 0; i < count; ++i) {
    if (ra_runtime_copy_leaderboard_at(i, &view) && view.id == id) {
      ra_menu_copy_text(g_leaderboard_entry_title,
          sizeof(g_leaderboard_entry_title), view.title);
      break;
    }
  }
  if (g_leaderboard_entry_title[0] == '\0')
    ra_menu_copy_text(g_leaderboard_entry_title, sizeof(g_leaderboard_entry_title),
        "Leaderboard");
  ra_runtime_request_leaderboard_entries(id);
  return ra_menu_open_page(RA_PAGE_LEADERBOARD_ENTRIES);
}

/* One READY board: the top rows in rank order, the player's own row when it is
 * outside them, and the explicit "not ranked" slot. */
static void leaderboard_entries_add_ready(void) {
  RaLeaderboardEntryView entry;
  int board_rows = 0;
  int user_shown = 0;
  int count = 0;
  int i;

  for (i = 0; i < RA_RUNTIME_MAX_LEADERBOARD_ENTRIES; ++i) {
    if (!ra_runtime_copy_leaderboard_entry_at(i, &entry))
      break;
    /* The runtime sends the top RA_RUNTIME_LEADERBOARD_TOP rows in rank order
           * and, only when the player is outside them, the player's own row appended
           * last . The list is never re-anchored: row one is always #1. */
    if (!entry.is_user && board_rows >= RA_RUNTIME_LEADERBOARD_TOP)
      continue;
    if (count >= RA_RUNTIME_MAX_LEADERBOARD_ENTRIES)
      break;

    snprintf(g_leaderboard_entry_rows[count],
        sizeof(g_leaderboard_entry_rows[count]),
        "#%u %s  %s  %s", (unsigned)entry.rank,
        entry.user[0] != '\0' ? entry.user : "Unknown",
        entry.value[0] != '\0' ? entry.value : "-",
        entry.submitted[0] != '\0' ? entry.submitted : "--:--:--");
    ra_menu_fit_text(g_leaderboard_entry_rows[count],
        sizeof(g_leaderboard_entry_rows[count]), 760.0f,
        FONT_SIZE);
    snprintf(g_leaderboard_entry_tips[count],
        sizeof(g_leaderboard_entry_tips[count]),
        entry.is_user ? "Your entry: rank %u"
              : "Rank %u: %s",
        (unsigned)entry.rank,
        entry.user[0] != '\0' ? entry.user : "Unknown");
    ra_menu_add_text_tip(g_leaderboard_entry_rows[count],
          entry.is_user ? GREEN : WHITE,
          g_leaderboard_entry_tips[count]);
    if (entry.is_user)
      user_shown = 1;
    else
      ++board_rows;
    ++count;
  }
  if (!user_shown && count < RA_RUNTIME_MAX_LEADERBOARD_ENTRIES) {
    /* No entry of ours anywhere on the board: show the slot explicitly, so the
           * player sees "not ranked" instead of wondering whether the row loaded. */
    snprintf(g_leaderboard_entry_rows[count],
        sizeof(g_leaderboard_entry_rows[count]),
        "#--- %s --- --:--:--",
        ra_menu_account()->username[0] != '\0' ? ra_menu_account()->username : "You");
    ra_menu_add_text(g_leaderboard_entry_rows[count], LITEGRAY);
    ++count;
  }
  if (count == 0)
    ra_menu_add_text("No leaderboard entries are available.", LITEGRAY);
}

/* The state row(s) for the requested board. */
static void leaderboard_entries_add_status(int state) {
  if (state == RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING) {
    ra_menu_add_text("Loading top 25 entries...", ORANGE);
  } else if (state == RA_RUNTIME_LEADERBOARD_ENTRIES_ERROR) {
    g_lb_page_error[0] = '\0';
    ra_runtime_copy_leaderboard_entries_error(g_lb_page_error,
                        sizeof(g_lb_page_error));
    ra_menu_add_text(g_lb_page_error[0] != '\0' ? g_lb_page_error
                      : "Leaderboard entries failed.",
        ORANGE);
  } else if (state == RA_RUNTIME_LEADERBOARD_ENTRIES_READY) {
    leaderboard_entries_add_ready();
  } else {
    ra_menu_add_text("Leaderboard entries are not available.", LITEGRAY);
  }
}

/* Static so the menu row can point at it: `ra_menu_add_text()` stores the pointer and the page
 * is drawn after the builder returns, so a stack buffer would dangle (a disc-based title:
 * the error row rendered as one glyph that changed every frame). */
void ra_menu_build_leaderboard_entries(void) {
  snprintf(g_leaderboard_entry_summary, sizeof(g_leaderboard_entry_summary),
      "Leaderboard: %s", g_leaderboard_entry_title);
  ra_menu_add_text(g_leaderboard_entry_summary, WHITE);

  leaderboard_entries_add_status(ra_runtime_leaderboard_entries_state());
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}



int ra_open_leaderboards(void) {
  return ra_menu_open_page(RA_PAGE_LEADERBOARDS);
}

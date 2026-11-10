#include "ra_menu_nav.h"

#include "ra_menu.h"
#include "ra_menu_nav.h"
#include "ra_menu_game.h"
#include "ra_menu_entries_cache.h"
#include "ra_menu_internal.h"
#include "ra_menu_account.h"
#include "ra_runtime.h"

static RaPage g_page;
static int g_page_changed;
static int g_is_xmb;

RaPage ra_menu_page(void) { return g_page; }
int ra_menu_is_xmb(void) { return g_is_xmb; }

void ra_menu_set_page(RaPage page) {
  g_page = page;
  g_page_changed = 1;
}

void ra_menu_set_xmb(int xmb) {
  g_is_xmb = xmb != 0;
  g_page = g_is_xmb ? RA_PAGE_ROOT : RA_PAGE_GAME;
  g_page_changed = 1;
}

int ra_menu_take_page_change(void) {
  int changed = g_page_changed;
  g_page_changed = 0;
  return changed;
}

RaPage ra_menu_parent_page(RaPage page, int is_xmb) {
  switch (page) {
  case RA_PAGE_ACCOUNT:
    return is_xmb ? RA_PAGE_ROOT : RA_PAGE_GAME;
  case RA_PAGE_NOTIFICATIONS:
  case RA_PAGE_OPTIONS:
    return RA_PAGE_ROOT;
  case RA_PAGE_BANNER_OPTIONS:
  case RA_PAGE_ACTIVE_BADGE_OPTIONS:
    /* Reachable from both trees: Notifications in the XMB, Options in game. */
    return is_xmb ? RA_PAGE_NOTIFICATIONS : RA_PAGE_GAME_OPTIONS;
  case RA_PAGE_SOUND_EFFECTS:
  case RA_PAGE_DEVELOPER_FEATURES:
    return RA_PAGE_OPTIONS;
  case RA_PAGE_GAME_OPTIONS:
  case RA_PAGE_LEADERBOARDS:
    return RA_PAGE_GAME;
  case RA_PAGE_ACHIEVEMENTS:
    /* With subsets the list sits one level below the set picker. */
    return ra_runtime_subset_count() > 0 ? RA_PAGE_ACHIEVEMENT_SETS
                      : RA_PAGE_GAME;
  case RA_PAGE_ACHIEVEMENT_SETS:
    return RA_PAGE_GAME;
  case RA_PAGE_LEADERBOARD_ENTRIES:
    return RA_PAGE_LEADERBOARDS;
  default:
    return is_xmb ? RA_PAGE_ROOT : RA_PAGE_GAME;
  }
}


static int open_page(RaPage page) {
  if (ra_menu_page() == RA_PAGE_ACCOUNT && page != RA_PAGE_ACCOUNT)
    ra_menu_zero_memory(ra_menu_account()->password, sizeof(ra_menu_account()->password));
  ra_menu_set_page(page);
  ra_menu_reset_active_challenges();
  ra_menu_invalidate_entries();
  ra_menu_set_message(0, NULL);
  return 0;
}


static int ra_open_root(void) {
  return open_page(ra_menu_is_xmb() ? RA_PAGE_ROOT : RA_PAGE_GAME);
}

int ra_menu_open_root(void) {
  return ra_open_root();
}

int ra_menu_back(void) {
  if (ra_menu_page() == RA_PAGE_ROOT || ra_menu_page() == RA_PAGE_GAME)
    return 0;
  if (ra_menu_page() == RA_PAGE_ACCOUNT)
    ra_menu_zero_memory(ra_menu_account()->password, sizeof(ra_menu_account()->password));
  ra_menu_set_page(ra_menu_parent_page(ra_menu_page(), ra_menu_is_xmb()));
  ra_menu_set_message(0, NULL);
  return 1;
}



int ra_menu_open_page(RaPage page) { return open_page(page); }

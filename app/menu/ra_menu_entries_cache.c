#include "ra_menu_entries_cache.h"
#include "ra_menu_entries.h"
#include "ra_menu_nav.h"
#include "ra_menu_account.h"
#include "ra_menu_game.h"
#include "ra_menu_internal.h"
#include "ra_menu_settings.h"
#include "ra_menu_achievements.h"
#include "ra_menu_leaderboards.h"

#include <psp2/kernel/processmgr.h>

#include "menu.h"
#include "ra_runtime.h"

#define RA_ENTRIES_REFRESH_US 500000ULL

static unsigned int g_entries_generation;
static int g_entries_page = -1;
static int g_entries_valid;
static int g_entries_force;
static unsigned long long g_entries_built_us;

void ra_menu_invalidate_entries(void) {
  g_entries_force = 1;
}

/* True while the cached list still describes the current page and runtime data. */
static int entries_cache_fresh(unsigned int generation,
    unsigned long long now) {
  return g_entries_valid && !g_entries_force && g_entries_page == ra_menu_page() &&
    g_entries_generation == generation &&
    (now - g_entries_built_us) < RA_ENTRIES_REFRESH_US;
}

/* Rebuilds the entry list for the page that is open right now. */
static void entries_build_page(void) {
  switch (ra_menu_page()) {
  case RA_PAGE_ACCOUNT:
    ra_menu_build_account();
    break;
  case RA_PAGE_NOTIFICATIONS:
    ra_menu_build_notifications();
    break;
  case RA_PAGE_BANNER_OPTIONS:
    ra_menu_build_banner_options();
    break;
  case RA_PAGE_ACTIVE_BADGE_OPTIONS:
    ra_menu_build_active_badge_options();
    break;
  case RA_PAGE_OPTIONS:
    ra_menu_build_options();
    break;
  case RA_PAGE_SOUND_EFFECTS:
    ra_menu_build_sound_effects();
    break;
  case RA_PAGE_DEVELOPER_FEATURES:
    ra_menu_build_developer_features();
    break;
  case RA_PAGE_GAME_OPTIONS:
    ra_menu_build_game_options();
    break;
  case RA_PAGE_GAME:
    ra_menu_build_game();
    break;
  case RA_PAGE_ACHIEVEMENTS:
    ra_menu_build_achievements();
    break;
  case RA_PAGE_ACHIEVEMENT_SETS:
    ra_menu_build_achievement_sets();
    break;
  case RA_PAGE_LEADERBOARDS:
    ra_menu_build_leaderboards();
    break;
  case RA_PAGE_LEADERBOARD_ENTRIES:
    ra_menu_build_leaderboard_entries();
    break;
  case RA_PAGE_ROOT:
  default:
    if (ra_menu_is_xmb())
      ra_menu_build_root();
    else
      ra_menu_build_game();
    break;
  }
}

MenuEntry *ra_menu_entries(int *count) {
  unsigned int generation;
  unsigned long long now;

  ra_menu_ensure_loaded();

  generation = ra_runtime_data_generation();
  now = (unsigned long long)sceKernelGetSystemTimeWide();
  if (entries_cache_fresh(generation, now)) {
    if (count != NULL)
            *count = ra_menu_entry_count();
    return ra_menu_entries_data();
  }

  g_entries_force = 0;
  g_entries_valid = 1;
  g_entries_page = ra_menu_page();
  g_entries_generation = generation;
  g_entries_built_us = now;
  ra_menu_entries_reset();

  entries_build_page();

  if (count != NULL)
        *count = ra_menu_entry_count();
  return ra_menu_entries_data();
}

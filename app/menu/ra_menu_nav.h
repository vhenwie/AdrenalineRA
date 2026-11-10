#ifndef RA_MENU_NAV_H
#define RA_MENU_NAV_H

#include "ra_menu.h"

typedef enum {
  RA_PAGE_ROOT = 0,
  RA_PAGE_ACCOUNT,
  RA_PAGE_NOTIFICATIONS,
  RA_PAGE_BANNER_OPTIONS,
  RA_PAGE_ACTIVE_BADGE_OPTIONS,
  RA_PAGE_OPTIONS,
  RA_PAGE_SOUND_EFFECTS,
  RA_PAGE_DEVELOPER_FEATURES,
  RA_PAGE_GAME,
  RA_PAGE_GAME_OPTIONS,
  RA_PAGE_ACHIEVEMENTS,
  RA_PAGE_ACHIEVEMENT_SETS,
  RA_PAGE_LEADERBOARDS,
  RA_PAGE_LEADERBOARD_ENTRIES
} RaPage;

RaPage ra_menu_parent_page(RaPage page, int is_xmb);
RaPage ra_menu_page(void);
int ra_menu_is_xmb(void);
void ra_menu_set_page(RaPage page);
void ra_menu_set_xmb(int xmb);
int ra_menu_take_page_change(void);

#endif

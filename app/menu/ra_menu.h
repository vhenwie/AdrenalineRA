#ifndef RA_MENU_H
#define RA_MENU_H

#include "main.h"
#include "menu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RA_MENU_TAB_INDEX 4
#define RA_MENU_VISIBLE_ROWS 14

void ra_menu_tick(const SceAdrenaline *adrenaline);
MenuEntry *ra_menu_entries(int *count);
int ra_menu_open_root(void);
/* In-page list navigation for the in-game root page: before menu.c moves the
 * cursor, the RA tab may consume the move to scroll the `Active Challenges`
 * window instead (the rows above the divider stay put). Returns 1 when consumed. */
int ra_menu_nav(int direction, int selected);

int ra_menu_back(void);
int ra_menu_take_page_change(void);
int ra_menu_achievements_page_active(void);
/* Triangle on the achievement page cycles the category filter (All / Missable /
 * Progression / Win / Unofficial) and remembers it per game. Returns 1 when it
 * consumed the press. */
int ra_menu_filter_cycle(void);
int ra_menu_hardcore_active(void);
int ra_menu_states_mode(void);
int ra_menu_is_busy(void);

void ra_menu_options_changed(void);
void ra_menu_draw_status(void);
/* Shows a one-line status message on the RA menu (colour is a vita2d
 * 0xAABBGGRR value). Used by menu.c when an action is unavailable. */
void ra_menu_show_message(uint32_t color, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* RA_MENU_H */

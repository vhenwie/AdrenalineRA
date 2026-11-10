#ifndef RA_MENU_ACHIEVEMENTS_H
#define RA_MENU_ACHIEVEMENTS_H

#include <stdint.h>

void ra_menu_build_achievements(void);
void ra_menu_build_achievement_sets(void);

/* Row action: toggles the pin for the id carried as callback data. */
int ra_menu_achievements_toggle_pin(void *data);
void ra_menu_achievements_defaults(void);
void ra_menu_sync_achievement_ui(void);
int ra_menu_achievements_page_active(void);
int ra_menu_filter_cycle(void);
void ra_menu_set_achievement_subset(uint32_t id);
int ra_open_achievements(void);
void ra_menu_set_titleid(const char *titleid);
const char *ra_menu_titleid(void);

#endif

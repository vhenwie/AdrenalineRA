#ifndef RA_MENU_SETTINGS_H
#define RA_MENU_SETTINGS_H

#include "ra_settings.h"
#include "ra_menu_nav.h"

void ra_menu_build_notifications(void);
void ra_menu_build_banner_options(void);
void ra_menu_build_active_badge_options(void);
void ra_menu_build_options(void);
void ra_menu_build_sound_effects(void);
void ra_menu_build_developer_features(void);

RaSettings *ra_menu_settings(void);
int ra_menu_open_page(RaPage page);
int ra_menu_save_settings(void);
void ra_menu_reload_settings(void);

#endif

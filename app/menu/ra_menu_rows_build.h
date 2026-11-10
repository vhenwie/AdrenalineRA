#ifndef RA_MENU_ROWS_BUILD_H
#define RA_MENU_ROWS_BUILD_H

#include "ra_runtime.h"

/* Builds one achievement row and its tooltip at `slot`, reusing the previous build when
 * the view has not changed. */
void ra_menu_add_achievement_row(const RaAchievementView *view, int slot);

#endif /* RA_MENU_ROWS_BUILD_H */

#include "ra_menu_achievements.h"

#include <stdio.h>

#include "menu.h"
#include "ra_menu_entries.h"
#include "ra_menu_nav.h"
#include "ra_menu_settings.h"
#include "ra_runtime.h"
#include "ra_server.h"

/* The menu callback type takes no argument, so each set row gets its own tiny
 * selector. Every subset the runtime knows is listed (RA_RUNTIME_MAX_SUBSETS), so a
 * game with five subsets shows five rows instead of the first three. */
#define RA_MENU_SET_ROWS RA_RUNTIME_MAX_SUBSETS

static int base_set_published(void) {
  RaRuntimeInfo info;

  ra_runtime_copy_info(&info);
  return info.has_base_set != 0;
}

static int ra_select_subset_slot(int slot) {
  RaSubsetView subset;

  if (slot == 0) {
    ra_menu_set_achievement_subset(0);
    return ra_menu_open_page(RA_PAGE_ACHIEVEMENTS);
  }
  if (!ra_runtime_copy_subset_at(slot - 1, &subset))
    return 0;
  ra_menu_set_achievement_subset(subset.id);
  return ra_menu_open_page(RA_PAGE_ACHIEVEMENTS);
}

static int ra_select_subset_0(void) { return ra_select_subset_slot(0); }
static int ra_select_subset_1(void) { return ra_select_subset_slot(1); }
static int ra_select_subset_2(void) { return ra_select_subset_slot(2); }
static int ra_select_subset_3(void) { return ra_select_subset_slot(3); }
static int ra_select_subset_4(void) { return ra_select_subset_slot(4); }
static int ra_select_subset_5(void) { return ra_select_subset_slot(5); }
static int ra_select_subset_6(void) { return ra_select_subset_slot(6); }
static int ra_select_subset_7(void) { return ra_select_subset_slot(7); }

/* One source of truth for the set-type labels: core/server/ra_server.c knows the Type
 * strings the payload carries (Base/Bonus/Challenge/Specialty/Exclusive per the RA
 * docs), so the menu does not keep its own copy that can drift. */
static const char *subset_type_name(uint8_t type) {
  return ra_server_set_type_name(type);
}

void ra_menu_build_achievement_sets(void) {
  static char rows[RA_MENU_SET_ROWS + 1][RA_RUNTIME_TITLE_MAX + 16];
  static int (*const selectors[RA_MENU_SET_ROWS + 1])(void) = {
    NULL, /* slot 0 is "Base Set", handled above */
    ra_select_subset_1, ra_select_subset_2,
    ra_select_subset_3, ra_select_subset_4,
    ra_select_subset_5, ra_select_subset_6,
    ra_select_subset_7, ra_select_subset_0 /* index 8 is unused: count <= 8 */
  };
  RaSubsetView subset;
  int count = ra_runtime_subset_count();
  int i;

  ra_menu_add_text_tip("Achievement Sets", LITEGRAY,
        "Choose a set to view its achievements.");

  /* Plain set names; achievement pages carry the exact counts. The site lets a
   * player disable the base set, in which case the server payload has no core
   * set and there is nothing to list for Base. */
  if (base_set_published()) {
    snprintf(rows[0], sizeof(rows[0]), "Base Set");
    ra_menu_add_callback(rows[0], ra_select_subset_0, WHITE);
  }

  if (count > RA_MENU_SET_ROWS)
    count = RA_MENU_SET_ROWS;
  for (i = 0; i < count; ++i) {
    if (!ra_runtime_copy_subset_at(i, &subset))
      break;
    /* `<name> (<type>)`: RA names the set and says what kind it is, and the
         * kind is the only difference between a bonus set and a challenge set the
         * player is meant to tell apart. */
    snprintf(rows[i + 1], sizeof(rows[i + 1]), "%s (%s)", subset.title,
        subset_type_name(subset.type));
    ra_menu_add_callback(rows[i + 1], selectors[i + 1], WHITE);
  }
  ra_menu_add_callback("Back", ra_menu_back, LITEGRAY);
}

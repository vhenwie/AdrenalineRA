#include "ra_menu_entries.h"
#include "ra_menu_tooltips.h"

#include <string.h>

#include "menu.h"
#include "rc_client.h"

/* Marker hue for the next row; see ra_type_marker_color(). */
static uint32_t g_pending_prefix_color;
static MenuEntry g_entries[RA_MENU_MAX_ENTRIES];
static int g_entry_count;

static MenuEntry *add_entry(const char *name, int type, uint32_t color,
              int (*callback)(), uint8_t *value,
              char **options, int n_options) {
  MenuEntry *entry;

  if (g_entry_count >= RA_MENU_MAX_ENTRIES)
    return NULL;

  entry = &g_entries[g_entry_count++];
  entry->name = (char*)name;
  entry->type = type;
  entry->color = color;
  entry->callback = callback;
  entry->value = value;
  entry->options = options;
  entry->n_options = n_options;
  entry->tooltip = ra_menu_tooltip_for(name);
  entry->scroll_text = NULL;
  entry->scroll_fallback = NULL;
  entry->hold_ms = 0;
  entry->callback_arg = NULL;
  entry->callback_data = NULL;
  /* Colour plumbing: reset per row, so a value set on a previous build of the
     * same slot cannot leak into a new one. 0 = unchanged behaviour. */
  entry->tooltip_color = 0;
  entry->value_color = 0;
  entry->prefix_color = g_pending_prefix_color;
  g_pending_prefix_color = 0;
  entry->dot_color = 0;
  return entry;
}

void ra_menu_add_text(const char *name, uint32_t color) {
  add_entry(name, MENU_ENTRY_TYPE_TEXT, color, NULL, NULL, NULL, 0);
}

void ra_menu_add_text_tip(const char *name, uint32_t color, const char *tip) {
  MenuEntry *entry = add_entry(name, MENU_ENTRY_TYPE_TEXT, color, NULL,
                NULL, NULL, 0);
  if (entry != NULL)
    entry->tooltip = tip;
}

/* Marker hue by achievement type; 0 for standard achievements. */
uint32_t ra_menu_type_marker_color(uint8_t type, int unofficial) {
  if (unofficial)
    return RA_COLOR_TYPE_UNSUPPORTED;
  switch (type) {
  case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
    return RA_COLOR_TYPE_MISSABLE;
  case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
    return RA_COLOR_TYPE_WIN;
  case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
    return RA_COLOR_TYPE_PROGRESS;
  default:
    return 0;
  }
}
void ra_menu_set_prefix_marker(uint8_t type, int unofficial) {
  g_pending_prefix_color = ra_menu_type_marker_color(type, unofficial);
}
/* Fold rich presence to ASCII: the PGF font draws unknown codepoints as an underscore. */


void ra_menu_add_scrolling_text(const char *label, const char *value,
                    const char *fallback, uint32_t color) {
  MenuEntry *entry = add_entry(label, MENU_ENTRY_TYPE_TEXT, color, NULL,
                NULL, NULL, 0);
  if (entry != NULL) {
    entry->scroll_text = value;
    entry->scroll_fallback = fallback;
  }
}

MenuEntry *ra_menu_add_option(const char *name, uint8_t *value, char **options,
              int n_options) {
  return add_entry(name, MENU_ENTRY_TYPE_OPTION, WHITE, NULL, value, options,
          n_options);
}

void ra_menu_add_callback(const char *name, int (*callback)(), uint32_t color) {
  add_entry(name, MENU_ENTRY_TYPE_CALLBACK, color, callback, NULL, NULL, 0);
}

void ra_menu_add_callback_arg(const char *name, int (*callback)(void*),
              void *data, uint32_t color, const char *tip) {
  MenuEntry *entry = add_entry(name, MENU_ENTRY_TYPE_CALLBACK, color, NULL,
                NULL, NULL, 0);
  if (entry != NULL) {
    entry->callback_arg = callback;
    entry->callback_data = data;
    entry->tooltip = tip;
  }
}

void ra_menu_add_hold_callback(const char *name, int (*callback)(), uint32_t color,
              int hold_ms) {
  MenuEntry *entry = add_entry(name, MENU_ENTRY_TYPE_CALLBACK, color,
                callback, NULL, NULL, 0);
  if (entry != NULL)
    entry->hold_ms = hold_ms;
}



int ra_menu_entry_count(void) { return g_entry_count; }
MenuEntry *ra_menu_entries_data(void) { return g_entries; }
void ra_menu_entries_reset(void) { g_entry_count = 0; }

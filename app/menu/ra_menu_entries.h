#ifndef RA_MENU_ENTRIES_H
#define RA_MENU_ENTRIES_H

#include <stdint.h>
#include "menu.h"

#ifndef RA_MENU_MAX_ENTRIES
#define RA_MENU_MAX_ENTRIES 512
#endif

MenuEntry *ra_menu_add_option(const char *name, uint8_t *value, char **options,
                              int n_options);
void ra_menu_add_text(const char *name, uint32_t color);
void ra_menu_add_text_tip(const char *name, uint32_t color, const char *tip);
void ra_menu_add_scrolling_text(const char *label, const char *value,
                                const char *fallback, uint32_t color);
void ra_menu_add_callback(const char *name, int (*callback)(), uint32_t color);
void ra_menu_add_callback_arg(const char *name, int (*callback)(void *),
                              void *data, uint32_t color, const char *tip);
void ra_menu_add_hold_callback(const char *name, int (*callback)(),
                               uint32_t color, int hold_ms);
void ra_menu_set_prefix_marker(uint8_t type, int unofficial);
uint32_t ra_menu_type_marker_color(uint8_t type, int unofficial);
int ra_menu_entry_count(void);
MenuEntry *ra_menu_entries_data(void);
void ra_menu_entries_reset(void);

#endif

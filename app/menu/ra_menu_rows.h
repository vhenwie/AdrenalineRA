#ifndef RA_MENU_ROWS_H
#define RA_MENU_ROWS_H

#include <stddef.h>
#include <stdint.h>

int ra_menu_progress_percent(const char *progress);
void ra_menu_type_labels(uint8_t type, int unofficial,
                         const char **marker, const char **prefix);
void ra_menu_fit_row(char *out, size_t capacity, const char *prefix,
                     const char *title, const char *tail, float max_width);
void ra_menu_format_achievement(char *out, size_t capacity, uint8_t type,
                                int unofficial, const char *title,
                                const char *progress);
void ra_menu_format_challenge(char *out, size_t capacity, uint8_t type,
                              int unofficial, const char *title,
                              const char *progress);
void ra_menu_format_next(char *out, size_t capacity, const char *label,
                         uint8_t type, int unofficial, const char *title);
void ra_menu_format_tip(char *out, size_t capacity, uint8_t type,
                        int unofficial, const char *description,
                        uint32_t points);

#endif

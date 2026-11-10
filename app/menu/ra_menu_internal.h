#ifndef RA_MENU_INTERNAL_H
#define RA_MENU_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "ra_settings.h"
#include "ra_runtime.h"

int ra_menu_busy_or_message(void);

void ra_menu_ensure_loaded(void);

void ra_menu_build_root(void);

void ra_menu_log(const char *format, ...);

void ra_menu_set_message(uint32_t color, const char *format, ...);
void ra_menu_show_prompt_error(void);
void ra_menu_show_message(uint32_t color, const char *text);
const char *ra_menu_message_text(void);
uint32_t ra_menu_message_color(void);

void ra_menu_format_mode(char *destination, size_t capacity,
                         const RaRuntimeInfo *info, const RaSettings *settings,
                         int signed_in);

extern char *ra_menu_yes_no_options[];
extern char *ra_menu_session_mode_options[];
extern char *ra_menu_no_yes_options[];
extern char *ra_menu_leaderboard_options[];
extern char *ra_menu_summary_options[];
extern char *ra_menu_next_achievement_options[];
extern char *ra_menu_sound_options[];
extern char *ra_menu_anchor_options[];
extern char *ra_menu_padding_options[];
extern char *ra_menu_indicator_size_options[];

#define RA_MENU_YES_NO_COUNT 2
#define RA_MENU_SESSION_MODE_COUNT 3
#define RA_MENU_NO_YES_COUNT 2
#define RA_MENU_LEADERBOARD_COUNT 4
#define RA_MENU_SUMMARY_COUNT 3
#define RA_MENU_NEXT_ACHIEVEMENT_COUNT 3
#define RA_MENU_SOUND_COUNT 3
#define RA_MENU_ANCHOR_COUNT 6
#define RA_MENU_PADDING_COUNT 5
#define RA_MENU_INDICATOR_SIZE_COUNT 3

void ra_menu_presence_fold(char *out, size_t capacity, const char *in);
unsigned int ra_menu_presence_dropped(void);

int ra_menu_prompt(const char *title, char *buffer, size_t capacity,
                   int sensitive);

const char *ra_menu_points_word(unsigned int points);
const char *ra_menu_points_word_lower(unsigned int points);

void ra_menu_fit_text(char *text, size_t capacity, float max_width, float scale);

void ra_menu_copy_text(char *destination, size_t capacity, const char *source);
void ra_menu_zero_memory(void *data, size_t length);

/* The Next/Then suggestion rows under the achievement summary. */
void ra_menu_achievement_add_next_rows(const RaRuntimeInfo *info);

void ra_menu_build_game_options(void);

#endif /* RA_MENU_INTERNAL_H */

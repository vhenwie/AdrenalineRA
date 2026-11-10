/*
	Adrenaline
	Copyright (C) 2016-2018, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __MENU_H__
#define __MENU_H__

#include <vita2d.h>

enum Colors {
	// Primary colors
	RED             = 0xFF0000FF,
	GREEN           = 0xFF00FF00,
	BLUE            = 0xFFFF0000,
	// Secondary colors
	CYAN            = 0xFFFFFF00,
	MAGENTA         = 0xFFFF00FF,
	YELLOW          = 0xFF44EEFF,
	// Tertiary colors
	AZURE           = 0xFFFF7F00,
	VIOLET          = 0xFFFF007F,
	ROSE            = 0xFF7F00FF,
	ORANGE          = 0xFF007FFF,
	CHARTREUSE      = 0xFF00FF7F,
	SPRING_GREEN    = 0xFF7FFF00,
	GOLD            = 0xFF4FC3F7,
	// Grayscale
	WHITE           = 0xFFFFFFFF,
	LITEGRAY        = 0xFFBFBFBF,
	GRAY            = 0xFF7F7F7F,
	DARKGRAY        = 0xFF3F3F3F,
	BLACK           = 0xFF000000
};

#define NOALPHA 0xFF

#define COLOR_ALPHA(color, alpha) (color & 0x00FFFFFF) | ((alpha & 0xFF) << 24)

#define SCREEN_LINE 1024
#define SCREEN_WIDTH 960
#define SCREEN_HEIGHT 544

#define ALIGN_CENTER(a, b) ((a - b) / 2)
#define ALIGN_RIGHT(x, w) ((x) - (w))

#define WINDOW_WIDTH 840.0f
#define WINDOW_HEIGHT 476.0f
#define WINDOW_X ALIGN_CENTER(SCREEN_WIDTH, WINDOW_WIDTH)
#define WINDOW_Y ALIGN_CENTER(SCREEN_HEIGHT, WINDOW_HEIGHT)
#define WINDOW_COLOR COLOR_ALPHA(BLACK, 0xAF)

#define FONT_SIZE 1.0f
#define FONT_Y_SPACE 23.0f

#define FONT_Y_LINE(y) (40.0f + (y) * FONT_Y_SPACE)

#define pgf_draw_text(x, y, color, scale, text) \
	vita2d_pgf_draw_text(font, x, (y) + 20, color, scale, text)

#define pgf_draw_textf(x, y, color, scale, ...) \
	vita2d_pgf_draw_textf(font, x, (y) + 20, color, scale, __VA_ARGS__)

enum MenuEntryTypes {
	MENU_ENTRY_TYPE_TEXT,
	MENU_ENTRY_TYPE_OPTION,
	MENU_ENTRY_TYPE_SCALE,
	MENU_ENTRY_TYPE_CALLBACK,
};

/* --- colour plumbing (docs/55-ra-menu-colors.md §7) ------------------------
 * Mechanism only: these fields are honoured by the draw path, and every one of
 * them defaults to 0 = "unchanged behaviour". No row sets a value yet, so the UI
 * looks exactly as it does today until the colour map is drafted.
 *
 * The palette is defined here so a row can be given a role without inventing a
 * literal at the call site; values are vita2d 0xAABBGGRR, comment gives the RGB. */
#define RA_COLOR_LABEL      0xFFFFFFFFu /* #FFFFFF row titles, navigation */
#define RA_COLOR_VALUE      0xFFD2CDC9u /* #C9CDD2 values, descriptions */
#define RA_COLOR_HINT       0xFF9B928Bu /* #8B929B disabled, Back, diagnostics */
#define RA_COLOR_SECTION    0xFFBDB2A8u /* #A8B2BD section headers */
#define RA_COLOR_OK         0xFF84DC3Du /* #3DDC84 unlocked / on */
#define RA_COLOR_HARDCORE   0xFF42C5F5u /* #F5C542 hardcore unlock, points */
#define RA_COLOR_LOCKED     0xFFC7BEB7u /* #B7BEC7 locked */
#define RA_COLOR_PINNED     0xFFFC84C0u /* #C084FC pinned / tracked */
#define RA_COLOR_ACTIVE     0xFF5C5CFFu /* #FF5C5C live challenge or board */
#define RA_COLOR_PROGRESS   0xFFFFA34Fu /* #4FA3FF progress, info, [P] */
#define RA_COLOR_WARN       0xFF20B0FFu /* #FFB020 destructive, hold */
#define RA_COLOR_MODE       0xFF187AFFu /* #FF7A18 hardcore mode chip */
#define RA_COLOR_MARK_M     0xFF4C8AFFu /* #FF8A4C [M] missable */
#define RA_COLOR_MARK_U     0xFFA6A09Au /* #9AA0A6 [U] unofficial */
#define RA_COLOR_DISABLED   0xFF78706Bu /* #6B7078 inert rows */
#define RA_COLOR_TRACK      0xFF332E2Au /* #2A2E33 progress track */

/* 00.10.151: toggles and type markers (docs/59 �65). Kept apart from the row palette
 * above because that one is shared with the achievement list, and the spec recolours only
 * the toggles and the bracketed type tokens. Format is 0xAABBGGRR like the rest of this
 * table. An "off" toggle is the same RGB desaturated, derived at the call site, so it can
 * never drift from its "on" colour. */
#define RA_COLOR_TOGGLE_FILTER    0xFFF7C34Fu /* #4FC3F7 sky blue */
#define RA_COLOR_TOGGLE_LOCKED    0xFF6B7FFFu /* #FF7F6B coral */
#define RA_COLOR_TOGGLE_UNLOCKED  0xFFEED322u /* #22D3EE cyan */
#define RA_COLOR_TOGGLE_PINNED    0xFFC069FFu /* #FF69C0 hot pink */
#define RA_COLOR_TYPE_MISSABLE    0xFF8AB0FFu /* #FFB08A peach */
#define RA_COLOR_TYPE_WIN         0xFFB0E37Bu /* #7BE3B0 mint */
#define RA_COLOR_TYPE_PROGRESS    0xFFF2A7B9u /* #B9A7F2 lavender */
#define RA_COLOR_TYPE_UNSUPPORTED 0xFF7E73B4u /* #B4737E dusty rose */

/* `value_color` sentinel: "on" values in RA_COLOR_OK, "off" in RA_COLOR_HINT,
 * decided at draw time from the stored value. */
#define RA_COLOR_TOGGLE     0x00000001u

typedef struct {
	char *name;
	int type;
	uint32_t color;
	int (* callback)();
	uint8_t *value;
	char **options;
	int n_options;
	const char *tooltip;
	/* When set, the row draws `name` at a fixed position and marquees this
	 * value in the remaining width (used for rich presence). */
	const char *scroll_text;
	/* Drawn instead of `scroll_text` when the value does not fit and has to
	 * scroll: the scroller slices strings by bytes, so a value holding
	 * multi-byte symbols scrolls as this ASCII form. When it fits, the value
	 * itself is drawn and the symbols come from the baked atlas
	 * (ra_glyphs.c / ra_text.c). */
	const char *scroll_fallback;
	/* >0 requires holding the confirm button this many milliseconds. */
	int hold_ms;
	/* Optional callback receiving row-specific context (leaderboard ID). */
	int (* callback_arg)(void *);
	void *callback_data;
	/* --- colour plumbing (docs/55-ra-menu-colors.md §7) ---------------------
	 * All four default to 0, which means "unchanged behaviour": tooltip ORANGE,
	 * option value GREEN, no marker split, no state dot. Nothing sets them yet. */
	uint32_t tooltip_color; /* tooltip/footer text                          */
	uint32_t value_color;   /* option value; RA_COLOR_TOGGLE = on/off auto   */
	uint32_t prefix_color;  /* leading `[X] ` marker inside `name`          */
	uint32_t dot_color;     /* 6 px state dot left of the row               */
} MenuEntry;

typedef struct {
	char *name;
	MenuEntry *entries;
	int n_entries;
	int moveable;
	int scroll;
} TabEntry;

extern vita2d_pgf *font;

extern int language, enter_button, date_format, time_format;
extern int menu_open;

int ExitAdrenalineMenu(void);

int AdrenalineDraw(SceSize args, void *argp);
int ScePspemuCustomSettingsHandler(int a1, int a2, int a3, int a4);

#endif

/* 00.10.152/154: resolve a toggle row''s colour from its name and value (on = the hue, off = the
 * same RGB dimmed). Defined in menu.c; ra_menu.c draws the achievement section headers. */
uint32_t ra_menu_toggle_color(const char* name, uint8_t value);
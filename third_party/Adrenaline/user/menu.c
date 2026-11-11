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

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/ime_dialog.h>
#include <psp2/system_param.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <vita2d.h>

#include "main.h"
#include "menu.h"
#include "pops.h"
#include "states.h"
#include "usb.h"
#include "ra_badge.h"

#include "utils.h"
#include "ra_log.h"
#include "ra_logqueue.h"
#include "ra_menu.h"
#include "ra_notify.h"
#include "ra_runtime.h"
#include "ra_text.h"
#include "math_utils.h"
#include "../adrenaline_version.h"

#include "includes/lcd3x_v.h"
#include "includes/lcd3x_f.h"
#include "includes/texture_v.h"
#include "includes/texture_f.h"

/* pspemu's POPS surface; defined below with the menu textures. */
extern void *pops_data;
#include "includes/opaque_v.h"
#include "includes/sharp_bilinear_f.h"
#include "includes/sharp_bilinear_v.h"
#include "includes/sharp_bilinear_simple_f.h"
#include "includes/sharp_bilinear_simple_v.h"
#include "includes/advanced_aa_v.h"
#include "includes/advanced_aa_f.h"
#include "includes/scale2x_v.h"
#include "includes/scale2x_f.h"
#include "includes/scale2x_plus_v.h"
#include "includes/scale2x_plus_f.h"
#include "includes/scale3x_v.h"
#include "includes/scale3x_f.h"
#include "includes/vflux_f.h"
#include "includes/vflux_v.h"

static const SceGxmProgram *const gxm_program_vflux_v = (SceGxmProgram*)&vflux_v;
static const SceGxmProgram *const gxm_program_vflux_f = (SceGxmProgram*)&vflux_f;

extern SceGxmContext *_vita2d_context;
int sceCommonDialogIsRunning();

vita2d_pgf *font;
vita2d_texture* overlay_texture = NULL;

static vita2d_texture* g_dialog_snapshots[2] = { NULL, NULL };
static int g_dialog_snapshot_slot = 0;
static int g_dialog_snapshot_ready = 0;
static int g_dialog_snapshot_attempted = 0;
static int g_dialog_present_frames = 0;
/* Frames left to log after the system quit dialog closes, so the presented branch is
 * visible in the log (diagnostic only). */
static int g_quit_log_frames = 0;

static int capture_dialog_snapshot(void);
static void ra_menu_log(const char *fmt, ...);
/* 00.10.128: the same line, drained to the card before the frame continues. The
 * `settings:` hand-over lines below use it and precede the definition. */
static void ra_menu_log_sync(const char *fmt, ...);

void AdrenalineMenuResetDialogSnapshot(void) {
	/* Alternate textures: the previous dialog may still reference the last
	 * snapshot when a second prompt opens, so never overwrite it in place. */
	g_dialog_snapshot_slot ^= 1;
	g_dialog_snapshot_ready = 0;
	g_dialog_snapshot_attempted = 0;
	g_dialog_present_frames = 0;
	vita2d_wait_rendering_done();
	if (!capture_dialog_snapshot())
		ra_menu_log("dialog snapshot failed\n");
}

int language = 0, enter_button = 0, date_format = 0, time_format = 0;

int g_direct_sharp_scale_active = 0;

static int EnterStandbyMode();
static int OpenOfficialSettings();
static int ExitPspEmuApplication();
static int ResetAdrenalineSettings();

// RGB colors for the filter box used by f.lux
static float flux_colors[] = {
	1.0f, 0.5f, 0.0f, 0.0f, // Yellow
	0.0f, 0.0f, 0.5f, 0.0f, // Blue
	0.0f, 0.0f, 0.0f, 0.0f  // Black
};

static char *graphics_options[] = { "Original", "Bilinear", "Sharp bilinear", "Advanced AA", "LCD3x", "Sharp bilinear without scanlines", "Scale2x", "Scale2x Plus", "Scale3x"};
static char *flux_mode_options[] = { "None", "Yellow", "Blue", "Black" };
static char *no_yes_options[] = { "No", "Yes" };
static char *yes_no_options[] = { "Yes", "No" };
static char *ms_location_options[] = { "ux0:pspemu", "ur0:pspemu", "imc0:pspemu", "xmc0:pspemu", "uma0:pspemu", "uma0:" };
static char *usbdevice_options[] = { "Memory Card", "Internal Storage", "sd2vita", "psvsd" };
static char *cfwtype_options[] = { "Epinephrine (EPI)", "ARK" };
static char *ef_location_options[] = { "Disabled", "uma0:pspemu", "xmc0:pspemu", "ux0:pspemu", "uma0:" };

static MenuEntry main_entries[] = {
	{ "Enter Standby Mode",        MENU_ENTRY_TYPE_CALLBACK, WHITE, EnterStandbyMode, NULL, NULL, 0 },
	{ "Open Official Settings",    MENU_ENTRY_TYPE_CALLBACK, WHITE, OpenOfficialSettings, NULL, NULL, 0 },
	{ "Exit PspEmu Application",   MENU_ENTRY_TYPE_CALLBACK, WHITE, ExitPspEmuApplication, NULL, NULL, 0 },
	{ "Exit Adrenaline Menu",      MENU_ENTRY_TYPE_CALLBACK, WHITE, ExitAdrenalineMenu, NULL, NULL, 0 },
};

static MenuEntry settings_entries[] = {
	{ "vPSP Custom Firmware",      MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.cfw_type, cfwtype_options, sizeof(cfwtype_options)/sizeof(char*) },
	{ "", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "Graphics Filtering",        MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.graphics_filtering, graphics_options, sizeof(graphics_options) / sizeof(char **) },
	{ "Smooth Graphics",           MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.no_smooth_graphics, yes_no_options, sizeof(yes_no_options) / sizeof(char **) },
	{ "f.lux Filter Color",        MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.flux_mode, flux_mode_options, sizeof(flux_mode_options) / sizeof(char **) },
	{ "Screen Scale X (PSP)",      MENU_ENTRY_TYPE_SCALE,  0, NULL, (uint8_t *)&config.psp_screen_scale_x, NULL, 0 },
	{ "Screen Scale Y (PSP)",      MENU_ENTRY_TYPE_SCALE,  0, NULL, (uint8_t *)&config.psp_screen_scale_y, NULL, 0 },
	{ "Screen Scale X (PS1)",      MENU_ENTRY_TYPE_SCALE,  0, NULL, (uint8_t *)&config.ps1_screen_scale_x, NULL, 0 },
	{ "Screen Scale Y (PS1)",      MENU_ENTRY_TYPE_SCALE,  0, NULL, (uint8_t *)&config.ps1_screen_scale_y, NULL, 0 },
	{ "", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "Memory Stick Location",     MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.ms_location, ms_location_options, sizeof(ms_location_options) / sizeof(char **) },
	{ "System Storage Location",   MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.ef_location, ef_location_options, sizeof(ef_location_options) / sizeof(char **) },
	{ "USB device",                MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.usbdevice, usbdevice_options, sizeof(usbdevice_options) / sizeof(char **) },
	{ "Skip Adrenaline Boot Logo", MENU_ENTRY_TYPE_OPTION, 0, NULL, &config.skip_logo, no_yes_options, sizeof(no_yes_options) / sizeof(char **) },
	{ "Reset Adrenaline Settings", MENU_ENTRY_TYPE_CALLBACK, YELLOW, ResetAdrenalineSettings, NULL, NULL, 0 },
};

static MenuEntry about_entries[] = {
	#if defined(NIGHTLY) && NIGHTLY == 1
	{ "6.61 Adrenaline-" ADRENALINE_VERSION_MAJOR_STR "." ADRENALINE_VERSION_MINOR_STR "." ADRENALINE_VERSION_MICRO_STR "-" EPI_NIGHTLY_VER, MENU_ENTRY_TYPE_TEXT, ORANGE, NULL, NULL, NULL, 0 },
	#else
	{ "6.61 Adrenaline-" ADRENALINE_VERSION_MAJOR_STR "." ADRENALINE_VERSION_MINOR_STR "." ADRENALINE_VERSION_MICRO_STR, MENU_ENTRY_TYPE_TEXT, ORANGE, NULL, NULL, NULL, 0 },
	#endif
	{ "by Cat and GrayJack", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "Credits", MENU_ENTRY_TYPE_TEXT, ORANGE, NULL, NULL, NULL, 0 },
	{ "Team molecule for HENkaku", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "TheFloW for starting Adrenaline", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "frangarcj for graphics filtering", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "Rinnegatamante for f.lux", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "xerpi for vita2dlib", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "qwikrazor87 for DRM RE", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
	{ "ARK CFW team for Inferno v2, collaboration, and more", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
//   { "Cat for featurrrs", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
//   { "GrayJack for hyperfocus time", MENU_ENTRY_TYPE_TEXT, WHITE, NULL, NULL, NULL, 0 },
};

static TabEntry tab_entries[] = {
	{ "Main", main_entries, sizeof(main_entries) / sizeof(MenuEntry), 1 },
	{ "States", NULL, 0, 0 },
	{ "Settings", settings_entries, sizeof(settings_entries) / sizeof(MenuEntry), 1 },
	{ "About", about_entries, sizeof(about_entries) / sizeof(MenuEntry), 0 },
	{ "RA", NULL, 0, 1 },
};

#define N_TABS (sizeof(tab_entries) / sizeof(TabEntry))
#define TAB_SIZE (WINDOW_WIDTH / N_TABS)

static AdrenalineConfig old_config;
static int tab_sel = 0;
static int menu_sel = 0;
int menu_open = 0;
/* 00.10.131: set by `ExitPspEmuApplication()` before it tears anything down. The
 * menu-close path uses it to stop doing display work of its own: the exit path does
 * exactly one hand-back, and a second one racing pspemu's teardown is what froze the
 * console (`docs/59` §47). */
static volatile int g_exiting;
/* 00.10.129: pspemu's settings hook runs on pspemu's thread, and it used to call
 * `EnterAdrenalineMenu()` from there while the frame thread was in the middle of its own
 * work. `RA_SETUP_20261029_214634_541.log` shows the two threads logging at the same
 * instant (three lines spliced together: `pad: PS winmenu: open: converting pspemu a…`),
 * so the menu-open path and the frame loop were genuinely running in parallel - and the
 * process died inside that path. The hook now leaves a request; the frame thread opens
 * the menu, because it is the thread that owns the pspemu pointers, the pops pause and
 * the runtime. */
static volatile int g_menu_open_request;
static unsigned int g_tooltip_focus_generation;
static int g_hide_menu = 0;

static int changed = 0;
static int open_official_settings = 0;
/* Which session set that flag (00.10.123). It is a static, so it outlives the game session;
 * if the player opens pspemu's settings and then leaves to the XMB without coming back to
 * the menu, the flag is still set on the next launch and the first `menu: open` of the new
 * session would take the settings-close path. */
static int open_official_settings_session = -1;

/* 00.60.01: Open Official Settings hands control back to the handler that opened
 * this menu. When the menu came from the L+R+Select shortcut there is no such
 * handler, so the row used to close the menu and appear to do nothing. Track the
 * source so the menu can say why instead. */
static int g_menu_from_settings_hook;

/* 00.60.01: pspemu's own settings UI is a common dialog too. Without this flag
 * the quit watcher treated "Open Official Settings" as the quit screen: it
 * stopped the session on the transition and reopened it on cancel (`settings:
 * official settings opened` -> `quit: system dialog up ... session closed first`
 * -> `quit: cancelled ... reopening the session`). */
static int g_official_settings_dialog;
static int g_official_settings_seen;
static unsigned long long g_official_settings_us;

static SceUID settings_semaid = -1;

static int EnterStandbyMode() {
	stopUsb(usbdevice_modid);
	ExitAdrenalineMenu();
	scePowerRequestSuspend();
	return 0;
}

static void SaveAdrSetting() {
	config.magic[0] = ADRENALINE_CFG_MAGIC_1;
	config.magic[1] = ADRENALINE_CFG_MAGIC_2;
	WriteFile("ux0:data/" ADRENALINE_TITLEID "/adrenaline.bin", &config, sizeof(AdrenalineConfig));

	SceIoStat stat;
	if (sceIoGetstat("ux0:app/" ADRENALINE_TITLEID "/adrenaline.bin", &stat) >= 0) {
		WriteFile("ux0:app/" ADRENALINE_TITLEID "/adrenaline.bin", &config, sizeof(AdrenalineConfig));
	}
}

static int OpenOfficialSettings() {
	if (!g_menu_from_settings_hook) {
		ra_menu_log_sync("settings: Open Official Settings needs the pspemu settings hook; "
		                 "menu shortcut path leaves it closed\n");
		ra_menu_show_message(YELLOW,
		                     "Open Settings from pspemu's own Settings entry to use this.");
		return 0;
	}
	/* The POPS "Switch Discs" UI lives here (operator's swap method, 2026-10-28): we hand
	 * the screen to pspemu's own settings and cannot see the disc index it picks. The line
	 * marks the window in the log, which is what the multi-disc work needs to correlate
	 * against. */
	ra_menu_log_sync("settings: official settings opened (Switch Discs happens here)\n");
	open_official_settings = 1;
	open_official_settings_session = ra_runtime_session_ordinal();
	/* 00.10.125: the other crash the operator can trigger in a second session. This call
	 * hands the screen to pspemu, so a core after these two lines is pspemu's own settings
	 * UI faulting rather than our menu. */
	ra_menu_log_sync("settings: handing the screen back to pspemu (session=%d)\n",
	            open_official_settings_session);
	g_official_settings_dialog = 1;
	g_official_settings_seen = 0;
	g_official_settings_us = (unsigned long long)sceKernelGetSystemTimeWide();
	ExitAdrenalineMenu();
	ra_menu_log_sync("settings: pspemu has the screen\n");
	return 0;
}

/* PSP and POPS do not hand the scanout back to the same surface.
 *
 * PSP: pspemu renders into SCE_PSPEMU_FRAMEBUFFER.
 * POPS: its compat query answers with pops_data, so the POPS exit screen is
 *       painted there; pointing the scanout at VRAM leaves a black surface
 *       while the emulated PSP is still running.
 *
 * Kept as two functions on purpose: a change to one mode's exit must not be
 * able to move the other mode's hand-back target. */
static void exit_display_handback(int pops, void *target) {
	SceDisplayFrameBuf fb;
	char line[128];

	sceGxmDisplayQueueFinish();
	SetPspemuFrameBuffer(target);
	sceDisplayWaitVblankStart();

	if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) >= 0)
		snprintf(line, sizeof(line),
		         "ra_exit: display handed back (scanout=%08x target=%08x pops=%d)\n",
		         (unsigned)(uintptr_t)fb.base, (unsigned)(uintptr_t)target, pops);
	else
		snprintf(line, sizeof(line),
		         "ra_exit: display handback queried failed (target=%08x pops=%d)\n",
		         (unsigned)(uintptr_t)target, pops);
	ra_logq_direct(line);
	ra_logq_flush();
}

static void exit_display_handback_psp(void) {
	exit_display_handback(0, (void *)SCE_PSPEMU_FRAMEBUFFER);
}

static void exit_display_handback_pops(void) {
	exit_display_handback(1, pops_data);
}

static int ExitPspEmuApplication() {
	int exit_pops = ra_runtime_last_title_was_pops();
	stopUsb(usbdevice_modid);
	/* 00.10.131: close our own menu first, and say that the exit path owns the
	 * display hand-back.
	 *
	 * The order used to be: leave_game -> badge shutdown -> GXM finish ->
	 * SetPspemuFrameBuffer -> vblank -> ExitAdrenalineMenu() -> ErrorExit. So the
	 * menu closed *after* the runtime it talks to was already gone, and its close
	 * path did a second GXM finish, a second `SetPspemuFrameBuffer` and a second
	 * vblank wait - while pspemu was servicing its own exit requests
	 * (`request: cmd=1`, `cmd=6` sit between them in
	 * `RA_SETUP_20261029_231356_077.log`). That is the exit after which the
	 * console froze: two hand-backs and a menu close racing pspemu's teardown.
	 *
	 * Now the menu closes while the runtime is still alive, and the close path
	 * skips the display work when `g_exiting` is set, because this function does
	 * it once, below. */
	g_exiting = 1;
	if (menu_open) {
		ra_logq_direct("ra_exit: closing the menu before the teardown\n");
		ra_logq_flush();
		ExitAdrenalineMenu();
	}
	/* leave_game releases the world pause and the CPU boost before returning, so the
	 * process cannot be torn down with a suspended emulated PSP (docs/35). */
	ra_runtime_leave_game();
	/* Belt and braces (00.10.90): leave_game already joins the badge worker, but this
	 * is the last function that runs before pspemu restarts itself inside this
	 * process, so make sure no worker of ours - and none inside SceHttp or an sceIo
	 * write - is left behind. Idempotent. */
	ra_badge_shutdown();

	/* Hand the display back before pspemu restarts itself (00.10.91, completed in
	 * 00.10.96). The PSP and POPS targets are deliberately separate (see
	 * exit_display_handback_*): sharing one target is what coupled the two modes. */
	if (exit_pops && pops_data != NULL)
		exit_display_handback_pops();
	else
		exit_display_handback_psp();

	ExitAdrenalineMenu();
	/* The exit marker is written directly, not queued: the next line is a call that may
	 * not return, and this is what proves the exit path ran in the log. */
	ra_logq_direct("ra_exit: ScePspemuErrorExit(0) after leave_game\n");
	ra_logq_flush();
	ScePspemuErrorExit(0);
	return 0;
}

static void ra_menu_log(const char *fmt, ...);

/* ---- frame-thread logging ------------------------------------------------
 *
 * Kept as a thin wrapper: every RA module now queues its lines through
 * `ra_logqueue.h`, which owns the ring, the worker and the fail-open fallback. Before
 * that, this file had its own ring (docs/30) while the runtime, notification, badge,
 * shot and net modules still wrote inline - and at an unlock burst those inline
 * appends produced a single `do_frame` of 1.8 s (docs/33 §2a). One writer, one
 * measurement.
 */
/* 00.10.152: the four achievement-page toggles (docs/59 §66).
 *
 * The plumbing already existed: a row asks for a colour (`RA_COLOR_TOGGLE`) and the
 * renderer resolves on/off from its value. What was missing was a distinct hue per toggle
 * and a grey that is provably the same colour dimmed. "Off" is computed here - the same RGB
 * scaled down - so on and off cannot drift apart the way two hand-written constants do.
 * Only these four row names are affected; the achievement list keeps the row palette. */
uint32_t ra_menu_toggle_color(const char* name, uint8_t value)
{
	uint32_t on_color;
	uint32_t r, g, b;

	if (name == NULL)
		return 0;
	if (strcmp(name, "Filter") == 0)
		on_color = RA_COLOR_TOGGLE_FILTER;
	else if (strcmp(name, "Locked") == 0)
		on_color = RA_COLOR_TOGGLE_LOCKED;
	else if (strcmp(name, "Unlocked") == 0)
		on_color = RA_COLOR_TOGGLE_UNLOCKED;
	else if (strcmp(name, "Pinned") == 0)
		on_color = RA_COLOR_TOGGLE_PINNED;
	else
		return 0;

	if (value != 0)
		return on_color;

    /* Dim the same hue: 38% of each channel plus a floor, so it reads as grey on the
     * near-black menu while the tint stays recognisable. */
	r = on_color & 0xFFu;
	g = (on_color >> 8) & 0xFFu;
	b = (on_color >> 16) & 0xFFu;
	r = r * 38u / 100u + 30u;
	g = g * 38u / 100u + 30u;
	b = b * 38u / 100u + 30u;
	if (r > 255u) r = 255u;
	if (g > 255u) g = 255u;
	if (b > 255u) b = 255u;
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void ra_menu_log(const char *fmt, ...) {
	char buffer[RA_LOGQ_LINE];
	va_list ap;
	int length;

	va_start(ap, fmt);
	length = vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	ra_logq_line(buffer, length);
}

/* Same line, but on the card before this frame continues (00.10.128).
 *
 * The ring's worker drains every 100 ms, so the lines written just before a core are
 * the ones a core takes with it. `RA_SETUP_20261029_211033_013.log` is the proof: the
 * operator watched the menu open and the process died, and the log has no `menu: open`
 * within 300 lines of its end - the open sequence never left the ring. Every `menu:`,
 * `settings:` and `pad: PS` line therefore goes through this one: each of them brackets
 * a call into pspemu, and a bracket that can be lost is not a bracket. */
static void ra_menu_log_sync(const char *fmt, ...) {
	char buffer[RA_LOGQ_LINE];
	va_list ap;
	int length;

	va_start(ap, fmt);
	length = vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	ra_logq_line(buffer, length);
	/* 00.10.135: not while the app is exiting.
	 *
	 * The exit path writes five `menu: close:` lines back to back, and each sync
	 * drain is an open+write+close on `ux0` *per destination*: the frame that
	 * carries them reads `slow loop 149138 us ... other=131570` in
	 * `RA_SETUP_20261030_071946_148.log` (line 1106) - the "laggy in some parts"
	 * the operator sees on the exit screen. The exit function already writes its
	 * own markers with `ra_logq_direct()` and calls `ra_logq_flush()` immediately
	 * before `ScePspemuErrorExit(0)`, so nothing is lost by queueing these: they
	 * are flushed by that call, and if the process died in between the direct
	 * markers are what name the step. */
	if (!g_exiting)
		ra_logq_flush();
}

static int EnterAdrenalineMenu() {
	initStates();

	/* rcheevos asks that emulation not be frozen immediately after an unlock, so
	 * the shell overlay prompts do not fight a pending award. `can_pause` reports
	 * how many frames it still wants; wait at most 10 (~166 ms) so opening the
	 * menu never feels delayed (docs/58 §2.4). */
	{
		uint32_t remaining = 0;
		int waited = 0;

		while (!ra_runtime_can_pause(&remaining) && waited < 10) {
			sceDisplayWaitVblankStart();
			++waited;
		}
	}

	/* 00.60.01: this used to memset() with &config, i.e. a pointer value, so the
	 * "old" copy was never the actual config. The close path compares it to decide
	 * which settings to re-apply. */
	old_config = config;

	changed = 0;
	menu_open = 1;
	g_official_settings_dialog = 0;
	g_official_settings_seen = 0;
	ra_notify_suppress(0);
	/* Crash window marker (00.10.111): the operator had a pspemu crash while opening this
	 * menu and the log's last line was the settings-close of the *previous* cycle, so the
	 * open path itself was invisible. One line at the top and one at the bottom bracket it. */
	ra_menu_log_sync("menu: open\n");

	/* 00.10.125: the two crashes the operator can still trigger - opening this menu and
	 * picking Open Official Settings - have exactly one thing in common: both call into
	 * pspemu. pspemu's functions are reached through `text_addr + offset` pointers captured
	 * once at module_start (main.c GetFunctions), so a second session is the one case where
	 * those pointers can be pointing at an older pspemu instance. Bracket every call, one
	 * line each, so the next core names the call instead of the browser afterwards. */
	ra_menu_log_sync("menu: open: converting pspemu address (session=%d text=0x%08X)\n",
	            ra_runtime_session_ordinal(), (unsigned)text_addr);

	if (open_official_settings && open_official_settings_session != ra_runtime_session_ordinal()) {
		/* 00.10.123: the flag came from a previous session - the player opened pspemu's
		 * settings, then left to the XMB instead of coming back to this menu. Acting on it
		 * here forces a disc check and a fingerprint read (both of which reach into pspemu
		 * with ScePspemuConvertAddress) on a session whose menu was never in that mode. The
		 * operator's crash log ends exactly here, one line after this branch logged a
		 * settings-close that this session never opened
		 * (RA_SETUP_20261029_195708_102.log line 678, then the eboot.bin core). */
		ra_menu_log_sync("settings: ignoring a settings-close left over from session %d (now %d)\n",
		            open_official_settings_session, ra_runtime_session_ordinal());
	} else if (open_official_settings) {
		/* Back from pspemu's settings, which is where "Switch Discs" happens. The runtime
		 * re-reads the live PS1 disc serial and changes media when it does not match the
		 * disc the session was opened with (00.10.104). */
		SceAdrenaline *watch = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);

		ra_menu_log_sync("settings: official settings closed; title='%.24s' id='%.12s' "
		            "pops=%d file='%.60s'\n",
		            watch != NULL ? watch->title : "(null)",
		            watch != NULL ? watch->titleid : "(null)",
		            watch != NULL ? (int)watch->pops_mode : -1,
		            watch != NULL ? watch->filename : "(null)");
		ra_runtime_official_settings_closed();
	}
	open_official_settings = 0;
	open_official_settings_session = -1;

	/* 00.10.125: the second call into pspemu on this path, and the one the operator's
	 * "menu open" crash would be in. One line either side names it. */
	ra_menu_log_sync("menu: open: converting pspemu address\n");
	SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
	ra_menu_log_sync("menu: open: pspemu address ok (%p)\n", (void *)adrenaline);
	/* 00.10.127: the pointer was reported and then dereferenced on the next line.
	 * A failed conversion is the one state where opening the menu must not fault -
	 * the menu is the only way back out. Degrade instead: no pops pause, and say
	 * so. */
	if (adrenaline == NULL)
		ra_menu_log_sync("menu: open: no pspemu address; opening without a pops "
		            "pause\n");
	ra_menu_tick(adrenaline);
	ra_menu_open_root();
	if (adrenaline == NULL) {
		/* keep going: the ARK check below needs nothing from pspemu */
	} else if (adrenaline->pops_mode || ra_runtime_title_is_pops()) {
		ra_menu_log_sync("menu: open: pausing pops (pops=%d)\n", (int)adrenaline->pops_mode);
		ScePspemuPausePops(1);
		ra_menu_log_sync("menu: open: pops paused\n");
	} else if (adrenaline->app_type == SCE_APP_TYPE_GAME || adrenaline->app_type == SCE_APP_TYPE_APP) {
		lockPower();
		/* The world pause belongs to the runtime, not to this file (docs/35).
		 *
		 * Both used to send ADRENALINE_PSP_CMD_PAUSE_WORLD on their own, and
		 * pentazemin's pauseWorld() cannot be entered twice: the second call rebuilds
		 * its thread list, drops the threads the first call suspended, and resumeWorld()
		 * can never bring them back - a frozen game behind a working menu. Claiming the
		 * pause here means only one PAUSE is ever in flight. */
		ra_menu_log_sync("menu: open: claiming the world pause\n");
		ra_runtime_pause_for_menu();
		ra_menu_log_sync("menu: open: world pause claimed\n");
	}

	// Check if ARK is installed
	SceIoStat stat;
	char ark_path[64];
	snprintf(ark_path, sizeof(ark_path), "%s" FLASH0_ARK_PATH, getPspemuMemoryStickLocation());
	if (sceIoGetstat(ark_path, &stat) < 0 && sceIoGetstat("ux0:pspemu" FLASH0_ARK_PATH, &stat) < 0){
		settings_entries[0].n_options = 1;
		config.cfw_type = 0;
	} else {
		settings_entries[0].n_options = sizeof(cfwtype_options)/sizeof(char*);
	}

	return 0;
}

int ExitAdrenalineMenu(void) {
	if (changed) {
		SaveAdrSetting();
	}

	SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);
	/* 00.10.127: the log line dereferenced the pointer it was reporting on, so a
	 * failed conversion crashed inside its own diagnostic. */
	ra_menu_log_sync("menu: close: pspemu address ok (%p pops=%d)\n", (void *)adrenaline,
	            adrenaline != NULL ? (int)adrenaline->pops_mode : -1);
	if (adrenaline == NULL) {
		ra_menu_log_sync("menu: close: no pspemu address; leaving pops alone\n");
	} else if (adrenaline->pops_mode || ra_runtime_title_is_pops()) {
		ra_menu_log_sync("menu: close: resuming pops\n");
		ScePspemuPausePops(0);
		ra_menu_log_sync("menu: close: pops resumed\n");
	} else if (adrenaline->app_type == SCE_APP_TYPE_GAME || adrenaline->app_type == SCE_APP_TYPE_APP)  {
		unlockPower();
		/* Drop this file's claim; the runtime only resumes the world if nothing else
		 * (e.g. a session load) is still holding it. See EnterAdrenalineMenu(). */
		ra_menu_log_sync("menu: close: releasing the world pause\n");
		ra_runtime_resume_for_menu();
		ra_menu_log_sync("menu: close: world pause released\n");
	}

	g_tooltip_focus_generation++;
	ra_menu_log_sync("menu: close\n");
	menu_open = 0;
	ra_runtime_shell_transition_clear();

	if (old_config.ms_location != config.ms_location) {
		SendAdrenalineRequest(ADRENALINE_PSP_CMD_REINSERT_MS);
	}

	if (old_config.ef_location != config.ef_location) {
		SendAdrenalineRequest(ADRENALINE_PSP_CMD_REINSERT_EF);
	}

	if (g_exiting) {
		/* 00.10.131: `ExitPspEmuApplication()` already closed this menu while the
		 * runtime was alive and owns the single display hand-back; repeating the
		 * GXM finish / SetPspemuFrameBuffer / vblank here is what raced pspemu's
		 * own teardown and froze the console. */
		ra_menu_log_sync("menu: close: display work is the exit path's (exiting)\n");
	} else {
		ra_menu_log_sync("menu: close: handing the framebuffer back\n");
		SetPspemuFrameBuffer((void *)SCE_PSPEMU_FRAMEBUFFER);
		sceDisplayWaitVblankStart();
	}

	ra_menu_log_sync("menu: close: signalling the menu semaphore\n");
	sceKernelSignalSema(settings_semaid, 1);
	ra_menu_log_sync("menu: close: done\n");

	finishStates();

	return 0;
}

static int ResetAdrenalineSettings(void) {
	memset(&config, 0, sizeof(AdrenalineConfig));
	config.magic[0] = ADRENALINE_CFG_MAGIC_1;
	config.magic[1] = ADRENALINE_CFG_MAGIC_2;
	config.psp_screen_scale_x = 2.0f;
	config.psp_screen_scale_y = 2.0f;
	config.ps1_screen_scale_x = 1.0f;
	config.ps1_screen_scale_y = 1.0f;
	WriteFile("ux0:data/" ADRENALINE_TITLEID "/adrenaline.bin", &config, sizeof(AdrenalineConfig));

	SceIoStat stat;
	if (sceIoGetstat("ux0:app/" ADRENALINE_TITLEID "/adrenaline.bin", &stat) >= 0) {
		WriteFile("ux0:app/" ADRENALINE_TITLEID "/adrenaline.bin", &config, sizeof(AdrenalineConfig));
	}

	return 0;
}

#define RA_TEXT_WINDOW_MAX 512

/* Left margin reserved for the marquee's sub-character shift: the window draws
 * its first glyph up to one character to the left of its origin, and this keeps
 * that spill inside the margin instead of over the row label or the panel edge. */
#define RA_SCROLL_INSET 14.0f

/* Scroll a few pixels past the last glyph.
 *
 * The window picks its slice by comparing float prefix widths, so stopping at
 * exactly `text_width - window_width` leaves the final character a fraction over
 * the edge whenever the travel is rounded up — which is why a long tooltip ended
 * at `... (25 Points` with no closing parenthesis. The pad guarantees the last
 * glyph is inside the window with room to spare. */
#define RA_SCROLL_RIGHT_PAD 16.0f

/* Horizontally windowed draw for marquee text.
 *
 * The region clip is not applied to PGF text in this renderer (pgf_draw_text
 * draws 20 px below the requested y, so the clip rect and the glyphs never
 * line up), which let a scrolling value run over the label to its left. This
 * computes the visible slice instead: it starts at the character covering
 * `offset` and ends at the last character that still fits in `max_width`, then
 * draws it at a fixed x. Nothing is ever drawn left of x. */
typedef struct {
	char text[RA_TEXT_WINDOW_MAX];
	float prefix[RA_TEXT_WINDOW_MAX];
	size_t start[RA_TEXT_WINDOW_MAX];
	size_t count;
	int valid;
} RaTextWindow;


static void draw_text_window(RaTextWindow *window, const char *text, float x,
							 float y, uint32_t color, float offset,
							 float max_width) {
	size_t begin = 0;
	size_t end;
	size_t length;
	size_t start_byte;
	size_t end_byte;
	float base;
	float origin_x = x;
	float avail;
	char slice[RA_TEXT_WINDOW_MAX];

	if (window == NULL || text == NULL || max_width <= 1.0f)
		return;

	if (!window->valid || strcmp(window->text, text) != 0) {
		char prefix[RA_TEXT_WINDOW_MAX];
		size_t bytes = 0;

		snprintf(window->text, sizeof(window->text), "%s", text);
		window->count = 0;
		while (text[bytes] != '\0' &&
		       window->count < RA_TEXT_WINDOW_MAX - 1) {
			size_t char_len = 1;

			while ((text[bytes + char_len] & 0xC0) == 0x80)
				++char_len;
			window->start[window->count] = bytes;
			bytes += char_len;
			if (bytes >= sizeof(prefix))
				break;
			memcpy(prefix, text, bytes);
			prefix[bytes] = '\0';
			/* 00.50.04: measured through ra_text_width() so a symbol contributes its
			 * atlas advance. With PGF's measurement, a slice containing a symbol had
			 * the wrong width and the symbol itself was never drawn - the marquee
			 * was the one path still rendering through PGF alone. */
			window->prefix[window->count] =
				(float)ra_text_width(font, FONT_SIZE, prefix);
			++window->count;
		}
		window->valid = 1;
	}
	if (window->count == 0)
		return;

	while (begin + 1 < window->count && window->prefix[begin] <= offset)
		++begin;
	base = begin > 0 ? window->prefix[begin - 1] : 0.0f;

	/* 00.10.159: scroll *to* the left edge instead of past it.
	 *
	 * This vita2d fork has no scissor, so anything drawn left of `x` is visible -
	 * which is why the old `x - shift` slid the marquee under the label (and why
	 * 00.10.158 snapped it to whole characters, turning the motion into a 250 ms
	 * stutter). Instead of moving the slice left, move the *anchor* right by the
	 * distance the first partially-scrolled glyph still had to travel, and start
	 * the slice at the next glyph. The leading glyph then marches left and
	 * vanishes exactly as it reaches `x`, the glyph behind it takes its place at
	 * one glyph width and continues the same motion, and nothing is ever drawn
	 * left of `x`. Continuous travel, no overlap, no scissor. */
	if (base < offset) {
		if (begin + 1 >= window->count) {
			/* Last glyph: nothing follows it, so park it at the anchor. */
			base = offset;
		} else {
			++begin;
			base = window->prefix[begin - 1];
			x += base - offset;
		}
	}

	/* 00.50.09: the slice must end at the *window* edge, not `max_width` from the
	 * shifted anchor.
	 *
	 * The shift above moves the anchor right by `base - offset` (the part of the
	 * first glyph already scrolled). Choosing `end` against `max_width` alone lets
	 * the drawn run reach `x + max_width`, i.e. up to one glyph past the window -
	 * what the operator saw as long descriptions running outside the panel when the
	 * marquee reached its tail (screenshot 2026-11-01). Account for the shift so
	 * the right edge can never exceed `origin_x + max_width`. */
	avail = max_width - (x - origin_x);
	if (avail < 0.0f)
		avail = 0.0f;

	end = begin;
	while (end + 1 < window->count &&
	       window->prefix[end + 1] - base <= avail)
		++end;

	start_byte = window->start[begin];
	end_byte = (end + 1 < window->count) ? window->start[end + 1]
	                                     : strlen(text);
	length = end_byte - start_byte;
	if (length >= sizeof(slice))
		length = sizeof(slice) - 1;
	memcpy(slice, text + start_byte, length);
	slice[length] = '\0';

	/* 00.50.12: trim by measuring the slice that will actually be drawn.
	 *
	 * The prefix table is a prediction; the renderer is the truth (mixed PGF and
	 * stack runs, rounding, symbols). m9i7 still showed a long description crossing
	 * the panel edge, so the boundary is now enforced on the final string: drop
	 * whole UTF-8 characters until its measured width fits the available width.
	 * Nothing is scaled - only characters past the edge are not drawn. */
	while (length > 1 &&
	       (float)ra_text_width(font, FONT_SIZE, slice) > avail) {
		--length;
		while (length > 0 && (slice[length] & 0xC0) == 0x80)
			--length;
		slice[length] = '\0';
	}

	/* 00.50.04: the slice is drawn with the mixed renderer.
	 *
	 * `pgf_draw_text` was the last PGF-only path left, so a marquee containing a
	 * symbol drew PGF's placeholder - and because the caller handed it the folded
	 * string, the symbol was not even there. The slice boundaries come from the
	 * same mixed measurement now, so atlas glyphs land where the widths say they
	 * land. The +20 keeps the menu's row-top convention (see the pgf_draw_text
	 * macro in menu.h) and gives ra_text_draw the baseline it expects. */
	ra_text_draw(font, x, y + 20.0f, color, FONT_SIZE, slice);
}

/* Wall-clock animation clock for the menu marquees.
 *
 * Both scrollers used `sceKernelGetProcessTimeWide()`, which counts CPU time. The
 * draw thread is regularly blocked (framebuffer copy, v-sync, the RA worker), so
 * that clock advanced in jerks and slower than reality: the marquees ran at a
 * fraction of their configured px/second and juddered. Wall time keeps the speed
 * honest, and clamping a single step means a stall pauses the scroll instead of
 * teleporting it. */
#define RA_SCROLL_MAX_STEP_US 50000ULL

typedef struct {
	unsigned long long last_us;
	unsigned long long accum_us;
} RaScrollClock;

static void ra_scroll_reset(RaScrollClock* clock) {
	clock->last_us = 0;
	clock->accum_us = 0;
}

static unsigned long long ra_scroll_advance(RaScrollClock* clock) {
	unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();
	unsigned long long delta = 0;

	if (clock->last_us != 0 && now > clock->last_us)
		delta = now - clock->last_us;
	if (delta > RA_SCROLL_MAX_STEP_US)
		delta = RA_SCROLL_MAX_STEP_US;
	clock->last_us = now;
	clock->accum_us += delta;
	return clock->accum_us;
}

#define RA_TOOLTIP_SCROLL_DELAY_US 2500000ULL
#define RA_TOOLTIP_SCROLL_END_HOLD_US 2500000ULL
#define RA_TOOLTIP_SCROLL_SPEED_PX_PER_SEC 40U

static void drawMenuTooltip(const char *tooltip, int tab, int selection,
							uint32_t text_color) {
	static const char *focus_text;
	static char focus_copy[RA_TEXT_WINDOW_MAX];
	static int focus_tab = -1;
	static int focus_selection = -1;
	static unsigned int focus_generation;
	static RaScrollClock tooltip_clock;
	static float focus_width;
	static RaTextWindow tooltip_window;
	float max_width = WINDOW_WIDTH - 16.0f;
	float y = FONT_Y_LINE(17) + 10.0f;
	unsigned long long elapsed;

	if (tooltip == NULL)
		return;

	/* The marquee starts only after the entry has held focus, and resets when
	 * focus changes or the menu was closed/reopened. */
	if (focus_text != tooltip || focus_tab != tab ||
	    focus_selection != selection ||
	    focus_generation != g_tooltip_focus_generation ||
	    strcmp(focus_copy, tooltip) != 0) {
		focus_text = tooltip;
		snprintf(focus_copy, sizeof(focus_copy), "%s", tooltip);
		focus_tab = tab;
		focus_selection = selection;
		focus_generation = g_tooltip_focus_generation;
		ra_scroll_reset(&tooltip_clock);
		/* Measure the string that is actually drawn, not the copied buffer: a
		 * truncated copy under-reports the width, the marquee then stops before
		 * the real end, and the final glyph - the closing parenthesis of
		 * "(N Points)" - never appears (operator report 2026-10-26: the long
		 * descriptions under Unlocked kept losing it). */
		focus_width = vita2d_pgf_text_width(font, FONT_SIZE, tooltip);
	}
	if (text_color == 0)
		text_color = ORANGE; /* default: unchanged for rows that set no colour */
	elapsed = ra_scroll_advance(&tooltip_clock);

	if (focus_width <= max_width) {
		pgf_draw_text(WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, focus_width),
		              y, text_color, FONT_SIZE, tooltip);
		return;
	}

	/* Scroll long tooltips through the window instead of letting them run off
	 * the screen. Scroll only until the end is visible, hold there long enough
	 * to read it, then reset to the beginning and wait again. */
	{
		unsigned int travel;
		unsigned int offset = 0;
		/* Leave the marquee inset on the left so its sub-character shift stays
		 * inside the panel instead of poking out of the window edge. */
		unsigned int left = (unsigned int)(WINDOW_X + 8.0f + RA_SCROLL_INSET);
		unsigned int right = (unsigned int)(WINDOW_X + WINDOW_WIDTH - 8.0f);
		float window_width = (float)(right - left);
		unsigned long long scroll_us;
		unsigned long long cycle_us;

		if (focus_width - window_width > 0.0f)
			travel = (unsigned int)(focus_width - window_width +
			                        RA_SCROLL_RIGHT_PAD + 0.5f);
		else
			travel = 0;
		scroll_us = (unsigned long long)travel * 1000000ULL /
			RA_TOOLTIP_SCROLL_SPEED_PX_PER_SEC;
		cycle_us = scroll_us +
			RA_TOOLTIP_SCROLL_END_HOLD_US + RA_TOOLTIP_SCROLL_DELAY_US;

		if (elapsed >= RA_TOOLTIP_SCROLL_DELAY_US) {
			unsigned long long phase =
				(elapsed - RA_TOOLTIP_SCROLL_DELAY_US) % cycle_us;

			if (phase < scroll_us) {
				offset = (unsigned int)(
					phase * RA_TOOLTIP_SCROLL_SPEED_PX_PER_SEC / 1000000ULL);
			} else if (phase < scroll_us + RA_TOOLTIP_SCROLL_END_HOLD_US) {
				offset = travel;
			} else {
				/* Reset to the start and hold there for the normal delay
				 * before starting another pass. */
				offset = 0;
			}
		}

		draw_text_window(&tooltip_window, tooltip, (float)left, y, text_color,
		                 (float)offset, window_width);
	}
}

#define RA_STATUS_SCROLL_DELAY_US 1500000ULL
#define RA_STATUS_SCROLL_END_HOLD_US 2000000ULL
/* PGF places glyphs on integer pixels (`vita2d_pgf_draw_text()` takes `int x`), so
 * the marquee advances in 1 px steps no matter how smooth the timing is. The speed
 * decides whether those steps are evenly spaced: at 60 Hz, 40 px/s is 0.667 px per
 * frame, i.e. one step every three frames (0,1,1), while 36 px/s is 0.6 and gives
 * (0,1,0,1,1) — a five-frame cycle that reads as a limp. Keep it a whole fraction. */
#define RA_STATUS_SCROLL_SPEED_PX_PER_SEC 40U
#define RA_STATUS_VALUE_MAX_WIDTH 560.0f

/* `focused` is the row under the cursor. Only that row animates: a marquee
 * running on an unfocused row is movement the player did not ask for, and it
 * was also the reason the status line kept scrolling while the cursor was
 * somewhere else. Unfocused rows show the first slice that fits and hold still. */
static void drawScrollingEntry(const char *label, const char *value,
							   const char *fallback, uint32_t color, float y,
							   int focused) {
	static char cached_label[160];
	static char cached_value[RA_TEXT_WINDOW_MAX];
	static char cached_fallback[RA_TEXT_WINDOW_MAX];
	static float cached_label_width;
	static float cached_value_width;
	static float cached_fallback_width;
	static RaScrollClock status_clock;
	static RaTextWindow status_window;
	const char *marquee;
	float marquee_width;
	float label_width;
	float value_width;
	float label_x;
	float value_x;
	float max_width;
	float window_width;
	float group_width;
	float group_x;
	unsigned long long elapsed;

	if (label == NULL || value == NULL)
		return;

	/* Widths follow the strings, not the frame: this row is drawn every frame and
	 * two PGF measurements per frame bought nothing. */
	if (strcmp(cached_label, label) != 0) {
		snprintf(cached_label, sizeof(cached_label), "%s", label);
		cached_label_width =
			vita2d_pgf_text_width(font, FONT_SIZE, cached_label);
	}
	if (strcmp(cached_value, value) != 0) {
		snprintf(cached_value, sizeof(cached_value), "%s", value);
		/* 00.10.160: measured through ra_text_width() so a symbol contributes the
		 * atlas advance rather than PGF's placeholder width. */
		cached_value_width =
			(float)ra_text_width(font, FONT_SIZE, cached_value);
		if (fallback != NULL && strcmp(cached_fallback, fallback) != 0) {
			snprintf(cached_fallback, sizeof(cached_fallback), "%s", fallback);
			cached_fallback_width =
				(float)vita2d_pgf_text_width(font, FONT_SIZE, cached_fallback);
		}
		ra_scroll_reset(&status_clock);
	}

	label_width = cached_label_width;
	value_width = cached_value_width;
	max_width = RA_STATUS_VALUE_MAX_WIDTH;
	if (max_width > WINDOW_WIDTH - label_width - 70.0f)
		max_width = WINDOW_WIDTH - label_width - 70.0f;
	if (max_width < 80.0f)
		max_width = 80.0f;
	/* 00.10.159: the window is the full measured width - no inset reserve.
	 *
	 * draw_text_window() now moves its anchor right instead of drawing left of
	 * `x` (00.10.159), so the text cannot reach the label whatever the gap is.
	 * The reserve would only have widened the label/value gap again. */
	window_width = max_width;
	if (window_width < 60.0f)
		window_width = 60.0f;

	/* Center the label+value as one group, like the old static status row.
	 * Long values get a fixed, centered reading area; short values center
	 * exactly on their natural width. The inset is part of the group so the
	 * value does not jump sideways when it starts or stops scrolling.
	 *
	 * 00.10.154: the gap is no longer a fixed 10 + inset. A value that fits the
	 * window cannot move, so it sits 4 units from the label - that void was the
	 * operator's "1-character gap". Only a value that actually marquees keeps the
	 * inset, because that inset exists precisely to stop the scrolled text from
	 * sliding under the label. */
	{
		float gap = (value_width <= window_width) ? 4.0f
		                                         : 4.0f;

		group_width = label_width + gap +
			(value_width <= window_width ? value_width : window_width);
		group_x = WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, group_width);
		label_x = group_x;
		value_x = group_x + label_width + gap;
	}

	pgf_draw_text(label_x, y, color, FONT_SIZE, label);
	if (value_width <= window_width) {
		/* 00.60.01: ASCII-only draw. ra_text drops non-ASCII codepoints, so the
		 * row is the text the font can actually draw.
		 *
		 * The row y is a row top: the pgf_draw_text macro turns it into a baseline
		 * with +20, which is what ra_text_draw() wants. */
		ra_text_draw(font, value_x, y + 20.0f, color, FONT_SIZE, value);
		return;
	}

	/* Too wide to fit, so it has to scroll.
	 *
	 * 00.50.04: it scrolls the *value*, symbols included. The folded fallback was
	 * there because the scroller sliced bytes and drew them through PGF; both are
	 * fixed now (slices break on UTF-8 boundaries, and draw_text_window renders
	 * through ra_text_draw), so the fold is only used when the atlas could not be
	 * created at all - where it is the difference between ASCII and a placeholder. */
	if (fallback != NULL && fallback[0] != '\0' && !ra_text_ready()) {
		marquee = fallback;
		marquee_width = cached_fallback_width;
	} else {
		marquee = value;
		marquee_width = value_width;
	}

	if (!focused) {
		/* Keep the clock parked at zero so moving the cursor onto this row
		 * starts the marquee from its beginning, delay included. */
		ra_scroll_reset(&status_clock);
		draw_text_window(&status_window, marquee, value_x, y, color, 0.0f,
		                 window_width);
		return;
	}

	elapsed = ra_scroll_advance(&status_clock);

	{
		unsigned int travel = (unsigned int)(marquee_width - window_width +
		                                    RA_SCROLL_RIGHT_PAD + 0.5f);
		unsigned long long scroll_us =
			(unsigned long long)travel * 1000000ULL /
			RA_STATUS_SCROLL_SPEED_PX_PER_SEC;
		unsigned long long cycle_us = scroll_us +
			RA_STATUS_SCROLL_END_HOLD_US + RA_STATUS_SCROLL_DELAY_US;
		unsigned int offset = 0;

		if (elapsed >= RA_STATUS_SCROLL_DELAY_US) {
			unsigned long long phase =
				(elapsed - RA_STATUS_SCROLL_DELAY_US) % cycle_us;
			if (phase < scroll_us)
				offset = (unsigned int)(phase *
					RA_STATUS_SCROLL_SPEED_PX_PER_SEC / 1000000ULL);
			else if (phase < scroll_us + RA_STATUS_SCROLL_END_HOLD_US)
				offset = travel;
		}

		draw_text_window(&status_window, marquee, value_x, y, color,
		                 (float)offset, window_width);
	}
}

/* Draw a row's text, with optional coloured `[X] ` marker and state dot.
 *
 * With both fields at 0 - which is every row today - this is byte-for-byte the
 * centred draw it replaced, so the plumbing is inert until the colour map sets a
 * value (docs/55-ra-menu-colors.md §7 items 3 and 5). */
static void drawRowText(const MenuEntry *entry, float y, uint32_t color) {
	const char *name = (entry->name != NULL) ? entry->name : "";
	const char *rest = name;
	size_t prefix_length = 0;
	float offset = 0.0f;
	float total_width;

	if (entry->dot_color != 0) {
		vita2d_draw_fill_circle(WINDOW_X + 12.0f, y + 10.0f, 3.0f,
		                        entry->dot_color);
		offset = 12.0f;
	}

	if (entry->prefix_color != 0 && name[0] == '[') {
		const char *close = strchr(name, ']');

		if (close != NULL) {
			prefix_length = (size_t)(close - name) + 1;
			if (name[prefix_length] == ' ')
				++prefix_length;
		}
	}

	total_width = offset + vita2d_pgf_text_width(font, FONT_SIZE, name);

	{
		float x = ALIGN_CENTER(SCREEN_WIDTH, total_width);

		if (prefix_length > 0) {
			char marker[16];
			size_t n = prefix_length < sizeof(marker) - 1
			               ? prefix_length
			               : sizeof(marker) - 1;

			memcpy(marker, name, n);
			marker[n] = '\0';
			pgf_draw_text(x, y, entry->prefix_color, FONT_SIZE, marker);
			x += vita2d_pgf_text_width(font, FONT_SIZE, marker);
			rest = name + prefix_length;
		}
		pgf_draw_text(x, y, color, FONT_SIZE, rest);
	}
}

static void drawMenu(void) {
	// Draw window
	vita2d_draw_rectangle(WINDOW_X, WINDOW_Y, WINDOW_WIDTH, WINDOW_HEIGHT, WINDOW_COLOR);

	// Draw title
	vita2d_draw_rectangle(WINDOW_X, WINDOW_Y, WINDOW_WIDTH, 38.0f, COLOR_ALPHA(GRAY, 0x7F));
	char *title = "Adrenaline Menu";
	pgf_draw_textf(WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, vita2d_pgf_text_width(font, FONT_SIZE, title)), FONT_Y_LINE(0), WHITE, FONT_SIZE, title);

	if (tab_sel == 0) {
		// Running app info
		SceAdrenaline *adrenaline = (SceAdrenaline *)ScePspemuConvertAddress(ADRENALINE_ADDRESS, KERMIT_INPUT_MODE, ADRENALINE_SIZE);

		char info[143] = {0};
		if (strcmp(adrenaline->title, "XMB\xE2\x84\xA2") == 0) {
			snprintf(info, sizeof(info), "XMB");
		} else {
			int res = 0;

			if (adrenaline->title[0] != 0) {
				res = snprintf(info, sizeof(info), "%s - ", adrenaline->title);
			}

			if (adrenaline->titleid[0] != 0 && res < (int)sizeof(info)) {
				snprintf(info+res, sizeof(info) - res, "%s", adrenaline->titleid);
			}
		}

		vita2d_draw_rectangle(WINDOW_X, FONT_Y_LINE(1) + 9.0f, WINDOW_WIDTH, 28.0f, COLOR_ALPHA(0xFFFF1F7F, 0x7F));
		pgf_draw_text(WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, vita2d_pgf_text_width(font, FONT_SIZE, info)), FONT_Y_LINE(1) + 10.0f, WHITE, FONT_SIZE, info);
	}

	// Draw tabs
	int i;
	for (i = 0; i < N_TABS; i++) {
		vita2d_draw_rectangle(WINDOW_X + (i * TAB_SIZE), FONT_Y_LINE(19) - 5.0f, TAB_SIZE, 38.0f, tab_sel == i ? 0xFF7F3F00 : COLOR_ALPHA(GRAY, 0x8F));

		if (i != 0) {
			vita2d_draw_rectangle(WINDOW_X + (i * TAB_SIZE) - 2.0f, FONT_Y_LINE(19) - 5.0f, 4.0f, 38.0f, COLOR_ALPHA(BLACK, 0x8F));
		}

		float x = WINDOW_X + (i * TAB_SIZE) + ALIGN_CENTER(TAB_SIZE, vita2d_pgf_text_width(font, FONT_SIZE, tab_entries[i].name));
		pgf_draw_text(x, FONT_Y_LINE(19), WHITE, FONT_SIZE, tab_entries[i].name);
	}

	// Draw entries
	MenuEntry *menu_entries = tab_entries[tab_sel].entries;
	if (menu_entries) {
		int first = 0;
		int last = tab_entries[tab_sel].n_entries;
		if (tab_sel == RA_MENU_TAB_INDEX) {
			first = tab_entries[tab_sel].scroll;
			if (first < 0)
				first = 0;
			if (first > last)
				first = last;
			last = first + RA_MENU_VISIBLE_ROWS;
			if (last > tab_entries[tab_sel].n_entries)
				last = tab_entries[tab_sel].n_entries;
		}
		for (i = first; i < last; i++) {
			float y = (tab_sel == 0) ? FONT_Y_LINE(3 + i) : FONT_Y_LINE(2 + i - first);

			uint32_t color = WHITE;
			if (menu_entries[i].type == MENU_ENTRY_TYPE_TEXT || menu_entries[i].type == MENU_ENTRY_TYPE_CALLBACK) {
				color = menu_entries[i].color;
			}

			// Focus
			if (i == menu_sel && tab_entries[tab_sel].moveable) {
				vita2d_draw_rectangle(WINDOW_X, y + 3.0f, WINDOW_WIDTH, FONT_Y_SPACE, COLOR_ALPHA(0xFFFF1F7F, 0x8F));
			}

			if (menu_entries[i].scroll_text != NULL) {
				/* Only the cursor row marquees; the rest hold still. The
				 * fallback (folded ASCII) is used only when the value has to
				 * scroll - see MenuEntry::scroll_fallback. */
				drawScrollingEntry(menu_entries[i].name,
				                   menu_entries[i].scroll_text,
				                   menu_entries[i].scroll_fallback, color, y,
				                   i == menu_sel &&
				                   tab_entries[tab_sel].moveable);
			} else if (menu_entries[i].type == MENU_ENTRY_TYPE_TEXT || menu_entries[i].type == MENU_ENTRY_TYPE_CALLBACK) {
				// Item
				drawRowText(&menu_entries[i], y, color);
			} else {
				// Item
				float x = vita2d_pgf_text_width(font, FONT_SIZE, menu_entries[i].name);
				pgf_draw_text(ALIGN_RIGHT((SCREEN_WIDTH / 2.0f) - 10.0f, x), y, color, FONT_SIZE, menu_entries[i].name);

				if (menu_entries[i].type == MENU_ENTRY_TYPE_OPTION) {
					// Option
					uint8_t value = *((uint8_t*)menu_entries[i].value);
					uint32_t value_color = menu_entries[i].value_color;
					/* 00.10.152: the four achievement-page toggles get their own hues
					 * (docs/59 §66). This is the plumbing that already existed for
					 * `RA_COLOR_TOGGLE` - a row asks for a colour, the renderer resolves
					 * on/off - so only the mapping is new. Off is the same RGB dimmed by
					 * a fixed amount (never a second constant), and the achievement list
					 * is untouched because its rows do not use these hues. */
					if (value_color == 0 || value_color == RA_COLOR_TOGGLE) {
						uint32_t toggle = ra_menu_toggle_color(menu_entries[i].name, value);

						if (toggle != 0)
							value_color = toggle;
						else if (value_color == RA_COLOR_TOGGLE)
							value_color = value != 0 ? RA_COLOR_OK : RA_COLOR_HINT;
						else
							value_color = GREEN;
					}
					pgf_draw_text((SCREEN_WIDTH / 2.0f) + 10.0f, y, value_color, FONT_SIZE, menu_entries[i].options[value]);
				} else if (menu_entries[i].type == MENU_ENTRY_TYPE_SCALE) {
						// Option
					float *value = (float *)menu_entries[i].value;
					uint32_t value_color = menu_entries[i].value_color;
					if (value_color == RA_COLOR_TOGGLE || value_color == 0)
						value_color = GREEN;
					pgf_draw_textf((SCREEN_WIDTH / 2.0f) + 10.0f, y, value_color, FONT_SIZE, "%.03f", *value);
				}
			}
		}
		// Info about PSP CFW change
		if (tab_sel == 2 && menu_sel == 0) {
			char *title = "Changing Custom Firmware requires to close and reopen Adrenaline to take effect.";
			pgf_draw_text(WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, vita2d_pgf_text_width(font, FONT_SIZE, title)), FONT_Y_LINE(17)+10, ORANGE, FONT_SIZE, title);
		}
		// Info about Original filter
		if (tab_sel == 2 && menu_sel == 2 && config.graphics_filtering == 0) {
			char *title = "All graphics related options are not taking effect with the Original rendering mode.";
			pgf_draw_text(WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, vita2d_pgf_text_width(font, FONT_SIZE, title)), FONT_Y_LINE(17)+10, ORANGE, FONT_SIZE, title);
		}
		if (tab_sel == 2 && menu_sel == 2 && config.graphics_filtering == 5) {
			char *title = (g_direct_sharp_scale_active) ? "Using IFTU Sharp Bilinear" : "Using vita2d Sharp Bilinear";
			pgf_draw_text(WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH, vita2d_pgf_text_width(font, FONT_SIZE, title)), FONT_Y_LINE(17)+10, ORANGE, FONT_SIZE, title);
		}
		if (tab_entries[tab_sel].moveable && menu_entries != NULL &&
		    menu_sel >= 0 && menu_sel < tab_entries[tab_sel].n_entries &&
		    menu_entries[menu_sel].tooltip != NULL) {
			drawMenuTooltip(menu_entries[menu_sel].tooltip, tab_sel, menu_sel,
			                menu_entries[menu_sel].tooltip_color);
		}
		if (tab_sel == RA_MENU_TAB_INDEX) {
			ra_menu_draw_status();
		}
	} else {
		drawStates();
	}
}

static int capture_dialog_snapshot(void) {
	const unsigned char* source;
	unsigned char* destination;
	vita2d_texture* snapshot;
	unsigned int stride;
	int y;

	if (g_dialog_snapshot_ready)
		return 1;
	if (g_dialog_snapshot_attempted)
		return 0;
	g_dialog_snapshot_attempted = 1;

	snapshot = g_dialog_snapshots[g_dialog_snapshot_slot];
	if (snapshot == NULL) {
		snapshot = vita2d_create_empty_texture(SCREEN_WIDTH, SCREEN_HEIGHT);
		g_dialog_snapshots[g_dialog_snapshot_slot] = snapshot;
	}
	if (snapshot == NULL)
		return 0;

	vita2d_wait_rendering_done();
	source = (const unsigned char*)vita2d_get_current_fb();
	destination = (unsigned char*)vita2d_texture_get_datap(snapshot);
	stride = vita2d_texture_get_stride(snapshot);
	if (source == NULL || destination == NULL || stride == 0)
		return 0;

	vita2d_texture_set_filters(snapshot,
	                           SCE_GXM_TEXTURE_FILTER_POINT,
	                           SCE_GXM_TEXTURE_FILTER_POINT);
	for (y = 0; y < SCREEN_HEIGHT; ++y)
		memcpy(destination + (size_t)y * stride,
		       source + (size_t)y * SCREEN_LINE * 4,
		       SCREEN_WIDTH * 4);
	g_dialog_snapshot_ready = 1;
	return 1;
}

void AdrenalineMenuPresentDialog(void) {
	/* Preserve the last menu frame as a static texture, then let the IME
	 * composite over it. Redrawing the RA menu (PGF text, list entries) on
	 * every keyboard frame is what crashed before; a single textured quad
	 * is stable and avoids the three-buffer flicker. */
	int dialog_result;

	if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_RUNNING)
		return;

	if (g_dialog_present_frames == 0)
		ra_menu_log("dialog present begin\n");

	/* Match the official IME sample: CPU-clear the back buffer and let the
	 * common dialog draw itself. Do not submit a GXM scene while the IME is
	 * running; the Shift/layout refresh can otherwise fault the GPU. The menu
	 * snapshot is still used by the close pump, not during active typing. */
	vita2d_clear_back_buffer();

	dialog_result = vita2d_common_dialog_update();
	if (dialog_result < 0 &&
	    dialog_result != SCE_COMMON_DIALOG_ERROR_NOT_RUNNING)
		ra_menu_log("common dialog update failed=0x%08X\n",
		            (unsigned)dialog_result);
	if (g_dialog_present_frames == 0)
		ra_menu_log("dialog present update=%d\n", dialog_result);

	vita2d_swap_buffers();
	sceDisplayWaitVblankStart();
	++g_dialog_present_frames;
}

void AdrenalineMenuPresentDialogClosing(void) {
	/* Keep the menu frame on screen while SceCommonDialog finishes closing.
	 * Without pumping this transition, cancelling one prompt and opening
	 * another leaves the dialog half-closed and the next update crashes. */
	static int logged;
	int frames = 0;
	int i;

	for (i = 0; i < 6; ++i) {
		int result;

		if (g_dialog_snapshot_ready &&
		    g_dialog_snapshots[g_dialog_snapshot_slot] != NULL) {
			vita2d_start_drawing();
			vita2d_draw_texture(g_dialog_snapshots[g_dialog_snapshot_slot],
			                    0.0f, 0.0f);
			vita2d_end_drawing();
			vita2d_wait_rendering_done();
		} else {
			vita2d_clear_back_buffer();
		}

		result = sceCommonDialogIsRunning()
			? vita2d_common_dialog_update()
			: SCE_COMMON_DIALOG_ERROR_NOT_RUNNING;
		vita2d_swap_buffers();
		sceDisplayWaitVblankStart();
		++frames;

		if (!sceCommonDialogIsRunning() ||
		    result == SCE_COMMON_DIALOG_ERROR_NOT_RUNNING)
			break;
		sceKernelDelayThread(16 * 1000);
	}

	if (!logged) {
		ra_menu_log("dialog close pump frames=%d\n", frames);
		logged = 1;
	}
}



/* Input safety: RA network jobs (sign-in, verify) are modal. While one runs,
 * the cursor stays put and no row may activate. resetPad() also makes sure a
 * button pressed during the wait cannot fire when the job ends. */
static int g_ra_input_locked;
static int g_ra_swallow_enter;

/* Rows with hold_ms require a sustained confirm press (Sign Out). */
static int g_hold_index = -1;
static unsigned long long g_hold_start_us;
static int g_hold_fired;

static void consumeEnterUntilRelease(void) {
	if (!g_ra_swallow_enter) {
		return;
	}
	if (!current_pad[PAD_ENTER]) {
		released_pad[PAD_ENTER] = 0;
		g_ra_swallow_enter = 0;
		return;
	}
	pressed_pad[PAD_ENTER] = 0;
	released_pad[PAD_ENTER] = 0;
	hold_pad[PAD_ENTER] = 0;
	hold2_pad[PAD_ENTER] = 0;
}

static void activateMenuEntry(MenuEntry *entry) {
	if (entry == NULL)
		return;
	if (entry->callback_arg != NULL)
		entry->callback_arg(entry->callback_data);
	else if (entry->callback != NULL)
		entry->callback();
}

static int holdCallbackReady(int index, MenuEntry *entry) {
	unsigned long long now;

	if (entry == NULL || entry->callback == NULL || entry->hold_ms <= 0) {
		return 0;
	}

	if (!current_pad[PAD_ENTER]) {
		g_hold_index = -1;
		g_hold_fired = 0;
		return 0;
	}

	now = sceKernelGetProcessTimeWide();
	if (g_hold_index != index) {
		g_hold_index = index;
		g_hold_start_us = now;
		g_hold_fired = 0;
		return 0;
	}

	if (!g_hold_fired &&
	    (now - g_hold_start_us) >= (unsigned long long)entry->hold_ms * 1000ULL) {
		g_hold_fired = 1;
		/* The row list may rebuild when this action completes (Sign Out).
		 * Swallow this press so its release cannot activate the new row. */
		g_ra_swallow_enter = 1;
		ra_menu_log("hold confirm name=%s hold_ms=%d\n", entry->name,
		            entry->hold_ms);
		activateMenuEntry(entry);
		return 1;
	}

	return 0;
}

static void ctrlMenu(void) {
	if (released_pad[PAD_PSBUTTON]) {
		ExitAdrenalineMenu();
	}

	/* Modal RA operation: block navigation and activation, keep the cursor. */
	if (ra_menu_is_busy()) {
		if (!g_ra_input_locked) {
			g_ra_input_locked = 1;
			ra_menu_log("menu input locked: RA worker busy\n");
		}
		resetPad();
		return;
	}
	if (g_ra_input_locked) {
		g_ra_input_locked = 0;
		g_ra_swallow_enter = 1;
		resetPad();
		ra_menu_log("menu input unlocked\n");
	}
	consumeEnterUntilRelease();

	if (!open_options) {
		/* Triangle cycles the achievement-page category filter (00.10.83). Only
		 * the RA tab sees this; every other tab keeps its current mapping. */
		if (pressed_pad[PAD_TRIANGLE] && tab_sel == RA_MENU_TAB_INDEX) {
			if (ra_menu_filter_cycle())
				menu_sel = 0;
		}

		if (released_pad[PAD_CANCEL]) {
			if (tab_sel == RA_MENU_TAB_INDEX && ra_menu_back()) {
				menu_sel = 0;
			} else {
				ExitAdrenalineMenu();
			}
		}

		if (pressed_pad[PAD_LTRIGGER]) {
			if (tab_sel == 2 && changed) {
				SaveAdrSetting();
			}

			if (tab_sel > 0) {
				menu_sel = 0;
				tab_sel--;
			} else if (tab_sel == 0) {
				menu_sel = 0;
				tab_sel = N_TABS-1;
			}


		}

		if (pressed_pad[PAD_RTRIGGER]) {
			if (tab_sel == 2 && changed) {
				SaveAdrSetting();
			}
			if (tab_sel < N_TABS-1) {
				menu_sel = 0;
				tab_sel++;
			} else if (tab_sel == N_TABS-1) {
				menu_sel = 0;
				tab_sel = 0;
			}
		}

		// SQUARE on Settings tab when on graphical options (filter, screen scale).
		if (pressed_pad[PAD_SQUARE] && tab_sel == 2 && menu_sel >= 1 && menu_sel <= 7) {
			g_hide_menu = 1;
		}
		if (released_pad[PAD_SQUARE] || tab_sel != 2 || (tab_sel == 2 && (menu_sel < 1 || menu_sel > 7))) {
			g_hide_menu = 0;
		}

		// Achievements scroll vertically with the normal Up/Down selection.
		// Left/Right must not page or jump the list.
	}

	// Savestates
	if (tab_sel == 1) {
		ctrlStates();
	} else {
		if (tab_entries[tab_sel].moveable) {
			MenuEntry *menu_entries = tab_entries[tab_sel].entries;
			if (menu_entries) {
				if (menu_entries[menu_sel].type == MENU_ENTRY_TYPE_CALLBACK) {
					MenuEntry *entry = &menu_entries[menu_sel];
					int activated = 0;

					if (entry->hold_ms > 0) {
						activated = holdCallbackReady(menu_sel, entry);
					} else if (released_pad[PAD_ENTER]) {
						if (entry->callback_arg != NULL ||
						    entry->callback != NULL)
							activateMenuEntry(entry);
						activated = 1;
					}

					if (activated && ra_menu_take_page_change())
						menu_sel = 0;
				} else if (menu_entries[menu_sel].type == MENU_ENTRY_TYPE_OPTION) {
					int option_changed = 0;
					if (released_pad[PAD_RIGHT] || released_pad[PAD_LEFT_ANALOG_RIGHT] || released_pad[PAD_ENTER]) {
						if ((*menu_entries[menu_sel].value) < menu_entries[menu_sel].n_options-1) {
							(*menu_entries[menu_sel].value)++;
						} else {
							(*menu_entries[menu_sel].value) = 0;
						}
						option_changed = 1;
					}

					if (released_pad[PAD_LEFT] || released_pad[PAD_LEFT_ANALOG_LEFT]) {
						if ((*menu_entries[menu_sel].value) > 0) {
							(*menu_entries[menu_sel].value)--;
						} else {
							(*menu_entries[menu_sel].value) = menu_entries[menu_sel].n_options-1;
						}
						option_changed = 1;
					}

					if (option_changed) {
						changed = 1;
						if (tab_sel == RA_MENU_TAB_INDEX)
							ra_menu_options_changed();
					}
				} else if (menu_entries[menu_sel].type == MENU_ENTRY_TYPE_SCALE) {
					if (hold2_pad[PAD_RIGHT] || hold2_pad[PAD_LEFT_ANALOG_RIGHT] || hold2_pad[PAD_ENTER]) {
						float *a = (float *)menu_entries[menu_sel].value;
						(*a) += 0.005f;
					}

					if (hold2_pad[PAD_LEFT] || hold2_pad[PAD_LEFT_ANALOG_LEFT]) {
						float *a = (float *)menu_entries[menu_sel].value;
						if (*a > 0.005f) {
							(*a) -= 0.005f;
						}
					}

					changed = 1;
				}
			}

			if ((hold_pad[PAD_UP] || hold_pad[PAD_LEFT_ANALOG_UP]) && !g_hide_menu) {
				/* The RA tab can consume the move to scroll the Active Challenges
				 * window; when it does, the cursor and every row above the divider
				 * stay where they are. */
				if (tab_sel == RA_MENU_TAB_INDEX && ra_menu_nav(-1, menu_sel)) {
					/* consumed */
				} else {
					menu_sel = (tab_entries[tab_sel].n_entries + menu_sel - 1) % tab_entries[tab_sel].n_entries;

					// Skip empty lines
					if (strcmp(tab_entries[tab_sel].entries[menu_sel].name, "") == 0) {
						menu_sel = (tab_entries[tab_sel].n_entries + menu_sel - 1) % tab_entries[tab_sel].n_entries;
					}
				}
			}

			if ((hold_pad[PAD_DOWN] || hold_pad[PAD_LEFT_ANALOG_DOWN]) && !g_hide_menu) {
				if (tab_sel == RA_MENU_TAB_INDEX && ra_menu_nav(1, menu_sel)) {
					/* consumed */
				} else {
					menu_sel = (tab_entries[tab_sel].n_entries + menu_sel + 1) % tab_entries[tab_sel].n_entries;

					// Skip empty lines
					if (strcmp(tab_entries[tab_sel].entries[menu_sel].name, "") == 0) {
						menu_sel = (tab_entries[tab_sel].n_entries + menu_sel + 1) % tab_entries[tab_sel].n_entries;
					}
				}
			}
		}
	}
}

void *pops_data = NULL;

/* Which surface the teardown presents while *we* own the scanout:
 * 0 = `pops_data`, 1 = pspemu's VRAM. Set in the shell_up block, which only
 * changes it when the two surfaces disagree about being painted. */
static int g_shell_src_vram;

/* Latched shell window, file scope so the draw-path selection can use it. */
static int g_shell_window;

int AdrenalineDraw(SceSize args, void *argp) {
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter_button);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_DATE_FORMAT, &date_format);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_TIME_FORMAT, &time_format);

	vita2d_init();

	{
		SceCommonDialogConfigParam dialog_config;
		int dialog_result;

		sceCommonDialogConfigParamInit(&dialog_config);
		dialog_config.language = (SceSystemParamLang)language;
		dialog_config.enterButtonAssign =
			(SceSystemParamEnterButtonAssign)enter_button;
		dialog_result = sceCommonDialogSetConfigParam(&dialog_config);
		if (dialog_result < 0) {
			ra_menu_log("common dialog config failed=0x%08X\n",
			            (unsigned)dialog_result);
		}
	}

	font = vita2d_load_default_pgf();

	ra_notify_init();
	ra_runtime_init();

	vita2d_texture *native_tex = vita2d_create_empty_texture_data(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)CONVERT_ADDRESS(NATIVE_FRAMEBUFFER), SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR);
	if (!native_tex) {
		return -1;
	}

	vita2d_texture *psp_tex = vita2d_create_empty_texture(PSP_SCREEN_LINE, PSP_SCREEN_HEIGHT);
	if (!psp_tex) {
		return -1;
	}

	void *psp_data = vita2d_texture_get_datap(psp_tex);

	vita2d_texture *pops_tex = vita2d_create_empty_texture_format(SCREEN_LINE, SCREEN_HEIGHT, SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR);
	if (!pops_tex) {
		return -1;
	}

	pops_data = vita2d_texture_get_datap(pops_tex);
	if (pops_data) {
		memset(pops_data, 0, SCREEN_LINE * SCREEN_HEIGHT * sizeof(uint32_t));
	}

	/* 00.60.01: ASCII-only text. No runtime TTF stack, no symbol atlas, no
	 * underscores: ra_text drops every non-ASCII codepoint. */
	if (!ra_text_init(font))
		ra_menu_log("status: text init failed; PGF only\n");
	ra_menu_log("status: text=ASCII-only; non-ASCII stripped\n");


	vita2d_shader *opaque_shader = vita2d_create_shader((SceGxmProgram *)opaque_v, (SceGxmProgram *)texture_f);
	vita2d_shader *sharp_shader = vita2d_create_shader((SceGxmProgram *)sharp_bilinear_v, (SceGxmProgram *)sharp_bilinear_f);
	vita2d_shader *advanced_aa_shader = vita2d_create_shader((SceGxmProgram *)advanced_aa_v, (SceGxmProgram *)advanced_aa_f);
	vita2d_shader *lcd3x_shader = vita2d_create_shader((SceGxmProgram *)lcd3x_v, (SceGxmProgram *)lcd3x_f);
	vita2d_shader *sharp_simple_shader = vita2d_create_shader((SceGxmProgram *)sharp_bilinear_simple_v, (SceGxmProgram *)sharp_bilinear_simple_f);
	vita2d_shader *scale2x_shader = vita2d_create_shader((SceGxmProgram *)scale2x_v, (SceGxmProgram *)scale2x_f);
	vita2d_shader *scale2x_plus_shader = vita2d_create_shader((SceGxmProgram *)scale2x_plus_v, (SceGxmProgram *)scale2x_plus_f);
	vita2d_shader *scale3x_shader = vita2d_create_shader((SceGxmProgram *)scale3x_v, (SceGxmProgram *)scale3x_f);
	vita2d_shader *overlay_shader = vita2d_create_shader((SceGxmProgram *)texture_v, (SceGxmProgram *)texture_f);

	// f.lux shader
	vita2d_shader *flux_shader = vita2d_create_shader_untextured((SceGxmProgram *)gxm_program_vflux_v, (SceGxmProgram *)gxm_program_vflux_f);

	// f.lux vertices
	SceUID flux_vertices_id = sceKernelAllocMemBlock("flux vertices", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(sizeof(float)*12, 4 * 1024), NULL);
	float *flux_vertices;
	sceKernelGetMemBlockBase(flux_vertices_id, (void **)&flux_vertices);
	flux_vertices[0] = 0.0f;
	flux_vertices[1] = 0.0f;
	flux_vertices[2] = 0.5f;
	flux_vertices[3] = 0.0f;
	flux_vertices[4] = 544.0f;
	flux_vertices[5] = 0.5f;
	flux_vertices[6] = 960.0f;
	flux_vertices[7] = 544.0f;
	flux_vertices[8] = 0.5f;
	flux_vertices[9] = 960.0f;
	flux_vertices[10] = 0.0f;
	flux_vertices[11] = 0.5f;
	sceGxmMapMemory(flux_vertices, ALIGN(sizeof(float)*12, 4 * 1024), SCE_GXM_MEMORY_ATTRIB_READ);

	// f.lux indices
	SceUID flux_indices_id = sceKernelAllocMemBlock( "flux indices", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(sizeof(uint16_t)*4, 4 * 1024), NULL);
	uint16_t *flux_indices;
	sceKernelGetMemBlockBase(flux_indices_id, (void **)&flux_indices);
	for (int i=0;i<4;i++){
		flux_indices[i] = i;
	}
	sceGxmMapMemory(flux_indices, ALIGN(sizeof(uint16_t)*4, 4 * 1024), SCE_GXM_MEMORY_ATTRIB_READ);

	// f.lux model-view-projection matrix
	matrix4x4 projection, modelview, mvp;
	matrix4x4_identity(modelview);
	matrix4x4_init_orthographic(projection, 0, 960, 544, 0, -1, 1);
	matrix4x4_multiply(mvp, projection, modelview);

	// f.lux shaders uniforms
	const SceGxmProgramParameter *vflux_color_param = sceGxmProgramFindParameterByName(gxm_program_vflux_f, "color");
	const SceGxmProgramParameter *vflux_wvp_param = sceGxmProgramFindParameterByName(gxm_program_vflux_v, "wvp");
	vita2d_shader *shader = opaque_shader;

	settings_semaid = sceKernelCreateSema("AdrenalineSettingsSemaphore", 0, 0, 1, NULL);
	if (settings_semaid < 0) {
		return settings_semaid;
	}

	// FPS counting
	// SceUInt64 cur_micros = 0, delta_micros = 0, last_micros = 0;
	// uint32_t frames = 0;
	// float fps = 0.0f;

	// keep track of entering pops mode
	int lastPops = 0;

	int direct_sharp_scale_requested = -1;
	int is_vita_tv = sceKernelIsPSVitaTV();
	int notification_was_visible = 0;
	/* Overlay hand-off hysteresis (00.10.85): while non-zero, the overlay keeps the
	 * frame even though the last card/tile is gone, so a title that toggles indicators
	 * every second does not hand the display back and forth. */
	unsigned long long g_overlay_hold_until_us = 0;
	/* 1 while the overlay has parked the direct sharp scaler to keep the frame
	 * for itself. Cleared once the last card and tile are gone. */
	int overlay_owns_frame = 0;



	/* 00.10.127: a PS press is the only reliable trigger for the crash the
	 * operator still reports, and the last line a core leaves behind is
	 * `pad: PS released with the menu closed` - a line that proves only that no
	 * *logged* step ran afterwards, not that pspemu faulted. For 8 frames after
	 * any PS transition this counter prints a start and an end line per frame, so
	 * a core reads as "the frame after the press did not finish" and, with the
	 * heap sizes next to it, says whether the process was also running out of
	 * memory at that moment. */
	static int ps_window_frames;
	static int shell_handback;

	while (1) {
		int draw_native = *(uint32_t *)CONVERT_ADDRESS(DRAW_NATIVE);
		SceAdrenaline *adrenaline = (SceAdrenaline *)CONVERT_ADDRESS(ADRENALINE_ADDRESS);

		ra_runtime_tick(adrenaline, menu_open != 0 ||
		                sceCommonDialogIsRunning() != 0);
		ra_menu_tick(adrenaline);

		/* On the XMB with no menu, pspemu owns the screen. A non-Original
		 * graphics filter otherwise leaves this loop compositing a cached
		 * game surface over the XMB, which is the reported black screen.
		 * Hand the framebuffer back once and keep pspemu's presentation. */
		if (ra_runtime_in_shell() && !menu_open && !g_exiting &&
		    !g_menu_open_request) {
			if (!shell_handback) {
				sceGxmDisplayQueueFinish();
				SetPspemuFrameBuffer((void *)SCE_PSPEMU_FRAMEBUFFER);
				sceDisplayWaitVblankStart();
				shell_handback = 1;
				ra_menu_log("draw: shell handback; pspemu owns the XMB\n");
			}
			sceDisplayWaitVblankStart();
			continue;
		}
		shell_handback = 0;

		{
			int ra_count = 0;

			tab_entries[1].entries = NULL;
			tab_entries[1].n_entries = 0;
			tab_entries[1].moveable = 0;

			tab_entries[RA_MENU_TAB_INDEX].entries =
				ra_menu_entries(&ra_count);
			tab_entries[RA_MENU_TAB_INDEX].n_entries = ra_count;
			if (ra_menu_take_page_change())
				menu_sel = 0;
			if (tab_sel == RA_MENU_TAB_INDEX &&
			    menu_sel >= ra_count)
				menu_sel = 0;
			if (tab_sel == RA_MENU_TAB_INDEX) {
				int visible = RA_MENU_VISIBLE_ROWS;
				int max_scroll = ra_count > visible ? ra_count - visible : 0;
				int scroll = tab_entries[RA_MENU_TAB_INDEX].scroll;

				if (menu_sel < scroll)
					scroll = menu_sel;
				else if (menu_sel >= scroll + visible)
					scroll = menu_sel - visible + 1;
				if (scroll > max_scroll)
					scroll = max_scroll;
				if (scroll < 0)
					scroll = 0;
				tab_entries[RA_MENU_TAB_INDEX].scroll = scroll;
			}
		}


		int request_direct_sharp_scale =
			!is_vita_tv && !adrenaline->pops_mode && !draw_native
			&& !ra_runtime_title_is_pops()
			&& config.graphics_filtering == 5 && config.no_smooth_graphics != 0
			&& config.flux_mode == FLUX_MODE_OFF
			&& config.psp_screen_scale_x == 2.0f
			&& config.psp_screen_scale_y == 2.0f;

		/* `overlay_owns_frame` parks the scaler on purpose (see the direct-path
		 * block below); re-requesting it here would take the display back from
		 * the overlay on the very next frame. */
		if (!overlay_owns_frame &&
		    request_direct_sharp_scale != direct_sharp_scale_requested) {
			int res = adrSetPspemuDirectSharpScale(request_direct_sharp_scale);
			direct_sharp_scale_requested = request_direct_sharp_scale;
			g_direct_sharp_scale_active = request_direct_sharp_scale && res >= 0;
		}

		// pause/unpause pops once after switching from psp to pops mode
		// this pause/unpause fixes slowdown in PS1 games that used to require manually entering/exiting menu
		if (!adrenaline->pops_mode && !ra_runtime_title_is_pops()) {
			lastPops = 0;
		}

		if (adrenaline->pops_mode && lastPops == 0) {
			if (pops_data) {
				memset(pops_data, 0, SCREEN_LINE * SCREEN_HEIGHT * sizeof(uint32_t));
			}
			ScePspemuPausePops(1);
			sceDisplayWaitVblankStart();
			ScePspemuPausePops(0);
			sceDisplayWaitVblankStart();
			lastPops = 1;
		}

		// Draw savestate screen
		if (adrenaline->savestate_mode != SAVESTATE_MODE_NONE) {
			vita2d_start_drawing();
			vita2d_clear_screen();

			char *title = "Please wait...";
			pgf_draw_textf(ALIGN_CENTER(SCREEN_WIDTH, vita2d_pgf_text_width(font, FONT_SIZE, title)), FONT_Y_LINE(8), WHITE, FONT_SIZE, title);

			// End drawing
			vita2d_end_drawing();
			vita2d_swap_buffers();

			// Sync
			if (!adrenaline->pops_mode)
				sceCompatLCDCSync();

			continue;
		}

		// Read pad
		readPad();
		/* 00.10.131: one heartbeat line per frame, and the counter is decremented
		 * *here* - at the top of the iteration, where every frame passes.
		 *
		 * The 00.10.127 version decremented at the bottom of the loop, and the
		 * direct path `continue`s long before that, so after a single PS press in
		 * the Adrenaline settings screen the window never expired:
		 * `RA_SETUP_20261029_231356_077.log` has thirty identical
		 * `pad: PS window frame start (n=8)` lines, and each of them was a
		 * *synchronous* pair of card writes on the frame thread - the frames around
		 * them read `slow loop 50054 us ... other=44954`. That is the stutter the
		 * operator reports when opening the exit menu.
		 *
		 * Queued, not sync: the press and release lines above/below are the
		 * brackets; this is only a heartbeat, and eight of them cost one drain.
		 */
		if (ps_window_frames > 0) {
			--ps_window_frames;
			ra_menu_log("pad: PS window frame %d/8\n", 8 - ps_window_frames);
		}
		{
			/* Diagnostic for the quit-window investigation (docs/54 row 21): what
			 * state is the machine in when the PS button is pressed? The
			 * "prompt needs several presses" symptom lives here, and the state at
			 * the press - who owns the scanout, whether we are presenting, whether
			 * POPS is still active - is what decides it. One line per press
			 * transition, so it cannot flood. */
			static int ps_was_down;
			int ps_down = current_pad[PAD_PSBUTTON] != 0;

			if (ps_down != ps_was_down) {
				SceDisplayFrameBuf fb;

				ps_was_down = ps_down;
				ps_window_frames = 8;
				ra_runtime_shell_transition();
				memset(&fb, 0, sizeof(fb));
				fb.size = sizeof(fb);
				sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE);
				/* 00.10.132: queued, not synchronous.
				 *
				 * 00.10.128 made every `pad: PS` line a synchronous card write so a
				 * crash could not swallow it, and the crash it was for is fixed
				 * (00.10.129). What it left behind is the stutter the operator kept
				 * reporting on the exit menu: one press is two or three of these
				 * lines, each an open+write+close on ux0, and
				 * `RA_SETUP_20261029_233841_655.log` shows the frames around them at
				 * 50-133 ms with `other` carrying all of it
				 * (`slow loop 50460 us ... other=40155`). The bracket did its job;
				 * it does not have to stay expensive. */
				ra_menu_log("pad: PS %s scanout=%p owns_ours=%d title_pops=%d "
				            "shell=%d menu=%d filter=%d\n",
				            ps_down ? "down" : "up", fb.base,
				            fb.base != (void *)SCE_PSPEMU_FRAMEBUFFER,
				            ra_runtime_title_is_pops(), ra_runtime_in_shell(),
				            menu_open, (int)config.graphics_filtering);
				/* Bracket the press (00.10.121): a crash between this line and
				 * `menu: open` happens in pspemu's own PS-button handling, because
				 * nothing of ours runs in between. */
				if (!ps_down && menu_open == 0)
					ra_menu_log("pad: PS released with the menu closed; the shell "
					            "owns this press\n");
			}
		}

		/* 00.10.129: pspemu's settings hook asked for the menu (see
		 * g_menu_open_request). Opened here, on the frame thread, so the pspemu
		 * pointers, the pops pause and the runtime are only ever touched from one
		 * thread.
		 *
		 * 00.10.132: and always opened, whatever the session state.
		 *
		 * 00.10.131 refused this when no game session existed, reading
		 * `EnterAdrenalineMenu()` as an RA-only menu. It is not: it is
		 * **Adrenaline's own menu** (it builds the CFW/savestate settings rows and
		 * hosts the RA entries), so refusing it removed the menu entirely -
		 * `RA_SETUP_20261029_234003_607.log` line 48,
		 * `menu: open request ignored; no game session yet`, and the operator's
		 * report that the menu "won't even display now" after a relaunch. A menu
		 * that is half of Adrenaline's UI must never depend on a RetroAchievements
		 * session. */
		if (g_menu_open_request) {
			g_menu_open_request = 0;
			if (menu_open == 0) {
				ra_menu_log_sync("menu: open requested by pspemu's settings hook "
				                 "(session=%d)\n",
				                 ra_runtime_session_ordinal());
				EnterAdrenalineMenu();
			}
		}


		// Double click detection
		if (menu_open == 0) {
			if (doubleClick(SCE_CTRL_PSBUTTON, 300 * 1000)) {
				/* 00.10.134: the jump is back, with the half that was missing.
				 *
				 * Stock Adrenaline answers a PS double press with
				 * `sceAppMgrLaunchAppByName2(ADRENALINE_TITLEID, ...)`, which lands on
				 * the LiveArea **without** stopping pspemu: the emulated PSP keeps
				 * running behind the shell. `00.10.133` disabled the path because
				 * neither of the two obvious answers worked - a session that keeps
				 * running cannot be seen, and a session left open shows the player
				 * in game while they are looking at the shell.
				 *
				 * So it stops the session through the same call the menu's Stop row
				 * uses, and arms a guarded restart: `ra_runtime_background_stop()`
				 * records that we left on purpose, and `ra_runtime_tick()` re-starts
				 * once the game has been visible for 30 consecutive frames, at least
				 * 3 s after the jump, and never in the middle of disc work. */
				ra_menu_log_sync("pad: PS double-click; leaving for the LiveArea and "
				                 "stopping the session (it restarts on return)\n");
				stopUsb(usbdevice_modid);
				ra_runtime_background_stop();

				if (sceAppMgrLaunchAppByName2(ADRENALINE_TITLEID, NULL, NULL) < 0) {
					ScePspemuErrorExit(0);
				}
			}
		}

		// Fast forward in pops. Hardcore disables it: RA evaluation is
		// vblank-driven, and fast-forward can advance more than one emulated
		// frame per vblank. A hit condition can then be sampled less often than
		// the game runs, which is exactly the leaderboard hole reported upstream.
		if (adrenaline->pops_mode) {
			int hardcore = ra_menu_hardcore_active();

			/* If fast-forward was left on before Hardcore became effective,
			 * turn it off once pspemu can be reached. */
			static int ff_off_checked;
			if (hardcore) {
				if (!ff_off_checked) {
					uint8_t *ff = (uint8_t *)ScePspemuConvertAddress(
					    0xABCD00A9, KERMIT_OUTPUT_MODE, 1);
					if (ff != NULL) {
						if (*ff) {
							*ff = 0;
							ScePspemuWritebackCache(ff, 1);
							ra_menu_log("pad: fast-forward forced off in hardcore\n");
						}
						ff_off_checked = 1;
					}
				}
			} else {
				ff_off_checked = 0;
			}

			// FSM for button combination
			static int combo_state = 0;
			if (current_pad[PAD_LTRIGGER] && current_pad[PAD_SELECT]) {
				combo_state = 1;
			} else {
				if (combo_state == 1) {
					combo_state = 2;
				} else {
					combo_state = 0;
				}
			}

			if (combo_state == 2) {
				if (hardcore) {
					ra_menu_log("pad: fast-forward toggle blocked in hardcore\n");
				} else {
					uint8_t *val = (uint8_t *)ScePspemuConvertAddress(
					    0xABCD00A9, KERMIT_OUTPUT_MODE, 1);
					/* 00.10.127: this was the one store in the file with no guard on
					 * its conversion. A NULL result (pspemu between instances, the
					 * state right after a media change) went straight into a strided
					 * write through address 0, which is how a fault ends up reported
					 * at an address that is not code. */
					if (val != NULL) {
						*val = !(*val);
						ScePspemuWritebackCache(val, 1);
					} else {
						ra_menu_log("pad: fast-forward toggle skipped; conversion "
						            "returned NULL (pops=%d)\n",
						            (int)adrenaline->pops_mode);
					}
				}
				combo_state = 0;
			}
		}

		// Do not draw if dialog is running
		{
			static int dialog_was_running;
			static int ime_was_running;
			int dialog_running = sceCommonDialogIsRunning();
			int ime_running =
			    sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING;

			/* 00.50.14: the on-screen keyboard is not a quit.
			 *
			 * `sceCommonDialogIsRunning()` is true for every common dialog, and
			 * the block below used that as "the quit screen is up": it stopped the
			 * session on the transition and reopened it on cancel. An IME dialog
			 * (the RA account fields, any in-game OSK) therefore tore the session
			 * down and rebuilt it - four `startup begin` lines in one Patapon 3
			 * session, with the game load aborted each time. The IME is identified
			 * by its own status call, which the menu already uses to present it. */
			if (ime_running) {
				if (!ime_was_running) {
					ime_was_running = 1;
					ra_notify_suppress(1);
					ra_menu_log("quit: IME dialog up; session kept, cards "
					            "suppressed\n");
				}
				sceDisplayWaitVblankStart();
				continue;
			}
			if (ime_was_running) {
				ime_was_running = 0;
				ra_notify_suppress(0);
			}

			/* pspemu's settings UI is a common dialog, not a quit. Keep the
			 * session and let pspemu present its own UI; clear the flag when the
			 * dialog closes so a later real quit still gets the quit handling. */
			if (g_official_settings_dialog) {
				if (dialog_running) {
					g_official_settings_seen = 1;
					ra_notify_suppress(1);
					sceDisplayWaitVblankStart();
					continue;
				}
				if (g_official_settings_seen) {
					g_official_settings_dialog = 0;
					g_official_settings_seen = 0;
					ra_notify_suppress(0);
					ra_menu_log("settings: official settings dialog closed; session kept\n");
				} else if ((unsigned long long)sceKernelGetSystemTimeWide() -
				           g_official_settings_us > 3000000ULL) {
					/* The hand-off never raised a common dialog; do not let the
					 * stale flag swallow a later real quit. */
					g_official_settings_dialog = 0;
					ra_notify_suppress(0);
					ra_menu_log("settings: official settings raised no dialog; flag cleared\n");
				}
			}

			if (dialog_running) {
				if (!dialog_was_running) {
					dialog_was_running = 1;
					/* 00.10.157: the quit screen is not a place for cards.
					 *
					 * The operator saw a leaderboard banner "a split second into the quit
					 * screen": a board can start in the frame the dialog appears, before
					 * the paused flag has any effect, and the card is already queued by
					 * then. Dropping the queue on the transition is cheap and cannot
					 * affect a cancel - if the player backs out, the session is intact and
					 * only a banner was suppressed. Reaching the confirm frame still stops
					 * the session outright (below). */
					ra_notify_suppress(1);
					/* 00.10.158: close the session on the dialog, not on the confirm.
					 *
					 * The operator saw a "Leaderboard Submitted" card appear while quitting
					 * even after the queue was dropped on this transition: the *submit
					 * result* arrives later, from the network worker, and pushes a fresh
					 * card. Dropping cards cannot stop something that has not arrived yet.
					 * The only way to guarantee nothing evaluates or reports during a quit
					 * is to end the session before anything else happens - so the request
					 * stop (refuse reads, worker performs the cleanup, rcheevos can no
					 * longer start or submit a board) is issued here, on the transition.
					 *
					 * Cancel is handled on the close path below: if the game is still
					 * running, the session is requested again through the same guarded
					 * start the menu's Start row uses. */
					ra_runtime_request_stop();
					ra_runtime_abort_load();
					ra_menu_log("quit: system dialog up (title='%s' pops=%d menu=%d "
					            "native=%d); session closed first\n",
					            adrenaline != NULL ? adrenaline->title : "(null)",
					            adrenaline != NULL && adrenaline->pops_mode,
					            menu_open, draw_native);
				}
				sceDisplayWaitVblankStart();
				continue;
			}
			if (dialog_was_running) {
				/* The dialog just closed: this is the frame where the game is either
				 * gone (quit to the shell) or still there. Log what we see, then the
				 * next few frames with the branch they take, so the presentation fix
				 * can be targeted instead of guessed. */
				dialog_was_running = 0;
				ra_menu_log("quit: system dialog closed (title='%s' pops=%d menu=%d "
				            "native=%d)\n",
				            adrenaline != NULL ? adrenaline->title : "(null)",
				            adrenaline != NULL && adrenaline->pops_mode,
				            menu_open, draw_native);
				g_quit_log_frames = 6;

				/* 00.10.145: the operator's rule - once the exit is confirmed, release
				 * everything *now*, so nothing can fire during the teardown frames.
				 *
				 * The exit screen is a common dialog: pspemu keeps running behind it, and
				 * the frames between "Yes" and the shell taking over were still being
				 * evaluated. That is where the leaderboard banner came from ("hit with a
				 * leaderboard banner again while exiting"), and where an achievement could
				 * land on a game the player has already left.
				 *
				 * Cancel is safe by construction: if the game is still the POPS title, this
				 * does nothing and the session continues. If it is gone, the session, any
				 * load in flight and every queued card are dropped *before* the teardown
				 * frames, and the display is handed back so pspemu's own exit screen and
				 * the XMB reach the glass instead of our stale buffer (the black screen
				 * with XMB audio). */
				/* 00.10.146: universal - POPS and PSP alike.
				 *
				 * The 00.10.145 test keyed on the *type* (`ra_runtime_title_is_pops() == 0`
				 * fired for PSP titles, and POPS only after the shell was already up), so
				 * the mode the operator actually reproduces - a POPS title - was the one it
				 * missed. Confirmation is mode-agnostic: the title the app reports is no
				 * longer a game (`adrenaline->title` empty) or the shell owns the screen.
				 * Both are false while the dialog is cancelled, so cancel still costs
				 * nothing. */
				{
					int title_gone = adrenaline == NULL || adrenaline->title[0] == '\0';

					if (title_gone || ra_runtime_in_shell() != 0) {
					ra_menu_log_sync("quit: exit confirmed; releasing the session before "
					                 "the teardown frames\n");
					ra_runtime_request_stop();
					ra_runtime_abort_load();
					ra_notify_clear();
					/* 00.10.154: no display hand-back here.
					 *
					 * 00.10.146 added GXM finish + SetPspemuFrameBuffer + vblank on this
					 * frame to fix a black screen, and the next hardware run reported
					 * "Exit PspEmu again crashed the Vita". Grabbing the display from
					 * pspemu at the instant it is tearing down is the one thing in this
					 * window that can hang the console. Releasing the session is enough -
					 * nothing can fire once it is gone - so pspemu keeps its display and
					 * runs its own teardown. */
					}
					else {
						/* 00.10.158: cancel.
						 *
						 * The dialog transition closed the session (nothing may
						 * evaluate or report while a quit is in progress - that is
						 * the operator's rule), so reopening the game's session is
						 * this branch's job. This is the same guarded request the
						 * menu's Start row issues: if a session already exists it
						 * logs why and does nothing, and if one cannot be started
						 * the reason is in the log rather than silently swallowed. */
						ra_menu_log("quit: cancelled; game still running, "
						            "reopening the session\n");
						ra_notify_suppress(0);
						ra_runtime_request_start();
					}
				}
			}
		}

		/* Upstream presentation logic, deliberately unchanged: with

		 * `FILTER_ORIGINAL` (and no RA overlay on screen) the loop takes the direct
		 * path, which presents nothing at all and leaves pspemu owning the display -
		 * that is how Adrenaline's own quit sequence (sce "Please wait…" and then
		 * the XMB) reaches the screen. Every attempt to composite in this state
		 * (00.10.35-00.10.49) replaced pspemu's frames with a stale copy and froze
		 * the picture, so the display path is back to stock. */
		/* Quit-to-shell: keep compositing, do not hand the display back.
		 *
		 * Why this is the real fix (see docs/51 §6 and docs/53):
		 *  - `pops.c:sceDisplaySetFrameBufForCompatPatched` answers pspemu's
		 *    "what is my framebuffer?" query with `pops_data` for as long as
		 *    `ra_runtime_title_is_pops()` is true - that is the PS1 prompt fix.
		 *    The title is still the PS1 game for the whole teardown, so pspemu
		 *    renders Adrenaline's own "Please wait…" exit screen *into
		 *    `pops_data`*, not into SCE_PSPEMU_FRAMEBUFFER.
		 *  - The old handoff pointed the scanout at SCE_PSPEMU_FRAMEBUFFER and
		 *    stopped presenting. That surface still holds the last PS1 frame,
		 *    which is exactly the frozen picture the operator saw - and why the
		 *    probes measured "pspemu writes nothing" (VRAM never changed; the
		 *    pixels went to `pops_data`, which nobody was scanning).
		 *  - So: keep presenting `pops_tex` (whose storage *is* `pops_data`)
		 *    through the teardown. pspemu's exit screen then reaches the glass
		 *    by itself, which is the vanilla behaviour, restored without a
		 *    display handoff and without touching the PSP side.
		 *  - When the shell is really up, `ra_runtime_title_is_pops()` goes
		 *    false, the compat hook stops redirecting, the loop falls through to
		 *    the stock `draw_psp` path over SCE_PSPEMU_FRAMEBUFFER and the XMB is
		 *    what pspemu paints there. */
		int shell_up = 0;
		{
			static int saw_pops;
			static int logged_state;
			static int probe_frames;
			/* Latched once the shell owns the screen, cleared only when the shell
			 * is gone. `pops_mode` can flap during the handover (the emulated
			 * machine tears down and the shell starts), and a single frame of
			 * "game live again" put us back into compositing - which is the last
			 * game frame flashing between two exit screens (operator report,
			 * 00.10.75). Once the shell window opens, it stays open. */

			if (ra_runtime_title_is_pops())
				saw_pops = 1;

			shell_up = (menu_open == 0 && ra_runtime_in_shell() && saw_pops);
			if (shell_up) {
				g_shell_window = 1;
				/* A load still in flight when the game goes away is aborted, so its
				 * callback cannot land on a reset client (docs/58 §2.3). */
				if (!logged_state)
					ra_runtime_abort_load();
			}
			else if (!ra_runtime_in_shell())
				g_shell_window = 0;

			if (shell_up) {
				/* THE authority: who owns the scanout right now.
				 *
				 * `ra_pops_redirect_active()` says where pspemu was told to draw,
				 * which is not the same thing: the 00.10.61 run logged
				 * `redirect=1` for every teardown frame while `pops_data` was
				 * frozen and VRAM was being repainted - i.e. the flag describes the
				 * POPS session that just ended, not the frame flow of the moments
				 * after it. Presenting our own surface is only meaningful when the
				 * hardware is actually scanning it; when dc:0/base is pspemu's
				 * buffer, anything we draw is invisible and the glass shows VRAM
				 * (which is vanilla's behaviour - and vanilla's quit is correct).
				 */
				SceDisplayFrameBuf fb;
				int scanout_pspemu;
				unsigned int pops_hash = 0;
				unsigned int vram_hash = 0;

				memset(&fb, 0, sizeof(fb));
				fb.size = sizeof(fb);
				scanout_pspemu =
					sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) >= 0 &&
					fb.base == (void *)SCE_PSPEMU_FRAMEBUFFER;

				if (pops_data != NULL) {
					const unsigned int *vram =
						(const unsigned int *)SCE_PSPEMU_FRAMEBUFFER;
					int i;

					for (i = 0; i < SCREEN_LINE * SCREEN_HEIGHT; i += 97)
						pops_hash = pops_hash * 31u +
						            ((const unsigned int *)pops_data)[i];
					for (i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i += 97)
						vram_hash = vram_hash * 31u + vram[i];
				}

				/* Never present `pops_data` once the game is gone.
				 *
				 * `pops_data` is the *previous* session's surface: whatever it
				 * holds is the last PS1 frame. Presenting it is what put the game
				 * picture back on top of pspemu's native "Please wait…" (operator
				 * report, 00.10.68: the message appeared, then the last game frame
				 * blitted over it). The hash rule below could not see that: with
				 * both surfaces static it kept its initial `pops` choice.
				 *
				 * From the moment the shell owns the screen, the only surface worth
				 * showing - and the only one that can change - is pspemu's own
				 * buffer, so that is the one this window presents. */
				g_shell_src_vram = 1;

				if (probe_frames < 5) {
					++probe_frames;
					ra_menu_log("quit: probe frame=%d scanout=%p pops=%08X "
					            "vram=%08X redirect=%d pspemu_owns=%d src=%s\n",
					            probe_frames, fb.base, pops_hash, vram_hash,
					            ra_pops_redirect_active(), scanout_pspemu,
					            g_shell_src_vram ? "vram" : "pops");
				}

				if (!logged_state) {
					logged_state = 1;
					ra_menu_log("quit: teardown scanout=%p pspemu_owns=%d "
					            "redirect=%d pops_data=%p\n",
					            fb.base, scanout_pspemu,
					            ra_pops_redirect_active(), pops_data);
				}

				/* No special presentation here any more (00.10.72).
				 *
				 * Every teardown workaround tried in this window - stop presenting,
				 * hand the display back, follow the hashes, take the display, hold a
				 * frame - existed to paper over the shell drawing into `pops_data`
				 * while the glass showed something else. Treated like a filtered PSP
				 * title there is nothing to paper over: the shell asks the same
				 * question, gets the same answer (`pops_data`), and the composite
				 * path below presents what it drew. Only logging remains. */
			} else {
				logged_state = 0;
				probe_frames = 0;
				g_shell_src_vram = 0;
			}
		}

		{
			int notification_visible = ra_notify_visible();
			int live_indicators = ra_notify_indicator_count();
			int queued_cards = ra_notify_card_count();
			/* Hand-off hysteresis (00.10.85). Every pause/resume of the direct path
			 * is a display hand-off, and a title like Ys I & II raises and drops an
			 * indicator every second or two, so the hand-off itself was the tearing
			 * the operator sees (docs/26 §"Tearing (Ys)"). Once the overlay owns the
			 * frame it keeps it for a moment after the last card and tile are gone,
			 * so consecutive states share one hand-off instead of producing two per
			 * indicator. */
			if (notification_visible) {
				g_overlay_hold_until_us =
					(unsigned long long)sceKernelGetSystemTimeWide() + 1500000ULL;
			} else if (notification_was_visible &&
			           g_overlay_hold_until_us != 0 &&
			           (unsigned long long)sceKernelGetSystemTimeWide() <
			               g_overlay_hold_until_us) {
				notification_visible = 1;
			} else {
				g_overlay_hold_until_us = 0;
			}
			int direct_path = (config.graphics_filtering == FILTER_ORIGINAL ||
	                   g_direct_sharp_scale_active ||
	                   overlay_owns_frame) &&
	                  menu_open == 0 && draw_native == 0 &&
	                  (g_shell_window || !ra_runtime_title_is_pops());

			/* One line per change: which presentation path is live, how
			 * many indicator slots are held and how deep the banner queue
			 * is. Without it a missing tile is ambiguous between no state,
			 * a frame that was never taken, and a scaler that kept the
			 * display for itself; a missing banner is ambiguous between
			 * "never raised" and "queued behind another". */
			{
				static int logged_filter = -1;
				static int logged_direct = -1;
				static int logged_indicators = -1;
				static int logged_cards = -1;

				if (config.graphics_filtering != logged_filter ||
				    direct_path != logged_direct ||
				    live_indicators != logged_indicators ||
				    queued_cards != logged_cards) {
					logged_filter = config.graphics_filtering;
					logged_direct = direct_path;
					logged_indicators = live_indicators;
					logged_cards = queued_cards;
					ra_menu_log("overlay: render filter=%d smooth=%d direct=%d "
					            "sharp=%d live=%d cards=%d\n",
					            (int)config.graphics_filtering,
					            (int)config.no_smooth_graphics, direct_path,
					            g_direct_sharp_scale_active, live_indicators,
					            queued_cards);
				}
			}

			if (direct_path && notification_visible) {
				if (!notification_was_visible) {
					ra_menu_log("overlay: direct path paused for RA overlay\n");
					if (g_direct_sharp_scale_active) {
						/* The IFTU keeps scaling pspemu's framebuffer straight to the
						 * display, so a GXM frame carrying our overlay never reaches
						 * the screen. Park the scaler while the overlay owns the
						 * frame; the request is re-applied below once the last card
						 * and tile are gone. */
						adrSetPspemuDirectSharpScale(0);
						direct_sharp_scale_requested = -1;
						overlay_owns_frame = 1;
						ra_menu_log("overlay: direct sharp scale suspended\n");
					}
				}
				notification_was_visible = 1;
			}

			if (direct_path && !notification_visible) {
				if (notification_was_visible) {
					/* The overlay forced a GXM display queue. Finish it and
					 * hand the display back to pspemu before direct rendering
					 * continues, otherwise the last GXM frame can remain frozen. */
					sceGxmDisplayQueueFinish();
					if (overlay_owns_frame && request_direct_sharp_scale) {
						/* Re-arm the scaler before scanout returns to pspemu's
						 * buffer, otherwise the first frame after the overlay
						 * flashes the game unscaled in the middle of the screen. */
						if (adrSetPspemuDirectSharpScale(1) >= 0) {
							direct_sharp_scale_requested = 1;
							g_direct_sharp_scale_active = 1;
						}
					}
					SetPspemuFrameBuffer((void *)SCE_PSPEMU_FRAMEBUFFER);
					sceDisplayWaitVblankStart();
					notification_was_visible = 0;
					ra_menu_log("overlay: direct path resumed\n");
				}
				overlay_owns_frame = 0;
				continue;
			}
		}


		if (request_direct_sharp_scale && !g_direct_sharp_scale_active) {
			sceGxmDisplayQueueFinish();
		}

		// Draw display
		vita2d_start_drawing();
		vita2d_clear_screen();

		// Select shader
		if (config.graphics_filtering == 1) {
			shader = opaque_shader;
		} else if (config.graphics_filtering == 2) {
			shader = sharp_shader;
		} else if (config.graphics_filtering == 3) {
			shader = advanced_aa_shader;
		} else if (config.graphics_filtering == 4) {
			shader = lcd3x_shader;
		} else if (config.graphics_filtering == 5) {
			if (g_direct_sharp_scale_active) {
				shader = opaque_shader;
			} else {
				shader = sharp_simple_shader;
			}
		} else if (config.graphics_filtering == 6) {
			shader = scale2x_shader;
		} else if (config.graphics_filtering == 7) {
			shader = scale2x_plus_shader;
		} else if (config.graphics_filtering == 8) {
			shader = scale3x_shader;
		} else {
			shader = opaque_shader;
		}

		vita2d_texture_set_program(shader->vertexProgram, shader->fragmentProgram);
		vita2d_texture_set_wvp(shader->wvpParam);
		vita2d_texture_set_vertexInput(&shader->vertexInput);
		vita2d_texture_set_fragmentInput(&shader->fragmentInput);

		// Smooth
		if (config.no_smooth_graphics == 0) {
			vita2d_texture_set_filters(psp_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
			vita2d_texture_set_filters(pops_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
		} else if (config.no_smooth_graphics != 0 && config.graphics_filtering == 1) {
			vita2d_texture_set_filters(psp_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
			vita2d_texture_set_filters(pops_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
		} else {
			vita2d_texture_set_filters(psp_tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
			vita2d_texture_set_filters(pops_tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
		}

		if (g_quit_log_frames > 0) {
			--g_quit_log_frames;
			ra_menu_log("quit: frame title='%s' pops=%d shell=%d native=%d menu=%d\n",
			            adrenaline != NULL ? adrenaline->title : "(null)",
			            adrenaline != NULL && adrenaline->pops_mode,
			            ra_runtime_in_shell(), draw_native, menu_open);
		}

		int draw_psp = (!adrenaline->pops_mode && !ra_runtime_title_is_pops() &&
		                !draw_native) || adrenaline->draw_psp_screen_in_pops;

		/* Follow pspemu, always. `ra_pops_redirect_active()` is set by the
		 * compat hook in pops.c from pspemu's own display calls, so it says where
		 * pspemu is drawing *now* - during the game, while the PS prompt is up,
		 * and through the POPS teardown - without consulting the runtime's
		 * identity flags, which flip before the first exit frame.
		 *
		 * If pspemu is using its own framebuffer, show that framebuffer; if it is
		 * using our texture, the branch below shows the texture. That single rule
		 * replaces every earlier heuristic (hash sampling, wait screens, display
		 * handoffs) and cannot desynchronise: it is derived from the same call
		 * that decides the answer. */
		if (!draw_psp && !draw_native && !ra_pops_redirect_active())
			draw_psp = 1;

		/* 00.60.01: while the pspemu shell owns the screen, both cached surfaces
		 * (`psp_tex`, `pops_tex`) still hold the last game/exit frame. Never draw
		 * either of them then - black is the honest background, and it also covers
		 * a notification composited over the XMB. */
		int shell_background = ra_runtime_in_shell();
		{
			static int logged_shell_black;

			if (shell_background && !logged_shell_black) {
				ra_menu_log("draw: shell background black; cached psp/pops surface skipped\n");
				logged_shell_black = 1;
			} else if (!shell_background) {
				logged_shell_black = 0;
			}
		}

		/* ... and while the shell owns the screen after a PS1 title, present the
		 * surface that is actually being painted (see the shell_up block). */

		/* PS1 gameplay is pspemu's to present (00.10.71).
		 *
		 * With the redirect gone, POPS renders into pspemu's own buffer and pspemu
		 * presents it - the vanilla arrangement. Compositing it as well means two
		 * surfaces alternate every vblank (ours, one frame behind, and pspemu's
		 * live one), which is the flicker reported against 00.10.70.
		 *
		 * So a PS1 title is allowed into the direct path above: the loop presents
		 * nothing and pspemu owns the display, exactly as for a PSP title in
		 * Original mode. `direct_path` is still suspended whenever a notification
		 * or a live tile is on screen, so the composite branch below takes over for
		 * exactly the frames that need our overlay - and it composes pspemu's own
		 * buffer, which is the picture a PS1 title now renders into. */

		if (draw_psp) {
			// Copy PSP framebuffer
			/* The copy is what actually reaches the screen in this branch, so its
			 * result is logged when it changes: a failing `sceDmacMemcpy` would
			 * leave `psp_tex` frozen while the source keeps changing, which looks
			 * exactly like a frozen game frame. */
			static int logged_dma = 1;
			/* 00.10.150: one copy at open, then hold it.
			 *
			 * 00.10.130 stopped the DMA entirely while the menu is open to kill the
			 * flicker (a copy taken while pspemu was still finishing a frame painted
			 * half of one behind the menu). The side effect the operator reported is the
			 * background itself: the menu opened over whatever frame happened to be
			 * copied before the pause, which can be seconds old.
			 *
			 * `menu_open_frames` counts frames since the menu opened. The copy runs
			 * exactly once, on the *second* frame: `EnterAdrenalineMenu()` has already
			 * issued `ScePspemuPausePops(1)` and one v-blank has passed by then, so
			 * pspemu is not mid-write and the copy cannot tear. Every later frame keeps
			 * that copy, which is what preserves the anti-flicker behaviour. */
			static int copy_skipped;
			static int menu_open_frames;
			static uint32_t last_game_id;
			RaRuntimeInfo menu_info;
			int dma_res;

			/* A different game means the cached PSP surface belongs to the
			 * previous title. Reset the one-copy-per-open state so this menu
			 * open takes a fresh frame instead of showing that old frame. */
			ra_runtime_copy_info(&menu_info);
			if (menu_info.game_id != last_game_id) {
				last_game_id = menu_info.game_id;
				menu_open_frames = 0;
				copy_skipped = 0;
			}

			/* 00.60.01: when the shell owns the screen, psp_tex still holds the last
			 * game frame (pspemu paints the XMB elsewhere), so drawing it behind the
			 * menu showed a stale game/exit screen. Black is the honest background. */
			if (shell_background) {
				menu_open_frames = 0;
				copy_skipped = 0;
				dma_res = 0;
			} else if (menu_open) {
				++menu_open_frames;
				if (!copy_skipped && menu_open_frames == 2) {
					dma_res = sceDmacMemcpy(psp_data,
					                        (void *)SCE_PSPEMU_FRAMEBUFFER,
					                        SCE_PSPEMU_FRAMEBUFFER_SIZE);
					ra_menu_log("draw: psp copy taken once at menu open (res=0x%08X)\n",
					            (unsigned)dma_res);
				} else {
					if (!copy_skipped) {
						copy_skipped = 1;
						ra_menu_log("draw: psp copy held while the menu is open\n");
					}
					dma_res = 0;
				}
			} else {
				menu_open_frames = 0;
				if (copy_skipped) {
					copy_skipped = 0;
					ra_menu_log("draw: psp copy resumed (menu closed)\n");
				}
				dma_res = sceDmacMemcpy(psp_data,
				                        (void *)SCE_PSPEMU_FRAMEBUFFER,
				                        SCE_PSPEMU_FRAMEBUFFER_SIZE);
			}

			if (dma_res != logged_dma) {
				logged_dma = dma_res;
				ra_menu_log("draw: psp copy res=0x%08X size=0x%X\n",
				            (unsigned)dma_res,
				            (unsigned)SCE_PSPEMU_FRAMEBUFFER_SIZE);
			}

			// Draw psp screen
			if (shell_background) {
				vita2d_draw_rectangle(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
				                      COLOR_ALPHA(BLACK, 0xFF));
			} else {
				vita2d_draw_texture_scale_rotate_hotspot(psp_tex, 480.0f, 272.0f, config.psp_screen_scale_x, config.psp_screen_scale_y, 0.0, 240.0, 136.0);
			}
		} else if (draw_native) {
			vita2d_draw_texture_scale_rotate_hotspot(native_tex, 480.0f, 272.0f, 1.0f, 1.0f, 0.0, 480.0, 272.0);
		} else {
			// Draw pops screen
			if (shell_background) {
				vita2d_draw_rectangle(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
				                      COLOR_ALPHA(BLACK, 0xFF));
			} else {
			/* "Original" used to be a passthrough of pspemu's own buffer; a PS1
			 * title now renders into pops_tex instead (pops.c) and is composited,
			 * so draw it 1:1 to keep the same picture. */
			float ps1_scale_x = config.graphics_filtering == FILTER_ORIGINAL
			                        ? 1.0f
			                        : config.ps1_screen_scale_x;
			float ps1_scale_y = config.graphics_filtering == FILTER_ORIGINAL
			                        ? 1.0f
			                        : config.ps1_screen_scale_y;
			vita2d_draw_texture_scale_rotate_hotspot(pops_tex, 480.0f, 272.0f, ps1_scale_x, ps1_scale_y, 0.0, 480.0, 272.0);
			if (overlay_texture && !ra_runtime_in_shell()) {
				vita2d_texture_set_program(overlay_shader->vertexProgram, overlay_shader->fragmentProgram);
				vita2d_texture_set_wvp(overlay_shader->wvpParam);
				vita2d_texture_set_vertexInput(&overlay_shader->vertexInput);
				vita2d_texture_set_fragmentInput(&overlay_shader->fragmentInput);

				vita2d_texture_set_filters(overlay_texture, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
				vita2d_draw_texture_scale_rotate_hotspot(overlay_texture, 480.0f, 272.0f, ps1_scale_x, ps1_scale_y, 0.0, 480.0, 272.0);
			}
			}
		}


		/* RA overlay first, menu second: the Adrenaline menu must sit in front of
		 * the session text and the notification cards, not behind them. Painter
		 * order is the z-order here, so the call site is the whole fix. */
		if (ra_runtime_overlay_visible() && !ra_runtime_in_shell())
			ra_notify_draw();

		// Draw Menu
		if (menu_open && !g_hide_menu) {
			vita2d_texture_set_program(overlay_shader->vertexProgram, overlay_shader->fragmentProgram);
			vita2d_texture_set_wvp(overlay_shader->wvpParam);
			vita2d_texture_set_vertexInput(&overlay_shader->vertexInput);
			vita2d_texture_set_fragmentInput(&overlay_shader->fragmentInput);

			drawMenu();
		}

		// f.lux filter drawing
		if (config.flux_mode != 0) {
			uint8_t flux_idx = (config.flux_mode * 4) - 1;

			// Updating our rectangle alpha value depending on daytime
			SceDateTime time;
			sceRtcGetCurrentClockLocalTime(&time);
			if (time.hour < 6)       // Night/Early Morning
				flux_colors[flux_idx] = 0.25f;
			else if (time.hour < 10) // Morning/Early Day
				flux_colors[flux_idx] = 0.1f;
			else if (time.hour < 15) // Mid day
				flux_colors[flux_idx] = 0.05f;
			else if (time.hour < 19) // Late day
				flux_colors[flux_idx] = 0.15f;
			else                     // Evening/Night
				flux_colors[flux_idx] = 0.2f;

			// Setting vertex and fragment program for f.lux
			sceGxmSetVertexProgram(_vita2d_context, flux_shader->vertexProgram);
			sceGxmSetFragmentProgram(_vita2d_context, flux_shader->fragmentProgram);

			// Setting color uniform
			void *rgba_buffer, *wvp_buffer;
			sceGxmReserveFragmentDefaultUniformBuffer(_vita2d_context, &rgba_buffer);
			sceGxmSetUniformDataF(rgba_buffer, vflux_color_param, 0, 4, &flux_colors[(config.flux_mode - 1) * 4]);

			// Setting wvp uniform
			sceGxmReserveVertexDefaultUniformBuffer(_vita2d_context, &wvp_buffer);
			sceGxmSetUniformDataF(wvp_buffer, vflux_wvp_param, 0, 16, (const float*)mvp);

			// Performing f.lux filter draw
			sceGxmSetVertexStream(_vita2d_context, 0, flux_vertices);
			sceGxmDraw(_vita2d_context, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, flux_indices, 4);
		}

		// Show FPS
		// pgf_draw_textf(0.0f, 0.0f, WHITE, FONT_SIZE, "FPS: %.2f", fps);

		// Calculate FPS
		// cur_micros = sceKernelGetProcessTimeWide();
		// if (cur_micros >= (last_micros + 1000000)) {
		//   delta_micros = cur_micros - last_micros;
		//   last_micros = cur_micros;
		//   fps = (frames / (double)delta_micros) * 1000000.0f;
		//   frames = 0;
		// }

		// End drawing
		vita2d_end_drawing();
		vita2d_swap_buffers();

	// Update FPS frames
	// frames++;

		/* 00.10.137: the picture after a LiveArea return.
		 *
		 * The operator's report: back from the LiveArea, one glitched frame, then a
		 * black screen - with music, inputs and our own session banner all alive -
		 * until PS is pressed twice, which is pspemu going through the compat
		 * framebuffer call again. Nothing re-asked pspemu where to draw after the
		 * jump, so it kept painting its old surface while this loop composited a
		 * texture that received nothing. Re-point it once, exactly like the exit
		 * path does, and say so. */
		if (ra_runtime_consume_display_reassert()) {
			sceGxmDisplayQueueFinish();
			SetPspemuFrameBuffer((void *)pops_data);
			sceDisplayWaitVblankStart();
			ra_menu_log_sync("display: pspemu framebuffer re-asserted after the "
			                 "LiveArea return (pops_data=%p)\n",
			                 (void *)pops_data);
		}

		// Sync
		if ((!adrenaline->pops_mode && !ra_runtime_title_is_pops() && !draw_native) || adrenaline->draw_psp_screen_in_pops) {
			sceCompatLCDCSync();
		} else {
			sceDisplayWaitVblankStart();
		}


		// Ctrl
		if (menu_open) {
			ctrlMenu();
		}
	}

	return sceKernelExitDeleteThread(0);
}

int ScePspemuCustomSettingsHandler(int a1, int a2, int a3, int a4) {
	if (a2 == 3) {
		/* 00.10.129: this runs on pspemu's thread. Opening the menu here meant the
		 * menu-open path (pspemu conversions, pops pause, disc re-check, rcheevos)
		 * ran in parallel with the frame loop - which is what the torn log line
		 * `pad: PS winmenu: open: converting pspemu a…` in
		 * RA_SETUP_20261029_214634_541.log was showing. Leave a request; the frame
		 * thread opens it on its next pass. */
		g_menu_from_settings_hook = 1;
		g_menu_open_request = 1;
	} else if (a2 == 1) {
		sceKernelWaitSema(settings_semaid, 1, NULL);

		if (!open_official_settings) {
			ScePspemuSetDisplayConfig();
			return 0;
		}
	}

	return ScePspemuSettingsHandler(a1, a2, a3, a4);
}
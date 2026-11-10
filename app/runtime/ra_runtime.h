#ifndef RA_RUNTIME_H
#define RA_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <psp2/types.h>

#include "../adrenaline_vita.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Achievement snapshot capacity; it bounds the arena and truncates beyond this. */
#define RA_RUNTIME_MAX_ACHIEVEMENTS 1536
#define RA_RUNTIME_MAX_SUBSETS 8
#define RA_RUNTIME_TITLE_MAX 96
#define RA_RUNTIME_DESCRIPTION_MAX 160
#define RA_RUNTIME_STATUS_MAX 128

typedef struct {
    uint32_t id;
    uint32_t points;
    uint8_t unlocked;
    /* Non-zero when the unlock was earned in hardcore. rcheevos reports the
     * mode as a bitmask, so both-earned stays a hardcore unlock. */
    uint8_t unlocked_hardcore;
    uint8_t bucket;
    /* RC_CLIENT_ACHIEVEMENT_TYPE_*: the menu marks missable ([M]) and progression
     * ([P]) rows and prints nothing for the standard type. */
    uint8_t type;
    /* Non-zero when the achievement is unofficial (RC_CLIENT_ACHIEVEMENT_CATEGORY_
     * UNOFFICIAL). Independent of `type`, so a row can read `[MU]`. */
    uint8_t unofficial;
    /* 0 for the main set, otherwise the subset's game id. */
    uint32_t subset_id;
    char title[RA_RUNTIME_TITLE_MAX];
    char description[RA_RUNTIME_DESCRIPTION_MAX];
    char progress[24];
} RaAchievementView;

typedef struct {
    uint32_t id;
    uint32_t achievement_count;
    /* RA_SERVER_SET_TYPE_ *of the set; the menu prints it next to the title. */
    uint8_t type;
    char title[RA_RUNTIME_TITLE_MAX];
} RaSubsetView;

/* The suggestion rcheevos returns for "what should I do next": the first
 * achievement of the locked bucket, i.e. exactly what the achievement page lists
 * under Locked, so the hint can never point at something the page does not hold. */
typedef struct {
    uint32_t id;
    uint32_t points;
    /* RC_CLIENT_ACHIEVEMENT_TYPE_ *and the unofficial category, so a suggestion row
     * can carry the same `[M] [P] [W] [U]` marker the achievement list uses. */
    uint8_t type;
    uint8_t unofficial;
    char title[RA_RUNTIME_TITLE_MAX];
    char description[RA_RUNTIME_DESCRIPTION_MAX];
} RaNextAchievementView;

/* (Per-set summary view removed in : the sets picker shows plain names and the
 * achievement pages own the counts.) */

#define RA_RUNTIME_MAX_LEADERBOARDS 64

typedef struct {
    uint32_t id;
    uint8_t state;
    uint8_t format;
    uint8_t lower_is_better;
    char title[RA_RUNTIME_TITLE_MAX];
    char description[RA_RUNTIME_DESCRIPTION_MAX];
    char tracker[24];
} RaLeaderboardView;

typedef struct {
    int running;
    int ready;
    int unsupported;
    int hardcore;
    int unsupported_platform;
    int has_base_set;
    int notifications;
    int spectator;
    int encore;
    uint32_t game_id;
    uint32_t achievement_count;
    uint32_t unlocked_count;
    uint32_t points_total;
    uint32_t points_unlocked;
    char game_title[RA_RUNTIME_TITLE_MAX];
    char rich_presence[RA_RUNTIME_STATUS_MAX];
    char status[RA_RUNTIME_STATUS_MAX];
} RaRuntimeInfo;

#define RA_RUNTIME_MAX_LEADERBOARD_ENTRIES 25
/* How many board rows are shown before the signed-in user's own row: 24 top entries plus
 * one row for the user when they are outside that range. */
#define RA_RUNTIME_LEADERBOARD_TOP 24
#define RA_RUNTIME_ENTRY_USER_MAX 48
#define RA_RUNTIME_ENTRY_VALUE_MAX 24

typedef struct {
    uint32_t rank;
    char user[RA_RUNTIME_ENTRY_USER_MAX];
    char value[RA_RUNTIME_ENTRY_VALUE_MAX];
    /* Submission date as `YYYY-MM-DD`; empty when the server sent no timestamp. */
    char submitted[12];
    /* 1 when this row is the signed-in user's own entry. */
    int is_user;
} RaLeaderboardEntryView;

enum {
    RA_RUNTIME_LEADERBOARD_ENTRIES_IDLE = 0,
    RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING,
    RA_RUNTIME_LEADERBOARD_ENTRIES_READY,
    RA_RUNTIME_LEADERBOARD_ENTRIES_ERROR
};

void ra_runtime_init(void);
/* Releases the power-service callback. Called on every exit path before pspemu
 * can unload the module; leaving it registered freezes the console on the next
 * app launch (). Idempotent. */
void ra_runtime_release_power_hook(void);
/* pspemu's own settings UI closed (where `Switch Discs` happens): re-reads the live PS1
 * disc serial and, when it is a different container than the session's, hashes that disc
 * and asks rcheevos to change media. */
void ra_runtime_official_settings_closed(void);
/* Session counter, 1 for the first session in this process. */
int ra_runtime_session_ordinal(void);
void ra_runtime_boot(void);
void ra_runtime_tick(const SceAdrenaline *adrenaline, int paused);
void ra_runtime_settings_changed(void);
/* 1 while the emulated world is paused by this module. Informational now: the menu no
 * longer reads it to decide anything - it claims and drops a pause through the two calls
 * below, so the two owners cannot overlap. */
int ra_runtime_holds_world(void);

/* The menu's world pause claim. `pentazemin`'s pauseWorld() is single-shot (a second
 * PAUSE loses the threads the first one suspended), so the module keeps one claim per
 * owner and sends exactly one PAUSE on the first claim and one RESUME when the last one
 * is dropped. Call pause when the menu opens and resume when it closes; both are
 * idempotent. */
void ra_runtime_pause_for_menu(void);
void ra_runtime_resume_for_menu(void);
int ra_runtime_request_start(void);
/* Stop the session on PS double press; tick restarts it when the game returns. */
void ra_runtime_background_stop(void);
/* 1 once after a LiveArea return; the frame loop re-points the framebuffer. */
int ra_runtime_consume_display_reassert(void);
/* Force the Plan C eject/re-insert transition from the debug harness. */
int ra_runtime_debug_media_toggle(void);

/* 1 while a game owns the screen and its RAM is readable; the overlay draws only then. */
int ra_runtime_overlay_visible(void);

/* 1 when the running title is a PS1 (POPS) game, latched for the life of the
 * title. `adrenaline->pops_mode` drops back to 0 mid-game, so display decisions
 * must use this instead: menu.c keeps the direct sharp-scale path off for PS1
 * titles, otherwise it takes the framebuffer over and repaints the Vita system
 * overlay (PS button) away. */
int ra_runtime_title_is_pops(void);
/* POPS state latched at game start, still valid after the XMB transition. */
int ra_runtime_last_title_was_pops(void);
void ra_runtime_request_stop(void);
/* Arms the short event mute used while the PS-button/shell transition owns the
 * screen. Called from the menu when the PS window opens. */
void ra_runtime_shell_transition(void);
/* Clears the mute as soon as the menu/shell transition is over. */
void ra_runtime_shell_transition_clear(void);

int ra_runtime_copy_info(RaRuntimeInfo *info);
/* Bumped every time the achievement snapshot changes, so the menu can cache the
 * rows it builds instead of re-copying and re-fitting all of them each frame. */
unsigned int ra_runtime_data_generation(void);
int ra_runtime_copy_achievements(int offset, RaAchievementView *out, int max,
                                 int *total);
int ra_runtime_copy_achievement_at(int unlocked, int index,
                                   RaAchievementView *out);
void ra_runtime_achievement_counts(int *unlocked, int *locked);

/* Achievements whose trigger is primed right now ("active challenges"), in the
 * order they were primed. Driven by the CHALLENGE_INDICATOR_SHOW/HIDE events, so
 * it is independent of whether the indicator row is enabled: the RA tab can list
 * what is live even with the tiles switched off. */
#define RA_RUNTIME_MAX_ACTIVE_CHALLENGES 16

typedef struct {
    uint32_t id;
    uint32_t points;
    /* RC_CLIENT_ACHIEVEMENT_TYPE_*: the list uses the same marker/prefix as the
     * achievement rows. */
    uint8_t type;
    /* Non-zero for an unofficial achievement; the marker gains a `U`. */
    uint8_t unofficial;
    char title[RA_RUNTIME_TITLE_MAX];
    char description[RA_RUNTIME_DESCRIPTION_MAX];
    /* Measured progress ("7/20") when the achievement has one, else empty. */
    char progress[24];
} RaActiveChallengeView;

int ra_runtime_active_challenge_count(void);
int ra_runtime_copy_active_challenges(RaActiveChallengeView *out, int max,
                                      int *total);

/* Subsets (achievement sets) for the running game, excluding the main set.
 * Returns 0 when the game has none, in which case the menu keeps its single
 * list layout. */
int ra_runtime_subset_count(void);
int ra_runtime_copy_subset_at(int index, RaSubsetView *out);

/* Next locked achievement(s) for the loaded game. The runtime refresh captures
 * the suggestions on the thread that owns rc_client, so this call only copies
 * pure data. Returns the number of entries written (0 = nothing to suggest, or
 * no session). */
#define RA_RUNTIME_NEXT_MAX 3
int ra_runtime_next_achievements(RaNextAchievementView *out, int max);

int ra_runtime_leaderboard_count(void);
int ra_runtime_copy_leaderboard_at(int index, RaLeaderboardView *out);

/* Fetches the top 25 entries for one leaderboard on the runtime thread. The
 * menu calls request once, then polls the state while the page is open. */
void ra_runtime_request_leaderboard_entries(uint32_t leaderboard_id);
int ra_runtime_leaderboard_entries_state(void);
int ra_runtime_copy_leaderboard_entries_error(char *out, size_t capacity);
int ra_runtime_copy_leaderboard_entry_at(int index, RaLeaderboardEntryView *out);

/* Number of known cheat plugins detected in seplugins at session start. */
int ra_runtime_detected_plugins(void);

/* 1 when the last session load failed or the payload took longer than the slow-network
 * threshold while the link was up: the RA tab appends `- slow network` and the runtime
 * raised the "Slow connection" banner (the retry ladder keeps trying in the background). */
int ra_runtime_network_slow(void);
int ra_runtime_offline_at_launch(void);

/* 1 while the runtime's own tick sees the pspemu shell ("XMB"). The menu must use this
 * instead of inspecting its own `adrenaline` copy: after a quit that copy still holds the
 * game title, so a menu-side `title == "XMB"` test never fires. */
int ra_runtime_in_shell(void);

/* Called when the user explicitly leaves pspemu for the LiveArea. The normal
 * runtime tick keeps a bounded grace window for the title -> II-style transitions;
 * an explicit exit must not wait that long or leave a stale local session. */
void ra_runtime_leave_game(void);

/* Called when the host app returns to its own LiveArea (double PS button).
 * Adrenaline only relaunches itself there and does not terminate pspemu, so the
 * game can still exist when the user comes back: pause instead of ending the
 * session. If pspemu really did go away, the identity clears and the normal
 * grace stops the session. */
void ra_runtime_abort_load(void);
void ra_runtime_logout(void);
int ra_runtime_can_pause(uint32_t *frames_remaining);
void ra_runtime_background(void);

/* A save state was just loaded. rcheevos' measured values belong to the old
 * timeline, so the runtime is reset exactly like PCSX2 does on state load. */
void ra_runtime_state_loaded(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_RUNTIME_H */

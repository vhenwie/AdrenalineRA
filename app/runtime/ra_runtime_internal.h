#ifndef RA_RUNTIME_INTERNAL_H
#define RA_RUNTIME_INTERNAL_H

/* Single internal header for the runtime translation units: the former
 * per-module runtime headers, consolidated. ra_runtime.h stays the public
 * facade. */

#include <stdint.h>
#include <psp2/types.h>

#include "ra_badge_key.h"
#include "ra_hash.h"
#include "ra_server.h"
#include "ra_runtime.h"
#include "ra_session.h"
#include "ra_settings.h"

#include "rc_client.h"

/* Memory the runtime maps and reads. */
#define RA_RUNTIME_RAM_BASE 0x88000000u
#define RA_RUNTIME_RAM_SIZE (64u * 1024u * 1024u)
/* PS1 main RAM: ePSP 0x09800000, i.e. 0x01800000 into the pspemu block. */
#define RA_POPS_RAM_OFFSET 0x01800000u
#define RA_POPS_RAM_SIZE 0x00200000u
/* PSP address X is at g_rt.ram + (X - RA_PSP_WINDOW_BASE). */
#define RA_PSP_WINDOW_BASE 0x08000000u
#define RA_POPS_SCRATCH_SIZE 0x400u
#define RA_POPS_SCRATCH_PSP 0x00013000u
#define RA_PSP_DISC_SERIAL_ADDRESS 0x09E80400u

#define RA_RUNTIME_STACK 0x30000
#define RA_RUNTIME_PRIORITY 0x10000100

/* Timing gates. */
#define RA_RUNTIME_REFRESH_DEFER_US 5000000ULL
#define RA_RUNTIME_START_FRAMES 30
#define RA_RUNTIME_RETRY_US 3000000ULL
#define RA_RUNTIME_NO_GAME_GRACE_US 10000000ULL
#define RA_RUNTIME_TRANSITION_HOLD_US 120000000ULL
#define RA_RUNTIME_BOOT_AUTH_US 20000000ULL
#define RA_RUNTIME_TIME_JUMP_US 3000000ULL
#define RA_RUNTIME_STATE_HOLD_US 250000ULL
#define RA_RUNTIME_HOLD_BUDGET_MS 2500u
#define RA_RUNTIME_AUTO_RESUME_DELAY_US (3000000ULL)
#define RA_RUNTIME_AUTO_RESUME_FRAMES 30
#define RA_RUNTIME_PS1_GATE_WAIT_MS 20000u
#define RA_RUNTIME_POPS_SUBMIT_GRACE_FRAMES 300u
#define RA_RUNTIME_SLOW_NETWORK_MS 8000u
#define RA_RUNTIME_LOAD_ATTEMPTS 3
#define RA_OFFLINE_BANNER_DELAY_US (5ULL * 1000ULL * 1000ULL)
#define RA_LINK_CHECK_US (3ULL * 1000ULL * 1000ULL)
#define RA_BADGE_WARM_PREFETCH 1
#define RA_RUNTIME_WARNING_ACHIEVEMENT_ID 101000001u

#define RA_RUNTIME_BUILD_TAG "1.0.0-rewrite"
#define RA_ENABLE_POPS 1
/* RC_ACHIEVEMENT_SET_TYPE_CORE (rc_api_runtime.h); never a selectable subset. */
#define RA_SUBSET_TYPE_CORE 0

/* Plan C media state: whether the disc is readable at all. */
typedef enum {
  RA_MEDIA_UNKNOWN = 0,
  RA_MEDIA_PRESENT,
  RA_MEDIA_EJECTED
} RaMediaState;

/* Cross-module runtime state.
 *
 * This module owns it; every other runtime module reads and writes the fields it
 * needs. State used by a single module stays static in that module. */
typedef struct {
  /* Worker thread and session lifetime. */
  SceUID start_sema;
  SceUID thread;
  volatile int stop;
  volatile int running;
  volatile int paused;
  volatile int suspended;
  volatile int start_pending;
  volatile int attempted;
  volatile int reuse_session;
  volatile int boot_auth_requested;
  volatile int boot_auth_done;
  volatile int power_hook_requested;
  SceUID power_callback;

  ra_session_t *session;
  char session_hash[40];
  int session_ordinal;
  int session_disc_index;
  int disabled_reload_done;

  /* Shell transition and host-visible identity. */
  volatile unsigned long long shell_transition_until_us;
  volatile unsigned int state_fingerprint;
  volatile int in_game;
  volatile int in_shell;
  int last_title_is_pops;
  int platform_downgraded;

  /* Game identity of the running title. */
  char game_titleid[16];
  char game_filename[RA_HASH_PATH_MAX];
  char game_title[RA_RUNTIME_TITLE_MAX];
  char game_badge_key[RA_BADGE_KEY_MAX];
  int game_is_pops;
  int title_is_pops;
  int booted_with_hardcore;
  int progress_logged;

  /* Achievement/leaderboard snapshot. */
  RaRuntimeInfo info;
  RaAchievementView achievements[RA_RUNTIME_MAX_ACHIEVEMENTS];
  int achievement_count;
  volatile unsigned int data_generation;
  RaSubsetView subsets[RA_RUNTIME_MAX_SUBSETS];
  int subset_count;
  RaLeaderboardView leaderboards[RA_RUNTIME_MAX_LEADERBOARDS];
  int leaderboard_count;
  /* Next-locked suggestions captured during refresh, so the menu never calls
   * into rc_client from the frame thread. */
  RaNextAchievementView next_achievements[RA_RUNTIME_NEXT_MAX];
  int next_achievement_count;
  volatile int game_unsupported;
  volatile int dirty;

  RaSettings settings;

  /* Evaluation hold: one world PAUSE per owner, released when the last drops. */
  volatile int world_paused;
  volatile int pause_load;
  volatile int pause_menu;
  volatile unsigned long long hold_started_us;
  SceUID hold_watchdog_thread;

  /* Frame loop timing and counters. */
  unsigned long long frames;
  unsigned long long frames_since_ready;
  unsigned long long last_tick_us;
  unsigned long long started_us;
  unsigned long long no_game_since_us;
  int no_game_stopped;
  unsigned long long last_start_check_us;
  unsigned long long last_presence_check_us;
  volatile int display_reassert;
  volatile int auto_resume_pending;
  int auto_resume_frames;
  unsigned long long auto_resume_after_us;

  /* Refresh and evaluation cost accounting. */
  unsigned long long last_refresh_us;
  unsigned long long refresh_sum_us;
  unsigned long long refresh_max_us;
  unsigned int refresh_count;
  unsigned long long eval_sum_us;
  unsigned long long eval_max_us;
  unsigned int eval_count;

  /* Session load. */
  unsigned long long load_started_us;
  unsigned long long load_parse_us;
  unsigned int load_payload_ms;
  volatile int load_failures;
  unsigned long long retry_at_us;
  int retry_deferred_logged;
  rc_client_achievement_list_t *warm_list;
  uint32_t warm_bucket;
  uint32_t warm_index;
  unsigned warm_requested;
  unsigned long long warm_next_us;

  /* RAM mapping and PS1 (POPS) memory gate. */
  uint8_t *ram;
  uint8_t *ps1_scratch;
  int ps1_loaded;
  int ps1_entry_tried;
  unsigned int memory_refusals;
  unsigned int memory_zero_fills;

  /* Leaderboard scoreboard suppression. */
  uint32_t rejected_lb_id;
  unsigned long long rejected_lb_until;

  /* Network classification. */
  int network_slow;
  int net_failed;
  int link_was_up;
  int offline_at_launch;
  unsigned long long last_link_check_us;
  int detected_plugins;

  /* Media monitoring and multi-disc switching. */
  char last_live_serial[16];
  RaMediaState media_state;
  int media_failures;
  int forced_disc_index;
  char forced_disc_path[RA_HASH_PATH_MAX];
  volatile int restart_disc_pending;
  char disc_tried[RA_HASH_PATH_MAX];
  char disc_hash[40];
  int disc_retry_disc;
  char disc_retry_hash[33];
  unsigned long long disc_retry_at_us;
  volatile int disc_banner_pending;
  volatile int disc_after_settings;
  int disc_watch_reported;
  const void *disc_watch_session;
  volatile int dev_media_forced_blank;
} RaRuntimeContext;

extern RaRuntimeContext g_rt;
/* Replaces the public info snapshot under the runtime lock. */
void ra_runtime_info_set(const RaRuntimeInfo *info);

/* Delay before each load retry. */
extern const unsigned long long g_retry_delays_us[RA_RUNTIME_LOAD_ATTEMPTS];

int ra_runtime_eval_suspended(void);
void ra_runtime_set_eval_hold(int hold);
void ra_runtime_hold_init(void);
ra_session_t *ra_runtime_session_acquire(void);
/* Retains the session, locks its client, and returns the client. The caller must
 * call ra_runtime_client_release() with the same session. */
ra_client_t *ra_runtime_client_acquire(ra_session_t **out_session);
void ra_runtime_client_release(ra_session_t *session);
/* Fills the next-achievement snapshot from a locked client during refresh. */
void ra_runtime_capture_next_achievements(rc_client_t *client);
int ra_runtime_game_memory_valid(void);
void ra_runtime_set_game_memory_valid(int valid);
int ra_runtime_disc_switch_hold(void);
unsigned long long ra_runtime_eval_hold_until(void);
void ra_runtime_set_eval_hold_until(unsigned long long until);

void ra_runtime_log(const char *format, ...);

/* Drains the offline submission queue. */
void ra_runtime_flush_netqueue(void);

/* Reports a session failure through the public info snapshot. */
void ra_runtime_fail_status(const char *status);

/* Worker-thread resume after standby or a shell return. */
void ra_runtime_resume(const char *source);

/* Fetches the session card badge art for the running game. */
void ra_runtime_prefetch_game_badge(void);

/* Session prepare and load phases, run before the frame loop. Both return 0 when
 * the session must not start. */
int ra_runtime_session_prepare(ra_hash_result_t *hash);
int ra_runtime_session_load(const ra_hash_result_t *hash);

/* Identity and lifetime helpers shared by the load and run phases. */
const char *ra_runtime_choose_vault_path(void);
void ra_runtime_push_session_start(void);
void ra_runtime_destroy_session(void);

/* Worker thread entry and frame loop (run phase). */
int ra_runtime_thread(SceSize args, void *argp);
void ra_runtime_run(void);
void ra_runtime_install_power_hook(void);

void ra_runtime_offline_arm(void);
void ra_runtime_reconnect_reset(void);
void ra_runtime_reconnect_note(int synced);
void ra_runtime_offline_cancel(void);
int ra_runtime_offline_due(void);
int ra_runtime_offline_take_shown(void);
void ra_runtime_offline_reset(void);

void ra_runtime_update_rich_presence(void);
void ra_runtime_set_rich_presence(const char *text);

int ra_runtime_set_is_base(uint32_t set_id, RaServerSetInfo *out);

/* 1 when a retained session is logged in, game-loaded and ready. */
int ra_runtime_session_ready(void);

void ra_runtime_status(int result, const char *error_message, void *userdata);

unsigned ra_runtime_count_disabled_achievements(ra_session_t *session);

typedef struct {
  uint32_t request_id;
  volatile int request_pending;
  int state;
  unsigned long long state_since_ms;
  char error[RA_RUNTIME_STATUS_MAX];
  RaLeaderboardEntryView entries[RA_RUNTIME_MAX_LEADERBOARD_ENTRIES];
  int entry_count;
  volatile int need_user;
} RaRuntimeLbState;

RaRuntimeLbState *ra_runtime_lb_state(void);

void ra_runtime_lb_log_failure(const char *phase, uint32_t id, int result,
                               const char *error_message);
const char *ra_runtime_lb_self_name(rc_client_t *client);
void ra_runtime_lb_store_entry(RaLeaderboardEntryView *destination,
                               const rc_client_leaderboard_entry_t *source,
                               unsigned int fallback_rank, const char *self);

void ra_runtime_load_settings(void);
int ra_runtime_settings_dirty(void);
void ra_runtime_mark_settings_dirty(void);
void ra_runtime_clear_settings_dirty(void);
void ra_runtime_apply_client_settings(void);

#endif /* RA_RUNTIME_INTERNAL_H */

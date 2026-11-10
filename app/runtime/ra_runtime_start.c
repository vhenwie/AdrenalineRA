#include "ra_runtime_internal.h"

#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/openpsid.h>
#include <psp2/power.h>

#include "main.h"
#include "ra_arena.h"
#include "ra_badge.h"
#include "ra_client.h"
#include "ra_credentials.h"
#include "ra_hash.h"
#include "ra_http.h"
#include "ra_log.h"
#include "ra_netqueue.h"
#include "ra_notify.h"
#include "ra_offline.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_cards.h"
#include "ra_runtime_clock.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_tracker.h"
#include "ra_runtime_core.h"
#include "ra_runtime_diag.h"
#include "ra_runtime_disc.h"
#include "ra_runtime_events.h"
#include "ra_runtime_hold.h"
#include "ra_runtime_lb_service.h"
#include "ra_runtime_lifecycle.h"
#include "ra_runtime_load.h"
#include "ra_runtime_memory.h"
#include "ra_runtime_path.h"
#include "ra_runtime_platform.h"
#include "ra_runtime_plugins.h"
#include "ra_server.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "ra_shot.h"


void ra_runtime_prefetch_game_badge(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc;
  const rc_client_game_t *game;
  char url[RA_BADGE_URL_MAX];
  char key[RA_BADGE_KEY_MAX];

  g_rt.game_badge_key[0] = '\0';
  if (client == NULL) {
    ra_runtime_log("runtime: game badge skipped (no session)\n");
    return;
  }
  rc = ra_client_get_rc_client(client);
  game = rc != NULL ? rc_client_get_game_info(rc) : NULL;
  if (game == NULL) {
    ra_runtime_log("runtime: game badge skipped (no game info)\n");
    ra_runtime_client_release(session);
    return;
  }
  if (rc_client_game_get_image_url(game, url, sizeof(url)) != RC_OK) {
    ra_runtime_log("runtime: game badge skipped (no image url)\n");
    ra_runtime_client_release(session);
    return;
  }
  if (!ra_badge_make_key(game->badge_name, url, key, sizeof(key))) {
    ra_runtime_log("runtime: game badge skipped (bad key name='%s')\n",
          game->badge_name != NULL ? game->badge_name : "");
    ra_runtime_client_release(session);
    return;
  }
  ra_badge_request(key, url, 0);
  ra_runtime_copy_text(g_rt.game_badge_key, sizeof(g_rt.game_badge_key), key);
  ra_runtime_log("runtime: game badge key=%s\n", key);
  ra_runtime_client_release(session);
}

/* Audit trail of awards the server did not confirm. Never an input: the site is
 * authoritative. Format: <achievement id> <h|s>. */
/* Award reporter: logs each outcome; the site stays authoritative for earned state. */

/* Announces the session once, with the disc in the title when it is already known. */

/* The disc is knowable when the session becomes ready, so the card is built once,
 * immediately. A later swap announces itself because its disc differs. */

/* How many containers this EBOOT has (0 when it cannot be read). */

static int session_create_client(void) {
  SceKernelOpenPsId psid;
  ra_session_config_t config;
  const char *vault_path;

  if (g_rt.session == NULL) {
    if (sceKernelGetOpenPsId(&psid) < 0) {
      ra_runtime_fail_status("Console ID unavailable");
      return 0;
    }
    vault_path = ra_runtime_choose_vault_path();

    memset(&config, 0, sizeof(config));
    config.device_id = (const uint8_t*)psid.id;
    config.vault_path = vault_path;
    config.game_hash = NULL;
    config.read_memory = ra_runtime_memory_read;
    config.event_handler = ra_runtime_events_dispatch;
    config.transport = ra_http_post;
    config.status_callback = ra_runtime_status;
    config.userdata = NULL;
    config.log_callback = ra_runtime_client_log;

    ra_netqueue_init();
    config.server_call = ra_netqueue_server_call;
    config.log_level = RC_CLIENT_LOG_LEVEL_INFO;

    g_rt.session = ra_session_create(&config);
    if (g_rt.session == NULL) {
      ra_netqueue_stop();
      ra_runtime_fail_status("Saved account could not be opened");
      return 0;
    }
    ra_client_set_pump(ra_session_get_client(g_rt.session), ra_netqueue_pump);
  }
  return 1;
}

static int session_open(void) {
  int result;

  ra_http_set_session_active(1);
  result = ra_http_init();
  if (result != 0) {
    ra_runtime_fail_status("Network stack unavailable");
    return 0;
  }
  ra_server_set_transport(ra_http_post);
  ra_server_set_timing_reporter(ra_runtime_request_timing);
  ra_server_set_award_reporter(ra_runtime_award_reported);

  if (!session_create_client())
    return 0;

  ra_runtime_apply_client_settings();
  ra_runtime_lb_reset();

  ra_runtime_lock();
  g_rt.info.running = 1;
  g_rt.info.ready = 0;
  g_rt.info.unsupported = 0;
  g_rt.info.unsupported_platform = 0;
  g_rt.info.hardcore = 0;
  g_rt.platform_downgraded = 0;
  ra_runtime_copy_text(g_rt.info.game_title, sizeof(g_rt.info.game_title), g_rt.game_title);
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status),
      ra_session_is_logged_in(g_rt.session) ? "Loading game..." : "Signing in...");
  ra_runtime_unlock();
  return 1;
}
static int session_login(void) {
  int result;

  if (!ra_session_is_logged_in(g_rt.session)) {
    result = ra_session_start(g_rt.session);
    if (result != RC_OK) {
      ra_runtime_fail_status("RetroAchievements session could not start");
      ra_runtime_destroy_session();
  return 0;
    }
  }
  return 1;
}
static int session_begin_load(const ra_hash_result_t *hash) {
  int result;

  /* A failed parse looks the same as a failed allocation: record the heap headroom. */
  ra_http_probe_heap();
  ra_runtime_log("runtime: heap probe=%u KiB free before load\n",
        ra_http_get_heap_probe_kib());

  
  ra_server_reset_trim_bytes();
  ra_server_reset_sets_counters();
  ra_server_set_payload_set(RA_SERVER_MERGE_SETS);

  /* Do not start the parse while the memory callbacks cannot answer. POPS is waited for
 * before the hold, since a held world freezes the PS1. */
  if (!ra_runtime_memory_readable()) {
    ra_runtime_log("runtime: memory still unreadable before parse (ps1=%d ram=%d); "
          "rcheevos may disable achievements with an invalid address\n",
          g_rt.ps1_loaded, g_rt.ram != NULL);
  }

  g_rt.disabled_reload_done = 0;

  result = ra_session_load_game(g_rt.session, hash->hash);
  if (result != RC_OK) {
    ra_server_set_parse_hooks(NULL, NULL);
    ra_runtime_fail_status("RetroAchievements game load could not start");
    ra_runtime_destroy_session();
  return 0;
  }
  return 1;
}
static void session_wait_ready(void) {
  int i;

  
  /* Wait for the load itself, not a stopwatch: a progressing load must not be cut off.
 * 30 s is a safety net for a dead network; an unknown hash stops at once. */
  for (i = 0; i < 3000 && !ra_session_is_ready(g_rt.session) &&
        !g_rt.game_unsupported && !g_rt.stop; ++i) {
    /* Enforce the hold budget here too: this thread is the one that would otherwise
         * sit in a 33 s parse with the game paused. */
    ra_runtime_hold_budget_tick();
    ra_session_do_frame(g_rt.session);
    sceKernelDelayThread(10 * 1000);
  }
  if (!ra_session_is_ready(g_rt.session) && !g_rt.game_unsupported && !g_rt.stop) {
    ra_runtime_log("runtime: load not ready after %u ms (net=%d held=%d); "
          "giving up on this attempt\n",
          (unsigned)(i * 10), ra_netqueue_depth(), g_rt.world_paused);
  }
  ra_server_set_parse_hooks(NULL, NULL);

  ra_runtime_log("runtime: sets trim removed=%u bytes\n",
        (unsigned)ra_server_last_trim_bytes());
  ra_runtime_log("runtime: sets mode=%s\n",
        ra_server_get_payload_set() == RA_SERVER_MERGE_SETS
          ? "merged"
          : (ra_server_get_payload_set() == RA_SERVER_NO_TRIM
            ? "all-sets" : "core-set-only"));
  if (ra_server_last_merge_delta() != 0) {
    ra_runtime_log("runtime: sets merged bonus rows into the core set (%+ld bytes)\n",
          (long)ra_server_last_merge_delta());
  }
  /* The picker lists non-core sets from the merged-set index, which only exists
     * when the payload carried two or more sets (ra_server_merge_sets() returns
     * NULL for one). `payload` is how many set objects the body held, `reason`
     * why the walk stopped (see ra_server.h), `bytes` how big the body was. */
  ra_runtime_log("runtime: sets bytes=%u payload=%d reason=%d\n",
        (unsigned)ra_server_last_sets_bytes(),
        ra_server_last_payload_sets(),
        ra_server_last_merge_reason());
}
static int session_unsupported_check(void) {
  if (g_rt.game_unsupported) {
    ra_runtime_lock();
    g_rt.info.running = 0;
    g_rt.info.ready = 0;
    g_rt.info.unsupported = 1;
    ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status),
        "RetroAchievements does not support this game");
    ra_runtime_unlock();
    ra_runtime_log("runtime: unsupported game titleid='%s'; automatic retry stopped\n",
          g_rt.game_titleid);
    ra_runtime_release_world();
    ra_notify_clear();
    ra_runtime_active_challenges_reset();
    ra_runtime_destroy_session();
  return 0;
  }
  return 1;
}
static void session_retry_one_set(const ra_hash_result_t *hash) {
  int result;
  int i;

  if (!ra_session_is_ready(g_rt.session) && !g_rt.stop &&
    ra_server_get_payload_set() == RA_SERVER_NO_TRIM) {
    /* The full two-set payload needs ~2.8 MB. If it did not load, retry with
         * one set so a memory-bound parse still leaves a playable game instead of
         * no achievements at all. */
    ra_runtime_log("runtime: full payload did not load; retrying with one set\n");
    ra_server_reset_trim_bytes();
    ra_server_reset_sets_counters();
    ra_server_set_payload_set(RA_SERVER_KEEP_FIRST_SET);
    ra_runtime_flush_netqueue();
    ra_session_unload_game(g_rt.session);
    /* The retry parses a payload too: hold for that parse as well, and clear
         * the hooks only once the parse has actually happened (in the pump). */
    g_rt.load_started_us = (unsigned long long)sceKernelGetSystemTimeWide();
    ra_server_set_parse_hooks(ra_runtime_parse_hold, ra_runtime_parse_release);
    result = ra_session_load_game(g_rt.session, hash->hash);
    if (result == RC_OK) {
      /* Same rule as the first attempt: wait for the load, not 2 s. */
      for (i = 0; i < 3000 && !ra_session_is_ready(g_rt.session) &&
            !g_rt.game_unsupported && !g_rt.stop; ++i) {
        ra_runtime_hold_budget_tick();
        ra_session_do_frame(g_rt.session);
        sceKernelDelayThread(10 * 1000);
      }
    }
    ra_server_set_parse_hooks(NULL, NULL);
    ra_runtime_log("runtime: sets trim removed=%u bytes (fallback)\n",
          (unsigned)ra_server_last_trim_bytes());
  }
}
static void session_classify_failure(void) {
  if (!ra_session_is_ready(g_rt.session)) {
    /* Classify the failure instead of a flat "unavailable": with the link up this is a
         * slow connection, not an offline console. The retry ladder below keeps trying
         * (5 s / 20 s / 45 s), so the text says so and a banner explains it once. */
    int server_status = ra_http_get_last_status();

    if (server_status >= 500) {
      /* The console is online and RA answered - with a 5xx. That is their outage,
             * not our network, and the player must be told the difference: on
             * ` 19:01` every request came back
             * `{"message":"Service Unavailable"}` / 503 and the old text blamed the
             * connection. The retry ladder still applies. */
      g_rt.network_slow = 1;
      ra_runtime_log("runtime: server unavailable (http=%d); session not ready, "
            "retrying\n", server_status);
      ra_runtime_fail_status("RetroAchievements server unavailable (retrying)");
      /* No card during the load : the status row and the retry ladder
             * already say it, and a card raised here queues *ahead of *the session banner.
             * A multi-disc PBP that takes a few seconds to hash produced a burst of three
             * of these before the session card; connectivity cards only make sense once a
             * session exists. */
    } else if (ra_http_link_up()) {
      g_rt.network_slow = 1;
      /* Transport stage codes: 10 connection, 11 request, 12 send/handshake. */
      ra_runtime_log("runtime: transport error (stage=%d err=0x%08X); session not "
            "ready, retrying\n", ra_http_get_last_stage(),
            (unsigned)ra_http_get_last_error());
      ra_runtime_fail_status("Connection error: achievements unavailable (retrying)");
      /* No card here either  - see the note above. */
    } else {
      ra_runtime_fail_status("RetroAchievements login or game load failed");
    }
    ra_server_set_parse_hooks(NULL, NULL);
    ra_runtime_destroy_session();
  return;
  }
}
static void session_recovered_notice(void) {
  /* The session came up after at least one failed attempt: say so once. */
  if (g_rt.load_failures > 0 && g_rt.network_slow) {
    g_rt.network_slow = 0;
    ra_notify_push(RA_NOTIFY_SESSION, 0, "RetroAchievements reconnected",
          "The session is live.");
    ra_runtime_log("runtime: slow network recovered after %d retr%s\n",
          (int)g_rt.load_failures, g_rt.load_failures == 1 ? "y" : "ies");
  }
}
static void session_publish_basics(void) {
  /* History first: not applied any more. The site is authoritative - the player
     * resets progress on the web between sessions on purpose, and an achievement the
     * site calls unearned must fire again. Badge cache lives under the RA game id:
     * badges/<game_id>/<badge>.png */
  ra_runtime_refresh();
  ra_badge_set_game(g_rt.info.game_id);
  ra_runtime_prefetch_game_badge();
  ra_runtime_prefetch_likely_badge();
}
typedef struct {
  unsigned total;
  unsigned active;
  unsigned inactive;
  unsigned tracking;
  unsigned disabled;
  int hardcore;
} SessionLbCounts;

static void session_count_leaderboards(SessionLbCounts *counts) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc = client != NULL ? ra_client_get_rc_client(client) : NULL;
  rc_client_leaderboard_list_t *lb_list =
    rc != NULL ? rc_client_create_leaderboard_list(
            rc, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE)
        : NULL;

    unsigned leaderboard_total = 0;
    unsigned lb_active = 0;
    unsigned lb_inactive = 0;
    unsigned lb_tracking = 0;
    unsigned lb_disabled = 0;

    if (lb_list != NULL) {
      uint32_t bucket_index;

      for (bucket_index = 0; bucket_index < lb_list->num_buckets;
        ++bucket_index) {
        const rc_client_leaderboard_bucket_t *bucket =
          &lb_list->buckets[bucket_index];
        uint32_t lb_index;

        leaderboard_total += (unsigned)bucket->num_leaderboards;
        for (lb_index = 0; lb_index < bucket->num_leaderboards;
          ++lb_index) {
          const rc_client_leaderboard_t *board =
            bucket->leaderboards[lb_index];

          if (board == NULL)
            continue;
          /* INACTIVE = not allowed to run; DISABLED = memref failed validation. */
          switch (board->state) {
          case RC_CLIENT_LEADERBOARD_STATE_ACTIVE:
            ++lb_active;
            break;
          case RC_CLIENT_LEADERBOARD_STATE_TRACKING:
            ++lb_tracking;
            break;
          case RC_CLIENT_LEADERBOARD_STATE_DISABLED:
            ++lb_disabled;
            break;
          default:
            ++lb_inactive;
            break;
          }
        }
      }
      rc_client_destroy_leaderboard_list(lb_list);
    }
  counts->total = leaderboard_total;
  counts->active = lb_active;
  counts->inactive = lb_inactive;
  counts->tracking = lb_tracking;
  counts->disabled = lb_disabled;
  counts->hardcore = rc != NULL ? rc_client_get_hardcore_enabled(rc) != 0 : 0;
  if (client != NULL)
    ra_runtime_client_release(session);
}
static void session_publish_leaderboards(void) {
  SessionLbCounts counts;
  unsigned leaderboard_total;

  memset(&counts, 0, sizeof(counts));
  session_count_leaderboards(&counts);
  leaderboard_total = counts.total;
  (void)leaderboard_total;
  ra_runtime_log("runtime: leaderboards loaded=%u active=%u inactive=%u "
        "tracking=%u disabled=%u hardcore=%d\n",
        counts.total, counts.active, counts.inactive, counts.tracking,
        counts.disabled, counts.hardcore);
  if (g_rt.achievement_count == 0 && counts.total == 0 && counts.hardcore) {
    ra_session_t *session;
    ra_client_t *client = ra_runtime_client_acquire(&session);
    rc_client_t *rc = client != NULL ? ra_client_get_rc_client(client) : NULL;

    if (rc != NULL)
      rc_client_set_hardcore_enabled(rc, 0);
    if (client != NULL)
      ra_runtime_client_release(session);
    g_rt.info.hardcore = 0;
    ra_runtime_log("runtime: no achievements or leaderboards; hardcore "
          "disabled for this game\n");
  }
}
static void session_ready_log(void) {

  ra_runtime_log("runtime: session ready achievements=%d hardcore=%d elapsed_ms=%llu\n",
        g_rt.achievement_count, g_rt.info.hardcore != 0,
        (unsigned long long)((sceKernelGetSystemTimeWide() - g_rt.started_us) /
                  1000ULL));
}
static void session_offline_replay(void) {
   {
    int pending = ra_offline_queue_depth();

    if (pending > 0) {
      ra_runtime_log("runtime: %d pending submission(s) at session start; "
            "replaying\n", pending);
      ra_netqueue_replay_offline();
    }
  }
}
static void session_pool_log(void) {
  /* Start the POPS submission grace window here: the frames before it are load frames. */
  g_rt.frames_since_ready = 0;
   {
    ra_session_t *session;
    ra_client_t *client = ra_runtime_client_acquire(&session);
    rc_client_t *rc = client != NULL ? ra_client_get_rc_client(client) : NULL;
    uint32_t memrefs = 0;
    uint32_t modified = 0;

    if (rc != NULL)
      rc_client_get_game_memref_counts(rc, &memrefs, &modified);
    if (client != NULL)
      ra_runtime_client_release(session);
    ra_runtime_log("runtime: pool memrefs=%u modified=%u\n", (unsigned)memrefs,
          (unsigned)modified);
  }
}
static void session_arena_log(void) {
  ra_runtime_log("runtime: arena used=%u KiB peak=%u KiB of %u KiB largest-free=%u KiB "
        "rover-repairs=%lu\n",
        (unsigned)(ra_arena_used() / 1024u),
        (unsigned)(ra_arena_peak() / 1024u),
        (unsigned)(ra_arena_capacity() / 1024u),
        (unsigned)(ra_arena_largest_free() / 1024u),
        (unsigned long)ra_arena_stat_rover_repairs());
}
static void session_announce(void) {
  /* Nothing is paused past this line: the session is live, so resume before the
     * announce card. Everything the player needs is already loaded or cached. */
  ra_runtime_release_world();
  ra_notify_clear();
  ra_runtime_active_challenges_reset();
  ra_runtime_tracker_reset();
  if (ra_server_platform_warning())
    g_rt.platform_downgraded = ra_runtime_downgrade_platform();
  ra_runtime_push_session_start();
  if (g_rt.platform_downgraded)
    ra_runtime_push_up_banner();
  ra_runtime_warm_begin();
}

int ra_runtime_session_load(const ra_hash_result_t *hash) {
  if (!session_open())
    return 0;
  if (!session_login())
    return 0;
  if (!session_begin_load(hash))
    return 0;

  session_wait_ready();
  if (!session_unsupported_check())
    return 0;
  session_retry_one_set(hash);
  if (!ra_session_is_ready(g_rt.session)) {
    session_classify_failure();
    return 0;
  }

  session_recovered_notice();
  ra_runtime_reload_for_disabled_achievements();
  session_publish_basics();
  session_publish_leaderboards();
  session_ready_log();
  session_offline_replay();
  session_pool_log();
  session_arena_log();
  session_announce();
  return 1;
}

const char *ra_runtime_choose_vault_path(void) {
  SceIoStat stat;

  if (sceIoGetstat(RA_CREDENTIALS_UR0, &stat) >= 0)
    return RA_CREDENTIALS_UR0;
  return RA_CREDENTIALS_UX0;
}

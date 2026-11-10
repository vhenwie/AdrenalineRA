#include "ra_runtime_load.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "ra_badge.h"
#include "ra_client.h"
#include "ra_notify.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_core.h"
#include "ra_runtime_hold.h"
#include "ra_runtime_memory.h"
#include "ra_server.h"
#include "rc_api_runtime.h"
#include "rc_client.h"
#include "ra_session.h"

static RaSubsetView refresh_subset_buf[RA_RUNTIME_MAX_SUBSETS];

static int refresh_subset_is_base(rc_client_t *rc, const rc_client_game_t *game,
                                  const rc_client_achievement_bucket_t *bucket) {
  const rc_client_subset_t *owner =
    bucket->subset_id != 0
      ? rc_client_get_subset_info(rc, bucket->subset_id)
      : NULL;

  return (owner == NULL) ||
    (game != NULL &&
      (owner->id == game->id ||
      (owner->title != NULL &&
      game->title != NULL &&
      strcmp(owner->title, game->title) == 0)));
}

static uint32_t refresh_subset_id(rc_client_t *rc, const rc_client_game_t *game,
                                  const rc_client_achievement_bucket_t *bucket,
                                  const rc_client_achievement_t *ach) {
  uint32_t origin;

  origin = ra_server_achievement_set(ach->id);

  /* With the sets merged, or with a single indexed set, the origin comes from
   * the index ra_server_merge_sets() built. A core set is the base. */
  if (origin != 0) {
    RaServerSetInfo set;

    if (ra_runtime_set_is_base(origin, &set))
      return 0;
    return origin;
  }
  return refresh_subset_is_base(rc, game, bucket) ? 0 : bucket->subset_id;
}

static void refresh_fill_achievement(rc_client_t *rc,
                                     const rc_client_game_t *game,
                                     const rc_client_achievement_bucket_t *bucket,
                                     const rc_client_achievement_t *ach,
                                     RaAchievementView *view) {
  memset(view, 0, sizeof(*view));
  view->id = ach->id;
  view->points = ach->points;
  view->unlocked = ach->unlocked != 0;
  view->unlocked_hardcore =
    (ach->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) != 0;
  view->bucket = ach->bucket;
  view->type = (uint8_t)ach->type;
  /* Category is a bitmask independent of the type. */
  view->unofficial = (ach->category &
            RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL) ? 1 : 0;
  view->subset_id = refresh_subset_id(rc, game, bucket, ach);
  ra_runtime_copy_text(view->title, sizeof(view->title), ach->title);
  ra_runtime_copy_text(view->description, sizeof(view->description),
      ach->description);
  ra_runtime_copy_text(view->progress, sizeof(view->progress),
      ach->measured_progress);
}

static void refresh_achievements(rc_client_t *rc, const rc_client_game_t *game,
                                 RaAchievementView *achievements, int *result) {
  rc_client_achievement_list_t *list = rc_client_create_achievement_list(
    rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  unsigned int b;
  unsigned int a;
  int count = 0;

  if (list != NULL) {
    for (b = 0; b < list->num_buckets && count < RA_RUNTIME_MAX_ACHIEVEMENTS;
      ++b) {
      const rc_client_achievement_bucket_t *bucket = &list->buckets[b];
      for (a = 0;
        a < bucket->num_achievements && count < RA_RUNTIME_MAX_ACHIEVEMENTS;
        ++a) {
        const rc_client_achievement_t *ach = bucket->achievements[a];

        if (ach == NULL || ach->id >= RA_RUNTIME_WARNING_ACHIEVEMENT_ID)
          continue;
        refresh_fill_achievement(rc, game, bucket, ach, &achievements[count++]);
      }
    }
    rc_client_destroy_achievement_list(list);
  }
  *result = count;
}

static void refresh_truncation_log(int count, uint32_t total) {
  static int truncation_logged;

  if (count >= RA_RUNTIME_MAX_ACHIEVEMENTS && total > (uint32_t)count &&
    !truncation_logged) {
    truncation_logged = 1;
    ra_runtime_log("runtime: achievement list truncated at %d of %u rows "
          "(RA_RUNTIME_MAX_ACHIEVEMENTS=%d)\n",
          count, (unsigned)total, (int)RA_RUNTIME_MAX_ACHIEVEMENTS);
  }
}

static void refresh_subsets_from_index(int *subset_count) {
  int indexed = ra_server_set_count();
  int s;

  for (s = 0; s < indexed && *subset_count < RA_RUNTIME_MAX_SUBSETS; ++s) {
    RaServerSetInfo set;

    if (!ra_server_set_at(s, &set))
      continue;
    ra_runtime_log("runtime: set id=%u type=%s ach=%u '%s'\n",
          (unsigned)set.id, ra_server_set_type_name(set.type),
          (unsigned)set.achievements, set.title);
    if (set.type == RA_SERVER_SET_TYPE_CORE)
      continue;
    if (set.achievements == 0)
      continue;
    memset(&refresh_subset_buf[*subset_count], 0,
      sizeof(refresh_subset_buf[*subset_count]));
    refresh_subset_buf[*subset_count].id = set.id;
    refresh_subset_buf[*subset_count].achievement_count = set.achievements;
    refresh_subset_buf[*subset_count].type = set.type;
    ra_runtime_copy_text(refresh_subset_buf[*subset_count].title,
        sizeof(refresh_subset_buf[*subset_count].title), set.title);
    ++*subset_count;
  }
}

static void refresh_subsets_from_client(rc_client_t *rc,
                                        const rc_client_game_t *game,
                                        int *subset_count) {
  rc_client_subset_list_t *subsets = rc_client_create_subset_list(rc);

  if (subsets != NULL) {
    unsigned int s;

    for (s = 0; s < subsets->num_subsets &&
          *subset_count < RA_RUNTIME_MAX_SUBSETS; ++s) {
      const rc_client_subset_t *subset = subsets->subsets[s];
      int is_core;

      if (subset == NULL)
        continue;
      is_core = (game != NULL) &&
          (subset->id == game->id ||
          (subset->title != NULL && game->title != NULL &&
            strcmp(subset->title, game->title) == 0));
      ra_runtime_log("runtime: subset id=%u game=%u ach=%u core=%d '%s'\n",
            (unsigned)subset->id, game != NULL ? (unsigned)game->id : 0u,
            (unsigned)subset->num_achievements, is_core,
            subset->title != NULL ? subset->title : "");
      if (is_core)
        continue;
      if (subset->num_achievements == 0)
        continue;
      memset(&refresh_subset_buf[*subset_count], 0,
        sizeof(refresh_subset_buf[*subset_count]));
      refresh_subset_buf[*subset_count].id = subset->id;
      refresh_subset_buf[*subset_count].achievement_count =
        subset->num_achievements;
      /* rc_client_subset_t carries no type; BONUS keeps the row selectable. */
      refresh_subset_buf[*subset_count].type = RA_SERVER_SET_TYPE_BONUS;
      ra_runtime_copy_text(refresh_subset_buf[*subset_count].title,
          sizeof(refresh_subset_buf[*subset_count].title), subset->title);
      ++*subset_count;
    }
    rc_client_destroy_subset_list(subsets);
  }
}

static void refresh_subsets(rc_client_t *rc, const rc_client_game_t *game) {
  static int group_note_logged;
  int subset_count = 0;

  if (ra_server_set_count() > 0)
    refresh_subsets_from_index(&subset_count);
  else
    refresh_subsets_from_client(rc, game, &subset_count);

  if (!group_note_logged) {
    group_note_logged = 1;
    ra_runtime_log("runtime: achievement groups absent from RA set "
          "payload; group headers deferred\n");
  }

  ra_runtime_lock();
  g_rt.subset_count = subset_count;
  memcpy(g_rt.subsets, refresh_subset_buf,
    sizeof(refresh_subset_buf[0]) * (size_t)subset_count);
  ra_runtime_unlock();
}

static void refresh_leaderboards(rc_client_t *rc) {
  static RaLeaderboardView refreshed[RA_RUNTIME_MAX_LEADERBOARDS];
  rc_client_leaderboard_list_t *leaderboards =
    rc_client_create_leaderboard_list(
      rc, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  int leaderboard_count = 0;
  unsigned int b;
  unsigned int a;

  if (leaderboards != NULL) {
    for (b = 0; b < leaderboards->num_buckets &&
        leaderboard_count < RA_RUNTIME_MAX_LEADERBOARDS; ++b) {
      const rc_client_leaderboard_bucket_t *bucket =
        &leaderboards->buckets[b];
      for (a = 0; a < bucket->num_leaderboards &&
          leaderboard_count < RA_RUNTIME_MAX_LEADERBOARDS; ++a) {
        const rc_client_leaderboard_t *lb = bucket->leaderboards[a];
        RaLeaderboardView *view;

        if (lb == NULL)
          continue;
        view = &refreshed[leaderboard_count++];
        memset(view, 0, sizeof(*view));
        view->id = lb->id;
        view->state = lb->state;
        view->format = lb->format;
        view->lower_is_better = lb->lower_is_better;
        ra_runtime_copy_text(view->title, sizeof(view->title), lb->title);
        ra_runtime_copy_text(view->description, sizeof(view->description),
            lb->description);
        ra_runtime_copy_text(view->tracker, sizeof(view->tracker),
            lb->tracker_value);
      }
    }
    rc_client_destroy_leaderboard_list(leaderboards);
  }

  ra_runtime_lock();
  g_rt.leaderboard_count = leaderboard_count;
  memcpy(g_rt.leaderboards, refreshed,
    sizeof(refreshed[0]) * (size_t)leaderboard_count);
  ra_runtime_unlock();
}

static int refresh_has_base_set(void) {
  int count = ra_server_set_count();
  int i;

  for (i = 0; i < count; ++i) {
    RaServerSetInfo set;

    if (ra_server_set_at(i, &set) && set.type == RA_SERVER_SET_TYPE_CORE)
      return 1;
  }
  return 0;
}

static void refresh_publish_info(rc_client_t *rc, const rc_client_game_t *game,
                                 const rc_client_user_game_summary_t *summary,
                                 int count, RaAchievementView *achievements) {
  ra_runtime_lock();
  ra_runtime_copy_text(g_rt.info.game_title, sizeof(g_rt.info.game_title),
      game != NULL && game->title != NULL ? game->title : g_rt.game_title);
  g_rt.info.game_id = game != NULL ? game->id : 0;
  g_rt.info.achievement_count = summary->num_core_achievements;
  g_rt.info.unlocked_count = summary->num_unlocked_achievements;
  g_rt.info.points_total = summary->points_core;
  g_rt.info.points_unlocked = summary->points_unlocked;
  if (!g_rt.progress_logged && summary->num_core_achievements > 0) {
    g_rt.progress_logged = 1;
    ra_runtime_log("runtime: session progress unlocked=%u/%u points=%u/%u\n",
          (unsigned)summary->num_unlocked_achievements,
          (unsigned)summary->num_core_achievements,
          (unsigned)summary->points_unlocked,
          (unsigned)summary->points_core);
  }
  g_rt.info.hardcore = rc_client_get_hardcore_enabled(rc) != 0;
  g_rt.info.has_base_set = refresh_has_base_set();
  g_rt.info.notifications = g_rt.settings.notifications != 0;
  g_rt.info.spectator = rc_client_get_spectator_mode_enabled(rc) != 0;
  g_rt.info.encore = rc_client_get_encore_mode_enabled(rc) != 0;
  g_rt.achievement_count = count;
  memcpy(g_rt.achievements, achievements, sizeof(achievements[0]) * (size_t)count);
  ++g_rt.data_generation;
  ra_runtime_unlock();
  g_rt.dirty = 0;
}

static void refresh_cost(unsigned long long started_us, int count) {
  unsigned long long refresh_us =
    (unsigned long long)sceKernelGetSystemTimeWide() - started_us;

  g_rt.refresh_sum_us += refresh_us;
  ++g_rt.refresh_count;
  if (refresh_us > g_rt.refresh_max_us)
    g_rt.refresh_max_us = refresh_us;
  if (refresh_us >= 8000ULL)
    ra_runtime_log("runtime: refresh took %llu us (achievements=%d)\n",
          refresh_us, count);
}

void ra_runtime_refresh(void) {
  unsigned long long refresh_started_us =
    (unsigned long long)sceKernelGetSystemTimeWide();
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc;
  const rc_client_game_t *game;
  rc_client_user_game_summary_t summary;
  /* Static, not automatic: the runtime thread's stack cannot hold this. */
  static RaAchievementView achievements[RA_RUNTIME_MAX_ACHIEVEMENTS];
  int count = 0;
  if (client == NULL)
    return;
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_client_release(session);
    return;
  }
  memset(&summary, 0, sizeof(summary));
  rc_client_get_user_game_summary(rc, &summary);
  game = rc_client_get_game_info(rc);
  if (game != NULL && game->title != NULL)
    ra_runtime_copy_text(g_rt.game_title, sizeof(g_rt.game_title), game->title);

  refresh_achievements(rc, game, achievements, &count);
  refresh_truncation_log(count, summary.num_core_achievements);
  refresh_subsets(rc, game);
  refresh_leaderboards(rc);
  ra_runtime_capture_next_achievements(rc);
  refresh_publish_info(rc, game, &summary, count, achievements);
  refresh_cost(refresh_started_us, count);
  ra_runtime_client_release(session);
}

/* Load window: the world is paused from identification until the session is live, with
 * a hard budget so a slow parse cannot freeze the console. menu.c suppresses its own
 * PAUSE_WORLD while ra_runtime_holds_world() is true. */

void ra_runtime_parse_hold(void) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

  g_rt.load_parse_us = now;
  g_rt.load_payload_ms = (unsigned int)((now - g_rt.load_started_us) / 1000ULL);
  ra_runtime_log("runtime: payload ready at %llu ms\n",
        (unsigned long long)((now - g_rt.load_started_us) / 1000ULL));

  /* Release the world before the parse: the parse is one blocking call the hold
   * budget cannot interrupt, so only the payload wait is protected. */
  ra_runtime_log("runtime: releasing game for the parse (hold=%llu ms)\n",
        (unsigned long long)((now - g_rt.load_started_us) / 1000ULL));
  ra_runtime_release_world();
}

void ra_runtime_parse_release(void) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

  ra_runtime_log("runtime: parse done at %llu ms (parse=%llu ms)\n",
        (unsigned long long)((now - g_rt.load_started_us) / 1000ULL),
        (unsigned long long)((g_rt.load_parse_us != 0
                    ? now - g_rt.load_parse_us
                    : 0) / 1000ULL));
}

int ra_runtime_reload_for_disabled_achievements(void) {
  ra_session_t *session = ra_runtime_session_acquire();
  unsigned disabled;
  unsigned now_disabled;
  int ready;
  unsigned long long before_us;
  int result;

  if (g_rt.disabled_reload_done || session == NULL ||
      g_rt.session_hash[0] == '\0') {
    ra_session_release(session);
    return 0;
  }
  disabled = ra_runtime_count_disabled_achievements(session);
  if (disabled == 0) {
    ra_session_release(session);
    return 0;
  }
  if (!ra_runtime_memory_readable()) {
    /* Nothing can be repaired while reads are refused: a reload now would disable
         * the same addresses again. The pre-hold gate wait exists so this cannot
         * happen on a PS1 title; log it if it does. */
    ra_runtime_log("runtime: %u achievements disabled and reads unavailable; reload "
          "skipped (%s)\n", disabled, g_rt.game_is_pops ? "PS1" : "PSP");
    ra_session_release(session);
    return 0;
  }

  g_rt.disabled_reload_done = 1;

  before_us = (unsigned long long)sceKernelGetSystemTimeWide();
  ra_runtime_log("runtime: %u achievements disabled at load; reloading now that reads "
        "work (%s)\n", disabled, g_rt.game_is_pops ? "PS1" : "PSP");
  ra_runtime_flush_netqueue();
  ra_session_unload_game(session);
  g_rt.load_started_us = (unsigned long long)sceKernelGetSystemTimeWide();
  ra_server_set_parse_hooks(ra_runtime_parse_hold, ra_runtime_parse_release);
  result = ra_session_load_game(session, g_rt.session_hash);
  if (result == RC_OK) {
    int i;

    for (i = 0; i < 3000 && !ra_session_is_ready(session) &&
          !g_rt.game_unsupported && !g_rt.stop; ++i) {
      ra_runtime_hold_budget_tick();
      ra_session_do_frame(session);
      sceKernelDelayThread(10 * 1000);
    }
  }
  ra_server_set_parse_hooks(NULL, NULL);
  ready = ra_session_is_ready(session);
  now_disabled = ra_runtime_count_disabled_achievements(session);
  ra_runtime_log("runtime: reload done in %u ms (ready=%d disabled now=%u)\n",
        (unsigned)((sceKernelGetSystemTimeWide() - before_us) / 1000ULL),
        ready, now_disabled);
  ra_session_release(session);
  if (ready)
    ra_runtime_refresh();
  return 1;
}

void ra_runtime_warm_begin(void) {
  ra_session_t *session;
  ra_client_t *client;
  rc_client_t *rc;

  if (RA_BADGE_WARM_PREFETCH == 0 || !g_rt.settings.badge_prefetch) {
    ra_runtime_log("runtime: badge warm cache disabled (setting=%d); art is fetched "
          "on demand\n", (int)g_rt.settings.badge_prefetch);
    return;
  }
  client = ra_runtime_client_acquire(&session);
  if (client == NULL) {
    ra_runtime_log("runtime: badge warm cache skipped (no session)\n");
    return;
  }
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_client_release(session);
    return;
  }

  /* Never leak the previous list (session reuse keeps the client alive). */
  if (g_rt.warm_list != NULL) {
    rc_client_destroy_achievement_list(g_rt.warm_list);
    g_rt.warm_list = NULL;
  }

  g_rt.warm_list = rc_client_create_achievement_list(
    rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  g_rt.warm_bucket = 0;
  g_rt.warm_index = 0;
  g_rt.warm_requested = 0;
  /* Start later than the old 3 s: the parse, the boot hash and FFVII's first
     * field all compete for the card and CPU in that window. */
  g_rt.warm_next_us = (unsigned long long)sceKernelGetSystemTimeWide() + 8000000ULL;
  ra_runtime_log("runtime: badge warm cache start buckets=%u (deferred 8000 ms)\n",
        g_rt.warm_list != NULL ? (unsigned)g_rt.warm_list->num_buckets : 0u);
  ra_runtime_client_release(session);
}

/* Only art that can plausibly appear next: active challenges and achievements the
 * player is close to unlocking. Walking the whole core set warmed hundreds of
 * textures the player would never see, and every one cost network and CPU. With
 * prefetch off, the unlock path fetches on demand exactly like the reference
 * clients. */
static int runtime_warm_bucket_wanted(uint8_t bucket_type) {
  return bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE ||
    bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE;
}

/* Returns 1 when the walk was freed because the setting turned prefetch off. */
static int warm_tick_setting_off(void) {
  if (g_rt.settings.badge_prefetch)
    return 0;
  ra_runtime_log("runtime: badge warm cache stopped by setting after %u request(s)\n",
        g_rt.warm_requested);
  rc_client_destroy_achievement_list(g_rt.warm_list);
  g_rt.warm_list = NULL;
  return 1;
}

/* Returns 1 when a visible card or a held world says "wait, then continue". */
static int warm_tick_defer(void) {
  unsigned long long now;

  if (ra_notify_visible() || ra_runtime_holds_world()) {
    now = sceKernelGetSystemTimeWide();
    g_rt.warm_next_us = now + 1000000ULL;
    return 1;
  }
  return 0;
}

/* Returns 1 when the badge subsystem is going away and the walk was freed. */
static int warm_tick_halted(void) {
  if (ra_badge_stopped() || !ra_badge_enabled() || ra_badge_stalled()) {
    ra_runtime_log("runtime: badge warm cache halted after %u request(s)\n",
          g_rt.warm_requested);
    rc_client_destroy_achievement_list(g_rt.warm_list);
    g_rt.warm_list = NULL;
    return 1;
  }
  return 0;
}

/* Queues at most one art request; returns 1 when one was queued, 0 when the
 * walk reached the end of the list. */
static int warm_tick_walk(void) {
  while (g_rt.warm_bucket < g_rt.warm_list->num_buckets) {
    const rc_client_achievement_bucket_t *bucket =
      &g_rt.warm_list->buckets[g_rt.warm_bucket];
    const rc_client_achievement_t *achievement;
    char url[RA_BADGE_URL_MAX];
    char key[RA_BADGE_KEY_MAX];

    if (!runtime_warm_bucket_wanted(bucket->bucket_type)) {
      ++g_rt.warm_bucket;
      g_rt.warm_index = 0;
      continue;
    }
    if (g_rt.warm_index >= bucket->num_achievements) {
      ++g_rt.warm_bucket;
      g_rt.warm_index = 0;
      continue;
    }

    achievement = bucket->achievements[g_rt.warm_index++];
    if (achievement == NULL)
      continue;
    /* Coloured art only . The lock phase doubled the download burst and
         * the FFVII log showed it plainly: during 61 `_lock` requests the in-game
         * loop average rose to 34.5 ms, and it returned to 16.68 ms once
         * `badge warm cache halted`. Locked art is needed only by the few active
         * challenge tiles, so those fetch it on demand. */
    if (rc_client_achievement_get_image_url(achievement,
                        RC_IMAGE_TYPE_ACHIEVEMENT,
                        url, sizeof(url)) != RC_OK)
      continue;
    if (!ra_badge_make_key(achievement->badge_name, url, key, sizeof(key)))
      continue;
    if (ra_badge_cached(key))
      continue;

    ra_badge_request(key, url, 0);
    ++g_rt.warm_requested;
    if ((g_rt.warm_requested % 40u) == 0u) {
      ra_runtime_log("runtime: badge warm cache progress requested=%u\n",
            g_rt.warm_requested);
    }
    return 1;
  }
  return 0;
}

static void warm_tick_locked(void) {
  unsigned long long now;

  if (g_rt.warm_list == NULL)
    return;

  /* The setting can change while a session runs (Developer Features), and a teardown
     * must not keep queueing art. Both free the walk and stop. */
  if (warm_tick_setting_off())
    return;

  /* Only while the player is actually playing . A card or indicator on
     * screen means the overlay is drawing and a texture is about to be needed - warming
     * art then would compete with the very card it is meant to help. A held world means
     * a load or the menu owns the machine. Either way: wait, then continue. */
  if (warm_tick_defer())
    return;

  /* Stop queueing art while the subsystem shuts down. */
  if (warm_tick_halted())
    return;

  now = sceKernelGetSystemTimeWide();
  if (now < g_rt.warm_next_us)
    return;
  /* Pace on the queue, not on a fixed rate: the worker drains far slower than
     * one request per 300 ms, so a fixed rate filled the 256-slot queue and
     * dropped the rest - which is why only some images were cached. */
  if (ra_badge_pending() >= 8)
    return;
  g_rt.warm_next_us = now + 1000000ULL;

  if (warm_tick_walk())
    return;

  ra_runtime_log("runtime: badge warm cache done requested=%u\n", g_rt.warm_requested);
  rc_client_destroy_achievement_list(g_rt.warm_list);
  g_rt.warm_list = NULL;
}

void ra_runtime_warm_tick(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);

  if (client == NULL)
    return;
  warm_tick_locked();
  ra_runtime_client_release(session);
}


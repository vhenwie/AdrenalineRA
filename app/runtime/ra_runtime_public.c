#include "ra_runtime.h"

#include "ra_client.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_core.h"
#include "ra_server.h"

#include <string.h>

#include "rc_client.h"

int ra_runtime_copy_info(RaRuntimeInfo *info) {
  if (info == NULL)
    return 0;
  ra_runtime_lock();
    *info = g_rt.info;
  ra_runtime_unlock();
  return 1;
}

unsigned int ra_runtime_data_generation(void) {
  return g_rt.data_generation;
}

void ra_runtime_achievement_counts(int *unlocked, int *locked) {
  int i;
  int u = 0;
  int l = 0;

  ra_runtime_lock();
  for (i = 0; i < g_rt.achievement_count; ++i) {
    if (g_rt.achievements[i].unlocked)
      ++u;
    else
      ++l;
  }
  ra_runtime_unlock();
  if (unlocked != NULL)
        *unlocked = u;
  if (locked != NULL)
        *locked = l;
}

int ra_runtime_copy_achievement_at(int unlocked, int index,
                RaAchievementView *out) {
  int i;
  int matched = 0;
  int found = 0;

  if (out == NULL || index < 0)
    return 0;
  ra_runtime_lock();
  for (i = 0; i < g_rt.achievement_count; ++i) {
    if ((g_rt.achievements[i].unlocked != 0) == (unlocked != 0)) {
      if (matched == index) {
                *out = g_rt.achievements[i];
        found = 1;
        break;
      }
      ++matched;
    }
  }
  ra_runtime_unlock();
  return found;
}

int ra_runtime_copy_achievements(int offset, RaAchievementView *out, int max,
                int *total) {
  int count;

  if (total != NULL)
        *total = 0;
  if (out == NULL || max <= 0)
    return 0;

  ra_runtime_lock();
  count = g_rt.achievement_count;
  if (total != NULL)
        *total = count;
  if (offset < 0)
    offset = 0;
  if (offset >= count) {
    ra_runtime_unlock();
    return 0;
  }
  if (max > count - offset)
    max = count - offset;
  memcpy(out, &g_rt.achievements[offset],
    sizeof(g_rt.achievements[0]) * (size_t)max);
  ra_runtime_unlock();
  return max;
}

int ra_runtime_subset_count(void) {
  int count;

  ra_runtime_lock();
  count = g_rt.subset_count;
  ra_runtime_unlock();
  return count;
}

int ra_runtime_copy_subset_at(int index, RaSubsetView *out) {
  int found = 0;

  if (out == NULL || index < 0)
    return 0;

  ra_runtime_lock();
  if (index < g_rt.subset_count) {
        *out = g_rt.subsets[index];
    found = 1;
  }
  ra_runtime_unlock();
  return found;
}

void ra_runtime_capture_next_achievements(rc_client_t *client) {
  RaNextAchievementView next[RA_RUNTIME_NEXT_MAX];
  const rc_client_achievement_t *achievement = NULL;
  int found = 0;

  memset(next, 0, sizeof(next));
  /* A NULL anchor starts at the first active set; rcheevos walks its own display
     * order and only reports entries it has already bucketed, so unofficial and
     * unsupported achievements are excluded for free. Each subsequent call passes
     * the previous hit as the anchor, which is the documented way to keep walking
     * the bucket: the API answers one achievement at a time, never a list. */
  while (client != NULL && found < RA_RUNTIME_NEXT_MAX) {
    achievement = rc_client_get_next_achievement_info(
      client, achievement, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED);
    if (achievement == NULL)
      break;
    next[found].id = achievement->id;
    next[found].points = achievement->points;
    next[found].type = achievement->type;
    next[found].unofficial =
      (achievement->category & RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL)
        ? 1 : 0;
    ra_runtime_copy_text(next[found].title, sizeof(next[found].title),
        achievement->title != NULL ? achievement->title : "");
    ra_runtime_copy_text(next[found].description, sizeof(next[found].description),
        achievement->description != NULL ? achievement->description : "");
    ++found;
  }
  ra_runtime_lock();
  memcpy(g_rt.next_achievements, next, sizeof(next));
  g_rt.next_achievement_count = found;
  ra_runtime_unlock();
}

int ra_runtime_next_achievements(RaNextAchievementView *out, int max) {
  if (out == NULL || max <= 0)
    return 0;
  if (max > RA_RUNTIME_NEXT_MAX)
    max = RA_RUNTIME_NEXT_MAX;
  ra_runtime_lock();
  if (max > g_rt.next_achievement_count)
    max = g_rt.next_achievement_count;
  memcpy(out, g_rt.next_achievements, sizeof(out[0]) * (size_t)max);
  ra_runtime_unlock();
  return max;
}

int ra_runtime_leaderboard_count(void) {
  int count;

  ra_runtime_lock();
  count = g_rt.leaderboard_count;
  ra_runtime_unlock();
  return count;
}

int ra_runtime_copy_leaderboard_at(int index, RaLeaderboardView *out) {
  int found = 0;

  if (out == NULL || index < 0)
    return 0;
  ra_runtime_lock();
  if (index < g_rt.leaderboard_count) {
        *out = g_rt.leaderboards[index];
    found = 1;
  }
  ra_runtime_unlock();
  return found;
}

int ra_runtime_detected_plugins(void) {
  return g_rt.detected_plugins;
}

int ra_runtime_network_slow(void) {
  return g_rt.network_slow;
}

int ra_runtime_offline_at_launch(void) {
  return g_rt.offline_at_launch;
}

int ra_runtime_in_shell(void) {
  return g_rt.in_shell;
}

void ra_runtime_request_leaderboard_entries(uint32_t leaderboard_id) {
  ra_runtime_lock();
  ra_runtime_lb_state()->request_id = leaderboard_id;
  ra_runtime_lb_state()->request_pending = leaderboard_id != 0;
  ra_runtime_lb_state()->need_user = 0;
  ra_runtime_lb_state()->state = leaderboard_id != 0 ? RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING
                  : RA_RUNTIME_LEADERBOARD_ENTRIES_IDLE;
  ra_runtime_lb_state()->state_since_ms = ra_runtime_now_ms();
  ra_runtime_lb_state()->error[0] = '\0';
  ra_runtime_lb_state()->entry_count = 0;
  ra_runtime_unlock();
}



int ra_runtime_in_game(void) { return g_rt.in_game; }

int ra_runtime_info_ready(void) { return g_rt.info.ready; }
unsigned long long ra_runtime_load_parse_us(void) { return g_rt.load_parse_us; }

int ra_runtime_net_failed(void) { return g_rt.net_failed; }
void ra_runtime_set_net_failed(int failed) { g_rt.net_failed = failed; }

void ra_runtime_set_rich_presence(const char *text) {
  ra_runtime_lock();
  ra_runtime_copy_text(g_rt.info.rich_presence, sizeof(g_rt.info.rich_presence), text);
  ra_runtime_unlock();
}

int ra_runtime_start_pending(void) { return g_rt.start_pending; }
int ra_runtime_session_ready(void) {
  ra_session_t *session = ra_runtime_session_acquire();
  int ready = session != NULL && ra_session_is_ready(session);

  ra_session_release(session);
  return ready;
}
int ra_runtime_game_is_pops(void) { return g_rt.game_is_pops; }
int ra_runtime_stopped(void) { return g_rt.stop; }
void ra_runtime_hashlib_reset(void);

void ra_runtime_mark_data_changed(void) { ++g_rt.data_generation; }


void ra_runtime_mark_game_unsupported(void) { g_rt.game_unsupported = 1; }
void ra_runtime_set_ready(void) {
  ra_runtime_lock();
  g_rt.info.ready = 1;
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status),
      "RetroAchievements ready");
  ra_runtime_unlock();
}

RaSettings *ra_runtime_settings(void) { return &g_rt.settings; }
const char *ra_runtime_game_title(void) { return g_rt.game_title; }
const char *ra_runtime_game_badge_key(void) { return g_rt.game_badge_key; }


void ra_runtime_mark_dirty(void) { g_rt.dirty = 1; }
int ra_runtime_game_unsupported(void) { return g_rt.game_unsupported; }

void ra_runtime_set_unsupported(void) {
  ra_runtime_lock();
  g_rt.info.hardcore = 0;
  g_rt.info.unsupported_platform = 1;
  ra_runtime_copy_text(g_rt.info.status, sizeof(g_rt.info.status),
      "Unsupported platform (UP) - Casual mode");
  ++g_rt.data_generation;
  ra_runtime_unlock();
  g_rt.dirty = 1;
}

int ra_runtime_set_is_base(uint32_t set_id, RaServerSetInfo *out) {
  int count = ra_server_set_count();
  int i;

  for (i = 0; i < count; ++i) {
    RaServerSetInfo set;

    if (!ra_server_set_at(i, &set) || set.id != set_id)
      continue;
    if (out != NULL)
            *out = set;
    return set.type == RA_SERVER_SET_TYPE_CORE;
  }
  return 0;
}

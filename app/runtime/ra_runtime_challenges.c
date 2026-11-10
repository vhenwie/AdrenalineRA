#include "ra_runtime_challenges.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_core.h"
#include "ra_runtime_internal.h"

#include <string.h>

#include "ra_client.h"
#include "ra_session.h"
#include "ra_runtime.h"
#include "rc_client.h"

static RaActiveChallengeView g_active_challenges[RA_RUNTIME_MAX_ACTIVE_CHALLENGES];
static int g_active_challenge_count;

void ra_runtime_mark_data_changed(void);

void ra_runtime_active_challenge_add(
  const rc_client_achievement_t *achievement) {
  const rc_client_achievement_t *source = achievement;
  int i;

  if (achievement == NULL || achievement->id == 0)
    return;

  /* The event carries a snapshot of the client's achievement; read the client's
     * own record as well so the list cannot show stale points, type or measured
     * progress. */
  {
    ra_session_t *session = ra_runtime_session_acquire();
    if (session != NULL) {
      ra_client_t *client = ra_session_get_client(session);
      rc_client_t *rc;

      ra_client_lock(client);
      rc = ra_client_get_rc_client(client);
      if (rc != NULL) {
        const rc_client_achievement_t *current =
          rc_client_get_achievement_info(rc, achievement->id);

        if (current != NULL)
          source = current;
      }
      ra_client_unlock(client);
      ra_session_release(session);
    }
  }

  ra_runtime_lock();
  for (i = 0; i < g_active_challenge_count; ++i) {
    if (g_active_challenges[i].id == achievement->id)
      break;
  }
  if (i == g_active_challenge_count) {
    if (g_active_challenge_count >= RA_RUNTIME_MAX_ACTIVE_CHALLENGES) {
      ra_runtime_unlock();
      return;
    }
    memset(&g_active_challenges[i], 0, sizeof(g_active_challenges[i]));
    g_active_challenges[i].id = achievement->id;
    ++g_active_challenge_count;
  }
  g_active_challenges[i].points = source->points;
  g_active_challenges[i].type = source->type;
  g_active_challenges[i].unofficial =
    (source->category & RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL) ? 1 : 0;
  ra_runtime_copy_text(g_active_challenges[i].title,
      sizeof(g_active_challenges[i].title), source->title);
  ra_runtime_copy_text(g_active_challenges[i].description,
      sizeof(g_active_challenges[i].description),
      source->description);
  ra_runtime_copy_text(g_active_challenges[i].progress,
      sizeof(g_active_challenges[i].progress),
      source->measured_progress);
  ra_runtime_mark_data_changed();
  ra_runtime_unlock();
}

void ra_runtime_active_challenge_remove(uint32_t id) {
  int i;

  ra_runtime_lock();
  for (i = 0; i < g_active_challenge_count; ++i) {
    if (g_active_challenges[i].id != id)
      continue;
    memmove(&g_active_challenges[i], &g_active_challenges[i + 1],
        sizeof(g_active_challenges[0]) *
          (size_t)(g_active_challenge_count - i - 1));
    --g_active_challenge_count;
    memset(&g_active_challenges[g_active_challenge_count], 0,
      sizeof(g_active_challenges[0]));
    ra_runtime_mark_data_changed();
    break;
  }
  ra_runtime_unlock();
}

/* A primed achievement can still report measured progress; keep the listed copy
 * in step with it so the tab shows the same number as the indicator. */
void ra_runtime_active_challenge_refresh(
  const rc_client_achievement_t *achievement) {
  int i;

  if (achievement == NULL)
    return;

  ra_runtime_lock();
  for (i = 0; i < g_active_challenge_count; ++i) {
    if (g_active_challenges[i].id != achievement->id)
      continue;
    ra_runtime_copy_text(g_active_challenges[i].progress,
        sizeof(g_active_challenges[i].progress),
        achievement->measured_progress);
    ra_runtime_mark_data_changed();
    break;
  }
  ra_runtime_unlock();
}

void ra_runtime_active_challenges_reset(void) {
  ra_runtime_lock();
  if (g_active_challenge_count > 0) {
    memset(g_active_challenges, 0, sizeof(g_active_challenges));
    g_active_challenge_count = 0;
    ra_runtime_mark_data_changed();
  }
  ra_runtime_unlock();
}

int ra_runtime_active_challenge_count(void) {
  int count;

  ra_runtime_lock();
  count = g_active_challenge_count;
  ra_runtime_unlock();
  return count;
}

int ra_runtime_copy_active_challenges(RaActiveChallengeView *out, int max,
                  int *total) {
  int count;

  if (total != NULL)
        *total = 0;
  if (out == NULL || max <= 0)
    return 0;

  ra_runtime_lock();
  count = g_active_challenge_count;
  if (total != NULL)
        *total = count;
  if (max > count)
    max = count;
  if (max > 0)
    memcpy(out, g_active_challenges, sizeof(out[0]) * (size_t)max);
  ra_runtime_unlock();
  return max;
}


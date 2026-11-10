#include "ra_runtime_badges.h"
#include "ra_runtime_internal.h"

#include <string.h>

#include "ra_badge.h"
#include "ra_client.h"
#include "ra_session.h"
#include "ra_runtime.h"
#include "rc_client.h"

int ra_runtime_badge_key(const rc_client_achievement_t *achievement,
              char *key, size_t key_capacity, char *url,
              size_t url_capacity) {
  const char *source;
  char generated[RA_BADGE_URL_MAX];

  if (achievement == NULL || key == NULL || url == NULL ||
    key_capacity == 0 || url_capacity == 0)
    return 0;

  source = achievement->badge_url;
  if (source == NULL || source[0] == '\0') {
    if (rc_client_achievement_get_image_url(
        achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, generated,
        sizeof(generated)) != RC_OK)
      return 0;
    source = generated;
  }
  if (source == NULL || source[0] == '\0')
    return 0;
  if (!ra_badge_make_key(achievement->badge_name, source, key,
            key_capacity))
    return 0;
  if (strlen(source) >= url_capacity)
    return 0;
  memcpy(url, source, strlen(source) + 1);
  return 1;
}

void ra_runtime_prefetch_badge_for(
  const rc_client_achievement_t *achievement, int high_priority) {
  char key[RA_BADGE_KEY_MAX];
  char url[RA_BADGE_URL_MAX];
  ra_session_t *session;

  if (achievement == NULL)
    return;
  session = ra_runtime_session_acquire();
  if (session == NULL)
    return;
  if (ra_runtime_badge_key(achievement, key, sizeof(key), url, sizeof(url))) {
    ra_badge_request(key, url, high_priority);
    ra_session_release(session);
    return;
  }
  ra_runtime_log("runtime: badge key unavailable achiev=%u name=%s url=%s\n",
        (unsigned)achievement->id,
        achievement->badge_name != NULL ? achievement->badge_name : "(null)",
        achievement->badge_url != NULL ? "set" : "(null)");
  ra_session_release(session);
}

/* Lazy DuckStation-style warm-up: one likely next badge, never a bulk fetch of
 * every locked achievement. rcheevos supplies the next-locked hint; when the
 * player has no unlock history yet, use the first locked achievement. */
void ra_runtime_prefetch_likely_badge(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc;
  rc_client_achievement_list_t *list;
  const rc_client_achievement_t *most_recent = NULL;
  const rc_client_achievement_t *first_locked = NULL;
  const rc_client_achievement_t *next;
  unsigned int b;
  unsigned int a;

  if (client == NULL)
    return;
  ra_badge_init();
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_client_release(session);
    return;
  }
  list = rc_client_create_achievement_list(
    rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  if (list == NULL) {
    ra_runtime_client_release(session);
    return;
  }

  for (b = 0; b < list->num_buckets; ++b) {
    const rc_client_achievement_bucket_t *bucket = &list->buckets[b];
    for (a = 0; a < bucket->num_achievements; ++a) {
      const rc_client_achievement_t *achievement =
        bucket->achievements[a];
      if (bucket->bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED ||
        bucket->bucket_type ==
          RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED) {
        if (most_recent == NULL ||
          achievement->unlock_time > most_recent->unlock_time)
          most_recent = achievement;
      } else if (bucket->bucket_type ==
            RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED &&
          first_locked == NULL) {
        first_locked = achievement;
      }
    }
  }
  rc_client_destroy_achievement_list(list);
  if (most_recent != NULL) {
    next = rc_client_get_next_achievement_info(
      rc, most_recent, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED);
    if (next != NULL) {
      ra_runtime_prefetch_badge_for(next, 0);
      ra_runtime_log("runtime: badge prefetch next=%u\n", (unsigned)next->id);
    }
  } else if (first_locked != NULL) {
    ra_runtime_prefetch_badge_for(first_locked, 0);
    ra_runtime_log("runtime: badge prefetch next=%u\n",
          (unsigned)first_locked->id);
  }
  ra_runtime_client_release(session);
}

void ra_runtime_prefetch_next_badge(
  const rc_client_achievement_t *achievement) {
  ra_session_t *session;
  ra_client_t *client;
  rc_client_t *rc;
  const rc_client_achievement_t *next;

  if (achievement == NULL)
    return;
  client = ra_runtime_client_acquire(&session);
  if (client == NULL)
    return;
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_client_release(session);
    return;
  }
  next = rc_client_get_next_achievement_info(
    rc, achievement, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED);
  if (next != NULL)
    ra_runtime_prefetch_badge_for(next, 0);
  ra_runtime_client_release(session);
}


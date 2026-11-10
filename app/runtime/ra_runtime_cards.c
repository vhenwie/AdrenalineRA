#include "ra_runtime_cards.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_hashlib.h"

#include <stdio.h>
#include <string.h>

#include "ra_badge.h"
#include "ra_client.h"
#include "ra_notify.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "rc_client.h"
#include "ra_runtime.h"

RaSettings *ra_runtime_settings(void);
const char *ra_runtime_game_title(void);
const char *ra_runtime_game_badge_key(void);

static int g_announced_disc;

int ra_runtime_announced_disc_get(void) { return g_announced_disc; }
void ra_runtime_announced_disc_set(int disc) { g_announced_disc = disc; }

static int session_card_summary(char *subtitle, size_t capacity, int *hardcore) {
  RaRuntimeInfo info;

  ra_runtime_copy_info(&info);
  if (ra_runtime_settings()->visibility_summary == 2 &&
      info.achievement_count == 0)
    return 0;
  *hardcore = info.hardcore;
  snprintf(subtitle, capacity,
      "Unlocked: %u of %u Achievements | Earned: %u of %u Points",
      (unsigned)info.unlocked_count, (unsigned)info.achievement_count,
      (unsigned)info.points_unlocked, (unsigned)info.points_total);
  return 1;
}

static void session_card_title(char *title, size_t capacity, int disc,
                               int hardcore) {
  if (disc > 0)
    snprintf(title, capacity, "%s - Disc %d%s", ra_runtime_game_title(), disc,
             hardcore ? " (Hardcore Mode)" : "");
  else
    snprintf(title, capacity, "%s%s", ra_runtime_game_title(),
             hardcore ? " (Hardcore Mode)" : "");
}

static void session_mode_card(void) {
  if (ra_runtime_settings()->spectator != 0) {
    ra_runtime_log("runtime: session mode spectator; unlocks are shown, not recorded\n");
    ra_notify_push(RA_NOTIFY_INFO, 0, "Spectator Mode",
                   "Unlocks are shown but not recorded");
  } else if (ra_runtime_settings()->encore != 0) {
    ra_runtime_log("runtime: session mode encore; earned achievements can trigger again\n");
    ra_notify_push(RA_NOTIFY_INFO, 0, "Encore Mode",
                   "Earned achievements can trigger again.");
  }
}

void ra_runtime_push_session_card(int disc) {
  char title[RA_RUNTIME_TITLE_MAX];
  char subtitle[RA_RUNTIME_DESCRIPTION_MAX];
  int hardcore = 0;

  if (!ra_runtime_settings()->notifications ||
      ra_runtime_settings()->visibility_summary == 0)
    return;
  if (!ra_runtime_session_ready()) {
    ra_runtime_log("runtime: session card suppressed; no ready session\n");
    return;
  }
  if (!session_card_summary(subtitle, sizeof(subtitle), &hardcore))
    return;
  session_card_title(title, sizeof(title), disc, hardcore);
  ra_runtime_log("runtime: %s image %s at announce (disc=%d)\n",
      disc > 0 ? "session disc" : "session",
      ra_runtime_game_badge_key()[0] == '\0' ? "n/a"
      : ra_badge_cached(ra_runtime_game_badge_key()) ? "cached" : "pending",
      disc);
  ra_notify_push_achievement(RA_NOTIFY_SESSION, 0, 0, title, subtitle,
      ra_runtime_game_badge_key()[0] != '\0' ? ra_runtime_game_badge_key() : NULL);
  if (disc <= 0)
    session_mode_card();
}
void ra_runtime_push_disc_banner(int disc) {
  ra_session_t *session;

  if (disc <= 0 || !ra_runtime_settings()->notifications)
    return;
  if (disc == g_announced_disc)
    return; /* the banner already names this disc (boot alignment) */
  session = ra_runtime_session_acquire();
  if (ra_runtime_game_title()[0] == '\0' || session == NULL) {
    ra_session_release(session);
    return;
  }
  ra_session_release(session);
  g_announced_disc = disc;
  ra_runtime_log("runtime: disc %d announced on the session card\n", disc);
  /* Same builder as the session card, so the disc card carries the game title, the disc
     * and the Hardcore suffix identically. */
  ra_runtime_push_session_card(disc);
}

void ra_runtime_update_rich_presence(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc;
  char text[RA_RUNTIME_STATUS_MAX];

  if (client == NULL) {
    ra_runtime_set_rich_presence("");
    return;
  }
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_set_rich_presence("");
    ra_runtime_client_release(session);
    return;
  }
  text[0] = '\0';
  rc_client_get_rich_presence_message(rc, text, sizeof(text));
  if (text[0] != '\0')
    ra_runtime_set_rich_presence(text);
  ra_runtime_client_release(session);
}

unsigned ra_runtime_count_disabled_achievements(ra_session_t *session) {
  ra_client_t *client;
  rc_client_t *rc;
  rc_client_achievement_list_t *list;
  unsigned disabled = 0;
  uint32_t b;
  uint32_t i;

  if (session == NULL)
    return 0;
  client = ra_session_get_client(session);
  ra_client_lock(client);
  rc = ra_client_get_rc_client(client);
  if (rc != NULL) {
    list = rc_client_create_achievement_list(rc,
                        RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (list != NULL) {
      for (b = 0; b < list->num_buckets; ++b) {
        const rc_client_achievement_bucket_t *bucket = &list->buckets[b];

        for (i = 0; i < bucket->num_achievements; ++i) {
          const rc_client_achievement_t *achievement = bucket->achievements[i];

          if (achievement != NULL &&
            achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED)
            ++disabled;
        }
      }
      rc_client_destroy_achievement_list(list);
    }
  }
  ra_client_unlock(client);
  return disabled;
}

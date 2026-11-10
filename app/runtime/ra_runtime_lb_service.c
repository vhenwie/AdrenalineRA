#include "ra_runtime_lb_service.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_core.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "ra_client.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "rc_client.h"

unsigned long long ra_runtime_now_ms(void);
void ra_runtime_mark_data_changed(void);

void ra_runtime_lb_reset(void) {
  ra_runtime_lock();
  ra_runtime_lb_state()->request_pending = 0;
  ra_runtime_lb_state()->need_user = 0;
  ra_runtime_lb_state()->request_id = 0;
  ra_runtime_lb_state()->state = RA_RUNTIME_LEADERBOARD_ENTRIES_IDLE;
  ra_runtime_lb_state()->error[0] = '\0';
  ra_runtime_lb_state()->entry_count = 0;
  ra_runtime_unlock();
}
static void lb_top_callback(int result, const char *error_message,
                  rc_client_leaderboard_entry_list_t *list,
                  rc_client_t *client, void *userdata) {
  const char *self = ra_runtime_lb_self_name(client);
  char failure[RA_RUNTIME_STATUS_MAX];
  uint32_t failure_id;
  int failure_result = 0;
  int count = 0;
  int found = 0;
  unsigned int i;

  (void)userdata;
  failure[0] = '\0';
  failure_id = (uint32_t)(uintptr_t)userdata;

  ra_runtime_lock();
  if (result == RC_OK && list != NULL) {
    for (i = 0; i < list->num_entries &&
          count < RA_RUNTIME_LEADERBOARD_TOP; ++i) {
      ra_runtime_lb_store_entry(&ra_runtime_lb_state()->entries[count], &list->entries[i],
                i + 1, self);
      if (ra_runtime_lb_state()->entries[count].is_user)
        found = 1;
      ++count;
    }
    ra_runtime_lb_state()->entry_count = count;
    ra_runtime_lb_state()->error[0] = '\0';
    /* Outside the top rows: fetch only the player's own row, which the menu
         * renders as row 25 with its real rank. */
    ra_runtime_lb_state()->need_user = (!found && self != NULL && self[0] != '\0');
    ra_runtime_lb_state()->state = ra_runtime_lb_state()->need_user ? RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING
                  : RA_RUNTIME_LEADERBOARD_ENTRIES_READY;
    ra_runtime_lb_state()->state_since_ms = ra_runtime_now_ms();
  } else {
    ra_runtime_lb_state()->entry_count = 0;
    ra_runtime_lb_state()->need_user = 0;
    ra_runtime_lb_state()->state = RA_RUNTIME_LEADERBOARD_ENTRIES_ERROR;
    ra_runtime_copy_text(ra_runtime_lb_state()->error, sizeof(ra_runtime_lb_state()->error),
        error_message != NULL ? error_message
                    : "Leaderboard entries could not be loaded.");
    ra_runtime_copy_text(failure, sizeof(failure), ra_runtime_lb_state()->error);
    failure_result = result;
  }
  ra_runtime_unlock();

  if (failure[0] != '\0' || failure_result != 0)
    ra_runtime_lb_log_failure("entries", failure_id, failure_result, failure);

  if (list != NULL)
    rc_client_destroy_leaderboard_entry_list(list);
}

/* Phase 2: the player's own row (`r=lbinfo&u=<user>&c=1`), appended after the top
 * rows and never merged into them, so #1 still reads as #1. */
static void lb_user_callback(int result, const char *error_message,
                  rc_client_leaderboard_entry_list_t *list,
                  rc_client_t *client, void *userdata) {
  const char *self = ra_runtime_lb_self_name(client);
  uint32_t failed_id = (uint32_t)(uintptr_t)userdata;
  int failed_result = 0;
  unsigned int i;
  int added = 0;

  ra_runtime_lock();
  if (result != RC_OK) {
    /* Not fatal (the top rows stand), but silent failures made a board look
         * stuck, so name it. The id comes from userdata: ra_runtime_lb_state()->request_id is not
         * guaranteed to still hold this request by the time the callback runs. */
    failed_result = result;
  }
  if (result == RC_OK && list != NULL && self != NULL && self[0] != '\0' &&
    ra_runtime_lb_state()->entry_count < RA_RUNTIME_MAX_LEADERBOARD_ENTRIES) {
    for (i = 0; i < list->num_entries; ++i) {
      if (list->entries[i].user == NULL ||
        strcmp(self, list->entries[i].user) != 0)
        continue;
      ra_runtime_lb_store_entry(&ra_runtime_lb_state()->entries[ra_runtime_lb_state()->entry_count],
                &list->entries[i], 0, self);
      ++ra_runtime_lb_state()->entry_count;
      added = 1;
      break;
    }
  }
  /* A missing or failed second fetch is not an error page: the top rows are
     * still valid, and the menu shows "not ranked" when there is no user row. */
  if (added)
    ra_runtime_mark_data_changed();
  ra_runtime_lb_state()->state = RA_RUNTIME_LEADERBOARD_ENTRIES_READY;
  ra_runtime_lb_state()->state_since_ms = ra_runtime_now_ms();
  ra_runtime_unlock();

  if (failed_result != 0)
    ra_runtime_lb_log_failure("user row", failed_id, failed_result, NULL);

  if (list != NULL)
    rc_client_destroy_leaderboard_entry_list(list);
}

/* A phase that never answers (offline mid-request, a dropped job) must not leave the
 * entries page spinning forever: report it once, after 15 s. */
static void lb_service_timeout(void) {
  int state = ra_runtime_lb_state()->state;
  unsigned long long since = ra_runtime_lb_state()->state_since_ms;

  if (state == RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING && since != 0 &&
    ra_runtime_now_ms() - since > 15000ULL) {
    ra_runtime_lock();
    if (ra_runtime_lb_state()->state == RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING &&
      ra_runtime_lb_state()->state_since_ms == since) {
      ra_runtime_lb_state()->state = RA_RUNTIME_LEADERBOARD_ENTRIES_ERROR;
      ra_runtime_copy_text(ra_runtime_lb_state()->error, sizeof(ra_runtime_lb_state()->error),
          "Leaderboard entries timed out.");
      ra_runtime_lb_state()->state_since_ms = ra_runtime_now_ms();
      ra_runtime_unlock();
      ra_runtime_log("runtime: leaderboard entries id=%u timed out after "
            "15 s\n", (unsigned)ra_runtime_lb_state()->request_id);
    } else {
      ra_runtime_unlock();
    }
  }
}

/* Takes the pending phase, clears it, and returns the top-rows request flag. */
static int lb_service_take(int *start_user, uint32_t *id) {
  int start_top;

  ra_runtime_lock();
  start_top = ra_runtime_lb_state()->request_pending;
  *start_user = ra_runtime_lb_state()->need_user;
  *id = ra_runtime_lb_state()->request_id;
  ra_runtime_lb_state()->request_pending = 0;
  ra_runtime_lb_state()->need_user = 0;
  ra_runtime_unlock();
  return start_top;
}

static int lb_service_get_rc(ra_session_t **session, rc_client_t **out) {
  ra_client_t *client = ra_runtime_client_acquire(session);
  rc_client_t *rc;

  *out = NULL;
  if (client != NULL) {
    rc = ra_client_get_rc_client(client);
    if (rc != NULL) {
      *out = rc;
      return 1;
    }
    ra_runtime_client_release(*session);
    *session = NULL;
  }
  ra_runtime_lock();
  ra_runtime_lb_state()->state = RA_RUNTIME_LEADERBOARD_ENTRIES_ERROR;
  ra_runtime_copy_text(ra_runtime_lb_state()->error, sizeof(ra_runtime_lb_state()->error),
      "RetroAchievements is not running.");
  ra_runtime_unlock();
  return 0;
}

/* Starts the fetch for the phase `lb_service_take()` reported. */
static void lb_service_begin(rc_client_t *rc, uint32_t id, int start_top) {
  rc_client_async_handle_t *handle;

  if (start_top) {
    /* Offset 1 (the API's first entry), not "around the user": `around_user`
         * asks the server for the player's neighbourhood (`r=lbinfo&u=`), which is
         * what made the list start mid-board and move with the player's rank
         * . Row one must always be #1. */
    handle = rc_client_begin_fetch_leaderboard_entries(
      rc, id, 1, RA_RUNTIME_LEADERBOARD_TOP, lb_top_callback,
      (void*)(uintptr_t)id);
  } else {
    /* Phase 2: only the player's own row, appended after the top rows. */
    handle = rc_client_begin_fetch_leaderboard_entries_around_user(
      rc, id, 1, lb_user_callback, (void*)(uintptr_t)id);
  }

  if (handle == NULL && start_top &&
    ra_runtime_lb_state()->state == RA_RUNTIME_LEADERBOARD_ENTRIES_LOADING) {
    ra_runtime_lock();
    ra_runtime_lb_state()->state = RA_RUNTIME_LEADERBOARD_ENTRIES_ERROR;
    ra_runtime_copy_text(ra_runtime_lb_state()->error, sizeof(ra_runtime_lb_state()->error),
        "Leaderboard entries could not be requested.");
    ra_runtime_unlock();
  }
}

void ra_runtime_lb_service(void) {
  ra_session_t *session = NULL;
  rc_client_t *rc;
  uint32_t id;
  int start_top;
  int start_user;

  session = ra_runtime_session_acquire();
  if (session == NULL)
    return;
  ra_session_release(session);
  session = NULL;

  lb_service_timeout();

  start_top = lb_service_take(&start_user, &id);
  if (!start_top && !start_user)
    return;

  if (!lb_service_get_rc(&session, &rc))
    return;

  lb_service_begin(rc, id, start_top);
  ra_runtime_client_release(session);
}

/* Active challenge list for the RA tab: built from the indicator events, kept even
 * when the indicator row is disabled. */


/* Last tracker value handed to the overlay: replace the text only when it changes. */
int ra_runtime_leaderboard_entries_state(void) {
  int state;

  ra_runtime_lock();
  state = ra_runtime_lb_state()->state;
  ra_runtime_unlock();
  return state;
}

int ra_runtime_copy_leaderboard_entries_error(char *out, size_t capacity) {
  if (out == NULL || capacity == 0)
    return 0;

  ra_runtime_lock();
  ra_runtime_copy_text(out, capacity, ra_runtime_lb_state()->error);
  ra_runtime_unlock();
  return out[0] != '\0';
}

int ra_runtime_copy_leaderboard_entry_at(int index, RaLeaderboardEntryView *out) {
  int found = 0;

  if (out == NULL || index < 0)
    return 0;

  ra_runtime_lock();
  if (index < ra_runtime_lb_state()->entry_count) {
        *out = ra_runtime_lb_state()->entries[index];
    found = 1;
  }
  ra_runtime_unlock();
  return found;
}

/* Cheat plugins: the signed policy is the real gate. Detection only counts files, so it
 * rejects a submission only when Hardcore started after the game booted. Returns 1
 * when the submission must be rejected. */
static int lb_validate_reject_plugins(uint32_t id) {
  if (g_rt.detected_plugins > 0 &&
    ra_settings_hardcore_effective(&g_rt.settings, 1) &&
    !g_rt.booted_with_hardcore) {
    ra_runtime_log("runtime: rejected leaderboard %u: %d cheat plugin(s) installed "
          "and Hardcore was enabled after this game booted; restart the "
          "game with Hardcore on so the plugin policy applies\n",
          (unsigned)id, g_rt.detected_plugins);
    return 1;
  }
  return 0;
}

/* A time leaderboard is a duration in seconds; zero or negative is invalid. */
static int lb_validate_reject_score(uint32_t id,
                                    const rc_client_leaderboard_t *info, int score) {
  if (info->format == RC_CLIENT_LEADERBOARD_FORMAT_TIME && score <= 0) {
    ra_runtime_log("runtime: rejected leaderboard %u invalid time score=%d\n",
          (unsigned)id, score);
    return 1;
  }
  if (score == (-2147483647 - 1)) {
    ra_runtime_log("runtime: rejected leaderboard %u sentinel score\n",
          (unsigned)id);
    return 1;
  }
  return 0;
}

/* Nothing may be submitted while the frame it came from was not a running game:
 * a paused world means the trigger was evaluated against a frozen snapshot. */
static int lb_validate_reject_paused(uint32_t id) {
  if (g_rt.paused) {
    ra_runtime_log("runtime: rejected leaderboard %u: submitted while the world was "
          "paused\n", (unsigned)id);
    return 1;
  }
  return 0;
}

/* POPS: PS1 RAM holds fill bytes before the game initialises it. No submission before
 * evaluated frames, and none of an obvious fill pattern (four equal or printable bytes). */
static int lb_validate_reject_pops_fill(uint32_t id, int score) {
  /* Counted by the runtime loop since `session ready` (`g_rt.frames_since_ready`). */
  if (g_rt.frames_since_ready < RA_RUNTIME_POPS_SUBMIT_GRACE_FRAMES) {
    ra_runtime_log("runtime: rejected leaderboard %u: only %llu frame(s) since "
          "session ready (needs %u)\n", (unsigned)id,
          g_rt.frames_since_ready,
          (unsigned)RA_RUNTIME_POPS_SUBMIT_GRACE_FRAMES);
    return 1;
  } {
    uint32_t value = (uint32_t)score;
    uint8_t b0 = (uint8_t)(value & 0xFF);
    uint8_t b1 = (uint8_t)((value >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)((value >> 16) & 0xFF);
    uint8_t b3 = (uint8_t)((value >> 24) & 0xFF);
    int same = (b0 == b1 && b1 == b2 && b2 == b3);
    int printable = b0 >= 0x20 && b0 < 0x7F && b1 >= 0x20 && b1 < 0x7F &&
            b2 >= 0x20 && b2 < 0x7F && b3 >= 0x20 && b3 < 0x7F;
    /* Distinct-byte count: fill memory is a small alphabet, a real score is
           * not. `0x2A2A2A2A` has one distinct byte, `0x2A2A2A29` (the value the
           *  log caught only via the frame gate) has two, and a genuine
           * score of any size almost always has three or four. Cheap, general and
           * independent of which fill byte the emulated machine happens to use. */
    uint8_t distinct = 1;
    int too_few_bytes;

    if (b1 != b0)
      ++distinct;
    if (b2 != b0 && b2 != b1)
      ++distinct;
    if (b3 != b0 && b3 != b1 && b3 != b2)
      ++distinct;
    too_few_bytes = distinct <= 2;

    if (same || printable || too_few_bytes) {
      ra_runtime_log("runtime: rejected leaderboard %u fill value 0x%08X "
            "(same=%d printable=%d distinct=%u); PS1 RAM was not "
            "initialised\n",
            (unsigned)id, (unsigned)value, same, printable,
            (unsigned)distinct);
      return 1;
    }
  }
  /* Proof that the guard ran and let this one through on purpose. */
  ra_runtime_log("runtime: leaderboard submit allowed id=%u value=%d frames=%llu\n",
        (unsigned)id, score, g_rt.frames_since_ready);
  return 0;
}

int ra_runtime_leaderboard_validate_impl(uint32_t id, int score) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc;
  const rc_client_leaderboard_t *info;
  int allowed = 1;

  if (client != NULL &&
      (rc = ra_client_get_rc_client(client)) != NULL &&
      (info = rc_client_get_leaderboard_info(rc, id)) != NULL) {
    if (lb_validate_reject_plugins(id) ||
        lb_validate_reject_score(id, info, score) ||
        lb_validate_reject_paused(id) ||
        (g_rt.game_is_pops && lb_validate_reject_pops_fill(id, score)))
      allowed = 0;
  }
  ra_runtime_client_release(session);
  return allowed;
}

static RaRuntimeLbState g_state;

RaRuntimeLbState *ra_runtime_lb_state(void) { return &g_state; }

void ra_runtime_lb_log_failure(const char *phase, uint32_t id, int result,
                const char *error_message) {
  char text[80];
  size_t i;

  if (error_message == NULL || (unsigned char)error_message[0] < 0x20 ||
    (unsigned char)error_message[0] >= 0x7F) {
    ra_runtime_log("runtime: leaderboard %s id=%u failed rc=%d\n", phase,
          (unsigned)id, result);
    return;
  }
  for (i = 0; i + 1 < sizeof(text) && error_message[i] != '\0'; ++i) {
    unsigned char c = (unsigned char)error_message[i];
    text[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
  }
  text[i] = '\0';
  ra_runtime_log("runtime: leaderboard %s id=%u failed rc=%d: %s\n", phase,
        (unsigned)id, result, text);
}

const char *ra_runtime_lb_self_name(rc_client_t *client) {
  const rc_client_user_t *user;

  if (client == NULL)
    return NULL;
  user = rc_client_get_user_info(client);
  return user != NULL ? user->display_name : NULL;
}

void ra_runtime_lb_store_entry(RaLeaderboardEntryView *destination,
                const rc_client_leaderboard_entry_t *source,
                unsigned int fallback_rank, const char *self) {
  memset(destination, 0, sizeof(*destination));
  destination->rank = source->rank != 0 ? source->rank : fallback_rank;
  ra_runtime_copy_text(destination->user, sizeof(destination->user), source->user);
  ra_runtime_copy_text(destination->value, sizeof(destination->value), source->display);
  /* `user_index` says whether the *list *contains the signed-in user, but a name
     * compare is what the row needs; it also covers the row the server sends for
     * the `&u=` request. */
  if (self != NULL && self[0] != '\0' && source->user != NULL &&
    strcmp(self, source->user) == 0)
    destination->is_user = 1;
  if (source->submitted > 0) {
    struct tm *local = localtime(&source->submitted);
    if (local != NULL)
      strftime(destination->submitted, sizeof(destination->submitted),
          "%Y-%m-%d", local);
  }
}

#include "ra_runtime_hashlib.h"
#include "ra_runtime_badges.h"
#include "ra_runtime_core.h"
#include "ra_runtime_internal.h"

#include <stdio.h>
#include <string.h>

#include "ra_client.h"
#include "ra_session.h"
#include "ra_hash.h"
#include "ra_runtime.h"
#include "rc_client.h"
#include "rc_api_runtime.h"
#include "rc_consoles.h"

#define RA_HASH_LIBRARY_MAX 1024

static struct {
  char hash[33];
  uint32_t game_id;
} g_hash_library[RA_HASH_LIBRARY_MAX];
static int g_hash_library_count;
static int g_hash_library_requested;

void ra_runtime_hashlib_reset(void) {
  ra_runtime_lock();
  g_hash_library_requested = 0;
  ra_runtime_unlock();
}

int ra_runtime_hash_library_count(void) {
  int count;

  ra_runtime_lock();
  count = g_hash_library_count;
  ra_runtime_unlock();
  return count;
}

int ra_runtime_start_pending(void);
int ra_runtime_game_is_pops(void);
int ra_runtime_stopped(void);

int ra_runtime_hash_library_game_id(const char *hash, uint32_t *game_id) {
  int i;

  if (hash == NULL || game_id == NULL)
    return 0;
  ra_runtime_lock();
  for (i = 0; i < g_hash_library_count; ++i) {
    if (strcmp(g_hash_library[i].hash, hash) == 0) {
      *game_id = g_hash_library[i].game_id;
      ra_runtime_unlock();
      return 1;
    }
  }
  ra_runtime_unlock();
  return 0;
}

static void hash_library_callback(int result, const char *error_message,
                    rc_client_hash_library_t *list,
                    rc_client_t *client, void *userdata) {
  unsigned int i;

  (void)client;
  (void)userdata;
  if (result != RC_OK || list == NULL) {
    ra_runtime_log("runtime: hash library failed (%d%s%s)\n", result,
          error_message != NULL ? " " : "",
          error_message != NULL ? error_message : "");
    return;
  }

  ra_runtime_lock();
  g_hash_library_count = 0;
  for (i = 0; i < list->num_entries &&
        g_hash_library_count < RA_HASH_LIBRARY_MAX; ++i) {
    const rc_client_hash_library_entry_t *entry = &list->entries[i];

    if (entry->hash[0] == '\0' || entry->game_id == 0)
      continue;
    snprintf(g_hash_library[g_hash_library_count].hash,
        sizeof(g_hash_library[g_hash_library_count].hash), "%s",
        entry->hash);
    g_hash_library[g_hash_library_count].game_id = entry->game_id;
    ++g_hash_library_count;
  }
  ra_runtime_unlock();
  ra_runtime_log("runtime: hash library %u entries kept of %u\n",
        (unsigned)g_hash_library_count, (unsigned)list->num_entries);
  /* The client allocated this list; it is ours to free (rcheevos'
     * `rc_client_destroy_hash_library`). Never freed here until  - a leak of
     * ~80 KB per session, in the module's heap, right where pspemu re-allocates when
     * `Exit PspEmu Application` restarts it. */
  rc_client_destroy_hash_library(list);
}

/* Fetched once, and only after the load: the library is a large response and the
 * load path is already the slowest thing a session does. */
void ra_runtime_maybe_fetch_hash_library(void) {
  ra_session_t *session;
  ra_client_t *client = ra_runtime_client_acquire(&session);
  rc_client_t *rc;

  if (client == NULL)
    return;
  if (g_hash_library_requested || ra_runtime_stopped()) {
    ra_runtime_client_release(session);
    return;
  }
  if (ra_runtime_start_pending() || !ra_session_is_ready(session)) {
    ra_runtime_client_release(session);
    return;
  }
  rc = ra_client_get_rc_client(client);
  if (rc == NULL) {
    ra_runtime_client_release(session);
    return;
  }

  g_hash_library_requested = 1;
  rc_client_begin_fetch_hash_library(
    rc, ra_runtime_game_is_pops() ? RC_CONSOLE_PLAYSTATION : RC_CONSOLE_PSP,
    hash_library_callback, NULL);
  ra_runtime_log("runtime: hash library requested (console=%u)\n",
        (unsigned)(ra_runtime_game_is_pops() ? RC_CONSOLE_PLAYSTATION
                    : RC_CONSOLE_PSP));
  ra_runtime_client_release(session);
}

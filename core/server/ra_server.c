#include "ra_server.h"
#include "ra_server_json.h"
#include "ra_server_util.h"
#include "ra_server_payload.h"
#include "ra_server_sets.h"
#include "ra_server_job.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"

#include "ra_arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#endif

static ra_server_transport_t g_transport;
static int g_rich_presence_enabled = 1;
static int g_leaderboard_submits_blocked;
static unsigned g_blocked_leaderboard_submits;
static ra_server_leaderboard_validator_t g_leaderboard_validator;
static unsigned g_rejected_leaderboard_submits;
static unsigned int g_payload_set = RA_SERVER_NO_TRIM;
static ra_server_parse_hook_t g_parse_before;
static ra_server_parse_hook_t g_parse_after;
static ra_server_timing_reporter_t g_timing_reporter;
static ra_server_award_reporter_t g_award_reporter;

/* Answers rcheevos gets without a network request. File scope, because they are
 * decided while the request is queued (see ra_server_job_prepare). */

/* Milliseconds for the timing report. Wall clock on Vita (a stalled network call
 * must show its real cost); CPU time on the host, where the tests only need
 * ordering, not absolute values. */
void ra_server_set_payload_set(unsigned int set_id) { g_payload_set = set_id; }
unsigned int ra_server_get_payload_set(void) { return g_payload_set; }

/* Deletes what the retired achievement cache left on disk. Vita only; the host
 * builds (and the tests) have nothing to purge. Best effort by design: a missing
 * directory, a busy file or an unwritable card must never fail a load. */
void ra_server_purge_caches(void) {
#ifdef __vita__
  static const char k_dir[] = "ux0:data/AdrenalineRA/sets";
  SceUID dir;
  SceIoDirent entry;

  dir = sceIoDopen(k_dir);
  if (dir >= 0) {
    for (;;) {
      char path[192];

      memset(&entry, 0, sizeof(entry));
      if (sceIoDread(dir, &entry) <= 0)
        break;
      if (entry.d_name[0] == '\0' || entry.d_name[0] == '.')
        continue;
      snprintf(path, sizeof(path), "%s/%s", k_dir, entry.d_name);
      sceIoRemove(path);
    }
    sceIoDclose(dir);
  }
  sceIoRmdir(k_dir);
  /* The hash cache is one file; hashes are recomputed every launch now. */
  sceIoRemove("ux0:data/AdrenalineRA/hash_cache");
#endif
}


void ra_server_set_transport(ra_server_transport_t transport) {
  g_transport = transport;
}


void ra_server_set_parse_hooks(ra_server_parse_hook_t before,
              ra_server_parse_hook_t after) {
  g_parse_before = before;
  g_parse_after = after;
}

void ra_server_set_timing_reporter(ra_server_timing_reporter_t reporter) {
  g_timing_reporter = reporter;
}

void ra_server_set_award_reporter(ra_server_award_reporter_t reporter) {
  g_award_reporter = reporter;
}

ra_server_transport_t ra_server_transport(void) { return g_transport; }
ra_server_parse_hook_t ra_server_parse_before(void) { return g_parse_before; }
ra_server_parse_hook_t ra_server_parse_after(void) { return g_parse_after; }
ra_server_timing_reporter_t ra_server_timing_reporter(void) { return g_timing_reporter; }
ra_server_award_reporter_t ra_server_award_reporter(void) { return g_award_reporter; }
int ra_server_leaderboard_blocked(void) { return g_leaderboard_submits_blocked; }
ra_server_leaderboard_validator_t ra_server_leaderboard_validator(void) {
  return g_leaderboard_validator;
}
void ra_server_note_blocked_lb(void) { ++g_blocked_leaderboard_submits; }
void ra_server_note_rejected_lb(void) { ++g_rejected_leaderboard_submits; }

void ra_server_set_rich_presence_enabled(int enabled) {
  g_rich_presence_enabled = enabled != 0;
}

int ra_server_rich_presence_enabled(void) {
  return g_rich_presence_enabled;
}

void ra_server_set_leaderboard_submits_blocked(int blocked) {
  g_leaderboard_submits_blocked = blocked != 0;
}

unsigned ra_server_blocked_leaderboard_submits(void) {
  return g_blocked_leaderboard_submits;
}

void ra_server_set_leaderboard_validator(ra_server_leaderboard_validator_t validator) {
  g_leaderboard_validator = validator;
}

unsigned ra_server_rejected_leaderboard_submits(void) {
  return g_rejected_leaderboard_submits;
}

static int g_platform_warning;

int ra_server_platform_warning(void) { return g_platform_warning; }
void ra_server_reset_platform_warning(void) { g_platform_warning = 0; }
void ra_server_platform_note_warning(const rc_api_server_response_t *response) {
  if (!g_platform_warning)
    g_platform_warning = ra_server_platform_read_warning(response, NULL);
}

int ra_server_platform_read_warning(const rc_api_server_response_t *server_response,
                         int *answered) {
  rc_api_start_session_response_t response;
  uint32_t i;
  int unsupported = 0;

  if (answered != NULL)
    *answered = 0;
  memset(&response, 0, sizeof(response));
  if (rc_api_process_start_session_server_response(&response, server_response) == RC_OK &&
    response.response.succeeded) {
    if (answered != NULL)
      *answered = 1;
    for (i = 0; i < response.num_unlocks; ++i) {
      if (response.unlocks[i].achievement_id == 101000001u) {
        unsupported = 1;
        break;
      }
    }
    for (i = 0; !unsupported && i < response.num_hardcore_unlocks; ++i) {
      if (response.hardcore_unlocks[i].achievement_id == 101000001u)
        unsupported = 1;
    }
  }
  rc_api_destroy_start_session_response(&response);
  return unsupported;
}

/* One-off hash resolve (Plan A). */


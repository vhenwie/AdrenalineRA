#ifndef RA_SERVER_H
#define RA_SERVER_H

#include "rc_client.h"
#include "rc_api_user.h"
#include "ra_http.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ra_server_transport_t)(const char *url, const char *post_data,
                  size_t post_length, const char *content_type,
                  ra_http_response_t *response);

/* Install the transport used by ra_server_call. The Vita module installs
 * ra_http_post; host tests install a mock. */
void ra_server_set_transport(ra_server_transport_t transport);
/* Parse hooks for r=achievementsets; both run on the requesting thread. */
typedef void (*ra_server_parse_hook_t)(void);
void ra_server_set_parse_hooks(ra_server_parse_hook_t before,
              ra_server_parse_hook_t after);

/* Optional per-request timing report. The load timing was one opaque number:
 * `payload ready`/`parse done` say how long the whole rcheevos callback took,
 * but not whether the time was spent on the network or in the parser. This
 * reports every request with its own name and duration so the next hardware log
 * answers that from one line. */
typedef void (*ra_server_timing_reporter_t)(const char *request, unsigned int ms,
                      int http_status);
void ra_server_set_timing_reporter(ra_server_timing_reporter_t reporter);

/* Reports awardachievement outcome; only 200 + Success:true is confirmed. */
typedef void (*ra_server_award_reporter_t)(unsigned int achievement_id,
                    int hardcore, int confirmed);
void ra_server_set_award_reporter(ra_server_award_reporter_t reporter);


/* When disabled, the rich-presence field is removed from ping requests. */
void ra_server_set_rich_presence_enabled(int enabled);

/* When blocked, leaderboard entry submissions (r=submitlbentry) are answered
 * locally with a failure and are never sent to the network. Used for Encore
 * Mode, where a replayed run must not post a leaderboard entry. */
void ra_server_set_leaderboard_submits_blocked(int blocked);

/* Number of leaderboard submissions dropped by the guard since startup. */
unsigned ra_server_blocked_leaderboard_submits(void);

/* Optional value validator. Called for each submitlbentry request with the
 * leaderboard id and submitted score. Returning 0 rejects the request locally
 * (for example, a negative time value) and it is never sent to the network. */
typedef int (*ra_server_leaderboard_validator_t)(uint32_t leaderboard_id,
                        int score);
void ra_server_set_leaderboard_validator(ra_server_leaderboard_validator_t validator);

/* Number of leaderboard submissions rejected by the validator. */
unsigned ra_server_rejected_leaderboard_submits(void);

/* Peak-memory control: keep one set and load the rest on demand. */
#define RA_SERVER_KEEP_FIRST_SET 0u
#define RA_SERVER_NO_TRIM 0xFFFFFFFFu
/* Merge all sets into the first; multi-subset evaluation crashes on this device. */
#define RA_SERVER_MERGE_SETS 0xFFFFFFFEu

void ra_server_set_payload_set(unsigned int set_id);
unsigned int ra_server_get_payload_set(void);

/* Set objects seen in the last r=achievementsets payload, counted before the merge
 * decision: the merged body keeps one set by design, so this is the only local
 * evidence of whether the server offered subsets for the game. 0 = not counted
 * (no `Sets` key, or the parse failed before the first object completed). */
int ra_server_last_payload_sets(void);

/* Why the merge walk ended as it did (RA_MERGE_ *in ra_server.c):
 *   0 merged / 1 no body / 2 no "Sets" / 3 no array / 4 bad set object
 *   5 set without "Achievements" / 6 array did not close / 7 body really has one set
 *   8 two or more sets but the index failed / 9 out of memory
 * `payload=1 reason=7` is a one-set body; `payload=1 reason=4` means the server sent
 * more and the walk gave up, which is a client bug. */
int ra_server_last_merge_reason(void);

/* Bytes of the body the walk ran on (fetched, before any rewrite). */
size_t ra_server_last_sets_bytes(void);

/* Rewrites a multi-set body so the first set holds every achievement and the
 * remaining sets hold none. Returns a malloc'd body (free with ra_arena_free) and
 * sets *out_length, or NULL when there is nothing to merge. */
char *ra_server_merge_sets(const char *body, size_t length, size_t *out_length);

/* Removes the achievement rows of every set except the selected one, in place.
 * Set objects are preserved (the subset picker needs them); only their
 * "Achievements" arrays are emptied. Returns the new body length. */
size_t ra_server_trim_payload(char *body, size_t length, unsigned int set_id);

/* Bytes removed from the last sets payload (0 when nothing was trimmed). */
/* Signed payload change from the last set merge (0 when there was one set). */
long ra_server_last_merge_delta(void);
size_t ra_server_last_trim_bytes(void);
/* Clears the counter; call before a load, not per request. */
void ra_server_reset_trim_bytes(void);
/* Clears the sets counters; call before a load, not per request. */
void ra_server_reset_sets_counters(void);

/* Merged-set index: the merge records set ids, types, titles and counts so the
 * menu can offer a set picker without re-reading the payload. */

#define RA_SERVER_MAX_SETS 8
#define RA_SERVER_SET_TITLE_MAX 48
/* One entry per achievement of the merged payload: 4096 covers a five-subset game
 * of ~3000 achievements with room to spare, and costs 32 KB of BSS. An overflow
 * drops the whole index (the menu falls back to one list) rather than filtering
 * wrongly, so this number is a capacity guarantee, not a target. */
#define RA_SERVER_SET_INDEX_CAPACITY 4096

/* Mirrors RC_ACHIEVEMENT_SET_TYPE_ *in rc_api_runtime.h. */
#define RA_SERVER_SET_TYPE_CORE 0
#define RA_SERVER_SET_TYPE_BONUS 1
#define RA_SERVER_SET_TYPE_SPECIALTY 2
#define RA_SERVER_SET_TYPE_EXCLUSIVE 3
/* "Challenge" is the fourth subset type in the RA docs (Bonus, Challenge, Specialty,
 * Exclusive): no patched ROM needed, but opt-in per user preference. rcheevos 12.5
 * has no value for it and files it under BONUS (rc_api_runtime.c:453-462, the same
 * fallback it uses for unknown strings), which is fine for evaluation. It is kept
 * distinct here because the picker shows the type next to the set name. */
#define RA_SERVER_SET_TYPE_CHALLENGE 4

/* Display name of a RA_SERVER_SET_TYPE_ *value ("Core", "Bonus", "Challenge",
 * "Specialty", "Exclusive"). */
const char *ra_server_set_type_name(unsigned int type);

typedef struct {
  uint32_t id;
  uint32_t achievements;
  uint8_t type;
  char title[RA_SERVER_SET_TITLE_MAX];
} RaServerSetInfo;

/* Sets in payload order, including the core set. 0 when the last load did not
 * merge (single-set game, or the payload could not be indexed). */
int ra_server_set_count(void);
int ra_server_set_at(int index, RaServerSetInfo *out);
/* Real set id the achievement came from, or 0 when unknown. */
uint32_t ra_server_achievement_set(uint32_t achievement_id);

/* Server-decided data is never cached; only images are cached. */
void ra_server_purge_caches(void);

/* rc_client_server_call_t-compatible callback. */

/* Request jobs run off the logic thread; the rcheevos callback stays on it. */
#define RA_SERVER_JOB_URL_MAX 1024
#define RA_SERVER_JOB_POST_MAX 1024
#define RA_SERVER_JOB_TYPE_MAX 64
#define RA_SERVER_JOB_NAME_MAX 32

typedef struct {
  /* Request, copied: rcheevos frees its own buffers as soon as the server_call
     * hook returns, so nothing here may point at them (except in the synchronous
     * path, which borrows them and finishes before returning). */
  rc_client_server_callback_t callback;
  void *callback_data;
  char url_storage[RA_SERVER_JOB_URL_MAX];
  char post_storage[RA_SERVER_JOB_POST_MAX];
  char type_storage[RA_SERVER_JOB_TYPE_MAX];
  const char *url;
  const char *post_data;
  const char *content_type;
  int sets_request;
  int rejected;              /* answered locally, no network involved */
  const char *reject_body;
  char request_name[RA_SERVER_JOB_NAME_MAX];

  /* Response, filled by execute and consumed by finish. */
  ra_http_response_t http;
  const char *response_body;
  uint32_t response_length;
  int response_status;
  char transport_error[64];
  unsigned long long started_ms;
} RaServerJob;

/* Copies a request into a job. `allow_borrow` lets the job point at the caller's
 * buffers when a body does not fit (only valid for the synchronous path, where
 * the request outlives the whole call). Returns 1 on success. */
int ra_server_job_prepare(RaServerJob *job, const rc_api_request_t *request,
            rc_client_server_callback_t callback,
            void *callback_data, int allow_borrow);
/* Does the network and processing work. Touches no rc_client state and never
 * invokes the callback: this is what runs on the worker. */
void ra_server_job_execute(RaServerJob *job);
/* Delivers the response to rcheevos. Must run on the thread that owns the
 * rc_client instance, which is also where the parse hooks fire. */
void ra_server_job_finish(RaServerJob *job);
/* Releases a job whose callback will never run (shutdown, discarded queue). */
void ra_server_job_dispose(RaServerJob *job);

void ra_server_call(const rc_api_request_t *request,
          rc_client_server_callback_t callback, void *callback_data,
          rc_client_t *client);

/* Resolve one hash through the server's r=gameid endpoint (Plan A). */
int ra_server_resolve_hash(const char *hash, uint32_t *game_id, int *asked);

/* Diagnostic only: start a session with Hardcore requested and report whether
 * the current identity receives warning 101000001. */
int ra_server_check_platform(uint32_t game_id, const char *game_hash,
                             const char *username, const char *token,
                             int *unsupported);

/* The current session's startsession response contained warning 101000001.
 * Reset once per session; not persisted. */
void ra_server_reset_platform_warning(void);
int ra_server_platform_warning(void);
void ra_server_platform_note_warning(const rc_api_server_response_t *response);
int ra_server_platform_read_warning(const rc_api_server_response_t *response,
                                    int *answered);

#ifdef __cplusplus
}
#endif

#endif /* RA_SERVER_H */

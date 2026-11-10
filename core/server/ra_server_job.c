#include "ra_server_job.h"

#include "ra_server_util.h"
#include "ra_server_payload.h"
#include "ra_server_sets.h"
#include "ra_arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_blocked_lb_body[] =
  "{\"Success\":false,\"Error\":\"Leaderboard submission disabled in Encore Mode.\"}";
static const char k_invalid_lb_body[] =
  "{\"Success\":false,\"Error\":\"Leaderboard value rejected by client.\"}";

static int job_reject_early(RaServerJob *job) {
  /* Blocked and rejected submissions were decided while the request was
     * queued; nothing here may reach the network for them. */
  if (job->rejected) {
    job->response_body = job->reject_body;
    job->response_length = (uint32_t)strlen(job->reject_body);
    job->response_status = 200;
    return 1;
  }
  return 0;
}

static void job_transport_call(RaServerJob *job, const char *post_data,
    ra_http_response_t *http_response) {
  if (job->url != NULL && ra_server_transport() != NULL) {
    ra_server_transport()(job->url, post_data, strlen(post_data),
          job->content_type != NULL
            ? job->content_type
            : "application/x-www-form-urlencoded",
          http_response);
  }
}

static void job_merge_payload(const char *post_data,
    ra_http_response_t *http_response) {
  if (http_response->body != NULL &&
    ra_server_get_payload_set() == RA_SERVER_MERGE_SETS &&
    strstr(post_data, "r=achievementsets") != NULL) {
    size_t merged_length = 0;
    char *merged;

    ra_server_sets_set_body_bytes(http_response->body_length);
    merged = ra_server_merge_sets(http_response->body,
                  http_response->body_length, &merged_length);
    if (merged != NULL) {
      ra_server_sets_set_merge_delta((long)merged_length -
            (long)http_response->body_length);
      /* The merged body replaces the fetched one. Only the fetched buffer is
             * owned here: a mock transport hands back static memory, and freeing
             * that is a bad free (ASAN caught it in the host test). */
      if (http_response->body_owned)
        ra_arena_free(http_response->body);
      http_response->body = merged;
      http_response->body_length = (uint32_t)merged_length;
      http_response->body_owned = 1;
    }
  } else if (http_response->body != NULL &&
      ra_server_get_payload_set() != RA_SERVER_NO_TRIM &&
      strstr(post_data, "r=achievementsets") != NULL) {
    size_t trimmed = ra_server_trim_payload(http_response->body,
                        http_response->body_length,
                        ra_server_get_payload_set());

    if (trimmed < http_response->body_length) {
      /* Shrink the allocation too: the body is alive for the whole parse,
             * and this is where the heap is won or lost. */
      char *shrunk = (char*)realloc(http_response->body, trimmed + 1);

      if (shrunk != NULL)
        http_response->body = shrunk;
      http_response->body_length = trimmed;
    }
  }
}

static void job_set_response(RaServerJob *job,
    ra_http_response_t *http_response, const char *body) {
  memset(&job->http, 0, sizeof(job->http));
  job->http = *http_response;

  if (http_response->status_code <= 0) {
    /* PPSSPP (Core/RetroAchievements.cpp: complete_server_call) reports a
         * transport failure as RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR and
         * puts the reason in the body. A bare status 0 only yields "No response",
         * which erases whether the failure was DNS, TLS, timeout or a short read,
         * and rcheevos treats it as a dropped request instead of a retryable one. */
    snprintf(job->transport_error, sizeof(job->transport_error),
        "Network error (%d)", http_response->status_code);
    job->response_body = job->transport_error;
    job->response_length = (uint32_t)strlen(job->transport_error);
    job->response_status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  } else {
    job->response_body = body;
    job->response_length = http_response->body_length;
    job->response_status = http_response->status_code;
  }
}

static void job_report_timing(RaServerJob *job, int status_code) {
  /* Timing first: it measures the request, not the queue wait or the parse. */
  if (ra_server_timing_reporter() != NULL) {
    ra_server_timing_reporter()(job->request_name,
            (unsigned int)(ra_server_now_ms() - job->started_ms),
            status_code);
  }
}

static void job_report_award(RaServerJob *job, int status_code,
    const char *response_body) {
  /* Report every award outcome as the logs line; nothing is stored and nothing is
     * replayed back into rcheevos - the site stays authoritative for earned state. */
  if (ra_server_award_reporter() != NULL && status_code > 0 &&
    strstr(job->post_data, "r=awardachievement") != NULL) {
    int achievement_id = 0;
    int hardcore = 0;
    int confirmed =
      status_code == 200 && response_body != NULL &&
      strstr(response_body, "\"Success\":true") != NULL;

    if (ra_server_url_param_int(job->post_data, "a", &achievement_id) &&
      achievement_id > 0) {
      (void)ra_server_url_param_int(job->post_data, "h", &hardcore);
      ra_server_award_reporter()((unsigned int)achievement_id, hardcore != 0,
              confirmed);
    }
  }
}

void ra_server_job_execute(RaServerJob *job) {
  ra_http_response_t http_response;
  const char *body = "";
  const char *post_data;
  char filtered[2048];

  if (job == NULL)
    return;
  if (job_reject_early(job))
    return;

  post_data = job->post_data != NULL ? job->post_data : "";
  job->started_ms = ra_server_now_ms();
  memset(&http_response, 0, sizeof(http_response));

  if (strstr(post_data, "r=ping") != NULL)
    post_data = ra_server_filter_rich_presence(post_data, filtered,
                    sizeof(filtered));

  /* No achievement-data cache, by the server resolves which sets a
     * session receives from the game hash *and *the account's per-set opt-in /
     * opt-out (RA docs: multiset / subset types), so a stored payload would keep
     * handing out a set list the account has already changed. Every load fetches.
     * Images are still cached (ra_badge.c). */
  job_transport_call(job, post_data, &http_response);

  job_merge_payload(post_data, &http_response);

  if (http_response.body != NULL)
    body = http_response.body;

  job_set_response(job, &http_response, body);
  job_report_timing(job, http_response.status_code);
  job_report_award(job, http_response.status_code, http_response.body);
}

void ra_server_job_finish(RaServerJob *job) {
  rc_api_server_response_t server_response;

  if (job == NULL)
    return;

  memset(&server_response, 0, sizeof(server_response));
  server_response.body = job->response_body != NULL ? job->response_body : "";
  server_response.body_length = job->response_length;
  server_response.http_status_code = job->response_status;

  if (!ra_server_platform_warning() && job->post_data != NULL &&
      strstr(job->post_data, "r=startsession") != NULL &&
      job->response_status == 200)
    ra_server_platform_note_warning(&server_response);

  /* The parse hooks freeze the emulated world around an `achievementsets`
     * parse. They run here, on the thread that owns rc_client - the thread the
     * freeze has to protect - and the download before this point never happens
     * under that freeze. */
  if (job->sets_request && ra_server_parse_before() != NULL)
    ra_server_parse_before()();
  if (job->callback != NULL)
    job->callback(&server_response, job->callback_data);
  if (job->sets_request && ra_server_parse_after() != NULL)
    ra_server_parse_after()();

  ra_http_response_free(&job->http);
  memset(&job->http, 0, sizeof(job->http));
  job->response_body = NULL;
  job->response_length = 0;
}

void ra_server_job_dispose(RaServerJob *job) {
  if (job == NULL)
    return;
  ra_http_response_free(&job->http);
  memset(&job->http, 0, sizeof(job->http));
  job->response_body = NULL;
  job->response_length = 0;
  job->callback = NULL;
  job->callback_data = NULL;
}

static int job_store_url(RaServerJob *job, const char *url,
    int allow_borrow) {
  if (url != NULL) {
    size_t length = strlen(url);

    if (length < sizeof(job->url_storage)) {
      memcpy(job->url_storage, url, length + 1);
      job->url = job->url_storage;
    } else if (allow_borrow) {
      job->url = url;
    } else {
      return 0;
    }
  }
  return 1;
}

static int job_store_post(RaServerJob *job, const char *post_data,
    int allow_borrow) {
  if (post_data != NULL) {
    size_t length = strlen(post_data);

    if (length < sizeof(job->post_storage)) {
      memcpy(job->post_storage, post_data, length + 1);
      job->post_data = job->post_storage;
    } else if (allow_borrow) {
      job->post_data = post_data;
    } else {
      return 0;
    }
  }
  return 1;
}

static int job_store_content_type(RaServerJob *job, const char *content_type,
    int allow_borrow) {
  if (content_type != NULL) {
    size_t length = strlen(content_type);

    if (length < sizeof(job->type_storage)) {
      memcpy(job->type_storage, content_type, length + 1);
      job->content_type = job->type_storage;
    } else if (allow_borrow) {
      job->content_type = content_type;
    }
  }
  return 1;
}

static void job_validate_lb(RaServerJob *job) {
  /* A leaderboard value that cannot be legitimate (a non-positive time, for
     * example) must never reach the server. */
  if (ra_server_leaderboard_validator() != NULL &&
    strstr(job->post_data, "r=submitlbentry") != NULL) {
    int leaderboard_id = 0;
    int score = 0;

    if (ra_server_url_param_int(job->post_data, "i", &leaderboard_id) &&
      ra_server_url_param_int(job->post_data, "s", &score) &&
      !ra_server_leaderboard_validator()((uint32_t)leaderboard_id, score)) {
      ra_server_note_rejected_lb();
      job->rejected = 1;
      job->reject_body = k_invalid_lb_body;
    }
  }
}

int ra_server_job_prepare(RaServerJob *job, const rc_api_request_t *request,
            rc_client_server_callback_t callback,
            void *callback_data, int allow_borrow) {
  if (job == NULL)
    return 0;

  memset(job, 0, sizeof(*job));
  job->callback = callback;
  job->callback_data = callback_data;
  job->post_data = "";

  if (!job_store_url(job, request != NULL ? request->url : NULL,
        allow_borrow))
    return 0;
  if (!job_store_post(job, request != NULL ? request->post_data : NULL,
        allow_borrow))
    return 0;
  job->content_type = "application/x-www-form-urlencoded";
  if (!job_store_content_type(job,
        request != NULL ? request->content_type : NULL, allow_borrow))
    return 0;

  job->sets_request = strstr(job->post_data, "r=achievementsets") != NULL;
  ra_server_request_name(job->post_data, job->request_name,
            sizeof(job->request_name));

  /* Encore Mode is a replay: never let it post a leaderboard entry. Decided
     * while the request is queued, so the counter and the answer stay in request
     * order instead of depending on when the worker got to it. */
  if (ra_server_leaderboard_blocked() &&
    (strstr(job->post_data, "r=submitlbentry") != NULL ||
    (job->url != NULL && strstr(job->url, "submitlbentry") != NULL))) {
    ra_server_note_blocked_lb();
    job->rejected = 1;
    job->reject_body = k_blocked_lb_body;
    return 1;
  }

  job_validate_lb(job);

  return 1;
}

void ra_server_call(const rc_api_request_t *request,
          rc_client_server_callback_t callback, void *callback_data,
          rc_client_t *client) {
  /* Synchronous composition: prepare, execute and deliver before returning.
     * Used by the host tests and as the runtime's fallback when the async queue
     * cannot take the request (see user/ra_netqueue.c). */
  static RaServerJob sync_job;

  (void)client;
  (void)ra_server_job_prepare(&sync_job, request, callback, callback_data, 1);
  ra_server_job_execute(&sync_job);
  ra_server_job_finish(&sync_job);
}


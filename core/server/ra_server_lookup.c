#include "ra_server.h"

#include "rc_api_runtime.h"
#include "rc_api_user.h"

#include <string.h>

typedef struct {
  int answered;
  uint32_t game_id;
} RaResolveSink;

static void resolve_hash_callback(const rc_api_server_response_t *server_response,
                void *callback_data) {
  RaResolveSink *sink = (RaResolveSink*)callback_data;
  rc_api_resolve_hash_response_t response;

  if (sink == NULL)
    return;
  memset(&response, 0, sizeof(response));
  /* A transport failure leaves the response empty; the processor then returns non-OK
     * and `answered` stays 0, which is the "could not ask" outcome the caller needs. */
  if (rc_api_process_resolve_hash_server_response(&response, server_response) == RC_OK) {
    sink->answered = 1;
    sink->game_id = response.game_id;
  }
  rc_api_destroy_resolve_hash_response(&response);
}

typedef struct {
  int answered;
  int unsupported;
} RaPlatformSink;

static void platform_check_callback(const rc_api_server_response_t *response,
                  void *data) {
  RaPlatformSink *sink = (RaPlatformSink*)data;

  if (sink == NULL)
    return;
  sink->unsupported = ra_server_platform_read_warning(response, &sink->answered);
}

int ra_server_check_platform(uint32_t game_id, const char *game_hash,
                             const char *username, const char *token,
                             int *unsupported) {
  rc_api_start_session_request_t params;
  rc_api_request_t request;
  RaPlatformSink sink;

  if (unsupported != NULL)
    *unsupported = 0;
  if (game_id == 0 || username == NULL || token == NULL)
    return 0;
  memset(&sink, 0, sizeof(sink));
  memset(&params, 0, sizeof(params));
  params.username = username;
  params.api_token = token;
  params.game_id = game_id;
  params.game_hash = game_hash;
  params.hardcore = 1;
  memset(&request, 0, sizeof(request));
  if (rc_api_init_start_session_request(&request, &params) != RC_OK)
    return 0;
  ra_server_call(&request, platform_check_callback, &sink, NULL);
  rc_api_destroy_request(&request);
  if (unsupported != NULL)
    *unsupported = sink.unsupported;
  return sink.answered;
}

int ra_server_resolve_hash(const char *hash, uint32_t *game_id, int *asked) {
  rc_api_resolve_hash_request_t params;
  rc_api_request_t request;
  RaResolveSink sink;

  if (game_id != NULL)
        *game_id = 0;
  if (asked != NULL)
        *asked = 0;
  if (hash == NULL || hash[0] == '\0')
    return 0;

  memset(&sink, 0, sizeof(sink));
  memset(&params, 0, sizeof(params));
  params.game_hash = hash;
  memset(&request, 0, sizeof(request));
  if (rc_api_init_resolve_hash_request(&request, &params) != RC_OK)
    return 0;

  /* Synchronous on purpose: the answer has to exist before the disc is switched, and
     * the transport fails fast when the link is down (`ra_http_link_up()`), which is the
     * case this has to survive. The request outlives the call, so `allow_borrow` is fine. */
  ra_server_call(&request, resolve_hash_callback, &sink, NULL);
  rc_api_destroy_request(&request);

  if (asked != NULL)
        *asked = sink.answered;
  if (sink.answered && game_id != NULL)
        *game_id = sink.game_id;
  return sink.answered && sink.game_id != 0;
}

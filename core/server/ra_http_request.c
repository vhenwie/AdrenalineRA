#include "ra_http_internal.h"
#include "ra_http_diag.h"

#define RA_HTTP_MAX_BODY (4 * 1024 * 1024)

#include "ra_arena.h"
#include "ra_log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void http_body_diag_capture(const char *body, size_t length) {
  size_t i;

  g_http_last_body_length = length; {
    size_t n = length < sizeof(g_http_last_body_prefix) - 1
            ? length : sizeof(g_http_last_body_prefix) - 1;
    for (i = 0; i < n; ++i) {
      unsigned char c = (unsigned char)body[i];
      g_http_last_body_prefix[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    g_http_last_body_prefix[n] = '\0';
  }
  if (length > 0) {
    size_t start = length > sizeof(g_http_last_body_tail) - 1
            ? length - (sizeof(g_http_last_body_tail) - 1) : 0;
    size_t j = 0;
    for (; start + j < length; ++j) {
      unsigned char c = (unsigned char)body[start + j];
      g_http_last_body_tail[j] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    g_http_last_body_tail[j] = '\0';
  }
}

/* Returns 1 when the buffer can take another read, 0 when it cannot grow, and -1
 * after freeing the body when the allocation failed. */
static int http_body_grow(char **body, size_t *capacity) {
  size_t new_capacity = ra_http_body_growth(*capacity);
  char *new_body;

  if (new_capacity > RA_HTTP_MAX_BODY)
    new_capacity = RA_HTTP_MAX_BODY;
  if (new_capacity <= *capacity) {
    ra_arena_free(*body);
    *body = NULL;
    return -1;
  }
  new_body = (char*)ra_arena_realloc(*body, new_capacity + 1);
  if (new_body == NULL) {
    ra_arena_free(*body);
    return -1;
  }
  *body = new_body;
  *capacity = new_capacity;
  return 1;
}

static int http_body_publish(ra_http_response_t *response, char *body,
    size_t length) {
  if (body == NULL) {
    body = (char*)ra_arena_malloc(1);
    if (body == NULL)
      return -1;
  }
  body[length] = '\0';
  response->body = body;
  response->body_length = length;
  response->body_owned = 1;
  http_body_diag_capture(body, length);
  return 0;
}

static int read_response_body(int request, ra_http_response_t *response) {
  unsigned long long content_length = 0;
  char *body = NULL;
  size_t capacity = 0;
  size_t length = 0;
  int got;

  content_length = 0;
  (void)sceHttpGetResponseContentLength(request, &content_length);
  g_http_last_content_length = (size_t)content_length;
  if (content_length > RA_HTTP_MAX_BODY)
    return -1;

  if (content_length > 0) {
    capacity = (size_t)content_length;
    body = (char*)ra_arena_malloc(capacity + 1);
    if (body == NULL)
      return -1;
  }

  for (;;) {
    if (content_length > 0 && length >= (size_t)content_length)
      break;

    if (length + 4096 + 1 > capacity) {
      int grown = http_body_grow(&body, &capacity);

      if (grown == 0)
        break;
      if (grown < 0)
        return -1;
    }

    got = sceHttpReadData(request, body + length,
              (unsigned int)(capacity - length));
    if (got < 0) {
      ra_arena_free(body);
      return got;
    }
    if (got == 0)
      break;
    length += (size_t)got;
  }

  if (content_length > 0 && length != (size_t)content_length) {
    /* A short read must never be handed to rcheevos as a complete body: the
         * JSON parser then reports whichever required field the truncated buffer
         * happens to stop before (for achievements that field is MemAddr), which
         * is indistinguishable from a malformed server payload. */
    ra_arena_free(body);
    return -1;
  }

  return http_body_publish(response, body, length);
}

static void http_request_diag_reset(const char *url) {
  g_http_last_stage = 10;
  g_http_last_error = 0;
  g_http_last_status = 0;
  g_http_last_body_length = 0;
  g_http_last_content_length = 0;
  g_http_last_body_prefix[0] = '\0';
  g_http_last_body_tail[0] = '\0';
  snprintf(g_http_last_url, sizeof(g_http_last_url), "%s", url);
  g_http_last_request[0] = '\0';
  g_http_last_request_body[0] = '\0';
}

static void http_capture_request(const char *post_data) {
  const char *r = strstr(post_data, "r=");

  if (r != NULL) {
    size_t i = 0;

    r += 2;
    while (r[i] != '\0' && r[i] != '&' &&
      i < sizeof(g_http_last_request) - 1) {
      g_http_last_request[i] = r[i];
      ++i;
    }
    g_http_last_request[i] = '\0';
  }
  ra_http_mask_request_body(post_data, g_http_last_request_body,
            sizeof(g_http_last_request_body));
}

static void http_capture_ssl_error(int request) {
  int ssl_error = -1;
  unsigned int ssl_detail = 0;

  if (sceHttpsGetSslError(request, &ssl_error, &ssl_detail) == 0) {
    g_http_ssl_error = ssl_error;
    g_http_ssl_detail = ssl_detail;
  }
}

static int http_send_and_status(int request, const char *post_data,
    size_t post_length, int *status) {
  int result = sceHttpSendRequest(request, post_data,
      (unsigned int)post_length);

  if (result < 0) {
    g_http_last_error = result;
    http_capture_ssl_error(request);
    /* The handshake itself failed (0x80431075 in the player logs). Reset the
         * stack before the next request instead of leaving every later one to die at
         * connection creation - one bad handshake poisoned the whole session until the
         * app was restarted. */
    g_http_needs_reset = 1;
  } else {
    g_http_last_stage = 13;
    result = sceHttpGetStatusCode(request, status);
    if (result < 0)
      g_http_last_error = result;
  }
  return result;
}

static int http_finish_response(int request, int status, int result,
    ra_http_response_t *response) {
  if (result >= 0 && status > 0) {
    g_http_last_stage = 14;
    g_http_last_status = status;
    response->status_code = status;
    result = read_response_body(request, response);
    if (result < 0) {
      g_http_last_error = result;
      ra_http_response_free(response);
      return -1;
    }
    ra_http_copy_response_header(request, "ETag", g_http_last_etag,
              sizeof(g_http_last_etag));
    ra_http_copy_response_header(request, "Last-Modified",
              g_http_last_modified,
              sizeof(g_http_last_modified));
    return 0;
  }
  response->status_code = status;
  return -1;
}

static int http_open_request(int template_id, int method, const char *url,
    size_t post_length, int *connection, int *request, int *failure) {
  *connection = sceHttpCreateConnectionWithURL(template_id, url, 1);
  if (*connection < 0) {
    g_http_last_error = *connection;
    /* 0x80431001 here means the stack itself is unusable (it follows a failed
           * handshake): mark it for a full reset on the next request. */
    g_http_needs_reset = 1;
    *failure = *connection;
    return 0;
  }

  g_http_last_stage = 11;
  *request = sceHttpCreateRequestWithURL(*connection, method, url,
                    (unsigned long long)post_length);
  if (*request < 0) {
    g_http_last_error = *request;
    g_http_needs_reset = 1;
    sceHttpDeleteConnection(*connection);
    *failure = *request;
    return 0;
  }
  return 1;
}

static void http_add_headers(int request, int method, const char *content_type,
    const char *accept) {
  if (method == SCE_HTTP_METHOD_POST) {
    sceHttpAddRequestHeader(request, "Content-Type", content_type,
                SCE_HTTP_HEADER_OVERWRITE);
  }
  /* RA returns very large game-set payloads. Ask for an unencoded body so the
     * synchronous reader always hands JSON to rcheevos. PNGs are already
     * compressed, so identity is still the smallest transfer. */
  sceHttpAddRequestHeader(request, "Accept-Encoding", "identity",
              SCE_HTTP_HEADER_OVERWRITE);
  sceHttpAddRequestHeader(request, "Accept", accept,
              SCE_HTTP_HEADER_OVERWRITE);
}

static int ra_http_request_locked(int template_id, int method, const char *url,
                const char *post_data, size_t post_length,
                const char *content_type, const char *accept,
                ra_http_response_t *response) {
  int connection;
  int request;
  int status = 0;
  int result;
  int failure = -1;

  if (response == NULL)
    return -1;
  memset(response, 0, sizeof(*response));
  if (url == NULL || template_id < 0)
    return -1;
  if (post_data == NULL)
    post_length = 0;
  if (content_type == NULL)
    content_type = "application/x-www-form-urlencoded";
  if (accept == NULL)
    accept = "application/json";

  http_request_diag_reset(url);

  /* No link: fail before touching a socket. The offline contract lets the player
     * wait for the connection to come back, and every attempt during that wait would
     * otherwise burn the 30 s resolve/connect/recv timeouts on the worker thread.
     * rcheevos reads a bare failure (status 0 -> RETRYABLE_CLIENT_ERROR in
     * ra_server_call) as a transport error and keeps the pending award, so the
     * unlock still syncs - with its original unlock time - once the link returns. */
  if (!ra_http_link_up()) {
    g_http_last_stage = 11;
    g_http_last_error = -1;
    return 0;
  }
  if (post_data != NULL)
    http_capture_request(post_data);

  if (!http_open_request(template_id, method, url, post_length, &connection,
        &request, &failure))
    return failure;
  http_add_headers(request, method, content_type, accept);
  g_http_last_stage = 12;
  result = http_send_and_status(request, post_data, post_length, &status);

  result = http_finish_response(request, status, result, response);

  sceHttpDeleteRequest(request);
  sceHttpDeleteConnection(connection);
  return result;
}

#define RA_HTTP_MIN_GAP_US 40000ULL

/* Set while the runtime thread wants the gate for an API call. The badge worker checks it
 * before taking the gate so a background download never parks an award behind a badge. */
static volatile int g_api_wanted;
static unsigned long long g_gate_last_us;

void ra_http_gate_api_wanted(int wanted) { g_api_wanted = wanted != 0; }
int ra_http_gate_wanted(void) { return g_api_wanted; }

void ra_http_gate_lock(void) {
  unsigned long long now;
  unsigned long long waited = 0;

  if (g_http_gate < 0)
    g_http_gate = sceKernelCreateMutex("ra_http_gate", 0, 0, NULL);
  if (g_http_gate >= 0) {
    now = (unsigned long long)sceKernelGetSystemTimeWide();
    sceKernelLockMutex(g_http_gate, 1, NULL);
    waited = (unsigned long long)sceKernelGetSystemTimeWide() - now;
    if (waited > 250000ULL)
      ra_log_append("RA_SETUP", "http: waited for the network gate\n",
            (int)strlen("http: waited for the network gate\n"));
  }

  /* Minimum spacing: back-to-back handshakes are what broke the stack. */
  if (g_gate_last_us != 0) {
    now = (unsigned long long)sceKernelGetSystemTimeWide();
    if (now >= g_gate_last_us &&
      now - g_gate_last_us < RA_HTTP_MIN_GAP_US)
      sceKernelDelayThread(
        (SceUInt)(RA_HTTP_MIN_GAP_US - (now - g_gate_last_us)));
  }
}

void ra_http_gate_unlock(void) {
  g_gate_last_us = (unsigned long long)sceKernelGetSystemTimeWide();
  if (g_http_gate >= 0)
    sceKernelUnlockMutex(g_http_gate, 1);
}

/* Serialize every RA request across both templates with a small gap. */
int ra_http_post(const char *url, const char *post_data, size_t post_length,
        const char *content_type, ra_http_response_t *response) {
  int result;

  if (response != NULL)
    memset(response, 0, sizeof(*response));

  ra_http_gate_api_wanted(1);
  ra_http_gate_lock();
  /* Init and the failed-handshake reset run under the same gate as the request,
   * so a reset can never tear down SceHttp/SSL while another request is live. */
  if (ra_http_init_locked() != 0) {
    ra_http_gate_unlock();
    ra_http_gate_api_wanted(0);
    return -1;
  }
  http_lock();
  result = ra_http_request_locked(g_http_template, SCE_HTTP_METHOD_POST, url,
                  post_data, post_length, content_type,
                  "application/json", response);
  http_unlock();
  ra_http_gate_unlock();
  ra_http_gate_api_wanted(0);
  return result;
}

int ra_http_get(const char *url, const char *accept,
        ra_http_response_t *response) {
  int result;

  if (response != NULL)
    memset(response, 0, sizeof(*response));

  /* Background download: let a queued API call go first, but never starve for longer
     * than ~2 s if the network is dead (the API times out on its own). */ {
    int spins = 0;
    while (ra_http_gate_wanted() && spins < 200) {
      sceKernelDelayThread(10000);
      spins++;
    }
  }
  ra_http_gate_lock();
  if (ra_http_init_locked() != 0) {
    ra_http_gate_unlock();
    return -1;
  }
  http_image_lock();
  if (g_http_image_template < 0) {
    g_http_image_active = 0;
    http_image_unlock();
    ra_http_gate_unlock();
    return -1;
  }

  g_http_image_active = 1;
  /* No warm connection. Reusing one SceHttp connection across badge GETs was
     * the only long-lived HTTP object we kept, and every badge fetch on hardware
     * came back as `status=0` while the JSON API over the same TLS stack worked.
     * A fresh connection per image costs one handshake on a one-time download and
     * removes the state that outlives a failed request. */
  result = ra_http_request_locked(g_http_image_template, SCE_HTTP_METHOD_GET,
                  url, NULL, 0, NULL,
                  accept != NULL ? accept
                        : "image/png,image/*;q=0.8",
                  response);
  g_http_image_active = 0;
  http_image_unlock();
  ra_http_gate_unlock();
  return result;
}

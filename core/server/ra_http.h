#ifndef RA_HTTP_H
#define RA_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int status_code;
  char *body;
  size_t body_length;
  int body_owned;
} ra_http_response_t;

/* Response-body growth policy; chunked responses start from a fixed chunk. */
#define RA_HTTP_BODY_FIRST_CHUNK 16384u

static inline size_t ra_http_body_growth(size_t capacity) {
  if (capacity == 0)
    return (size_t)RA_HTTP_BODY_FIRST_CHUNK;
  if (capacity > ((size_t)-1) / 2u)
    return capacity;
  return capacity * 2u;
}

/* Synchronous HTTP/HTTPS POST. On Vita this uses SceHttp with the system
 * certificate store and hostname verification. */
int ra_http_post(const char *url, const char *post_data, size_t post_length,
        const char *content_type, ra_http_response_t *response);

/* 1 when the console currently has an internet connection, 0 when it does not.
 * Costs nothing (netctl state, no packets) and is the gate for the offline policy:
 * a session is only ever started when the network was available at launch. */
int ra_http_link_up(void);

/* Synchronous binary GET for cached resources such as achievement badges.
 * Uses a separate SceHttp template and lock so a badge download cannot block
 * an rc_client API POST. */
int ra_http_get(const char *url, const char *accept,
        ra_http_response_t *response);

void ra_http_response_free(ra_http_response_t *response);

int ra_http_init(void);
void ra_http_shutdown(void);

/* Production uses one client identity. These setters are retained for the
 * shared API and are no-ops in the production build. */
void ra_http_set_user_agent(const char *user_agent);
const char *ra_http_get_user_agent(void);
void ra_http_reset_user_agent(int is_ps1);

void ra_http_set_console_identity(int is_ps1, const char *override_ua);

/* Keeps the network/HTTP stack alive while the in-game runtime is using it.
 * Menu worker shutdowns become no-ops until the session ends. */
void ra_http_set_session_active(int active);
int ra_http_get_last_stage(void);
int ra_http_get_last_error(void);
int ra_http_get_last_status(void);
int ra_http_get_last_net_state(void);
int ra_http_get_last_ssl_error(void);
unsigned int ra_http_get_last_ssl_detail(void);
size_t ra_http_get_last_body_length(void);
/* Content-Length the server reported (0 when the response is chunked). */
size_t ra_http_get_last_content_length(void);

/* Last value returned by the heap probe, in KiB (0 when not probed). */
unsigned int ra_http_get_heap_probe_kib(void);
/* Largest allocatable block, recorded for the next failure line. */
void ra_http_probe_heap(void);
const char *ra_http_get_last_body_prefix(void);
const char *ra_http_get_last_body_tail(void);
const char *ra_http_get_last_url(void);
const char *ra_http_get_last_request(void);
/* POST body of the last request with the token masked. */
const char *ra_http_get_last_request_body(void);
/* Response validators, if the server sent them (empty when absent). */
const char *ra_http_get_last_etag(void);
const char *ra_http_get_last_modified(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_HTTP_H */

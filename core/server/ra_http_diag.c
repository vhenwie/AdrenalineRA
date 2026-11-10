#include "ra_http_diag.h"

#include "ra_arena.h"
#include "ra_http_internal.h"
#include <string.h>
#include <psp2/net/http.h>

static unsigned int g_heap_probe_kib;

void ra_http_probe_heap(void) {
  g_heap_probe_kib = (unsigned int)ra_arena_system_probe_kib();
}

unsigned int ra_http_get_heap_probe_kib(void) {
  return g_heap_probe_kib;
}

void ra_http_copy_response_header(int request, const char *field, char *out,
                size_t capacity) {
  char *headers = NULL;
  unsigned int header_size = 0;
  const char *value = NULL;
  unsigned int value_size = 0;

  out[0] = '\0';
  if (capacity == 0)
    return;
  if (sceHttpGetAllResponseHeaders(request, &headers, &header_size) < 0 ||
    headers == NULL)
    return;
  if (sceHttpParseResponseHeader(headers, header_size, field, &value,
                &value_size) < 0 ||
    value == NULL)
    return;
  if (value_size >= capacity)
    value_size = (unsigned int)capacity - 1;
  memcpy(out, value, value_size);
  out[value_size] = '\0';
}

void ra_http_mask_request_body(const char *post_data, char *out, size_t capacity) {
  size_t i = 0;
  size_t o = 0;

  if (out == NULL || capacity == 0)
    return;
  out[0] = '\0';
  if (post_data == NULL)
    return;
  while (post_data[i] != '\0' && o + 8 < capacity) {
    if ((i == 0 || post_data[i - 1] == '&') &&
      (post_data[i] == 't' || post_data[i] == 'p') &&
      post_data[i + 1] == '=') {
      out[o++] = post_data[i];
      out[o++] = '=';
      out[o++] = '*';
      i += 2;
      while (post_data[i] != '\0' && post_data[i] != '&')
        ++i;
      continue;
    }
    out[o++] = post_data[i++];
  }
  out[o] = '\0';
}

int ra_http_get_last_stage(void) { return g_http_last_stage; }
int ra_http_get_last_error(void) { return g_http_last_error; }
int ra_http_get_last_status(void) { return g_http_last_status; }
int ra_http_get_last_net_state(void) { return g_http_last_net_state; }
int ra_http_get_last_ssl_error(void) { return g_http_ssl_error; }
unsigned int ra_http_get_last_ssl_detail(void) { return g_http_ssl_detail; }
size_t ra_http_get_last_body_length(void) { return g_http_last_body_length; }
size_t ra_http_get_last_content_length(void) { return g_http_last_content_length; }
const char *ra_http_get_last_body_prefix(void) { return g_http_last_body_prefix; }
const char *ra_http_get_last_body_tail(void) { return g_http_last_body_tail; }
const char *ra_http_get_last_url(void) { return g_http_last_url; }
const char *ra_http_get_last_request(void) { return g_http_last_request; }
const char *ra_http_get_last_request_body(void) { return g_http_last_request_body; }
const char *ra_http_get_last_etag(void) { return g_http_last_etag; }
const char *ra_http_get_last_modified(void) { return g_http_last_modified; }


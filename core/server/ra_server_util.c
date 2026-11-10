#include "ra_server_util.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/kernel/processmgr.h>

#include "ra_arena.h"

/* Reads an integer URL parameter. The parameter must start the string or follow
 * an '&', so "s=" cannot match inside another value. */
int ra_server_url_param_int(const char *post, const char *name, int *value) {
  size_t length;
  const char *at;

  if (post == NULL || name == NULL || value == NULL)
    return 0;
  length = strlen(name);
  at = post;
  while (*at != '\0') {
    at = strstr(at, name);
    if (at == NULL)
      return 0;
    if ((at == post || at[-1] == '&') && at[length] == '=') {
      char *end = NULL;
      long parsed = strtol(at + length + 1, &end, 10);
      if (end == at + length + 1)
        return 0;
      if (parsed < -2147483647L - 1L || parsed > 2147483647L)
        return 0;
            *value = (int)parsed;
      return 1;
    }
    at += length;
  }
  return 0;
}

const char *ra_server_filter_rich_presence(const char *input, char *output,
                    size_t capacity) {
  const char *field;
  const char *end;
  size_t prefix;
  size_t used = 0;

  if (ra_server_rich_presence_enabled() || input == NULL || output == NULL ||
    capacity == 0)
    return input;
  field = input;
  if (strncmp(field, "m=", 2) != 0) {
    field = strstr(input, "&m=");
    if (field == NULL)
      return input;
    ++field;
  }
  end = field;
  while (*end != '\0' && *end != '&')
    ++end;
  if (*end == '&')
    ++end;

  prefix = (size_t)(field - input);
  if (prefix > 0)
    --prefix;
  if (prefix > 0) {
    if (prefix + 1 >= capacity)
      return input;
    memcpy(output, input, prefix);
    used = prefix;
    output[used++] = '&';
  }
  if (strlen(end) >= capacity - used)
    return input;
  memcpy(output + used, end, strlen(end) + 1);
  return output;
}

void ra_http_response_free(ra_http_response_t *response) {
  if (response == NULL)
    return;
  if (response->body_owned && response->body != NULL)
    ra_arena_free(response->body);
  memset(response, 0, sizeof(*response));
}


unsigned long long ra_server_now_ms(void) {
#ifdef __vita__
  return (unsigned long long)(sceKernelGetSystemTimeWide() / 1000ULL);
#else
  return (unsigned long long)((clock() * 1000ULL) / (unsigned long long)CLOCKS_PER_SEC);
#endif
}

/* Copies the r=<name> value of a dorequest body, or "?" when there is none. */
void ra_server_request_name(const char *post_data, char *out, size_t capacity) {
  const char *r;
  size_t i = 0;

  if (out == NULL || capacity == 0)
    return;
  out[0] = '\0';
  if (post_data == NULL)
    return;
  r = strstr(post_data, "r=");
  if (r == NULL)
    return;
  r += 2;
  while (r[i] != '\0' && r[i] != '&' && i < capacity - 1) {
    out[i] = r[i];
    ++i;
  }
  out[i] = '\0';
  if (i == 0)
    snprintf(out, capacity, "?");
}


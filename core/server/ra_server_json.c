#include "ra_server_json.h"

#include <string.h>

/* End of the JSON string that starts at the opening quote. */
static char *json_skip_string(char *p, char *end) {
  ++p;
  while (p < end) {
    if (*p == '\\') {
      p += 2;
      continue;
    }
    if (*p == '"')
      return p + 1;
    ++p;
  }
  return end;
}

/* Matching ']' for the '[' at open, honouring nested arrays and strings. */
char *ra_server_json_match_array(char *open, char *end) {
  int depth = 0;
  char *p = open;

  while (p < end) {
    if (*p == '"') {
      p = json_skip_string(p, end);
      continue;
    }
    if (*p == '[') {
      ++depth;
    }
    else if (*p == ']') {
      if (--depth == 0)
        return p;
    }
    ++p;
  }
  return NULL;
}

const char *ra_server_json_skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    ++p;
  return p;
}

/* Returns the pointer just past the JSON value that starts at p, or NULL when the
 * value is malformed. String literals are skipped, so braces inside a title do not
 * confuse the depth scan. */
const char *ra_server_json_skip_value(const char *p, const char *end) {
  int depth = 0;

  if (p >= end)
    return NULL;

  if (*p == '"') {
    ++p;
    while (p < end) {
      if (*p == '\\') {
        p += 2;
        continue;
      }
      if (*p == '"')
        return p + 1;
      ++p;
    }
    return NULL;
  }

  if (*p == '{' || *p == '[') {
    while (p < end) {
      if (*p == '"') {
        const char *next = ra_server_json_skip_value(p, end);
        if (next == NULL)
          return NULL;
        p = next;
        continue;
      }
      if (*p == '{' || *p == '[')
        ++depth;
      else if (*p == '}' || *p == ']') {
        --depth;
        if (depth == 0)
          return p + 1;
      }
      ++p;
    }
    return NULL;
  }

  while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
    ++p;
  return p;
}

/* Finds key inside [from, to), like strstr but bounded. */
const char *ra_server_json_find(const char *from, const char *to, const char *key) {
  size_t key_len = strlen(key);
  const char *p;

  if (to <= from || (size_t)(to - from) < key_len)
    return NULL;
  for (p = from; p + key_len <= to; ++p) {
    if (memcmp(p, key, key_len) == 0)
      return p;
  }
  return NULL;
}

/* `"Key":123` inside [from, to). 1 on success. */
int ra_server_json_read_uint(const char *from, const char *to, const char *key,
            uint32_t *out) {
  const char *at = ra_server_json_find(from, to, key);
  uint32_t value = 0;
  int digits = 0;

  if (at == NULL || out == NULL)
    return 0;
  at = ra_server_json_skip_ws(at + strlen(key), to);
  while (at < to && *at >= '0' && *at <= '9') {
    if (value > 0x0CCCCCCCu)
      return 0; /* absurd length: treat as malformed */
    value = value * 10u + (uint32_t)(*at - '0');
    ++at;
    ++digits;
  }
  if (!digits)
    return 0;
  *out = value;
  return 1;
}

/* `"Key":"text"` inside [from, to). 0 when the value is null or not a string. */
int ra_server_json_read_string(const char *from, const char *to, const char *key,
              char *out, size_t capacity) {
  const char *at = ra_server_json_find(from, to, key);
  size_t written = 0;

  if (at == NULL || out == NULL || capacity == 0)
    return 0;
  at = ra_server_json_skip_ws(at + strlen(key), to);
  if (at >= to || *at != '"')
    return 0;
  ++at;
  while (at < to && *at != '"') {
    char ch = *at;

    if (ch == '\\' && at + 1 < to) {
      ++at;
      ch = *at;
    }
    if (written + 1 < capacity)
      out[written++] = ch;
    ++at;
  }
  out[written] = '\0';
  return 1;
}


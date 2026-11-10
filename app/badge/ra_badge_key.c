/* Filesystem-safe badge cache keys. */

#include "ra_badge_key.h"

#include <string.h>

int ra_badge_key_valid(const char *key) {
  size_t i, length;

  if (key == NULL)
    return 0;
  length = strlen(key);
  if (length == 0 || length >= RA_BADGE_KEY_MAX)
    return 0;
  for (i = 0; i < length; ++i) {
    char c = key[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static void mark_locked(const char *url, char *out) {
  size_t length;

  if (url == NULL || strstr(url, "_lock.") == NULL)
    return;
  length = strlen(out);
  if (length >= 5 && strcmp(out + length - 5, "_lock") == 0)
    return;
  if (length + 5 >= RA_BADGE_KEY_MAX)
    return;
  memmove(out + length, "_lock", 6);
}

static int copy_url_name(const char *url, char *out, size_t capacity) {
  const char *start;
  const char *end;
  size_t length;

  start = strrchr(url, '/');
  start = start != NULL ? start + 1 : url;
  end = start;
  while (*end != '\0' && *end != '?' && *end != '#')
    ++end;
  if (end == start)
    return 0;
  if (end - start > 4 && strncmp(end - 4, ".png", 4) == 0)
    end -= 4;
  length = (size_t)(end - start);
  if (length == 0 || length >= capacity || length >= RA_BADGE_KEY_MAX)
    return 0;
  memmove(out, start, length);
  out[length] = '\0';
  return ra_badge_key_valid(out);
}

int ra_badge_make_key(const char *badge_name, const char *url, char *out,
                      size_t capacity) {
  size_t length;

  if (out == NULL || capacity == 0)
    return 0;

  if (badge_name == NULL || !ra_badge_key_valid(badge_name)) {
    if (url == NULL)
      return 0;
    return copy_url_name(url, out, capacity);
  }

  length = strlen(badge_name);
  if (length >= capacity)
    return 0;
  memmove(out, badge_name, length + 1);
  mark_locked(url, out);
  return ra_badge_key_valid(out);
}

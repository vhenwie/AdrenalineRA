#include "ra_server_payload.h"

#include <stdlib.h>
#include <string.h>

#include "ra_server.h"
#include "ra_server_json.h"

static size_t g_last_trim_bytes;

size_t ra_server_last_trim_bytes(void) { return g_last_trim_bytes; }
void ra_server_reset_trim_bytes(void) { g_last_trim_bytes = 0; }

size_t ra_server_trim_payload(char *body, size_t length, unsigned int set_id) {
  static const char achievements_key[] = "\"Achievements\":[";
  static const char set_id_key[] = "\"AchievementSetId\":";
  char *end;
  char *cursor;
  unsigned int set_index = 0;

  if (body == NULL || length == 0 || set_id == RA_SERVER_NO_TRIM)
    return length;

  end = body + length;
  cursor = body;
  while (cursor < end) {
    char *ach = strstr(cursor, achievements_key);
    char *id = strstr(cursor, set_id_key);
    char *open;
    char *close;
    unsigned int id_value = 0;
    int have_id = 0;
    int keep;

    if (ach == NULL || ach >= end)
      break;

    if (id != NULL && id < ach) {
      id_value = (unsigned int)strtoul(id + strlen(set_id_key), NULL, 10);
      have_id = 1;
    }

    open = ach + strlen("\"Achievements\":");
    close = ra_server_json_match_array(open, end);
    if (close == NULL)
      break; /* malformed: leave the payload untouched */

    keep = (set_id == RA_SERVER_KEEP_FIRST_SET)
        ? (set_index == 0)
        : (have_id && id_value == set_id);

    if (keep) {
      cursor = close + 1;
    }
    else {
      size_t removed = (size_t)(close - (open + 1));
      memmove(open + 1, close, (size_t)(end - close));
      end -= removed;
      g_last_trim_bytes += removed;
      cursor = open + 1;
    }
    ++set_index;
  }

    *end = '\0';
  return (size_t)(end - body);
}

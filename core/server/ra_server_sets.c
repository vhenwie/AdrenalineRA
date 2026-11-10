#include "ra_server.h"

#include <stdlib.h>
#include <string.h>

#include "ra_server_json.h"

#define RA_SERVER_MAX_MERGE_SETS 8
#define RA_MERGE_OK 0
#define RA_MERGE_NO_BODY 1
#define RA_MERGE_NO_SETS_KEY 2
#define RA_MERGE_NO_ARRAY 3
#define RA_MERGE_BAD_SET 4
#define RA_MERGE_NO_ACH 5
#define RA_MERGE_BAD_TAIL 6
#define RA_MERGE_ONE_SET 7
#define RA_MERGE_INDEX_FAIL 8
#define RA_MERGE_ALLOC_FAIL 9

static long g_last_merge_delta;
static int g_last_payload_sets;
static int g_last_merge_reason;
static size_t g_last_sets_bytes;

void ra_server_sets_set_body_bytes(size_t bytes) { g_last_sets_bytes = bytes; }
void ra_server_sets_set_merge_delta(long delta) { g_last_merge_delta = delta; }

static RaServerSetInfo g_sets_info[RA_SERVER_MAX_SETS];
static int g_sets_info_count;
typedef struct {
  uint32_t achievement_id;
  uint32_t set_id;
} RaServerSetEntry;
static RaServerSetEntry g_set_entries[RA_SERVER_SET_INDEX_CAPACITY];
static int g_set_entry_count;

int ra_server_set_count(void) {
  return g_sets_info_count;
}

int ra_server_set_at(int index, RaServerSetInfo *out) {
  if (out == NULL || index < 0 || index >= g_sets_info_count)
    return 0;
  *out = g_sets_info[index];
  return 1;
}

uint32_t ra_server_achievement_set(uint32_t achievement_id) {
  int i;

  if (achievement_id == 0)
    return 0;
  for (i = 0; i < g_set_entry_count; ++i) {
    if (g_set_entries[i].achievement_id == achievement_id)
      return g_set_entries[i].set_id;
  }
  return 0;
}

/* Human-readable name for RA_SERVER_SET_TYPE_*, used by the runtime log and the
 * picker. RA's docs call these Base / Bonus / Challenge / Specialty / Exclusive;
 * the core set keeps the name the menu has always shown ("Core"). */
const char *ra_server_set_type_name(unsigned int type) {
  switch (type) {
  case RA_SERVER_SET_TYPE_BONUS:
    return "Bonus";
  case RA_SERVER_SET_TYPE_CHALLENGE:
    return "Challenge";
  case RA_SERVER_SET_TYPE_SPECIALTY:
    return "Specialty";
  case RA_SERVER_SET_TYPE_EXCLUSIVE:
    return "Exclusive";
  case RA_SERVER_SET_TYPE_CORE:
  default:
    return "Core";
  }
}

static void set_index_reset(void) {
  g_sets_info_count = 0;
  g_set_entry_count = 0;
}

static uint8_t json_set_type(const char *from, const char *to) {
  char text[16];

  if (!ra_server_json_read_string(from, to, "\"Type\":", text, sizeof(text)))
    return RA_SERVER_SET_TYPE_CORE;
  if (strcmp(text, "bonus") == 0)
    return RA_SERVER_SET_TYPE_BONUS;
  if (strcmp(text, "challenge") == 0)
    return RA_SERVER_SET_TYPE_CHALLENGE;
  if (strcmp(text, "specialty") == 0)
    return RA_SERVER_SET_TYPE_SPECIALTY;
  if (strcmp(text, "exclusive") == 0)
    return RA_SERVER_SET_TYPE_EXCLUSIVE;
  if (strcmp(text, "core") == 0)
    return RA_SERVER_SET_TYPE_CORE;
  /* An unknown type is still a set with rows. rcheevos resolves it to BONUS
     * (rc_api_runtime.c:461-462) rather than dropping it, and so does this: typing
     * it CORE would hide it from the picker, which skips the core set. */
  return RA_SERVER_SET_TYPE_BONUS;
}

/* Appends every `"ID":n` object found in [open+1, close). Returns 0 when the array
 * does not fit the index, which makes the caller drop the whole index. */
static int set_index_add_achievements(const char *open, const char *close,
                  uint32_t set_id, uint32_t *count) {
  const char *p = ra_server_json_skip_ws(open + 1, close);
  uint32_t added = 0;

  if (count != NULL)
        *count = 0;
  while (p < close && *p == '{') {
    const char *next = ra_server_json_skip_value(p, close);
    uint32_t id = 0;

    if (next == NULL)
      return 0;
    if (ra_server_json_read_uint(p, next, "\"ID\":", &id)) {
      if (g_set_entry_count >= RA_SERVER_SET_INDEX_CAPACITY)
        return 0;
      g_set_entries[g_set_entry_count].achievement_id = id;
      g_set_entries[g_set_entry_count].set_id = set_id;
      ++g_set_entry_count;
      ++added;
    }
    p = ra_server_json_skip_ws(next, close);
    if (p < close && *p == ',')
      p = ra_server_json_skip_ws(p + 1, close);
  }
  if (count != NULL)
        *count = added;
  return 1;
}

#define RA_MERGE_OK 0
#define RA_MERGE_NO_BODY 1
#define RA_MERGE_NO_SETS_KEY 2
#define RA_MERGE_NO_ARRAY 3
#define RA_MERGE_BAD_SET 4
#define RA_MERGE_NO_ACH 5
#define RA_MERGE_BAD_TAIL 6
#define RA_MERGE_ONE_SET 7
#define RA_MERGE_INDEX_FAIL 8
#define RA_MERGE_ALLOC_FAIL 9

typedef struct {
  const char *element_starts[RA_SERVER_MAX_MERGE_SETS];
  const char *element_ends[RA_SERVER_MAX_MERGE_SETS];
  const char *ach_open[RA_SERVER_MAX_MERGE_SETS];
  const char *ach_close[RA_SERVER_MAX_MERGE_SETS];
  size_t sets;
  const char *sets_close;
} RaMergeWalk;

static int merge_find_achievements_array(const char *p,
    const char *element_end, const char **open, const char **close) {
  const char *ach = ra_server_json_find(p, element_end, "\"Achievements\":");

  *open = (ach != NULL)
      ? ra_server_json_skip_ws(ach + strlen("\"Achievements\":"), element_end)
      : NULL;
  *close = (*open != NULL && *open < element_end && (*open)[0] == '[')
        ? ra_server_json_match_array((char*)*open, (char*)element_end)
        : NULL;
  return *close != NULL;
}

static int merge_walk_sets(const char *body, const char *end,
    RaMergeWalk *walk) {
  const char *sets_key;
  const char *array;
  const char *p;

  g_last_merge_reason = RA_MERGE_NO_SETS_KEY;
  sets_key = ra_server_json_find(body, end, "\"Sets\"");
  if (sets_key == NULL)
    return 0;
  array = sets_key;
  while (array < end && *array != '[')
    ++array;
  g_last_merge_reason = RA_MERGE_NO_ARRAY;
  if (array >= end)
    return 0;

  p = ra_server_json_skip_ws(array + 1, end);
  while (p < end && *p == '{' && walk->sets < RA_SERVER_MAX_MERGE_SETS) {
    const char *element_end = ra_server_json_skip_value(p, end);
    const char *open;
    const char *close;

    g_last_merge_reason = RA_MERGE_BAD_SET;
    if (element_end == NULL)
      return 0;
    g_last_merge_reason = RA_MERGE_NO_ACH;
    if (!merge_find_achievements_array(p, element_end, &open, &close))
      return 0; /* malformed: leave the payload as fetched */

    walk->element_starts[walk->sets] = p;
    walk->element_ends[walk->sets] = element_end;
    walk->ach_open[walk->sets] = open;
    walk->ach_close[walk->sets] = close;
    ++walk->sets;
    /* Recorded as the walk progresses: a payload that fails halfway still
         * reports how many set objects were seen, which is the useful half of the
         * answer ("the server sent two, the parse gave up" vs "one"). */
    g_last_payload_sets = (int)walk->sets;

    p = ra_server_json_skip_ws(element_end, end);
    if (p < end && *p == ',')
      p = ra_server_json_skip_ws(p + 1, end);
  }
  g_last_merge_reason = RA_MERGE_BAD_TAIL;
  if (p >= end || *p != ']')
    return 0;
  walk->sets_close = p;

  g_last_merge_reason = RA_MERGE_ONE_SET;
  if (walk->sets < 2)
    return 0; /* one set already: nothing to merge */
  return 1;
}

/* Index the sets before the body is rebuilt: the set objects (and the ids they
 * own) do not survive the merge, and the menu needs them to offer a set list. */
static void merge_index_sets(const RaMergeWalk *walk) {
  size_t s;
  int indexed = 1;

  for (s = 0; s < walk->sets; ++s) {
    const char *fields_end = walk->element_ends[s];
    const char *ach_key = ra_server_json_find(walk->element_starts[s],
                    fields_end, "\"Achievements\":");
    RaServerSetInfo *info = &g_sets_info[s];

    memset(info, 0, sizeof(*info));
    if (ach_key != NULL)
      fields_end = ach_key; /* the set's own keys precede its arrays */

    if (!ra_server_json_read_uint(walk->element_starts[s], fields_end,
              "\"AchievementSetId\":", &info->id) ||
      info->id == 0) {
      indexed = 0;
      break;
    }
    ra_server_json_read_string(walk->element_starts[s], fields_end,
            "\"Title\":", info->title, sizeof(info->title));
    info->type = json_set_type(walk->element_starts[s], fields_end);
    if (!set_index_add_achievements(walk->ach_open[s], walk->ach_close[s],
                    info->id, &info->achievements)) {
      indexed = 0;
      break;
    }
  }

  if (indexed) {
    g_sets_info_count = (int)walk->sets;
  } else {
    /* A partial map would filter the menu wrongly; drop it and stay with a
           * single list, exactly as before this index existed. */
    set_index_reset();
    g_last_merge_reason = RA_MERGE_INDEX_FAIL;
  }
}

static char *merge_build_body(const char *body, const char *end, size_t length,
    const RaMergeWalk *walk, size_t *out_length) {
  char *out;
  size_t total;
  size_t s;
  int wrote;

  out = (char*)malloc(length + 8);
  if (out == NULL) {
    g_last_merge_reason = RA_MERGE_ALLOC_FAIL;
    return NULL;
  }

  /* Everything up to and including the first set's "Achievements":[ ... */
  total = (size_t)((walk->ach_open[0] + 1) - body);
  memcpy(out, body, total);

  /* ... then every set's rows, comma separated. */
  wrote = 0;
  for (s = 0; s < walk->sets; ++s) {
    size_t inner = (size_t)(walk->ach_close[s] - (walk->ach_open[s] + 1));

    if (inner == 0)
      continue;
    if (wrote)
      out[total++] = ',';
    memcpy(out + total, walk->ach_open[s] + 1, inner);
    total += inner;
    wrote = 1;
  }

  /* Close the first set's array, keep the rest of that object, close the Sets
     * array, then the tail of the payload. The other set objects are dropped, so
     * rcheevos only ever builds one subset. */
  out[total++] = ']';
  memcpy(out + total, walk->ach_close[0] + 1,
    (size_t)(walk->element_ends[0] - (walk->ach_close[0] + 1)));
  total += (size_t)(walk->element_ends[0] - (walk->ach_close[0] + 1));
  out[total++] = ']';
  memcpy(out + total, walk->sets_close + 1,
    (size_t)(end - (walk->sets_close + 1)));
  total += (size_t)(end - (walk->sets_close + 1));
  out[total] = '\0';

  g_last_merge_reason = RA_MERGE_OK;
  if (out_length != NULL)
        *out_length = total;
  return out;
}

char *ra_server_merge_sets(const char *body, size_t length, size_t *out_length) {
  const char *end = body + length;
  RaMergeWalk walk;

  memset(&walk, 0, sizeof(walk));
  if (out_length != NULL)
        *out_length = 0;
  set_index_reset();
  g_last_merge_reason = RA_MERGE_NO_BODY;
  if (body == NULL || length == 0)
    return NULL;

  if (!merge_walk_sets(body, end, &walk)) {
    /* One set is not merged, but its id and type still have to be indexed: a
     * base-only or bonus-only payload otherwise looks like an anonymous set. */
    if (g_last_merge_reason == RA_MERGE_ONE_SET)
      merge_index_sets(&walk);
    return NULL;
  }

  merge_index_sets(&walk);

  return merge_build_body(body, end, length, &walk, out_length);
}

long ra_server_last_merge_delta(void) { return g_last_merge_delta; }
int ra_server_last_payload_sets(void) { return g_last_payload_sets; }
int ra_server_last_merge_reason(void) { return g_last_merge_reason; }
size_t ra_server_last_sets_bytes(void) { return g_last_sets_bytes; }

void ra_server_reset_sets_counters(void) {
  g_last_merge_delta = 0;
  g_last_payload_sets = 0;
  g_last_merge_reason = 0;
  g_last_sets_bytes = 0;
  set_index_reset();
}


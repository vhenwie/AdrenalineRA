/* Durable offline submission queue. */

#include "ra_offline.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_gate.h"
#include "ra_http.h"
#include "ra_vault.h"

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#endif

#define LINE_MAX (64 + 1 + RA_OFFLINE_MAX_URL + 1 + RA_OFFLINE_MAX_BODY + 2)
#define FILE_MAX (RA_OFFLINE_MAX_ENTRIES * (LINE_MAX + 1))

static char g_file_text[FILE_MAX];
static volatile int g_queue_pending;
static int g_queue_pending_known;
static char g_kept_text[FILE_MAX];
static char g_line[LINE_MAX + 1];
static ra_offline_credentials_t g_credentials_provider;

#ifdef __vita__
static SceUID g_offline_lock = -1;
static void offline_lock(void) {
  if (g_offline_lock < 0)
    g_offline_lock = sceKernelCreateSema("ra_offline", 0, 1, 1, NULL);
  if (g_offline_lock >= 0)
    sceKernelWaitSema(g_offline_lock, 1, NULL);
}
static void offline_unlock(void) {
  if (g_offline_lock >= 0)
    sceKernelSignalSema(g_offline_lock, 1);
}
#else
static void offline_lock(void) {}
static void offline_unlock(void) {}
#endif

void ra_offline_set_credentials_provider(ra_offline_credentials_t provider) {
  g_credentials_provider = provider;
}

static void offline_hex(const uint8_t *data, size_t length, char *out,
            size_t capacity) {
  static const char digits[] = "0123456789abcdef";
  size_t i;

  if (capacity == 0)
    return;
  for (i = 0; i < length && (i * 2u) + 2u <= capacity; ++i) {
    out[i * 2u] = digits[(data[i] >> 4) & 0x0F];
    out[(i * 2u) + 1u] = digits[data[i] & 0x0F];
  }
  out[(i * 2u) < capacity ? i * 2u : capacity - 1u] = '\0';
}

static void offline_mac(const char *url, const char *post_data, char *out,
            size_t capacity) {
  uint8_t mac[32];
  char payload[RA_OFFLINE_MAX_URL + 1 + RA_OFFLINE_MAX_BODY + 2];
  int length;

  length = snprintf(payload, sizeof(payload), "%s\t%s", url, post_data);
  if (length < 0 || (size_t)length >= sizeof(payload)) {
    if (capacity > 0)
      out[0] = '\0';
    return;
  }
  ra_hmac_sha256(RA_GATE_KEY, sizeof(RA_GATE_KEY), (const uint8_t*)payload,
        (size_t)length, mac);
  offline_hex(mac, sizeof(mac), out, capacity);
}

static int offline_open_read(void) {
#ifdef __vita__
  SceUID fd;
  int length;

  fd = sceIoOpen(RA_OFFLINE_PATH_UR0, SCE_O_RDONLY, 0);
  if (fd < 0)
    fd = sceIoOpen(RA_OFFLINE_PATH_UX0, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;
  length = (int)sceIoRead(fd, g_file_text, sizeof(g_file_text) - 1);
  sceIoClose(fd);
  if (length <= 0)
    return 0;
  g_file_text[length] = '\0';
  return 1;
#else
  return 0;
#endif
}

static int offline_write(const char *text, size_t length) {
#ifdef __vita__
  SceUID fd;
  int written;

  if (length == 0) {
    sceIoRemove(RA_OFFLINE_PATH_UR0);
    sceIoRemove(RA_OFFLINE_PATH_UX0);
    return 1;
  }
  fd = sceIoOpen(RA_OFFLINE_PATH_UR0, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        0777);
  if (fd < 0)
    fd = sceIoOpen(RA_OFFLINE_PATH_UX0,
          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return 0;
  written = (int)sceIoWrite(fd, text, length);
  sceIoClose(fd);
  return written == (int)length;
#else
  (void)text;
  (void)length;
  return 0;
#endif
}

static int offline_count(const char *text) {
  int count = 0;

  for (; text != NULL && *text != '\0'; ++text) {
    if (*text == '\n')
      ++count;
  }
  return count;
}

static int offline_depth_impl(void) {
  if (!offline_open_read()) {
    g_queue_pending = 0;
    g_queue_pending_known = 1;
    return 0;
  }
  g_queue_pending = offline_count(g_file_text);
  g_queue_pending_known = 1;
  return g_queue_pending;
}

int ra_offline_queue_depth(void) {
  int depth;

  offline_lock();
  depth = offline_depth_impl();
  offline_unlock();
  return depth;
}

int ra_offline_queue_pending(void) {
  if (!g_queue_pending_known)
    (void)ra_offline_queue_depth();
  return g_queue_pending;
}

static int offline_body_key(const char *body, char *out, size_t capacity) {
  static const char *const names[2] = { "a=", "i=" };
  int n;

  if (out == NULL || capacity < 2)
    return 0;
  out[0] = '\0';
  if (body == NULL)
    return 0;
  for (n = 0; n < 2; ++n) {
    const char *at = strstr(body, names[n]);
    size_t i = 0;

    if (at == NULL)
      continue;
    at += 2;
    out[i++] = (n == 0) ? 'a' : 'i';
    while (*at >= '0' && *at <= '9' && i < capacity - 1)
      out[i++] = *at++;
    out[i] = '\0';
    return i > 1;
  }
  return 0;
}

static int offline_already_queued(const char *post_data) {
  char want[24];
  const char *cursor;
  int queued = 0;

  offline_open_read();
  if (!offline_body_key(post_data, want, sizeof(want)))
    return 0;
  cursor = g_file_text;
  while (*cursor != '\0' && !queued) {
    const char *newline = strchr(cursor, '\n');
    size_t length = newline != NULL ? (size_t)(newline - cursor)
                    : strlen(cursor);

    if (length < sizeof(g_line)) {
      const char *first_tab;
      const char *second_tab;

      memcpy(g_line, cursor, length);
      g_line[length] = '\0';
      first_tab = strchr(g_line, '\t');
      second_tab = first_tab != NULL ? strchr(first_tab + 1, '\t') : NULL;
      if (second_tab != NULL) {
        char have[24];

        if (offline_body_key(second_tab + 1, have, sizeof(have)) &&
            strcmp(have, want) == 0)
          queued = 1;
      }
    }
    cursor = newline != NULL ? newline + 1 : cursor + length;
  }
  return queued;
}

static int offline_keep_tail(size_t *kept) {
  const char *cursor = g_file_text;
  int lines = offline_count(g_file_text);
  int keep_lines = lines;
  int skip;
  size_t keep;

  if (keep_lines > RA_OFFLINE_MAX_ENTRIES - 1)
    keep_lines = RA_OFFLINE_MAX_ENTRIES - 1;
  skip = lines - keep_lines;
  while (skip-- > 0) {
    const char *newline = strchr(cursor, '\n');

    if (newline == NULL) {
      cursor += strlen(cursor);
      break;
    }
    cursor = newline + 1;
  }
  keep = strlen(cursor);
  if (keep >= sizeof(g_kept_text))
    return 0;
  memcpy(g_kept_text, cursor, keep);
  g_kept_text[keep] = '\0';
  *kept = keep;
  return 1;
}

static int offline_redact_body(const char *body, char *out, size_t capacity) {
  size_t i = 0;
  size_t used = 0;

  while (body[i] != '\0') {
    size_t start = i;
    size_t length;

    while (body[i] != '\0' && body[i] != '&')
      ++i;
    length = i - start;
    if (!(length >= 2 && body[start] == 't' && body[start + 1] == '=')) {
      if (used != 0) {
        if (used + 1 >= capacity)
          return 0;
        out[used++] = '&';
      }
      if (used + length >= capacity)
        return 0;
      memcpy(out + used, body + start, length);
      used += length;
    }
    if (body[i] == '&')
      ++i;
  }
  out[used] = '\0';
  return 1;
}

static int offline_body_user_matches(const char *body, const char *username) {
  size_t i = 0;
  size_t want = strlen(username);

  while (body[i] != '\0') {
    size_t start = i;

    while (body[i] != '\0' && body[i] != '&')
      ++i;
    if (i - start == want + 2 && body[start] == 'u' && body[start + 1] == '=' &&
        memcmp(body + start + 2, username, want) == 0)
      return 1;
    if (body[i] == '&')
      ++i;
  }
  return 0;
}

static int offline_add_impl(const char *url, const char *post_data) {
  char body[RA_OFFLINE_MAX_BODY + 1];
  char tag[65];
  char record[LINE_MAX + 2];
  size_t record_length;
  size_t keep;

  if (url == NULL || post_data == NULL || url[0] == '\0' || post_data[0] == '\0')
    return 0;
  if (strlen(url) > RA_OFFLINE_MAX_URL || strlen(post_data) > RA_OFFLINE_MAX_BODY)
    return 0;
  if (!offline_redact_body(post_data, body, sizeof(body)))
    return 0;

  offline_mac(url, body, tag, sizeof(tag));
  if (tag[0] == '\0')
    return 0;
  record_length = (size_t)snprintf(record, sizeof(record), "%s\t%s\t%s\n", tag,
                  url, body);
  if (record_length >= sizeof(record))
    return 0;

  if (offline_already_queued(body))
    return 1;

  if (!offline_keep_tail(&keep))
    return 0;
  if (keep + record_length >= sizeof(g_kept_text))
    return 0;
  memcpy(g_kept_text + keep, record, record_length);
  keep += record_length;
  g_kept_text[keep] = '\0';
  if (!offline_write(g_kept_text, keep))
    return 0;
  g_queue_pending = offline_count(g_kept_text);
  g_queue_pending_known = 1;
  return 1;
}

typedef struct {
  char url[RA_OFFLINE_MAX_URL + 1];
  char body[RA_OFFLINE_MAX_BODY + 1];
  size_t body_length;
  size_t line_length;
  char tag[65];
  int had_newline;
} OfflineReplayLine;

static int offline_replay_next(const char **cursor, OfflineReplayLine *line) {
  const char *at = *cursor;
  const char *newline = strchr(at, '\n');
  size_t length = newline != NULL ? (size_t)(newline - at) : strlen(at);
  const char *first_tab;
  const char *second_tab;
  size_t url_length;

  if (length > LINE_MAX || length < 68) {
    *cursor = newline != NULL ? newline + 1 : at + length;
    return 0;
  }
  memcpy(g_line, at, length);
  g_line[length] = '\0';
  *cursor = newline != NULL ? newline + 1 : at + length;
  line->line_length = length;
  line->had_newline = newline != NULL;

  first_tab = strchr(g_line, '\t');
  second_tab = first_tab != NULL ? strchr(first_tab + 1, '\t') : NULL;
  if (first_tab == NULL || second_tab == NULL ||
      (size_t)(first_tab - g_line) != 64u)
    return 0;

  url_length = (size_t)(second_tab - first_tab - 1);
  line->body_length = strlen(second_tab + 1);
  if (url_length == 0 || url_length > RA_OFFLINE_MAX_URL ||
      line->body_length == 0 || line->body_length > RA_OFFLINE_MAX_BODY)
    return 0;
  memcpy(line->url, first_tab + 1, url_length);
  line->url[url_length] = '\0';
  memcpy(line->body, second_tab + 1, line->body_length);
  line->body[line->body_length] = '\0';

  offline_mac(line->url, line->body, line->tag, sizeof(line->tag));
  if (line->tag[0] == '\0' || memcmp(g_line, line->tag, 64) != 0)
    return 0;
  return 1;
}

static int offline_prepare_post(const OfflineReplayLine *line, char *post,
                                size_t capacity) {
  char username[RA_VAULT_MAX_USERNAME];
  char token[RA_VAULT_MAX_TOKEN];
  char clean[RA_OFFLINE_MAX_BODY + 1];

  if (g_credentials_provider == NULL ||
      !g_credentials_provider(username, sizeof(username), token, sizeof(token)))
    return 0;
  if (!offline_redact_body(line->body, clean, sizeof(clean)))
    return 0;
  if (!offline_body_user_matches(clean, username))
    return -1;
  if (snprintf(post, capacity, "%s&t=%s", clean, token) >= (int)capacity)
    return 0;
  return 1;
}

static int offline_replay_impl(void) {
  const char *cursor;
  OfflineReplayLine line;
  int sent = 0;
  size_t kept = 0;

  if (!offline_open_read())
    return 0;

  cursor = g_file_text;
  while (*cursor != '\0') {
    ra_http_response_t response;
    char post[RA_OFFLINE_MAX_BODY + RA_VAULT_MAX_TOKEN + 8];
    int prepared;
    int result;
    if (!offline_replay_next(&cursor, &line))
      continue;
    prepared = offline_prepare_post(&line, post, sizeof(post));
    if (prepared < 0)
      continue;
    if (prepared == 0)
      return sent;

    memset(&response, 0, sizeof(response));
    result = ra_http_post(line.url, post, strlen(post),
            "application/x-www-form-urlencoded", &response);
    if (result != 0 || response.status_code == 0) {
      size_t rest = line.had_newline ? strlen(cursor) : 0;

      if (kept + line.line_length + 1 + rest < sizeof(g_kept_text)) {
        memcpy(g_kept_text + kept, g_line, line.line_length);
        kept += line.line_length;
        g_kept_text[kept++] = '\n';
        if (rest > 0) {
          memcpy(g_kept_text + kept, cursor, rest);
          kept += rest;
        }
        g_kept_text[kept] = '\0';
        offline_write(g_kept_text, kept);
        g_queue_pending = offline_count(g_kept_text);
        g_queue_pending_known = 1;
      }
      ra_http_response_free(&response);
      return sent;
    }
    ra_http_response_free(&response);
    ++sent;
    if (line.had_newline && *cursor != '\0') {
#ifdef __vita__
      sceKernelDelayThread(RA_OFFLINE_REPLAY_GAP_MS * 1000);
#endif
    }
  }

  if (offline_write("", 0)) {
    g_queue_pending = 0;
    g_queue_pending_known = 1;
  }
  return sent;
}


int ra_offline_queue_add(const char *url, const char *post_data) {
  int result;

  offline_lock();
  result = offline_add_impl(url, post_data);
  offline_unlock();
  return result;
}

int ra_offline_queue_replay(void) {
  int result;

  offline_lock();
  result = offline_replay_impl();
  offline_unlock();
  return result;
}

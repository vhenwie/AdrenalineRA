#ifndef RA_LOG_H
#define RA_LOG_H

/* Timestamped per-session logs shared by the bubble, pspemu module and menu. */

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <string.h>

#define RA_LOG_ROOT "ux0:data/AdrenalineRA"
#define RA_LOG_DIR RA_LOG_ROOT "/logs"
#define RA_LOG_SESSION RA_LOG_ROOT "/RA_SESSION"
#define RA_LOG_CONTINUE RA_LOG_ROOT "/RA_SETUP.CONTINUE"
#define RA_LOG_STAMP_MAX 32
#define RA_LOG_PATH_MAX 96

/* Master logging gate, owned by core/log/ra_logqueue.c. Defaults to off until the
 * signed settings file says otherwise; the menu toggle updates it immediately. */
int ra_log_enabled(void);
void ra_log_set_enabled(int enabled);

static inline int ra_log_length(const char *text, int limit) {
  int length = 0;

  while (length < limit && text[length] != '\0')
    length++;
  return length;
}

static inline void ra_log_put2(char *p, unsigned value) {
  p[0] = (char)('0' + (value / 10) % 10);
  p[1] = (char)('0' + value % 10);
}

static inline void ra_log_put4(char *p, unsigned value) {
  p[0] = (char)('0' + (value / 1000) % 10);
  p[1] = (char)('0' + (value / 100) % 10);
  p[2] = (char)('0' + (value / 10) % 10);
  p[3] = (char)('0' + value % 10);
}

static inline void ra_log_put3(char *p, unsigned value) {
  p[0] = (char)('0' + (value / 100) % 10);
  p[1] = (char)('0' + (value / 10) % 10);
  p[2] = (char)('0' + value % 10);
}

static inline void ra_log_make_stamp(char *out, int size) {
  SceDateTime now;

  if (size < 24) {
    if (size > 0)
      out[0] = '\0';
    return;
  }

  if (sceRtcGetCurrentClockLocalTime(&now) == 0) {
    ra_log_put4(out, (unsigned)now.year);
    ra_log_put2(out + 4, (unsigned)now.month);
    ra_log_put2(out + 6, (unsigned)now.day);
    out[8] = '_';
    ra_log_put2(out + 9, (unsigned)now.hour);
    ra_log_put2(out + 11, (unsigned)now.minute);
    ra_log_put2(out + 13, (unsigned)now.second);
    out[15] = '_';
    ra_log_put3(out + 16, (unsigned)(now.microsecond / 1000));
    out[19] = '\0';
  } else {
    unsigned long long value = sceKernelGetSystemTimeWide();
    char digits[24];
    int count = 0;
    int i;

    do {
      digits[count++] = (char)('0' + (value % 10));
      value /= 10;
    } while (value != 0 && count < 20);

    out[0] = 't';
    for (i = 0; i < count; i++)
      out[1 + i] = digits[count - 1 - i];
    out[1 + count] = '\0';
  }
}

static inline int ra_log_stamp_ok(const char *stamp) {
  int length = ra_log_length(stamp, RA_LOG_STAMP_MAX);
  int i;

  if (length < 8 || length >= RA_LOG_STAMP_MAX)
    return 0;
  for (i = 0; i < length; i++) {
    char c = stamp[i];
    if (!((c >= '0' && c <= '9') || c == '_' || c == 't'))
      return 0;
  }
  return 1;
}

static inline void ra_log_copy(char *dst, int size, const char *src) {
  int i = 0;

  if (size <= 0)
    return;
  while (i < size - 1 && src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static inline void ra_log_append_text(char *dst, int size, const char *src) {
  int at = ra_log_length(dst, size);
  int i = 0;

  while (src[i] != '\0' && at < size - 1)
    dst[at++] = src[i++];
  dst[at] = '\0';
}

/* Cached-descriptor writer: one open per file, not one per line. */
static SceUID ra_log_cached_fd = -1;
static char ra_log_cached_path[RA_LOG_PATH_MAX];
static unsigned ra_log_cached_opens;

static inline void ra_log_forget_cached(void) {
  if (ra_log_cached_fd >= 0)
    sceIoClose(ra_log_cached_fd);
  ra_log_cached_fd = -1;
  ra_log_cached_path[0] = '\0';
}

static inline int ra_log_write_append(const char *path, const char *text,
                  int length) {
  SceUID fd;
  int attempt;

  if (length <= 0)
    return 0;

  for (attempt = 0; attempt < 2; ++attempt) {
    int same = ra_log_cached_fd >= 0 &&
        strcmp(ra_log_cached_path, path) == 0;

    if (!same) {
      ra_log_forget_cached();
      fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
            0777);
      if (fd < 0)
        return fd;
      ra_log_cached_fd = fd;
      ra_log_copy(ra_log_cached_path, RA_LOG_PATH_MAX, path);
      ra_log_cached_opens++;
    }
    if (sceIoWrite(ra_log_cached_fd, text, (SceSize)length) == length)
      return 0;
    ra_log_forget_cached();
  }
  return -1;
}

static inline int ra_log_write_file(const char *path, const char *text,
                  int length, int append) {
  SceUID fd;

  if (length < 0)
    return -1;
  if (append)
    return ra_log_write_append(path, text, length);

  fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return fd;
  if (length > 0 && sceIoWrite(fd, text, (SceSize)length) != length) {
    sceIoClose(fd);
    return -1;
  }
  sceIoClose(fd);
  return 0;
}

/* Verbose gate, mirroring the marker-file switch Adrenaline+ uses: an empty file
 * at RA_LOG_HOT_MARKER turns the highest-frequency diagnostic lines back on.
 * The marker is read once, so flipping it takes a restart - which is the point:
 * the per-site cost of the gate is one load and a branch. */
#define RA_LOG_HOT_MARKER RA_LOG_DIR "/ra_verbose_log"

static inline int ra_log_hot_enabled(void) {
  static int cached = -1;
  SceIoStat stat;

  if (cached < 0)
    cached = sceIoGetstat(RA_LOG_HOT_MARKER, &stat) >= 0 ? 1 : 0;
  return cached;
}


static inline int ra_log_read_session(char *out, int size) {
  SceUID fd;
  int length;

  if (size <= 1)
    return 0;

  fd = sceIoOpen(RA_LOG_SESSION, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;

  length = (int)sceIoRead(fd, out, (SceSize)(size - 1));
  sceIoClose(fd);
  if (length <= 0)
    return 0;

  while (length > 0 &&
    (out[length - 1] == '\r' || out[length - 1] == '\n' ||
      out[length - 1] == ' '))
    length--;
  out[length] = '\0';

  return ra_log_stamp_ok(out);
}

static inline void ra_log_new_session(void) {
  char stamp[RA_LOG_STAMP_MAX];
  int length;

  if (!ra_log_enabled())
    return;
  sceIoMkdir(RA_LOG_ROOT, 0777);
  sceIoMkdir(RA_LOG_DIR, 0777);
  ra_log_make_stamp(stamp, sizeof(stamp));
  length = ra_log_length(stamp, sizeof(stamp));
  if (length > 0)
    (void)ra_log_write_file(RA_LOG_SESSION, stamp, length, 0);
}

static inline int ra_log_continue_requested(void) {
  SceIoStat stat;

  return sceIoGetstat(RA_LOG_CONTINUE, &stat) >= 0;
}

static inline void ra_log_begin_session(int continuation) {
  char stamp[RA_LOG_STAMP_MAX];

  if (!ra_log_enabled())
    return;
  sceIoMkdir(RA_LOG_ROOT, 0777);
  sceIoMkdir(RA_LOG_DIR, 0777);
  if (!(continuation && ra_log_read_session(stamp, sizeof(stamp))))
    ra_log_new_session();
  sceIoRemove(RA_LOG_CONTINUE);
}

static inline void ra_log_mark_continue(void) {
  sceIoMkdir(RA_LOG_DIR, 0777);
  (void)ra_log_write_file(RA_LOG_CONTINUE, "1", 1, 0);
}

static inline void ra_log_session_path(char *out, int size, const char *base) {
  char stamp[RA_LOG_STAMP_MAX];

  if (!ra_log_read_session(stamp, sizeof(stamp))) {
    ra_log_make_stamp(stamp, sizeof(stamp));
    if (stamp[0] != '\0')
      (void)ra_log_write_file(RA_LOG_SESSION, stamp,
                  ra_log_length(stamp, sizeof(stamp)), 0);
  }

  ra_log_copy(out, size, RA_LOG_DIR "/");
  ra_log_append_text(out, size, base);
  ra_log_append_text(out, size, "_");
  ra_log_append_text(out, size, stamp);
  ra_log_append_text(out, size, ".log");
}

static inline void ra_log_append(const char *base, const char *text, int length) {
  char path[RA_LOG_PATH_MAX];

  if (!ra_log_enabled())
    return;
  if (length <= 0)
    length = ra_log_length(text, 1024);
  ra_log_session_path(path, sizeof(path), base);
  (void)ra_log_write_file(path, text, length, 1);
}

#endif /* RA_LOG_H */

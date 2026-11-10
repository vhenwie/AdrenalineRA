/* Asynchronous log writer for the runtime and menu. */

#include "ra_logqueue.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ra_log.h"

#define RING_SIZE 64
#define PATH_MAX 96
#define PRIORITY 0x10000120
#define FALLBACK_PRIORITY 0x10000100
#define STACK_SIZE 0x10000
#define DRAIN_US (100 * 1000)

/* Master logging switch. */

static volatile int g_enabled;

int ra_log_enabled(void) {
  return g_enabled != 0;
}

void ra_log_set_enabled(int enabled) {
  g_enabled = enabled != 0;
  if (g_enabled) {
    sceIoMkdir(RA_LOG_ROOT, 0777);
    sceIoMkdir(RA_LOG_DIR, 0777);
  }
}

static char g_ring[RING_SIZE][RA_LOGQ_LINE];
static char g_batch[RING_SIZE * RA_LOGQ_LINE];
static volatile int g_head;
static volatile int g_tail;
static SceUID g_lock = -1;
static SceUID g_write_lock = -1;
static SceUID g_thread = -1;
static int g_inline;
static unsigned int g_dropped;

static SceUID g_session_fd = -1;
static char g_session_path[PATH_MAX];
static SceUID g_stable_fd = -1;

static void logq_lock(void) {
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

static void logq_unlock(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

static void logq_write(const char *text, int length);

static void logq_write_exclusive(const char *text, int length) {
  if (g_write_lock >= 0)
    sceKernelWaitSema(g_write_lock, 1, NULL);
  logq_write(text, length);
  if (g_write_lock >= 0)
    sceKernelSignalSema(g_write_lock, 1);
}

static void logq_write_to(SceUID *fd, char *cached_path, const char *path,
            const char *text, int length) {
  int attempt;

  for (attempt = 0; attempt < 2; ++attempt) {
    int same = *fd >= 0;

    if (same && cached_path != NULL && cached_path[0] != '\0' &&
        strcmp(cached_path, path) != 0) {
      sceIoClose(*fd);
      *fd = -1;
      same = 0;
    }
    if (!same) {
      *fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
      if (*fd < 0)
        return;
      if (cached_path != NULL)
        snprintf(cached_path, PATH_MAX, "%s", path);
    }
    if (sceIoWrite(*fd, text, (SceSize)length) == length)
      return;
    sceIoClose(*fd);
    *fd = -1;
  }
}

static void logq_write(const char *text, int length) {
  char path[PATH_MAX];

  if (!ra_log_enabled() || length <= 0)
    return;
  ra_log_session_path(path, sizeof(path), "RA_SETUP");
  logq_write_to(&g_session_fd, g_session_path, path, text, length);
  logq_write_to(&g_stable_fd, NULL, RA_LOG_DIR "/RA_SETUP.log", text, length);
}

static int logq_take_batch(void) {
  int length = 0;
  int count = 0;

  logq_lock();
  while (g_head != g_tail && count < RING_SIZE) {
    int size = (int)strlen(g_ring[g_head]);

    if (length + size < (int)sizeof(g_batch)) {
      memcpy(g_batch + length, g_ring[g_head], (size_t)size);
      length += size;
    }
    g_head = (g_head + 1) % RING_SIZE;
    ++count;
  }
  logq_unlock();
  return length;
}

static int logq_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    int length;
    sceKernelDelayThread(DRAIN_US);
    while ((length = logq_take_batch()) > 0)
      logq_write_exclusive(g_batch, length);
  }
  return 0;
}

static void logq_start(void) {
  char notice[96];
  int length;

  if (g_thread >= 0 || g_inline)
    return;
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_logq", 0, 1, 1, NULL);
  if (g_write_lock < 0)
    g_write_lock = sceKernelCreateSema("ra_logqw", 0, 1, 1, NULL);
  if (g_lock < 0) {
    g_inline = 1;
    logq_write("log: no lock; writing inline\n",
        (int)strlen("log: no lock; writing inline\n"));
    return;
  }

  g_thread = sceKernelCreateThread("ra_logq", logq_worker, PRIORITY,
                  STACK_SIZE, 0, 0, NULL);
  if (g_thread < 0)
    g_thread = sceKernelCreateThread("ra_logq", logq_worker,
                    FALLBACK_PRIORITY, STACK_SIZE, 0, 0, NULL);
  if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
    length = snprintf(notice, sizeof(notice),
            "log: worker failed (0x%08X); writing inline\n",
            (unsigned)g_thread);
    g_thread = -1;
    g_inline = 1;
    logq_write(notice, length);
    return;
  }
  length = snprintf(notice, sizeof(notice),
          "log: worker started thid=0x%08X ring=%d\n",
          (unsigned)g_thread, RING_SIZE);
  logq_write(notice, length);
}

void ra_logq_line(const char *line, int length) {
  int next;

  if (!ra_log_enabled() || line == NULL || length <= 0)
    return;
  if (length > RA_LOGQ_LINE - 1)
    length = RA_LOGQ_LINE - 1;
  logq_start();
  if (g_inline) {
    logq_write(line, length);
    return;
  }
  if (g_lock < 0)
    return;

  logq_lock();
  next = (g_tail + 1) % RING_SIZE;
  if (next == g_head) {
    ++g_dropped;
    logq_unlock();
    if (g_dropped == 1) {
      static const char notice[] = "log: ring full; lines are being dropped\n";
      logq_write_exclusive(notice, (int)strlen(notice));
    }
    return;
  }
  memcpy(g_ring[g_tail], line, (size_t)length);
  if (g_ring[g_tail][length - 1] != '\n')
    g_ring[g_tail][length - 1] = '\n';
  g_ring[g_tail][length] = '\0';
  g_tail = next;
  logq_unlock();
}

void ra_logq_printf(const char *format, ...) {
  char text[RA_LOGQ_LINE];
  va_list args;
  int length;

  if (format == NULL)
    return;
  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length > (int)sizeof(text) - 1)
    length = (int)sizeof(text) - 1;
  ra_logq_line(text, length);
}

void ra_logq_flush(void) {
  int length;

  if (g_lock < 0)
    return;
  while ((length = logq_take_batch()) > 0)
    logq_write_exclusive(g_batch, length);
}

void ra_logq_printf_sync(const char *format, ...) {
  char text[RA_LOGQ_LINE];
  va_list args;
  int length;

  if (format == NULL)
    return;
  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length > (int)sizeof(text) - 1)
    length = (int)sizeof(text) - 1;
  ra_logq_line(text, length);
  ra_logq_flush();
}

void ra_logq_direct(const char *line) {
  int length;

  if (!ra_log_enabled() || line == NULL)
    return;
  length = (int)strlen(line);
  if (length > RA_LOGQ_LINE - 1)
    length = RA_LOGQ_LINE - 1;
  logq_write_exclusive(line, length);
}

unsigned int ra_logq_dropped(void) {
  return g_dropped;
}

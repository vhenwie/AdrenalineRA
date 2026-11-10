#include "ra_runtime_core.h"
#include "ra_runtime_internal.h"

#include <stdarg.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "ra_log.h"
#include "ra_logqueue.h"

static SceUID g_runtime_lock = -1;

void ra_runtime_lock(void) {
  unsigned long long wait_start;
  unsigned long long wait_us;
  static unsigned long long last_warn_us;

  if (g_runtime_lock < 0)
    g_runtime_lock = sceKernelCreateSema("ra_runtime_lock", 0, 1, 1, NULL);
  wait_start = (unsigned long long)sceKernelGetSystemTimeWide();
  if (g_runtime_lock >= 0)
    sceKernelWaitSema(g_runtime_lock, 1, NULL);
  wait_us = (unsigned long long)sceKernelGetSystemTimeWide() - wait_start;
  if (wait_us > 30000ULL &&
    wait_start - last_warn_us > 3000000ULL) {
    last_warn_us = wait_start;
    ra_runtime_log("runtime: lock wait %u ms (another thread held it)\n",
          (unsigned int)(wait_us / 1000ULL));
  }
}

void ra_runtime_unlock(void) {
  if (g_runtime_lock >= 0)
    sceKernelSignalSema(g_runtime_lock, 1);
}

void ra_runtime_copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  if (source == NULL)
    source = "";
  snprintf(destination, capacity, "%s", source);
}



unsigned long long ra_runtime_now_ms(void) {
  return (unsigned long long)(sceKernelGetSystemTimeWide() / 1000ULL);
}

ra_client_t *ra_runtime_client_acquire(ra_session_t **out_session) {
  ra_session_t *session;
  ra_client_t *client;

  if (out_session != NULL)
    *out_session = NULL;
  session = ra_runtime_session_acquire();
  if (session == NULL)
    return NULL;
  client = ra_session_get_client(session);
  if (client == NULL) {
    ra_session_release(session);
    return NULL;
  }
  if (out_session == NULL) {
    ra_session_release(session);
    return NULL;
  }
  ra_client_lock(client);
  *out_session = session;
  return client;
}

void ra_runtime_client_release(ra_session_t *session) {
  ra_client_t *client;

  if (session == NULL)
    return;
  client = ra_session_get_client(session);
  if (client != NULL)
    ra_client_unlock(client);
  ra_session_release(session);
}

RaRuntimeContext g_rt = {
  .start_sema = -1,
  .thread = -1,
  .power_callback = -1,
  .hold_watchdog_thread = -1,
  .disc_retry_disc = -1,
};

const unsigned long long g_retry_delays_us[RA_RUNTIME_LOAD_ATTEMPTS] = {
  5000000ULL, 20000000ULL, 45000000ULL
};

void ra_runtime_info_set(const RaRuntimeInfo *info) {
  ra_runtime_lock();
  g_rt.info = *info;
  ra_runtime_unlock();
}

void ra_runtime_log(const char *format, ...) {
  char text[320];
  va_list args;
  int length;

  if (!ra_log_enabled())
    return;

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  /* vsnprintf returns the length it *would *have written. Passing that to
     * ra_log_append() after truncation makes it read past the buffer, so clamp
     * it. Long diagnostic lines (response head/tail) made this reachable. */
  if (length > (int)sizeof(text) - 1)
    length = (int)sizeof(text) - 1;
  if (length > 0)
    ra_logq_line(text, length);
}

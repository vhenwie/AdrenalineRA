/* Deferred server calls for the runtime thread. */

#include "ra_netqueue.h"

#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ra_log.h"
#include "ra_logqueue.h"
#include "ra_offline.h"
#include "ra_server.h"

#define JOBS 6
#define PRIORITY 0x10000120
#define FALLBACK_PRIORITY 0x10000100
#define STACK_SIZE 0x10000
#define WAIT_US (50 * 1000)

enum { FREE = 0, QUEUED, RUNNING, DONE };

static RaServerJob g_jobs[JOBS];
static volatile int g_state[JOBS];
static volatile int g_head;
static volatile int g_tail;
static volatile int g_work;
static SceUID g_lock = -1;
static SceUID g_wake = -1;
static SceUID g_thread = -1;
static volatile int g_stop;
static volatile int g_replay_offline;
static volatile int g_offline_replayed;
static volatile int g_abandoned;
static unsigned int g_delivered;
static unsigned int g_inline;

static void net_log(const char *format, ...) {
  char text[160];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length < 0)
    return;
  if (length >= (int)sizeof(text))
    length = (int)sizeof(text) - 1;
  if (length > 0)
    ra_logq_line(text, length);
}

static void net_lock(void) {
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

static void net_unlock(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

static void net_reset(void) {
  int i;

  for (i = 0; i < JOBS; ++i) {
    memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
    g_state[i] = FREE;
  }
  g_head = 0;
  g_tail = 0;
  g_work = 0;
}

static void net_persist_failure(const RaServerJob *job) {
  if (job->rejected || job->transport_error[0] == '\0' ||
      job->post_data == NULL)
    return;
  if (strcmp(job->request_name, "awardachievement") != 0 &&
      strcmp(job->request_name, "submitlbentry") != 0)
    return;
  if (job->url == NULL || job->url[0] == '\0')
    return;
  if (ra_offline_queue_add(job->url, job->post_data))
    net_log("net: %s kept for replay (transport error: %s)\n",
        job->request_name, job->transport_error);
  else
    net_log("net: %s NOT kept (queue full or record too large)\n",
        job->request_name);
}

static int net_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  while (!g_stop) {
    SceUInt timeout = WAIT_US;
    int slot;
    int state;

    sceKernelWaitSema(g_wake, 1, &timeout);
    if (g_replay_offline) {
      int replayed;
      g_replay_offline = 0;
      replayed = ra_offline_queue_replay();
      net_log("net: offline replay sent=%d\n", replayed);
      if (replayed > 0)
        g_offline_replayed += replayed;
    }

    for (;;) {
      net_lock();
      slot = g_work;
      state = g_state[slot];
      if (state == QUEUED)
        g_state[slot] = RUNNING;
      net_unlock();
      if (state != QUEUED)
        break;

      ra_server_job_execute(&g_jobs[slot]);
      net_persist_failure(&g_jobs[slot]);

      net_lock();
      g_state[slot] = DONE;
      g_work = (g_work + 1) % JOBS;
      net_unlock();
      if (g_stop)
        break;
    }
  }
  return 0;
}

void ra_netqueue_init(void) {
  g_stop = 0;
  if (g_abandoned) {
    static int logged;
    if (!logged) {
      logged = 1;
      net_log("net: queue stays off after an abandoned worker; requests run "
          "synchronously for the rest of the session\n");
    }
    return;
  }
  if (g_thread >= 0)
    return;
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_net", 0, 1, 1, NULL);
  if (g_wake < 0)
    g_wake = sceKernelCreateSema("ra_net_wake", 0, 0, 1, NULL);
  if (g_lock < 0 || g_wake < 0) {
    net_log("net: queue unavailable (lock=%d wake=%d); requests run inline\n",
        (int)g_lock, (int)g_wake);
    return;
  }

  net_reset();
  g_thread = sceKernelCreateThread("ra_netqueue", net_worker, PRIORITY,
                  STACK_SIZE, 0, 0, NULL);
  if (g_thread < 0)
    g_thread = sceKernelCreateThread("ra_netqueue", net_worker,
                    FALLBACK_PRIORITY, STACK_SIZE, 0, 0, NULL);
  if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
    net_log("net: worker failed (0x%08X); requests run inline\n",
        (unsigned)g_thread);
    g_thread = -1;
    return;
  }
  net_log("net: queue started (jobs=%d prio=0x%08X)\n", JOBS,
      (unsigned)PRIORITY);
}

void ra_netqueue_stop(void) {
  unsigned long long started_us;
  int i;

  if (g_thread < 0)
    return;
  started_us = (unsigned long long)sceKernelGetSystemTimeWide();
  g_stop = 1;
  sceKernelSignalSema(g_wake, 1);
  {
    SceUInt timeout = 1500 * 1000;
    if (sceKernelWaitThreadEnd(g_thread, NULL, &timeout) < 0) {
      net_log("net: worker still busy after 1500 ms; queue retained\n");
      return;
    } else {
      sceKernelDeleteThread(g_thread);
      g_thread = -1;
    }
  }

  for (i = 0; i < JOBS; ++i) {
    if (g_state[i] != FREE)
      ra_server_job_dispose(&g_jobs[i]);
    g_state[i] = FREE;
  }
  g_head = 0;
  g_tail = 0;
  g_work = 0;
  net_log("net: queue stopped after %u ms (delivered=%u inline=%u)\n",
      (unsigned)(((unsigned long long)sceKernelGetSystemTimeWide() -
            started_us) / 1000ULL), g_delivered, g_inline);
  g_delivered = 0;
  g_inline = 0;
}

void ra_netqueue_server_call(const rc_api_request_t *request,
              rc_client_server_callback_t callback, void *callback_data,
              rc_client_t *client) {
  int slot;
  int state;

  if (g_lock < 0 || g_thread < 0 || g_stop || g_abandoned) {
    ra_server_call(request, callback, callback_data, client);
    return;
  }
  net_lock();
  slot = g_tail;
  state = g_state[slot];
  if (state == FREE &&
      ra_server_job_prepare(&g_jobs[slot], request, callback, callback_data, 0)) {
    g_state[slot] = QUEUED;
    g_tail = (g_tail + 1) % JOBS;
    net_unlock();
    sceKernelSignalSema(g_wake, 1);
    return;
  }
  ++g_inline;
  net_unlock();
  net_log("net: queue full; request sent inline\n");
  ra_server_call(request, callback, callback_data, client);
}

void ra_netqueue_pump(void) {
  int guard;

  if (g_lock < 0)
    return;
  for (guard = 0; guard < JOBS; ++guard) {
    int state;

    net_lock();
    state = g_state[g_head];
    net_unlock();
    if (state != DONE)
      break;
    ra_server_job_finish(&g_jobs[g_head]);
    ++g_delivered;
    net_lock();
    g_state[g_head] = FREE;
    g_head = (g_head + 1) % JOBS;
    net_unlock();
  }
}

int ra_netqueue_depth(void) {
  int depth = 0;
  int i;

  net_lock();
  for (i = 0; i < JOBS; ++i) {
    if (g_state[i] != FREE)
      ++depth;
  }
  net_unlock();
  return depth;
}

void ra_netqueue_replay_offline(void) {
  if (g_thread < 0 || g_stop || g_abandoned)
    return;
  g_replay_offline = 1;
  sceKernelSignalSema(g_wake, 1);
}

int ra_netqueue_take_replay_count(void) {
  int count = g_offline_replayed;
  g_offline_replayed = 0;
  return count;
}

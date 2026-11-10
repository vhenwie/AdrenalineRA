#include "ra_badge.h"
#include "ra_badge_internal.h"
#include "ra_badge_png.h"
#include "ra_badge_blob.h"
#include "ra_badge_fetch.h"
#include "ra_badge_texture.h"
#include "ra_badge_state.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng/lodepng.h"
#include "ra_http.h"
#include "ra_log.h"
#include "ra_logqueue.h"
/* Qualified: libvita2d ships its own utils.h and is searched first. */
#include "../../third_party/Adrenaline/user/utils.h"

/* DuckStation separates badge networking, URL caching and texture upload.
 * Vita has synchronous SceHttp, so this module keeps one low-priority worker
 * (network + RAM) and a small draw-thread texture cache. */

#define RA_BADGE_QUEUE_MAX 256
/* Fixed twelve-slot LRU. The live indicators keep up to six badge textures on screen
 * at once, the unlock card adds one more, and prefetched near-unlock art shares the
 * same cache; twelve covers that working set without letting the cache grow. Every
 * drawn tile bumps its age, so an off-screen badge is the eviction victim. A 64x64
 * badge is 16 KB, so the whole set is ~192 KB. */
#define RA_BADGE_TEXTURE_CACHE 12
#define RA_BADGE_MAX_BYTES (128 * 1024)
#define RA_BADGE_MAX_DIM 128
#define RA_BADGE_STACK 0x10000
/* 0x10000200 is outside the legal range (sceKernelCreateThread returns
 * 0x80028023), which is why no badge ever downloaded. 0x10000110 is a valid
 * low-priority slot just below the other RA workers. */
#define RA_BADGE_PRIORITY 0x10000110
#define RA_BADGE_FALLBACK_PRIORITY 0x10000100

/* Warm-up pacing and circuit breaker: one connection per badge, so pace the queue and
 * stop it after a run of failures. */
/* One TLS connection per badge; 120 ms is the measured floor. */
#define RA_BADGE_PACE_MS 120
/* Prefetch art is written to the cache; on-demand art is kept in RAM so the
 * unlock frame never waits for a ux0 write. A slow prefetch write backs off. */
#define RA_BADGE_SLOW_WRITE_MS 150u
#define RA_BADGE_SLOW_WRITE_BACKOFF_MS 800u
#define RA_BADGE_FAIL_STREAK 5
#define RA_BADGE_PAUSE_MS 30000
#define RA_BADGE_MAX_PAUSES 3







static RaBadgeJob g_jobs[RA_BADGE_QUEUE_MAX];
static int g_job_count;
static SceUID g_badge_wake = -1;
static SceUID g_badge_thread = -1;
static volatile int g_badge_stop;

static volatile int g_badge_quit;
static volatile int g_badge_worker_exited;
static volatile int g_badge_enabled = 1;
/* Set when the circuit breaker gave up for this session; cleared on the next
 * ra_badge_init/set_enabled so a new game starts with a clean slate. */
static volatile int g_badge_stalled;
static unsigned int g_badge_downloads;
static unsigned int g_badge_failures;
/* Negative cache : a key with no PNG on the card is remembered for a few
 * seconds, so the draw thread does not `sceIoGetstat()` the same missing file on every
 * frame. That per-frame stat is what made an unlock banner lag while the console was
 * offline - the art could not be downloaded, so every frame re-probed the card. */
/* Keys the worker has just fetched, waiting for the draw thread to upload them
 * . Ring of 12: the worker produces far slower than 60 Hz, so this never
 * fills in practice, and a full ring simply drops its oldest entry. */
/* Duration of the last prefetch cache write, in ms; read by the worker to back off. */












/* Read the PNG signature and IHDR directly. This runs on the badge worker, so
 * it must not touch lodepng's adr_malloc heap concurrently with the draw
 * thread's decode. */


/* Defined below; queued before the cache write so the draw thread can upload the
 * held PNG while the worker is still touching ux0. */

/* Returns 1 when the PNG is held for the draw thread, 0 on any failure, so the
 * worker can count a failure run for the circuit breaker. */

/* Sleeps in slices so a stop request or a settings change is honoured inside a
 * backoff pause instead of after it. */


/* Queues a freshly cached key for the draw thread to upload . Called from
 * the worker; every field of the ring is touched under `g_badge_lock`. */

/* Pops the next queued job under the lock. Returns 1 when `job` was filled, 0 when
 * the drain loop must stop (queue empty, or a stop/disable request cleared it). */
static int badge_worker_pop_job(RaBadgeJob *job) {
  int found = 0;

  ra_badge_lock();
  if (!g_badge_enabled || g_badge_stop) {
    g_job_count = 0;
    ra_badge_unlock();
    return 0;
  }
  if (g_job_count != 0) {
    *job = g_jobs[0];
    --g_job_count;
    if (g_job_count > 0)
      memmove(&g_jobs[0], &g_jobs[1],
          sizeof(g_jobs[0]) * (size_t)g_job_count);
    found = 1;
  }
  ra_badge_unlock();
  return found;
}

/* Fetches one popped job and applies the pacing / failure-run circuit breaker. */
static void badge_worker_run_job(const RaBadgeJob *job, int *fail_streak,
    int *pauses) {
  ++g_badge_downloads;
  ra_badge_log("badge: fetch %s\n", job->key);
  if (ra_badge_fetch(job)) {
    *fail_streak = 0;
    /* Only on-demand art is uploaded now; prefetch art is already on disk
             * and is decoded when it is actually drawn. */
    if (job->prime)
      ra_badge_prime_queue(job->key);
    if (!job->prime &&
      ra_badge_last_write_ms() >= RA_BADGE_SLOW_WRITE_MS) {
      ra_badge_log("badge: slow cache write %u ms; backing off %u ms\n",
          ra_badge_last_write_ms(), RA_BADGE_SLOW_WRITE_BACKOFF_MS);
      ra_badge_pause(RA_BADGE_SLOW_WRITE_BACKOFF_MS);
    }
  } else if (g_badge_enabled && !g_badge_stop) {
    ++g_badge_failures;
    if (++(*fail_streak) >= RA_BADGE_FAIL_STREAK) {
      /* The host is refusing this console (TLS is the usual way it
                 * shows up). Carrying on only keeps every other request in
                 * the session broken, so back off, then give up. */
      *fail_streak = 0;
      ++(*pauses);
      ra_badge_log("badge: %d consecutive failures; pausing %u ms "
          "(pause %d of %d, %u failed of %u)\n",
          RA_BADGE_FAIL_STREAK, (unsigned)RA_BADGE_PAUSE_MS,
          *pauses, RA_BADGE_MAX_PAUSES, g_badge_failures,
          g_badge_downloads);
      if (*pauses >= RA_BADGE_MAX_PAUSES) {
        ra_badge_lock();
        g_job_count = 0;
        ra_badge_unlock();
        g_badge_stalled = 1;
        ra_badge_log("badge: prefetch stopped for this session "
            "(%u failed of %u requested)\n",
            g_badge_failures, g_badge_downloads);
      }
      ra_badge_pause(RA_BADGE_PAUSE_MS);
    } else {
      ra_badge_pause(RA_BADGE_PACE_MS);
    }
  }
}

/* Drains the queue until it is empty or a stop request clears it. */
static void badge_worker_drain(int *fail_streak, int *pauses) {
  RaBadgeJob job;

  for (;;) {
    if (!badge_worker_pop_job(&job))
      break;
    badge_worker_run_job(&job, fail_streak, pauses);
    /* Pace even the successful run: one handshake after another is what
           * got the host to start refusing us. */
    if (g_job_count > 0)
      ra_badge_pause(RA_BADGE_PACE_MS);
  }
}

static int badge_worker(SceSize args, void *argp) {
  int fail_streak = 0;
  int pauses = 0;

  (void)args;
  (void)argp;

  for (;;) {
    /* Bounded wait so a teardown is noticed: on stop the worker leaves the thread
         * instead of parking on the semaphore, which is what lets `ra_badge_stop()`
         * join it before the module unloads. */
    SceUInt wait_us = 100 * 1000;

    sceKernelWaitSema(g_badge_wake, 1, &wait_us);
    /* Parked, not dead : a pause keeps the thread alive so the next
         * session's queue has a waiter. Only teardown leaves this loop. */
    if (g_badge_quit)
      break;
    badge_worker_drain(&fail_streak, &pauses);
  }
  /* Terminal: return normally so `ra_badge_shutdown()` can join this thread and delete
     * it. Exiting/deleting ourselves here would race with that join . The exit
     * bit is what stops a finished thread from looking like a live worker. */
  g_badge_worker_exited = 1;
  ra_badge_log("badge: worker exit\n");
  return 0;
}

void ra_badge_init(void) {
  if (g_badge_wake < 0)
    g_badge_wake = sceKernelCreateSema("ra_badge_wake", 0, 1, 1, NULL);
  /* Serialise thread creation: `ra_badge_request()` revives a returned worker, and two
     * requests from different threads must not create two workers draining one queue. */
  ra_badge_lock();
  /* A thread that returned cannot be started again: reclaim a stale badge thread before
 * the liveness test below. */
  if (g_badge_thread >= 0 && g_badge_worker_exited) {
    sceKernelDeleteThread(g_badge_thread);
    g_badge_thread = -1;
    ra_badge_log("badge: reclaimed an exited worker; starting a new one\n");
  }
  if (g_badge_thread < 0 && g_badge_wake >= 0) {
    g_badge_worker_exited = 0;
    g_badge_quit = 0;
    g_badge_thread = sceKernelCreateThread("ra_badge", badge_worker,
                      RA_BADGE_PRIORITY,
                      RA_BADGE_STACK, 0, 0, NULL);
    if (g_badge_thread < 0) {
      /* Never leave badges dead because of a scheduling attribute: retry
             * at the priority the other RA workers already use. */
      g_badge_thread = sceKernelCreateThread("ra_badge", badge_worker,
                        RA_BADGE_FALLBACK_PRIORITY,
                        RA_BADGE_STACK, 0, 0, NULL);
    }
    if (g_badge_thread >= 0) {
      sceKernelStartThread(g_badge_thread, 0, NULL);
      ra_badge_log("badge: worker started\n");
    } else {
      ra_badge_log("badge: worker create failed (0x%08X)\n",
          (unsigned)g_badge_thread);
    }
  }
  ra_badge_unlock();
  g_badge_stop = 0;
  g_badge_stalled = 0;
  g_badge_downloads = 0;
  g_badge_failures = 0;
  sceIoMkdir("ux0:data/AdrenalineRA", 0777);
  sceIoMkdir(RA_BADGE_DIR, 0777);
}

void ra_badge_stop(void) {
  
  if (!g_badge_stop)
    ra_badge_log("badge: worker paused; queued art dropped\n");
  g_badge_stop = 1;
  ra_badge_lock();
  g_job_count = 0;
  ra_badge_unlock();
  if (g_badge_wake >= 0)
    sceKernelSignalSema(g_badge_wake, 1);
}


void ra_badge_shutdown(void) {
  /* The only path that ends the worker : a pause must not, because the
     * runtime resumes a paused session by calling `ra_badge_init()` again. */
  g_badge_quit = 1;
  ra_badge_stop();
  if (g_badge_thread >= 0) {
    SceUInt timeout = 1500 * 1000;

    if (sceKernelWaitThreadEnd(g_badge_thread, NULL, &timeout) < 0) {
      ra_badge_log("badge: worker still busy after 1500 ms; keeping it\n");
      g_badge_worker_exited = 0;
      return;
    }
    sceKernelDeleteThread(g_badge_thread);
    g_badge_thread = -1;
  }
  g_badge_worker_exited = 0;
  g_badge_quit = 0;
}


int ra_badge_worker_alive(void) {
  return g_badge_thread >= 0 && !g_badge_worker_exited;
}


int ra_badge_stopped(void) {
  return g_badge_stop;
}

void ra_badge_set_enabled(int enabled) {
  g_badge_enabled = enabled != 0;
  if (g_badge_enabled) {
    g_badge_stop = 0;
    g_badge_stalled = 0;
    g_badge_downloads = 0;
    g_badge_failures = 0;
  }
  if (!g_badge_enabled) {
    ra_badge_lock();
    g_job_count = 0;
    ra_badge_unlock();
  }
  if (g_badge_wake >= 0)
    sceKernelSignalSema(g_badge_wake, 1);
}

int ra_badge_enabled(void) {
  return g_badge_enabled;
}

int ra_badge_pending(void) {
  int count;

  ra_badge_lock();
  count = g_job_count;
  ra_badge_unlock();
  return count;
}

int ra_badge_stalled(void) {
  return g_badge_stalled;
}

/* 1 when the cache directory for the current game exists . Diagnostics only:
 * the POPS report "active badges have no images" has to distinguish a missing directory
 * (the `badges/0/` failuredocuments) from a download that never happened. */

/* Gate: disabled, paused, stalled, invalid key or no URL. A teardown is not a bug:
 * silence it. */
static int badge_request_rejected(const char *key, const char *url) {
  if (!g_badge_enabled || g_badge_stop || g_badge_stalled ||
    !ra_badge_key_valid(key) || url == NULL || url[0] == '\0') {

    if (g_badge_stop)
      return 1;
    ra_badge_log("badge: skipped key='%s' enabled=%d stop=%d stalled=%d "
        "valid=%d url=%d\n",
        key != NULL ? key : "(null)", g_badge_enabled, g_badge_stop,
        g_badge_stalled, ra_badge_key_valid(key),
        url != NULL && url[0] != '\0');
    return 1;
  }
  return 0;
}

/* A texture already in RAM, or a PNG already on the card, needs no download. */
static int badge_request_cached(const char *key, int high_priority) {
  if (high_priority != 0 && ra_badge_texture_memory_cached(key))
    return 1;
  if (ra_badge_exists(key)) {
    if (ra_log_hot_enabled())
      ra_badge_log("badge: cached-hit %s\n", key);
    return 1;
  }
  return 0;
}

/* A queue with no waiter is a black hole : if the worker has returned,
 * start a fresh one instead of queueing art nothing will ever fetch. `ra_badge_init`
 * is the only place that creates the thread, and it is idempotent while one lives. */
static int badge_request_ensure_worker(void) {
  if (!ra_badge_worker_alive()) {
    ra_badge_log("badge: no live worker; restarting it\n");
    ra_badge_init();
    if (!ra_badge_worker_alive())
      return 0;
  }
  return 1;
}

/* Inserts or promotes the job under the lock. Returns 1 when the job was queued. */
static int badge_request_enqueue(const char *key, const char *url,
    int high_priority, int *was_empty) {
  int i;

  ra_badge_lock();
  if (!g_badge_enabled || g_badge_stop) {
    ra_badge_unlock();
    return 0;
  }
  *was_empty = g_job_count == 0;
  for (i = 0; i < g_job_count; ++i) {
    if (strcmp(g_jobs[i].key, key) == 0) {
      if (high_priority != 0) {
        g_jobs[i].prime = 1;
        if (i > 0) {
          RaBadgeJob promoted = g_jobs[i];
          memmove(&g_jobs[1], &g_jobs[0],
              sizeof(g_jobs[0]) * (size_t)i);
          g_jobs[0] = promoted;
        }
      }
      ra_badge_unlock();
      return 0;
    }
  }
  if (g_job_count >= RA_BADGE_QUEUE_MAX) {
    if (!high_priority) {
      ra_badge_unlock();
      return 0;
    }
    /* A real unlock outranks one prefetch entry. */
    --g_job_count;
  }

  if (high_priority != 0 && g_job_count > 0) {
    memmove(&g_jobs[1], &g_jobs[0],
        sizeof(g_jobs[0]) * (size_t)g_job_count);
    i = 0;
  } else {
    i = g_job_count;
  }
  memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
  strncpy(g_jobs[i].key, key, sizeof(g_jobs[i].key) - 1);
  strncpy(g_jobs[i].url, url, sizeof(g_jobs[i].url) - 1);
  g_jobs[i].prime = high_priority != 0;
  ++g_job_count;
  ra_badge_unlock();
  return 1;
}

void ra_badge_request(const char *key, const char *url, int high_priority) {
  int was_empty;

  if (badge_request_rejected(key, url))
    return;
  if (badge_request_cached(key, high_priority))
    return;
  if (!badge_request_ensure_worker())
    return;
  if (!badge_request_enqueue(key, url, high_priority, &was_empty))
    return;

  /* One line where a burst starts, not one per badge: 200 queued lines compete for
     * the same ux0 card the game streams its ISO from. */
  if (was_empty)
    ra_badge_log("badge: queue start %s\n", key);
  sceKernelSignalSema(g_badge_wake, 1);
}

void ra_badge_log(const char *format, ...) {
  char text[256];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length > 0)
    ra_logq_line(text, length);
}









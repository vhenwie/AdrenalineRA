#include "ra_badge_fetch.h"
#include "ra_badge_blob.h"
#include "ra_badge_png.h"
#include "ra_badge_texture.h"
#include "ra_badge_internal.h"
#include "ra_http.h"

#include <string.h>

#include <psp2/kernel/threadmgr.h>

#define RA_BADGE_MAX_BYTES (128 * 1024)

static unsigned int g_last_write_ms;

unsigned int ra_badge_last_write_ms(void) { return g_last_write_ms; }

static int badge_download(const RaBadgeJob *job, ra_http_response_t *response) {
  if (ra_http_get(job->url, "image/png,image/*;q=0.8", response) != 0 ||
      response->status_code != 200 || response->body == NULL ||
      response->body_length == 0) {
    ra_badge_log("badge: download failed for %s (status=%d stage=%d err=0x%08X ssl=%d/0x%08X)\n",
        job->key, response->status_code, ra_http_get_last_stage(),
        (unsigned)ra_http_get_last_error(), ra_http_get_last_ssl_error(),
        ra_http_get_last_ssl_detail());
    return 0;
  }
  if (response->body_length > RA_BADGE_MAX_BYTES ||
      !ra_badge_png_header_ok((const unsigned char *)response->body,
                              response->body_length, NULL, NULL)) {
    ra_badge_log("badge: invalid PNG for %s (%u bytes)\n", job->key,
        (unsigned int)response->body_length);
    return 0;
  }
  return 1;
}

static int badge_hold_prime(const RaBadgeJob *job,
                            const ra_http_response_t *response) {
  if (!ra_badge_blob_store(job->key, (const unsigned char *)response->body,
                           response->body_length)) {
    ra_badge_log("badge: could not hold %s in memory\n", job->key);
    return 0;
  }
  ra_badge_log("badge: held %s in memory (%u bytes)\n", job->key,
      (unsigned int)response->body_length);
  ra_badge_prime_queue(job->key);
  return 1;
}

static int badge_persist(const RaBadgeJob *job,
                         const ra_http_response_t *response) {
  unsigned long long write_start_us =
    (unsigned long long)sceKernelGetSystemTimeWide();

  if (!ra_badge_png_write_atomic(job->key,
          (const unsigned char *)response->body, response->body_length)) {
    ra_badge_log("badge: cache write failed for %s\n", job->key);
    return 0;
  }
  g_last_write_ms =
    (unsigned)(((unsigned long long)sceKernelGetSystemTimeWide() -
                write_start_us) / 1000);
  ra_badge_log("badge: cached %s (%u bytes) in %u ms\n", job->key,
      (unsigned int)response->body_length, g_last_write_ms);
  ra_badge_note_present(job->key);
  return 1;
}

int ra_badge_fetch(const RaBadgeJob *job) {
  ra_http_response_t response;

  if (job == NULL || !ra_badge_enabled() || ra_badge_stopped())
    return 0;
  memset(&response, 0, sizeof(response));
  if (!badge_download(job, &response))
    goto fail;
  if (!ra_badge_enabled() || ra_badge_stopped())
    goto fail;
  if (job->prime && !badge_hold_prime(job, &response))
    goto fail;
  if (!badge_persist(job, &response)) {
    ra_http_response_free(&response);
    return job->prime ? 1 : 0;
  }
  ra_http_response_free(&response);
  return 1;
fail:
  ra_http_response_free(&response);
  return 0;
}

void ra_badge_pause(unsigned int ms) {
  unsigned int slept = 0;

  while (slept < ms && ra_badge_enabled() && !ra_badge_stopped()) {
    sceKernelDelayThread(50 * 1000);
    slept += 50;
  }
}

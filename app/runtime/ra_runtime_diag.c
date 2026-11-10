#include "ra_runtime_diag.h"
#include "ra_runtime_internal.h"

#include <psp2/kernel/processmgr.h>

#include "ra_runtime.h"
#include "ra_http.h"
#include "ra_notify.h"
#include "ra_netqueue.h"
#include "ra_offline.h"

int ra_runtime_info_ready(void);
unsigned long long ra_runtime_load_parse_us(void);

void ra_runtime_client_log(const char *message, void *userdata) {
  (void)userdata;
  if (message == NULL)
    return;

  /* While a load is being parsed, tag every rcheevos line with the time since the payload
     * became available. The parse is one blocking call, so this is the only way to tell which
     * phase inside it costs what : the memref index took a large title from 33 s to
     * 18.8 s and the rest is still unexplained). */
  if (ra_runtime_load_parse_us() != 0 && !ra_runtime_info_ready()) {
    unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

    ra_runtime_log("rc: [+%llu ms] %s\n", (now - ra_runtime_load_parse_us()) / 1000ULL, message);
    return;
  }

  ra_runtime_log("rc: %s\n", message);
}


int ra_runtime_net_failed(void);
void ra_runtime_set_net_failed(int failed);
int ra_runtime_in_game(void);

void ra_runtime_request_timing(const char *request, unsigned int ms,
                int http_status) {
  ra_runtime_log("runtime: request r=%s took %u ms status=%d\n",
        request != NULL ? request : "?", ms, http_status);

  /* A failed request that is not a server error leaves no trace otherwise: the award
     * log lines only say "Network error (0)", which cannot distinguish a link gate from a
     * connection create from a handshake. `stage` and `err` are what named the 
     * fast failures, so they are logged for every non-200 too. */
  if (http_status != 200) {
    ra_runtime_log("runtime: request detail stage=%d err=0x%08X net=0x%08X\n",
          ra_http_get_last_stage(),
          (unsigned)ra_http_get_last_error(),
          (unsigned)ra_http_get_last_net_state());
  }

  /* Link recovery also comes from a successful request after a failure streak: netctl
 * can report connected while every request fails. */
  if (http_status > 0) {
    if (ra_runtime_net_failed()) {
      ra_runtime_set_net_failed(0);
      ra_runtime_log("runtime: server reachable again after failures\n");
      /* Replay only inside a game session with something pending : the
             * queue is replayed at `session ready` as well, and a recovery in the XMB has
             * no session to sync into. */
      if (!ra_runtime_offline_at_launch() && ra_runtime_in_game() &&
          ra_offline_queue_pending() > 0)
        ra_netqueue_replay_offline();
      if (!ra_runtime_offline_at_launch())
        ra_runtime_reconnect_note(0);
    }
  } else if (request != NULL) {
    ra_runtime_set_net_failed(1);
    ra_runtime_reconnect_reset();
  }

  /* RetroAchievements answers 200 for an accepted *and *a refused award, so
     * the HTTP status alone cannot say whether an unlock reached the site - the
     * body can. Body is token-masked by the transport; the response head is the
     * server's own words. Only for the requests that decide an outcome, so this
     * never becomes noise. */
  if (request != NULL &&
    (strcmp(request, "awardachievement") == 0 ||
    strcmp(request, "submitlbentry") == 0 ||
    strcmp(request, "startsession") == 0 ||
    /* Log the unlock counts from both payloads; an empty list is otherwise
 * indistinguishable from the server having sent the progress elsewhere. */
    strcmp(request, "achievementsets") == 0 ||
    strcmp(request, "lbinfo") == 0)) {
    ra_runtime_log("runtime: request body='%s'\n",
          ra_http_get_last_request_body());
    ra_runtime_log("runtime: response head='%s'\n",
          ra_http_get_last_body_prefix());
  }
}

void ra_runtime_award_reported(unsigned int achievement_id, int hardcore,
                int confirmed) {
  if (achievement_id == 0)
    return;

  ra_runtime_log("runtime: award %s %u (%c)\n",
        confirmed ? "confirmed" : "NOT confirmed",
        achievement_id, hardcore ? 'h' : 's');
}

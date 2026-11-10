#include "ra_runtime_boot_auth.h"

#include "ra_runtime_internal.h"
#include <stdio.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/openpsid.h>
#include <psp2/power.h>
#include "ra_badge.h"
#include "ra_client.h"
#include "main.h"
#include "ra_credentials.h"
#include "ra_hash.h"
#include "ra_http.h"
#include "ra_netqueue.h"
#include "ra_notify.h"
#include "ra_runtime_cards.h"
#include "ra_runtime_core.h"
#include "ra_runtime_challenges.h"
#include "ra_runtime_diag.h"
#include "ra_runtime_disc_serial.h"
#include "ra_runtime_events.h"
#include "ra_runtime_hold.h"
#include "ra_runtime_load.h"
#include "ra_runtime_memory.h"
#include "ra_runtime_path.h"
#include "ra_runtime_platform.h"
#include "ra_runtime_plugins.h"
#include "ra_server.h"
#include "ra_session.h"
#include "ra_settings.h"
#include "ra_shot.h"

/* Preconditions and network init. Returns 0 when boot auth must be skipped. */
static int boot_auth_begin(void) {
  ra_runtime_load_settings();
  if (!g_rt.settings.ra_enabled || !ra_credentials_exists()) {
    ra_runtime_log("runtime: boot auth skipped (disabled or no credentials)\n");
    return 0;
  }
  if (!ra_http_link_up()) {
    ra_runtime_log("runtime: boot auth skipped (no network)\n");
    return 0;
  }

  ra_http_set_session_active(1);
  /* No game yet: the boot probe reports the PSP identity. */
  ra_http_set_console_identity(0, NULL);
  if (ra_http_init() != 0) {
    ra_runtime_log("runtime: boot auth failed (network init)\n");
    return 0;
  }
  ra_server_set_transport(ra_http_post);
  ra_server_set_timing_reporter(ra_runtime_request_timing);
  ra_server_set_award_reporter(ra_runtime_award_reported);
  return 1;
}

/* Console identity, config and the client itself. */
static int boot_auth_create_session(void) {
  SceKernelOpenPsId psid;
  ra_session_config_t config;
  const char *vault_path;

  if (sceKernelGetOpenPsId(&psid) < 0) {
    ra_runtime_log("runtime: boot auth failed (console id)\n");
    return 0;
  }
  vault_path = ra_runtime_choose_vault_path();

  memset(&config, 0, sizeof(config));
  config.device_id = (const uint8_t*)psid.id;
  config.vault_path = vault_path;
  config.game_hash = NULL;
  config.read_memory = ra_runtime_memory_read;
  config.event_handler = ra_runtime_events_dispatch;
  config.transport = ra_http_post;
  config.status_callback = ra_runtime_status;
  config.userdata = NULL;
  ra_netqueue_init();
  config.server_call = ra_netqueue_server_call;

  g_rt.session = ra_session_create(&config);
  if (g_rt.session == NULL) {
    ra_netqueue_stop();
    ra_runtime_log("runtime: boot auth failed (vault/client)\n");
    return 0;
  }
  ra_client_set_pump(ra_session_get_client(g_rt.session), ra_netqueue_pump);

  ra_runtime_apply_client_settings();
  return 1;
}

static int boot_auth_start_login(void) {
  int result = ra_session_start(g_rt.session);

  if (result != RC_OK) {
    ra_runtime_log("runtime: boot auth failed (login start %d)\n", result);
    ra_runtime_destroy_session();
    return 0;
  }
  return 1;
}

/* Pumps the client until the login lands or the boot-auth cap expires. */
static void boot_auth_await_ready(void) {
  unsigned long long deadline;

  /* Boot auth runs on the worker before any game is loaded. A fixed 2 s cap
     * was too short for a cold login while Wi-Fi settles; a failed boot auth
     * moves the whole login into game start and adds seconds to session-ready. */
  deadline = (unsigned long long)sceKernelGetSystemTimeWide() +
    RA_RUNTIME_BOOT_AUTH_US;

  while (!ra_session_is_logged_in(g_rt.session) && !g_rt.stop &&
    (unsigned long long)sceKernelGetSystemTimeWide() < deadline) {
    ra_session_do_frame(g_rt.session);
    sceKernelDelayThread(10 * 1000);
  }

  if (!ra_session_is_logged_in(g_rt.session)) {
    /* Name the cause: a 5xx here is RetroAchievements being unavailable (their
         * outage), not a local network problem - ` 19:01` answered every
         * request with `{"message":"Service Unavailable"}` and status 503. */
    ra_runtime_log("runtime: boot auth failed (login not accepted, http=%d)\n",
          ra_http_get_last_status());
    ra_runtime_destroy_session();
    return;
  }

  ra_runtime_log("runtime: boot auth ready\n");
}

void ra_runtime_boot_auth(void) {
  if (!boot_auth_begin())
    return;
  if (!boot_auth_create_session())
    return;
  if (!boot_auth_start_login())
    return;
  boot_auth_await_ready();
}

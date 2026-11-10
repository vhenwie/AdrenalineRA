#include "ra_http_template.h"
#include "ra_http_internal.h"
#include "ra_http.h"

#include <stdio.h>

#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/libssl.h>
#include <psp2/kernel/threadmgr.h>

#include <stdlib.h>
#include <string.h>

#include "ra_log.h"

#define RA_HTTP_POOL_SIZE (256 * 1024)
#define RA_SSL_POOL_SIZE (256 * 1024)
#define RA_HTTP_TIMEOUT_USEC (30 * 1000 * 1000)
#define RA_HTTP_IMAGE_TIMEOUT_USEC (10 * 1000 * 1000)




static int g_http_initialized;
static int g_http_ua_stale;
static int g_ssl_initialized;

static int http_image_trylock(void) {
  if (g_http_image_mutex < 0)
    g_http_image_mutex = sceKernelCreateMutex("ra_http_image", 0, 0, NULL);
  if (g_http_image_mutex < 0)
    return 0;
  return sceKernelTryLockMutex(g_http_image_mutex, 1) >= 0;
}

static int create_image_template(void);

/* Template teardown has no locking of its own: every caller here already holds
 * the gate, or is about to take it. */
static void http_drop_templates_locked(void) {
  if (g_http_template >= 0) {
    sceHttpDeleteTemplate(g_http_template);
    g_http_template = -1;
  }
  if (g_http_image_template >= 0) {
    sceHttpDeleteTemplate(g_http_image_template);
    g_http_image_template = -1;
  }
}

void ra_http_drop_templates(void) {
  int locked = 0;

  if (g_http_gate < 0)
    g_http_gate = sceKernelCreateMutex("ra_http_gate", 0, 0, NULL);
  if (g_http_gate >= 0)
    locked = sceKernelLockMutex(g_http_gate, 1, NULL) >= 0;

  http_drop_templates_locked();

  if (locked)
    sceKernelUnlockMutex(g_http_gate, 1);
}

void ra_http_set_console_identity(int is_ps1, const char *override_ua) {
  (void)is_ps1;
  (void)override_ua;
  snprintf(g_user_agent, sizeof(g_user_agent), "%s", RA_HTTP_UA_PROD);
}

static int create_image_template(void) {
  int result;

  if (g_http_image_template >= 0)
    return 0;

  result = sceHttpCreateTemplate(g_user_agent, SCE_HTTP_VERSION_1_1, 1);
  if (result < 0) {
    g_http_image_template = -1;
    return result;
  }

  g_http_image_template = result;
  sceHttpSetResolveTimeOut(g_http_image_template, RA_HTTP_IMAGE_TIMEOUT_USEC);
  sceHttpSetConnectTimeOut(g_http_image_template, RA_HTTP_IMAGE_TIMEOUT_USEC);
  sceHttpSetSendTimeOut(g_http_image_template, RA_HTTP_IMAGE_TIMEOUT_USEC);
  sceHttpSetRecvTimeOut(g_http_image_template, RA_HTTP_IMAGE_TIMEOUT_USEC);
  return 0;
}

/* Caller holds g_http_gate. Resetting the stack while another thread still owns
 * a request or a template tears SceHttp/SSL out from under it. */
static void http_stack_reset(void) {
  g_http_needs_reset = 0;
  http_drop_templates_locked();
  (void)sceHttpTerm();
  (void)sceSslTerm();
  g_http_initialized = 0;
  g_ssl_initialized = 0;
  ra_log_append("RA_SETUP",
        "http: stack reset after a failed request\n",
        (int)strlen("http: stack reset after a failed request\n"));
}

static int http_load_modules(void) {
  int result;

  g_http_last_stage = 1;
  result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  if (result < 0) {
    g_http_last_error = result;
    return result;
  }
  g_http_last_stage = 2;
  result = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
  if (result < 0) {
    /* HTTPS normally pulls in the HTTP module; keep going and let
         * sceHttpInit report the definitive error if it is really missing. */
    g_http_last_error = result;
  }
  g_http_last_stage = 3;
  /* HTTPS pulls SSL in as a dependency on every firmware we target, but say it explicitly:
     * the module list is the one thing that can differ between 3.60 and 3.74, and `sceSslInit`
     * below must not depend on another loader having run first . Tolerant like the
     * HTTP load above - the definitive error comes from sceSslInit. */
  result = sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
  if (result < 0)
    g_http_last_error = result;
  result = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
  if (result < 0) {
    g_http_last_error = result;
    return result;
  }
  return 0;
}

static void http_init_net(void) {
  static char net_memory[16 * 1024];
  SceNetInitParam net_param;
  int result;

  g_http_last_stage = 4;
  net_param.memory = net_memory;
  net_param.size = sizeof(net_memory);
  net_param.flags = 0;
  result = sceNetInit(&net_param);
  if (result < 0) {
    /* Already-initialized is fine; other failures surface from sceHttpInit below. */
    g_http_last_error = result;
  }
  g_http_last_stage = 5;
  (void)sceNetCtlInit(); {
    int net_state = -1;
    if (sceNetCtlInetGetState(&net_state) == 0)
      g_http_last_net_state = net_state;
  }
}

static int http_init_ssl_http(void) {
  int result;

  g_http_last_stage = 6;
  result = sceSslInit(RA_SSL_POOL_SIZE);
  if (result == 0) {
    g_ssl_initialized = 1;
  } else if (result != SCE_SSL_ERROR_ALREADY_INITED) {
    g_http_last_error = result;
    return result;
  }

  g_http_last_stage = 7;
  result = sceHttpInit(RA_HTTP_POOL_SIZE);
  if (result < 0 && result != SCE_HTTP_ERROR_ALREADY_INITED) {
    g_http_last_error = result;
    return result;
  }

  g_http_last_stage = 8;
  /* Verify the server chain, CN and validity window. KNOWN_CA_CHECK is not
     * used here: it rejects otherwise-valid chains whose root is not in the
     * console's preinstalled list. */
  result = sceHttpsEnableOption(SCE_HTTPS_FLAG_SERVER_VERIFY |
                SCE_HTTPS_FLAG_CN_CHECK |
                SCE_HTTPS_FLAG_NOT_AFTER_CHECK |
                SCE_HTTPS_FLAG_NOT_BEFORE_CHECK);
  if (result < 0) {
    g_http_last_error = result;
    return result;
  }
  return 0;
}

static int http_init_templates(void) {
  g_http_last_stage = 9;
  g_http_template = sceHttpCreateTemplate(g_user_agent,
                      SCE_HTTP_VERSION_1_1, 1);
  if (g_http_template < 0) {
    g_http_last_error = g_http_template;
    return g_http_template;
  }

  sceHttpSetResolveTimeOut(g_http_template, RA_HTTP_TIMEOUT_USEC);
  sceHttpSetConnectTimeOut(g_http_template, RA_HTTP_TIMEOUT_USEC);
  sceHttpSetSendTimeOut(g_http_template, RA_HTTP_TIMEOUT_USEC);
  sceHttpSetRecvTimeOut(g_http_template, RA_HTTP_TIMEOUT_USEC);

  /* Badge images are optional; do not fail the whole session if the second
     * template cannot be allocated. */
  (void)create_image_template();
  /* One gate for both templates: created here, on the init path, so the runtime
     * thread and the badge worker can never build two of them. */
  if (g_http_gate < 0)
    g_http_gate = sceKernelCreateMutex("ra_http_gate", 0, 0, NULL);
  g_http_last_stage = 0;
  g_http_last_error = 0;
  g_http_initialized = 1;
  return 0;
}

/* Caller holds g_http_gate. The failed-handshake reset and template rebuild
 * both touch SceHttp/SSL, so they must never overlap an in-flight request. */
int ra_http_init_locked(void) {
  int result;

  /* Recover SceHttp/SSL after a failed handshake; one bad request must not poison the session. */
  if (g_http_needs_reset) {
    http_stack_reset();
  }

  if (g_http_initialized && !g_http_ua_stale) {
    /* A previous shutdown may have removed only the image template. */
    (void)create_image_template();
    return 0;
  }
  if (g_http_ua_stale) {
    /* A console switch changed the identity: the templates still carry the
         * previous User-Agent, so they are rebuilt by the code below. */
    g_http_ua_stale = 0;
    http_drop_templates_locked();
  }

  result = http_load_modules();
  if (result < 0)
    return result;
  http_init_net();
  result = http_init_ssl_http();
  if (result < 0)
    return result;
  return http_init_templates();
}

int ra_http_init(void) {
  int result;

  ra_http_gate_lock();
  result = ra_http_init_locked();
  ra_http_gate_unlock();
  return result;
}

void ra_http_set_session_active(int active) {
  g_http_session_active = active != 0;
}

void ra_http_shutdown(void) {
  if (g_http_session_active)
    return;

  /* Serialize teardown with a badge GET. ra_http_get holds this lock for the
     * whole request, so the template cannot be deleted underneath SceHttp. If a
     * fetch is in flight, leave the stack initialized and let the next call
     * retry rather than blocking the menu worker on a slow network. */
  if (!http_image_trylock())
    return;
  if (g_http_image_active) {
    http_image_unlock();
    return;
  }
  if (g_http_image_template >= 0) {
    sceHttpDeleteTemplate(g_http_image_template);
    g_http_image_template = -1;
  }
  http_image_unlock();

  if (g_http_template >= 0) {
    sceHttpDeleteTemplate(g_http_template);
    g_http_template = -1;
  }
  if (g_http_initialized) {
    sceHttpTerm();
    g_http_initialized = 0;
  }
  if (g_ssl_initialized) {
    sceSslTerm();
    g_ssl_initialized = 0;
  }
}

int ra_http_link_up(void) {
  /* Network init is not fatal when already initialised; the state query is the real test. */
  static int netctl_ready;
  int state = -1;

  if (!netctl_ready) {
    (void)sceNetCtlInit();
    netctl_ready = 1;
  }
  if (sceNetCtlInetGetState(&state) != 0) {
    /* The stack is not answering at all: allow the next call to try again. */
    netctl_ready = 0;
    return 0;
  }
  return state == SCE_NETCTL_STATE_CONNECTED;
}



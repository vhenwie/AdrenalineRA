#include "ra_menu_worker.h"

#include <psp2/kernel/openpsid.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_credentials.h"
#include "ra_http.h"
#include "ra_server.h"
#include "ra_setup.h"
#include "ra_vault.h"
#include "rc_api_user.h"
#include "rc_client.h"
#include "rc_error.h"

#define WORKER_STACK 0x10000
#define WORKER_PRIORITY 0x10000100
#define PASSWORD_MAX 256

typedef struct {
  int done;
  int result;
  char error[192];
  char username[RA_VAULT_MAX_USERNAME];
  unsigned int score;
} ProfileJob;

static SceUID g_lock = -1;
static SceUID g_request = -1;
static SceUID g_thread = -1;
static volatile int g_stop;
static RaJobKind g_pending;
static RaJobKind g_last_kind;
static RaJobState g_state;
static char g_stage[96];
static char g_message[192];
static char g_username[RA_VAULT_MAX_USERNAME];
static unsigned int g_score;
static int g_score_valid;
static unsigned long long g_finished_us;
static char g_pending_user[RA_VAULT_MAX_USERNAME];
static char g_pending_password[PASSWORD_MAX];

static void zero_memory(void *data, size_t length) {
  volatile unsigned char *bytes = (volatile unsigned char*)data;
  while (length-- > 0)
    *bytes++ = 0;
}

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source != NULL ? source : "");
}

static void lock_state(void) {
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

static void unlock_state(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

static void set_stage(const char *stage) {
  lock_state();
  copy_text(g_stage, sizeof(g_stage), stage);
  unlock_state();
}

static void set_result(RaJobState state, const char *message,
          const char *username, unsigned int score, int score_valid) {
  lock_state();
  g_state = state;
  copy_text(g_message, sizeof(g_message), message);
  if (username != NULL)
    copy_text(g_username, sizeof(g_username), username);
  g_score = score;
  g_score_valid = score_valid;
  g_finished_us = (unsigned long long)sceKernelGetSystemTimeWide();
  unlock_state();
}

static void network_error(char *message, size_t capacity, const char *prefix) {
  snprintf(message, capacity, "%s (stage %d, 0x%08X)", prefix,
      ra_http_get_last_stage(), (unsigned)ra_http_get_last_error());
}

static int run_signin(char *message, size_t capacity, char *username,
          size_t username_capacity) {
  SceKernelOpenPsId psid;
  char canonical[RA_VAULT_MAX_USERNAME];
  char error[192];
  int login_result;

  copy_text(message, capacity, "Initializing network...");
  set_stage(message);
  if (ra_http_init() != 0) {
    network_error(message, capacity, "Network init failed");
    ra_http_shutdown();
    return -1;
  }

  copy_text(message, capacity, "Contacting RetroAchievements...");
  set_stage(message);
  canonical[0] = '\0';
  error[0] = '\0';
  login_result = -1;
  if (sceKernelGetOpenPsId(&psid) < 0) {
    copy_text(message, capacity, "Could not read the console ID.");
  } else {
    login_result = ra_setup_login_to_vault(g_pending_user,
      g_pending_password, (const uint8_t*)psid.id, RA_CREDENTIALS_UR0,
      canonical, sizeof(canonical), error, sizeof(error));
    if (login_result != 0)
      login_result = ra_setup_login_to_vault(g_pending_user,
        g_pending_password, (const uint8_t*)psid.id, RA_CREDENTIALS_UX0,
        canonical, sizeof(canonical), error, sizeof(error));
  }

  if (login_result != 0) {
    copy_text(message, capacity,
        error[0] != '\0' ? error : "Login failed.");
  } else if (canonical[0] == '\0') {
    login_result = -1;
    copy_text(message, capacity,
        "Login succeeded but the username was missing.");
  } else {
    copy_text(username, username_capacity, canonical);
    copy_text(message, capacity, "Signed in.");
  }
  zero_memory(g_pending_password, sizeof(g_pending_password));
  zero_memory(canonical, sizeof(canonical));
  ra_http_shutdown();
  return login_result;
}

static int run_verify_network(char *message, size_t capacity) {
  int result;
  int net_state;

  copy_text(message, capacity, "Initializing network...");
  set_stage(message);
  result = ra_http_init();
  net_state = ra_http_get_last_net_state();
  ra_http_shutdown();
  if (result != 0) {
    network_error(message, capacity, "Network init failed");
    return -1;
  }
  snprintf(message, capacity, "Network OK (net_state=%d).", net_state);
  return 0;
}

static uint32_t profile_read_memory(uint32_t address, uint8_t *buffer,
                  uint32_t num_bytes, rc_client_t *client) {
  (void)address;
  (void)buffer;
  (void)num_bytes;
  (void)client;
  return 0;
}

static void profile_complete(int result, const char *error_message,
              rc_client_t *client, void *userdata) {
  ProfileJob *job = (ProfileJob*)userdata;
  const rc_client_user_t *user;

  job->done = 1;
  job->result = result;
  if (error_message != NULL)
    copy_text(job->error, sizeof(job->error), error_message);
  if (result != RC_OK)
    return;
  user = rc_client_get_user_info(client);
  if (user == NULL) {
    job->result = RC_INVALID_STATE;
    copy_text(job->error, sizeof(job->error),
        "profile response was missing the user");
    return;
  }
  if (user->username != NULL)
    copy_text(job->username, sizeof(job->username), user->username);
  job->score = user->score;
}

static int run_verify_account(char *message, size_t capacity,
              char *username, size_t username_capacity,
              unsigned int *score, int *score_valid) {
  char token[RA_VAULT_MAX_TOKEN];
  ProfileJob job;
  rc_client_t *client;
  int result;

  *score = 0;
  *score_valid = 0;
  if (!ra_credentials_load(username, username_capacity, token, sizeof(token))) {
    copy_text(message, capacity, "Not signed in.");
    return -1;
  }
  memset(&job, 0, sizeof(job));
  copy_text(job.username, sizeof(job.username), username);

  copy_text(message, capacity, "Initializing network...");
  set_stage(message);
  result = ra_http_init();
  if (result != 0) {
    zero_memory(token, sizeof(token));
    network_error(message, capacity, "Network init failed");
    ra_http_shutdown();
    return -1;
  }

  copy_text(message, capacity, "Verifying account...");
  set_stage(message);
  ra_server_set_transport(ra_http_post);
  client = rc_client_create(profile_read_memory, ra_server_call);
  if (client == NULL) {
    ra_http_shutdown();
    zero_memory(token, sizeof(token));
    copy_text(message, capacity, "Could not create the RA client.");
    return -1;
  }

  rc_client_begin_login_with_token(client, username, token,
                  profile_complete, &job);
  ra_http_shutdown();
  rc_client_destroy(client);
  zero_memory(token, sizeof(token));

  if (!job.done || job.result != RC_OK) {
    snprintf(message, capacity,
        "Account check failed: %s (stage %d, 0x%08X)",
        job.error[0] != '\0' ? job.error : "unknown error",
        ra_http_get_last_stage(), (unsigned)ra_http_get_last_error());
    return -1;
  }
  if (job.username[0] != '\0')
    copy_text(username, username_capacity, job.username);
  *score = job.score;
  *score_valid = 1;
  snprintf(message, capacity, "Account OK: %s (%u %s).", username, *score,
             *score == 1 ? "point" : "points");
  return 0;
}

static int worker_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    RaJobKind kind;
    char message[192];
    char username[RA_VAULT_MAX_USERNAME];
    char canonical[RA_VAULT_MAX_USERNAME];
    unsigned int score = 0;
    int score_valid = 0;
    int result;

    sceKernelWaitSema(g_request, 1, NULL);
    if (g_stop)
      break;
    lock_state();
    kind = g_pending;
    g_pending = RA_JOB_NONE;
    copy_text(username, sizeof(username), g_pending_user);
    copy_text(g_username, sizeof(g_username), g_pending_user);
    g_state = RA_JOB_RUNNING;
    copy_text(g_stage, sizeof(g_stage), "Working...");
    unlock_state();

    message[0] = '\0';
    canonical[0] = '\0';
    switch (kind) {
    case RA_JOB_SIGN_IN:
      result = run_signin(message, sizeof(message), canonical,
                sizeof(canonical));
      if (result == 0)
        copy_text(username, sizeof(username), canonical);
      break;
    case RA_JOB_VERIFY_NETWORK:
      result = run_verify_network(message, sizeof(message));
      break;
    case RA_JOB_VERIFY_ACCOUNT:
      result = run_verify_account(message, sizeof(message), username,
                    sizeof(username), &score, &score_valid);
      break;
    default:
      result = -1;
      copy_text(message, sizeof(message), "Unknown RA job.");
      break;
    }

    zero_memory(g_pending_password, sizeof(g_pending_password));
    zero_memory(g_pending_user, sizeof(g_pending_user));
    zero_memory(canonical, sizeof(canonical));
    set_result(result == 0 ? RA_JOB_DONE_OK : RA_JOB_DONE_ERROR,
        message[0] != '\0' ? message : "Operation finished.",
        username[0] != '\0' ? username : NULL, score, score_valid);
  }
  return 0;
}

void ra_worker_shutdown(void) {
  if (g_thread < 0)
    return;
  g_stop = 1;
  if (g_request >= 0)
    sceKernelSignalSema(g_request, 1);
  {
    SceUInt timeout = 1000 * 1000;
    if (sceKernelWaitThreadEnd(g_thread, NULL, &timeout) < 0)
      sceKernelDeleteThread(g_thread);
  }
  g_thread = -1;
  if (g_request >= 0) {
    sceKernelDeleteSema(g_request);
    g_request = -1;
  }
  if (g_lock >= 0) {
    sceKernelDeleteSema(g_lock);
    g_lock = -1;
  }
}

static int worker_ensure(void) {
  if (g_thread >= 0)
    return 0;
  g_stop = 0;
  g_lock = sceKernelCreateSema("ra_menu_lock", 0, 1, 1, NULL);
  if (g_lock < 0)
    return -1;
  g_request = sceKernelCreateSema("ra_menu_request", 0, 0, 1, NULL);
  if (g_request < 0) {
    sceKernelDeleteSema(g_lock);
    g_lock = -1;
    return -1;
  }
  g_thread = sceKernelCreateThread("ra_menu_worker", worker_thread,
                  WORKER_PRIORITY, WORKER_STACK, 0, 0, NULL);
  if (g_thread < 0) {
    sceKernelDeleteSema(g_request);
    sceKernelDeleteSema(g_lock);
    g_request = -1;
    g_lock = -1;
    return -1;
  }
  if (sceKernelStartThread(g_thread, 0, NULL) < 0) {
    sceKernelDeleteThread(g_thread);
    g_thread = -1;
    sceKernelDeleteSema(g_request);
    sceKernelDeleteSema(g_lock);
    g_request = -1;
    g_lock = -1;
    return -1;
  }
  return 0;
}

static int start_job(RaJobKind kind, const char *username,
          const char *password) {
  if (worker_ensure() != 0)
    return -1;

  lock_state();
  if (g_state != RA_JOB_IDLE || g_pending != RA_JOB_NONE) {
    unlock_state();
    return -1;
  }
  memset(g_pending_user, 0, sizeof(g_pending_user));
  memset(g_pending_password, 0, sizeof(g_pending_password));
  if (username != NULL)
    copy_text(g_pending_user, sizeof(g_pending_user), username);
  if (password != NULL)
    copy_text(g_pending_password, sizeof(g_pending_password), password);
  g_pending = kind;
  g_last_kind = kind;
  g_state = RA_JOB_RUNNING;
  g_score_valid = 0;
  g_score = 0;
  g_message[0] = '\0';
  g_finished_us = 0;
  copy_text(g_stage, sizeof(g_stage), "Starting...");
  unlock_state();
  sceKernelSignalSema(g_request, 1);
  return 0;
}

int ra_worker_start_signin(const char *username, const char *password) {
  if (username == NULL || username[0] == '\0' ||
      password == NULL || password[0] == '\0')
    return -1;
  return start_job(RA_JOB_SIGN_IN, username, password);
}

int ra_worker_start_verify_network(void) {
  return start_job(RA_JOB_VERIFY_NETWORK, NULL, NULL);
}

int ra_worker_start_verify_account(void) {
  return start_job(RA_JOB_VERIFY_ACCOUNT, NULL, NULL);
}

void ra_worker_snapshot(RaJobSnapshot *snapshot) {
  if (snapshot == NULL)
    return;
  memset(snapshot, 0, sizeof(*snapshot));
  if (g_lock < 0)
    return;
  lock_state();
  snapshot->state = g_state;
  snapshot->kind = g_last_kind;
  copy_text(snapshot->stage, sizeof(snapshot->stage), g_stage);
  copy_text(snapshot->message, sizeof(snapshot->message), g_message);
  copy_text(snapshot->username, sizeof(snapshot->username), g_username);
  snapshot->score = g_score;
  snapshot->score_valid = g_score_valid;
  snapshot->finished_us = g_finished_us;
  unlock_state();
}

void ra_worker_consume(void) {
  if (g_lock < 0)
    return;
  lock_state();
  if (g_state == RA_JOB_DONE_OK || g_state == RA_JOB_DONE_ERROR)
    g_state = RA_JOB_IDLE;
  unlock_state();
}

int ra_worker_busy(void) {
  int busy = 0;

  if (g_lock < 0)
    return 0;
  lock_state();
  busy = g_state == RA_JOB_RUNNING || g_pending != RA_JOB_NONE;
  unlock_state();
  return busy;
}

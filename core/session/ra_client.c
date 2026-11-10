#include "ra_client.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

struct ra_client {
  rc_client_t *rc;
  ra_client_state_t state;
  rc_client_event_handler_t event_handler;
  void *event_userdata;
  ra_client_log_callback_t log_callback;
  void *log_userdata;
  ra_client_pump_t pump;
  ra_client_done_callback_t pending_callback;
  void *pending_userdata;
  int pending_is_load;
  int eval_hold;
  SceUID lock;
  volatile SceUID lock_owner;
  int lock_depth;
};

void ra_client_lock(ra_client_t *client) {
  SceUID self;

  if (client == NULL || client->lock < 0)
    return;
  self = sceKernelGetThreadId();
  if (client->lock_owner == self) {
    ++client->lock_depth;
    return;
  }
  sceKernelWaitSema(client->lock, 1, NULL);
  client->lock_owner = self;
  client->lock_depth = 1;
}

void ra_client_unlock(ra_client_t *client) {
  if (client == NULL || client->lock < 0)
    return;
  if (--client->lock_depth > 0)
    return;
  client->lock_owner = -1;
  sceKernelSignalSema(client->lock, 1);
}

static void dispatch_log(const char *message, const rc_client_t *rc) {
  ra_client_t *client = (ra_client_t*)rc_client_get_userdata((rc_client_t*)rc);

  if (client != NULL && client->log_callback != NULL)
    client->log_callback(message, client->log_userdata);
}

static void dispatch_event(const rc_client_event_t *event, rc_client_t *rc) {
  ra_client_t *client = (ra_client_t*)rc_client_get_userdata(rc);

  if (client != NULL && client->event_handler != NULL)
    client->event_handler(event, rc);
}

static void finish(ra_client_t *client, int result, const char *error_message) {
  ra_client_done_callback_t callback;
  void *userdata;
  int is_load;

  if (client == NULL)
    return;
  callback = client->pending_callback;
  userdata = client->pending_userdata;
  is_load = client->pending_is_load;
  client->pending_callback = NULL;
  client->pending_userdata = NULL;
  client->pending_is_load = 0;
  client->state = result == RC_OK
    ? (is_load ? RA_CLIENT_STATE_READY : RA_CLIENT_STATE_LOGGED_IN)
    : RA_CLIENT_STATE_ERROR;
  if (callback != NULL)
    callback(result, error_message, userdata);
}

static void login_complete(int result, const char *error_message,
                  rc_client_t *rc, void *userdata) {
  (void)rc;
  finish((ra_client_t*)userdata, result, error_message);
}

static void load_complete(int result, const char *error_message,
                  rc_client_t *rc, void *userdata) {
  (void)rc;
  finish((ra_client_t*)userdata, result, error_message);
}

ra_client_t *ra_client_create(const ra_client_config_t *config) {
  ra_client_t *client;

  if (config == NULL || config->read_memory == NULL ||
      config->server_call == NULL)
    return NULL;
  client = (ra_client_t*)calloc(1, sizeof(*client));
  if (client == NULL)
    return NULL;
  client->rc = rc_client_create(config->read_memory, config->server_call);
  if (client->rc == NULL) {
    free(client);
    return NULL;
  }
  client->event_handler = config->event_handler;
  client->event_userdata = config->userdata;
  client->log_callback = config->log_callback;
  client->log_userdata = config->userdata;
  client->state = RA_CLIENT_STATE_IDLE;
  client->lock = sceKernelCreateSema("ra_client", 0, 1, 1, NULL);
  client->lock_owner = -1;
  rc_client_set_userdata(client->rc, client);
  rc_client_set_event_handler(client->rc, dispatch_event);
  if (client->log_callback != NULL && config->log_level > 0)
    rc_client_enable_logging(client->rc, config->log_level, dispatch_log);
  return client;
}

void ra_client_destroy(ra_client_t *client) {
  SceUID lock;

  if (client == NULL)
    return;
  lock = client->lock;
  ra_client_lock(client);
  if (client->rc != NULL) {
    rc_client_destroy(client->rc);
    client->rc = NULL;
  }
  ra_client_unlock(client);
  if (lock >= 0)
    sceKernelDeleteSema(lock);
  free(client);
}

int ra_client_begin_login_with_token(ra_client_t *client, const char *username,
                  const char *token, ra_client_done_callback_t callback,
                  void *userdata) {
  rc_client_async_handle_t *handle;

  if (client == NULL || client->rc == NULL)
    return RC_INVALID_STATE;
  if (username == NULL || username[0] == '\0' ||
      token == NULL || token[0] == '\0')
    return RC_INVALID_STATE;

  ra_client_lock(client);
  if (client->state == RA_CLIENT_STATE_LOGGING_IN ||
      client->state == RA_CLIENT_STATE_LOADING_GAME) {
    ra_client_unlock(client);
    return RC_INVALID_STATE;
  }
  client->state = RA_CLIENT_STATE_LOGGING_IN;
  client->pending_callback = callback;
  client->pending_userdata = userdata;
  client->pending_is_load = 0;
  handle = rc_client_begin_login_with_token(client->rc, username, token,
                      login_complete, client);
  if (handle == NULL && client->state == RA_CLIENT_STATE_LOGGING_IN) {
    finish(client, RC_INVALID_STATE, "login could not start");
    ra_client_unlock(client);
    return RC_INVALID_STATE;
  }
  ra_client_unlock(client);
  return RC_OK;
}

int ra_client_begin_load_game_ex(ra_client_t *client, const char *hash,
                ra_client_done_callback_t callback, void *userdata,
                rc_client_async_handle_t **out_handle) {
  rc_client_async_handle_t *handle;

  if (out_handle != NULL)
    *out_handle = NULL;
  if (client == NULL || client->rc == NULL)
    return RC_INVALID_STATE;
  if (hash == NULL || hash[0] == '\0')
    return RC_INVALID_STATE;

  ra_client_lock(client);
  if (client->state == RA_CLIENT_STATE_LOGGING_IN ||
      client->state == RA_CLIENT_STATE_LOADING_GAME) {
    ra_client_unlock(client);
    return RC_INVALID_STATE;
  }
  client->state = RA_CLIENT_STATE_LOADING_GAME;
  client->pending_callback = callback;
  client->pending_userdata = userdata;
  client->pending_is_load = 1;
  handle = rc_client_begin_load_game(client->rc, hash, load_complete, client);
  if (handle == NULL && client->state == RA_CLIENT_STATE_LOADING_GAME) {
    finish(client, RC_INVALID_STATE, "game load could not start");
    ra_client_unlock(client);
    return RC_INVALID_STATE;
  }
  if (out_handle != NULL)
    *out_handle = handle;
  ra_client_unlock(client);
  return RC_OK;
}

int ra_client_begin_load_game(ra_client_t *client, const char *hash,
              ra_client_done_callback_t callback, void *userdata) {
  return ra_client_begin_load_game_ex(client, hash, callback, userdata, NULL);
}

void ra_client_abort_async(ra_client_t *client, rc_client_async_handle_t *handle) {
  if (client == NULL || client->rc == NULL || handle == NULL)
    return;
  ra_client_lock(client);
  rc_client_abort_async(client->rc, handle);
  ra_client_unlock(client);
}

void ra_client_set_eval_hold(ra_client_t *client, int hold) {
  if (client != NULL)
    client->eval_hold = hold != 0;
}

void ra_client_do_frame(ra_client_t *client) {
  if (client == NULL)
    return;
  ra_client_lock(client);
  if (client->pump != NULL)
    client->pump();
  if (!client->eval_hold && client->rc != NULL)
    rc_client_do_frame(client->rc);
  ra_client_unlock(client);
}

void ra_client_idle(ra_client_t *client) {
  if (client == NULL)
    return;
  ra_client_lock(client);
  if (client->pump != NULL)
    client->pump();
  if (client->rc != NULL)
    rc_client_idle(client->rc);
  ra_client_unlock(client);
}

void ra_client_set_pump(ra_client_t *client, ra_client_pump_t pump) {
  if (client != NULL)
    client->pump = pump;
}

void ra_client_reset(ra_client_t *client) {
  if (client == NULL || client->rc == NULL)
    return;
  ra_client_lock(client);
  rc_client_reset(client->rc);
  ra_client_unlock(client);
}

void ra_client_unload_game(ra_client_t *client) {
  if (client == NULL || client->rc == NULL)
    return;
  ra_client_lock(client);
  rc_client_unload_game(client->rc);
  client->state = rc_client_get_user_info(client->rc) != NULL
    ? RA_CLIENT_STATE_LOGGED_IN : RA_CLIENT_STATE_IDLE;
  ra_client_unlock(client);
}

ra_client_state_t ra_client_get_state(const ra_client_t *client) {
  return client != NULL ? client->state : RA_CLIENT_STATE_ERROR;
}

int ra_client_is_logged_in(const ra_client_t *client) {
  int logged_in;

  if (client == NULL || client->rc == NULL)
    return 0;
  ra_client_lock((ra_client_t*)client);
  logged_in = rc_client_get_user_info(client->rc) != NULL;
  ra_client_unlock((ra_client_t*)client);
  return logged_in;
}

int ra_client_is_game_loaded(const ra_client_t *client) {
  int loaded;

  if (client == NULL || client->rc == NULL)
    return 0;
  ra_client_lock((ra_client_t*)client);
  loaded = rc_client_is_game_loaded(client->rc);
  ra_client_unlock((ra_client_t*)client);
  return loaded;
}

void ra_client_set_spectator_mode(ra_client_t *client, int enabled) {
  if (client == NULL || client->rc == NULL)
    return;
  ra_client_lock(client);
  rc_client_set_spectator_mode_enabled(client->rc, enabled != 0);
  ra_client_unlock(client);
}

rc_client_t *ra_client_get_rc_client(ra_client_t *client) {
  return client != NULL ? client->rc : NULL;
}

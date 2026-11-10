#include "ra_session.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct ra_session {
  ra_client_t *client;
  ra_vault_credentials_t credentials;
  ra_session_status_callback_t status_callback;
  void *userdata;
  char game_hash[64];
  int logged_in;
  int ready;
  rc_client_async_handle_t *load_handle;
  int refs;
};

static void report(ra_session_t *session, int result, const char *error) {
  if (session->status_callback != NULL)
    session->status_callback(result, error, session->userdata);
}

static void load_done(int result, const char *error_message, void *userdata) {
  ra_session_t *session = (ra_session_t*)userdata;

  session->load_handle = NULL;
  session->ready = result == RC_OK ? 1 : 0;
  report(session, result, error_message);
}

static void login_done(int result, const char *error_message, void *userdata) {
  ra_session_t *session = (ra_session_t*)userdata;

  if (result != RC_OK) {
    report(session, result, error_message);
    return;
  }
  session->logged_in = 1;
  if (session->game_hash[0] == '\0')
    return;
  result = ra_client_begin_load_game_ex(session->client, session->game_hash,
                    load_done, session, &session->load_handle);
  if (result != RC_OK)
    report(session, result, "game load could not start");
}

ra_session_t *ra_session_create(const ra_session_config_t *config) {
  ra_session_t *session;
  ra_client_config_t client_config;

  if (config == NULL || config->device_id == NULL ||
      config->vault_path == NULL || config->read_memory == NULL ||
      config->transport == NULL)
    return NULL;

  session = (ra_session_t*)calloc(1, sizeof(*session));
  if (session == NULL)
    return NULL;
  if (ra_vault_load(config->vault_path, config->device_id,
          &session->credentials) != 0) {
    free(session);
    return NULL;
  }

  if (config->game_hash != NULL)
    snprintf(session->game_hash, sizeof(session->game_hash), "%s",
        config->game_hash);
  session->status_callback = config->status_callback;
  session->userdata = config->userdata;

  ra_server_set_transport(config->transport);
  memset(&client_config, 0, sizeof(client_config));
  client_config.read_memory = config->read_memory;
  client_config.server_call = config->server_call != NULL
                  ? config->server_call : ra_server_call;
  client_config.event_handler = config->event_handler;
  client_config.userdata = config->userdata;
  client_config.log_callback = config->log_callback;
  client_config.log_level = config->log_level;

  session->client = ra_client_create(&client_config);
  if (session->client == NULL) {
    ra_vault_clear(&session->credentials);
    free(session);
    return NULL;
  }
  session->refs = 1;
  return session;
}

void ra_session_retain(ra_session_t *session) {
  if (session != NULL)
    __atomic_add_fetch(&session->refs, 1, __ATOMIC_RELAXED);
}

void ra_session_release(ra_session_t *session) {
  if (session == NULL)
    return;
  if (__atomic_sub_fetch(&session->refs, 1, __ATOMIC_ACQ_REL) != 0)
    return;
  ra_client_destroy(session->client);
  ra_vault_clear(&session->credentials);
  free(session);
}

int ra_session_start(ra_session_t *session) {
  int result;

  if (session == NULL || session->client == NULL)
    return RC_INVALID_STATE;
  if (session->logged_in)
    return RC_OK;

  result = ra_client_begin_login_with_token(session->client,
                      session->credentials.username,
                      session->credentials.token,
                      login_done, session);
  if (result != RC_OK)
    report(session, result, "login could not start");
  return result;
}

void ra_session_do_frame(ra_session_t *session) {
  if (session != NULL && session->client != NULL)
    ra_client_do_frame(session->client);
}

void ra_session_idle(ra_session_t *session) {
  if (session != NULL && session->client != NULL)
    ra_client_idle(session->client);
}

void ra_session_reset(ra_session_t *session) {
  if (session != NULL && session->client != NULL)
    ra_client_reset(session->client);
}

int ra_session_load_game(ra_session_t *session, const char *hash) {
  int result;

  if (session == NULL || session->client == NULL ||
      hash == NULL || hash[0] == '\0')
    return RC_INVALID_STATE;
  if (session->ready && strcmp(session->game_hash, hash) == 0)
    return RC_OK;

  if (session->game_hash[0] != '\0' && strcmp(session->game_hash, hash) != 0) {
    ra_client_unload_game(session->client);
    session->ready = 0;
  }
  snprintf(session->game_hash, sizeof(session->game_hash), "%s", hash);

  if (!session->logged_in)
    return RC_OK;
  result = ra_client_begin_load_game_ex(session->client, session->game_hash,
                    load_done, session, &session->load_handle);
  if (result != RC_OK)
    report(session, result, "game load could not start");
  return result;
}

void ra_session_abort_load(ra_session_t *session) {
  if (session == NULL || session->client == NULL ||
      session->load_handle == NULL)
    return;
  ra_client_abort_async(session->client, session->load_handle);
  session->load_handle = NULL;
  session->ready = 0;
}

void ra_session_unload_game(ra_session_t *session) {
  if (session == NULL || session->client == NULL)
    return;
  ra_client_unload_game(session->client);
  session->game_hash[0] = '\0';
  session->ready = 0;
  session->load_handle = NULL;
}

void ra_session_logout(ra_session_t *session) {
  rc_client_t *rc;

  if (session == NULL || session->client == NULL)
    return;
  ra_session_abort_load(session);
  ra_client_lock(session->client);
  rc = ra_client_get_rc_client(session->client);
  if (rc != NULL)
    rc_client_logout(rc);
  ra_client_unlock(session->client);
  session->logged_in = 0;
  session->ready = 0;
  session->game_hash[0] = '\0';
}

int ra_session_is_logged_in(const ra_session_t *session) {
  return session != NULL && session->logged_in;
}

void ra_session_destroy(ra_session_t *session) {
  ra_session_release(session);
}

ra_client_t *ra_session_get_client(ra_session_t *session) {
  return session != NULL ? session->client : NULL;
}

int ra_session_copy_credentials(ra_session_t *session,
                                ra_vault_credentials_t *out) {
  if (session == NULL || out == NULL || !session->logged_in)
    return 0;
  *out = session->credentials;
  return 1;
}

int ra_session_is_ready(const ra_session_t *session) {
  return session != NULL && session->logged_in && session->ready &&
    ra_client_is_game_loaded(session->client);
}

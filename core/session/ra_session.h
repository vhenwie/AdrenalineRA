#ifndef RA_SESSION_H
#define RA_SESSION_H

#include "ra_client.h"
#include "ra_server.h"
#include "ra_vault.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ra_session ra_session_t;

typedef void (*ra_session_status_callback_t)(int result, const char *error_message,
                      void *userdata);

typedef struct {
  const uint8_t *device_id;
  const char *vault_path;
  const char *game_hash;
  rc_client_read_memory_func_t read_memory;
  rc_client_event_handler_t event_handler;
  ra_server_transport_t transport;
  /* Server-call implementation for rc_client. NULL selects the synchronous
     * ra_server_call(). The Vita runtime installs its deferred queue here so the
     * request runs off the runtime thread. */
  rc_client_server_call_t server_call;
  ra_session_status_callback_t status_callback;
  void *userdata;
  /* Optional rcheevos diagnostics; forwarded to ra_client. */
  ra_client_log_callback_t log_callback;
  int log_level;
} ra_session_config_t;

/* Loads the encrypted token, creates ra_client, and logs in. The status
 * callback fires again when the precomputed-hash game load completes. */
ra_session_t *ra_session_create(const ra_session_config_t *config);
int ra_session_start(ra_session_t *session);
int ra_session_load_game(ra_session_t *session, const char *hash);
void ra_session_unload_game(ra_session_t *session);
void ra_session_do_frame(ra_session_t *session);
void ra_session_idle(ra_session_t *session);
void ra_session_reset(ra_session_t *session);
void ra_session_destroy(ra_session_t *session);
void ra_session_retain(ra_session_t *session);
void ra_session_release(ra_session_t *session);

ra_client_t *ra_session_get_client(ra_session_t *session);
int ra_session_copy_credentials(ra_session_t *session,
                                ra_vault_credentials_t *out);
int ra_session_is_logged_in(const ra_session_t *session);
/* Aborts an in-flight game load, so its callback never lands on a reset client. */
void ra_session_abort_load(ra_session_t *session);
void ra_session_logout(ra_session_t *session);
int ra_session_is_ready(const ra_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* RA_SESSION_H */

#ifndef RA_CLIENT_H
#define RA_CLIENT_H

#include <stdint.h>

#include "rc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ra_client ra_client_t;

typedef enum {
  RA_CLIENT_STATE_IDLE = 0,
  RA_CLIENT_STATE_LOGGING_IN,
  RA_CLIENT_STATE_LOGGED_IN,
  RA_CLIENT_STATE_LOADING_GAME,
  RA_CLIENT_STATE_READY,
  RA_CLIENT_STATE_ERROR
} ra_client_state_t;

/* Completion callback for login/load. The callback may run synchronously when
 * the supplied server_call implementation responds synchronously. */
typedef void (*ra_client_done_callback_t)(int result, const char *error_message,
                    void *userdata);

/* rcheevos' own diagnostics ("Identified game: ...", "Request has timed out.",
 * "MemAddr not found in response", ...). PPSSPP always installs this hook via
 * rc_client_enable_logging(); without it a parse failure is a single terse
 * string with no way to tell which request produced it. */
typedef void (*ra_client_log_callback_t)(const char *message, void *userdata);

typedef struct {
  rc_client_read_memory_func_t read_memory;
  rc_client_server_call_t server_call;
  rc_client_event_handler_t event_handler;
  void *userdata;
  /* Optional. 0 / NULL disables the hook. Use a RC_CLIENT_LOG_LEVEL_ *value. */
  ra_client_log_callback_t log_callback;
  int log_level;
} ra_client_config_t;

ra_client_t *ra_client_create(const ra_client_config_t *config);
void ra_client_destroy(ra_client_t *client);

/* Pump run at the top of every do_frame()/idle(), before rc_client is touched.
 *
 * The Vita runtime installs its deferred server-call queue here: the HTTP request
 * runs on a worker, and the rcheevos callback for it is delivered from this hook,
 * i.e. always on the thread that owns rc_client. Nothing else about the client
 * changes. Optional; NULL clears it. */
typedef void (*ra_client_pump_t)(void);
void ra_client_set_pump(ra_client_t *client, ra_client_pump_t pump);

/* Token login only. The password path intentionally does not exist here. */
int ra_client_begin_login_with_token(ra_client_t *client, const char *username,
                  const char *token,
                  ra_client_done_callback_t callback,
                  void *userdata);

/* Load a game by its precomputed hash.
 *
 * `out_handle` (optional) receives the in-flight request so the caller can abort
 * it - `ra_session_abort_load()` uses this when the game is lost or the user signs
 * out, instead of letting the completion callback land on a reset client
 * . Pass NULL when the caller does not need it. */
int ra_client_begin_load_game_ex(ra_client_t *client, const char *hash,
                ra_client_done_callback_t callback,
                void *userdata,
                rc_client_async_handle_t **out_handle);
int ra_client_begin_load_game(ra_client_t *client, const char *hash,
              ra_client_done_callback_t callback,
              void *userdata);
/* Cancels an in-flight async request. */
void ra_client_abort_async(ra_client_t *client, rc_client_async_handle_t *handle);

/* Stops (`hold != 0`) or resumes advancing logic frames without touching the client: the
 * pump still runs, so queued server work is not stalled. Used while the runtime resolves a
 * live disc switch that may have changed the game. */
void ra_client_set_eval_hold(ra_client_t *client, int hold);

void ra_client_do_frame(ra_client_t *client);
/* Idle pump: scheduled server work without advancing a logic frame. The
 * reference clients call this while paused or before the game has booted. */
void ra_client_idle(ra_client_t *client);
/* Clears measured state without unloading. Required after
 * RC_CLIENT_EVENT_RESET, which rcheevos raises when hardcore is toggled with a
 * game loaded; processing stays disabled until this runs. */
void ra_client_reset(ra_client_t *client);
void ra_client_unload_game(ra_client_t *client);

ra_client_state_t ra_client_get_state(const ra_client_t *client);
int ra_client_is_logged_in(const ra_client_t *client);
int ra_client_is_game_loaded(const ra_client_t *client);
void ra_client_set_spectator_mode(ra_client_t *client, int enabled);

/* Serializes every rc_client call with the client's own do_frame/idle. The lock
 * is recursive for the owning thread, so an event callback may call back into
 * the client while rc_client_do_frame is running. A caller that receives
 * ra_client_get_rc_client() must hold this lock for every rc_client call. */
void ra_client_lock(ra_client_t *client);
void ra_client_unlock(ra_client_t *client);

/* Escape hatch for UI/tests that need to query rc_client directly. The pointer
 * is only valid while the owning session is retained, and no rc_client call may
 * use it outside ra_client_lock()/ra_client_unlock(). */
rc_client_t *ra_client_get_rc_client(ra_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* RA_CLIENT_H */

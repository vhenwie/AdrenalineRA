#ifndef RA_NETQUEUE_H
#define RA_NETQUEUE_H

#include "rc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deferred server calls: rcheevos requests run on a worker, not the logic thread. */

/* Creates the worker (idempotent). Safe to call again after ra_netqueue_stop(). */
void ra_netqueue_init(void);

/* Answers a request from the queue, or synchronously when the queue is full or
 * the request does not fit. rc_client_server_call_t-compatible. */
void ra_netqueue_server_call(const rc_api_request_t *request,
              rc_client_server_callback_t callback,
              void *callback_data, rc_client_t *client);

/* Delivers every finished response in request order. Runs on the runtime thread,
 * installed through ra_client_set_pump(). */
void ra_netqueue_pump(void);

/* Stops the worker and discards anything not yet delivered. Anything still in
 * flight is dropped without its callback: the client is about to be destroyed,
 * so invoking it would touch freed state. */
void ra_netqueue_stop(void);

/* Requests currently queued or in flight; diagnostics only. */
int ra_netqueue_depth(void);

/* Number of queued submissions that reached the server since the last call, and resets
 * the counter. The runtime turns a non-zero answer into the "Reconnected" banner. */
int ra_netqueue_take_replay_count(void);

/* Asks the worker to replay the submissions an earlier session could not deliver
 * (see core/offline/ra_offline.h). Called once when a link is available; a second call
 * while a replay is pending is harmless. */
void ra_netqueue_replay_offline(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_NETQUEUE_H */

#ifndef RA_RUNTIME_DISC_H
#define RA_RUNTIME_DISC_H

#include <stddef.h>
#include <stdint.h>

#include "rc_client.h"

/* Restart path for a validated disc switch: re-hashes the container that is live. */
void ra_runtime_disc_restart(int disc_index);

/* Called when pspemu's official settings close; records that a disc check is due. */
void ra_runtime_disc_check(void);

/* Media watch: samples the live serial and drives eject/restore/switch. */
void ra_runtime_disc_watch(void);

/* Retries a disc that could not be resolved, using the hash already computed. */
void ra_runtime_disc_retry(void);

/* rcheevos completion for begin_change_media. */
void ra_runtime_disc_change_media_callback(int result, const char *error_message,
                                           rc_client_t *client, void *userdata);

/* Applies a new disc file to the running session. */
void ra_runtime_disc_changed(const char *path);

#endif /* RA_RUNTIME_DISC_H */

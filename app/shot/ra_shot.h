#ifndef RA_SHOT_H
#define RA_SHOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the screenshot worker (idempotent). Captures go to
 * ux0:data/AdrenalineRA/screenshots/<folder>/. */
void ra_shot_init(void);

/* Queues a capture of the emulated game frame at unlock time. Never blocks the
 * caller: the stored-PNG encode and the write happen on a low-priority worker.
 * `folder` is the game name, `name` the achievement name; both are normalised to
 * [A-Za-z0-9_-]. */
void ra_shot_request(const char *folder, const char *name);

/* Writes the paused burst copies that are still queued. Called at exit; during
 * play the copy worker handles them when the world is paused. */
void ra_shot_flush_copies(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_SHOT_H */

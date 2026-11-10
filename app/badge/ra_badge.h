#ifndef RA_BADGE_H
#define RA_BADGE_H

#include <stddef.h>
#include <stdint.h>

#include <vita2d.h>

#include "ra_badge_key.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RA_BADGE_URL_MAX 192
/* Badge cache directory; keys carry a `_lock` suffix for locked art. */
#define RA_BADGE_DIR "ux0:data/AdrenalineRA/badges"

/* Starts the low-priority badge downloader and its cache. Idempotent. */
void ra_badge_init(void);

/* Stops queued work and prevents new requests. Lightweight: called on suspend, on a
 * shell flap and when a game closes, so it never waits. The worker parks on its wake
 * semaphore and is joined only by ra_badge_shutdown(). */
void ra_badge_stop(void);

/* Terminal teardown : stop, then join the worker (bounded 1500 ms) and delete
 * it. Call this on the exit path - module_stop and the explicit leave-game - so no
 * thread of ours is alive when the module unloads. */
void ra_badge_shutdown(void);

/* 1 once ra_badge_stop() was called; the runtime's warm pass stops queueing art. */
int ra_badge_stopped(void);

/* 1 while the badge worker thread exists and has not returned . A paused
 * worker is alive; only ra_badge_shutdown() ends it. Diagnostics: report this with the
 * cache state, because a dead worker looked exactly like a healthy one. */
int ra_badge_worker_alive(void);

/* Achievement Badges setting. Disabling prevents new requests and hides
 * cached textures without freeing GPU resources from the runtime thread. */
void ra_badge_set_enabled(int enabled);
int ra_badge_enabled(void);

/* RA game id used as the cache sub-directory:
 * ux0:data/AdrenalineRA/badges/<game_id>/<badge>.png */
void ra_badge_set_game(uint32_t game_id);

/* Builds a filesystem-safe cache key from the rcheevos badge name, falling
 * back to the URL filename. Returns 1 on success. */
int ra_badge_make_key(const char *badge_name, const char *url, char *out,
          size_t capacity);

/* Queues a badge if it is not already cached. high_priority is used for the
 * achievement currently being unlocked, so it jumps ahead of game-start
 * prefetch work. */
void ra_badge_request(const char *key, const char *url, int high_priority);

/* Draw-thread lookup. Returns NULL until a cached PNG exists; the caller
 * keeps drawing the existing procedural glyph in that case. */
vita2d_texture *ra_badge_texture(const char *key);

/* Background art warm-up : the worker queues the key of every badge it has
 * just fetched, and the draw thread uploads one of them per frame - outside any unlock
 * frame. Art is held in RAM; the worker does not write ux0 while a game runs. By the
 * time a card needs the image it is already a texture, so the unlock frame does no
 * file I/O, no decode and no upload. Draw thread only, at most one texture per call. */
void ra_badge_prime_step(void);

/* Frame boundary for the texture cache: frees the textures that were chosen as
 * eviction victims during the previous frame. Never free a texture inside the frame
 * that is still sampling it (vita2d queues its draws and reads texture memory when
 * the frame is submitted); call this once at the top of the overlay draw. */
void ra_badge_frame_begin(void);

/* Filesystem-only cache probe (no GPU work). */
int ra_badge_cached(const char *key);

/* Queued downloads not yet attempted. The warm-cache pass paces itself on this,
 * otherwise the 256-slot queue fills faster than the worker drains and requests
 * are dropped. */
int ra_badge_pending(void);

/* 1 once the circuit breaker stops prefetching for this session (the host was refusing
 * us). The warm-cache pass stops instead of re-queueing the whole set into a stalled
 * queue. */
int ra_badge_stalled(void);

/* 1 when the badge cache directory exists (diagnostics). */
int ra_badge_cache_dir_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_BADGE_H */

#ifndef RA_ARENA_H
#define RA_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dedicated allocator: SceLibc's heap is fixed near 1 MB, too small for large
 * achievement sets, so every allocation is served from this arena. */
#ifndef RA_ARENA_THRESHOLD
#define RA_ARENA_THRESHOLD 0u
#endif

/* Kernel reservation for the arena; 12 MiB covers the worst parse with headroom. */
#define RA_ARENA_SIZE (12u * 1024u * 1024u)

/* 1 once an arena is available. */
int ra_arena_ready(void);

/* Installs an externally provided region. Returns 1 on success, 0 on bad input
 * or when already initialised. Host tests call this with their own buffer. */
int ra_arena_init(void *base, size_t size);

void *ra_arena_malloc(size_t size);
void *ra_arena_calloc(size_t count, size_t size);
void *ra_arena_realloc(void *ptr, size_t size);
void ra_arena_free(void *ptr);

/* 1 when the pointer lies inside the arena payload area. */
int ra_arena_owns(const void *ptr);
size_t ra_arena_capacity(void);
size_t ra_arena_used(void);
size_t ra_arena_largest_free(void);
/* Peak ra_arena_used() seen while the current run allocated. */
size_t ra_arena_peak(void);
/* Allocator work for the current run: successful allocations and block steps
 * scanned. Reported so a slow parse can be attributed to the allocator or not. */
unsigned long ra_arena_stat_allocations(void);
unsigned long ra_arena_stat_scans(void);
/* Times a stale scan start (rover) was found and reset. Must be 0 on hardware;
 * non-zero means the allocator recovered instead of dereferencing dead memory. */
unsigned long ra_arena_stat_rover_repairs(void);

/* Largest block the *system *heap can still serve, in KiB.
 *
 * A plain malloc() of a large block is answered by the arena once --wrap is in
 * place, so measuring "free heap" with malloc reported the arena's size and hid
 * the real SceLibc heap. This probes the unwrapped allocator. */
size_t ra_arena_system_probe_kib(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_ARENA_H */

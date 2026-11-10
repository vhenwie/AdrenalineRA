#ifndef RA_RUNTIME_DISC_SERIAL_H
#define RA_RUNTIME_DISC_SERIAL_H

#include <stddef.h>

/* Serial of one disc inside a POPS EBOOT, normalised to bare letters/digits. */
int ra_runtime_disc_pbp_serial(const char *path, int disc_index, char *out,
                               size_t capacity);

/* Serial of the disc POPS is actually running, read from the container header in
 * PSP RAM. Returns 0 when the address cannot be read. */
int ra_runtime_disc_live_serial(char *out, size_t capacity);

#endif /* RA_RUNTIME_DISC_SERIAL_H */

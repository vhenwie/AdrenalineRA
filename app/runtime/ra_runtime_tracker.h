#ifndef RA_RUNTIME_TRACKER_H
#define RA_RUNTIME_TRACKER_H

#include <stdint.h>

int ra_runtime_tracker_update(uint32_t id, const char *display);
void ra_runtime_tracker_forget(uint32_t id);
void ra_runtime_tracker_reset(void);

#endif

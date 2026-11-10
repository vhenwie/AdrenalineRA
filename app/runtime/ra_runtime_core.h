#ifndef RA_RUNTIME_CORE_H
#define RA_RUNTIME_CORE_H

#include <stddef.h>

void ra_runtime_lock(void);
void ra_runtime_unlock(void);
void ra_runtime_copy_text(char *destination, size_t capacity, const char *source);
unsigned long long ra_runtime_now_ms(void);

#endif

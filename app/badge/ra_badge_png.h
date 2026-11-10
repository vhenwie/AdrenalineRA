#ifndef RA_BADGE_PNG_H
#define RA_BADGE_PNG_H

#include <stddef.h>

int ra_badge_png_header_ok(const unsigned char *data, size_t size,
                           unsigned int *width, unsigned int *height);
int ra_badge_png_write_atomic(const char *key, const unsigned char *data,
                              size_t length);

#endif

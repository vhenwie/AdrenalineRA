#ifndef RA_HTTP_DIAG_H
#define RA_HTTP_DIAG_H

#include <stddef.h>

void ra_http_probe_heap(void);
unsigned int ra_http_get_heap_probe_kib(void);
void ra_http_copy_response_header(int request, const char *field, char *out,
                                  size_t capacity);
void ra_http_mask_request_body(const char *post_data, char *out, size_t capacity);

#endif

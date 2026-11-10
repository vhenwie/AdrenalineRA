#ifndef RA_SERVER_UTIL_H
#define RA_SERVER_UTIL_H

#include <stddef.h>
#include "ra_http.h"

int ra_server_url_param_int(const char *post, const char *name, int *value);
const char *ra_server_filter_rich_presence(const char *input, char *output,
                                           size_t capacity);
int ra_server_rich_presence_enabled(void);
unsigned long long ra_server_now_ms(void);
void ra_server_request_name(const char *post_data, char *out, size_t capacity);

#endif

#ifndef RA_SERVER_JSON_H
#define RA_SERVER_JSON_H

#include <stddef.h>
#include <stdint.h>

char *ra_server_json_match_array(char *open, char *end);
const char *ra_server_json_skip_ws(const char *p, const char *end);
const char *ra_server_json_skip_value(const char *p, const char *end);
const char *ra_server_json_find(const char *from, const char *to, const char *key);
int ra_server_json_read_uint(const char *from, const char *to, const char *key,
                             uint32_t *out);
int ra_server_json_read_string(const char *from, const char *to, const char *key,
                               char *out, size_t capacity);

#endif

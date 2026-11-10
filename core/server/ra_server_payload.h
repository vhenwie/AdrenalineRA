#ifndef RA_SERVER_PAYLOAD_H
#define RA_SERVER_PAYLOAD_H

#include <stddef.h>

size_t ra_server_trim_payload(char *body, size_t length, unsigned int set_id);
size_t ra_server_last_trim_bytes(void);
void ra_server_reset_trim_bytes(void);

#endif

#ifndef RA_SETUP_H
#define RA_SETUP_H

#include <stddef.h>
#include <stdint.h>

#include "ra_http.h"
#include "ra_vault.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same signature as ra_http_post; tests can inject a synchronous mock. */
typedef int (*ra_setup_transport_t)(const char *url, const char *post_data,
                                    size_t post_length, const char *content_type,
                                    ra_http_response_t *response);

/* NULL restores the real Vita transport (ra_http_post). */
void ra_setup_set_transport(ra_setup_transport_t transport);

/* login2 -> connect token -> encrypted vault; the password is never stored or logged. */
int ra_setup_login_to_vault(const char *username, const char *password,
                            const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
                            const char *vault_path,
                            char *canonical_username,
                            size_t canonical_capacity,
                            char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif /* RA_SETUP_H */

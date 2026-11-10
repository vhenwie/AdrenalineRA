#ifndef RA_CREDENTIALS_H
#define RA_CREDENTIALS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_CREDENTIALS_UR0 "ur0:data/AdrenalineRA/ra.cfg"
#define RA_CREDENTIALS_UX0 "ux0:data/AdrenalineRA/ra.cfg"

int ra_credentials_exists(void);
int ra_credentials_load(char *username, size_t username_capacity,
                        char *token, size_t token_capacity);
int ra_credentials_save(const char *username, const char *token,
                        char *error, size_t error_capacity);
void ra_credentials_clear_files(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_CREDENTIALS_H */

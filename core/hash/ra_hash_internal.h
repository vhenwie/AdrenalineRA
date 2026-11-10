#ifndef RA_HASH_INTERNAL_H
#define RA_HASH_INTERNAL_H

#include "rc_hash_internal.h"

void ra_hash_error_cb_public(const char *message,
                             const rc_hash_iterator_t *iterator);
const rc_hash_filereader_t *ra_hash_filereader(void);
const rc_hash_cdreader_t *ra_hash_cdreader(void);

void ra_hash_status_set_error(const char *message);
void ra_hash_status_set_block_error(const char *message);
void ra_hash_status_set_geometry(const char *message);
const char *ra_hash_status_last_error(void);
const char *ra_hash_status_last_block_error(void);
const char *ra_hash_status_geometry(void);

#endif

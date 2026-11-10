#ifndef RA_HASH_CD_PBP_H
#define RA_HASH_CD_PBP_H

#include <stdint.h>

#include "ra_hash_cd.h"

/* Little-endian 32-bit read used by the PBP walk. */
uint32_t ra_hash_cd_u32(const uint8_t *p);

/* Loads one 16-sector block through the PBP index (stored or inflated). */
int ra_hash_cd_load_pbp_block(ra_cd_t *cd, uint32_t block);

#endif /* RA_HASH_CD_PBP_H */

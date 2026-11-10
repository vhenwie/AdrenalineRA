#ifndef RA_HASH_CD_H
#define RA_HASH_CD_H

#include <stddef.h>
#include <stdint.h>

#include "ra_hash_io.h"

#define RA_PBP_BLOCK_SIZE 0x9300u
#define RA_PBP_BLOCK_SECTORS 16u
#define RA_PBP_INDEX_OFFSET 0x4000u
#define RA_PBP_DATA_OFFSET 0x100000u
#define RA_PBP_MAX_BLOCKS (1u << 20)
#define RA_PBP_USER_OFFSET 24u
#define RA_PBP_SYNC "\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00"
#define RA_PBP_READAHEAD_BLOCKS 4u

typedef struct {
  ra_file_t *file;
  uint64_t file_size;
  uint32_t sector_size;
  uint32_t first_sector;
  int is_cso;
  int is_pbp;
  uint64_t pbp_index;
  uint64_t pbp_data;
  uint32_t block_size;
  uint32_t block_count;
  uint32_t align;
  uint32_t *index;
  uint8_t *block;
  uint32_t block_index;
  uint32_t block_length;
  /* Batched index reads: one 32-entry refill per 32 blocks. */
  uint8_t idx_cache[32 * 32];
  uint32_t idx_first;
  uint32_t idx_count;
  /* Read-ahead window for consecutive blocks. */
  uint8_t *ra_buf;
  uint32_t ra_first;
  uint32_t ra_count;
} ra_cd_t;

void *ra_cd_open(const char *path, uint32_t track);
size_t ra_cd_read(void *handle, uint32_t sector, void *buffer, size_t size);
uint32_t ra_cd_first_sector(void *handle);
void ra_cd_close(void *handle);

#endif

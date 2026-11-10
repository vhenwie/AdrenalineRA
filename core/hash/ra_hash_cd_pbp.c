#include "ra_hash_cd_pbp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puff.h"
#include "ra_hash_internal.h"

uint32_t ra_hash_cd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int pbp_index_fill(ra_cd_t *cd, uint32_t block) {
  char message[96];
  uint32_t first;
  uint32_t count;

  if (cd->idx_count != 0 && block >= cd->idx_first &&
    block < cd->idx_first + cd->idx_count)
    return 0;

  /* Serve index entries from a 32-entry cache. */
  first = block;
  count = cd->block_count - first;
  if (count > 32)
    count = 32;
  if (ra_seek(cd->file, (int64_t)(cd->pbp_index + (uint64_t)first * 32u),
        SEEK_SET) != 0 ||
    ra_read_exact(cd->file, cd->idx_cache, (size_t)count * 32u) != 0) {
    cd->idx_count = 0;
    cd->ra_count = 0;
    snprintf(message, sizeof(message), "PBP index read failed at block %u",
        (unsigned)block);
    ra_hash_status_set_block_error(message);
    ra_hash_status_set_error(message);
    return -1;
  }
  cd->idx_first = first;
  cd->idx_count = count;
  return 0;
}

static int pbp_serve_readahead(ra_cd_t *cd, uint32_t block) {
  /* Serve from the read-ahead window when the block is present. */
  if (cd->ra_count != 0 && block >= cd->ra_first &&
    block < cd->ra_first + cd->ra_count) {
    memcpy(cd->block,
      cd->ra_buf + (size_t)(block - cd->ra_first) * RA_PBP_BLOCK_SIZE,
      RA_PBP_BLOCK_SIZE);
    cd->block_length = RA_PBP_BLOCK_SIZE;
    cd->block_index = block;
    return 1;
  }
  cd->ra_count = 0;
  return 0;
}

/* Fetch a run of contiguous raw blocks in one read. */
static uint32_t pbp_probe_run(ra_cd_t *cd, uint32_t block) {
  uint32_t run = 0;
  uint32_t prev_end = 0;
  uint8_t peek[32];

  while (run < RA_PBP_READAHEAD_BLOCKS && block + run < cd->block_count) {
    uint32_t run_offset;
    uint32_t run_length;

    if (ra_seek(cd->file,
          (int64_t)(cd->pbp_index + (uint64_t)(block + run) * 32u),
          SEEK_SET) != 0 ||
      ra_read_exact(cd->file, peek, sizeof(peek)) != 0)
      break;
    run_offset = ra_hash_cd_u32(peek);
    run_length = (uint32_t)peek[4] | ((uint32_t)peek[5] << 8);
    if (run_length < RA_PBP_BLOCK_SIZE)
      break; /* compressed or a short tail block: not part of a raw run */
    if (run > 0 && run_offset != prev_end)
      break; /* not adjacent */
    if (cd->pbp_data + (uint64_t)run_offset + RA_PBP_BLOCK_SIZE >
      cd->file_size)
      break;
    prev_end = run_offset + RA_PBP_BLOCK_SIZE;
    ++run;
  }
  return run;
}

static int pbp_fetch_run(ra_cd_t *cd, uint32_t block, const uint8_t *entry) {
  uint32_t run;

  if (cd->ra_buf == NULL)
    cd->ra_buf = (uint8_t*)malloc((size_t)RA_PBP_READAHEAD_BLOCKS *
                  RA_PBP_BLOCK_SIZE);
  if (cd->ra_buf == NULL)
    return 0;
  run = pbp_probe_run(cd, block);
  if (run >= 2) {
    uint64_t run_source = cd->pbp_data + ra_hash_cd_u32(entry);

    if (ra_seek(cd->file, (int64_t)run_source, SEEK_SET) == 0 &&
      ra_read_exact(cd->file, cd->ra_buf,
            (size_t)run * RA_PBP_BLOCK_SIZE) == 0) {
      cd->ra_first = block;
      cd->ra_count = run;
      memcpy(cd->block, cd->ra_buf, RA_PBP_BLOCK_SIZE);
      cd->block_length = RA_PBP_BLOCK_SIZE;
      cd->block_index = block;
      return 1;
    }
  }
  cd->ra_count = 0;
  return 0;
}

static int pbp_refresh_entry(ra_cd_t *cd, uint32_t block, uint8_t *entry) {
  char message[96];

  if (cd->idx_count == 0 || block < cd->idx_first ||
    block >= cd->idx_first + cd->idx_count) {
    if (ra_seek(cd->file, (int64_t)(cd->pbp_index + (uint64_t)block * 32u),
          SEEK_SET) != 0 ||
      ra_read_exact(cd->file, entry, 32) != 0) {
      snprintf(message, sizeof(message),
          "PBP index read failed at block %u", (unsigned)block);
      ra_hash_status_set_block_error(message);
      ra_hash_status_set_error(message);
      return -1;
    }
  } else {
    memcpy(entry, cd->idx_cache + (size_t)(block - cd->idx_first) * 32u, 32);
  }
  return 0;
}

static int pbp_read_stored(ra_cd_t *cd, uint32_t block, uint64_t source) {
  char message[96];

  /* stored uncompressed */
  if (ra_seek(cd->file, (int64_t)source, SEEK_SET) != 0 ||
    ra_read_exact(cd->file, cd->block, RA_PBP_BLOCK_SIZE) != 0) {
    snprintf(message, sizeof(message), "PBP block %u raw read failed",
        (unsigned)block);
    ra_hash_status_set_block_error(message);
    ra_hash_status_set_error(message);
    return -1;
  }
  cd->block_length = RA_PBP_BLOCK_SIZE;
  return 0;
}

static int pbp_read_inflated(ra_cd_t *cd, uint32_t block, uint64_t source,
    uint32_t length) {
  uint8_t *packed = (uint8_t*)malloc(length);
  unsigned long source_length = (unsigned long)length;
  unsigned long dest_length = (unsigned long)RA_PBP_BLOCK_SIZE;
  char message[96];
  int result;

  if (packed == NULL) {
    ra_hash_status_set_error("PBP block buffer allocation failed");
    return -1;
  }
  if (ra_seek(cd->file, (int64_t)source, SEEK_SET) != 0 ||
    ra_read_exact(cd->file, packed, length) != 0) {
    free(packed);
    snprintf(message, sizeof(message), "PBP block %u read failed",
        (unsigned)block);
    ra_hash_status_set_block_error(message);
    ra_hash_status_set_error(message);
    return -1;
  }
  result = puff(cd->block, &dest_length, packed, &source_length);
  free(packed);
  if (result != 0) {
    snprintf(message, sizeof(message),
        "PBP block %u inflate failed rc=%d in=%u out=%lu",
        (unsigned)block, result, (unsigned)length, dest_length);
    ra_hash_status_set_block_error(message);
    ra_hash_status_set_error(message);
    return -1;
  }
  if (dest_length != RA_PBP_BLOCK_SIZE) {
    snprintf(message, sizeof(message),
        "PBP block %u inflated to %lu of %u bytes",
        (unsigned)block, dest_length, (unsigned)RA_PBP_BLOCK_SIZE);
    ra_hash_status_set_block_error(message);
    ra_hash_status_set_error(message);
    return -1;
  }
  cd->block_length = RA_PBP_BLOCK_SIZE;
  return 0;
}

static int pbp_deliver_block(ra_cd_t *cd, uint32_t block,
    const uint8_t *entry) {
  uint32_t offset = ra_hash_cd_u32(entry);
  uint32_t length = (uint32_t)entry[4] | ((uint32_t)entry[5] << 8);
  uint64_t source;
  char message[96];

  if (length == 0) {
    ra_hash_status_set_error("PBP index entry has a zero length");
    return -1;
  }

  source = cd->pbp_data + (uint64_t)offset;
  if (source + length > cd->file_size) {
    snprintf(message, sizeof(message),
        "PBP block %u is past EOF (off 0x%X len %u)",
        (unsigned)block, (unsigned)offset, (unsigned)length);
    ra_hash_status_set_block_error(message);
    ra_hash_status_set_error(message);
    return -1;
  }

  if (length >= RA_PBP_BLOCK_SIZE || (entry[6] & 1u) != 0)
    return pbp_read_stored(cd, block, source);
  return pbp_read_inflated(cd, block, source, length);
}

int ra_hash_cd_load_pbp_block(ra_cd_t *cd, uint32_t block) {
  uint8_t entry[32];

  if (block >= cd->block_count)
    return -1;

  if (pbp_index_fill(cd, block) != 0)
    return -1;

  memcpy(entry, cd->idx_cache + (size_t)(block - cd->idx_first) * 32u,
    sizeof(entry));

  if (pbp_serve_readahead(cd, block))
    return 0;

  if (pbp_fetch_run(cd, block, entry))
    return 0;

  /* Point-of-use re-read of the entry: the run probe above used the same cache, so this
     * is a memcpy from `cd->idx_cache` unless it evicted the block. */
  if (pbp_refresh_entry(cd, block, entry) != 0)
    return -1;

  if (pbp_deliver_block(cd, block, entry) != 0)
    return -1;

  cd->block_index = block;
  return 0;
}

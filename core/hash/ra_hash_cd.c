#include "ra_hash_cd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puff.h"
#include "ra_hash_cd_pbp.h"
#include "ra_hash_internal.h"

/* Which disc of a multi-disc POPS EBOOT to hash. Single-threaded by design. */
static int g_pbp_disc_index;

void ra_hash_set_disc_index(int disc_index) {
  if (disc_index < 0)
    disc_index = 0;
  if (disc_index > 7)
    disc_index = 7;
  g_pbp_disc_index = disc_index;
}

int ra_hash_get_disc_index(void) {
  return g_pbp_disc_index;
}

/* 0x220 covers the multi-disc table at PSAR+0x200. */
static int pbp_locate_disc(ra_cd_t *cd, uint64_t *disc_offset) {
  uint8_t header[0x28];
  uint8_t buffer[0x220];
  uint64_t psar;
  uint32_t disc_relative;

  if (ra_seek(cd->file, 0, SEEK_SET) != 0 ||
    ra_read_exact(cd->file, header, sizeof(header)) != 0 ||
    memcmp(header, "\0PBP", 4) != 0) {
    ra_hash_status_set_error("not a PBP file");
    return -1;
  }

  psar = (uint64_t)ra_hash_cd_u32(header + 0x24);
  if (psar == 0 || psar >= cd->file_size) {
    ra_hash_status_set_error("PBP has no DATA.PSAR");
    return -1;
  }

  if (ra_seek(cd->file, (int64_t)psar, SEEK_SET) != 0 ||
    ra_read_exact(cd->file, buffer, 16) != 0) {
    ra_hash_status_set_error("PBP DATA.PSAR is truncated");
    return -1;
  }

  if (memcmp(buffer, "PSTITLEIMG000000", 16) == 0) {
    if (ra_seek(cd->file, (int64_t)psar, SEEK_SET) != 0 ||
      ra_read_exact(cd->file, buffer, sizeof(buffer)) != 0) {
      ra_hash_status_set_error("PBP PSTITLEIMG header is truncated");
      return -1;
    }
    disc_relative = ra_hash_cd_u32(buffer + 0x200 + 4u * (uint32_t)g_pbp_disc_index);
    if (disc_relative == 0 || psar + disc_relative >= cd->file_size) {
      ra_hash_status_set_error(g_pbp_disc_index > 0 ? "PBP has no such disc"
                      : "PBP has no PS1 disc image");
      return -1;
    }
    *disc_offset = psar + disc_relative;
  } else if (memcmp(buffer, "PSISOIMG0000", 12) == 0) {
    *disc_offset = psar;
  } else {
    ra_hash_status_set_error("PBP DATA.PSAR is not a PS1 disc image");
    return -1;
  }

  if (ra_seek(cd->file, (int64_t)*disc_offset, SEEK_SET) != 0 ||
    ra_read_exact(cd->file, buffer, 12) != 0 ||
    memcmp(buffer, "PSISOIMG0000", 12) != 0) {
    ra_hash_status_set_error("PBP does not contain PSISOIMG0000");
    return -1;
  }
  return 0;
}

/* The index table ends at the first entry with a zero stored length. */
static int pbp_count_index_blocks(ra_cd_t *cd) {
  uint8_t chunk[2048];
  uint32_t count = 0;
  size_t got;
  size_t i;
  int done = 0;

  while (!done && count < RA_PBP_MAX_BLOCKS) {
    if (ra_seek(cd->file, (int64_t)(cd->pbp_index + (uint64_t)count * 32u),
          SEEK_SET) != 0)
      break;
    got = ra_read(cd->file, chunk, sizeof(chunk));
    if (got < 32)
      break;
    for (i = 0; i + 32 <= got; i += 32) {
      if (chunk[i + 4] == 0 && chunk[i + 5] == 0) {
        done = 1;
        break;
      }
      ++count;
    }
  }
  if (count == 0) {
    ra_hash_status_set_error("PBP PSISOIMG index is empty");
    return -1;
  }
  cd->block_count = count;
  return 0;
}

static void pbp_publish_geometry(ra_cd_t *cd) {
  char geometry[128];

  snprintf(geometry, sizeof(geometry),
      "blocks=%u index=0x%X data=0x%X size=%llu",
      (unsigned)cd->block_count, (unsigned)cd->pbp_index,
      (unsigned)cd->pbp_data, (unsigned long long)cd->file_size);
  ra_hash_status_set_geometry(geometry);
}

static int ra_cd_init_pbp(ra_cd_t *cd) {
  uint64_t disc_offset = 0;

  if (pbp_locate_disc(cd, &disc_offset) != 0)
    return -1;

  cd->is_pbp = 1;
  cd->sector_size = 2352;
  cd->first_sector = 0;
  cd->block_size = RA_PBP_BLOCK_SIZE;
  cd->pbp_index = disc_offset + RA_PBP_INDEX_OFFSET;
  cd->pbp_data = disc_offset + RA_PBP_DATA_OFFSET;
  cd->block = (uint8_t*)malloc(RA_PBP_BLOCK_SIZE);
  if (cd->block == NULL)
    return -1;

  if (pbp_count_index_blocks(cd) != 0) {
    free(cd->block);
    cd->block = NULL;
    return -1;
  }
  cd->block_index = 0xFFFFFFFFu;
  cd->idx_count = 0;
  if (cd->ra_buf != NULL) free(cd->ra_buf);
  cd->ra_buf = NULL;
  cd->ra_count = 0;
  ra_hash_status_set_block_error("none");
  pbp_publish_geometry(cd);


  if (ra_hash_cd_load_pbp_block(cd, 0) != 0 ||
    memcmp(cd->block, RA_PBP_SYNC, 12) != 0) {
    ra_hash_status_set_error("PBP disc image is not raw MODE2/2352");
    free(cd->ra_buf);
    cd->ra_buf = NULL;
    cd->ra_count = 0;
    free(cd->block);
    cd->block = NULL;
    return -1;
  }
  return 0;
}

static int cd_open_cso(ra_cd_t *cd) {
  uint8_t header[24];
  uint32_t header_size;
  uint64_t total_size;
  uint32_t block_size;
  uint32_t index_bytes;

  if (ra_read_exact(cd->file, header, sizeof(header)) != 0 ||
    memcmp(header, "CISO", 4) != 0)
    return -1;

  header_size = (uint32_t)header[4] | ((uint32_t)header[5] << 8) |
        ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
  total_size = (uint64_t)header[8] | ((uint64_t)header[9] << 8) |
        ((uint64_t)header[10] << 16) | ((uint64_t)header[11] << 24) |
        ((uint64_t)header[12] << 32) | ((uint64_t)header[13] << 40) |
        ((uint64_t)header[14] << 48) | ((uint64_t)header[15] << 56);
  block_size = (uint32_t)header[16] | ((uint32_t)header[17] << 8) |
        ((uint32_t)header[18] << 16) | ((uint32_t)header[19] << 24);
  cd->align = header[21];
  if (block_size < 2048 || block_size > (16u * 1024u * 1024u) ||
    cd->align > 31 || total_size == 0 || total_size > 0x80000000ULL)
    return -1;

  cd->is_cso = 1;
  cd->block_size = block_size;
  cd->block_count = (uint32_t)((total_size + block_size - 1) / block_size);
  index_bytes = (size_t)cd->block_count * 4;
  cd->index = (uint32_t*)malloc(index_bytes);
  cd->block = (uint8_t*)malloc(block_size);
  if (cd->index == NULL || cd->block == NULL ||
    ra_seek(cd->file, header_size, SEEK_SET) != 0 ||
    ra_read(cd->file, cd->index, index_bytes) != index_bytes)
    return -1;
  cd->block_index = 0xFFFFFFFFu;
  cd->idx_count = 0;
  if (cd->ra_buf != NULL) free(cd->ra_buf);
  cd->ra_buf = NULL;
  cd->ra_count = 0;
  return 0;
}

void *ra_cd_open(const char *path, uint32_t track) {
  ra_cd_t *cd;
  const char *extension;
  uint64_t size = 0;
  uint64_t mtime = 0;

  (void)track;
  if (ra_file_info(path, &size, &mtime) != 0)
    return NULL;

  cd = (ra_cd_t*)calloc(1, sizeof(*cd));
  if (cd == NULL)
    return NULL;
  cd->file_size = size;
  cd->sector_size = 2048;
  cd->first_sector = 0;
  cd->file = ra_open(path);
  if (cd->file == NULL) {
    free(cd);
    return NULL;
  }

  extension = strrchr(path, '.');
  if (extension != NULL && strcasecmp(extension, ".cso") == 0) {
    if (cd_open_cso(cd) != 0) {
      free(cd->index);
      free(cd->block);
      ra_close(cd->file);
      free(cd);
      return NULL;
    }
  } else if (extension != NULL && strcasecmp(extension, ".pbp") == 0) {
    if (ra_cd_init_pbp(cd) != 0) {
      free(cd->block);
      ra_close(cd->file);
      free(cd);
      return NULL;
    }
  }

  return cd;
}

static size_t cd_read_pbp(ra_cd_t *cd, uint32_t sector, uint8_t *out,
    size_t requested) {
  uint32_t block;
  uint32_t block_offset;
  size_t copy;

  if (sector >= cd->block_count * RA_PBP_BLOCK_SECTORS)
    return 0;
  block = sector / RA_PBP_BLOCK_SECTORS;
  block_offset = (sector % RA_PBP_BLOCK_SECTORS) * cd->sector_size +
        RA_PBP_USER_OFFSET;
  if (requested > cd->sector_size - RA_PBP_USER_OFFSET)
    requested = cd->sector_size - RA_PBP_USER_OFFSET;
  if (cd->block_index != block && ra_hash_cd_load_pbp_block(cd, block) != 0)
    return 0;
  copy = requested;
  if (block_offset + copy > cd->block_length) {
    copy = block_offset < cd->block_length
        ? (size_t)(cd->block_length - block_offset)
        : 0;
  }
  if (copy > 0)
    memcpy(out, cd->block + block_offset, copy);
  return copy;
}
static int ra_cd_load_block(ra_cd_t *cd, uint32_t block) {
  uint32_t entry = cd->index[block];
  uint32_t start = (entry & 0x7FFFFFFFu) << cd->align;
  uint32_t end;
  uint32_t block_start = block * cd->block_size;
  uint32_t block_length = cd->block_size;
  int compressed = (entry & 0x80000000u) == 0;

  if (block + 1 < cd->block_count)
    end = (cd->index[block + 1] & 0x7FFFFFFFu) << cd->align;
  else
    end = (uint32_t)cd->file_size;

  if (block_start >= cd->file_size)
    return -1;
  if (block_start + block_length > cd->file_size)
    block_length = (uint32_t)(cd->file_size - block_start);
  if (end < start || end > cd->file_size)
    return -1;

  if (compressed) {
    size_t packed_size = (size_t)(end - start);
    unsigned long source_length = (unsigned long)packed_size;
    unsigned long dest_length = (unsigned long)cd->block_size;
    uint8_t *packed;
    int result;

    if (packed_size == 0 || packed_size > (size_t)cd->block_size * 2u)
      return -1;
    packed = (uint8_t*)malloc(packed_size);
    if (packed == NULL)
      return -1;
    if (ra_seek(cd->file, start, SEEK_SET) != 0 ||
      ra_read_exact(cd->file, packed, packed_size) != 0) {
      free(packed);
      return -1;
    }
    result = puff(cd->block, &dest_length, packed, &source_length);
    free(packed);
    if (result != 0)
      return -1;
    cd->block_length = (uint32_t)dest_length;
  } else {
    if (ra_seek(cd->file, start, SEEK_SET) != 0 ||
      ra_read_exact(cd->file, cd->block, block_length) != 0)
      return -1;
    cd->block_length = block_length;
  }

  cd->block_index = block;
  return 0;
}

static size_t cd_read_cso(ra_cd_t *cd, uint32_t sector, uint8_t *out,
    size_t requested) {
  size_t done = 0;
  uint64_t offset;

  while (done < requested) {
    uint32_t block;
    uint32_t block_offset;
    size_t copy;

    offset = (uint64_t)sector * cd->sector_size + done;
    if (offset >= cd->file_size)
      break;
    block = (uint32_t)(offset / cd->block_size);
    block_offset = (uint32_t)(offset % cd->block_size);
    if (block >= cd->block_count)
      break;
    if (cd->block_index != block) {
      if (ra_cd_load_block(cd, block) != 0)
        break;
    }
    if (block_offset >= cd->block_length)
      break;
    copy = cd->block_length - block_offset;
    if (copy > requested - done)
      copy = requested - done;
    memcpy(out + done, cd->block + block_offset, copy);
    done += copy;
  }
  return done;
}

size_t ra_cd_read(void *handle, uint32_t sector, void *buffer,
            size_t requested) {
  ra_cd_t *cd = (ra_cd_t*)handle;
  uint8_t *out = (uint8_t*)buffer;
  uint64_t offset;

  if (cd == NULL || buffer == NULL)
    return 0;

  if (cd->is_pbp)
    return cd_read_pbp(cd, sector, out, requested);

  if (!cd->is_cso) {
    offset = (uint64_t)sector * cd->sector_size;
    if (ra_seek(cd->file, (int64_t)offset, SEEK_SET) != 0)
      return 0;
    return ra_read(cd->file, buffer, requested);
  }

  return cd_read_cso(cd, sector, out, requested);
}

uint32_t ra_cd_first_sector(void *handle) {
  return handle != NULL ? ((ra_cd_t*)handle)->first_sector : 0;
}

void ra_cd_close(void *handle) {
  ra_cd_t *cd = (ra_cd_t*)handle;
  if (cd == NULL)
    return;
  free(cd->index);
  free(cd->block);
  free(cd->ra_buf);
  ra_close(cd->file);
  free(cd);
}

int ra_hash_read_titleid(const char *path, char title_id[11]) {
  ra_cd_t *cd = ra_cd_open(path, 0);
  uint8_t sector[2048];
  size_t got;

  if (cd == NULL)
    return 0;
  got = ra_cd_read(cd, 16, sector, sizeof(sector));
  ra_cd_close(cd);
  if (got < 883 + 10)
    return 0;

  memcpy(title_id, sector + 883, 10);
  title_id[10] = '\0';
  return 1;
}



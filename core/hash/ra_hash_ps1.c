#include "ra_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "puff.h"
#include "rc_hash_internal.h"
#include "ra_hash_cd.h"
#include "ra_hash_io.h"
#include "ra_hash_internal.h"

static uint32_t ra_hash_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}


static int ra_iso_name_matches(const uint8_t *rec, uint32_t rec_length,
                               const char *name) {
  size_t want = strlen(name);
  uint32_t length;

  if (rec_length < 33 || name[0] == '\0')
    return 0;
  length = rec[32];
  if (length < want || length > rec_length - 33)
    return 0;
  if (strncasecmp((const char*)(rec + 33), name, want) != 0)
    return 0;
  return length == want || rec[33 + want] == ';';
}

static int ra_iso_find(ra_cd_t *cd, uint32_t dir_lba, uint32_t dir_size,
          const char *name, uint32_t *out_lba, uint32_t *out_size) {
  uint8_t sector[2048];
  uint32_t sectors = (dir_size + 2047u) / 2048u;
  uint32_t s;

  if (sectors == 0)
    sectors = 1;
  for (s = 0; s < sectors; ++s) {
    uint32_t offset = 0;

    if (ra_cd_read(cd, dir_lba + s, sector, sizeof(sector)) < sizeof(sector))
      return 0;
    while (offset + 33 <= sizeof(sector)) {
      uint32_t length = sector[offset];

      if (length == 0 || offset + length > sizeof(sector))
        break;
      if (sector[offset + 32] != 0 &&
        ra_iso_name_matches(sector + offset, length, name)) {
                *out_lba = ra_hash_u32(sector + offset + 2);
                *out_size = ra_hash_u32(sector + offset + 10);
        return 1;
      }
      offset += length;
    }
  }
  return 0;
}

/* Boot payload signature, captured while walking the container: POPS strips the
 * 0x800-byte PS-X EXE header, so the game's first bytes appear at
 * `t_addr & 0x1FFFFF` in PS1 RAM . The runtime refuses POPS reads
 * until this signature is present, so a trigger can never evaluate the POPS
 * loader's leftovers (the 0x2A2A2A29 "First Credit High Score" submission). */
static uint32_t g_ps1_entry_offset;
static uint8_t g_ps1_entry_sample[32];
static int g_ps1_entry_valid;

int ra_hash_ps1_entry_point(uint32_t *offset, uint8_t sample[32]) {
  if (!g_ps1_entry_valid || offset == NULL || sample == NULL)
    return 0;
  *offset = g_ps1_entry_offset;
  memcpy(sample, g_ps1_entry_sample, sizeof(g_ps1_entry_sample));
  return 1;
}

static int ps1_boot_line(const char *text, char *boot, size_t boot_size) {
  const char *line;
  const char *start;
  int i;

  boot[0] = '\0';
  for (line = text; *line != '\0'; ) {
    const char *end = line;
    while (*end != '\0' && *end != '\n')
      ++end;
    if (strncasecmp(line, "BOOT", 4) == 0) {
      start = line + 4;
      while (start < end && (*start == ' ' || *start == '\t'))
        ++start;
      if (start < end && *start == '=') {
        ++start;
        while (start < end && (*start == ' ' || *start == '\t'))
          ++start;
        if (strncasecmp(start, "cdrom:", 6) == 0)
          start += 6;
        while (start < end && *start == '\\')
          ++start;
        i = 0;
        while (start < end && *start != ';' && *start != ' ' &&
                       *start != '\r' && i < (int)boot_size - 1)
          boot[i++] = *start++;
        boot[i] = '\0';
        break;
      }
    }
    line = (*end == '\n') ? end + 1 : end;
    if (*line == '\0')
      break;
  }
  return boot[0] != '\0';
}

/* The 0x800-byte header fills the first sector, so the payload starts at
     * file offset 0x800 = the next sector . Reading it out of
     * `sector` would have been past the buffer. */
static void ps1_capture_entry(ra_cd_t *cd, uint32_t exe_lba,
    const uint8_t *exe_header) {
  uint8_t sample[2048];

  g_ps1_entry_offset = ra_hash_u32(exe_header + 0x18) & 0x1FFFFFu;
  g_ps1_entry_valid = 0;
  if (ra_cd_read(cd, exe_lba + 1, sample, sizeof(sample)) >=
    sizeof(g_ps1_entry_sample)) {
    memcpy(g_ps1_entry_sample, sample, sizeof(g_ps1_entry_sample));
    g_ps1_entry_valid = 1;
  }
}

static int ps1_hash_executable(ra_cd_t *cd, uint32_t exe_lba, uint32_t exe_size,
    const char *boot, char hash[33]) {
  uint8_t sector[2048];
  uint32_t size;
  uint32_t offset;
  md5_state_t md5;
  uint8_t digest[16];
  static const char hex[] = "0123456789abcdef";
  int i;

  if (ra_cd_read(cd, exe_lba, sector, sizeof(sector)) < sizeof(sector)) {
    ra_hash_status_set_error("container boot executable is truncated");
    return 0;
  }
  size = exe_size;
  if (memcmp(sector, "PS-X EXE", 8) == 0) {
    size = ra_hash_u32(sector + 28) + 2048u;
    ps1_capture_entry(cd, exe_lba, sector);
  }

  md5_init(&md5);
  md5_append(&md5, (const md5_byte_t*)boot, (int)strlen(boot));

  offset = 0;
  while (offset < size) {
    uint32_t take = size - offset;
    size_t got;

    if (take > sizeof(sector))
      take = sizeof(sector);
    got = ra_cd_read(cd, exe_lba + offset / 2048u, sector, take);
    if (got != take) {
      ra_hash_status_set_error("container image read failed");
      return 0;
    }
    md5_append(&md5, (const md5_byte_t*)sector, (int)got);
    offset += take;
  }

  md5_finish(&md5, digest);
  for (i = 0; i < 16; ++i) {
    hash[i * 2] = hex[digest[i] >> 4];
    hash[i * 2 + 1] = hex[digest[i] & 15];
  }
  hash[32] = '\0';
  return 1;
}

static int ps1_read_cnf(ra_cd_t *cd, uint32_t cnf_lba, uint32_t cnf_size,
    char *text, size_t capacity) {
  uint8_t cnf[2048];

  if (cnf_size > capacity - 1)
    cnf_size = capacity - 1;
  if (ra_cd_read(cd, cnf_lba, cnf, cnf_size) < cnf_size)
    return 0;
  memcpy(text, cnf, cnf_size);
  text[cnf_size] = '\0';
  return 1;
}

int ra_hash_ps1_from_container(const char *path, char hash[33]) {
  ra_cd_t *cd;
  uint8_t pvd[2048];
  uint32_t root_lba;
  uint32_t root_size;
  uint32_t cnf_lba;
  uint32_t cnf_size;
  uint32_t exe_lba;
  uint32_t exe_size;
  char boot[64];
  char text[2049];
  int result = 0;

  if (path == NULL || hash == NULL)
    return 0;
  cd = ra_cd_open(path, 0);
  if (cd == NULL || !cd->is_pbp) {
    ra_hash_status_set_error("container is not a PBP");
    ra_cd_close(cd);
    return 0;
  }

  if (ra_cd_read(cd, 16, pvd, sizeof(pvd)) < sizeof(pvd) ||
    memcmp(pvd + 1, "CD001", 5) != 0) {
    ra_hash_status_set_error("container has no CD001 volume");
    goto done;
  }
  root_lba = ra_hash_u32(pvd + 158);
  root_size = ra_hash_u32(pvd + 166);
  if (!ra_iso_find(cd, root_lba, root_size, "SYSTEM.CNF", &cnf_lba, &cnf_size)) {
    ra_hash_status_set_error("container has no SYSTEM.CNF");
    goto done;
  }

  /* SYSTEM.CNF as text: copy and terminate. */
  if (!ps1_read_cnf(cd, cnf_lba, cnf_size, text, sizeof(text)))
    goto done;

  if (!ps1_boot_line(text, boot, sizeof(boot))) {
    ra_hash_status_set_error("container SYSTEM.CNF has no BOOT line");
    goto done;
  }

  if (!ra_iso_find(cd, root_lba, root_size, boot, &exe_lba, &exe_size)) {
    ra_hash_status_set_error("container has no boot executable");
    goto done;
  }

  if (!ps1_hash_executable(cd, exe_lba, exe_size, boot, hash))
    goto done;
  result = 1;

done:
  ra_cd_close(cd);
  return result;
}

/* Distinguish an unreadable container from a failed ISO9660 walk on PS1 hash failure. */
static void ra_pbp_describe_volume(const char *path) {
  ra_cd_t *cd;
  uint8_t pvd[256];
  size_t got;
  char message[128];

  if (path == NULL || strrchr(path, '.') == NULL ||
    strcasecmp(strrchr(path, '.'), ".pbp") != 0)
    return;

  cd = ra_cd_open(path, 0);
  if (cd == NULL) {
    snprintf(message, sizeof(message), "container reopen failed (%.90s)",
        ra_hash_status_last_block_error());
    ra_hash_status_set_block_error(message);
    return;
  }

  got = ra_cd_read(cd, 16, pvd, sizeof(pvd));
  if (got < 7) {
    snprintf(message, sizeof(message), "PVD read %u bytes (%s)",
        (unsigned)got, ra_hash_status_last_block_error());
    ra_hash_status_set_block_error(message);
  } else if (memcmp(pvd + 1, "CD001", 5) != 0) {
    snprintf(message, sizeof(message),
        "sector 16 is not a PVD (%02X%02X%02X%02X)", pvd[0], pvd[1],
        pvd[2], pvd[3]);
    ra_hash_status_set_block_error(message);
  } else {
    snprintf(message, sizeof(message), "PVD ok root=%u size=%u blocks=%u",
        (unsigned)ra_hash_u32(pvd + 158),
        (unsigned)ra_hash_u32(pvd + 166), (unsigned)cd->block_count);
    ra_hash_status_set_block_error(message);
  }

  ra_cd_close(cd);
}

static int ps1_try_rc_hash(const char *path, char hash[33]) {
  rc_hash_iterator_t iterator;

  memset(&iterator, 0, sizeof(iterator));
  rc_hash_initialize_iterator(&iterator, path, NULL, 0);
  iterator.callbacks.error_message = ra_hash_error_cb_public;
  iterator.callbacks.filereader = *ra_hash_filereader();
  iterator.callbacks.cdreader = *ra_hash_cdreader();

  ra_hash_status_set_error("rc_hash_psx did not return a hash");
  if (!rc_hash_psx(hash, &iterator)) {
    ra_pbp_describe_volume(path);
    rc_hash_destroy_iterator(&iterator);
    return 0;
  }
  rc_hash_destroy_iterator(&iterator);
  return 1;
}

static int ps1_container_fallback(const char *path, char hash[33]) {
  /* rc_hash_psx cannot walk this container on device even though the
       * volume reads correctly . Use the documented walk
       * directly: same rules, same bytes, verified against the four fixtures
       * and the authoritative .bin on the host. */
  if (!ra_hash_ps1_from_container(path, hash)) {
    /* Keep the specific reason . Overwriting it with one generic string
           * is why the Disc 2 failure reached the player log as an unexplained
           * "PS1 container walk failed" while the real cause was a 0x204-byte table
           * buffer that could not hold entry 1. */
    char detail[96];

    snprintf(detail, sizeof(detail), "%s", ra_hash_last_error());
    if (detail[0] == '\0' || strcmp(detail, "ok") == 0)
      ra_hash_status_set_error("PS1 container walk failed");
    else
      ra_hash_status_set_error(detail);
    return 0;
  }
  /* Visible in the runtime log next to the hash, so a fallback session is
       * never silent. */
  ra_hash_status_set_block_error("rc_hash_psx failed; container walk used");
  return 1;
}

int ra_hash_ps1(const char *path, const char *cache_path,
        ra_hash_result_t *result) {
  const char *extension;
  char hash[33];
  uint64_t size = 0;
  uint64_t mtime = 0;

  if (result == NULL)
    return 0;
  memset(result, 0, sizeof(*result));
  if (path == NULL || path[0] == '\0') {
    ra_hash_status_set_error("empty game path");
    return 0;
  }
  strncpy(result->path, path, sizeof(result->path) - 1);
  result->path[sizeof(result->path) - 1] = '\0';

  if (ra_file_info(path, &size, &mtime) != 0) {
    ra_hash_status_set_error("game file not found");
    return 0;
  }
  result->file_size = size;
  result->file_mtime = mtime;

  if (ra_cache_lookup(cache_path, path, size, mtime, hash)) {
    strncpy(result->hash, hash, 32);
    result->hash[32] = '\0';
    result->from_cache = 1;
    ra_hash_status_set_error("ok");
    return 1;
  }

  extension = strrchr(path, '.');
  if (extension == NULL ||
    (strcasecmp(extension, ".pbp") != 0 &&
    strcasecmp(extension, ".iso") != 0 &&
    strcasecmp(extension, ".cso") != 0)) {
    ra_hash_status_set_error("unsupported PS1 image extension");
    return 0;
  }

  if (!ps1_try_rc_hash(path, hash) && !ps1_container_fallback(path, hash))
    return 0;

  hash[32] = '\0';
  strncpy(result->hash, hash, 32);
  result->hash[32] = '\0';
  ra_cache_store(cache_path, path, size, mtime, result->hash);
  ra_hash_status_set_error("ok");
  return 1;
}

int ra_hash_probe_ps1(const char *path) {
  ra_cd_t *cd;
  int result;

  if (path == NULL || path[0] == '\0')
    return 0;
  cd = ra_cd_open(path, 0);
  if (cd == NULL)
    return 0;
  result = cd->is_pbp;
  ra_cd_close(cd);
  return result;
}



#include "ra_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rc_hash_internal.h"
#include "ra_hash_internal.h"
#include "ra_hash_io.h"
#include "ra_hash_cd.h"

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#else
#include <sys/stat.h>
#endif

static char g_error[256] = "hash not attempted";
static char g_block_error[128] = "none";
static char g_geometry[128] = "none";

void ra_hash_status_set_error(const char *message) {
  if (message == NULL)
    message = "unknown hash error";
  strncpy(g_error, message, sizeof(g_error) - 1);
  g_error[sizeof(g_error) - 1] = '\0';
}

void ra_hash_status_set_block_error(const char *message) {
  strncpy(g_block_error, message, sizeof(g_block_error) - 1);
  g_block_error[sizeof(g_block_error) - 1] = '\0';
}

void ra_hash_status_set_geometry(const char *message) {
  strncpy(g_geometry, message, sizeof(g_geometry) - 1);
  g_geometry[sizeof(g_geometry) - 1] = '\0';
}

const char *ra_hash_status_last_error(void) { return g_error; }
const char *ra_hash_status_last_block_error(void) { return g_block_error; }
const char *ra_hash_status_geometry(void) { return g_geometry; }

static void ra_set_error(const char *message) {
  ra_hash_status_set_error(message);
}

static void ra_set_block_error(const char *message) {
  ra_hash_status_set_block_error(message);
}

const char *ra_hash_last_error(void) { return ra_hash_status_last_error(); }
const char *ra_hash_last_block_error(void) { return ra_hash_status_last_block_error(); }
const char *ra_hash_pbp_geometry(void) { return ra_hash_status_geometry(); }


static void ra_hash_error_cb(const char *message,
              const rc_hash_iterator_t *iterator) {
  (void)iterator;
  ra_set_error(message);
}

static const rc_hash_filereader_t g_filereader = {
  ra_hash_file_open,
  ra_hash_file_seek,
  ra_hash_file_tell,
  ra_hash_file_read,
  ra_hash_file_close
};

static const rc_hash_cdreader_t g_cdreader = {
  ra_cd_open,
  ra_cd_read,
  ra_cd_close,
  ra_cd_first_sector,
  NULL
};

void ra_hash_error_cb_public(const char *message,
              const rc_hash_iterator_t *iterator) {
  ra_hash_error_cb(message, iterator);
}

const rc_hash_filereader_t *ra_hash_filereader(void) { return &g_filereader; }
const rc_hash_cdreader_t *ra_hash_cdreader(void) { return &g_cdreader; }

static int hash_validate(const char *path, ra_hash_result_t *result,
                         uint64_t *size, uint64_t *mtime) {
  const char *extension;

  if (ra_file_info(path, size, mtime) != 0) {
    ra_set_error("game file not found");
    return 0;
  }
  result->file_size = *size;
  result->file_mtime = *mtime;
  extension = strrchr(path, '.');
  if (extension == NULL ||
      (strcasecmp(extension, ".iso") != 0 &&
       strcasecmp(extension, ".cso") != 0 &&
       strcasecmp(extension, ".pbp") != 0)) {
    ra_set_error("unsupported game extension");
    return 0;
  }
  return 1;
}

static int hash_compute(const char *path, char hash[33]) {
  rc_hash_iterator_t iterator;

  memset(&iterator, 0, sizeof(iterator));
  rc_hash_initialize_iterator(&iterator, path, NULL, 0);
  iterator.callbacks.error_message = ra_hash_error_cb;
  iterator.callbacks.filereader = g_filereader;
  iterator.callbacks.cdreader = g_cdreader;
  ra_set_error("rhash did not return a hash");
  if (!rc_hash_psp(hash, &iterator)) {
    rc_hash_destroy_iterator(&iterator);
    return 0;
  }
  rc_hash_destroy_iterator(&iterator);
  hash[32] = '\0';
  return 1;
}

int ra_hash_game(const char *path, const char *cache_path,
        ra_hash_result_t *result) {
  char hash[33];
  uint64_t size = 0;
  uint64_t mtime = 0;

  if (result == NULL)
    return 0;
  memset(result, 0, sizeof(*result));
  if (path == NULL || path[0] == '\0') {
    ra_set_error("empty game path");
    return 0;
  }
  strncpy(result->path, path, sizeof(result->path) - 1);
  result->path[sizeof(result->path) - 1] = '\0';
  if (!hash_validate(path, result, &size, &mtime))
    return 0;
  if (ra_cache_lookup(cache_path, path, size, mtime, hash)) {
    strncpy(result->hash, hash, 32);
    result->hash[32] = '\0';
    result->from_cache = 1;
    ra_set_error("ok");
    return 1;
  }
  if (!hash_compute(path, hash))
    return 0;
  strncpy(result->hash, hash, 32);
  result->hash[32] = '\0';
  ra_cache_store(cache_path, path, size, mtime, result->hash);
  ra_set_error("ok");
  return 1;
}


/* ISO9660 walk fallback for rc_hash_psx: PVD -> root -> SYSTEM.CNF -> BOOT. */


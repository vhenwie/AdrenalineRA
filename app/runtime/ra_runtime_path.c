#include "ra_runtime_path.h"
#include "ra_runtime_internal.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>

#include "ra_hash.h"
#include "ra_runtime.h"
/* Qualified: libvita2d ships its own utils.h and is searched first. */
#include "../../third_party/Adrenaline/user/utils.h"

int ra_runtime_translate_psp_path(const char *input, char *output, size_t capacity) {
  const char *prefix = NULL;

  if (input == NULL || input[0] == '\0')
    return 0;
  if (strncmp(input, "ms0:", 4) == 0)
    prefix = getPspemuMemoryStickLocation();
  else if (strncmp(input, "ef0:", 4) == 0)
    prefix = getPspemuEfLocation();

  if (prefix != NULL)
    snprintf(output, capacity, "%s%s", prefix, input + 4);
  else
    snprintf(output, capacity, "%s", input);
  return 1;
}

int ra_runtime_file_exists(const char *path) {
  SceIoStat stat;
  return path != NULL && sceIoGetstat(path, &stat) >= 0;
}

int ra_runtime_normalize_titleid(const char raw[11], char normalized[11]) {
  int i;
  int j = 0;

  for (i = 0; i < 10 && raw[i] != '\0'; ++i) {
    if (raw[i] != '-' && raw[i] != ' ')
      normalized[j++] = raw[i];
  }
  normalized[j] = '\0';
  return j == 9;
}

int ra_runtime_scan_for_title(const char *directory, const char *titleid,
            char *output, size_t capacity, int depth) {
  SceUID dfd;
  SceIoDirent entry;

  if (depth > 2 || directory == NULL)
    return 0;
  dfd = sceIoDopen(directory);
  if (dfd < 0)
    return 0;

  for (;;) {
    char path[RA_HASH_PATH_MAX];
    char raw_title[11];
    char normalized[11];
    const char *extension;

    memset(&entry, 0, sizeof(entry));
    if (sceIoDread(dfd, &entry) <= 0)
      break;
    if (strcmp(entry.d_name, ".") == 0 ||
      strcmp(entry.d_name, "..") == 0)
      continue;
    snprintf(path, sizeof(path), "%s/%s", directory, entry.d_name);

    if (SCE_SO_ISDIR(entry.d_stat.st_mode)) {
      if (ra_runtime_scan_for_title(path, titleid, output, capacity, depth + 1)) {
        sceIoDclose(dfd);
        return 1;
      }
      continue;
    }

    extension = strrchr(path, '.');
    if (extension == NULL ||
      (strcasecmp(extension, ".iso") != 0 &&
      strcasecmp(extension, ".cso") != 0))
      continue;
    if (!ra_hash_read_titleid(path, raw_title))
      continue;
    if (!ra_runtime_normalize_titleid(raw_title, normalized))
      continue;
    if (strcasecmp(normalized, titleid) == 0) {
      snprintf(output, capacity, "%s", path);
      sceIoDclose(dfd);
      return 1;
    }
  }

  sceIoDclose(dfd);
  return 0;
}

int ra_runtime_resolve_game_path(const char *filename, const char *titleid,
              char *output, size_t capacity) {
  const char *roots[2];
  char mapped[RA_HASH_PATH_MAX];
  char root[RA_HASH_PATH_MAX];
  int i;

  if (titleid == NULL || titleid[0] == '\0')
    return 0;

  if (filename != NULL && filename[0] != '\0' &&
    ra_runtime_translate_psp_path(filename, mapped, sizeof(mapped)) &&
    ra_runtime_file_exists(mapped)) {
    snprintf(output, capacity, "%s", mapped);
    return 1;
  }

  roots[0] = getPspemuMemoryStickLocation();
  roots[1] = getPspemuEfLocation();
  for (i = 0; i < 2; ++i) {
    if (roots[i] == NULL)
      continue;
    snprintf(root, sizeof(root), "%s/ISO", roots[i]);
    if (ra_runtime_scan_for_title(root, titleid, output, capacity, 0))
      return 1;
    snprintf(root, sizeof(root), "%s/PSP/GAME", roots[i]);
    if (ra_runtime_scan_for_title(root, titleid, output, capacity, 0))
      return 1;
  }
  return 0;
}


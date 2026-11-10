#include "ra_badge_png.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "ra_badge.h"
#include "ra_badge_state.h"

#define RA_BADGE_MAX_DIM 128

static unsigned int badge_be32(const unsigned char *data) {
  return ((unsigned int)data[0] << 24) | ((unsigned int)data[1] << 16) |
    ((unsigned int)data[2] << 8) | (unsigned int)data[3];
}


int ra_badge_png_header_ok(const unsigned char *data, size_t size,
              unsigned int *width, unsigned int *height) {
  unsigned int ihdr_length;
  unsigned int w;
  unsigned int h;

  if (data == NULL || size < 33)
    return 0;
  if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' ||
    data[3] != 'G' || data[4] != 0x0D || data[5] != 0x0A ||
    data[6] != 0x1A || data[7] != 0x0A)
    return 0;

  ihdr_length = badge_be32(data + 8);
  if (ihdr_length != 13 || data[12] != 'I' || data[13] != 'H' ||
    data[14] != 'D' || data[15] != 'R')
    return 0;

  w = badge_be32(data + 16);
  h = badge_be32(data + 20);
  if (w == 0 || h == 0 || w > RA_BADGE_MAX_DIM || h > RA_BADGE_MAX_DIM)
    return 0;

  if (width != NULL)
        *width = w;
  if (height != NULL)
        *height = h;
  return 1;
}
int ra_badge_png_write_atomic(const char *key, const unsigned char *data,
              size_t length) {
  char path[96];
  char temporary[100];
  SceUID file;
  size_t written = 0;

  if (!ra_badge_path(key, path, sizeof(path)))
    return 0;
  if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
    (int)sizeof(temporary))
    return 0;

  sceIoRemove(temporary);
  file = sceIoOpen(temporary, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
          0666);
  if (file < 0)
    return 0;
  while (written < length) {
    int result = sceIoWrite(file, data + written,
                (unsigned int)(length - written));

    if (result <= 0) {
      sceIoClose(file);
      sceIoRemove(temporary);
      return 0;
    }
    written += (size_t)result;
  }
  sceIoClose(file);
  if (sceIoRename(temporary, path) < 0) {
    sceIoRemove(temporary);
    return 0;
  }
  return 1;
}

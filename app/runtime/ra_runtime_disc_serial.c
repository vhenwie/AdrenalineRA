#include "ra_runtime_disc_serial.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>

#include "ra_hash.h"
#include "ra_runtime_internal.h"
#include "ra_runtime_memory.h"

/* Opens the PBP, validates its header and PSAR magic, and reports the container
 * offset for `disc_index`. Closes the file on every failure. */
static SceUID disc_pbp_open_container(const char *path, int disc_index,
                                      uint64_t *disc_offset_out) {
  SceUID fd;
  unsigned char header[0x28];
  unsigned char magic[16];
  uint64_t psar;
  uint64_t disc_offset;
  uint32_t relative;

  fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return -1;
  if (sceIoRead(fd, header, sizeof(header)) != (int)sizeof(header) ||
    memcmp(header, "\0PBP", 4) != 0) {
    sceIoClose(fd);
    return -1;
  }
  psar = (uint64_t)header[0x24] | ((uint64_t)header[0x25] << 8) |
    ((uint64_t)header[0x26] << 16) | ((uint64_t)header[0x27] << 24);
  sceIoLseek(fd, (SceOff)psar, SCE_SEEK_SET);
  if (sceIoRead(fd, magic, 16) != 16) {
    sceIoClose(fd);
    return -1;
  }
  if (memcmp(magic, "PSTITLEIMG000000", 16) == 0) {
    unsigned char entry[4];

    sceIoLseek(fd, (SceOff)(psar + 0x200 + 4u * (uint32_t)disc_index),
        SCE_SEEK_SET);
    if (sceIoRead(fd, entry, 4) != 4) {
      sceIoClose(fd);
      return -1;
    }
    relative = (uint32_t)entry[0] | ((uint32_t)entry[1] << 8) |
        ((uint32_t)entry[2] << 16) | ((uint32_t)entry[3] << 24);
    disc_offset = psar + relative;
  } else if (memcmp(magic, "PSISOIMG0000", 12) == 0 && disc_index == 0) {
    disc_offset = psar;
  } else {
    sceIoClose(fd);
    return -1;
  }
  *disc_offset_out = disc_offset;
  return fd;
}

/* `_SCUS_94163\0` -> `the title`: the same normalisation for both sides of the
 * comparison, so the RAM copy and the container copy compare directly. */
static int disc_serial_normalise(const unsigned char *serial, size_t serial_size,
                                 char *out, size_t capacity) {
  size_t i;
  size_t written = 0;

  for (i = 0; i < serial_size && written + 1 < capacity; ++i) {
    char c = (char)serial[i];

    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      out[written++] = c;
    else if (c == '\0')
      break;
  }
  out[written] = '\0';
  return written >= 9;
}

int ra_runtime_disc_pbp_serial(const char *path, int disc_index, char *out,
                size_t capacity) {
  SceUID fd;
  unsigned char serial[16];
  uint64_t disc_offset = 0;

  if (path == NULL || out == NULL || capacity < 12 || disc_index < 0)
    return 0;
  out[0] = '\0';

  fd = disc_pbp_open_container(path, disc_index, &disc_offset);
  if (fd < 0)
    return 0;
  if (disc_offset == 0 ||
    sceIoLseek(fd, (SceOff)(disc_offset + 0x400), SCE_SEEK_SET) < 0 ||
    sceIoRead(fd, serial, sizeof(serial)) != (int)sizeof(serial)) {
    sceIoClose(fd);
    return 0;
  }
  sceIoClose(fd);

  return disc_serial_normalise(serial, sizeof(serial), out, capacity);
}

/* Which disc POPS is playing right now, from the mapped container header. */
int ra_runtime_disc_live_serial(char *out, size_t capacity) {
  unsigned char raw[16];
  size_t i;
  size_t written = 0;

  if (out == NULL || capacity < 12)
    return 0;
  out[0] = '\0';
  /* The header only exists once POPS has the disc mounted; before that the window
     * still holds the loader (the same rule `read_psp_memory()` applies). */
  if (!ra_runtime_game_memory_valid() || !g_rt.ps1_loaded)
    return 0;
  memset(raw, 0, sizeof(raw));
  if (!ra_runtime_memory_read_psp(RA_PSP_DISC_SERIAL_ADDRESS, raw, sizeof(raw)))
    return 0;
  for (i = 0; i < sizeof(raw) && written + 1 < capacity; ++i) {
    char c = (char)raw[i];

    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      out[written++] = c;
    else if (c == '\0')
      break;
  }
  out[written] = '\0';
  return written >= 9;
}

#include "ra_shot.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "menu.h"
#include "pops.h"
#include "ra_logqueue.h"
#include "ra_runtime.h"

#define SHOT_DIR "ux0:data/AdrenalineRA/screenshots"
#define SHOT_QUEUE 4
#define COPY_QUEUE 8
#define FOLDER_MAX 48
#define SHOT_STACK 0x20000
#define COPY_STACK 0x8000
#define SHOT_PRIORITY 0x10000110
#define COPY_PRIORITY 0x10000130
#define STORED_BLOCK 65535

typedef struct {
  char folder[FOLDER_MAX];
  char name[FOLDER_MAX];
} ShotJob;

static ShotJob g_jobs[SHOT_QUEUE];
static int g_job_count;
static SceUID g_lock = -1;
static SceUID g_wake = -1;
static SceUID g_thread = -1;
static SceUID g_copy_thread = -1;
static unsigned int g_crc_table[256];
static int g_crc_ready;
static unsigned long long g_burst_until;
static ShotJob g_copies[COPY_QUEUE];
static int g_copy_count;
static char g_burst_source[192];
static int g_burst_ready;
static int g_dirs_ready;

static void shot_lock(void) {
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_shot", 0, 1, 1, NULL);
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

static void shot_unlock(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

static void shot_log(const char *format, ...) {
  char text[200];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length > 0)
    ra_logq_line(text, length);
}

static void sanitize(const char *title, char *out, size_t capacity) {
  size_t length = 0;

  if (out == NULL || capacity == 0)
    return;
  if (title == NULL)
    title = "";
  for (; *title != '\0' && length + 1 < capacity; ++title) {
    char c = *title;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_')
      out[length++] = c;
    else
      out[length++] = '_';
  }
  while (length > 0 && (out[length - 1] == '_' || out[length - 1] == '.'))
    --length;
  if (length == 0)
    snprintf(out, capacity, "unknown");
  else
    out[length] = '\0';
}

static unsigned int name_hash(const char *text) {
  unsigned int hash = 2166136261u;

  while (text != NULL && *text != '\0') {
    hash ^= (unsigned char)*text++;
    hash *= 16777619u;
  }
  return hash;
}

static void sanitize_name(const char *title, char *out, size_t capacity) {
  char full[256];

  sanitize(title, full, sizeof(full));
  if (strlen(full) + 1 <= capacity) {
    snprintf(out, capacity, "%s", full);
    return;
  }
  if (capacity > 10)
    snprintf(out, capacity, "%.*s_%08X", (int)(capacity - 10), full,
        name_hash(title));
  else
    snprintf(out, capacity, "%08X", name_hash(title));
}

static void job_path(char *out, size_t capacity, const char *folder,
                     const char *name) {
  snprintf(out, capacity, "%s/%s/%s.png", SHOT_DIR, folder, name);
}

static int copy_file(const char *source, const ShotJob *job) {
  char folder[FOLDER_MAX + 40];
  char target[192];
  unsigned char *buffer;
  SceUID in;
  SceUID out;
  SceIoStat source_stat;
  unsigned long long expected = 0;
  unsigned long long copied = 0;
  int ok = 1;
  int got = 0;

  if (source == NULL || source[0] == '\0' || job == NULL)
    return 0;
  sceIoMkdir("ux0:data/AdrenalineRA", 0777);
  sceIoMkdir(SHOT_DIR, 0777);
  snprintf(folder, sizeof(folder), "%s/%s", SHOT_DIR, job->folder);
  sceIoMkdir(folder, 0777);
  job_path(target, sizeof(target), job->folder, job->name);
  if (strcmp(source, target) == 0)
    return 1;

  buffer = (unsigned char *)malloc(32768);
  if (buffer == NULL)
    return 0;
  in = sceIoOpen(source, SCE_O_RDONLY, 0);
  out = in >= 0 ? sceIoOpen(target, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                            0666) : -1;
  if (in < 0 || out < 0)
    ok = 0;
  if (in >= 0 && sceIoGetstat(source, &source_stat) >= 0)
    expected = (unsigned long long)source_stat.st_size;
  while (ok && (got = sceIoRead(in, buffer, 32768)) > 0) {
    if (sceIoWrite(out, buffer, (unsigned int)got) != got)
      ok = 0;
    else
      copied += (unsigned long long)got;
  }
  if (ok && got < 0)
    ok = 0;
  if (ok && expected != 0 && copied != expected)
    ok = 0;
  if (in >= 0)
    sceIoClose(in);
  if (out >= 0)
    sceIoClose(out);
  if (!ok && out >= 0)
    sceIoRemove(target);
  free(buffer);
  return ok;
}

static void crc_init(void) {
  unsigned int i;
  unsigned int j;

  for (i = 0; i < 256; ++i) {
    unsigned int value = i;
    for (j = 0; j < 8; ++j)
      value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    g_crc_table[i] = value;
  }
  g_crc_ready = 1;
}

static unsigned int crc_bytes(const unsigned char *type,
              const unsigned char *data, size_t length) {
  unsigned int crc = 0xFFFFFFFFu;
  size_t i;

  if (!g_crc_ready)
    crc_init();
  for (i = 0; i < 4; ++i)
    crc = g_crc_table[(crc ^ type[i]) & 0xFF] ^ (crc >> 8);
  for (i = 0; i < length; ++i)
    crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

static void put_be32(unsigned char *out, unsigned int value) {
  out[0] = (unsigned char)(value >> 24);
  out[1] = (unsigned char)(value >> 16);
  out[2] = (unsigned char)(value >> 8);
  out[3] = (unsigned char)value;
}

static int png_write(int file, const unsigned char *data, size_t length) {
  while (length > 0) {
    size_t chunk = length > 32768 ? 32768 : length;

    if (sceIoWrite(file, data, (unsigned int)chunk) != (int)chunk)
      return 0;
    data += chunk;
    length -= chunk;
    if (length > 0)
      sceKernelDelayThread(1000);
  }
  return 1;
}

static int png_chunk(int file, const char *type, const unsigned char *data,
          size_t length) {
  unsigned char header[8];
  unsigned char crc[4];

  put_be32(header, (unsigned int)length);
  memcpy(header + 4, type, 4);
  put_be32(crc, crc_bytes((const unsigned char*)type, data, length));
  if (sceIoWrite(file, header, 8) != 8)
    return 0;
  if (length > 0 && !png_write(file, data, length))
    return 0;
  return sceIoWrite(file, crc, 4) == 4;
}

static unsigned int adler32_bytes(const unsigned char *data, size_t length) {
  unsigned int a = 1;
  unsigned int b = 0;
  size_t i;

  for (i = 0; i < length; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

/* Expands the framebuffer into RGBA scanlines, each with its filter byte. */
static void shot_fill_raw(const uint32_t *source, size_t pitch, int width, int height,
                          unsigned char *raw) {
  size_t offset = 0;
  int x;
  int y;

  for (y = 0; y < height; ++y) {
    raw[offset++] = 0;
    for (x = 0; x < width; ++x) {
      uint32_t color = source[(size_t)y * pitch + x];
      raw[offset++] = (unsigned char)(color & 0xFF);
      raw[offset++] = (unsigned char)((color >> 8) & 0xFF);
      raw[offset++] = (unsigned char)((color >> 16) & 0xFF);
      raw[offset++] = 0xFF;
    }
  }
}

/* Creates the output folder and opens the PNG, filling `path`. */
static int shot_open_output(const ShotJob *job, char *path, size_t capacity) {
  char folder[FOLDER_MAX + 40];

  sceIoMkdir("ux0:data/AdrenalineRA", 0777);
  sceIoMkdir(SHOT_DIR, 0777);
  snprintf(folder, sizeof(folder), "%s/%s", SHOT_DIR, job->folder);
  sceIoMkdir(folder, 0777);
  snprintf(path, capacity, "%s/%s.png", folder, job->name);
  return sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
}

/* Writes the PNG signature, IHDR, the stored-deflate IDAT blocks and IEND. */
static int shot_emit_png(int file, const unsigned char *raw, size_t raw_size,
                         int width, int height, unsigned char *block) {
  unsigned char ihdr[13];
  unsigned char zlib_header[2];
  static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 0x0D,
                        0x0A, 0x1A, 0x0A };
  size_t offset;
  int ok;

  put_be32(ihdr, (unsigned)width);
  put_be32(ihdr + 4, (unsigned)height);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  zlib_header[0] = 0x78;
  zlib_header[1] = 0x01;
  ok = sceIoWrite(file, signature, 8) == 8 &&
    png_chunk(file, "IHDR", ihdr, sizeof(ihdr)) &&
    png_chunk(file, "IDAT", zlib_header, sizeof(zlib_header));

  for (offset = 0; ok && offset < raw_size; offset += STORED_BLOCK) {
    size_t length = raw_size - offset;
    unsigned int block_length;

    if (length > STORED_BLOCK)
      length = STORED_BLOCK;
    block_length = (unsigned int)length;
    block[0] = 0x00;
    block[1] = (unsigned char)(block_length & 0xFF);
    block[2] = (unsigned char)((block_length >> 8) & 0xFF);
    block[3] = (unsigned char)(~block_length & 0xFF);
    block[4] = (unsigned char)((~block_length >> 8) & 0xFF);
    memcpy(block + 5, raw + offset, length);
    ok = png_chunk(file, "IDAT", block, length + 5);
  }

  if (ok) {
    unsigned int adler = adler32_bytes(raw, raw_size);
    block[0] = 0x01;
    block[1] = 0x00;
    block[2] = 0x00;
    block[3] = 0xFF;
    block[4] = 0xFF;
    put_be32(block + 5, adler);
    ok = png_chunk(file, "IDAT", block, 9) &&
      png_chunk(file, "IEND", NULL, 0);
  }
  return ok;
}

static void shot_write(const ShotJob *job) {
  unsigned long long started_us =
    (unsigned long long)sceKernelGetSystemTimeWide();
  int ps1 = ra_runtime_title_is_pops() && pops_data != NULL;
  const uint32_t *source = ps1 ? (const uint32_t*)pops_data
                : (const uint32_t*)SCE_PSPEMU_FRAMEBUFFER;
  const size_t pitch = (size_t)(ps1 ? SCREEN_LINE : PSP_SCREEN_LINE);
  const int width = ps1 ? SCREEN_WIDTH : PSP_SCREEN_WIDTH;
  const int height = ps1 ? SCREEN_HEIGHT : PSP_SCREEN_HEIGHT;
  const size_t raw_size = (size_t)height * ((size_t)width * 4 + 1);
  unsigned char *raw;
  unsigned char *block;
  char path[192];
  int file;
  int ok;

  raw = (unsigned char*)malloc(raw_size);
  block = (unsigned char*)malloc(STORED_BLOCK + 5);
  if (raw == NULL || block == NULL) {
    free(raw);
    free(block);
    shot_log("shot: no memory for frame\n");
    return;
  }

  shot_fill_raw(source, pitch, width, height, raw);

  file = shot_open_output(job, path, sizeof(path));
  if (file < 0) {
    free(raw);
    free(block);
    shot_log("shot: open failed for %s\n", path);
    return;
  }

  ok = shot_emit_png(file, raw, raw_size, width, height, block);

  sceIoClose(file);
  free(raw);
  free(block);
  if (ok)
    shot_log("shot: %s (%u ms)\n", path,
        (unsigned)(((unsigned long long)sceKernelGetSystemTimeWide() -
              started_us) / 1000));
  else
    shot_log("shot: write failed for %s (%u ms)\n", path,
        (unsigned)(((unsigned long long)sceKernelGetSystemTimeWide() -
              started_us) / 1000));
}

static int shot_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    ShotJob job;

    sceKernelWaitSema(g_wake, 1, NULL);
    for (;;) {
      shot_lock();
      if (g_job_count == 0) {
        shot_unlock();
        break;
      }
      job = g_jobs[0];
      --g_job_count;
      if (g_job_count > 0)
        memmove(&g_jobs[0], &g_jobs[1], sizeof(g_jobs[0]) * (size_t)g_job_count);
      shot_unlock();
      shot_write(&job);
      if (g_burst_source[0] != '\0')
        g_burst_ready = 1;
    }
  }
  return 0;
}

static int copy_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    ShotJob job;
    char source[192];

    sceKernelDelayThread(500 * 1000);
    if (!ra_runtime_holds_world())
      continue;
    shot_lock();
    if (!g_burst_ready || g_copy_count == 0) {
      shot_unlock();
      continue;
    }
    job = g_copies[0];
    --g_copy_count;
    if (g_copy_count > 0)
      memmove(&g_copies[0], &g_copies[1], sizeof(g_copies[0]) * (size_t)g_copy_count);
    snprintf(source, sizeof(source), "%s", g_burst_source);
    shot_unlock();
    if (copy_file(source, &job))
      shot_log("shot: burst copy '%s' from first image\n", job.name);
    else
      shot_log("shot: burst copy failed for '%s'\n", job.name);
  }
  return 0;
}

void ra_shot_flush_copies(void) {
  ShotJob job;

  while (g_copy_count > 0) {
    shot_lock();
    if (g_copy_count == 0) {
      shot_unlock();
      break;
    }
    job = g_copies[0];
    --g_copy_count;
    if (g_copy_count > 0)
      memmove(&g_copies[0], &g_copies[1], sizeof(g_copies[0]) * (size_t)g_copy_count);
    shot_unlock();
    if (!g_burst_ready || !copy_file(g_burst_source, &job))
      shot_log("shot: burst copy failed for '%s' at exit\n", job.name);
    else
      shot_log("shot: burst copy '%s' at exit\n", job.name);
  }
}

void ra_shot_init(void) {
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_shot", 0, 1, 1, NULL);
  if (g_wake < 0)
    g_wake = sceKernelCreateSema("ra_shot_wake", 0, 1, 1, NULL);
  if (g_thread < 0 && g_wake >= 0) {
    g_thread = sceKernelCreateThread("ra_shot", shot_worker, SHOT_PRIORITY,
                      SHOT_STACK, 0, 0, NULL);
    if (g_thread >= 0)
      sceKernelStartThread(g_thread, 0, NULL);
  }
  if (g_copy_thread < 0) {
    g_copy_thread = sceKernelCreateThread("ra_shot_copy", copy_worker,
                       COPY_PRIORITY, COPY_STACK, 0, 0, NULL);
    if (g_copy_thread >= 0)
      sceKernelStartThread(g_copy_thread, 0, NULL);
  }
  if (!g_dirs_ready) {
    sceIoMkdir("ux0:data/AdrenalineRA", 0777);
    sceIoMkdir(SHOT_DIR, 0777);
    g_dirs_ready = 1;
  }
}

void ra_shot_request(const char *folder, const char *name) {
  unsigned long long now_us;
  char safe_folder[FOLDER_MAX];
  char safe_name[FOLDER_MAX];

  if (g_wake < 0)
    return;
  sanitize(folder, safe_folder, sizeof(safe_folder));
  sanitize_name(name, safe_name, sizeof(safe_name));
  now_us = (unsigned long long)sceKernelGetSystemTimeWide();
  shot_lock();
  if (g_burst_until != 0 && now_us < g_burst_until) {
    if (g_copy_count < COPY_QUEUE) {
      memset(&g_copies[g_copy_count], 0, sizeof(g_copies[0]));
      snprintf(g_copies[g_copy_count].folder,
          sizeof(g_copies[g_copy_count].folder), "%s", safe_folder);
      snprintf(g_copies[g_copy_count].name,
          sizeof(g_copies[g_copy_count].name), "%s", safe_name);
      ++g_copy_count;
      shot_log("shot: burst queue '%s' for a paused copy\n", safe_name);
    } else {
      shot_log("shot: burst queue full; '%s' has no image\n", safe_name);
    }
    shot_unlock();
    return;
  }

  g_burst_until = now_us + 1500000ULL;
  g_burst_ready = 0;
  job_path(g_burst_source, sizeof(g_burst_source), safe_folder, safe_name);
  g_job_count = 1;
  memset(&g_jobs[0], 0, sizeof(g_jobs[0]));
  snprintf(g_jobs[0].folder, sizeof(g_jobs[0].folder), "%s", safe_folder);
  snprintf(g_jobs[0].name, sizeof(g_jobs[0].name), "%s", safe_name);
  shot_unlock();
  sceKernelSignalSema(g_wake, 1);
}

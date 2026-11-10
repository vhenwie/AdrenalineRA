#include "ra_sound.h"

#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_logqueue.h"
#include "ra_wav.h"

#define SOUND_QUEUE 8
#define SOUND_MAX_FILE (2 * 1024 * 1024)
#define SOUND_FRAMES 1024

static SceUID g_thread = -1;
static SceUID g_sema = -1;
static SceUID g_lock = -1;

static void sound_lock(void);
static void sound_unlock(void);
static int g_queue[SOUND_QUEUE];
static int g_head;
static int g_tail;
static volatile int g_enabled = 1;
static volatile int g_message = 1;
static volatile int g_unlock = 1;
static volatile int g_lb_submit = 1;
static int16_t *g_cached[3];
static size_t g_cached_count[3];

static void sound_log(const char *text) {
  ra_logq_line(text, (int)strlen(text));
}

void ra_sound_set_options(int enabled, int message, int unlock, int lb_submit) {
  g_enabled = enabled != 0;
  g_message = message;
  g_unlock = unlock;
  g_lb_submit = lb_submit;
}

static const char *sound_name(int kind) {
  switch (kind) {
  case RA_SOUND_UNLOCK:
    return "unlock";
  case RA_SOUND_LBSUBMIT:
    return "lbsubmit";
  default:
    return "message";
  }
}

static int sound_choice(int kind) {
  if (!g_enabled)
    return 0;
  switch (kind) {
  case RA_SOUND_UNLOCK:
    return g_unlock;
  case RA_SOUND_LBSUBMIT:
    return g_lb_submit;
  default:
    return g_message;
  }
}

static int read_file(const char *path, unsigned char **data, size_t *size) {
  SceUID fd;
  SceIoStat stat;
  unsigned char *buffer;
  int got;

  if (sceIoGetstat(path, &stat) < 0 || stat.st_size <= 0 ||
      stat.st_size > SOUND_MAX_FILE)
    return 0;
  fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;
  buffer = (unsigned char*)malloc((size_t)stat.st_size);
  if (buffer == NULL) {
    sceIoClose(fd);
    return 0;
  }
  got = (int)sceIoRead(fd, buffer, (SceSize)stat.st_size);
  sceIoClose(fd);
  if (got != (int)stat.st_size) {
    free(buffer);
    return 0;
  }
  *data = buffer;
  *size = (size_t)stat.st_size;
  return 1;
}

static int load_sound(const char *path, int16_t **samples, size_t *count) {
  unsigned char *data = NULL;
  size_t size = 0;
  RaWav wav;
  int result;

  if (!read_file(path, &data, &size))
    return 0;
  memset(&wav, 0, sizeof(wav));
  result = ra_wav_parse(data, size, &wav);
  if (result)
    result = ra_wav_take_pcm48k(&wav, samples, count);
  ra_wav_free(&wav);
  free(data);
  return result;
}

static int load_for_kind(int kind, int choice, int16_t **samples,
                         size_t *count) {
  char path[192];
  const char *name = sound_name(kind);

  if (choice == 2) {
    snprintf(path, sizeof(path), "ux0:data/AdrenalineRA/sounds/%s.wav", name);
    if (load_sound(path, samples, count)) {
      sound_log("sound: override loaded\n");
      return 1;
    }
    sound_log("sound: override missing; using built-in\n");
  }
  snprintf(path, sizeof(path), "ux0:app/PSPEMUCFW/ra_sounds/%s.wav", name);
  if (load_sound(path, samples, count))
    return 1;
  snprintf(path, sizeof(path), "app0:ra_sounds/%s.wav", name);
  return load_sound(path, samples, count);
}

static int open_audio_port(void) {
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                SOUND_FRAMES, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  if (port < 0)
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                SOUND_FRAMES, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  if (port < 0)
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE,
                SOUND_FRAMES, 48000, SCE_AUDIO_OUT_MODE_STEREO);
  return port;
}

static int sound_samples(int kind, int choice, int16_t **samples,
                         size_t *count, int *from_cache) {
  unsigned long long load_start;
  unsigned long long load_ms;

  if (choice == 1 && g_cached[kind] != NULL) {
    *samples = g_cached[kind];
    *count = g_cached_count[kind];
    *from_cache = 1;
    return 1;
  }
  load_start = sceKernelGetSystemTimeWide();
  if (!load_for_kind(kind, choice, samples, count)) {
    sound_log("sound: file could not be loaded\n");
    return 0;
  }
  load_ms = (sceKernelGetSystemTimeWide() - load_start) / 1000ULL;
  {
    char line[96];
    snprintf(line, sizeof(line), "sound: load %s took %llu ms (%u samples)\n",
             sound_name(kind), load_ms, (unsigned)*count);
    sound_log(line);
  }
  if (choice == 1) {
    char line[80];
    g_cached[kind] = *samples;
    g_cached_count[kind] = *count;
    *from_cache = 1;
    snprintf(line, sizeof(line), "sound: cached %s (%u samples)\n",
             sound_name(kind), (unsigned)*count);
    sound_log(line);
  }
  return 1;
}

static void sound_output(int port, const int16_t *samples, size_t count) {
  size_t offset = 0;

  while (offset < count) {
    int16_t block[SOUND_FRAMES * 2];
    size_t left = count - offset;
    size_t copy = left < sizeof(block) / sizeof(block[0])
      ? left : sizeof(block) / sizeof(block[0]);

    memset(block, 0, sizeof(block));
    memcpy(block, samples + offset, copy * sizeof(int16_t));
    sceAudioOutOutput(port, block);
    offset += copy;
  }
  sceAudioOutOutput(port, NULL);
}

static void play_one(int kind) {
  int choice = sound_choice(kind);
  int16_t *samples = NULL;
  size_t count = 0;
  int from_cache = 0;
  int port;

  if (choice == 0 || kind < RA_SOUND_MESSAGE || kind > RA_SOUND_LBSUBMIT)
    return;
  if (!sound_samples(kind, choice, &samples, &count, &from_cache))
    return;
  port = open_audio_port();
  if (port < 0) {
    sound_log("sound: no audio port available\n");
    if (!from_cache)
      free(samples);
    return;
  }
  sound_output(port, samples, count);
  sceAudioOutReleasePort(port);
  if (!from_cache)
    free(samples);
}

static int sound_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    int kind;
    sceKernelWaitSema(g_sema, 1, NULL);
    sound_lock();
    if (g_head == g_tail) {
      sound_unlock();
      continue;
    }
    kind = g_queue[g_head];
    g_head = (g_head + 1) % SOUND_QUEUE;
    sound_unlock();
    play_one(kind);
  }
  return 0;
}

static void sound_lock(void) {
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_sound_lock", 0, 1, 1, NULL);
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

static void sound_unlock(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

void ra_sound_init(void) {
  sound_lock();
  if (g_sema < 0)
    g_sema = sceKernelCreateSema("ra_sound", 0, 0, SOUND_QUEUE, NULL);
  if (g_thread < 0 && g_sema >= 0) {
    g_thread = sceKernelCreateThread("ra_sound", sound_thread,
                      0x10000100, 0x8000, 0, 0, NULL);
    if (g_thread >= 0)
      sceKernelStartThread(g_thread, 0, NULL);
  }
  sound_unlock();
}

void ra_sound_play(int kind) {
  int next;

  if (!g_enabled || sound_choice(kind) == 0)
    return;
  ra_sound_init();
  sound_lock();
  if (g_sema < 0) {
    sound_unlock();
    return;
  }
  next = (g_tail + 1) % SOUND_QUEUE;
  if (next == g_head) {
    sound_unlock();
    return;
  }
  g_queue[g_tail] = kind;
  g_tail = next;
  sound_unlock();
  sceKernelSignalSema(g_sema, 1);
}

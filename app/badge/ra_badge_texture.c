#include "ra_badge.h"
#include "ra_badge_texture.h"
#include "ra_badge_blob.h"
#include "ra_badge_png.h"
#include "ra_badge_state.h"
#include "ra_badge_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

/* Qualified: libvita2d ships its own utils.h and is searched first. */
#include "../../third_party/Adrenaline/user/utils.h"
#include "lodepng/lodepng.h"

#define RA_BADGE_TEXTURE_CACHE 12
#define RA_BADGE_PRIME_RING 12
#define RA_BADGE_MAX_BYTES (128 * 1024)
#define RA_BADGE_MAX_DIM 128

static char g_badge_wanted[RA_BADGE_KEY_MAX];
static unsigned int g_badge_wanted_frame;
static unsigned int g_badge_frame;
static char g_miss_logged[RA_BADGE_KEY_MAX];


typedef struct {
  char key[RA_BADGE_KEY_MAX];
  vita2d_texture *texture;
  unsigned int age;
  /* Marked when this texture has been chosen as an eviction victim. It is freed at
     * the next frame boundary, never inside the frame that is still referencing it
     * (see ra_badge_frame_begin). */
  int dead;
  unsigned int dead_frame;
} RaBadgeTexture;
#define RA_BADGE_PRIME_RING 12
#define RA_BADGE_MAX_BYTES (128 * 1024)
#define RA_BADGE_MAX_DIM 128

static char g_badge_wanted[RA_BADGE_KEY_MAX];
static unsigned int g_badge_wanted_frame;
static unsigned int g_badge_frame;
static char g_miss_logged[RA_BADGE_KEY_MAX];

static char g_prime_keys[RA_BADGE_PRIME_RING][RA_BADGE_KEY_MAX];
static int g_prime_head;
static int g_prime_count;
static RaBadgeTexture g_textures[RA_BADGE_TEXTURE_CACHE];
static unsigned int g_texture_age;

int ra_badge_texture_memory_cached(const char *key) {
  int i;
  int found = 0;

  ra_badge_lock();
  for (i = 0; i < RA_BADGE_TEXTURE_CACHE; ++i) {
    if (g_textures[i].texture != NULL && !g_textures[i].dead &&
      strcmp(g_textures[i].key, key) == 0) {
      found = 1;
      break;
    }
  }
  ra_badge_unlock();
  if (!found && ra_badge_blob_has(key))
    found = 1;
  return found;
}

void ra_badge_prime_queue(const char *key) {
  int i;

  if (!ra_badge_key_valid(key))
    return;
  ra_badge_lock();
  for (i = 0; i < g_prime_count; ++i) {
    int index = (g_prime_head + i) % RA_BADGE_PRIME_RING;

    if (strcmp(g_prime_keys[index], key) == 0) {
      ra_badge_unlock();
      return;
    }
  }
  if (g_prime_count >= RA_BADGE_PRIME_RING) {
    g_prime_head = (g_prime_head + 1) % RA_BADGE_PRIME_RING;
    --g_prime_count;
  }
  snprintf(g_prime_keys[(g_prime_head + g_prime_count) % RA_BADGE_PRIME_RING],
      RA_BADGE_KEY_MAX, "%s", key);
  ++g_prime_count;
  ra_badge_unlock();
}

static void badge_texture_miss(const char *key, const char *reason) {
  if (strcmp(g_miss_logged, key) == 0)
    return;
  snprintf(g_miss_logged, sizeof(g_miss_logged), "%s", key);
  ra_badge_log("badge: texture miss %s (%s)\n", key, reason);
}

/* The draw thread either served `key` or gave up on it: the background prime step may
 * take slots again. */
static void badge_wanted_done(const char *key) {
  if (g_badge_wanted[0] != '\0' && strcmp(g_badge_wanted, key) == 0)
    g_badge_wanted[0] = '\0';
}


void ra_badge_frame_begin(void) {
  int i;
  int freed = 0;

  ++g_badge_frame;
  ra_badge_lock();
  for (i = 0; i < RA_BADGE_TEXTURE_CACHE; ++i) {
    if (g_textures[i].dead && g_textures[i].texture != NULL) {
      vita2d_free_texture(g_textures[i].texture);
      memset(&g_textures[i], 0, sizeof(g_textures[i]));
      ++freed;
    }
  }
  ra_badge_unlock();
  if (freed > 0)
    ra_badge_log("badge: freed %d texture(s) at frame boundary\n", freed);
}

typedef struct {
  char path[96];
  unsigned char *data;
  unsigned char *image;
  unsigned int width;
  unsigned int height;
  int read_length;
  int from_blob;
} RaBadgeLoaded;

/* Cache hit: bump the age so the entry is not the next eviction victim, and tell
 * the prime step the key it was waiting for has been served. */
static vita2d_texture *badge_texture_cached(const char *key) {
  int i;

  for (i = 0; i < RA_BADGE_TEXTURE_CACHE; ++i) {
    if (g_textures[i].texture != NULL && !g_textures[i].dead &&
      strcmp(g_textures[i].key, key) == 0) {
      g_textures[i].age = ++g_texture_age;
      badge_wanted_done(key);
      return g_textures[i].texture;
    }
  }
  return NULL;
}

/* Reads the PNG bytes: the worker's held blob first, the on-disk cache second. */
static int badge_texture_load_file(const char *key, RaBadgeLoaded *load);

static int badge_texture_load(const char *key, RaBadgeLoaded *load) {
  size_t blob_size = 0;
  int read_length;

  memset(load, 0, sizeof(*load));
  if (!ra_badge_blob_take(key, &load->data, &blob_size))
    return badge_texture_load_file(key, load);
  read_length = (int)blob_size;
  load->from_blob = 1;
  load->read_length = read_length;
  if (!ra_badge_png_header_ok(load->data, (size_t)read_length,
      &load->width, &load->height)) {
    free(load->data);
    badge_texture_miss(key, "held PNG invalid");
    badge_wanted_done(key);
    return 0;
  }
  return 1;
}

/* On-disk half of badge_texture_load: path, stat, size and read checks. Every
 * failure reports itself here (miss + badge_wanted_done) exactly like the inline
 * path did; "open failed" is the one path that never called badge_wanted_done,
 * and still does not. */
static int badge_texture_load_file(const char *key, RaBadgeLoaded *load) {
  SceIoStat stat;
  SceUID file;
  int read_length;

  if (!ra_badge_path(key, load->path, sizeof(load->path)) ||
    ra_badge_missing_recently(key)) {
    badge_texture_miss(key, ra_badge_missing_recently(key) ? "negative cache"
                            : "no cache path");
    badge_wanted_done(key);
    return 0;
  }
  if (sceIoGetstat(load->path, &stat) < 0 || stat.st_size <= 0) {
    ra_badge_note_missing(key);
    badge_texture_miss(key, "no PNG on card");
    badge_wanted_done(key);
    return 0;
  }
  if (stat.st_size > RA_BADGE_MAX_BYTES) {
    sceIoRemove(load->path);
    badge_texture_miss(key, "file over 128 KB, removed");
    badge_wanted_done(key);
    return 0;
  }
  load->data = (unsigned char*)malloc((size_t)stat.st_size);
  if (load->data == NULL) {
    badge_texture_miss(key, "no memory to read the PNG");
    badge_wanted_done(key);
    return 0;
  }
  file = sceIoOpen(load->path, SCE_O_RDONLY, 0);
  if (file < 0) {
    free(load->data);
    badge_texture_miss(key, "open failed");
    return 0;
  }
  read_length = sceIoRead(file, load->data, (unsigned int)stat.st_size);
  sceIoClose(file);
  load->read_length = read_length;
  if (read_length <= 0 ||
      (!load->from_blob && read_length != (int)stat.st_size)) {
    free(load->data);
    if (!load->from_blob)
      sceIoRemove(load->path);
    badge_texture_miss(key, "short read, removed");
    badge_wanted_done(key);
    return 0;
  }
  if (!ra_badge_png_header_ok(load->data, (size_t)read_length,
        &load->width, &load->height)) {
    free(load->data);
    if (!load->from_blob)
      sceIoRemove(load->path);
    badge_texture_miss(key, load->from_blob ? "held PNG invalid"
                              : "not a PNG, removed");
    badge_wanted_done(key);
    return 0;
  }
  return 1;
}

/* Returns the slot to fill, or -1 when this frame may not evict: the victim is
 * marked dead and freed at the next frame boundary instead. */
static int badge_texture_pick_slot(const char *key) {
  int i;
  int slot = -1;
  unsigned int oldest_age = 0;

  for (i = 0; i < RA_BADGE_TEXTURE_CACHE; ++i) {
    if (g_textures[i].texture == NULL) {
      slot = i;
      break;
    }
    if (slot < 0 || g_textures[i].age < oldest_age) {
      oldest_age = g_textures[i].age;
      slot = i;
    }
  }
  if (slot < 0)
    slot = 0;
  if (g_textures[slot].texture != NULL) {
    if (!g_textures[slot].dead) {
      g_textures[slot].dead = 1;
      g_textures[slot].dead_frame = g_badge_frame;
      snprintf(g_badge_wanted, sizeof(g_badge_wanted), "%s", key);
      g_badge_wanted_frame = g_badge_frame;
      ra_badge_log("badge: eviction of %s deferred to the frame boundary (for %s)\n",
          g_textures[slot].key, key);
      badge_texture_miss(key, "cache full this frame");
      return -1;
    }
    /* Only free once the condemning frame is over. */
    if (g_textures[slot].dead_frame == g_badge_frame)
      return -1;
    vita2d_free_texture(g_textures[slot].texture);
    memset(&g_textures[slot], 0, sizeof(g_textures[slot]));
  }
  return slot;
}

/* Creates the texture and copies the decoded pixels into it. NULL on allocation
 * failure (already reported); the caller owns `image` either way. */
static vita2d_texture *badge_texture_upload(const char *key,
    unsigned char *image, unsigned int width, unsigned int height) {
  vita2d_texture *texture;
  unsigned char *destination;
  unsigned int stride;
  unsigned int row;

  vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
  texture = vita2d_create_empty_texture(width, height);
  if (texture == NULL) {
    badge_texture_miss(key, "texture allocation failed");
    return NULL;
  }
  destination = (unsigned char*)vita2d_texture_get_datap(texture);
  stride = (unsigned int)vita2d_texture_get_stride(texture);
  for (row = 0; row < height; ++row) {
    memcpy(destination + (size_t)row * stride,
      image + (size_t)row * width * 4,
      (size_t)width * 4);
  }
  return texture;
}

static void badge_texture_store(const char *key, int slot,
    vita2d_texture *texture, unsigned long long texture_started) {
  unsigned int ms;

  memset(&g_textures[slot], 0, sizeof(g_textures[slot]));
  snprintf(g_textures[slot].key, sizeof(g_textures[slot].key), "%s", key);
  g_textures[slot].texture = texture;
  g_textures[slot].age = ++g_texture_age;
  if (strcmp(g_miss_logged, key) == 0)
    g_miss_logged[0] = '\0';
  ms = (unsigned int)(((unsigned long long)
      sceKernelGetSystemTimeWide() - texture_started) / 1000ULL);
  if (ms >= 30)
    ra_badge_log("badge: texture %s built in %u ms\n", key, ms);
  badge_wanted_done(key);
}

/* Decodes the loaded PNG; on failure, free the bytes and report the miss. */
static int badge_texture_decode(const char *key, RaBadgeLoaded *load) {
  if (lodepng_decode32(&load->image, &load->width, &load->height, load->data,
            (size_t)load->read_length) != 0 ||
    load->image == NULL || load->width == 0 || load->height == 0 ||
    load->width > RA_BADGE_MAX_DIM || load->height > RA_BADGE_MAX_DIM) {
    ra_badge_log("badge: decode failed for %s (%u bytes)\n", key,
        (unsigned)load->read_length);
    free(load->data);
    if (load->image != NULL)
      adr_free(load->image);
    if (!load->from_blob)
      sceIoRemove(load->path);
    badge_wanted_done(key);
    return 0;
  }
  free(load->data);
  return 1;
}

vita2d_texture *ra_badge_texture(const char *key) {
  RaBadgeLoaded load;
  vita2d_texture *texture;
  int slot;
  unsigned long long texture_started =
    (unsigned long long)sceKernelGetSystemTimeWide();

  /* Stop only blocks new downloads. A card that is already on screen keeps
     * its cached image while the session tears down. */
  if (!ra_badge_enabled() || !ra_badge_key_valid(key)) {
    badge_texture_miss(key != NULL ? key : "(null)", "disabled or invalid key");
    return NULL;
  }

  /* Cache entries persist until a failed validation removes them. */

  texture = badge_texture_cached(key);
  if (texture != NULL)
    return texture;

  if (!badge_texture_load(key, &load))
    return NULL;
  if (!badge_texture_decode(key, &load))
    return NULL;

  slot = badge_texture_pick_slot(key);
  if (slot < 0) {
    adr_free(load.image);
    return NULL;
  }

  texture = badge_texture_upload(key, load.image, load.width, load.height);
  adr_free(load.image);
  if (texture == NULL) {
    badge_wanted_done(key);
    return NULL;
  }

  badge_texture_store(key, slot, texture, texture_started);
  return texture;
}


void ra_badge_prime_step(void) {
  char key[RA_BADGE_KEY_MAX];

  ra_badge_lock();
  /* Yield while a draw-thread texture is waiting for a slot . This runs
     * before the card's lookup in the same frame, so taking the slot that
     * `ra_badge_frame_begin()` just freed is what starved the session banner. The age
     * window keeps a dismissed card from blocking the warm pass for more than two
     * frames. */
  if (g_badge_wanted[0] != '\0' &&
    g_badge_frame - g_badge_wanted_frame <= 2u) {
    ra_badge_unlock();
    return;
  }
  if (g_prime_count <= 0) {
    ra_badge_unlock();
    return;
  }
  snprintf(key, sizeof(key), "%s", g_prime_keys[g_prime_head]);
  g_prime_head = (g_prime_head + 1) % RA_BADGE_PRIME_RING;
  --g_prime_count;
  ra_badge_unlock();

  /* Upload only: the PNG is already on the card, so this is one read, one decode and
     * one texture - done now, while nothing is waiting for it, instead of inside the
     * frame that first draws the unlock card. */
  if (ra_badge_texture(key) == NULL)
    ra_badge_prime_queue(key);
}


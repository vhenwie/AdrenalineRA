#include "ra_arena.h"

#include <string.h>

#if defined(__vita__) || defined(RA_ARENA_WRAP)
#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif
extern void *__real_malloc(size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);
#define SYS_MALLOC __real_malloc
#define SYS_REALLOC __real_realloc
#define SYS_FREE __real_free
#else
#include <stdlib.h>
#define SYS_MALLOC malloc
#define SYS_REALLOC realloc
#define SYS_FREE free
#endif

#define ALIGN 16u
#define MAGIC 0x5241726Eu

typedef struct block {
  size_t size;
  unsigned int magic;
  unsigned int free;
  struct block *prev;
  struct block *next;
} __attribute__((aligned(16))) block_t;

static unsigned char *g_base;
static size_t g_size;
static block_t *g_first;
static block_t *g_rover;
static int g_ready;
static size_t g_used_bytes;
static size_t g_peak_bytes;
static unsigned long g_allocations;
static unsigned long g_scans;
static unsigned long g_rover_repairs;

#ifdef __vita__
static SceUID g_mutex = -1;

static void arena_lock(void) {
  if (g_mutex < 0)
    g_mutex = sceKernelCreateMutex("ra_arena", 0, 0, NULL);
  if (g_mutex >= 0)
    sceKernelLockMutex(g_mutex, 1, NULL);
}

static void arena_unlock(void) {
  if (g_mutex >= 0)
    sceKernelUnlockMutex(g_mutex, 1);
}
#else
static void arena_lock(void) {}
static void arena_unlock(void) {}
#endif

static size_t align_up(size_t value) {
  return (value + (ALIGN - 1u)) & ~(size_t)(ALIGN - 1u);
}

int ra_arena_ready(void) { return g_ready; }
size_t ra_arena_capacity(void) { return g_ready ? g_size : 0; }
unsigned long ra_arena_stat_allocations(void) { return g_allocations; }
unsigned long ra_arena_stat_scans(void) { return g_scans; }
unsigned long ra_arena_stat_rover_repairs(void) { return g_rover_repairs; }
size_t ra_arena_peak(void) { return g_peak_bytes; }

size_t ra_arena_system_probe_kib(void) {
  static const size_t sizes[] = { 4096, 2048, 1024, 512, 256 };
  size_t i;

  for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    void *block = SYS_MALLOC(sizes[i] * 1024u);
    if (block != NULL) {
      SYS_FREE(block);
      return sizes[i];
    }
  }
  return 0;
}

int ra_arena_init(void *base, size_t size) {
  block_t *block;

  if (g_ready || base == NULL)
    return 0;
  if (size < sizeof(block_t) + 2u * ALIGN)
    return 0;
  g_base = (unsigned char*)base;
  g_size = size;
  block = (block_t*)g_base;
  block->size = size - sizeof(block_t);
  block->magic = MAGIC;
  block->free = 1;
  block->prev = NULL;
  block->next = NULL;
  g_first = block;
  g_rover = block;
  g_ready = 1;
  return 1;
}

int ra_arena_owns(const void *ptr) {
  const unsigned char *p = (const unsigned char*)ptr;

  if (!g_ready || p == NULL)
    return 0;
  return p >= g_base + sizeof(block_t) && p < g_base + g_size;
}

size_t ra_arena_used(void) {
  size_t used = 0;
  block_t *block;

  if (!g_ready)
    return 0;
  for (block = g_first; block != NULL; block = block->next) {
    if (!block->free)
      used += block->size;
  }
  return used;
}

size_t ra_arena_largest_free(void) {
  size_t largest = 0;
  block_t *block;

  if (!g_ready)
    return 0;
  for (block = g_first; block != NULL; block = block->next) {
    if (block->free && block->size > largest)
      largest = block->size;
  }
  return largest;
}

static int block_valid(const block_t *block) {
  return block != NULL && block->magic == MAGIC;
}

static void *arena_alloc(size_t size) {
  block_t *block;
  block_t *start;
  size_t need = align_up(size);

  if (!g_ready || need == 0)
    return NULL;
  start = (g_rover != NULL && block_valid(g_rover)) ? g_rover : g_first;
  block = start;
  for (;;) {
    block_t *rest;

    ++g_scans;
    if (!block_valid(block)) {
      ++g_rover_repairs;
      g_rover = g_first;
      return NULL;
    }
    if (block->free && block->size >= need) {
      if (block->size >= need + sizeof(block_t) + ALIGN) {
        rest = (block_t*)((unsigned char*)(block + 1) + need);
        rest->size = block->size - need - sizeof(block_t);
        rest->magic = MAGIC;
        rest->free = 1;
        rest->prev = block;
        rest->next = block->next;
        if (rest->next != NULL)
          rest->next->prev = rest;
        block->next = rest;
        block->size = need;
      }
      block->free = 0;
      ++g_allocations;
      g_used_bytes += block->size;
      if (g_used_bytes > g_peak_bytes)
        g_peak_bytes = g_used_bytes;
      g_rover = (block->next != NULL) ? block->next : g_first;
      return (void*)(block + 1);
    }
    block = block->next;
    if (block == NULL) {
      if (start == g_first)
        break;
      block = g_first;
      if (block == start)
        break;
    }
  }
  return NULL;
}

static void arena_free_block(block_t *block) {
  if (block->free)
    return;
  block->free = 1;
  g_used_bytes -= block->size;

  if (block->next != NULL && block->next->free) {
    block_t *next = block->next;
    block->size += sizeof(block_t) + next->size;
    block->next = next->next;
    if (block->next != NULL)
      block->next->prev = block;
    next->magic = 0;
    if (g_rover == next)
      g_rover = block;
  }
  if (block->prev != NULL && block->prev->free) {
    block_t *prev = block->prev;
    prev->size += sizeof(block_t) + block->size;
    prev->next = block->next;
    if (prev->next != NULL)
      prev->next->prev = prev;
    block->magic = 0;
    if (g_rover == block)
      g_rover = prev;
  }
}

static int arena_create(void) {
#ifdef __vita__
  SceUID uid;
  void *base = NULL;

  uid = sceKernelAllocMemBlock("ra_arena", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                RA_ARENA_SIZE, NULL);
  if (uid < 0)
    return 0;
  if (sceKernelGetMemBlockBase(uid, &base) < 0) {
    sceKernelFreeMemBlock(uid);
    return 0;
  }
  return ra_arena_init(base, RA_ARENA_SIZE);
#else
  return 0;
#endif
}

void *ra_arena_malloc(size_t size) {
  void *ptr = NULL;

  if (size < RA_ARENA_THRESHOLD)
    return SYS_MALLOC(size);
  arena_lock();
  if (!g_ready)
    arena_create();
  if (g_ready)
    ptr = arena_alloc(size);
  arena_unlock();
  return ptr != NULL ? ptr : SYS_MALLOC(size);
}

void *ra_arena_calloc(size_t count, size_t size) {
  size_t total;
  void *ptr;

  if (count != 0 && size > (size_t)-1 / count)
    return NULL;
  total = count * size;
  ptr = ra_arena_malloc(total);
  if (ptr != NULL)
    memset(ptr, 0, total);
  return ptr;
}

void *ra_arena_realloc(void *ptr, size_t size) {
  block_t *block;
  size_t old;
  void *next = NULL;

  if (ptr == NULL)
    return ra_arena_malloc(size);
  if (!ra_arena_owns(ptr))
    return SYS_REALLOC(ptr, size);
  block = ((block_t*)ptr) - 1;
  if (!block_valid(block))
    return NULL;

  arena_lock();
  old = block->size;
  if (size <= old) {
    arena_unlock();
    return ptr;
  }
  next = arena_alloc(size);
  if (next != NULL) {
    memcpy(next, ptr, old);
    arena_free_block(block);
  }
  arena_unlock();

  if (next == NULL) {
    next = SYS_MALLOC(size);
    if (next != NULL) {
      memcpy(next, ptr, old);
      ra_arena_free(ptr);
    }
  }
  return next;
}

void ra_arena_free(void *ptr) {
  block_t *block;

  if (ptr == NULL)
    return;
  if (!ra_arena_owns(ptr)) {
    SYS_FREE(ptr);
    return;
  }
  block = ((block_t*)ptr) - 1;
  arena_lock();
  if (block_valid(block))
    arena_free_block(block);
  arena_unlock();
}

#if defined(__vita__) || defined(RA_ARENA_WRAP)
void *__wrap_malloc(size_t size) { return ra_arena_malloc(size); }
void *__wrap_calloc(size_t count, size_t size) { return ra_arena_calloc(count, size); }
void *__wrap_realloc(void *ptr, size_t size) { return ra_arena_realloc(ptr, size); }
void __wrap_free(void *ptr) { ra_arena_free(ptr); }
#endif

#ifndef RA_HASH_IO_H
#define RA_HASH_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#else
#include <stdio.h>
#endif

typedef struct {
#ifdef __vita__
  SceUID fd;
#else
  FILE *fp;
#endif
} ra_file_t;

ra_file_t *ra_open(const char *path);
void ra_close(ra_file_t *file);
int ra_seek(ra_file_t *file, int64_t offset, int origin);
int64_t ra_tell(ra_file_t *file);
size_t ra_read(ra_file_t *file, void *buffer, size_t size);
int ra_write_file(const char *path, const void *buffer, size_t size);
int ra_file_info(const char *path, uint64_t *size, uint64_t *mtime);
int ra_cache_lookup(const char *cache_path, const char *path, uint64_t size,
                    uint64_t mtime, char *hash);
void ra_cache_store(const char *cache_path, const char *path, uint64_t size,
                    uint64_t mtime, const char *hash);
int ra_read_exact(ra_file_t *file, void *buffer, size_t size);
void *ra_hash_file_open(const char *path);
void ra_hash_file_seek(void *handle, int64_t offset, int origin);
int64_t ra_hash_file_tell(void *handle);
size_t ra_hash_file_read(void *handle, void *buffer, size_t size);
void ra_hash_file_close(void *handle);

#endif

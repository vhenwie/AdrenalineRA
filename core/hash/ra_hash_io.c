#include "ra_hash_io.h"

#include <stdlib.h>
#include <stdio.h>
#include <psp2/io/stat.h>
#include <string.h>

#define RA_HASH_MAX_CACHE_LINE 1024

ra_file_t *ra_open(const char *path) {
  ra_file_t *file;
  if (path == NULL || path[0] == '\0')
    return NULL;

  file = (ra_file_t*)calloc(1, sizeof(*file));
  if (file == NULL)
    return NULL;

#ifdef __vita__
  file->fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
  if (file->fd < 0) {
    free(file);
    return NULL;
  }
#else
  file->fp = fopen(path, "rb");
  if (file->fp == NULL) {
    free(file);
    return NULL;
  }
#endif
  return file;
}

void ra_close(ra_file_t *file) {
  if (file == NULL)
    return;
#ifdef __vita__
  if (file->fd >= 0)
    sceIoClose(file->fd);
#else
  if (file->fp != NULL)
    fclose(file->fp);
#endif
  free(file);
}

int ra_seek(ra_file_t *file, int64_t offset, int origin) {
  if (file == NULL)
    return -1;
#ifdef __vita__
  return sceIoLseek(file->fd, (SceOff)offset, origin) < 0 ? -1 : 0;
#elif defined(_WIN32)
  return _fseeki64(file->fp, offset, origin) == 0 ? 0 : -1;
#else
  return fseeko(file->fp, (off_t)offset, origin) == 0 ? 0 : -1;
#endif
}

int64_t ra_tell(ra_file_t *file) {
  if (file == NULL)
    return -1;
#ifdef __vita__
  return (int64_t)sceIoLseek(file->fd, 0, SCE_SEEK_CUR);
#elif defined(_WIN32)
  return _ftelli64(file->fp);
#else
  return (int64_t)ftello(file->fp);
#endif
}

size_t ra_read(ra_file_t *file, void *buffer, size_t size) {
  if (file == NULL || buffer == NULL || size == 0)
    return 0;
#ifdef __vita__
  {
    int got = sceIoRead(file->fd, buffer, (SceSize)size);
    return got > 0 ? (size_t)got : 0;
  }
#else
  return fread(buffer, 1, size, file->fp);
#endif
}

int ra_write_file(const char *path, const void *buffer, size_t size) {
#ifdef __vita__
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  size_t written;
  if (fd < 0)
    return -1;
  written = (size_t)sceIoWrite(fd, buffer, (SceSize)size);
  sceIoClose(fd);
#else
  FILE *fp = fopen(path, "ab");
  size_t written;
  if (fp == NULL)
    return -1;
  written = fwrite(buffer, 1, size, fp);
  fclose(fp);
#endif
  return written == size ? 0 : -1;
}

int ra_file_info(const char *path, uint64_t *size, uint64_t *mtime) {
#ifdef __vita__
  SceIoStat stat;
  if (sceIoGetstat(path, &stat) < 0)
    return -1;
  *size = (uint64_t)stat.st_size;
  *mtime = (((((uint64_t)stat.st_mtime.year * 13u +
      (uint64_t)stat.st_mtime.month) * 32u +
      (uint64_t)stat.st_mtime.day) * 24u +
      (uint64_t)stat.st_mtime.hour) * 60u +
    (uint64_t)stat.st_mtime.minute) * 60u +
    (uint64_t)stat.st_mtime.second;
  return 0;
#else
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
    *size = (uint64_t)st.st_size;
    *mtime = (uint64_t)st.st_mtime;
  return 0;
#endif
}

int ra_cache_lookup(const char *cache_path, const char *path,
            uint64_t size, uint64_t mtime, char hash[33]) {
  ra_file_t *file;
  char line[RA_HASH_MAX_CACHE_LINE];
  size_t used = 0;
  int ch;

  if (cache_path == NULL || cache_path[0] == '\0')
    return 0;
  file = ra_open(cache_path);
  if (file == NULL)
    return 0;

  for (;;) {
    int got = 0;
#ifdef __vita__
    got = sceIoRead(file->fd, &ch, 1) == 1;
#else
    ch = fgetc(file->fp);
    got = ch != EOF;
#endif
    if (!got || ch == '\n') {
      char *p1;
      char *p2;
      char *p3;
      unsigned long long cached_size;
      unsigned long long cached_mtime;
      if (used > 0) {
        line[used] = '\0';
        p1 = strchr(line, '|');
        p2 = p1 ? strchr(p1 + 1, '|') : NULL;
        p3 = p2 ? strchr(p2 + 1, '|') : NULL;
        if (p1 && p2 && p3) {
                    *p1 = '\0';
                    *p2 = '\0';
                    *p3 = '\0';
          cached_size = strtoull(line, NULL, 10);
          cached_mtime = strtoull(p1 + 1, NULL, 10);
          if (cached_size == size && cached_mtime == mtime &&
            strcmp(p3 + 1, path) == 0) {
            strncpy(hash, p2 + 1, 32);
            hash[32] = '\0';
            ra_close(file);
            return 1;
          }
        }
      }
      used = 0;
      if (!got)
        break;
      continue;
    }
    if (used + 1 < sizeof(line))
      line[used++] = (char)ch;
  }
  ra_close(file);
  return 0;
}

void ra_cache_store(const char *cache_path, const char *path,
            uint64_t size, uint64_t mtime, const char *hash) {
  char line[RA_HASH_MAX_CACHE_LINE];
  int length;

  if (cache_path == NULL || cache_path[0] == '\0')
    return;
  length = snprintf(line, sizeof(line), "%llu|%llu|%s|%s\n",
          (unsigned long long)size,
          (unsigned long long)mtime, hash, path);
  if (length > 0 && (size_t)length < sizeof(line))
    ra_write_file(cache_path, line, (size_t)length);
}

void *ra_hash_file_open(const char *path) { return ra_open(path); }
void ra_hash_file_seek(void *handle, int64_t offset, int origin) {
  (void)ra_seek((ra_file_t*)handle, offset, origin);
}
int64_t ra_hash_file_tell(void *handle) { return ra_tell((ra_file_t*)handle); }
size_t ra_hash_file_read(void *handle, void *buffer, size_t size) {
  return ra_read((ra_file_t*)handle, buffer, size);
}
void ra_hash_file_close(void *handle) { ra_close((ra_file_t*)handle); }

int ra_read_exact(ra_file_t *file, void *buffer, size_t size) {
  uint8_t *out = (uint8_t*)buffer;
  size_t done = 0;

  /* `sceIoRead` may return short; keep reading until the requested size is filled. */
  while (done < size) {
    size_t got = ra_read(file, out + done, size - done);
    if (got == 0)
      return -1;
    done += got;
  }
  return 0;
}



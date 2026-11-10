#ifndef RA_RUNTIME_PATH_H
#define RA_RUNTIME_PATH_H

#include <stddef.h>

int ra_runtime_translate_psp_path(const char *input, char *output, size_t capacity);
int ra_runtime_file_exists(const char *path);
int ra_runtime_normalize_titleid(const char raw[11], char normalized[11]);
int ra_runtime_scan_for_title(const char *directory, const char *titleid,
                              char *output, size_t capacity, int depth);
int ra_runtime_resolve_game_path(const char *filename, const char *titleid,
                                 char *output, size_t capacity);

#endif

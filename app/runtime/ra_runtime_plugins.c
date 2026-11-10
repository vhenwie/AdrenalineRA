#include "ra_runtime_plugins.h"
#include "ra_runtime_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/dirent.h>

/* Qualified because libvita2d ships its own utils.h and its include directory is
 * searched before the Adrenaline user directory. */
#include "../../third_party/Adrenaline/user/utils.h"

int ra_runtime_scan_cheat_plugins(void) {
  static const char *known[] = {
    "cwcheat", "tempar", "cheatmaster", "nite", "freecheat", "codecheat"
  };
  char directory[192];
  const char *root = getPspemuMemoryStickLocation();
  SceUID dfd;
  SceIoDirent entry;
  int found = 0;

  if (root == NULL)
    return 0;
  snprintf(directory, sizeof(directory), "%s/seplugins", root);
  dfd = sceIoDopen(directory);
  if (dfd < 0)
    return 0;

  for (;;) {
    char lower[128];
    size_t i;
    int match = 0;

    memset(&entry, 0, sizeof(entry));
    if (sceIoDread(dfd, &entry) <= 0)
      break;
    if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0)
      continue;
    for (i = 0; i < sizeof(lower) - 1 && entry.d_name[i] != '\0'; ++i)
      lower[i] = (char)tolower((unsigned char)entry.d_name[i]);
    lower[i] = '\0';
    for (i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
      if (strstr(lower, known[i]) != NULL) {
        match = 1;
        break;
      }
    }
    if (match) {
      ra_runtime_log("runtime: cheat plugin detected '%s'\n", entry.d_name);
      ++found;
    }
  }
  sceIoDclose(dfd);

  if (found > 0)
    ra_runtime_log("runtime: plugin scan found %d cheat plugin(s)\n", found);
  return found;
}

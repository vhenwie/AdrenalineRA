#include "ra_menu_status.h"
#include "ra_menu_account.h"
#include "ra_menu_internal.h"
#include "ra_menu_worker.h"

#include <stdarg.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vita2d.h>

#include "menu.h"
#include "ra_ime.h"
/* Qualified: libvita2d ships its own utils.h and is searched first. */
#include "../../third_party/Adrenaline/user/utils.h"

static void status_text(char *text, size_t capacity, uint32_t *color,
                        int *running) {
  RaJobSnapshot snapshot;

  ra_worker_snapshot(&snapshot);
  text[0] = '\0';
  *color = LITEGRAY;
  *running = 0;
  if (snapshot.state == RA_JOB_RUNNING) {
    ra_menu_copy_text(text, capacity, snapshot.stage);
    *color = AZURE;
    *running = 1;
  } else if (snapshot.state == RA_JOB_DONE_OK) {
    ra_menu_copy_text(text, capacity, snapshot.message);
    *color = GREEN;
  } else if (snapshot.state == RA_JOB_DONE_ERROR) {
    ra_menu_copy_text(text, capacity, snapshot.message);
    *color = RED;
  } else if (ra_menu_message_text()[0] != '\0') {
    ra_menu_copy_text(text, capacity, ra_menu_message_text());
    *color = ra_menu_message_color() ? ra_menu_message_color() : WHITE;
  } else if (ra_menu_account()->status[0] != '\0') {
    ra_menu_copy_text(text, capacity, ra_menu_account()->status);
    *color = LITEGRAY;
  }
}

static void status_spinner(float y) {
  static const float dx[10] = {
    0.0f, 5.9f, 9.5f, 9.5f, 5.9f, 0.0f, -5.9f, -9.5f, -9.5f, -5.9f
  };
  static const float dy[10] = {
    -10.0f, -8.1f, -3.1f, 3.1f, 8.1f, 10.0f, 8.1f, 3.1f, -3.1f, -8.1f
  };
  unsigned phase = (unsigned)((sceKernelGetSystemTimeWide() / 90000ULL) % 10ULL);
  int i;

  for (i = 0; i < 10; ++i) {
    int distance = (i + 10 - (int)phase) % 10;
    unsigned alpha = distance == 0 ? 0xFF : (unsigned)(0xFF - distance * 20);

    vita2d_draw_fill_circle(WINDOW_X + 30.0f + dx[i],
                            y + 12.0f + dy[i], 2.6f,
                            COLOR_ALPHA(AZURE, alpha));
  }
}

void ra_menu_draw_status(void) {
  char text[192];
  uint32_t color;
  int running;
  float y = FONT_Y_LINE(16) + 4.0f;
  float x;

  status_text(text, sizeof(text), &color, &running);
  if (text[0] == '\0')
    return;
  ra_menu_fit_text(text, sizeof(text), 760.0f, 0.9f);
  if (running) {
    status_spinner(y);
    x = WINDOW_X + 52.0f;
  } else {
    x = WINDOW_X + ALIGN_CENTER(WINDOW_WIDTH,
                                vita2d_pgf_text_width(font, 0.9f, text));
  }
  pgf_draw_text(x, y, color, 0.9f, text);
}

static char g_message[192];
static uint32_t g_message_color;

const char *ra_menu_message_text(void) { return g_message; }
uint32_t ra_menu_message_color(void) { return g_message_color; }

void ra_menu_set_message(uint32_t color, const char *format, ...) {
  va_list args;

  if (format == NULL) {
    g_message[0] = '\0';
    return;
  }

  va_start(args, format);
  vsnprintf(g_message, sizeof(g_message), format, args);
  va_end(args);
  g_message[84] = '\0';
  g_message_color = color;
}
void ra_menu_show_prompt_error(void) {
  ra_menu_set_message(RED, "Keyboard failed: 0x%08X (see RA_SETUP).",
        (unsigned)ra_ime_last_error());
}
void ra_menu_show_message(uint32_t color, const char *text) {
  ra_menu_set_message(color, "%s", text != NULL ? text : "");
}

extern void AdrenalineMenuResetDialogSnapshot(void);
extern void AdrenalineMenuPresentDialog(void);
extern void AdrenalineMenuPresentDialogClosing(void);
extern int sceCommonDialogIsRunning(void);

int ra_menu_prompt(const char *title, char *buffer, size_t capacity,
          int sensitive) {
  int result;
  int i;

  /* Do not capture a new snapshot or initialize IME while the previous
     * common dialog is still closing. */
  for (i = 0; i < 50 && sceCommonDialogIsRunning(); ++i)
    sceKernelDelayThread(10 * 1000);

  AdrenalineMenuResetDialogSnapshot();
  result = ra_ime_prompt(title, buffer, capacity, sensitive,
            AdrenalineMenuPresentDialog,
            AdrenalineMenuPresentDialogClosing);

  /* A first prompt can occasionally be dismissed by the menu's own launch
     * button before the IME receives the typed text. An empty accepted result
     * is never useful for username/password, so reopen once instead of
     * silently leaving the row empty. */
  if (result > 0 && buffer[0] == '\0') {
    ra_menu_log("ime prompt returned empty; reopening once");
    AdrenalineMenuResetDialogSnapshot();
    result = ra_ime_prompt(title, buffer, capacity, sensitive,
              AdrenalineMenuPresentDialog,
              AdrenalineMenuPresentDialogClosing);
  }

  /* The IME consumes the confirm/cancel button. Adrenaline's menu input
     * state still thinks that button is held, so the next readPad() would
     * report a release and immediately reopen the menu item. */
  resetPad();
  return result;
}

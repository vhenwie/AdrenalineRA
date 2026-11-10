#include "ra_ime.h"

#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mbedtls/platform_time.h"
#include "ra_log.h"

#define IME_TEXT_MAX (SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1)
#define IME_CLOSE_TIMEOUT_MS 250

extern int sceCommonDialogIsRunning(void);

static int g_loaded;
static int g_prompt_active;
static int g_last_error;

int ra_ime_last_error(void) {
  return g_last_error;
}

static void ime_log(const char *format, ...) {
  char text[160];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length >= (int)sizeof(text))
    length = (int)sizeof(text) - 1;
  if (length > 0)
    ra_log_append("RA_SETUP", text, length);
}

static void ensure_module(void) {
  int result;

  if (g_loaded)
    return;
  result = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
  if (result >= 0) {
    g_loaded = 1;
  } else {
    g_last_error = result;
    ime_log("ime module load result=0x%08X (continuing)\n", (unsigned)result);
  }
}

void ra_ime_preload(void) {
  if (!g_loaded)
    ensure_module();
}

static int wait_dialog_idle(void) {
  int waited = 0;

  while (waited < IME_CLOSE_TIMEOUT_MS) {
    if (!sceCommonDialogIsRunning())
      return 1;
    sceKernelDelayThread(10 * 1000);
    waited += 10;
  }
  return 0;
}

static void wait_buttons_released(void) {
  int i;

  for (i = 0; i < 60; ++i) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0 || pad.buttons == 0)
      return;
    sceKernelDelayThread(16 * 1000);
  }
}

/* Fills in the dialog description. The dialog was initialised by the caller. */
static void ime_prompt_fill_param(SceImeDialogParam *param,
    const SceWChar16 *title16, SceWChar16 *initial16, SceWChar16 *input16,
    size_t capacity, int sensitive) {
  param->supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
  param->languagesForced = SCE_TRUE;
  param->type = SCE_IME_TYPE_DEFAULT;
  param->dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
  param->textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  param->option = sensitive
    ? (SCE_IME_OPTION_NO_ASSISTANCE | SCE_IME_OPTION_NO_AUTO_CAPITALIZATION)
    : 0;
  param->title = title16;
  param->maxTextLength = (SceUInt32)(capacity - 1);
  param->initialText = initial16;
  param->inputTextBuffer = input16;
}

/* Opens the dialog and pumps it until it closes. Returns the sceImeDialogInit
 * result; *accepted is 1 only when the player pressed Enter. */
static int ime_prompt_run(const SceImeDialogParam *param, int sensitive,
    ra_ime_present_t present, ra_ime_present_t closing, int *accepted) {
  SceImeDialogResult result;
  SceCommonDialogStatus status;
  int init_result;
  int entered = 0;
  int i;

  *accepted = 0;
  memset(&result, 0, sizeof(result));

  init_result = sceImeDialogInit(param);
  if (init_result < 0)
    return init_result;

  g_loaded = 1;
  g_last_error = 0;
  ime_log("ime open sensitive=%d init=%d\n", sensitive ? 1 : 0, init_result);
  sceKernelDelayThread(50 * 1000);
  wait_buttons_released();
  ime_log("ime ready\n");
  while ((status = sceImeDialogGetStatus()) == SCE_COMMON_DIALOG_STATUS_RUNNING) {
    if (present != NULL)
      present();
    else
      sceKernelDelayThread(16 * 1000);
  }
  if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
    sceImeDialogGetResult(&result);
    entered = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
  }
  ime_log("ime close status=%d button=%d\n", (int)status, (int)result.button);
  sceImeDialogTerm();
  ime_log("ime term\n");

  if (closing != NULL) {
    for (i = 0; i < 6; ++i) {
      closing();
      if (!sceCommonDialogIsRunning())
        break;
      sceKernelDelayThread(16 * 1000);
    }
  }
  if (!wait_dialog_idle())
    ime_log("ime close wait timeout\n");
  else
    ime_log("ime close idle\n");
  sceKernelDelayThread(60 * 1000);
  *accepted = entered;
  return init_result;
}

/* Acquires the one-prompt-at-a-time guard and waits for any common dialog to
 * close. Returns 0 when the dialog may open, else the ra_ime_prompt error code. */
static int ime_prompt_begin(void) {
  if (g_prompt_active) {
    g_last_error = 0x80000002;
    ime_log("ime prompt reentry ignored\n");
    return -2;
  }
  g_prompt_active = 1;
  ensure_module();

  if (!wait_dialog_idle()) {
    g_last_error = SCE_COMMON_DIALOG_ERROR_BUSY;
    ime_log("ime open deferred: common dialog busy\n");
    g_prompt_active = 0;
    return -4;
  }
  return 0;
}

int ra_ime_prompt(const char *title, char *buffer, size_t capacity,
        int sensitive, ra_ime_present_t present, ra_ime_present_t closing) {
  static SceWChar16 title16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
  static SceWChar16 initial16[IME_TEXT_MAX];
  static SceWChar16 input16[IME_TEXT_MAX];
  SceImeDialogParam param;
  int init_result;
  int begin_result;
  int accepted = 0;
  int prompt_result = 0;

  if (buffer == NULL || capacity == 0)
    return -1;
  if (capacity > IME_TEXT_MAX)
    capacity = IME_TEXT_MAX;
  g_last_error = 0;

  begin_result = ime_prompt_begin();
  if (begin_result != 0)
    return begin_result;

  wait_buttons_released();
  sceKernelDelayThread(50 * 1000);
  ra_text_ascii_to_utf16(title, title16,
            sizeof(title16) / sizeof(title16[0]));
  ra_text_ascii_to_utf16(buffer, initial16,
            sizeof(initial16) / sizeof(initial16[0]));
  memset(input16, 0, sizeof(input16));

  sceImeDialogParamInit(&param);
  ime_prompt_fill_param(&param, title16, initial16, input16, capacity,
      sensitive);

  init_result = ime_prompt_run(&param, sensitive, present, closing,
      &accepted);
  if (init_result < 0) {
    g_last_error = init_result;
    ime_log("ime open failed=0x%08X\n", (unsigned)init_result);
    prompt_result = -3;
  }

  if (!wait_dialog_idle()) {
    g_last_error = SCE_COMMON_DIALOG_ERROR_BUSY;
    g_prompt_active = 0;
    ime_log("ime close deferred: common dialog still busy\n");
    return -4;
  }
  wait_buttons_released();
  memset(initial16, 0, sizeof(initial16));
  if (accepted) {
    ra_text_utf16_to_ascii(input16, buffer, capacity);
    prompt_result = 1;
  }
  memset(input16, 0, sizeof(input16));
  g_prompt_active = 0;
  ime_log("ime return result=%d len=%u\n", prompt_result,
      accepted ? (unsigned)strlen(buffer) : 0);
  return prompt_result;
}

void ra_text_ascii_to_utf16(const char *source, SceWChar16 *destination,
              size_t capacity) {
  size_t i = 0;

  if (destination == NULL || capacity == 0)
    return;
  if (source == NULL)
    source = "";
  while (source[i] != '\0' && i + 1 < capacity) {
    destination[i] = (SceWChar16)(unsigned char)source[i];
    ++i;
  }
  destination[i] = 0;
}

void ra_text_utf16_to_ascii(const SceWChar16 *source, char *destination,
              size_t capacity) {
  size_t i = 0;

  if (destination == NULL || capacity == 0)
    return;
  if (source == NULL) {
    destination[0] = '\0';
    return;
  }
  while (source[i] != 0 && i + 1 < capacity) {
    destination[i] = (char)(source[i] & 0x7F);
    ++i;
  }
  destination[i] = '\0';
}

/* Vita libc compatibility required by rcheevos and Mbed TLS. */

mbedtls_ms_time_t mbedtls_ms_time(void) {
  return (mbedtls_ms_time_t)(sceKernelGetSystemTimeWide() / 1000);
}

struct tm *gmtime_r(const time_t *timer, struct tm *result) {
  struct tm *value = gmtime(timer);

  if (value == NULL || result == NULL)
    return NULL;
  *result = *value;
  return result;
}

/* The Vita target links SceLibc through stubs with -nostdlib, so newlib's
 * _ctype_ is not on the link line: removing this table leaves the rcheevos
 * ctype lookups and ra_runtime_plugins unresolved. Keep it until the build
 * links newlib's libc instead. */
const char _ctype_[257] = {
  [0] = 0,
  [1 ... 8] = _C,
  [9 ... 13] = _C | _S,
  [14 ... 31] = _C,
  [32] = _S | _B,
  [33 ... 47] = _P,
  [48 ... 57] = _N,
  [58 ... 64] = _P,
  [65 ... 70] = _U | _X,
  [71 ... 90] = _U,
  [91 ... 96] = _P,
  [97 ... 102] = _L | _X,
  [103 ... 122] = _L,
  [123 ... 126] = _P,
  [127] = _C,
};

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
  int64_t usec;

  (void)clock_id;
  if (tp == NULL)
    return -1;
  usec = (int64_t)sceKernelGetSystemTimeWide();
  tp->tv_sec = (time_t)(usec / 1000000);
  tp->tv_nsec = (long)((usec % 1000000) * 1000);
  return 0;
}

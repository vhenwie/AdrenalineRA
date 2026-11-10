#ifndef RA_IME_H
#define RA_IME_H

#include <stddef.h>

#include <psp2/ime_dialog.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ra_ime_present_t)(void);

/* Native Vita keyboard. `present` is called while the dialog is running so the
 * caller can redraw its own UI behind the system keyboard. `sensitive` keeps
 * the text visible while typing but disables IME assistance and auto-capital-
 * ization so the system does not offer or learn the entered secret. */
int ra_ime_prompt(const char *title, char *buffer, size_t capacity,
                  int sensitive, ra_ime_present_t present,
                  ra_ime_present_t closing);
int ra_ime_last_error(void);

/* Load the IME sysmodule before the first prompt. Doing this while the menu is
 * already running avoids a first-open stall/flicker on the Account page. */
void ra_ime_preload(void);

void ra_text_ascii_to_utf16(const char *source, SceWChar16 *destination,
                            size_t capacity);
void ra_text_utf16_to_ascii(const SceWChar16 *source, char *destination,
                            size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RA_IME_H */

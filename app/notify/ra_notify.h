#ifndef RA_NOTIFY_H
#define RA_NOTIFY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  RA_NOTIFY_INFO = 0,
  RA_NOTIFY_SESSION,
  RA_NOTIFY_UNLOCK,
  RA_NOTIFY_ERROR,
  RA_NOTIFY_MASTERY,
  RA_NOTIFY_LB_START,
  RA_NOTIFY_LB_SUBMIT,
  RA_NOTIFY_LB_FAIL
};

void ra_notify_init(void);
void ra_notify_clear(void);
/* 1 = drop every new notification (and clear the queue). Used for the whole quit
 * and official-settings dialog window, so a late leaderboard result cannot appear
 * on the quit screen. */
void ra_notify_suppress(int suppressed);

/* There is no loading spinner: a second drawing thread cannot be joined safely
 * while the emulated world is held, and abandoning it crashed pspemu. See the
 * note in ra_notify.c. The session hold is now short (parse only). */

void ra_notify_set_position(int anchor, int padding_auto, int padding_h,
              int padding_v);
void ra_notify_push(int kind, uint32_t id, const char *title,
          const char *subtitle);
void ra_notify_push_points(int kind, uint32_t id, int points,
            const char *title, const char *subtitle);
/* Unlock card with a cached achievement badge. badge_key may be NULL or empty;
 * the existing procedural glyph remains the fallback. */
void ra_notify_push_achievement(int kind, uint32_t id, int points,
                const char *title, const char *subtitle,
                const char *badge_key);

/* Live indicators: persistent tiles drawn while a challenge achievement, progress
 * indicator or leaderboard tracker is active. A challenge shows its badge art
 * only, like PPSSPP and PCSX2; progress and trackers add a short caption with the
 * number. They are independent of the card queue, fade in/out on state changes,
 * and are only dropped wholesale by ra_notify_clear(). */
enum {
  RA_INDICATOR_CHALLENGE = 0,
  RA_INDICATOR_PROGRESS,
  RA_INDICATOR_TRACKER
};

void ra_notify_indicator_set(int kind, uint32_t id, const char *text);
/* Same, plus the achievement badge key and the progress caption ("12/30").
 * Either text or progress must be non-empty; badge_key may be NULL.
 *
 * Progress is a singleton, like PPSSPP's single ACHIEVEMENT_PROGRESS entry: a
 * second progress indicator takes the first one's slot (art swaps in place, no
 * new fade), so the row cannot fill up with badges. */
void ra_notify_indicator_set_detail(int kind, uint32_t id, const char *text,
                  const char *progress, const char *badge_key);
void ra_notify_indicator_clear(int kind, uint32_t id);
/* Clear every live indicator of one kind, fading them out. Used for progress
 * because rcheevos raises its HIDE from a timer with no achievement attached. */
void ra_notify_indicator_clear_kind(int kind);
/* Cards on screen plus cards waiting in the queue (0..RA_NOTIFY_QUEUE_SIZE+1).
 * Diagnostics only: the draw thread logs it so a banner burst that never reaches
 * the screen can be told apart from one that was never queued. */
int ra_notify_card_count(void);
/* Anchor for the indicator rows: 0/1/2 top left/center/right,
 * 3/4/5 bottom left/center/right. Default 5 (bottom right). */
void ra_notify_set_indicator_position(int anchor);
/* Tile size: 0 small (36 px), 1 standard (48 px), 2 large (60 px). */
void ra_notify_set_indicator_size(int size);

/* A live indicator always takes the frame from pspemu's direct rendering path, for as
 * long as it lasts - same cost as a card. That is the player decision: a live
 * challenge/progress/tracker tile must stay on screen, so the `Live Indicator Frame`
 * setting ("only with banners") and the suppressed-draw path it needed were removed
 * . Cards always took the frame; now there is no second policy to disagree
 * with them. */
/* 1 while a card is on screen or a live indicator is present. The draw thread
 * uses this to decide whether the overlay needs the frame at all, so it must
 * include the indicators (direct rendering skips vita2d until this is 1). */
int ra_notify_visible(void);
/* Live indicator slots in use (fades included), 0..6. Diagnostics only: the draw
 * thread logs it so a missing tile can be told apart from a frame that was never
 * composited. */
int ra_notify_indicator_count(void);
void ra_notify_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_NOTIFY_H */

#include "ra_notify.h"
#include "ra_notify_internal.h"
#include "ra_notify_queue.h"

#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "ra_badge.h"
#include "ra_notify_gfx.h"
#include "ra_notify_indicator.h"
#include "ra_log.h"
#include "ra_logqueue.h"
#include "ra_sound.h"

#define RA_NOTIFY_QUEUE_SIZE 16
#define RA_NOTIFY_TITLE_MAX 96
#define RA_NOTIFY_SUBTITLE_MAX 160
#define RA_NOTIFY_CARD_W 720.0f
#define RA_NOTIFY_CARD_H 96.0f
#define RA_NOTIFY_CARD_MIN_W 360.0f
#define RA_NOTIFY_CARD_MIN_H 84.0f
#define RA_NOTIFY_ENTER_US 180000ULL
#define RA_NOTIFY_EXIT_US 350000ULL



/* Age after which a queued leaderboard-start or info card is no longer worth
 * showing. Results and unlocks are never dropped by age. */
#define RA_NOTIFY_STALE_US 6000000ULL

/* 1 while the quit/official-settings dialog owns the screen: no new card may be
 * queued, even if a leaderboard result arrives from a worker after the clear. */

/* Live indicators: one badge tile per active challenge, one for live progress,
 * and a leaderboard tracker box, matching PPSSPP/PCSX2 layout. */
/* menu.c keeps its own logger static, so the notification module needs one. Only the
 * rare drops and supersedes below log, never the per-frame paths. */





/* Frees one slot in a full queue: the oldest non-unlock card first, then the
 * oldest card. This is what stops a burst of info/leaderboard cards from
 * costing an achievement banner. */

/* Inserts at a ring index; room must already exist (queue_make_room_locked). */

/* Ring index of the first queued leaderboard result, or -1. A start notice goes in
 * front of it: rcheevos raises the start before the submit, so showing the submit
 * first reverses the story. */

/* Drops the first queued entry of one kind and id. Returns 1 when one was found. */

/* A backlog of cards must not keep the player reading yesterday's message. Once
 * more than two are waiting, the informational kinds (leaderboard notices and RA
 * messages) are cut in half; unlock and mastery cards keep their full time because
 * they are the ones a player must be able to read. */



int ra_notify_visible(void) {
  int visible;
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

  ra_notify_lock();
  ra_notify_indicator_expire(now);
  visible = ra_notify_queue_card_visible_locked() || ra_notify_indicator_visible(now);
  ra_notify_unlock();
  return visible;
}

void ra_notify_init(void) {
  ra_badge_init();
  ra_notify_lock();
  ra_notify_unlock();
}

void ra_notify_clear(void) {
  ra_notify_queue_clear();
  ra_notify_lock();
  ra_notify_indicator_reset();
  ra_notify_unlock();
}

void ra_notify_suppress(int suppressed) {
  ra_notify_queue_suppress(suppressed);
  if (suppressed)
    ra_notify_clear();
}

void ra_notify_log(const char *format, ...) {
  char text[224];
  va_list args;
  int length;

  va_start(args, format);
  length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length > 0)
    ra_logq_line(text, length);
}





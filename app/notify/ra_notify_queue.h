#ifndef RA_NOTIFY_QUEUE_H
#define RA_NOTIFY_QUEUE_H

#include <stdint.h>
#include "ra_notify.h"
#include "ra_badge.h"

#define RA_NOTIFY_QUEUE_SIZE 16
#define RA_NOTIFY_TITLE_MAX 96
#define RA_NOTIFY_SUBTITLE_MAX 160
#define RA_NOTIFY_STALE_US 6000000ULL

typedef struct {
  int kind;
  uint32_t id;
  int points;
  char title[RA_NOTIFY_TITLE_MAX];
  char subtitle[RA_NOTIFY_SUBTITLE_MAX];
  char badge_key[RA_BADGE_KEY_MAX];
  unsigned long long start_us;
  unsigned long long end_us;
  /* When the card was pushed. A leaderboard "started" notice shown ten seconds
     * after the fact is worse than no notice at all, so the pump drops those. */
  unsigned long long queued_us;
} RaNotifyItem;

void ra_notify_queue_clear(void);
void ra_notify_queue_suppress(int suppressed);
int ra_notify_queue_card_visible(void);
int ra_notify_queue_card_visible_locked(void);
int ra_notify_take_current(RaNotifyItem *out, unsigned long long *now);
int ra_notify_anchor(void);
int ra_notify_padding_auto(void);
int ra_notify_padding_h(void);
int ra_notify_padding_v(void);

#endif

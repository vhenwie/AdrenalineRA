#include "ra_notify_queue.h"
#include <stdio.h>
#include "ra_notify_internal.h"

#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "ra_sound.h"

static SceUID g_lock = -1;

void ra_notify_lock(void) {
  if (g_lock < 0)
    g_lock = sceKernelCreateSema("ra_notify", 0, 1, 1, NULL);
  if (g_lock >= 0)
    sceKernelWaitSema(g_lock, 1, NULL);
}

void ra_notify_unlock(void) {
  if (g_lock >= 0)
    sceKernelSignalSema(g_lock, 1);
}

uint32_t ra_notify_accent_for(int kind) {
  switch (kind) {
  case RA_NOTIFY_UNLOCK:
  case RA_NOTIFY_MASTERY:
    return 0xFF45B0E8;
  case RA_NOTIFY_ERROR:
    return 0xFF7373E5;
  case RA_NOTIFY_LB_START:
  case RA_NOTIFY_LB_SUBMIT:
  case RA_NOTIFY_LB_FAIL:
    return 0xFFC868BA;
  case RA_NOTIFY_SESSION:
  case RA_NOTIFY_INFO:
  default:
    return 0xFFF7C34F;
  }
}

int ra_notify_sound_for(int kind) {
  if (kind == RA_NOTIFY_UNLOCK || kind == RA_NOTIFY_MASTERY)
    return RA_SOUND_UNLOCK;
  if (kind == RA_NOTIFY_LB_SUBMIT)
    return RA_SOUND_LBSUBMIT;
  return RA_SOUND_MESSAGE;
}

int ra_notify_kind_is_leaderboard(int kind) {
  return kind == RA_NOTIFY_LB_START || kind == RA_NOTIFY_LB_SUBMIT ||
    kind == RA_NOTIFY_LB_FAIL;
}

int ra_notify_kind_is_leaderboard_result(int kind) {
  return kind == RA_NOTIFY_LB_SUBMIT || kind == RA_NOTIFY_LB_FAIL;
}

unsigned long long ra_notify_duration_for(int kind) {
  switch (kind) {
  case RA_NOTIFY_SESSION:
    return 6000000ULL;
  case RA_NOTIFY_UNLOCK:
    return 5000000ULL;
  case RA_NOTIFY_MASTERY:
    return 20000000ULL;
  case RA_NOTIFY_ERROR:
    return 6000000ULL;
  case RA_NOTIFY_LB_START:
  case RA_NOTIFY_LB_SUBMIT:
  case RA_NOTIFY_LB_FAIL:
    return 3000000ULL;
  default:
    return 4000000ULL;
  }
}

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source != NULL ? source : "");
}


static RaNotifyItem g_current;
static int g_current_active;
static RaNotifyItem g_queue[RA_NOTIFY_QUEUE_SIZE];
static int g_queue_head;
static int g_queue_tail;
static int g_suppressed;
static int g_anchor = 2;
static int g_padding_auto = 1;
static int g_padding_h = 2;
static int g_padding_v = 2;

static int queue_depth_locked(void) {
  return (g_queue_tail - g_queue_head + RA_NOTIFY_QUEUE_SIZE) %
    RA_NOTIFY_QUEUE_SIZE;
}

static void queue_make_room_locked(void) {
  int drop = g_queue_head;
  int scan = g_queue_head;

  while (scan != g_queue_tail) {
    if (g_queue[scan].kind != RA_NOTIFY_UNLOCK) {
      drop = scan;
      break;
    }
    scan = (scan + 1) % RA_NOTIFY_QUEUE_SIZE;
  }
  while (drop != g_queue_tail) {
    int following = (drop + 1) % RA_NOTIFY_QUEUE_SIZE;

    g_queue[drop] = g_queue[following];
    drop = following;
  }
  g_queue_tail = (g_queue_tail == 0)
          ? RA_NOTIFY_QUEUE_SIZE - 1
          : g_queue_tail - 1;
}

static void queue_insert_locked(int position, const RaNotifyItem *item) {
  int index = g_queue_tail;

  while (index != position) {
    int previous =
      (index == 0) ? RA_NOTIFY_QUEUE_SIZE - 1 : index - 1;

    g_queue[index] = g_queue[previous];
    index = previous;
  }
  g_queue[position] = *item;
  g_queue_tail = (g_queue_tail + 1) % RA_NOTIFY_QUEUE_SIZE;
}

static int queue_first_result_locked(void) {
  int index = g_queue_head;

  while (index != g_queue_tail) {
    if (ra_notify_kind_is_leaderboard_result(g_queue[index].kind))
      return index;
    index = (index + 1) % RA_NOTIFY_QUEUE_SIZE;
  }
  return -1;
}

static int queue_drop_locked(int kind, uint32_t id) {
  int index = g_queue_head;

  while (index != g_queue_tail) {
    if (g_queue[index].kind == kind && g_queue[index].id == id) {
      int move = index;

      while (move != g_queue_tail) {
        int following = (move + 1) % RA_NOTIFY_QUEUE_SIZE;

        if (following == g_queue_tail)
          break;
        g_queue[move] = g_queue[following];
        move = following;
      }
      g_queue_tail = (g_queue_tail == 0)
              ? RA_NOTIFY_QUEUE_SIZE - 1
              : g_queue_tail - 1;
      return 1;
    }
    index = (index + 1) % RA_NOTIFY_QUEUE_SIZE;
  }
  return 0;
}

static unsigned long long duration_for_locked(int kind) {
  unsigned long long duration = ra_notify_duration_for(kind);

  if (queue_depth_locked() > 2 &&
    (ra_notify_kind_is_leaderboard(kind) || kind == RA_NOTIFY_INFO) &&
    duration > 1800000ULL)
    return duration / 2;
  return duration;
}

static void start_current(const RaNotifyItem *item) {
  g_current = *item;
  g_current.start_us = (unsigned long long)sceKernelGetSystemTimeWide();
  g_current.end_us = g_current.start_us + duration_for_locked(item->kind);
  g_current_active = 1;
  ra_sound_play(ra_notify_sound_for(item->kind));
}

static int pop_locked(void) {
  while (g_queue_head != g_queue_tail) {
    RaNotifyItem item = g_queue[g_queue_head];
    unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

    g_queue_head = (g_queue_head + 1) % RA_NOTIFY_QUEUE_SIZE;
    /* A "leaderboard started" notice or an RA info line that has been waiting
         * this long has stopped being true or useful, and showing it late is exactly
         * what made the start of a leaderboard appear after its result
         * (). Results and unlocks are never dropped by age: a result is
         * what the player is waiting for, and an unlock is the point of the session. */
    if (item.queued_us != 0 && now > item.queued_us &&
      now - item.queued_us > RA_NOTIFY_STALE_US &&
      (item.kind == RA_NOTIFY_LB_START || item.kind == RA_NOTIFY_INFO)) {
      ra_notify_log("card: dropped stale kind=%d id=%u (%u ms late)\n",
          item.kind, (unsigned)item.id,
          (unsigned)((now - item.queued_us) / 1000));
      continue;
    }
    start_current(&item);
    return 1;
  }
  return 0;
}

static int expire_or_pop_locked(void) {
  unsigned long long now = (unsigned long long)sceKernelGetSystemTimeWide();

  if (g_current_active && now >= g_current.end_us)
    g_current_active = 0;
  if (!g_current_active && !pop_locked())
    return 0;
  return 1;
}

int ra_notify_card_count(void) {
  int count;

  ra_notify_lock();
  count = (g_current_active ? 1 : 0);
  if (g_queue_tail >= g_queue_head)
    count += g_queue_tail - g_queue_head;
  else
    count += RA_NOTIFY_QUEUE_SIZE - g_queue_head + g_queue_tail;
  ra_notify_unlock();
  return count;
}

void ra_notify_set_position(int anchor, int padding_auto, int padding_h,
              int padding_v) {
  g_anchor = anchor >= 0 && anchor <= 5 ? anchor : 2;
  g_padding_auto = padding_auto != 0;
  g_padding_h = padding_h;
  g_padding_v = padding_v;
}

/* Suppressed, untitled, and SESSION cards (which clear the queue first). Returns 1
 * when the card must be dropped. */
static int notify_push_gate(int kind, const char *title) {
  if (g_suppressed)
    return 1;
  if (title == NULL || title[0] == '\0')
    return 1;
  if (kind == RA_NOTIFY_SESSION)
    ra_notify_clear();
  return 0;
}

static void notify_item_init(RaNotifyItem *item, int kind, uint32_t id,
    int points, const char *title, const char *subtitle,
    const char *badge_key) {
  memset(item, 0, sizeof(*item));
  item->kind = kind;
  item->id = id;
  item->points = points;
  copy_text(item->title, sizeof(item->title), title);
  copy_text(item->subtitle, sizeof(item->subtitle), subtitle);
  copy_text(item->badge_key, sizeof(item->badge_key), badge_key);
  item->queued_us = (unsigned long long)sceKernelGetSystemTimeWide();
}

static void notify_log_leaderboard(int kind, uint32_t id, const char *title) {
  if (ra_notify_kind_is_leaderboard(kind))
    ra_notify_log("card: +%s id=%u '%s'\n",
        kind == RA_NOTIFY_LB_START
          ? "start"
          : (kind == RA_NOTIFY_LB_SUBMIT ? "result" : "fail"),
        (unsigned)id, title);
}

/* Caller holds the lock. A result cuts its own start card short and drops the
 * queued duplicate of that start. */
static void notify_push_cut_start_locked(int kind, uint32_t id,
    unsigned long long now) {
  if (ra_notify_kind_is_leaderboard_result(kind) && id != 0) {
    if (g_current_active && g_current.kind == RA_NOTIFY_LB_START &&
      g_current.id == id) {
      /* Cut the start card short: its result follows in a moment. */
      g_current.end_us = now;
      ra_notify_log("card: leaderboard start id=%u cut short by its result\n",
          (unsigned)id);
    }
    if (queue_drop_locked(RA_NOTIFY_LB_START, id))
      ra_notify_log("card: dropped queued leaderboard start id=%u\n",
          (unsigned)id);
  }
}

/* Caller holds the lock. De-duplicate per (kind, id), not per id: a leaderboard's
 * start and its result share the id, and they are two different messages. */
static int notify_push_duplicate_locked(int kind, uint32_t id) {
  int i;

  if (id == 0)
    return 0;
  if (g_current_active && g_current.id == id && g_current.kind == kind)
    return 1;
  for (i = g_queue_head; i != g_queue_tail;
    i = (i + 1) % RA_NOTIFY_QUEUE_SIZE) {
    if (g_queue[i].id == id && g_queue[i].kind == kind)
      return 1;
  }
  return 0;
}

/* Caller holds the lock. Starts the card when nothing is on screen, otherwise
 * queues it. */
static void notify_push_enqueue_locked(const RaNotifyItem *item) {
  int position;

  if (!g_current_active) {
    start_current(item);
    return;
  }

  /* Room first: eviction keeps unlock cards and drops the oldest card
       * otherwise, so a burst of info/leaderboard notices can never cost an
       * achievement banner. */
  if (queue_depth_locked() + 1 >= RA_NOTIFY_QUEUE_SIZE)
    queue_make_room_locked();
  position = g_queue_tail;
  if (item->kind == RA_NOTIFY_LB_START) {
    int ahead = queue_first_result_locked();

    /* The start of a leaderboard belongs in front of results that are still
           * waiting: rcheevos raises it first, so showing the submit first would
           * tell the story backwards. */
    if (ahead >= 0) {
      position = ahead;
      ra_notify_log("card: leaderboard start id=%u goes ahead of a queued "
          "result\n",
          (unsigned)item->id);
    }
  }
  queue_insert_locked(position, item);
}

static void notify_push_internal(int kind, uint32_t id, int points,
                const char *title, const char *subtitle,
                const char *badge_key) {
  RaNotifyItem item;

  if (notify_push_gate(kind, title))
    return;

  notify_item_init(&item, kind, id, points, title, subtitle, badge_key);
  
  notify_log_leaderboard(kind, id, title);

  ra_notify_lock();

  notify_push_cut_start_locked(kind, id, item.queued_us);
  if (kind == RA_NOTIFY_LB_START && id != 0 && g_current_active &&
    ra_notify_kind_is_leaderboard_result(g_current.kind) && g_current.id == id) {
    ra_notify_unlock();
    ra_notify_log("card: dropped leaderboard start id=%u (result already up)\n",
        (unsigned)id);
    return;
  }

  if (notify_push_duplicate_locked(kind, id)) {
    ra_notify_unlock();
    return;
  }

  notify_push_enqueue_locked(&item);
  ra_notify_unlock();
}

void ra_notify_push_points(int kind, uint32_t id, int points,
            const char *title, const char *subtitle) {
  notify_push_internal(kind, id, points, title, subtitle, NULL);
}

void ra_notify_push_achievement(int kind, uint32_t id, int points,
                const char *title, const char *subtitle,
                const char *badge_key) {
  notify_push_internal(kind, id, points, title, subtitle, badge_key);
}

void ra_notify_push(int kind, uint32_t id, const char *title,
          const char *subtitle) {
  notify_push_internal(kind, id, 0, title, subtitle, NULL);
}


void ra_notify_queue_clear(void) {
  ra_notify_lock();
  g_current_active = 0;
  g_queue_head = g_queue_tail = 0;
  memset(&g_current, 0, sizeof(g_current));
  ra_notify_unlock();
}

void ra_notify_queue_suppress(int suppressed) {
  ra_notify_lock();
  g_suppressed = suppressed != 0;
  ra_notify_unlock();
}

int ra_notify_queue_card_visible_locked(void) {
  (void)expire_or_pop_locked();
  return g_current_active || g_queue_head != g_queue_tail;
}

int ra_notify_queue_card_visible(void) {
  int visible;
  ra_notify_lock();
  visible = ra_notify_queue_card_visible_locked();
  ra_notify_unlock();
  return visible;
}

int ra_notify_take_current(RaNotifyItem *out, unsigned long long *now) {
  int visible;
  if (out == NULL || now == NULL)
    return 0;
  ra_notify_lock();
  visible = expire_or_pop_locked();
  if (visible)
    *out = g_current;
  *now = (unsigned long long)sceKernelGetSystemTimeWide();
  ra_notify_unlock();
  return visible;
}

int ra_notify_anchor(void) { return g_anchor; }
int ra_notify_padding_auto(void) { return g_padding_auto; }
int ra_notify_padding_h(void) { return g_padding_h; }
int ra_notify_padding_v(void) { return g_padding_v; }

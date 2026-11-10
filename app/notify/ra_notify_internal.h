#ifndef RA_NOTIFY_INTERNAL_H
#define RA_NOTIFY_INTERNAL_H

#include <stdint.h>

uint32_t ra_notify_accent_for(int kind);
int ra_notify_sound_for(int kind);
int ra_notify_kind_is_leaderboard(int kind);
int ra_notify_kind_is_leaderboard_result(int kind);
unsigned long long ra_notify_duration_for(int kind);

void ra_notify_lock(void);
void ra_notify_unlock(void);

void ra_notify_log(const char *format, ...);

#endif

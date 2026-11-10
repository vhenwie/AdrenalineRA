#ifndef RA_NOTIFY_INDICATOR_H
#define RA_NOTIFY_INDICATOR_H

void ra_notify_indicator_draw(void);
void ra_notify_indicator_expire(unsigned long long now);
int ra_notify_indicator_visible(unsigned long long now);
void ra_notify_indicator_reset(void);

#endif

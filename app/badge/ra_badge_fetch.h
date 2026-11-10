#ifndef RA_BADGE_FETCH_H
#define RA_BADGE_FETCH_H

#include "ra_badge.h"

typedef struct {
  char key[RA_BADGE_KEY_MAX];
  char url[RA_BADGE_URL_MAX];
  int prime;
} RaBadgeJob;


int ra_badge_fetch(const RaBadgeJob *job);
void ra_badge_pause(unsigned int ms);
unsigned int ra_badge_last_write_ms(void);

#endif

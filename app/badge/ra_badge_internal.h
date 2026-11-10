#ifndef RA_BADGE_INTERNAL_H
#define RA_BADGE_INTERNAL_H

void ra_badge_log(const char *format, ...);

int ra_badge_missing_recently(const char *key);
void ra_badge_note_missing(const char *key);
void ra_badge_note_present(const char *key);

int ra_badge_exists(const char *key);
int ra_badge_cached(const char *key);

#endif

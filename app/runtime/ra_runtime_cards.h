#ifndef RA_RUNTIME_CARDS_H
#define RA_RUNTIME_CARDS_H

void ra_runtime_push_session_card(int disc);
void ra_runtime_push_disc_banner(int disc);
int ra_runtime_announced_disc_get(void);
void ra_runtime_announced_disc_set(int disc);

#endif

#ifndef RA_SOUND_H
#define RA_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
  RA_SOUND_MESSAGE = 0,
  RA_SOUND_UNLOCK,
  RA_SOUND_LBSUBMIT
};

void ra_sound_init(void);
void ra_sound_set_options(int enabled, int message, int unlock, int lb_submit);
void ra_sound_play(int kind);

#ifdef __cplusplus
}
#endif

#endif /* RA_SOUND_H */

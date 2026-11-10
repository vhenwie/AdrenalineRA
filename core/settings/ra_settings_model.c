#include "ra_settings.h"

#include <string.h>

static uint8_t clamp_u8(uint8_t value, uint8_t minimum, uint8_t maximum) {
  if (value < minimum)
    return minimum;
  if (value > maximum)
    return maximum;
  return value;
}

void ra_settings_defaults(RaSettings *settings) {
  if (settings == NULL)
    return;
  memset(settings, 0, sizeof(*settings));
  settings->notifications = 1;
  settings->hardcore = 1;
  settings->leaderboards = 3;
  settings->badges = 1;
  settings->badge_prefetch = 1;
  settings->sound_effects = 1;
  settings->sound_message = 1;
  settings->sound_unlock = 1;
  settings->sound_lboard_submit = 1;
  settings->appearance_anchor = 2;
  settings->indicator_anchor = 5;
  settings->indicator_size = 1;
  settings->appearance_padding_auto = 1;
  settings->appearance_padding_h = 2;
  settings->appearance_padding_v = 2;
  settings->visibility_summary = 2;
  settings->visibility_unlock = 1;
  settings->visibility_mastery = 1;
  settings->visibility_challenge = 1;
  settings->visibility_progress = 1;
  settings->visibility_lboard_start = 1;
  settings->visibility_lboard_submit = 1;
  settings->visibility_lboard_cancel = 1;
  settings->visibility_lboard_trackers = 1;
  settings->indicator_values = 0;
  settings->visibility_login = 1;
  settings->visibility_verbose = 0;
  settings->logging = 0;
}
void ra_settings_normalize(RaSettings *settings, int signed_in) {
  if (settings == NULL)
    return;
  settings->leaderboards = clamp_u8(settings->leaderboards, 0, 3);
  settings->appearance_anchor = clamp_u8(settings->appearance_anchor, 0, 5);
  settings->indicator_anchor = clamp_u8(settings->indicator_anchor, 0, 5);
  settings->indicator_size = clamp_u8(settings->indicator_size, 0, 2);
  settings->appearance_padding_h = clamp_u8(settings->appearance_padding_h, 0, 4);
  settings->appearance_padding_v = clamp_u8(settings->appearance_padding_v, 0, 4);
  settings->visibility_summary = clamp_u8(settings->visibility_summary, 0, 2);
  settings->sound_message = clamp_u8(settings->sound_message, 0, 2);
  settings->sound_unlock = clamp_u8(settings->sound_unlock, 0, 2);
  settings->sound_lboard_submit = clamp_u8(settings->sound_lboard_submit, 0, 2);
  if (!signed_in)
    settings->ra_enabled = 0;
}

int ra_settings_hardcore_effective(const RaSettings *settings, int signed_in) {
  return settings != NULL && signed_in && settings->ra_enabled != 0 &&
    settings->hardcore != 0;
}

int ra_session_mode_value(int spectator, int encore) {
  if (spectator)
    return 1;
  if (encore)
    return 2;
  return 0;
}

void ra_session_mode_flags(int mode, int *spectator, int *encore) {
  if (mode < 0 || mode > 2)
    mode = 0;
  if (spectator != NULL)
    *spectator = mode == 1 ? 1 : 0;
  if (encore != NULL)
    *encore = mode == 2 ? 1 : 0;
}

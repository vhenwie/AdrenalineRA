#include "ra_settings_policy.h"

#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <string.h>

#include "ra_settings.h"

int ra_settings_policy_value(const char *text, const char *key, char *output,
            size_t capacity) {
  const char *line = text;
  size_t key_length = strlen(key);

  if (output == NULL || capacity == 0)
    return 0;
  output[0] = '\0';
  while (line != NULL && *line != '\0') {
    const char *end = strchr(line, '\n');
    size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
    const char *equals = memchr(line, '=', length);

    if (equals != NULL) {
      const char *key_end = equals;
      const char *value = equals + 1;
      const char *value_end = line + length;
      size_t actual_key_length;

      while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t'))
        --key_end;
      actual_key_length = (size_t)(key_end - line);
      while (value < value_end && (*value == ' ' || *value == '\t'))
        ++value;
      while (value_end > value && (value_end[-1] == '\r' ||
             value_end[-1] == ' ' || value_end[-1] == '\t'))
        --value_end;
      if (actual_key_length == key_length &&
          strncmp(line, key, key_length) == 0) {
        size_t value_length = (size_t)(value_end - value);
        if (value_length >= capacity)
          value_length = capacity - 1;
        memcpy(output, value, value_length);
        output[value_length] = '\0';
        return 1;
      }
    }
    line = end != NULL ? end + 1 : NULL;
  }
  return 0;
}

int ra_settings_format_policy(const RaSettings *settings, char *text,
    size_t capacity) {
  int hardcore;
  int hardcore_pref;
  int ra_enabled;
  int plugins;
  const char *savestates;
  int length;

  ra_enabled = settings->ra_enabled != 0;
  hardcore_pref = settings->hardcore != 0;
  hardcore = ra_enabled && hardcore_pref;
  plugins = !ra_enabled ? 0 : (hardcore ? 2 : 1);
  savestates = hardcore ? "save-only" : "on";

  length = snprintf(text, capacity,
    "version=%u\nra_enabled=%d\nhardcore=%d\nhardcore_pref=%d\n"
    "savestates=%s\nplugins=%s\nnotifications=%d\nnext_achievement=%d\n"
    "leaderboards=%d\nbadges=%d\nbadge_prefetch=%d\ntest_unofficial=%d\n"
    "auto_screenshot=%d\nspectator=%d\nencore=%d\nsound_effects=%d\n"
    "sound_message=%d\nsound_unlock=%d\nsound_lboard_submit=%d\n"
    "appearance_anchor=%d\nindicator_anchor=%d\nindicator_size=%d\n"
    "indicator_values=%d\nappearance_padding_auto=%d\n"
    "appearance_padding_h=%d\nappearance_padding_v=%d\n"
    "visibility_summary=%d\nvisibility_unlock=%d\nvisibility_mastery=%d\n"
    "visibility_challenge=%d\nvisibility_progress=%d\n"
    "visibility_lboard_start=%d\nvisibility_lboard_submit=%d\n"
    "visibility_lboard_cancel=%d\nvisibility_lboard_trackers=%d\n"
    "visibility_login=%d\nvisibility_account=%d\nvisibility_verbose=%d\n"
    "logging=%d\n",
    (unsigned)POLICY_VERSION, ra_enabled, hardcore, hardcore_pref, savestates,
    plugins == 0 ? "normal" : (plugins == 2 ? "none" : "allowlist"),
    settings->notifications, settings->next_achievement, settings->leaderboards,
    settings->badges, settings->badge_prefetch, settings->test_unofficial,
    settings->auto_screenshot, settings->spectator, settings->encore,
    settings->sound_effects, settings->sound_message, settings->sound_unlock,
    settings->sound_lboard_submit, settings->appearance_anchor,
    settings->indicator_anchor, settings->indicator_size,
    settings->indicator_values, settings->appearance_padding_auto,
    settings->appearance_padding_h, settings->appearance_padding_v,
    settings->visibility_summary, settings->visibility_unlock,
    settings->visibility_mastery, settings->visibility_challenge,
    settings->visibility_progress, settings->visibility_lboard_start,
    settings->visibility_lboard_submit, settings->visibility_lboard_cancel,
    settings->visibility_lboard_trackers, settings->visibility_login,
    settings->visibility_login, settings->visibility_verbose, settings->logging);
  if (length < 0 || (size_t)length >= capacity)
    return -1;
  return length;
}

int ra_settings_write_policy_mirror(const char *text) {
  char temporary[160];
  SceUID fd;

  snprintf(temporary, sizeof(temporary), "%s.tmp", RA_SETTINGS_POLICY_PATH);
  fd = sceIoOpen(temporary, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return 0;
  if (sceIoWrite(fd, text, strlen(text)) < 0) {
    sceIoClose(fd);
    sceIoRemove(temporary);
    return 0;
  }
  sceIoClose(fd);
  if (sceIoRename(temporary, RA_SETTINGS_POLICY_PATH) < 0) {
    sceIoRemove(RA_SETTINGS_POLICY_PATH);
    if (sceIoRename(temporary, RA_SETTINGS_POLICY_PATH) < 0) {
      sceIoRemove(temporary);
      return 0;
    }
  }
  return 1;
}

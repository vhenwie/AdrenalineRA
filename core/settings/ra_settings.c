#include "ra_settings.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_gate.h"
#include "ra_settings_policy.h"

#define BIN_MAGIC 0x31534152u
#define BIN_VERSION 1u
#define TEXT_MAX 2048u
#define SIGNED_TEXT_MAX (TEXT_MAX + 80u)

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t payload_len;
  uint32_t crc;
  uint8_t mac[16];
} SettingsHeader;

static RaSettingsAuthState g_auth_state = RA_SETTINGS_AUTH_UNKNOWN;

static int dir_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

static void ensure_dir(const char *path) {
  if (!dir_exists(path))
    sceIoMkdir(path, 0777);
}

static uint32_t crc_update(uint32_t hash, const uint8_t *data, size_t length) {
  size_t i;

  for (i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t crc_parts(const uint8_t *first, size_t first_length,
                const uint8_t *second, size_t second_length) {
  uint32_t hash = crc_update(2166136261u, first, first_length);
  return crc_update(hash, second, second_length);
}

static void hmac_parts(const uint8_t *key, size_t keylen,
                const uint8_t *first, size_t first_length,
                const uint8_t *second, size_t second_length,
                uint8_t out[32]) {
  uint8_t k[64];
  uint8_t ipad[64];
  uint8_t opad[64];
  uint8_t inner[32];
  ra_sha256_ctx ctx;
  size_t i;

  memset(k, 0, sizeof(k));
  if (keylen > sizeof(k))
    ra_sha256(key, keylen, k);
  else
    memcpy(k, key, keylen);
  for (i = 0; i < sizeof(k); ++i) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36);
    opad[i] = (uint8_t)(k[i] ^ 0x5C);
  }

  ra_sha256_init(&ctx);
  ra_sha256_update(&ctx, ipad, sizeof(ipad));
  ra_sha256_update(&ctx, first, first_length);
  if (second != NULL)
    ra_sha256_update(&ctx, second, second_length);
  ra_sha256_final(&ctx, inner);

  ra_sha256_init(&ctx);
  ra_sha256_update(&ctx, opad, sizeof(opad));
  ra_sha256_update(&ctx, inner, sizeof(inner));
  ra_sha256_final(&ctx, out);
}

static void hex_encode(const uint8_t *data, size_t length, char *output,
            size_t capacity) {
  static const char digits[] = "0123456789abcdef";
  size_t i;

  if (output == NULL || capacity == 0)
    return;
  if (length * 2 + 1 > capacity)
    length = (capacity - 1) / 2;
  for (i = 0; i < length; ++i) {
    output[i * 2] = digits[data[i] >> 4];
    output[i * 2 + 1] = digits[data[i] & 0x0F];
  }
  output[length * 2] = '\0';
}

static int text_signature(const char *text, size_t length, char *output,
            size_t capacity) {
  uint8_t mac[32];

  if (text == NULL || output == NULL || capacity < 65)
    return 0;
  ra_hmac_sha256(RA_GATE_KEY, sizeof(RA_GATE_KEY), (const uint8_t*)text,
        length, mac);
  hex_encode(mac, sizeof(mac), output, capacity);
  return 1;
}

static int text_verify(const char *text) {
  const char *line;
  const char *marker = NULL;
  char expected[65];
  size_t prefix_length;

  if (text == NULL)
    return 0;
  line = text;
  while (line != NULL && *line != '\0') {
    if (strncmp(line, "signature=", strlen("signature=")) == 0) {
      marker = line;
      break;
    }
    line = strchr(line, '\n');
    if (line != NULL)
      ++line;
  }
  if (marker == NULL)
    return 0;
  prefix_length = (size_t)(marker - text);
  if (strlen(marker + strlen("signature=")) < sizeof(expected) - 1)
    return 0;
  if (!text_signature(text, prefix_length, expected, sizeof(expected)))
    return 0;
  if (strncmp(marker + strlen("signature="), expected,
        sizeof(expected) - 1) != 0)
    return 0;
  marker += strlen("signature=") + sizeof(expected) - 1;
  return *marker == '\0' || *marker == '\n';
}

static int read_text_file(const char *path, char *text, size_t capacity) {
  SceUID fd;
  int length;

  if (path == NULL || text == NULL || capacity == 0)
    return 0;
  text[0] = '\0';
  fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;
  length = (int)sceIoRead(fd, text, capacity - 1);
  sceIoClose(fd);
  if (length <= 0 || (size_t)length >= capacity - 1)
    return 0;
  text[length] = '\0';
  return 1;
}

static int read_binary(const char *path, char *text, size_t capacity,
                int *found) {
  SettingsHeader header;
  uint8_t prefix[offsetof(SettingsHeader, mac)];
  uint8_t mac[32];
  SceUID fd;
  int got;

  *found = 0;
  fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;
  *found = 1;
  got = (int)sceIoRead(fd, &header, sizeof(header));
  if (got != (int)sizeof(header) || header.magic != BIN_MAGIC ||
      header.version != BIN_VERSION || header.header_size != sizeof(header) ||
      header.payload_len == 0 || header.payload_len >= capacity ||
      header.payload_len > TEXT_MAX) {
    sceIoClose(fd);
    return -1;
  }
  got = (int)sceIoRead(fd, text, header.payload_len);
  sceIoClose(fd);
  if (got != (int)header.payload_len)
    return -1;
  text[header.payload_len] = '\0';
  {
    uint32_t stored_crc = header.crc;
    header.crc = 0;
    memcpy(prefix, &header, offsetof(SettingsHeader, mac));
    if (stored_crc != crc_parts(prefix, sizeof(prefix),
                      (const uint8_t*)text, header.payload_len))
      return -1;
    header.crc = stored_crc;
  }
  memcpy(prefix, &header, offsetof(SettingsHeader, mac));
  hmac_parts(RA_GATE_KEY, sizeof(RA_GATE_KEY), prefix, sizeof(prefix),
        (const uint8_t*)text, header.payload_len, mac);
  return memcmp(header.mac, mac, sizeof(header.mac)) == 0 ? 1 : -1;
}

static int write_segments_atomic(const char *path, const void *first,
                size_t first_length, const void *second,
                size_t second_length) {
  char temporary[192];
  SceUID fd;

  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  fd = sceIoOpen(temporary, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return -1;
  if (first_length > 0 &&
      sceIoWrite(fd, first, (SceSize)first_length) != (int)first_length) {
    sceIoClose(fd);
    sceIoRemove(temporary);
    return -1;
  }
  if (second_length > 0 &&
      sceIoWrite(fd, second, (SceSize)second_length) != (int)second_length) {
    sceIoClose(fd);
    sceIoRemove(temporary);
    return -1;
  }
  sceIoClose(fd);
  if (sceIoRename(temporary, path) < 0) {
    sceIoRemove(path);
    if (sceIoRename(temporary, path) < 0) {
      sceIoRemove(temporary);
      return -1;
    }
  }
  return 0;
}

static int write_binary(const char *text, size_t length) {
  SettingsHeader header;
  uint8_t prefix[offsetof(SettingsHeader, mac)];
  uint8_t mac[32];

  if (length == 0 || length > TEXT_MAX)
    return -1;
  memset(&header, 0, sizeof(header));
  header.magic = BIN_MAGIC;
  header.version = BIN_VERSION;
  header.header_size = (uint16_t)sizeof(header);
  header.payload_len = (uint32_t)length;
  memcpy(prefix, &header, sizeof(prefix));
  header.crc = crc_parts(prefix, sizeof(prefix), (const uint8_t*)text, length);
  memcpy(prefix, &header, sizeof(prefix));
  hmac_parts(RA_GATE_KEY, sizeof(RA_GATE_KEY), prefix, sizeof(prefix),
        (const uint8_t*)text, length, mac);
  memcpy(header.mac, mac, sizeof(header.mac));
  if (write_segments_atomic(RA_SETTINGS_BIN_UR0, &header, sizeof(header),
              text, length) == 0)
    return 0;
  return write_segments_atomic(RA_SETTINGS_BIN_UX0, &header, sizeof(header),
                text, length);
}


static int policy_int(const char *text, const char *key, int fallback) {
  char value[32];

  if (!ra_settings_policy_value(text, key, value, sizeof(value)) || value[0] == '\0')
    return fallback;
  return atoi(value);
}

static uint8_t clamp_u8(int value, int minimum, int maximum) {
  if (value < minimum)
    return (uint8_t)minimum;
  if (value > maximum)
    return (uint8_t)maximum;
  return (uint8_t)value;
}


int ra_settings_logging_pref(void) {
  char text[1024];
  const char *at;

  if (!read_text_file(RA_SETTINGS_POLICY_PATH, text, sizeof(text)))
    return 0;
  at = strstr(text, "logging=");
  return at != NULL && at[8] == '1' ? 1 : 0;
}

static int settings_read_source(char *text, size_t capacity) {
  int found = 0;
  int signed_mirror;
  int result;

  result = read_binary(RA_SETTINGS_BIN_UR0, text, capacity, &found);
  if (result < 0) {
    g_auth_state = RA_SETTINGS_AUTH_INVALID;
    return 0;
  }
  if (!found) {
    result = read_binary(RA_SETTINGS_BIN_UX0, text, capacity, &found);
    if (result < 0) {
      g_auth_state = RA_SETTINGS_AUTH_INVALID;
      return 0;
    }
  }
  if (!found) {
    if (!read_text_file(RA_SETTINGS_POLICY_PATH, text, capacity))
      return 0;
    signed_mirror = strstr(text, "signature=") != NULL;
    if (signed_mirror && !text_verify(text)) {
      g_auth_state = RA_SETTINGS_AUTH_INVALID;
      return 0;
    }
    g_auth_state = signed_mirror ? RA_SETTINGS_AUTH_VALID
                  : RA_SETTINGS_AUTH_LEGACY;
  } else {
    g_auth_state = RA_SETTINGS_AUTH_VALID;
  }
  return 1;
}

static int settings_apply_core(RaSettings *settings, const char *text) {
  int old_unlock_sound = policy_int(text, "unlock_sound", -1);

  settings->ra_enabled = policy_int(text, "ra_enabled", 0) != 0;
  settings->hardcore = policy_int(text, "hardcore_pref",
        policy_int(text, "hardcore", 0)) != 0;
  settings->notifications = policy_int(text, "notifications",
        policy_int(text, "visibility_unlock", 1)) != 0;
  settings->next_achievement = clamp_u8(policy_int(text,
        "next_achievement", 0), 0, 2);
  settings->leaderboards = clamp_u8(policy_int(text, "leaderboards", 3), 0, 3);
  settings->badges = policy_int(text, "badges", 1) != 0;
  if (policy_int(text, "version", 0) >= 8)
    settings->badge_prefetch = policy_int(text, "badge_prefetch", 1) != 0;
  else
    settings->badge_prefetch = 1;
  settings->test_unofficial = policy_int(text, "test_unofficial", 0) != 0;
  if (policy_int(text, "version", 0) >= 5)
    settings->auto_screenshot = policy_int(text, "auto_screenshot", 0) != 0;
  else
    settings->auto_screenshot = 0;
  settings->spectator = policy_int(text, "spectator", 0) != 0;
  settings->encore = policy_int(text, "encore", 0) != 0;
  return old_unlock_sound;
}

static void settings_apply_sound(RaSettings *settings, const char *text,
    int old_unlock_sound) {
  settings->sound_effects = policy_int(text, "sound_effects",
        old_unlock_sound >= 0 ? old_unlock_sound : 1) != 0;
  settings->sound_message = clamp_u8(policy_int(text, "sound_message", 1), 0, 2);
  settings->sound_unlock = clamp_u8(policy_int(text, "sound_unlock",
        old_unlock_sound >= 0 ? old_unlock_sound : 1), 0, 2);
  settings->sound_lboard_submit = clamp_u8(policy_int(text,
        "sound_lboard_submit", 1), 0, 2);
}

static void settings_apply_appearance(RaSettings *settings, const char *text) {
  settings->appearance_anchor = clamp_u8(policy_int(text,
        "appearance_anchor", 2), 0, 5);
  settings->indicator_anchor = clamp_u8(policy_int(text,
        "indicator_anchor", 5), 0, 5);
  settings->indicator_size = clamp_u8(policy_int(text, "indicator_size", 1), 0, 2);
  settings->appearance_padding_auto =
    policy_int(text, "appearance_padding_auto", 1) != 0;
  settings->appearance_padding_h = clamp_u8(policy_int(text,
        "appearance_padding_h", 2), 0, 4);
  settings->appearance_padding_v = clamp_u8(policy_int(text,
        "appearance_padding_v", 2), 0, 4);
}

static void settings_apply_visibility(RaSettings *settings, const char *text) {
  int policy_version;

  settings->visibility_summary = clamp_u8(policy_int(text,
        "visibility_summary", 2), 0, 2);
  settings->visibility_unlock = policy_int(text, "visibility_unlock", 1) != 0;
  settings->visibility_mastery = policy_int(text, "visibility_mastery", 1) != 0;
  settings->visibility_challenge = policy_int(text, "visibility_challenge", 1) != 0;
  policy_version = policy_int(text, "version", 0);
  if (policy_version >= 5) {
    settings->visibility_progress = policy_int(text, "visibility_progress", 1) != 0;
    settings->visibility_lboard_trackers =
      policy_int(text, "visibility_lboard_trackers", 1) != 0;
    settings->indicator_values = policy_int(text, "indicator_values", 0) != 0;
  } else {
    settings->visibility_progress = 1;
    settings->visibility_lboard_trackers = 1;
    settings->indicator_values = 0;
  }
  settings->visibility_lboard_start =
    policy_int(text, "visibility_lboard_start", 1) != 0;
  settings->visibility_lboard_submit =
    policy_int(text, "visibility_lboard_submit", 1) != 0;
  settings->visibility_lboard_cancel =
    policy_int(text, "visibility_lboard_cancel", 1) != 0;
  settings->visibility_login = policy_int(text, "visibility_login",
        policy_int(text, "visibility_account", 1)) != 0;
  settings->visibility_verbose = policy_int(text, "visibility_verbose", 0) != 0;
  settings->logging = policy_int(text, "logging", 0) != 0;
}

void ra_settings_load(RaSettings *settings) {
  char text[TEXT_MAX + 1];
  int old_unlock_sound;

  if (settings == NULL)
    return;
  ra_settings_defaults(settings);
  g_auth_state = RA_SETTINGS_AUTH_DEFAULTS;
  ensure_dir("ur0:data");
  ensure_dir("ur0:data/AdrenalineRA");
  ensure_dir("ux0:data");
  ensure_dir(RA_SETTINGS_DIR);

  if (!settings_read_source(text, sizeof(text)))
    return;

  old_unlock_sound = settings_apply_core(settings, text);
  settings_apply_sound(settings, text, old_unlock_sound);
  settings_apply_appearance(settings, text);
  settings_apply_visibility(settings, text);
}

RaSettingsAuthState ra_settings_auth_state(void) {
  return g_auth_state;
}



int ra_settings_save(const RaSettings *settings) {
  char text[SIGNED_TEXT_MAX];
  char signature[65];
  int length;
  int payload_length;

  if (settings == NULL)
    return -1;
  ensure_dir("ur0:data");
  ensure_dir("ur0:data/AdrenalineRA");
  ensure_dir("ux0:data");
  ensure_dir(RA_SETTINGS_DIR);

  length = ra_settings_format_policy(settings, text, sizeof(text));
  if (length < 0)
    return -1;
  payload_length = length;
  if (payload_length > (int)TEXT_MAX)
    return -1;
  if (!text_signature(text, (size_t)payload_length, signature, sizeof(signature)))
    return -1;
  if ((size_t)length + strlen("signature=\n") + strlen(signature) + 1 >
      sizeof(text))
    return -1;
  length += snprintf(text + length, sizeof(text) - (size_t)length,
          "signature=%s\n", signature);
  if (length < 0 || length >= (int)sizeof(text))
    return -1;
  if (write_binary(text, (size_t)payload_length) != 0)
    return -1;
  if (!ra_settings_write_policy_mirror(text))
    return -1;
  g_auth_state = RA_SETTINGS_AUTH_VALID;
  return 0;
}


int ra_settings_copy_to(const char *destination) {
  unsigned char data[1024];
  SceUID input;
  SceUID output;
  int got;
  int result = 0;

  if (destination == NULL || destination[0] == '\0')
    return -1;
  input = sceIoOpen(RA_SETTINGS_POLICY_PATH, SCE_O_RDONLY, 0);
  if (input < 0)
    return -1;
  output = sceIoOpen(destination, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
          0777);
  if (output < 0) {
    sceIoClose(input);
    return -1;
  }
  while ((got = (int)sceIoRead(input, data, sizeof(data))) > 0) {
    if (sceIoWrite(output, data, (SceSize)got) != got) {
      result = -1;
      break;
    }
  }
  if (got < 0)
    result = -1;
  sceIoClose(output);
  sceIoClose(input);
  return result;
}

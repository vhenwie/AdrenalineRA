#ifndef RA_SETTINGS_POLICY_H
#define RA_SETTINGS_POLICY_H

#include <stddef.h>

#include "ra_settings.h"

/* Reads one `key=value` from the policy text. */
int ra_settings_policy_value(const char *text, const char *key, char *output,
                             size_t capacity);

/* Formats the signed policy body and writes the RA_POLICY.TXT mirror. */
int ra_settings_format_policy(const RaSettings *settings, char *text,
                              size_t capacity);
int ra_settings_write_policy_mirror(const char *text);

#endif /* RA_SETTINGS_POLICY_H */

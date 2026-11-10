#ifndef RA_RUNTIME_PLATFORM_H
#define RA_RUNTIME_PLATFORM_H

void ra_runtime_apply_submission_policy(void);
void ra_runtime_push_up_banner(void);
void ra_runtime_load_settings(void);
int ra_runtime_settings_dirty(void);
void ra_runtime_mark_settings_dirty(void);
void ra_runtime_clear_settings_dirty(void);
void ra_runtime_apply_client_settings(void);
int ra_runtime_downgrade_platform(void);
void ra_runtime_set_unsupported(void);

#endif

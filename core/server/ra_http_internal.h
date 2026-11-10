#ifndef RA_HTTP_INTERNAL_H
#define RA_HTTP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/net/http.h>
#include <psp2/kernel/threadmgr.h>

#include "ra_http.h"

#define RA_HTTP_UA_PROD "Adrenaline/8.0.2 (PS Vita)"
extern char g_user_agent[128];
extern SceUID g_http_gate;
extern int g_http_template;
extern int g_http_image_template;
extern volatile int g_http_image_active;
extern volatile int g_http_needs_reset;
extern volatile int g_http_session_active;
extern SceUID g_http_mutex;
extern SceUID g_http_image_mutex;
extern int g_http_last_stage;
extern int g_http_last_error;
extern int g_http_last_status;
extern int g_http_last_net_state;
extern int g_http_ssl_error;
extern unsigned int g_http_ssl_detail;
extern size_t g_http_last_body_length;
extern size_t g_http_last_content_length;
extern char g_http_last_body_prefix[97];
extern char g_http_last_body_tail[225];
extern char g_http_last_url[257];
extern char g_http_last_request[33];
extern char g_http_last_request_body[192];
extern char g_http_last_etag[96];
extern char g_http_last_modified[48];

void http_lock(void);
void http_unlock(void);
void http_image_lock(void);
void http_image_unlock(void);

void ra_http_gate_lock(void);
void ra_http_gate_unlock(void);
void ra_http_gate_api_wanted(int wanted);
int ra_http_gate_wanted(void);
/* Gate-held variants: the caller must hold g_http_gate. */
int ra_http_init_locked(void);

#endif

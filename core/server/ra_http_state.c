#include "ra_http_internal.h"
#include "ra_http.h"

#include <string.h>

#include <psp2/kernel/threadmgr.h>

#define RA_HTTP_UA_PROD "Adrenaline/8.0.2 (PS Vita)"

char g_user_agent[128] = RA_HTTP_UA_PROD;
volatile int g_http_needs_reset;
int g_http_template = -1;
int g_http_image_template = -1;
SceUID g_http_image_mutex = -1;
volatile int g_http_image_active;
int g_http_last_stage;
int g_http_last_error;
int g_http_last_status;
int g_http_last_net_state = -1;
int g_http_ssl_error = -1;
unsigned int g_http_ssl_detail;
size_t g_http_last_body_length;
size_t g_http_last_content_length;
char g_http_last_body_prefix[97];
char g_http_last_body_tail[225];
char g_http_last_url[257];
char g_http_last_request[33];
char g_http_last_request_body[192];
char g_http_last_etag[96];
char g_http_last_modified[48];
SceUID g_http_mutex = -1;
volatile int g_http_session_active;
SceUID g_http_gate = -1;

const char *ra_http_get_user_agent(void) { return g_user_agent; }
void ra_http_set_user_agent(const char *user_agent) { (void)user_agent; }
void ra_http_reset_user_agent(int is_ps1) { (void)is_ps1; }

void http_lock(void) {
  if (g_http_mutex < 0)
    g_http_mutex = sceKernelCreateMutex("ra_http", 0, 0, NULL);
  if (g_http_mutex >= 0)
    sceKernelLockMutex(g_http_mutex, 1, NULL);
}

void http_unlock(void) {
  if (g_http_mutex >= 0)
    sceKernelUnlockMutex(g_http_mutex, 1);
}

void http_image_lock(void) {
  if (g_http_image_mutex < 0)
    g_http_image_mutex = sceKernelCreateMutex("ra_http_image", 0, 0, NULL);
  if (g_http_image_mutex >= 0)
    sceKernelLockMutex(g_http_image_mutex, 1, NULL);
}

void http_image_unlock(void) {
  if (g_http_image_mutex >= 0)
    sceKernelUnlockMutex(g_http_image_mutex, 1);
}

#ifndef RA_RUNTIME_DIAG_H
#define RA_RUNTIME_DIAG_H

void ra_runtime_client_log(const char *message, void *userdata);
void ra_runtime_request_timing(const char *request, unsigned int ms, int http_status);
void ra_runtime_award_reported(unsigned int achievement_id, int hardcore,
                               int confirmed);

#endif

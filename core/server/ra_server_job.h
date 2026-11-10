#ifndef RA_SERVER_JOB_H
#define RA_SERVER_JOB_H

#include "ra_server.h"

ra_server_transport_t ra_server_transport(void);
ra_server_parse_hook_t ra_server_parse_before(void);
ra_server_parse_hook_t ra_server_parse_after(void);
ra_server_timing_reporter_t ra_server_timing_reporter(void);
ra_server_award_reporter_t ra_server_award_reporter(void);
int ra_server_leaderboard_blocked(void);
ra_server_leaderboard_validator_t ra_server_leaderboard_validator(void);
void ra_server_note_blocked_lb(void);
void ra_server_note_rejected_lb(void);
int ra_server_platform_warning(void);
void ra_server_platform_note_warning(const rc_api_server_response_t *response);

#endif

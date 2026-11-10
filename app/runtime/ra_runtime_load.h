#ifndef RA_RUNTIME_LOAD_H
#define RA_RUNTIME_LOAD_H

/* Rebuilds the achievement/leaderboard/subset snapshot from the live client. */
void ra_runtime_refresh(void);

/* Load-window timing: the parse start/end marks carried on the heartbeat. */
void ra_runtime_parse_hold(void);
void ra_runtime_parse_release(void);

/* One reload attempt when the session came up with achievements disabled because
 * memory was unreadable during the parse. */
int ra_runtime_reload_for_disabled_achievements(void);

/* Badge warm-up: one artwork request at a time, only while nothing else runs. */
void ra_runtime_warm_begin(void);
void ra_runtime_warm_tick(void);

#endif /* RA_RUNTIME_LOAD_H */

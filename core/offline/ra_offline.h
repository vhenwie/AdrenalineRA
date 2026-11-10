#ifndef RA_OFFLINE_H
#define RA_OFFLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Crash-safe queue for submissions that never reached the server. */
#define RA_OFFLINE_PATH_UR0 "ur0:data/AdrenalineRA/pending.dat"
#define RA_OFFLINE_PATH_UX0 "ux0:data/AdrenalineRA/pending.dat"
#define RA_OFFLINE_MAX_ENTRIES 8
/* Gap between replayed records, in ms. RA timestamps an award with the server's clock, so
 * records posted in one burst share a timestamp and lose their order. */
#define RA_OFFLINE_REPLAY_GAP_MS 1100
#define RA_OFFLINE_MAX_URL 256
#define RA_OFFLINE_MAX_BODY 1024

/* Stores one undelivered request. Returns 1 when the record was written. */
int ra_offline_queue_add(const char *url, const char *post_data);

/* Supplies the current username/token for replay. The queue stores no token. */
typedef int (*ra_offline_credentials_t)(char *username, size_t username_capacity,
                                        char *token, size_t token_capacity);
void ra_offline_set_credentials_provider(ra_offline_credentials_t provider);

/* Sends every stored record, newest last, and rewrites the file with the ones that
 * still have to be sent (a transport failure stops the replay). Returns the number
 * of records the server accepted. */
int ra_offline_queue_replay(void);

/* Records currently on the card; 0 when the file does not exist. Reads the card - call it
 * from session start or diagnostics, never from a per-frame path. */
int ra_offline_queue_depth(void);

/* The same count without touching the card : cached from the last read and kept
 * current by add/replay. This is what the runtime's per-tick check uses. */
int ra_offline_queue_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_OFFLINE_H */

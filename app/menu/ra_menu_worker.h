#ifndef RA_MENU_WORKER_H
#define RA_MENU_WORKER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RA_JOB_NONE = 0,
    RA_JOB_SIGN_IN,
    RA_JOB_VERIFY_NETWORK,
    RA_JOB_VERIFY_ACCOUNT
} RaJobKind;

typedef enum {
    RA_JOB_IDLE = 0,
    RA_JOB_RUNNING,
    RA_JOB_DONE_OK,
    RA_JOB_DONE_ERROR
} RaJobState;

typedef struct {
    RaJobState state;
    RaJobKind kind;
    char stage[96];
    char message[192];
    char username[64];
    unsigned int score;
    int score_valid;
    unsigned long long finished_us;
} RaJobSnapshot;

int ra_worker_start_signin(const char *username, const char *password);
int ra_worker_start_verify_network(void);
int ra_worker_start_verify_account(void);

void ra_worker_shutdown(void);
void ra_worker_snapshot(RaJobSnapshot *snapshot);
void ra_worker_consume(void);
int ra_worker_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_MENU_WORKER_H */

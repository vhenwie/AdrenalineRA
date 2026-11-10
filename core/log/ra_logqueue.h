#ifndef RA_LOGQUEUE_H
#define RA_LOGQUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* One log writer for the whole module: a ring drained by a worker. */
#define RA_LOGQ_LINE 320

/* Queues one line. `length` <= 0 is ignored, and a line that does not fit is
 * truncated - always with a newline at the cut, so the next queued line cannot be
 * glued onto it. (It was: `a log` L83 reads
 * `... lboard=struntime: slow loop ...` because the 277-character settings line was cut at
 * 207 and lost its `\n`.) */
void ra_logq_line(const char *line, int length);

/* printf into a stack buffer, then queue. Never blocks on I/O. */
void ra_logq_printf(const char *format, ...);

/* Drains everything queued right now, on the caller's thread. Only for shutdown, when
 * the worker is about to disappear with pending lines. */
void ra_logq_flush(void);

/* Writes one line immediately, bypassing the ring. Only for the exit path: a line that
 * must be on the card before `ScePspemuErrorExit()` cannot wait for the worker's next
 * drain. */
void ra_logq_direct(const char *line);

/* Queue a line and drain immediately on the caller's thread; crash brackets use this. */
void ra_logq_printf_sync(const char *format, ...);

/* Lines dropped because the ring was full since startup; diagnostics only. */
unsigned int ra_logq_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* RA_LOGQUEUE_H */

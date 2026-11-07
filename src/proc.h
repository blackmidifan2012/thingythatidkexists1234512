#pragma once
/* Event-loop integrated timers, child processes and worker jobs. */
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ---- timers (wl_event_loop based). Callback runs on the main thread. */
typedef struct Timer Timer;
Timer *timer_add(int ms, bool repeat, void (*cb)(void *), void *ud);
void timer_set(Timer *t, int ms);          /* re-arm; 0 disarms */
void timer_free(Timer *t);

/* ---- child processes */
typedef void (*ProcDone)(int status, char *out, size_t len, void *ud);
/* Runs argv, captures stdout, calls done on the main thread when it exits.
 * `out` is NUL-terminated and owned by the caller of done (freed afterwards). */
bool proc_run(char *const argv[], ProcDone done, void *ud);
bool proc_run_sh(const char *cmd, ProcDone done, void *ud);
/* Detached launch (apps). Sets up the Wayland session environment. */
bool proc_spawn(char *const argv[]);
bool proc_spawn_sh(const char *cmd);
bool proc_exists(const char *prog);        /* program found in $PATH */

/* ---- worker threads for blocking work such as image decoding */
typedef void (*JobFn)(void *arg);          /* runs on a worker thread */
typedef void (*JobDone)(void *arg);        /* runs on the main thread */
void jobs_init(void);
void job_submit(JobFn fn, JobDone done, void *arg);
void jobs_shutdown(void);

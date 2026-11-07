#pragma once
/* Background audio playback shared by Music, Voice Recorder and the shell's media
 * controls (volume HUD, lock screen). Plays through an external player process so the
 * compositor never links a media stack. */
#include <stdbool.h>
#include <sys/types.h>

typedef enum { PL_STOPPED, PL_PLAYING, PL_PAUSED } PlayerState;

bool player_available(void);                 /* a usable backend program exists */
bool player_play(const char *path);          /* start a file from the beginning */
void player_pause(bool pause);
void player_toggle(void);
void player_stop(void);
void player_seek(double sec);                /* absolute position */
PlayerState player_state(void);
double player_position(void);                /* seconds */
double player_duration(void);                /* seconds, 0 when unknown */
const char *player_path(void);               /* current file or NULL */
/* Metadata for the shell's media controls (set by whoever starts playback). */
void player_set_meta(const char *title, const char *artist);
const char *player_title(void);
const char *player_artist(void);
/* Called when the current file finishes; used by Music to advance its queue. */
void player_on_finished(void (*cb)(void *ud), void *ud);
/* next/previous hooks for the shell's media controls (NULL = hidden) */
void player_set_queue_hooks(void (*next)(void), void (*prev)(void));
void player_next(void);
void player_prev(void);
bool player_has_queue(void);

/* ---- extensions
 * player_play() resets meta, hooks, owner, duration and the finished callback; the caller
 * sets them again right after starting a file. */
bool player_can_play(const char *path);      /* some backend handles this file type */
bool player_can_seek(void);
/* Loads a file in the paused state at `pos` without starting a process (resume after a
 * restart). player_pause(false) / player_toggle() starts it. */
void player_cue(const char *path, double pos);
void player_set_duration(double sec);        /* known length when the backend can't tell */
void player_set_owner(const char *app_id);   /* "music", "recorder" ... */
const char *player_owner(void);              /* "" when nobody claimed the playback */
bool player_active(void);                    /* a file is loaded (playing or paused) */
/* Call while drawing anything that shows the playback position: the poll timer then
 * repaints the screen a few times per second (and stops when nobody is watching). */
void player_watch(void);
/* fork/exec for audio helper processes (also used by the recorder): default signal
 * handling, /dev/null stdio, no inherited fds, dies with the compositor. -1 on error. */
pid_t audio_spawn(char *const argv[]);

/* Shared audio player engine.
 *
 * Playback runs in an external program; nothing media related is linked into the
 * compositor. mpv is preferred and driven over its JSON IPC socket (pause, seek, position,
 * duration). Simpler players (ffplay, mpg123, ogg123, pw-play, paplay, aplay) are paused
 * with SIGSTOP/SIGCONT and their position is derived from the wall clock; seeking restarts
 * them with a start offset where the program supports one. A poll timer runs only while a
 * process exists; it reaps the child (natural end -> finished callback) and keeps the UI
 * that shows the position fresh. */
#include "player.h"
#include "../i18n.h"
#include "../proc.h"
#include "../shell/shell.h"
#include "../util.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { BK_NONE, BK_MPV, BK_FFPLAY, BK_MPG123, BK_OGG123, BK_PWPLAY, BK_PAPLAY, BK_APLAY, BK_COUNT } Backend;

static const char *const BK_PROG[BK_COUNT] = {NULL, "mpv", "ffplay", "mpg123", "ogg123", "pw-play", "paplay", "aplay"};

static struct {
	Backend bk;
	pid_t pid;
	PlayerState st;
	char *path;
	char title[256], artist[256], owner[32];
	double dur;              /* best known duration (backend or caller) */
	double base, t0;         /* position = base + (now - t0) while playing */
	double spawn_t, paused_t;
	double watch_t;
	/* mpv IPC */
	int sock;
	char sockpath[108];
	char rbuf[4096];
	size_t rlen;
	double pending_seek;     /* sent once the socket connects (-1 = none) */
	bool pending_pause;
	Timer *poll;
	int poll_ms;
	void (*fin)(void *);
	void *fin_ud;
	void (*next)(void), (*prev)(void);
} P = {.sock = -1, .pending_seek = -1};

/* processes that were asked to quit and still need reaping */
static struct { pid_t pid; double t; } g_dying[8];
static int g_ndying;
static Timer *g_reaper;
static bool g_atexit;
static signed char g_have[BK_COUNT];
static double g_have_t;

/* ---------------------------------------------------------------- helpers */

static bool have(Backend b) {
	double now = now_sec();
	if (now - g_have_t > 20) { memset(g_have, -1, sizeof g_have); g_have_t = now; }
	if (g_have[b] < 0) g_have[b] = proc_exists(BK_PROG[b]) ? 1 : 0;
	return g_have[b] == 1;
}

static const char *ext_of(const char *path) {
	const char *b = path_basename(path), *d = strrchr(b, '.');
	return d ? d + 1 : "";
}

static bool ext_in(const char *ext, const char *list) {
	char e[12];
	size_t n = strlen(ext);
	if (!n || n >= sizeof e) return false;
	for (size_t i = 0; i <= n; i++) e[i] = (char)(ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + 32 : ext[i]);
	for (const char *p = list; *p;) {
		const char *s = strchr(p, ' ');
		size_t l = s ? (size_t)(s - p) : strlen(p);
		if (l == n && !memcmp(p, e, n)) return true;
		p += l + (s ? 1 : 0);
	}
	return false;
}

static bool supports(Backend b, const char *path) {
	const char *e = ext_of(path);
	switch (b) {
	case BK_MPV: case BK_FFPLAY: return true;
	case BK_MPG123: return ext_in(e, "mp3 mp2 mpga");
	case BK_OGG123: return ext_in(e, "ogg oga flac opus");
	case BK_PWPLAY: case BK_PAPLAY: return ext_in(e, "wav flac ogg oga aiff aif au");
	case BK_APLAY: return ext_in(e, "wav");
	default: return false;
	}
}

static Backend pick(const char *path) {
	for (int b = BK_MPV; b < BK_COUNT; b++)
		if (have((Backend)b) && supports((Backend)b, path)) return (Backend)b;
	return BK_NONE;
}

static bool can_seek(Backend b) { return b == BK_MPV || b == BK_FFPLAY || b == BK_MPG123 || b == BK_OGG123; }

pid_t audio_spawn(char *const argv[]) {
	pid_t parent = getpid();
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		/* the compositor blocks SIGINT/SIGTERM for its signalfd; children need them back */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent) _exit(0);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		for (int fd = 3; fd < 1024; fd++) close(fd);
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static void reaper_tick(void *ud) {
	double now = now_sec();
	for (int i = 0; i < g_ndying;) {
		int st;
		pid_t r = waitpid(g_dying[i].pid, &st, WNOHANG);
		if (r == g_dying[i].pid || (r < 0 && errno == ECHILD)) {
			g_dying[i] = g_dying[--g_ndying];
			continue;
		}
		if (now - g_dying[i].t > 1.5) kill(g_dying[i].pid, SIGKILL);
		i++;
	}
	if (!g_ndying) timer_set(g_reaper, 0);
}

/* Asks a child to quit without blocking the compositor; it is reaped in the background. */
static void dispose(pid_t pid) {
	if (pid <= 0) return;
	kill(pid, SIGTERM);
	kill(pid, SIGCONT);
	int st;
	if (waitpid(pid, &st, WNOHANG) == pid) return;
	if (g_ndying == (int)ARRAY_LEN(g_dying)) {   /* list full: be blunt */
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}
	g_dying[g_ndying].pid = pid;
	g_dying[g_ndying++].t = now_sec();
	if (!g_reaper) g_reaper = timer_add(100, true, reaper_tick, NULL);
	else timer_set(g_reaper, 100);
}

static void at_exit(void) {
	if (P.pid > 0) { kill(P.pid, SIGKILL); kill(P.pid, SIGCONT); }
	for (int i = 0; i < g_ndying; i++) kill(g_dying[i].pid, SIGKILL);
	if (P.sockpath[0]) unlink(P.sockpath);
}

static void ipc_close(void) {
	if (P.sock >= 0) close(P.sock);
	P.sock = -1;
	P.rlen = 0;
	if (P.sockpath[0]) unlink(P.sockpath);
}

static void kill_child(void) {
	ipc_close();
	if (P.pid > 0) dispose(P.pid);
	P.pid = 0;
}

static void poll_tick(void *ud);

static void poll_arm(int ms) {
	if (!P.poll) P.poll = timer_add(ms, true, poll_tick, NULL);
	else if (P.poll_ms != ms) timer_set(P.poll, ms);
	P.poll_ms = ms;
}

static void poll_disarm(void) {
	if (P.poll && P.poll_ms) timer_set(P.poll, 0);
	P.poll_ms = 0;
}

static double now_pos(void) {
	double p = P.base;
	if (P.st == PL_PLAYING) p += now_sec() - P.t0;
	if (P.dur > 0 && p > P.dur) p = P.dur;
	return MAX(p, 0);
}

/* --------------------------------------------------------------- mpv IPC */

static void ipc_send(const char *json) {
	if (P.sock < 0) return;
	size_t n = strlen(json);
	ssize_t w = send(P.sock, json, n, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ipc_close();
}

static void ipc_cmd_pause(bool pause) {
	ipc_send(pause ? "{\"command\":[\"set_property\",\"pause\",true]}\n" : "{\"command\":[\"set_property\",\"pause\",false]}\n");
}

static void ipc_cmd_seek(double sec) {
	char b[96];
	snprintf(b, sizeof b, "{\"command\":[\"seek\",%.3f,\"absolute\"]}\n", sec);
	ipc_send(b);
}

static void ipc_connect(void) {
	if (P.sock >= 0 || !P.sockpath[0]) return;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return;
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	str_copy(a.sun_path, sizeof a.sun_path, P.sockpath);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return; }
	P.sock = fd;
	P.rlen = 0;
	if (P.pending_seek >= 0) { ipc_cmd_seek(P.pending_seek); P.pending_seek = -1; }
	if (P.pending_pause) ipc_cmd_pause(true);
}

static void ipc_line(const char *line) {
	cJSON *j = cJSON_Parse(line);
	if (!j) return;
	cJSON *rid = cJSON_GetObjectItem(j, "request_id"), *err = cJSON_GetObjectItem(j, "error"),
	      *data = cJSON_GetObjectItem(j, "data");
	if (cJSON_IsNumber(rid) && cJSON_IsString(err) && str_eq(err->valuestring, "success") && cJSON_IsNumber(data)) {
		double v = data->valuedouble;
		if (rid->valueint == 1 && v >= 0) {
			P.base = v;
			P.t0 = now_sec();
		} else if (rid->valueint == 2 && v > 0) {
			P.dur = v;
		}
	}
	cJSON_Delete(j);
}

static void ipc_read(void) {
	while (P.sock >= 0) {
		if (P.rlen >= sizeof P.rbuf - 1) P.rlen = 0;   /* overlong line: drop it */
		ssize_t n = recv(P.sock, P.rbuf + P.rlen, sizeof P.rbuf - 1 - P.rlen, MSG_DONTWAIT);
		if (n == 0) { ipc_close(); return; }
		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) ipc_close();
			return;
		}
		P.rlen += (size_t)n;
		P.rbuf[P.rlen] = 0;
		char *s = P.rbuf, *nl;
		while ((nl = strchr(s, '\n'))) {
			*nl = 0;
			ipc_line(s);
			s = nl + 1;
		}
		P.rlen = strlen(s);
		memmove(P.rbuf, s, P.rlen + 1);
	}
}

/* ------------------------------------------------------------ processes */

static bool start_child(double offset) {
	Backend b = pick(P.path);
	if (b == BK_NONE) return false;
	char off[32] = "", arg[160] = "";
	char *argv[16];
	int n = 0;
	argv[n++] = (char *)BK_PROG[b];
	P.sockpath[0] = 0;
	switch (b) {
	case BK_MPV: {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		snprintf(P.sockpath, sizeof P.sockpath, "%s/metrowl-mpv-%d.sock", rt && *rt ? rt : "/tmp", (int)getpid());
		unlink(P.sockpath);
		snprintf(arg, sizeof arg, "--input-ipc-server=%s", P.sockpath);
		argv[n++] = "--no-video";
		argv[n++] = "--no-terminal";
		argv[n++] = "--idle=no";
		argv[n++] = "--audio-display=no";
		argv[n++] = arg;
		if (offset > 0.5) { snprintf(off, sizeof off, "--start=%.2f", offset); argv[n++] = off; }
		argv[n++] = "--";
		break;
	}
	case BK_FFPLAY:
		argv[n++] = "-nodisp";
		argv[n++] = "-autoexit";
		argv[n++] = "-loglevel";
		argv[n++] = "quiet";
		if (offset > 0.5) { snprintf(off, sizeof off, "%.2f", offset); argv[n++] = "-ss"; argv[n++] = off; }
		break;
	case BK_MPG123:
		argv[n++] = "-q";
		/* -k skips MPEG frames: 1152 samples each, assume 44.1 kHz */
		if (offset > 0.5) { snprintf(off, sizeof off, "%ld", lround(offset * 44100.0 / 1152.0)); argv[n++] = "-k"; argv[n++] = off; }
		argv[n++] = "--";
		break;
	case BK_OGG123:
		argv[n++] = "-q";
		if (offset > 0.5) { snprintf(off, sizeof off, "%.0f", offset); argv[n++] = "-k"; argv[n++] = off; }
		break;
	case BK_APLAY:
		argv[n++] = "-q";
		argv[n++] = "--";
		break;
	default:
		break;
	}
	argv[n++] = P.path;
	argv[n] = NULL;
	pid_t pid = audio_spawn(argv);
	if (pid < 0) { P.sockpath[0] = 0; return false; }
	if (!g_atexit) { atexit(at_exit); g_atexit = true; }
	P.bk = b;
	P.pid = pid;
	P.spawn_t = now_sec();
	P.base = can_seek(b) ? offset : 0;
	P.t0 = P.spawn_t;
	P.pending_seek = -1;
	P.pending_pause = false;
	poll_arm(250);
	return true;
}

static void child_exited(int status) {
	ipc_close();
	P.pid = 0;
	poll_disarm();
	double ran = now_sec() - P.spawn_t;
	double pos = now_pos();
	bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	bool near_end = P.dur > 0 && pos >= P.dur - 3;
	if (!clean && !near_end && (ran < 3 || WIFEXITED(status))) {
		/* the program refused the file or found no audio device */
		LOGE("player: %s exited with status %d for %s", BK_PROG[P.bk], status, P.path ? P.path : "");
		P.st = PL_STOPPED;
		P.base = 0;
		shell_toast(_("Can't play this file"), P.title[0] ? P.title : path_basename(P.path ? P.path : ""), NULL);
		shell_invalidate();
		return;
	}
	P.st = PL_STOPPED;
	P.base = 0;
	shell_invalidate();
	if (P.fin) P.fin(P.fin_ud);
}

static void poll_tick(void *ud) {
	if (P.pid > 0) {
		int status;
		pid_t r = waitpid(P.pid, &status, WNOHANG);
		if (r == P.pid || (r < 0 && errno == ECHILD)) { child_exited(r == P.pid ? status : 0); return; }
		if (P.bk == BK_MPV) {
			if (P.sock < 0) ipc_connect();
			ipc_read();
			if (P.sock >= 0 && P.st == PL_PLAYING) {
				ipc_send("{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":1}\n");
				ipc_send("{\"command\":[\"get_property\",\"duration\"],\"request_id\":2}\n");
			}
		}
		/* a long pause releases the process (and the sound device); resuming restarts it */
		if (P.st == PL_PAUSED && now_sec() - P.paused_t > 300) {
			kill_child();
			poll_disarm();
			return;
		}
	} else {
		poll_disarm();
		return;
	}
	if (P.st == PL_PLAYING && now_sec() - P.watch_t < 1.5) shell_invalidate();
}

static void reset_session(void) {
	kill_child();
	poll_disarm();
	P.title[0] = P.artist[0] = P.owner[0] = 0;
	P.dur = 0;
	P.fin = NULL;
	P.fin_ud = NULL;
	P.next = P.prev = NULL;
	P.base = 0;
}

/* ------------------------------------------------------------------- API */

bool player_available(void) {
	for (int b = BK_MPV; b < BK_COUNT; b++) if (have((Backend)b)) return true;
	return false;
}

bool player_can_play(const char *path) { return path && pick(path) != BK_NONE; }

bool player_can_seek(void) {
	if (P.pid > 0) return can_seek(P.bk);
	return P.path && can_seek(pick(P.path));
}

bool player_play(const char *path) {
	reset_session();
	free(P.path);
	P.path = xstrdup(path);
	P.st = PL_STOPPED;
	shell_invalidate();
	if (!start_child(0)) return false;
	P.st = PL_PLAYING;
	return true;
}

void player_cue(const char *path, double pos) {
	reset_session();
	free(P.path);
	P.path = xstrdup(path);
	P.base = MAX(pos, 0);
	P.st = PL_PAUSED;
	P.paused_t = now_sec();
	shell_invalidate();
}

void player_pause(bool pause) {
	if (!P.path) return;
	if (pause) {
		if (P.st != PL_PLAYING) return;
		P.base = now_pos();
		P.st = PL_PAUSED;
		P.paused_t = now_sec();
		if (P.pid > 0) {
			if (P.bk == BK_MPV) {
				if (P.sock >= 0) ipc_cmd_pause(true);
				else P.pending_pause = true;
			} else {
				kill(P.pid, SIGSTOP);
			}
			poll_arm(1000);
		}
	} else {
		if (P.st == PL_PLAYING) return;
		if (P.pid > 0) {
			if (P.bk == BK_MPV) {
				P.pending_pause = false;
				ipc_cmd_pause(false);
			} else {
				kill(P.pid, SIGCONT);
			}
			poll_arm(250);
		} else {
			/* nothing running (cued, finished or released after a long pause) */
			double at = P.st == PL_PAUSED ? P.base : 0;
			if (P.dur > 0 && at >= P.dur - 0.5) at = 0;
			if (!start_child(at)) { shell_invalidate(); return; }
			if (!can_seek(P.bk)) P.base = 0;
		}
		P.t0 = now_sec();
		P.st = PL_PLAYING;
	}
	shell_invalidate();
}

void player_toggle(void) { player_pause(P.st == PL_PLAYING); }

void player_stop(void) {
	kill_child();
	poll_disarm();
	P.st = PL_STOPPED;
	P.base = 0;
	shell_invalidate();
}

void player_seek(double sec) {
	if (!P.path) return;
	if (P.dur > 0) sec = MIN(sec, P.dur - 0.25);
	sec = MAX(sec, 0);
	if (P.pid > 0 && P.bk == BK_MPV) {
		if (P.sock >= 0) ipc_cmd_seek(sec);
		else P.pending_seek = sec;
		P.base = sec;
		P.t0 = now_sec();
	} else if (P.pid > 0) {
		Backend b = P.bk;
		if (!can_seek(b) && sec > 0.5) return;
		bool playing = P.st == PL_PLAYING;
		kill_child();
		if (playing) {
			if (!start_child(sec)) { P.st = PL_STOPPED; P.base = 0; }
			else P.t0 = now_sec();
		} else {
			P.base = sec;   /* resumes from here */
			poll_disarm();
		}
	} else {
		if (P.st == PL_PAUSED || P.st == PL_STOPPED) {
			P.base = sec;
			if (P.st == PL_STOPPED) { P.st = PL_PAUSED; P.paused_t = now_sec(); }
		}
	}
	shell_invalidate();
}

PlayerState player_state(void) { return P.st; }
double player_position(void) { return P.path ? now_pos() : 0; }
double player_duration(void) { return P.dur; }
const char *player_path(void) { return P.path; }
bool player_active(void) { return P.path && P.st != PL_STOPPED; }

void player_set_duration(double sec) { if (sec > 0) P.dur = sec; }

void player_set_meta(const char *title, const char *artist) {
	str_copy(P.title, sizeof P.title, title ? title : "");
	str_copy(P.artist, sizeof P.artist, artist ? artist : "");
	shell_invalidate();
}

const char *player_title(void) {
	if (P.title[0] || !P.path) return P.title;
	return path_basename(P.path);
}

const char *player_artist(void) { return P.artist; }

void player_set_owner(const char *app_id) { str_copy(P.owner, sizeof P.owner, app_id ? app_id : ""); }
const char *player_owner(void) { return P.owner; }

void player_on_finished(void (*cb)(void *ud), void *ud) { P.fin = cb; P.fin_ud = ud; }

void player_set_queue_hooks(void (*next)(void), void (*prev)(void)) { P.next = next; P.prev = prev; }

void player_next(void) {
	if (P.next) P.next();
}

void player_prev(void) {
	if (P.prev) P.prev();
	else player_seek(0);
}

bool player_has_queue(void) { return P.next != NULL; }

void player_watch(void) { P.watch_t = now_sec(); }

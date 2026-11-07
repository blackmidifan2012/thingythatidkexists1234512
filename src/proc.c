#include "proc.h"
#include "server.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

/* ----------------------------------------------------------------- timers */

struct Timer {
	struct wl_event_source *src;
	bool repeat;
	int ms;
	void (*cb)(void *);
	void *ud;
};

static int timer_fire(void *data) {
	Timer *t = data;
	if (t->repeat && t->ms > 0) wl_event_source_timer_update(t->src, t->ms);
	t->cb(t->ud);
	return 0;
}

Timer *timer_add(int ms, bool repeat, void (*cb)(void *), void *ud) {
	Timer *t = xcalloc(1, sizeof *t);
	t->repeat = repeat;
	t->ms = ms;
	t->cb = cb;
	t->ud = ud;
	t->src = wl_event_loop_add_timer(srv.loop, timer_fire, t);
	if (ms > 0) wl_event_source_timer_update(t->src, ms);
	return t;
}

void timer_set(Timer *t, int ms) {
	if (!t) return;
	t->ms = ms;
	wl_event_source_timer_update(t->src, ms);
}

void timer_free(Timer *t) {
	if (!t) return;
	wl_event_source_remove(t->src);
	free(t);
}

/* -------------------------------------------------------------- processes */

typedef struct {
	pid_t pid;
	int fd;
	struct wl_event_source *src;
	char *buf;
	size_t len, cap;
	ProcDone done;
	void *ud;
} Proc;

static void proc_finish(Proc *p) {
	wl_event_source_remove(p->src);
	close(p->fd);
	int status = 0;
	waitpid(p->pid, &status, 0);
	p->buf[p->len] = 0;
	if (p->done) p->done(status, p->buf, p->len, p->ud);
	free(p->buf);
	free(p);
}

static int proc_readable(int fd, uint32_t mask, void *data) {
	Proc *p = data;
	for (;;) {
		if (p->len + 4096 + 1 > p->cap) {
			p->cap = p->cap ? p->cap * 2 : 8192;
			p->buf = xrealloc(p->buf, p->cap);
		}
		ssize_t n = read(fd, p->buf + p->len, p->cap - p->len - 1);
		if (n > 0) { p->len += (size_t)n; if (p->len > (64u << 20)) break; continue; }
		if (n < 0 && errno == EAGAIN) return 0;
		if (n < 0 && errno == EINTR) continue;
		break;
	}
	proc_finish(p);
	return 0;
}

static void child_env(void) {
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", "metrowl", 0);
	setenv("MOZ_ENABLE_WAYLAND", "1", 0);
	/* ask toolkits for their phone / touch layouts (Kirigami, Plasma Mobile, Qt Quick, GTK) */
	setenv("QT_QUICK_CONTROLS_MOBILE", "1", 0);
	setenv("PLASMA_PLATFORM", "phone:handset", 0);
	setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", 0);
	setenv("GTK_CSD", "0", 0);
	setenv("MOZ_USE_XINPUT2", "1", 0);
	setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
	setenv("SDL_VIDEODRIVER", "wayland,x11", 0);
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 0);
	setenv("ELM_DISPLAY", "wl", 0);
	setenv("CLUTTER_BACKEND", "wayland", 0);
}

/* fork() copies the compositor's page tables, which stalls the main thread for
 * milliseconds on a phone every time a status poll or a sound runs. posix_spawn
 * (vfork-like) does not; the fork path below stays as the fallback. */
static bool spawn_fast(char *const argv[], int out_fd, pid_t *pid) {
	static bool env_ready;
	if (!env_ready) { child_env(); env_ready = true; }   /* same variables, set once */
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawnattr_init(&at);
	sigset_t def, none;
	sigemptyset(&def);
	sigaddset(&def, SIGPIPE);
	sigemptyset(&none);
	posix_spawnattr_setsigdefault(&at, &def);
	posix_spawnattr_setsigmask(&at, &none);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	int rc = posix_spawnp(pid, argv[0], &fa, &at, argv, environ);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	return rc == 0;
}

bool proc_run(char *const argv[], ProcDone done, void *ud) {
	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) != 0) return false;
	pid_t pid;
	if (!spawn_fast(argv, pfd[1], &pid)) pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); return false; }
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		dup2(pfd[1], 1);
		dup2(nul, 2);
		signal(SIGPIPE, SIG_DFL);
		child_env();
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);
	Proc *p = xcalloc(1, sizeof *p);
	p->pid = pid;
	p->fd = pfd[0];
	p->done = done;
	p->ud = ud;
	p->src = wl_event_loop_add_fd(srv.loop, pfd[0], WL_EVENT_READABLE, proc_readable, p);
	return true;
}

bool proc_run_sh(const char *cmd, ProcDone done, void *ud) {
	char *argv[] = {"sh", "-c", (char *)cmd, NULL};
	return proc_run(argv, done, ud);
}

bool proc_spawn(char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		setsid();
		pid_t pid2 = fork();
		if (pid2 != 0) _exit(0);
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		const char *log = getenv("METROWL_APP_LOG");
		if (log) {
			int lf = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
		} else {
			dup2(nul, 1);
			dup2(nul, 2);
		}
		signal(SIGPIPE, SIG_DFL);
		child_env();
		execvp(argv[0], argv);
		_exit(127);
	}
	int st;
	waitpid(pid, &st, 0);
	return true;
}

bool proc_spawn_sh(const char *cmd) {
	char *argv[] = {"sh", "-c", (char *)cmd, NULL};
	return proc_spawn(argv);
}

bool proc_exists(const char *prog) {
	if (strchr(prog, '/')) return access(prog, X_OK) == 0;
	const char *path = getenv("PATH");
	if (!path) path = "/usr/local/bin:/usr/bin:/bin";
	char *copy = xstrdup(path), *save = NULL;
	bool found = false;
	for (char *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
		char *full = path_join(d, prog);
		found = access(full, X_OK) == 0;
		free(full);
	}
	free(copy);
	return found;
}

/* ------------------------------------------------------------------- jobs */

typedef struct Job {
	struct Job *next;
	JobFn fn;
	JobDone done;
	void *arg;
	bool finished;
} Job;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Job *g_pending_head, *g_pending_tail, *g_done_head, *g_done_tail;
static pthread_t g_threads[2];
static int g_nthreads, g_evfd = -1;
static bool g_quit;
static struct wl_event_source *g_ev_src;

static void *worker(void *arg) {
	for (;;) {
		pthread_mutex_lock(&g_mu);
		while (!g_pending_head && !g_quit) pthread_cond_wait(&g_cv, &g_mu);
		if (g_quit) { pthread_mutex_unlock(&g_mu); return NULL; }
		Job *j = g_pending_head;
		g_pending_head = j->next;
		if (!g_pending_head) g_pending_tail = NULL;
		pthread_mutex_unlock(&g_mu);
		j->fn(j->arg);
		pthread_mutex_lock(&g_mu);
		j->next = NULL;
		if (g_done_tail) g_done_tail->next = j; else g_done_head = j;
		g_done_tail = j;
		pthread_mutex_unlock(&g_mu);
		uint64_t one = 1;
		if (write(g_evfd, &one, sizeof one) < 0) { /* nothing sensible to do */ }
	}
}

static int jobs_ready(int fd, uint32_t mask, void *data) {
	uint64_t v;
	if (read(fd, &v, sizeof v) < 0) { /* spurious wakeup */ }
	for (;;) {
		pthread_mutex_lock(&g_mu);
		Job *j = g_done_head;
		if (j) {
			g_done_head = j->next;
			if (!g_done_head) g_done_tail = NULL;
		}
		pthread_mutex_unlock(&g_mu);
		if (!j) break;
		if (j->done) j->done(j->arg);
		free(j);
	}
	return 0;
}

void jobs_init(void) {
	g_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	g_ev_src = wl_event_loop_add_fd(srv.loop, g_evfd, WL_EVENT_READABLE, jobs_ready, NULL);
	g_nthreads = 2;
	for (int i = 0; i < g_nthreads; i++) pthread_create(&g_threads[i], NULL, worker, NULL);
}

void job_submit(JobFn fn, JobDone done, void *arg) {
	Job *j = xcalloc(1, sizeof *j);
	j->fn = fn;
	j->done = done;
	j->arg = arg;
	pthread_mutex_lock(&g_mu);
	if (g_pending_tail) g_pending_tail->next = j; else g_pending_head = j;
	g_pending_tail = j;
	pthread_cond_signal(&g_cv);
	pthread_mutex_unlock(&g_mu);
}

void jobs_shutdown(void) {
	pthread_mutex_lock(&g_mu);
	g_quit = true;
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	for (int i = 0; i < g_nthreads; i++) pthread_join(g_threads[i], NULL);
	g_nthreads = 0;
}

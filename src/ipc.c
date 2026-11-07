#include "server.h"
#include "apps/apps.h"
#include "config.h"
#include "i18n.h"
#include "proc.h"
#include "shell/shell.h"
#include "sys.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Line based control protocol on a unix socket: one command per connection, the
 * reply is plain text and the connection closes when the command finished.
 * See `metrowlctl help`. Used for scripting and for automated tests. */

typedef struct Client {
	struct wl_list link;
	int fd;
	struct wl_event_source *src;
	char buf[2048];
	int len;
	bool busy;            /* waiting for an asynchronous command to finish */
} Client;

static int g_listen_fd = -1;
static struct wl_event_source *g_listen_src;
static struct wl_list g_clients;
static char g_path[256];

static void client_close(Client *c);

static void reply(Client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(Client *c, const char *fmt, ...) {
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0) {
		ssize_t w = write(c->fd, buf, (size_t)MIN(n, (int)sizeof buf - 1));
		(void)w;
	}
}

static void finish(Client *c, const char *msg) {
	if (msg) reply(c, "%s\n", msg);
	client_close(c);
}

/* ------------------------------------------------------------- async ops -- */

typedef struct { Client *c; char path[512]; } ShotJob;

static bool client_alive(Client *c) {
	Client *it;
	wl_list_for_each(it, &g_clients, link) if (it == c) return true;
	return false;
}

static void shot_done(cairo_surface_t *surf, void *ud) {
	ShotJob *j = ud;
	if (client_alive(j->c)) {
		if (surf && cairo_surface_write_to_png(surf, j->path) == CAIRO_STATUS_SUCCESS)
			reply(j->c, "ok %s %dx%d\n", j->path, cairo_image_surface_get_width(surf),
			      cairo_image_surface_get_height(surf));
		else
			reply(j->c, "err screenshot failed\n");
		client_close(j->c);
	}
	free(j);
}

typedef struct {
	Client *c;
	double x1, y1, x2, y2;
	int steps, i;
	Timer *t;
	bool hold_end;
} Swipe;

static void swipe_step(void *ud) {
	Swipe *s = ud;
	double f = (double)s->i / s->steps;
	double x = s->x1 + (s->x2 - s->x1) * f, y = s->y1 + (s->y2 - s->y1) * f;
	if (s->i == 0) input_sim_pointer(0, x, y);
	else if (s->i < s->steps) input_sim_pointer(1, x, y);
	else {
		input_sim_pointer(1, s->x2, s->y2);
		input_sim_pointer(2, s->x2, s->y2);
	}
	s->i++;
	if (s->i > s->steps) {
		if (client_alive(s->c)) finish(s->c, "ok");
		timer_free(s->t);
		free(s);
	}
}

typedef struct { Client *c; Timer *t; int tries; int stable; } IdleWait;

static void idle_poll(void *ud) {
	IdleWait *w = ud;
	if (!client_alive(w->c)) { timer_free(w->t); free(w); return; }
	if (shell_is_animating()) w->stable = 0; else w->stable++;
	if (w->stable >= 3 || ++w->tries > 120) {
		finish(w->c, w->tries > 120 ? "err timeout" : "ok");
		timer_free(w->t);
		free(w);
		return;
	}
	timer_set(w->t, 50);
}

static void wait_cb(void *ud) {
	IdleWait *w = ud;
	if (client_alive(w->c)) finish(w->c, "ok");
	timer_free(w->t);
	free(w);
}

/* -------------------------------------------------------------- commands -- */

static xkb_keysym_t keysym_by_name(const char *n) {
	xkb_keysym_t s = xkb_keysym_from_name(n, XKB_KEYSYM_NO_FLAGS);
	if (s == XKB_KEY_NoSymbol) s = xkb_keysym_from_name(n, XKB_KEYSYM_CASE_INSENSITIVE);
	return s;
}

static void cmd_state(Client *c) {
	static const char *scr[] = {"lock", "home", "app", "ext", "switcher"};
	reply(c, "screen=%s\n", scr[sh.screen]);
	reply(c, "page=%s pos=%.2f target=%d\n", sh.home_pager.target == 0 ? "start" : "apps", sh.home_pager.pos, sh.home_pager.target);
	reply(c, "ptr: down=%d moved=%d active=%u captured=%u hits=%d scrollers=%d\n", sh.ui.p.down, sh.ui.p.moved, sh.ui.p.active, sh.ui.p.captured, sh.ui.nhits[sh.ui.hb], sh.ui.nscr[sh.ui.hb]);
	reply(c, "app=%s\n", sh.cur_app ? sh.cur_app->id : "");
	reply(c, "view=%s\n", sh.cur_view && sh.cur_view->app_id ? sh.cur_view->app_id : "");
	reply(c, "locked=%d\nscreen_off=%d\nactioncenter=%d\nkeyboard=%d\n", sh.locked, sh.screen_off,
	      sh.ac_target_open, osk_wanted());
	reply(c, "notifications=%d\n", sh.notifs.len);
	reply(c, "size=%.0fx%.0f scale=%.3f px=%dx%d\n", sh.w, sh.h, sh.scale, sh.pw, sh.ph);
	reply(c, "lang=%s dark=%d accent=%d\n", i18n_code(i18n_lang()), cfg.dark, cfg.accent);
	reply(c, "views=%d\n", views_count_mapped());
	reply(c, "animating=%d\n", shell_is_animating());
	finish(c, "ok");
}

static void run_command(Client *c, char *line) {
	char *args[16];
	int n = 0;
	char *rest = NULL;
	char *p = line;
	while (n < 15) {
		while (*p == ' ') p++;
		if (!*p) break;
		const char *cmd0 = n > 0 ? args[0] : NULL;
		if (cmd0 && (str_eq(cmd0, "type") || str_eq(cmd0, "notify") || str_eq(cmd0, "set") || str_eq(cmd0, "exec")) && n >= 1 + (str_eq(cmd0, "set") ? 1 : 0)) {
			rest = p;
			break;
		}
		args[n++] = p;
		while (*p && *p != ' ') p++;
		if (*p) *p++ = 0;
	}
	if (n == 0) { finish(c, "err empty command"); return; }
	const char *cmd = args[0];
	double x1 = n > 1 ? atof(args[1]) : 0, y1 = n > 2 ? atof(args[2]) : 0;

	if (str_eq(cmd, "ping")) { finish(c, "pong"); return; }
	if (str_eq(cmd, "help")) {
		reply(c, "commands: ping state screenshot <png> | tap x y | down/move/up x y | swipe x1 y1 x2 y2 [ms]\n"
		         "  longpress x y [ms] | key <keysym> | type <text> | text <text> | back | home | search | lock\n"
		         "  unlock | switcher | action [open|close] | launch <b:id|d:id> | open <builtin> [arg] | tiles\n"
		         "  tap-tile <name> | notify <summary>|<body> | set <key> <value> | wait <ms> | idle | quit\n");
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "state")) { cmd_state(c); return; }
	if (str_eq(cmd, "screenshot")) {
		ShotJob *j = xcalloc(1, sizeof *j);
		j->c = c;
		str_copy(j->path, sizeof j->path, n > 1 ? args[1] : "/tmp/metrowl.png");
		c->busy = true;
		output_request_capture(shot_done, j);
		return;
	}
	if (str_eq(cmd, "tap")) {
		input_sim_pointer(0, x1, y1);
		input_sim_pointer(2, x1, y1);
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "down")) { input_sim_pointer(0, x1, y1); finish(c, "ok"); return; }
	if (str_eq(cmd, "move")) { input_sim_pointer(1, x1, y1); finish(c, "ok"); return; }
	if (str_eq(cmd, "up")) { input_sim_pointer(2, x1, y1); finish(c, "ok"); return; }
	if (str_eq(cmd, "swipe") || str_eq(cmd, "longpress")) {
		Swipe *s = xcalloc(1, sizeof *s);
		s->c = c;
		s->x1 = x1; s->y1 = y1;
		if (str_eq(cmd, "swipe")) {
			s->x2 = n > 3 ? atof(args[3]) : x1;
			s->y2 = n > 4 ? atof(args[4]) : y1;
			int ms = n > 5 ? atoi(args[5]) : 250;
			s->steps = CLAMP(ms / 16, 2, 200);
		} else {
			s->x2 = x1; s->y2 = y1;
			int ms = n > 3 ? atoi(args[3]) : 700;
			s->steps = CLAMP(ms / 16, 2, 400);
		}
		c->busy = true;
		s->t = timer_add(16, true, swipe_step, s);
		return;
	}
	if (str_eq(cmd, "key")) {
		xkb_keysym_t s = n > 1 ? keysym_by_name(args[1]) : XKB_KEY_NoSymbol;
		if (s == XKB_KEY_NoSymbol) { finish(c, "err unknown key"); return; }
		char utf8[8] = "";
		xkb_keysym_to_utf8(s, utf8, sizeof utf8);
		if (sh.screen != SCR_EXT || sh.ui.focus || dialog_active()) {
			shell_key(s, utf8, true, 0);
			shell_key(s, utf8, false, 0);
		} else if (!input_inject_keysym(s, 0)) {
			finish(c, "err no keyboard focus");
			return;
		}
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "type") || str_eq(cmd, "text")) {
		if (!rest && n > 1) rest = args[1];
		if (!rest) { finish(c, "err missing text"); return; }
		if (sh.ui.focus || sh.screen != SCR_EXT) ui_text_event(&sh.ui, rest), shell_invalidate();
		else input_inject_text(rest);
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "back")) { shell_back(); finish(c, "ok"); return; }
	if (str_eq(cmd, "home")) { shell_go_home(); finish(c, "ok"); return; }
	if (str_eq(cmd, "search")) { shell_open_search(); finish(c, "ok"); return; }
	if (str_eq(cmd, "lock")) { shell_lock(n > 1 && str_eq(args[1], "off")); finish(c, "ok"); return; }
	if (str_eq(cmd, "unlock")) { shell_unlock(); finish(c, "ok"); return; }
	if (str_eq(cmd, "switcher")) { shell_open_switcher(); finish(c, "ok"); return; }
	if (str_eq(cmd, "action")) {
		bool open = !(n > 1 && str_eq(args[1], "close"));
		if (n == 1) open = !sh.ac_target_open;
		shell_toggle_action_center(open);
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "launch")) { if (n > 1) shell_launch(args[1]); finish(c, n > 1 ? "ok" : "err missing app"); return; }
	if (str_eq(cmd, "open")) { if (n > 1) shell_open_builtin(args[1], n > 2 ? args[2] : NULL); finish(c, n > 1 ? "ok" : "err missing app"); return; }
	if (str_eq(cmd, "exec")) { if (rest) proc_spawn_sh(rest); finish(c, rest ? "ok" : "err missing command"); return; }
	if (str_eq(cmd, "keyboard")) { shell_show_keyboard(n < 2 || !str_eq(args[1], "off")); finish(c, "ok"); return; }
	if (str_eq(cmd, "tiles")) {
		for (int i = 0; i < start_tile_count(); i++) {
			char key[96];
			Rect r;
			if (start_tile_info(i, key, sizeof key, &r))
				reply(c, "%s %.0f %.0f %.0f %.0f\n", key, r.x, r.y, r.w, r.h);
		}
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "tap-tile")) {
		for (int i = 0; i < start_tile_count() && n > 1; i++) {
			char key[96];
			Rect r;
			if (start_tile_info(i, key, sizeof key, &r) && (strstr(key, args[1]) || str_eq(start_app_name(key), args[1]))) {
				input_sim_pointer(0, rect_cx(r), rect_cy(r));
				input_sim_pointer(2, rect_cx(r), rect_cy(r));
				finish(c, "ok");
				return;
			}
		}
		finish(c, "err tile not found (is it on screen?)");
		return;
	}
	if (str_eq(cmd, "notify")) {
		char *body = rest ? strchr(rest, '|') : NULL;
		if (body) *body++ = 0;
		shell_notify("metrowl", rest ? rest : "Notification", body ? body : "", NULL, NULL, 1, 0, false);
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "set")) {
		const char *k = n > 1 ? args[1] : "", *v = rest ? rest : "";
		if (str_eq(k, "lang")) { str_copy(cfg.lang, sizeof cfg.lang, v); }
		else if (str_eq(k, "theme")) cfg.dark = !str_eq(v, "light");
		else if (str_eq(k, "accent")) cfg.accent = CLAMP(atoi(v), 0, 19);
		else if (str_eq(k, "h24")) cfg.h24 = atoi(v) != 0;
		else if (str_eq(k, "pin")) {
			if (*v) { char h[65]; sha256_hex(v, strlen(v), h); str_copy(cfg.pin_hash, sizeof cfg.pin_hash, h); }
			else cfg.pin_hash[0] = 0;
		}
		else if (str_eq(k, "wallpaper")) { str_copy(cfg.wallpaper, sizeof cfg.wallpaper, v); cfg.start_wallpaper = *v != 0; wallpaper_changed(); }
		else if (str_eq(k, "lock_wallpaper")) { str_copy(cfg.lock_wallpaper, sizeof cfg.lock_wallpaper, v); wallpaper_changed(); }
		else { finish(c, "err unknown setting"); return; }
		shell_apply_settings();
		config_save_soon();
		finish(c, "ok");
		return;
	}
	if (str_eq(cmd, "wait")) {
		IdleWait *w = xcalloc(1, sizeof *w);
		w->c = c;
		c->busy = true;
		w->t = timer_add(MAX(1, n > 1 ? atoi(args[1]) : 100), false, wait_cb, w);
		return;
	}
	if (str_eq(cmd, "idle")) {
		IdleWait *w = xcalloc(1, sizeof *w);
		w->c = c;
		c->busy = true;
		w->t = timer_add(50, false, idle_poll, w);
		return;
	}
	if (str_eq(cmd, "quit")) { finish(c, "ok"); srv_quit(); return; }
	finish(c, "err unknown command (try: help)");
}

/* --------------------------------------------------------------- sockets -- */

static void client_close(Client *c) {
	wl_event_source_remove(c->src);
	close(c->fd);
	wl_list_remove(&c->link);
	free(c);
}

static int client_readable(int fd, uint32_t mask, void *data) {
	Client *c = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { client_close(c); return 0; }
	if (c->busy) return 0;
	ssize_t n = read(fd, c->buf + c->len, sizeof c->buf - (size_t)c->len - 1);
	if (n <= 0) { if (n < 0 && errno == EAGAIN) return 0; client_close(c); return 0; }
	c->len += (int)n;
	c->buf[c->len] = 0;
	char *nl = strchr(c->buf, '\n');
	if (!nl && c->len < (int)sizeof c->buf - 1) return 0;
	if (nl) *nl = 0;
	run_command(c, str_trim(c->buf));
	return 0;
}

static int listen_readable(int fd, uint32_t mask, void *data) {
	int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (cfd < 0) return 0;
	Client *c = xcalloc(1, sizeof *c);
	c->fd = cfd;
	c->src = wl_event_loop_add_fd(srv.loop, cfd, WL_EVENT_READABLE, client_readable, c);
	wl_list_insert(&g_clients, &c->link);
	return 0;
}

void ipc_init(const char *path) {
	wl_list_init(&g_clients);
	if (path) str_copy(g_path, sizeof g_path, path);
	else {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (rt && *rt) snprintf(g_path, sizeof g_path, "%s/metrowl-ipc", rt);
		else snprintf(g_path, sizeof g_path, "/tmp/metrowl-ipc-%d", (int)getuid());
	}
	g_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (g_listen_fd < 0) return;
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	str_copy(addr.sun_path, sizeof addr.sun_path, g_path);
	unlink(g_path);
	if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(g_listen_fd, 8) != 0) {
		LOGE("ipc: cannot listen on %s: %s", g_path, strerror(errno));
		close(g_listen_fd);
		g_listen_fd = -1;
		return;
	}
	g_listen_src = wl_event_loop_add_fd(srv.loop, g_listen_fd, WL_EVENT_READABLE, listen_readable, NULL);
	setenv("METROWL_IPC", g_path, 1);
	LOGI("control socket: %s", g_path);
}

void ipc_shutdown(void) {
	if (g_listen_fd >= 0) {
		wl_event_source_remove(g_listen_src);
		close(g_listen_fd);
		unlink(g_path);
	}
}

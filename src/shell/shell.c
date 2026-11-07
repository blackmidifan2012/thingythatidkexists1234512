#include "shell.h"
#include "../apps/apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../sys.h"
#include "../util.h"

#include <glib.h>
#include <math.h>
#include <sys/stat.h>

Shell sh;

#define STATUS_H 26.0
#define NAV_H 48.0

static Timer *g_tick_timer, *g_idle_timer, *g_longpress_timer;
static int g_last_minute = -1;
static double g_flash_t0;
static cairo_surface_t *g_snap;           /* scratch surface for page transitions */
static int g_snap_w, g_snap_h;
static bool g_ext_shown_last;
static bool g_was_animating;
static Anim g_lock_anim;                  /* 0 = lock covers the screen, 1 = gone */
static double g_lock_pos;
static bool g_lock_dragging;

/* ---- transition state */
enum { TR_NONE, TR_LEAVE, TR_ENTER };
static struct {
	int phase;
	double t0, dur;
	ScreenKind from, to;
	const AppDef *to_app;
	View *to_view;
	Rect origin;
	Color color;
} trn;

/* -------------------------------------------------------------- helpers --- */

static void apply_theme(void) {
	theme_make(&sh.ui.th, cfg.dark, RGBX(ACCENT_RGB[CLAMP(cfg.accent, 0, 19)]));
}

bool shell_is_animating(void) { return g_was_animating || trn.phase != TR_NONE; }

void shell_invalidate(void) {
	sh.dirty = true;
	srv_schedule_frame();
}

void shell_activity(void) {
	sh.last_input = now_sec();
}

/* ----------------------------------------------------------- app registry */

static const AppDef *const BUILTIN[] = {
	&app_settings, &app_store, &app_photos, &app_calculator, &app_clock, &app_calendar, &app_weather,
	&app_people, &app_notes, &app_files, &app_music, &app_internet, &app_camera, &app_search,
	&app_video, &app_battery, &app_storage, &app_terminal, &app_minesweeper, &app_recorder, &app_phone,
};

int app_count(void) { return (int)ARRAY_LEN(BUILTIN); }
const AppDef *app_get(int i) { return (i >= 0 && i < app_count()) ? BUILTIN[i] : NULL; }
const AppDef *app_find(const char *id) {
	if (!id) return NULL;
	if (str_starts(id, "b:")) id += 2;
	for (int i = 0; i < app_count(); i++) if (str_eq(BUILTIN[i]->id, id)) return BUILTIN[i];
	return NULL;
}

/* ------------------------------------------------------------ geometry ---- */

Rect shell_content_rect(void) {
	double top = STATUS_H;
	double bottom = sh.h - NAV_H - sh.kbd_pos;
	return R(0, top, sh.w, MAX(100, bottom - top));
}

static double kbd_target_height(void) {
	return osk_wanted() ? osk_height() : 0;
}

Rect shell_ext_rect(bool fullscreen) {
	if (fullscreen) return R(0, 0, sh.w, sh.h);
	/* with auto-hide the app gets the bar's space; a swipe up from the edge shows it on top */
	double top = STATUS_H, bottom = sh.h - (cfg.nav_autohide ? 0 : NAV_H) - kbd_target_height();
	return R(0, top, sh.w, MAX(100, bottom - top));
}

void shell_set_output(int pw, int ph, double scale) {
	sh.pw = pw;
	sh.ph = ph;
	sh.scale = scale;
	sh.w = srv.out ? srv.out->lw : pw / scale;
	sh.h = srv.out ? srv.out->lh : ph / scale;
	sh.status_h = STATUS_H;
	sh.nav_h = NAV_H;
	if (g_snap) { cairo_surface_destroy(g_snap); g_snap = NULL; }
	shell_invalidate();
}

/* ------------------------------------------------------------ snapshots --- */

cairo_surface_t *shell_snapshot_new(void) {
	if (!g_snap || g_snap_w != sh.pw || g_snap_h != sh.ph) {
		if (g_snap) cairo_surface_destroy(g_snap);
		g_snap = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sh.pw, sh.ph);
		g_snap_w = sh.pw;
		g_snap_h = sh.ph;
	}
	cairo_t *c = cairo_create(g_snap);
	cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(c, 0, 0, 0, 0);
	cairo_paint(c);
	cairo_destroy(c);
	return g_snap;
}

/* Runs `fn` with the UI redirected to the scratch surface. */
cairo_surface_t *shell_draw_snapshot(void (*fn)(UI *, void *), void *arg) {
	UI *ui = &sh.ui;
	cairo_surface_t *s = shell_snapshot_new();
	cairo_t *saved = ui->cr, *c = cairo_create(s);
	cairo_scale(c, sh.scale, sh.scale);
	ui->cr = c;
	bool was_blocked = ui->blocked;
	ui->blocked = true;
	fn(ui, arg);
	ui->blocked = was_blocked;
	ui->cr = saved;
	cairo_destroy(c);
	cairo_surface_flush(s);
	return s;
}

/* ------------------------------------------------------- screen drawing --- */

static void draw_screen(UI *ui, ScreenKind k) {
	Rect full = R(0, 0, sh.w, sh.h);
	Rect content = shell_content_rect();
	switch (k) {
	case SCR_HOME: {
		/* background */
		gfx_fill_rect(ui->cr, full, ui->th.bg);
		ui_pager_update(ui, ui_id("home-pager"), &sh.home_pager, content, 2);
		for (int p = 0; p < 2; p++) {
			Rect pg;
			if (!ui_pager_page(&sh.home_pager, p, content, &pg)) continue;
			cairo_save(ui->cr);
			cairo_translate(ui->cr, pg.x - content.x, 0);
			ui_clip_push(ui, content);
			if (p == 0) start_draw(ui, content, 0);
			else applist_draw(ui, content);
			ui_clip_pop(ui);
			cairo_restore(ui->cr);
		}
		ui_pager_end(ui);
		break;
	}
	case SCR_APP:
		gfx_fill_rect(ui->cr, full, ui->th.bg);
		if (sh.cur_app && sh.cur_app->draw) {
			ui_clip_push(ui, R(0, 0, sh.w, sh.h));
			sh.cur_app->draw(ui, content);
			ui_clip_pop(ui);
		}
		break;
	case SCR_SWITCHER:
		gfx_fill_rect(ui->cr, full, ui->th.bg);
		switcher_draw(ui, content);
		break;
	case SCR_EXT:
		gfx_fill_rect(ui->cr, full, ui->th.bg);
		if (sh.launching.active) {
			/* accent splash screen while the Linux app starts, like a WP splash */
			gfx_fill_rect(ui->cr, full, sh.launching.color);
			Color fg = col_contrast(sh.launching.color);
			double s = MIN(sh.w, sh.h) * 0.3;
			if (sh.launching.img && cfg.mono_icons) gfx_image_silhouette(ui->cr, sh.launching.img, R(rect_cx(full) - s / 2, rect_cy(full) - s / 2 - 20, s, s), fg);
			else if (sh.launching.img) gfx_image_contain(ui->cr, sh.launching.img, R(rect_cx(full) - s / 2, rect_cy(full) - s / 2 - 20, s, s), 1.0);
			else if (sh.launching.def) icon_draw(ui->cr, sh.launching.def->icon, rect_cx(full), rect_cy(full) - 20, s, fg);
			gfx_text(ui->cr, sh.launching.name, rect_cx(full), rect_cy(full) + s / 2 + 4, 22, FW_SEMILIGHT, fg, TA_CENTER, sh.w - 40, 1);
			ui_progress_dots(ui, R(40, sh.h - 140, sh.w - 80, 10), fg);
		}
		break;
	case SCR_LOCK:
		break;
	}
}

typedef struct { ScreenKind k; } DrawArg;
static void draw_screen_cb(UI *ui, void *arg) { draw_screen(ui, ((DrawArg *)arg)->k); }

/* ------------------------------------------------------------ transitions */

static bool transitions_enabled(void) { return cfg.animations && !cfg.battery_saver; }

static void tr_commit_target(void) {
	sh.screen = trn.to;
	sh.cur_app = trn.to_app;
	sh.cur_view = trn.to_view;
	if (trn.to == SCR_EXT && trn.to_view) {
		sh.fullscreen_ext = trn.to_view->fullscreen;
		view_activate(trn.to_view);
	} else {
		views_set_visible_only(NULL);
		input_clear_focus();
		sh.fullscreen_ext = false;
	}
	if (trn.to == SCR_HOME) start_enter_anim();
	views_relayout();
}

void shell_transition(ScreenKind to, const AppDef *app, View *view, Rect origin, Color col, bool back) {
	UI *ui = &sh.ui;
	if (trn.phase != TR_NONE) { tr_commit_target(); trn.phase = TR_NONE; }
	trn.from = sh.screen;
	trn.to = to;
	trn.to_app = app;
	trn.to_view = view;
	trn.origin = origin;
	trn.color = col;
	if (!transitions_enabled() || sh.screen == SCR_LOCK || (sh.screen == SCR_EXT && sh.cur_view) ||
	    sh.screen_off || (trn.from == to && trn.to_app == sh.cur_app && trn.to_view == sh.cur_view)) {
		tr_commit_target();
		shell_invalidate();
		return;
	}
	trn.phase = TR_LEAVE;
	trn.t0 = ui->now > 0 ? now_sec() : now_sec();
	trn.dur = sh.screen == SCR_HOME ? 0.36 : 0.2;
	if (sh.screen == SCR_HOME) start_leave_anim(origin);
	shell_invalidate();
}

static void draw_turnstiled_screen(UI *ui, ScreenKind k, double angle) {
	DrawArg a = {k};
	cairo_surface_t *s = shell_draw_snapshot(draw_screen_cb, &a);
	/* the snapshot is in pixels; the cr is scaled by `scale`, so draw in pixel space */
	cairo_save(ui->cr);
	cairo_scale(ui->cr, 1.0 / sh.scale, 1.0 / sh.scale);
	gfx_fill_rect(ui->cr, R(0, 0, sh.pw, sh.ph), ui->th.bg);
	ui_draw_turnstile(ui->cr, s, sh.pw, sh.ph, R(0, 0, sh.pw, sh.ph), angle, 0, 1.0);
	cairo_restore(ui->cr);
}

static void draw_base(UI *ui) {
	double now = ui->now;
	if (trn.phase == TR_LEAVE) {
		double p = (now - trn.t0) / trn.dur;
		if (p >= 1) {
			tr_commit_target();
			trn.phase = TR_ENTER;
			trn.t0 = now;
			trn.dur = trn.to == SCR_HOME ? 0.42 : 0.26;
			p = 0;
			if (trn.to == SCR_EXT) trn.phase = TR_NONE;
		} else {
			ui->animating = true;
		}
		if (trn.phase == TR_LEAVE) {
			if (trn.from == SCR_HOME) {
				ui->blocked = true;
				draw_screen(ui, SCR_HOME);
				ui->blocked = false;
			} else {
				draw_turnstiled_screen(ui, trn.from, -ease(EASE_IN_QUAD, p) * 78);
			}
			return;
		}
	}
	if (trn.phase == TR_ENTER) {
		double p = (now - trn.t0) / trn.dur;
		if (p >= 1) {
			trn.phase = TR_NONE;
		} else {
			ui->animating = true;
			if (trn.to == SCR_HOME) {
				ui->blocked = true;
				draw_screen(ui, SCR_HOME);
				ui->blocked = false;
			} else {
				draw_turnstiled_screen(ui, sh.screen, (1 - ease(EASE_OUT_CUBIC, p)) * 78);
			}
			return;
		}
	}
	draw_screen(ui, sh.screen);
}

/* ------------------------------------------------------------ lock state -- */

void shell_set_screen(ScreenKind k) { sh.screen = k; shell_invalidate(); }

void shell_wake(void) {
	if (!sh.screen_off) return;
	sh.screen_off = false;
	output_set_blanked(false);
	sh.last_input = now_sec();
	shell_invalidate();
}

void shell_lock(bool screen_off) {
	dialog_close();
	sh.locked = true;
	g_lock_pos = 0;
	anim_set(&g_lock_anim, 0);
	g_lock_dragging = false;
	lock_reset();
	shell_toggle_action_center(false);
	sh.ac_pos = 0;
	if (screen_off) {
		sh.screen_off = true;
		output_set_blanked(true);
	} else {
		shell_wake();
	}
	shell_invalidate();
}

void shell_unlock_from(double pos) {
	if (!sh.locked) return;
	sh.locked = false;
	g_lock_dragging = false;
	anim_start(&g_lock_anim, pos, 1, now_sec(), 0.3, EASE_OUT_CUBIC);
	if (sh.screen == SCR_HOME) start_enter_anim();
	shell_invalidate();
}

void shell_unlock(void) { shell_unlock_from(g_lock_pos); }

double shell_lock_pos(void) { return g_lock_pos; }
void shell_lock_set_pos(double p, bool dragging) { g_lock_pos = CLAMP(p, 0, 1); g_lock_dragging = dragging; anim_set(&g_lock_anim, g_lock_pos); }
bool shell_lock_dragging(void) { return g_lock_dragging; }

/* ----------------------------------------------------------- action center */

void shell_toggle_action_center(bool open) {
	if (open == sh.ac_target_open && !sh.ac_dragging) return;
	sh.ac_target_open = open;
	sh.ac_dragging = false;
	anim_start(&sh.ac_anim, sh.ac_pos, open ? 1 : 0, now_sec(), 0.28, EASE_OUT_CUBIC);
	shell_invalidate();
}

/* ------------------------------------------------------------ navigation -- */

void shell_go_home(void) {
	if (sh.locked) return;
	if (dialog_active()) dialog_close();
	if (sh.ac_target_open) { shell_toggle_action_center(false); }
	if (osk_wanted() && sh.kbd_forced) shell_show_keyboard(false);
	if (sh.screen == SCR_HOME) {
		if (start_edit_mode()) { start_back(); return; }
		if (sh.home_pager.target != 0) ui_pager_goto(&sh.home_pager, 0);
		else start_back_to_top();
		sh.ui.focus = 0;
		shell_invalidate();
		return;
	}
	sh.home_pager.target = 0;
	sh.home_pager.pos = 0;
	shell_transition(SCR_HOME, NULL, NULL, R(0, 0, 0, 0), sh.ui.th.accent, true);
}

void shell_back(void) {
	if (clock_overlay_active() && clock_overlay_back()) { shell_invalidate(); return; }
	if (sh.locked) { lock_back(); return; }
	if (dialog_back()) return;
	if (sh.ui.focus && osk_wanted()) { sh.ui.focus = 0; shell_invalidate(); return; }
	if (sh.kbd_forced) { shell_show_keyboard(false); return; }
	if (actioncenter_back()) return;
	switch (sh.screen) {
	case SCR_HOME:
		if (start_back()) return;
		if (applist_back()) return;
		if (sh.home_pager.target != 0) ui_pager_goto(&sh.home_pager, 0);
		shell_invalidate();
		break;
	case SCR_APP:
		if (sh.cur_app && sh.cur_app->back && sh.cur_app->back()) { shell_invalidate(); return; }
		/* like Windows Phone: back on an app's first page leaves it, still running */
		shell_go_home();
		break;
	case SCR_SWITCHER:
		shell_go_home();
		break;
	case SCR_EXT:
		shell_go_home();
		break;
	default:
		break;
	}
}

void shell_open_switcher(void) {
	if (sh.locked) return;
	dialog_close();
	shell_toggle_action_center(false);
	switcher_open();
	shell_transition(SCR_SWITCHER, NULL, NULL, R(0, 0, 0, 0), sh.ui.th.accent, false);
}

void shell_open_search(void) {
	if (sh.locked) return;
	shell_open_builtin("search", NULL);
}

void shell_open_builtin(const char *id, const char *arg) {
	const AppDef *d = app_find(id);
	if (!d) { LOGE("unknown built-in app %s", id); return; }
	if (sh.locked) return;
	if (d->launcher) { dialog_close(); shell_toggle_action_center(false); d->launcher(); return; }
	dialog_close();
	shell_toggle_action_center(false);
	bool found = false;
	for (int i = 0; i < sh.app_stack.len; i++) {
		if (sh.app_stack.items[i] == d) {
			const AppDef *x = sh.app_stack.items[i];
			vec_remove(&sh.app_stack, i);
			vec_push(&sh.app_stack, x);
			found = true;
			break;
		}
	}
	if (!found) vec_push(&sh.app_stack, d);
	if (d->open) d->open(arg);
	Rect origin = R(0, 0, 0, 0);
	Color col = d->color ? RGBX(d->color) : sh.ui.th.accent;
	start_launch_rect(id, &origin, &col);
	shell_transition(SCR_APP, d, NULL, origin, col, false);
}

void shell_close_app(const AppDef *def) {
	for (int i = 0; i < sh.app_stack.len; i++) {
		if (sh.app_stack.items[i] == def) { vec_remove(&sh.app_stack, i); break; }
	}
	if (def->close) def->close();
	if (sh.screen == SCR_APP && sh.cur_app == def) shell_go_home();
	shell_invalidate();
}

/* ------------------------------------------------------------- launching -- */

static void launching_clear(void) {
	free(sh.launching.name);
	sh.launching.name = NULL;
	sh.launching.active = false;
	sh.launching.img = NULL;
}

void shell_launch(const char *app) {
	if (!app || sh.locked) return;
	if (str_starts(app, "b:") || (!str_starts(app, "d:") && app_find(app))) {
		shell_open_builtin(app, NULL);
		return;
	}
	if (str_starts(app, "d:")) app += 2;
	DesktopApp *a = desktop_find(app);
	if (!a) { shell_toast(_("App not found"), app, NULL); return; }
	dialog_close();
	shell_toggle_action_center(false);
	View *existing = view_find_by_app(a);
	Rect origin = R(0, 0, 0, 0);
	Color col = sh.ui.th.accent;
	start_launch_rect(app, &origin, &col);
	if (existing) {
		shell_transition(SCR_EXT, NULL, existing, origin, col, false);
		return;
	}
	if (!desktop_launch(a)) { shell_toast(_("Could not start app"), a->name, NULL); return; }
	launching_clear();
	sh.launching.active = true;
	sh.launching.t0 = now_sec();
	sh.launching.name = xstrdup(a->name);
	sh.launching.color = col;
	sh.launching.img = desktop_icon(a, 128);
	sh.launching.def = NULL;
	sh.launching.app = a;
	sh.cur_view = NULL;
	shell_transition(SCR_EXT, NULL, NULL, origin, col, false);
}

void shell_view_mapped(View *v) {
	if (sh.locked && !sh.screen_off) { /* keep the lock screen, show the app after unlock */ }
	bool bound = false;
	if (sh.launching.active && (v->app == sh.launching.app || !sh.launching.app)) bound = true;
	if (sh.launching.active && !bound && now_sec() - sh.launching.t0 < 20) bound = true;
	launching_clear();
	sh.cur_view = v;
	if (trn.phase != TR_NONE) { trn.to = SCR_EXT; trn.to_view = v; trn.to_app = NULL; }
	sh.screen = SCR_EXT;
	sh.fullscreen_ext = v->fullscreen;
	sh.home_pager.target = 0;
	view_activate(v);
	g_ext_shown_last = false;
	shell_invalidate();
	(void)bound;
}

void shell_view_unmapped(View *v) {
	if (sh.cur_view == v) {
		sh.cur_view = NULL;
		if (sh.screen == SCR_EXT) {
			/* fall back to another running app or Start */
			View *other = NULL, *it;
			wl_list_for_each(it, &srv.views, link)
				if (it != v && it->mapped && !it->is_dialog) { other = it; break; }
			if (other && other->mapped) shell_transition(SCR_EXT, NULL, other, R(0, 0, 0, 0), sh.ui.th.accent, true);
			else if (!sh.launching.active) shell_transition(SCR_HOME, NULL, NULL, R(0, 0, 0, 0), sh.ui.th.accent, true);
		}
	}
	if (trn.to_view == v) trn.to_view = NULL;
	shell_invalidate();
}

void shell_view_updated(View *v) {
	if (v == sh.cur_view) {
		sh.fullscreen_ext = v->fullscreen;
		views_relayout();
	}
	shell_invalidate();
}

void shell_show_keyboard(bool show) {
	sh.kbd_forced = show;
	shell_invalidate();
	views_relayout();
}

/* --------------------------------------------------------- notifications -- */

static void notif_free(Notification *n) {
	free(n->app_name); free(n->summary); free(n->body); free(n->icon);
	free(n->desktop_entry); free(n->action_key);
	free(n);
}

uint32_t shell_notify(const char *app, const char *summary, const char *body, const char *icon,
                      const char *desktop_entry, int urgency, uint32_t replaces, bool internal) {
	Notification *n = NULL;
	if (replaces) {
		for (int i = 0; i < sh.notifs.len; i++) if (sh.notifs.items[i]->id == replaces) n = sh.notifs.items[i];
	}
	bool fresh = !n;
	if (n) {
		free(n->app_name); free(n->summary); free(n->body); free(n->icon); free(n->desktop_entry);
	} else {
		n = xcalloc(1, sizeof *n);
		n->id = ++sh.next_notif_id;
		vec_push(&sh.notifs, n);
	}
	n->app_name = xstrdup(app && *app ? app : "");
	n->summary = xstrdup(summary ? summary : "");
	n->body = xstrdup(body ? body : "");
	n->icon = xstrdup(icon ? icon : "");
	n->desktop_entry = xstrdup(desktop_entry ? desktop_entry : "");
	n->urgency = urgency;
	n->internal = internal;
	n->time = time(NULL);
	n->toasted = false;
	bool quiet = cfg.quiet_hours && urgency < 2;
	if (cfg.toasts && !quiet) {
		bool queued = false;
		for (int i = 0; i < sh.toasts.len; i++) if (sh.toasts.items[i] == n) queued = true;
		if (!queued) vec_push(&sh.toasts, n);
		if (sh.toasts.len == 1) n->toast_t0 = 0;
	}
	/* updates of an existing notification (progress...) stay silent */
	if (fresh && !quiet && !internal) app_play_event(EV_NOTIF);
	shell_invalidate();
	return n->id;
}

void shell_notify_remove(uint32_t id) {
	for (int i = 0; i < sh.notifs.len; i++) {
		Notification *n = sh.notifs.items[i];
		if (n->id != id) continue;
		for (int j = 0; j < sh.toasts.len; j++) if (sh.toasts.items[j] == n) { vec_remove(&sh.toasts, j); break; }
		vec_remove(&sh.notifs, i);
		notifyd_close(id, 3);
		notif_free(n);
		break;
	}
	shell_invalidate();
}

void shell_notify_clear(void) {
	while (sh.notifs.len) {
		Notification *n = sh.notifs.items[sh.notifs.len - 1];
		sh.notifs.len--;
		notifyd_close(n->id, 2);
		notif_free(n);
	}
	sh.toasts.len = 0;
	shell_invalidate();
}

int shell_notify_count_for(const char *app_name, const char *desktop_entry) {
	int c = 0;
	for (int i = 0; i < sh.notifs.len; i++) {
		Notification *n = sh.notifs.items[i];
		if (n->internal) continue;
		bool match = false;
		if (desktop_entry && *desktop_entry && *n->desktop_entry && g_ascii_strcasecmp(n->desktop_entry, desktop_entry) == 0) match = true;
		if (!match && app_name && *app_name && *n->app_name && g_ascii_strcasecmp(n->app_name, app_name) == 0) match = true;
		if (match) c++;
	}
	return c;
}

bool shell_notify_activate(Notification *n) {
	char key[200];
	if (*n->desktop_entry && desktop_find(n->desktop_entry)) {
		notifyd_action(n->id, "default");
		snprintf(key, sizeof key, "d:%s", n->desktop_entry);
		shell_launch(key);
		return true;
	}
	if (*n->app_name) {
		for (int i = 0; i < app_count(); i++) {
			const AppDef *d = app_get(i);
			if (d->hidden || d->launcher) continue;
			if (g_ascii_strcasecmp(n->app_name, d->id) == 0 || str_eq(n->app_name, d->name) || str_eq(n->app_name, tr(d->name))) {
				shell_open_builtin(d->id, NULL);
				return true;
			}
		}
		DesktopApp *a = desktop_find_by_app_id(n->app_name);
		if (a) {
			notifyd_action(n->id, "default");
			snprintf(key, sizeof key, "d:%s", a->id);
			shell_launch(key);
			return true;
		}
	}
	return false;
}

void shell_toast(const char *title, const char *body, const char *app_name) {
	shell_notify(app_name ? app_name : "", title, body ? body : "", NULL, NULL, 1, 0, true);
}

/* --------------------------------------------------------- screenshots ---- */

static void screenshot_done(cairo_surface_t *surf, void *ud) {
	if (!surf) { shell_toast(_("Screenshot failed"), "", NULL); return; }
	char *dir = path_join(home_dir(), "Pictures/Screenshots");
	mkdir_p(dir);
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char name[96];
	strftime(name, sizeof name, "Screenshot_%Y%m%d_%H%M%S.png", &tm);
	char *path = path_join(dir, name);
	if (cairo_surface_write_to_png(surf, path) == CAIRO_STATUS_SUCCESS)
		shell_toast(_("Screenshot saved"), name, _("Photos"));
	else
		shell_toast(_("Screenshot failed"), path, NULL);
	free(path);
	free(dir);
	photos_rescan();
}

void shell_screenshot(void) {
	g_flash_t0 = now_sec();
	output_request_capture(screenshot_done, NULL);
	shell_invalidate();
}

void shell_power_menu(void) {
	static const char *items[5];
	items[0] = _("Lock");
	items[1] = _("Sleep");
	items[2] = _("Restart");
	items[3] = _("Shut down");
	items[4] = NULL;
	extern void power_menu_cb(int, void *);
	dialog_list(_("Power"), items, 4, -1, power_menu_cb, NULL);
}

void power_menu_cb(int idx, void *ud) {
	switch (idx) {
	case 0: shell_lock(false); break;
	case 1: shell_lock(true); sys_suspend(); break;
	case 2: sys_reboot(); break;
	case 3: sys_poweroff(); break;
	default: break;
	}
}

/* ------------------------------------------------------ settings applied -- */

void shell_apply_settings(void) {
	i18n_init(cfg.lang);
	i18n_set_region(cfg.region);
	gfx_set_text_scale(cfg.text_scale);
	icons_set_mode(cfg.icon_mode);
	apply_theme();
	srv_set_keymap(cfg.kb_layout[0] ? cfg.kb_layout : (i18n_lang() == LANG_TR ? "tr" : "us"), NULL);
	desktop_scan();
	start_reload();
	osk_reset();
	wallpaper_changed();
	if (srv.out) output_reapply_scale();
	shell_invalidate();
}

/* ---------------------------------------------------------- pointer input - */

bool shell_wants_pointer(double x, double y) {
	if (sh.locked || sh.screen_off) return true;
	if (sh.screen != SCR_EXT || !sh.cur_view) return true;
	if (dialog_active() || sh.ac_pos > 0.001 || sh.ac_target_open) return true;
	return ui_hit_test(&sh.ui, x, y, UL_CHROME);
}

static void longpress_cb(void *ud) {
	ui_longpress_fire(&sh.ui);
	shell_invalidate();
}

void shell_pointer(int type, double x, double y, int source) {
	UI *ui = &sh.ui;
	double now = now_sec();
	sh.last_input = now;
	if (sh.screen_off) { shell_wake(); return; }
	if (!g_longpress_timer) g_longpress_timer = timer_add(0, false, longpress_cb, NULL);
	switch (type) {
	case 0:
		ui_pointer_down(ui, x, y, now, source);
		timer_set(g_longpress_timer, 480);
		break;
	case 1:
		ui_pointer_move(ui, x, y, now);
		if (ui->p.moved) timer_set(g_longpress_timer, 0);
		break;
	case 2:
		ui_pointer_up(ui, x, y, now);
		timer_set(g_longpress_timer, 0);
		break;
	case 3:
		ui_longpress_fire(ui);
		break;
	}
	shell_invalidate();
}

void shell_hover(double x, double y) {
	ui_pointer_hover(&sh.ui, x, y);
}

void shell_scroll(double dx, double dy) {
	ui_wheel(&sh.ui, dx, dy);
	shell_invalidate();
}

void shell_key(xkb_keysym_t sym, const char *utf8, bool pressed, uint32_t mods) {
	UI *ui = &sh.ui;
	if (!pressed) return;
	sh.last_input = now_sec();
	bool ctrl = mods & WLR_MODIFIER_CTRL;
	UiKey k = UK_NONE;
	switch (sym) {
	case XKB_KEY_BackSpace: k = UK_BACKSPACE; break;
	case XKB_KEY_Delete: k = UK_DELETE; break;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: k = UK_ENTER; break;
	case XKB_KEY_Left: k = UK_LEFT; break;
	case XKB_KEY_Right: k = UK_RIGHT; break;
	case XKB_KEY_Up: k = UK_UP; break;
	case XKB_KEY_Down: k = UK_DOWN; break;
	case XKB_KEY_Home: k = UK_HOME; break;
	case XKB_KEY_End: k = UK_END; break;
	case XKB_KEY_Tab: k = UK_TAB; break;
	case XKB_KEY_Escape: k = UK_ESC; break;
	case XKB_KEY_Page_Up: k = UK_PGUP; break;
	case XKB_KEY_Page_Down: k = UK_PGDN; break;
	default: break;
	}
	if (ui->focus) {
		/* text field focused: keys go to it */
		if (k != UK_NONE) ui_key_event(ui, k);
		else if (utf8 && utf8[0] && !ctrl && (unsigned char)utf8[0] >= 0x20) ui_text_event(ui, utf8);
		shell_invalidate();
		return;
	}
	if (sh.locked) {
		if (k == UK_ENTER || k == UK_UP) lock_key(k, utf8);
		else lock_key(k, utf8);
		shell_invalidate();
		return;
	}
	if (dialog_active() && k == UK_ESC) { dialog_back(); return; }
	/* no text focus: navigation keys */
	if (k == UK_ESC || k == UK_BACKSPACE) { shell_back(); return; }
	if (sh.screen == SCR_HOME && utf8 && utf8[0] && (unsigned char)utf8[0] >= 0x20 && !ctrl) {
		/* typing on Start/All apps jumps into search with that text */
		shell_open_search();
		ui_text_event(ui, utf8);
		shell_invalidate();
		return;
	}
	if (k != UK_NONE) ui_key_event(ui, k);
	if (utf8 && utf8[0] && !ctrl && (unsigned char)utf8[0] >= 0x20) ui_text_event(ui, utf8);
	shell_invalidate();
}

/* ------------------------------------------------------------- rendering -- */

static void draw_flash(UI *ui) {
	double t = ui->now - g_flash_t0;
	if (g_flash_t0 > 0 && t < 0.25) {
		gfx_fill_rect(ui->cr, R(0, 0, sh.w, sh.h), RGBA(0xffffff, (float)(0.8 * (1 - t / 0.25))));
		ui->animating = true;
	}
}

static void draw_overlays(UI *ui, bool ext_visible) {
	ui->blocked = false;
	ui->layer = UL_CHROME;
	if (!sh.locked || g_lock_pos > 0.02) chrome_draw(ui);
	ui->layer = UL_OSK;
	double kb_target = (osk_wanted() ? osk_height() : 0);
	sh.kbd_anim.active = true;
	if (sh.kbd_anim.to != kb_target) {
		anim_start(&sh.kbd_anim, sh.kbd_pos, kb_target, ui->now, 0.22, EASE_OUT_CUBIC);
		views_relayout();
	}
	sh.kbd_pos = ui_anim(ui, &sh.kbd_anim);
	ui->kbd_inset = sh.kbd_pos;
	/* with the nav bar auto-hidden the keyboard sits on the bottom edge, where the app
	 * layout already expects it */
	bool nav_hidden = (sh.fullscreen_ext || (cfg.nav_autohide && sh.cur_view)) && sh.screen == SCR_EXT;
	if (sh.kbd_pos > 0.5) osk_draw(ui, sh.h - (nav_hidden ? 0 : NAV_H));
	ui->layer = UL_PANEL;
	actioncenter_draw(ui);
	/* these slides are advanced in shell_render(): keep frames coming until they finish,
	 * otherwise on a touchscreen (no hover events) they freeze where the finger let go */
	if (sh.ac_anim.active || g_lock_anim.active) ui->animating = true;
	ui->layer = UL_DIALOG;
	dialog_draw(ui);
	if (sh.locked || g_lock_pos < 0.999 ) {
		ui->layer = UL_DIALOG;
		lock_draw(ui, R(0, 0, sh.w, sh.h));
	}
	if (clock_overlay_active()) {
		ui->layer = UL_DIALOG;
		clock_overlay_draw(ui, R(0, 0, sh.w, sh.h));
	}
	ui->layer = UL_TOAST;
	chrome_toast_draw(ui);
	chrome_volume_hud(ui);
	draw_flash(ui);
	(void)ext_visible;
}

bool shell_render(double now) {
	if (!sh.dirty && !g_was_animating && trn.phase == TR_NONE) return false;
	if (sh.w <= 0 || !g_layer_base) return false;
	sh.dirty = false;
	UI *ui = &sh.ui;

	/* lock animation bookkeeping */
	if (anim_running(&g_lock_anim, now)) g_lock_pos = anim_value(&g_lock_anim, now);
	else if (g_lock_anim.active) { g_lock_pos = g_lock_anim.to; g_lock_anim.active = false; }

	/* action centre */
	if (anim_running(&sh.ac_anim, now)) sh.ac_pos = anim_value(&sh.ac_anim, now);
	else if (sh.ac_anim.active) { sh.ac_pos = sh.ac_anim.to; sh.ac_anim.active = false; }

	bool lock_covering = sh.locked && g_lock_pos < 0.02;
	bool ext_visible = sh.screen == SCR_EXT && sh.cur_view && !sh.launching.active && trn.phase != TR_LEAVE &&
	                   !(lock_covering && !g_lock_dragging) && !sh.screen_off;
	if (trn.phase == TR_ENTER && trn.to == SCR_EXT) ext_visible = sh.cur_view != NULL;

	UiLayer *layer = ext_visible ? g_layer_overlay : g_layer_base;
	cairo_t *cr = uilayer_begin(layer);
	ui_begin(ui, cr, sh.w, sh.h, sh.scale, now);
	ui->kbd_inset = sh.kbd_pos;
	ui->layer = UL_BASE;
	ui->blocked = (trn.phase != TR_NONE) || lock_covering;
	bool in_transition = trn.phase != TR_NONE;
	/* a fully open action centre is opaque and covers everything below it */
	bool covered = sh.ac_pos >= 0.999 && !sh.ac_dragging && trn.phase == TR_NONE;
	if (!ext_visible && !covered) draw_base(ui);
	/* the frame that ends a transition was drawn with input blocked: draw one more so the
	 * page registers its hit areas, otherwise the first tap after opening an app is lost */
	if (in_transition && trn.phase == TR_NONE) ui->redraw = true;
	osk_note_wanted(ui->kbd_wanted, ui->kbd_purpose);
	draw_overlays(ui, ext_visible);

	/* focus lost -> keep OSK state consistent */
	if (!ui->kbd_wanted && ui->focus == 0) { /* nothing */ }
	bool animating = ui->animating;
	ui_end(ui);
	uilayer_commit(layer, !ext_visible);
	if (ext_visible != g_ext_shown_last || (ext_visible && views_foreground() != sh.cur_view)) {
		g_ext_shown_last = ext_visible;
		uilayer_set_visible(g_layer_base, !ext_visible);
		uilayer_set_visible(g_layer_overlay, ext_visible);
		if (ext_visible) view_activate(sh.cur_view);
		else views_set_visible_only(NULL);
	}
	if (ext_visible) { uilayer_set_visible(g_layer_overlay, true); uilayer_set_visible(g_layer_base, false); }
	else { uilayer_set_visible(g_layer_base, true); uilayer_set_visible(g_layer_overlay, false); }

	bool more = animating || ui->redraw || trn.phase != TR_NONE;
	g_was_animating = more;
	/* the scratch surface for transitions is screen sized: release it while idle */
	if (!more && g_snap) { cairo_surface_destroy(g_snap); g_snap = NULL; }
	if (ui->redraw) sh.dirty = true;
	return more;
}

/* ---------------------------------------------------------------- timers -- */

static void tick_cb(void *ud) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	if (tm.tm_min != g_last_minute) {
		g_last_minute = tm.tm_min;
		shell_invalidate();
	}
	/* live tiles flip on a slow cadence */
	if (cfg.live_tiles && sh.screen == SCR_HOME && !sh.screen_off) shell_invalidate();
}

static void idle_cb(void *ud) {
	if (sh.screen_off || cfg.screen_timeout <= 0) return;
	if (input_idle_seconds() > cfg.screen_timeout && !srv.idle_inhibited) {
		if (!sh.locked) shell_lock(true);
		else { sh.screen_off = true; output_set_blanked(true); }
	}
}

void shell_tick_minute(void) { tick_cb(NULL); }

/* Low battery: a warning notification with the warning sound at 15 % and 5 %. */
static void battery_check(void *ud) {
	(void)ud;
	static int warned = 100;
	Battery b = sys_battery();
	if (!b.present || b.charging || b.full) { warned = 100; return; }
	int level = b.pct <= 5 ? 5 : b.pct <= 15 ? 15 : 100;
	if (level >= warned) return;
	warned = level;
	char body[96];
	snprintf(body, sizeof body, _("%d%% battery remaining. Connect your charger."), b.pct);
	shell_notify(_("Battery"), _("Battery low"), body, "", "", 2, 0, true);
	app_play_event(EV_WARN);
}

void shell_init(void) {
	memset(&sh, 0, sizeof sh);
	ui_init(&sh.ui);
	i18n_set_region(cfg.region);
	sh.status_h = STATUS_H;
	sh.nav_h = NAV_H;
	sh.locked = !srv.opt_nolock;
	timer_add(30000, true, battery_check, NULL);
	sh.screen = SCR_HOME;
	sh.last_input = now_sec();
	anim_set(&sh.kbd_anim, 0);
	anim_set(&sh.ac_anim, 0);
	anim_set(&g_lock_anim, sh.locked ? 0 : 1);
	g_lock_pos = sh.locked ? 0 : 1;
	apply_theme();
	srv_set_keymap(cfg.kb_layout[0] ? cfg.kb_layout : (i18n_lang() == LANG_TR ? "tr" : "us"), NULL);
	start_init();
	osk_init();
	for (int i = 0; i < app_count(); i++)
		if (app_get(i)->init) app_get(i)->init();
	g_tick_timer = timer_add(1000, true, tick_cb, NULL);
	g_idle_timer = timer_add(2000, true, idle_cb, NULL);
	LOGI("shell ready (%s)", sh.locked ? "locked" : "unlocked");
}

void shell_shutdown(void) {
	start_save();
	timer_free(g_tick_timer);
	timer_free(g_idle_timer);
	timer_free(g_longpress_timer);
}

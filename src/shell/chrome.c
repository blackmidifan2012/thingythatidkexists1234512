#include "shell.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../sys.h"
#include "../util.h"
#include "../proc.h"

#include <math.h>

/* Status bar, navigation bar (back / start / search), toasts and volume HUD. */

static int wifi_level(int sig) { return sig > 66 ? 3 : sig > 33 ? 2 : sig > 8 ? 1 : 0; }

void chrome_status_draw(UI *ui, Rect area, bool light) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Color fg = light ? COL_WHITE : th->fg;
	double cy = rect_cy(area);
	/* left: connectivity */
	double x = 10;
	WifiInfo w = sys_wifi();
	if (sys_airplane()) {
		icon_draw(cr, ICON_AIRPLANE, x + 8, cy, 15, fg);
		x += 24;
	} else {
		if (w.available && w.enabled) {
			if (w.connected) icon_draw_wifi_level(cr, x + 9, cy + 1, 16, fg, wifi_level(w.signal));
			else icon_draw_wifi_level(cr, x + 9, cy + 1, 16, col_alpha(fg, 0.5f), 0);
			x += 26;
		}
		if (sys_demo()) { icon_draw_signal(cr, x + 9, cy, 14, fg, 3); x += 24; }
	}
	if (cfg.quiet_hours) { icon_draw(cr, ICON_MOON, x + 8, cy, 14, fg); x += 22; }
	if (cfg.battery_saver) { gfx_fill_circle(cr, x + 6, cy, 3, fg); x += 14; }
	/* right: battery + clock */
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char tb[32];
	i18n_fmt_time(tb, sizeof tb, &tm, cfg.h24);
	double rx = area.w - 10;
	Size ts = gfx_text_size(cr, tb, 14, FW_SEMILIGHT, 0, 1);
	gfx_text(cr, tb, rx, cy - ts.h / 2, 14, FW_SEMILIGHT, fg, TA_RIGHT, 0, 1);
	rx -= ts.w + 12;
	Battery b = sys_battery();
	if (b.present) {
		icon_draw_battery(cr, rx - 14, cy, 18, fg, b.pct, b.charging);
		rx -= 34;
		char pb[8];
		snprintf(pb, sizeof pb, "%d%%", b.pct);
		Size ps = gfx_text_size(cr, pb, 13, FW_REGULAR, 0, 1);
		gfx_text(cr, pb, rx + 4, cy - ps.h / 2, 13, FW_REGULAR, col_alpha(fg, 0.9f), TA_RIGHT, 0, 1);
	}
}

static Rect nav_rect(void) { return R(0, sh.h - sh.nav_h, sh.w, sh.nav_h); }

bool chrome_hits_nav(double x, double y) { return rect_has(nav_rect(), x, y); }

static void nav_hide_cb(void *ud) { shell_invalidate(); }

static bool nav_visible(void) {
	if (sh.locked) return false;
	if ((sh.fullscreen_ext || (cfg.nav_autohide && sh.cur_view)) && sh.screen == SCR_EXT) return sh.ui.now < sh.nav_peek_until;
	return true;
}

static bool status_visible(void) {
	if (sh.locked) return false;
	return !(sh.fullscreen_ext && sh.screen == SCR_EXT);
}

void chrome_draw(UI *ui) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	bool ext = sh.screen == SCR_EXT && sh.cur_view && !sh.launching.active;

	/* ---- status bar */
	if (status_visible()) {
		Rect sb = R(0, 0, sh.w, sh.status_h);
		if (ext) gfx_fill_rect(cr, sb, th->bg);
		chrome_status_draw(ui, sb, false);
	}

	/* pull-down zone for the action center (also over fullscreen apps) */
	if (!sh.locked) {
		UiId pid = ui_id("ac-pull");
		Rect zone = R(0, 0, sh.w, status_visible() ? sh.status_h + 4 : 10);
		ui_hit_drag(ui, pid, zone);
		double dx, dy;
		if (ui_dragging(ui, pid) && ui->p.moved) {
			sh.ac_dragging = true;
			sh.ac_target_open = true;
			sh.ac_anim.active = false;
			sh.ac_pos = CLAMP(ui->p.y / (sh.h * 0.45), 0, 1);
			ui->redraw = true;
		}
		(void)dx; (void)dy;
		double vx, vy;
		if (ui_fling(ui, pid, &vx, &vy) && (sh.ac_dragging || ui->p.moved)) {
			/* judge by the release point: on a slow phone a whole flick can arrive between
			 * two frames, and bunched-up events make its measured speed unreliable */
			sh.ac_pos = CLAMP(ui->p.y / (sh.h * 0.45), 0, 1);
			bool open = sh.ac_pos > 0.2 || vy > 300;
			sh.ac_dragging = true;   /* makes the toggle animate from where the finger left it */
			shell_toggle_action_center(open);
		}
		if (ui_clicked(ui, pid) && !sh.ac_dragging) {
			/* tapping the status bar scrolls Start to top, like a phone */
			if (sh.screen == SCR_HOME) start_back_to_top();
		}
	}

	/* bottom-edge swipe reveals the nav bar in fullscreen apps */
	if (!sh.locked && (sh.fullscreen_ext || (cfg.nav_autohide && sh.cur_view)) && sh.screen == SCR_EXT) {
		UiId bid = ui_id("nav-peek");
		ui_hit_drag(ui, bid, R(0, sh.h - 12, sh.w, 12));
		if (ui_dragging(ui, bid) && ui->p.moved && ui->p.sy - ui->p.y > 12) {
			sh.nav_peek_until = ui->now + 3.5;
			/* redraw once the peek expires so the bar actually hides again */
			static Timer *hide;
			if (!hide) hide = timer_add(0, false, nav_hide_cb, NULL);
			timer_set(hide, 3600);
		}
	}

	/* ---- navigation bar */
	if (nav_visible()) {
		Rect nb = nav_rect();
		gfx_fill_rect(cr, nb, th->bg);
		ui_hit(ui, ui_id("nav-bg"), nb);
		static const Icon icons[3] = {ICON_BACK, ICON_START, ICON_SEARCH};
		for (int i = 0; i < 3; i++) {
			Rect br = R(nb.x + i * nb.w / 3, nb.y, nb.w / 3, nb.h);
			UiId id = ui_idn("nav-btn", i);
			ui_hit(ui, id, br);
			bool pr = ui_pressed(ui, id);
			Color c = pr ? th->sub : th->fg;
			icon_draw(cr, icons[i], rect_cx(br), rect_cy(br), i == 1 ? 21 : 24, c);
			if (i == 0 && ui_longpressed(ui, id)) { shell_open_switcher(); }
			else if (ui_clicked(ui, id)) {
				if (i == 0) shell_back();
				else if (i == 1) shell_go_home();
				else shell_open_search();
			}
		}
		/* a keyboard toggle for Linux apps that do not use text-input-v3 */
		if (sh.screen == SCR_EXT && sh.cur_view) {
			Rect kr = R(nb.w - 54, nb.y + 4, 48, nb.h - 8);
			UiId kid = ui_id("nav-kbd");
			ui_hit(ui, kid, kr);
			icon_draw(cr, ICON_KEYBOARD, rect_cx(kr), rect_cy(kr), 18, ui_pressed(ui, kid) ? th->sub : col_alpha(th->fg, 0.7f));
			if (ui_clicked(ui, kid)) shell_show_keyboard(!osk_wanted());
		}
	}
	/* make sure the lower overscroll area never lets touches through */
}

/* ------------------------------------------------------------------ toasts */

static Image *notif_icon(const Notification *n, int px) {
	if (*n->desktop_entry) {
		DesktopApp *a = desktop_find(n->desktop_entry);
		if (a) return desktop_icon(a, px);
	}
	if (*n->icon) {
		Image *i = desktop_icon_by_name(n->icon, px);
		if (i) return i;
	}
	DesktopApp *a = *n->app_name ? desktop_find_by_app_id(n->app_name) : NULL;
	return a ? desktop_icon(a, px) : NULL;
}

void chrome_notif_icon_draw(UI *ui, const Notification *n, Rect r, Color fg) {
	Image *img = notif_icon(n, 96);
	if (img) gfx_image_silhouette(ui->cr, img, r, fg);
	else icon_draw(ui->cr, ICON_MESSAGE, rect_cx(r), rect_cy(r), r.w * 0.8, fg);
}

static void toast_activate(Notification *n) {
	if (shell_notify_activate(n)) {
		/* opened the app it came from */
	} else if (!n->internal) {
		shell_toggle_action_center(true);
	}
}

void chrome_toast_draw(UI *ui) {
	cairo_t *cr = ui->cr;
	if (!sh.toasts.len) return;
	Notification *n = sh.toasts.items[0];
	double now = ui->now;
	if (n->toast_t0 == 0) n->toast_t0 = now;
	double age = now - n->toast_t0;
	double life = n->urgency >= 2 ? 9 : 5;
	static double drag_x;
	UiId id = ui_id("toast");
	double slide = age < 0.25 ? ease(EASE_OUT_CUBIC, age / 0.25) : (age > life - 0.25 ? 1 - ease(EASE_IN_QUAD, (age - (life - 0.25)) / 0.25) : 1);
	double th_h = 76, y = -th_h + slide * (th_h + 2);
	ui->animating = true;
	double dx = 0;
	if (ui_dragging(ui, id)) { double ddx, ddy; if (ui_drag_delta(ui, id, &ddx, &ddy)) drag_x += ddx; dx = drag_x; }
	else if (drag_x != 0) { drag_x = 0; }
	Rect r = R(MAX(0, dx), y, sh.w, th_h);
	bool dismiss = false;
	double vx, vy;
	if (ui_fling(ui, id, &vx, &vy)) {
		if (dx > 80 || vx > 600) dismiss = true;
		drag_x = 0;
	}
	if (slide > 0.05) ui_hit_drag(ui, id, r);
	gfx_fill_rect(cr, r, ui->th.accent);
	Color fg = ui->th.on_accent;
	Rect ir = R(r.x + 14, r.y + 14, 48, 48);
	chrome_notif_icon_draw(ui, n, ir, fg);
	if (*n->app_name) gfx_text(cr, i18n_upper(n->app_name), r.x + 76, r.y + 6, 12, FW_SEMIBOLD, col_alpha(fg, 0.8f), TA_LEFT, sh.w - 90, 1);
	gfx_text(cr, n->summary, r.x + 76, r.y + 20, 19, FW_SEMIBOLD, fg, TA_LEFT, sh.w - 90, 1);
	if (*n->body) gfx_text(cr, n->body, r.x + 76, r.y + 44, 15, FW_REGULAR, col_alpha(fg, 0.92f), TA_LEFT, sh.w - 90, 1);
	if (ui_clicked(ui, id) && !sh.locked) { toast_activate(n); dismiss = true; }
	if (age > life) dismiss = true;
	if (dismiss) {
		n->toasted = true;
		vec_remove(&sh.toasts, 0);
		if (n->internal) shell_notify_remove(n->id);
		if (sh.toasts.len) sh.toasts.items[0]->toast_t0 = 0;
		drag_x = 0;
		ui->redraw = true;
	}
}

/* -------------------------------------------------------------- volume HUD */

void chrome_volume_hud(UI *ui) {
	if (sh.volume_hud_t == 0) return;
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double t0 = fabs(sh.volume_hud_t);
	bool is_vol = sh.volume_hud_t > 0;
	double age = ui->now - t0;
	if (age > 1.8) { sh.volume_hud_t = 0; return; }
	ui->animating = true;
	double slide = age < 0.15 ? age / 0.15 : age > 1.6 ? 1 - (age - 1.6) / 0.2 : 1;
	double h = 62, y = -h + slide * h;
	Rect r = R(0, y, sh.w, h);
	gfx_fill_rect(cr, r, th->chrome);
	int v = is_vol ? (sys_muted() ? 0 : sys_volume()) : sys_brightness();
	Icon ic = is_vol ? (sys_muted() ? ICON_MUTE : ICON_VOLUME) : ICON_BRIGHTNESS;
	icon_draw(cr, ic, 36, y + h / 2, 26, th->fg);
	double bx = 66, bw = sh.w - bx - 70;
	gfx_fill_rect(cr, R(bx, y + h / 2 - 3, bw, 6), th->chrome2);
	gfx_fill_rect(cr, R(bx, y + h / 2 - 3, bw * v / 100.0, 6), th->accent);
	gfx_fill_rect(cr, R(bx + bw * v / 100.0 - 5, y + h / 2 - 15, 10, 30), th->fg);
	char b[16];
	snprintf(b, sizeof b, "%d", v);
	gfx_text(cr, b, sh.w - 18, y + h / 2 - 14, 22, FW_SEMILIGHT, th->fg, TA_RIGHT, 0, 1);
}

#include "shell.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../sys.h"
#include "../util.h"

#include <math.h>

/* Action Center: quick actions + notification list. Pulled down from the top. */

static Scroll g_scroll;
static bool g_expanded;

typedef struct { const char *id; const char *label; Icon icon; } Quick;

static bool quick_state(const char *id) {
	if (str_eq(id, "wifi")) return sys_wifi().enabled;
	if (str_eq(id, "bt")) return sys_bluetooth_enabled();
	if (str_eq(id, "air")) return sys_airplane();
	if (str_eq(id, "quiet")) return cfg.quiet_hours;
	if (str_eq(id, "saver")) return cfg.battery_saver;
	if (str_eq(id, "bright")) return true;
	return false;
}

static void quick_toggle(const char *id) {
	if (str_eq(id, "wifi")) {
		if (sys_wifi().available || sys_demo()) sys_wifi_set_enabled(!sys_wifi().enabled);
		else shell_toast(_("Wi-Fi"), _("NetworkManager (nmcli) is not available"), NULL);
	} else if (str_eq(id, "bt")) {
		if (sys_bluetooth_available()) sys_bluetooth_set(!sys_bluetooth_enabled());
		else shell_toast(_("Bluetooth"), _("No Bluetooth adapter found"), NULL);
	} else if (str_eq(id, "air")) sys_set_airplane(!sys_airplane());
	else if (str_eq(id, "quiet")) { cfg.quiet_hours = !cfg.quiet_hours; config_save_soon(); }
	else if (str_eq(id, "saver")) { cfg.battery_saver = !cfg.battery_saver; config_save_soon(); if (cfg.battery_saver) sys_set_brightness(MIN(sys_brightness(), 40)); }
	else if (str_eq(id, "shot")) { shell_toggle_action_center(false); shell_screenshot(); }
	else if (str_eq(id, "kbd")) { shell_toggle_action_center(false); shell_show_keyboard(!osk_wanted()); }
	else if (str_eq(id, "settings")) { shell_toggle_action_center(false); shell_open_builtin("settings", NULL); }
	else if (str_eq(id, "lock")) { shell_lock(false); }
	else if (str_eq(id, "bright")) {
		/* cycles low -> medium -> high like the phone's brightness quick action */
		int b = sys_brightness();
		sys_set_brightness(b < 30 ? 50 : b < 60 ? 80 : b < 90 ? 100 : 25);
		sh.volume_hud_t = -now_sec();
	}
}

static const char *quick_deeplink(const char *id) {
	if (str_eq(id, "wifi")) return "wifi";
	if (str_eq(id, "bt")) return "bluetooth";
	if (str_eq(id, "air")) return "airplane";
	if (str_eq(id, "saver")) return "battery";
	if (str_eq(id, "bright")) return "display";
	if (str_eq(id, "quiet")) return "notifications";
	return NULL;
}

bool actioncenter_back(void) {
	if (sh.ac_target_open || sh.ac_pos > 0.01) { shell_toggle_action_center(false); return true; }
	return false;
}

static Image *notif_img(const Notification *n) {
	if (*n->desktop_entry) { DesktopApp *a = desktop_find(n->desktop_entry); if (a) return desktop_icon(a, 96); }
	if (*n->icon) { Image *i = desktop_icon_by_name(n->icon, 96); if (i) return i; }
	DesktopApp *a = *n->app_name ? desktop_find_by_app_id(n->app_name) : NULL;
	return a ? desktop_icon(a, 96) : NULL;
}

void actioncenter_draw(UI *ui) {
	if (sh.ac_pos <= 0.001) return;
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double pos = sh.ac_pos;
	double oy = -(1 - pos) * sh.h;
	cairo_save(cr);
	cairo_translate(cr, 0, oy);
	Rect panel = R(0, 0, sh.w, sh.h);
	gfx_fill_rect(cr, panel, th->bg);
	ui_hit(ui, ui_id("ac-panel"), panel);
	ui->layer = UL_PANEL;
	chrome_status_draw(ui, R(0, 0, sh.w, sh.status_h), false);

	/* ---- quick actions */
	static const Quick Q[] = {
		{"wifi", N_("Wi-Fi"), ICON_WIFI}, {"bt", N_("Bluetooth"), ICON_BLUETOOTH},
		{"air", N_("Airplane mode"), ICON_AIRPLANE}, {"quiet", N_("Quiet hours"), ICON_MOON},
		{"saver", N_("Battery saver"), ICON_BATTERY}, {"shot", N_("Screenshot"), ICON_SCREENSHOT},
		{"kbd", N_("Keyboard"), ICON_KEYBOARD}, {"bright", N_("Brightness"), ICON_BRIGHTNESS},
	};
	static const Quick LOCK = {"lock", N_("Lock"), ICON_LOCK};
	double pad = 10, gap = 4, y = sh.status_h + 4;

	/* ---- header: date on the left, ALL SETTINGS on the right (WP 8.1) */
	{
		time_t tt = time(NULL);
		struct tm tm;
		localtime_r(&tt, &tm);
		char date[96];
		i18n_fmt_date_long(date, sizeof date, &tm);
		gfx_text(cr, date, 16, y + 4, 20, FW_SEMILIGHT, th->fg, TA_LEFT, sh.w * 0.6, 1);
		const char *all = i18n_upper(_("all settings"));
		Size as = gfx_text_size(cr, all, 13, FW_SEMIBOLD, 0, 1);
		Rect ar = R(sh.w - 16 - as.w - 12, y, as.w + 24, 34);
		UiId aid = ui_id("ac-allsettings");
		ui_hit(ui, aid, ar);
		gfx_text(cr, all, sh.w - 16, y + 9, 13, FW_SEMIBOLD, ui_pressed(ui, aid) ? th->sub : th->fg, TA_RIGHT, 0, 1);
		if (ui_clicked(ui, aid)) { shell_toggle_action_center(false); shell_open_builtin("settings", NULL); }
		y += 38;
	}
	int per_row = 4;
	double qw = (sh.w - 2 * pad - gap * (per_row - 1)) / per_row, qh = MIN(qw, 84);
	int shown = g_expanded ? 8 : 4;
	for (int i = 0; i < shown; i++) {
		int r = i / per_row, c = i % per_row;
		Rect qr = R(pad + c * (qw + gap), y + r * (qh + gap), qw, qh);
		const Quick *q = &Q[i];
		if (str_eq(q->id, "bright") && !sys_has_backlight()) q = &LOCK;
		bool on = quick_state(q->id);
		UiId id = ui_idn("ac-quick", i);
		ui_hit(ui, id, qr);
		bool pr = ui_pressed(ui, id);
		Color bg = on ? th->accent : th->chrome2;
		if (pr) bg = th->fg;
		gfx_fill_rect(cr, qr, bg);
		Color fg = pr ? th->bg : (on ? th->on_accent : th->fg);
		Icon ic = q->icon;
		if (str_eq(q->id, "battery") ) ic = ICON_BATTERY;
		if (ic == ICON_BATTERY) icon_draw_battery(cr, rect_cx(qr), qr.y + qh * 0.4, 30, fg, sys_battery().present ? sys_battery().pct : 100, false);
		else icon_draw(cr, ic, rect_cx(qr), qr.y + qh * 0.4, 26, fg);
		gfx_text(cr, tr(q->label), rect_cx(qr), qr.y + qh - 22, 12, FW_REGULAR, fg, TA_CENTER, qw - 6, 1);
		if (ui_clicked(ui, id)) quick_toggle(q->id);
		if (ui_longpressed(ui, id)) {
			const char *dl = quick_deeplink(q->id);
			if (dl) { shell_toggle_action_center(false); shell_open_builtin("settings", dl); }
		}
	}
	y += ((shown + per_row - 1) / per_row) * (qh + gap);
	/* expand / collapse */
	{
		Rect er = R(sh.w - 56, y - 2, 46, 30);
		UiId eid = ui_id("ac-expand");
		ui_hit(ui, eid, R(er.x - 20, er.y - 4, er.w + 30, er.h + 8));
		icon_draw(cr, g_expanded ? ICON_CHEVRON_U : ICON_CHEVRON_D, rect_cx(er), rect_cy(er), 16, ui_pressed(ui, eid) ? th->sub : th->fg);
		gfx_text(cr, g_expanded ? _("collapse") : _("expand"), er.x - 4, rect_cy(er) - 9, 14, FW_REGULAR, th->sub, TA_RIGHT, 0, 1);
		if (ui_clicked(ui, eid)) g_expanded = !g_expanded;
	}
	y += 34;

	/* ---- notifications header */
	int count = 0;
	for (int i = 0; i < sh.notifs.len; i++) if (!sh.notifs.items[i]->internal) count++;
	gfx_text(cr, i18n_upper(_("notifications")), 16, y + 2, 14, FW_SEMIBOLD, th->sub, TA_LEFT, sh.w / 2, 1);
	if (count) {
		Rect cr_ = R(sh.w - 120, y - 4, 110, 32);
		UiId cid = ui_id("ac-clear");
		ui_hit(ui, cid, cr_);
		gfx_text(cr, _("clear all"), sh.w - 16, y + 1, 16, FW_SEMILIGHT, ui_pressed(ui, cid) ? th->sub : th->accent, TA_RIGHT, 0, 1);
		if (ui_clicked(ui, cid)) shell_notify_clear();
	}
	y += 30;

	/* ---- list */
	double bottom = sh.h - 44;
	Rect view = R(0, y, sh.w, bottom - y);
	if (!count) {
		gfx_text(cr, _("No new notifications"), sh.w / 2, y + 40, 20, FW_SEMILIGHT, th->dim, TA_CENTER, sh.w - 40, 1);
	} else {
		const double ROW_H = 88;
		double content_h = count * ROW_H;
		double off = ui_scroll_begin(ui, ui_id("ac-scroll"), &g_scroll, view, content_h);
		cairo_translate(cr, 0, view.y);
		double ry = 0;
		time_t now_t = time(NULL);
		for (int k = sh.notifs.len - 1; k >= 0; k--) {
			Notification *n = sh.notifs.items[k];
			if (n->internal) continue;
			UiId id = ui_idn("ac-notif", (int)n->id);
			double *st = ui_state(ui, id, 0);   /* st[0] = x offset, st[1] = removal progress */
			double h = ROW_H * (1 - st[1]);
			if (st[1] >= 1) { shell_notify_remove(n->id); st[0] = st[1] = 0; ui->redraw = true; break; }
			Rect row = R(0, ry, sh.w, ROW_H);
			bool visible = ry + h > off && ry < off + view.h;
			if (visible && st[1] == 0) {
				ui_scroller_push(ui, id, row, UI_AXIS_X);
				ui_hit(ui, id, row);
				double dx, dy, vx, vy;
				if (ui_drag_delta(ui, id, &dx, &dy)) st[0] = MAX(0, st[0] + dx);
				if (ui_fling(ui, id, &vx, &vy)) {
					if (st[0] > sh.w * 0.3 || vx > 700) st[1] = 0.001;
				}
				if (!ui_dragging(ui, id) && st[1] == 0 && st[0] != 0) st[0] = ui_smooth(ui, st[0], 0, 18);
				ui_scroller_pop(ui);
			}
			if (st[1] > 0) {
				st[0] += sh.w * 3 * ui->dt;
				st[1] = MIN(1, st[1] + ui->dt * 4.5);
				ui->animating = true;
			}
			if (visible && h > 1) {
				cairo_save(cr);
				cairo_rectangle(cr, 0, ry, sh.w, MAX(0, h));
				cairo_clip(cr);
				cairo_translate(cr, st[0], 0);
				bool pr = ui_pressed(ui, id);
				if (pr) gfx_fill_rect(cr, row, col_alpha(th->fg, 0.12f));
				Rect ir = R(16, ry + 14, 44, 44);
				gfx_fill_rect(cr, ir, th->accent);
				Image *img = notif_img(n);
				if (img) gfx_image_silhouette(cr, img, rect_inset(ir, 7), th->on_accent);
				else icon_draw(cr, ICON_MESSAGE, rect_cx(ir), rect_cy(ir), 24, th->on_accent);
				double tx = 74, tw = sh.w - tx - 16;
				char ago[48];
				i18n_fmt_ago(ago, sizeof ago, n->time, now_t);
				Size as = gfx_text_size(cr, ago, 13, FW_REGULAR, 0, 1);
				gfx_text(cr, ago, sh.w - 14, ry + 10, 13, FW_REGULAR, th->sub, TA_RIGHT, 0, 1);
				if (*n->app_name) gfx_text(cr, i18n_upper(n->app_name), tx, ry + 10, 12, FW_SEMIBOLD, th->sub, TA_LEFT, tw - as.w - 10, 1);
				gfx_text(cr, n->summary, tx, ry + 26, 20, FW_SEMIBOLD, th->fg, TA_LEFT, tw, 1);
				if (*n->body) gfx_text(cr, n->body, tx, ry + 50, 15, FW_REGULAR, th->sub, TA_LEFT, tw, 2);
				cairo_restore(cr);
				if (ui_clicked(ui, id)) {
					shell_toggle_action_center(false);
					shell_notify_activate(n);
				}
			}
			ry += h;
		}
		ui_scroll_end(ui);
	}

	/* ---- bottom handle: drag up to close */
	{
		Rect hr = R(0, sh.h - 44, sh.w, 44);
		gfx_fill_rect(cr, hr, th->bg);
		UiId hid = ui_id("ac-handle");
		ui_hit_drag(ui, hid, hr);
		icon_draw(cr, ICON_CHEVRON_U, sh.w / 2, sh.h - 22, 18, th->sub);
		if (ui_dragging(ui, hid) && ui->p.moved) {
			sh.ac_dragging = true;
			sh.ac_anim.active = false;
			sh.ac_pos = CLAMP(ui->p.y / (sh.h - 22), 0, 1);
			ui->redraw = true;
		}
		double vx, vy;
		if (ui_fling(ui, hid, &vx, &vy) && (sh.ac_dragging || ui->p.moved)) {
			/* a short push up is enough, see the pull-down in chrome.c */
			sh.ac_pos = CLAMP(ui->p.y / (sh.h - 22), 0, 1);
			bool close = sh.ac_pos < 0.9 || vy < -300;
			sh.ac_dragging = true;   /* makes the toggle animate from where the finger left it */
			shell_toggle_action_center(!close);
		}
		if (ui_clicked(ui, hid)) shell_toggle_action_center(false);
	}
	cairo_restore(cr);
}

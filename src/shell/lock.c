#include "shell.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../sys.h"
#include "../util.h"

#include <math.h>

/* Lock screen: wallpaper, big clock, notifications; swipe up to unlock (or enter the PIN). */

static bool g_pin_mode;
static char g_pin[16];
static Anim g_pin_anim;          /* keypad slide-in */
static double g_shake_t0;
static bool g_wrong;
static Anim g_snap_back;
static bool g_snapping;
static double g_drag_start_y;
static double g_drag_base_pos;

void lock_reset(void) {
	g_pin_mode = false;
	g_pin[0] = 0;
	g_wrong = false;
	anim_set(&g_pin_anim, 0);
	g_snapping = false;
}

bool lock_is_busy(void) { return g_pin_mode; }

bool lock_back(void) {
	if (g_pin_mode) {
		g_pin_mode = false;
		g_pin[0] = 0;
		anim_start(&g_pin_anim, 1, 0, sh.ui.now ? sh.ui.now : now_sec(), 0.25, EASE_OUT_CUBIC);
		shell_invalidate();
		return true;
	}
	return false;
}

static void try_unlock(UI *ui) {
	if (!cfg.pin_hash[0]) { shell_unlock(); return; }
	if (!g_pin_mode) {
		g_pin_mode = true;
		g_pin[0] = 0;
		anim_start(&g_pin_anim, 0, 1, ui->now ? ui->now : now_sec(), 0.3, EASE_OUT_CUBIC);
		shell_invalidate();
	}
}

static void submit_pin(UI *ui) {
	char h[65];
	sha256_hex(g_pin, strlen(g_pin), h);
	if (str_eq(h, cfg.pin_hash)) {
		g_pin_mode = false;
		g_pin[0] = 0;
		shell_unlock();
	} else {
		g_wrong = true;
		g_shake_t0 = ui->now ? ui->now : now_sec();
		g_pin[0] = 0;
	}
	shell_invalidate();
}

void lock_key(UiKey k, const char *utf8) {
	if (!sh.locked) return;
	if (!g_pin_mode) {
		if (k == UK_ENTER || k == UK_UP || (utf8 && utf8[0] == ' ')) try_unlock(&sh.ui);
		else if (utf8 && utf8[0] >= '0' && utf8[0] <= '9') { try_unlock(&sh.ui); if (g_pin_mode) { g_pin[0] = utf8[0]; g_pin[1] = 0; } }
		return;
	}
	size_t n = strlen(g_pin);
	if (k == UK_BACKSPACE) { if (n) g_pin[n - 1] = 0; }
	else if (k == UK_ESC) lock_back();
	else if (k == UK_ENTER) { if (n >= 4) submit_pin(&sh.ui); }
	else if (utf8 && utf8[0] >= '0' && utf8[0] <= '9' && n < 12) { g_pin[n] = utf8[0]; g_pin[n + 1] = 0; }
}

static void draw_notifications(UI *ui, double x, double y, double w) {
	cairo_t *cr = ui->cr;
	int shown = 0;
	for (int i = sh.notifs.len - 1; i >= 0 && shown < 2; i--) {
		Notification *n = sh.notifs.items[i];
		if (n->internal) continue;
		double ry = y + shown * 50;
		icon_draw(cr, ICON_MESSAGE, x + 14, ry + 20, 22, RGBX(0xffffff));
		gfx_text(cr, n->summary, x + 40, ry, 18, FW_SEMIBOLD, COL_WHITE, TA_LEFT, w - 40, 1);
		if (*n->body) gfx_text(cr, n->body, x + 40, ry + 22, 15, FW_REGULAR, RGBA(0xffffff, 0.8f), TA_LEFT, w - 40, 1);
		shown++;
	}
}

static void draw_keypad(UI *ui, double top, double alpha) {
	cairo_t *cr = ui->cr;
	double kw = MIN(86, (sh.w - 80) / 3), gap = 10;
	double gx = (sh.w - (kw * 3 + gap * 2)) / 2;
	static const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "", "0", "<"};
	for (int i = 0; i < 12; i++) {
		if (!keys[i][0]) continue;
		int r = i / 3, c = i % 3;
		Rect kr = R(gx + c * (kw + gap), top + r * (kw * 0.78 + gap), kw, kw * 0.78);
		UiId id = ui_idn("pin-key", i);
		if (ui->layer >= 0) ui_hit(ui, id, kr);
		bool pr = ui_pressed(ui, id);
		gfx_fill_rect(cr, kr, pr ? col_alpha(COL_WHITE, 0.95f * (float)alpha) : RGBA(0xffffff, 0.16f * (float)alpha));
		if (keys[i][0] == '<') icon_draw(cr, ICON_BACKSPACE, rect_cx(kr), rect_cy(kr), 26, pr ? COL_BLACK : col_alpha(COL_WHITE, (float)alpha));
		else gfx_text_in(cr, keys[i], kr, 34, FW_SEMILIGHT, pr ? COL_BLACK : col_alpha(COL_WHITE, (float)alpha), TA_CENTER, true);
		if (ui_clicked(ui, id) && sh.locked) {
			size_t n = strlen(g_pin);
			if (keys[i][0] == '<') { if (n) g_pin[n - 1] = 0; }
			else if (n < 12) { g_pin[n] = keys[i][0]; g_pin[n + 1] = 0; }
			g_wrong = false;
		}
	}
	/* confirm button */
	Rect ok = R(gx, top + 4 * (kw * 0.78 + gap), kw * 3 + gap * 2, 44);
	UiId oid = ui_id("pin-ok");
	ui_hit(ui, oid, ok);
	bool pr = ui_pressed(ui, oid);
	gfx_fill_rect(cr, ok, pr ? COL_WHITE : col_alpha(ui->th.accent, (float)alpha));
	gfx_text_in(cr, _("enter"), ok, 22, FW_SEMILIGHT, pr ? COL_BLACK : col_alpha(ui->th.on_accent, (float)alpha), TA_CENTER, true);
	if (ui_clicked(ui, oid) && sh.locked && strlen(g_pin) >= 1) submit_pin(ui);
}

void lock_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	double pos = shell_lock_pos();
	if (pos >= 0.999 && !sh.locked) return;
	double off = -pos * sh.h;
	UiId lid = ui_id("lock-drag");
	bool active = sh.locked;
	Rect full = R(0, 0, sh.w, sh.h);
	if (active && !g_pin_mode) ui_hit_drag(ui, lid, full);
	else if (active) ui_hit(ui, lid, full);
	cairo_save(cr);
	cairo_translate(cr, 0, off);

	/* wallpaper */
	Image *wp = wallpaper_lock(sh.w, sh.h);
	if (wp) {
		gfx_image_cover(cr, wp, full, 1.0);
	} else {
		wallpaper_draw_generated(cr, full, ui->th.accent, ui->th.dark, 7);
	}
	double dim = g_pin_mode ? 0.55 : 0.18;
	double pa = ui_anim(ui, &g_pin_anim);
	gfx_fill_rect(cr, full, RGBA(0x000000, (float)(0.18 + (dim - 0.18) * pa)));

	if (active && !g_pin_mode) {
		double dx, dy;
		if (ui_dragging(ui, lid)) {
			if (!shell_lock_dragging()) { g_drag_start_y = ui->p.y; g_drag_base_pos = pos; ui->p.fy = ui->p.y; }
			if (ui_drag_delta(ui, lid, &dx, &dy)) {
				shell_lock_set_pos(pos - dy / sh.h, true);
			}
		}
		double vx, vy;
		if (ui_fling(ui, lid, &vx, &vy)) {
			double p = shell_lock_pos();
			bool open = p > 0.28 || vy < -500;
			shell_lock_set_pos(p, false);
			if (open && ui->p.moved) {
				if (cfg.pin_hash[0]) {
					/* snap back and ask for the PIN */
					anim_start(&g_snap_back, p, 0, ui->now, 0.28, EASE_OUT_CUBIC);
					g_snapping = true;
					try_unlock(ui);
				} else {
					shell_unlock_from(p);
				}
			} else {
				anim_start(&g_snap_back, p, 0, ui->now, 0.28, EASE_OUT_CUBIC);
				g_snapping = true;
			}
		}
		if (g_snapping) {
			if (anim_running(&g_snap_back, ui->now)) {
				shell_lock_set_pos(anim_value(&g_snap_back, ui->now), false);
				ui->animating = true;
			} else { shell_lock_set_pos(g_snap_back.to, false); g_snapping = false; }
		}
		if (ui_clicked(ui, lid)) { /* a simple tap shows the hint bounce */ g_snap_back = (Anim){0}; anim_start(&g_snap_back, 0.06, 0, ui->now, 0.35, EASE_OUT_CUBIC); g_snapping = true; }
	}

	/* status icons */
	chrome_status_draw(ui, R(0, 0, sh.w, 26), true);

	/* clock and date */
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char tbuf[32], dbuf[64];
	i18n_fmt_time(tbuf, sizeof tbuf, &tm, cfg.h24);
	i18n_fmt_date_long(dbuf, sizeof dbuf, &tm);
	double clock_alpha = 1 - pa;
	double base_y = sh.h - 340;
	/* split "12:34 PM" so the AM/PM marker is small */
	char big[16], small[8] = "";
	str_copy(big, sizeof big, tbuf);
	char *sp = strchr(big, ' ');
	if (sp) { str_copy(small, sizeof small, sp + 1); *sp = 0; }
	Size bs = gfx_text_size(cr, big, 104, FW_LIGHT, 0, 1);
	gfx_text(cr, big, 22, base_y, 104, FW_LIGHT, col_alpha(COL_WHITE, (float)clock_alpha), TA_LEFT, 0, 1);
	if (small[0]) gfx_text(cr, small, 28 + bs.w, base_y + bs.h * 0.42, 26, FW_SEMILIGHT, col_alpha(COL_WHITE, (float)clock_alpha), TA_LEFT, 0, 1);
	gfx_text(cr, dbuf, 26, base_y + bs.h - 6, 28, FW_SEMILIGHT, col_alpha(COL_WHITE, (float)clock_alpha), TA_LEFT, sh.w - 40, 1);
	if (clock_alpha > 0.3) {
		cairo_push_group(cr);
		draw_notifications(ui, 26, base_y + bs.h + 40, sh.w - 52);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, clock_alpha);
	}

	/* swipe hint */
	if (!g_pin_mode) {
		double bob = sin(ui->now * 3) * 4;
		icon_draw(cr, ICON_CHEVRON_U, sh.w / 2, sh.h - 34 + bob, 18, col_alpha(COL_WHITE, (float)(0.8 * clock_alpha)));
		ui->animating = true;
	}

	/* PIN entry */
	if (pa > 0.01) {
		double shake = 0;
		if (g_wrong) {
			double st = ui->now - g_shake_t0;
			if (st < 0.5) { shake = sin(st * 50) * 12 * (1 - st / 0.5); ui->animating = true; }
		}
		gfx_text(cr, _("Enter your password"), sh.w / 2, 70 + (1 - pa) * 40, 24, FW_SEMILIGHT, col_alpha(COL_WHITE, (float)pa), TA_CENTER, sh.w - 40, 1);
		size_t n = strlen(g_pin);
		double dots = MAX((double)n, 4.0);
		double dw = 22, dx0 = sh.w / 2 - dots * dw / 2 + shake;
		for (int i = 0; i < (int)dots; i++) {
			double cx = dx0 + i * dw + dw / 2, cy = 140;
			if (i < (int)n) gfx_fill_circle(cr, cx, cy, 7, col_alpha(COL_WHITE, (float)pa));
			else gfx_stroke_circle(cr, cx, cy, 7, 2, col_alpha(COL_WHITE, (float)(0.6 * pa)));
		}
		if (g_wrong) gfx_text(cr, _("Wrong password, try again"), sh.w / 2, 170, 16, FW_REGULAR, col_alpha(RGBX(0xff8080), (float)pa), TA_CENTER, sh.w - 40, 1);
		double kw = MIN(86, (sh.w - 80) / 3);
		double top = sh.h - (4 * (kw * 0.78 + 10) + 60 + 20);
		draw_keypad(ui, top + (1 - pa) * 120, pa);
	}
	cairo_restore(cr);
}

#include "shell.h"
#include "../config.h"
#include "../i18n.h"
#include "../util.h"

#include <math.h>

/* Metro style dialogs: message box, list picker, text input and context menu. */

typedef enum { D_NONE, D_MESSAGE, D_LIST, D_INPUT, D_MENU, D_PICK } DKind;

/* One column of a looping picker: `off` is the (fractional) index at the centre. */
typedef struct { int n; double off, target; bool loop; } PCol;
enum { PK_TIME, PK_DATE, PK_LIST };

static struct {
	DKind kind;
	char title[160];
	char text[640];
	char btn[2][64];
	int nbtn;
	char *items[96];
	int n_items;
	int selected;
	DialogCb cb;
	void *ud;
	double t0;
	char input[160];
	char hint[64];
	int tb_flags;
	double mx, my;
	Scroll scroll;
	bool focus_pending;
	/* pickers */
	int pk;
	PCol col[3];
	int ncol;
	bool h24;
	AppBar bar;
} D;

static void finish(int result);

static void free_items(void) {
	for (int i = 0; i < D.n_items; i++) free(D.items[i]);
	D.n_items = 0;
}

static void begin(DKind k, const char *title, DialogCb cb, void *ud) {
	free_items();
	memset(&D.scroll, 0, sizeof D.scroll);
	D.kind = k;
	str_copy(D.title, sizeof D.title, title ? title : "");
	D.text[0] = 0;
	D.nbtn = 0;
	D.cb = cb;
	D.ud = ud;
	D.t0 = now_sec();
	D.selected = -1;
	shell_invalidate();
}

void dialog_message(const char *title, const char *text, const char *btn1, const char *btn2, DialogCb cb, void *ud) {
	begin(D_MESSAGE, title, cb, ud);
	str_copy(D.text, sizeof D.text, text);
	str_copy(D.btn[0], sizeof D.btn[0], btn1 ? btn1 : _("ok"));
	D.nbtn = 1;
	if (btn2) { str_copy(D.btn[1], sizeof D.btn[1], btn2); D.nbtn = 2; }
}

void dialog_list(const char *title, const char *const *items, int n, int selected, DialogCb cb, void *ud) {
	begin(D_LIST, title, cb, ud);
	for (int i = 0; i < n && i < (int)ARRAY_LEN(D.items); i++) D.items[D.n_items++] = xstrdup(items[i]);
	D.selected = selected;
}

void dialog_input(const char *title, const char *hint, const char *initial, int tb_flags, DialogCb cb, void *ud) {
	begin(D_INPUT, title, cb, ud);
	str_copy(D.input, sizeof D.input, initial ? initial : "");
	str_copy(D.hint, sizeof D.hint, hint ? hint : "");
	D.tb_flags = tb_flags;
	D.focus_pending = true;
	str_copy(D.btn[0], sizeof D.btn[0], _("ok"));
	str_copy(D.btn[1], sizeof D.btn[1], _("cancel"));
	D.nbtn = 2;
}

void dialog_menu(double x, double y, const char *const *items, int n, DialogCb cb, void *ud) {
	begin(D_MENU, "", cb, ud);
	for (int i = 0; i < n && i < 24; i++) D.items[D.n_items++] = xstrdup(items[i]);
	D.mx = x;
	D.my = y;
}

const char *dialog_input_text(void) { return D.input; }

/* ------------------------------------------------------------- pickers ---- */

static int col_value(const PCol *c) {
	int v = (int)lround(c->target);
	if (c->loop) v = ((v % c->n) + c->n) % c->n;
	return CLAMP(v, 0, c->n - 1);
}

static void col_set(PCol *c, int n, int v, bool loop) {
	c->n = n;
	c->loop = loop;
	c->off = c->target = CLAMP(v, 0, n - 1);
}

static int days_in_month(int year, int m0) {
	static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	return m0 == 1 && leap ? 29 : d[CLAMP(m0, 0, 11)];
}

#define YEAR0 1900
#define NYEARS 300

/* date column order follows the language: month/day/year or day/month/year */
static int date_col(int what /*0 month 1 day 2 year*/) {
	if (i18n_lang() == LANG_TR) return what == 0 ? 1 : what == 1 ? 0 : 2;
	return what;
}

void dialog_time(const char *title, int hour, int minute, DialogCb cb, void *ud) {
	begin(D_PICK, title ? title : _("choose time"), cb, ud);
	D.pk = PK_TIME;
	D.h24 = cfg.h24;
	D.bar = (AppBar){0};
	if (D.h24) {
		col_set(&D.col[0], 24, hour, true);
		D.ncol = 2;
	} else {
		col_set(&D.col[0], 12, hour % 12, true);
		col_set(&D.col[2], 2, hour >= 12, false);
		D.ncol = 3;
	}
	col_set(&D.col[1], 60, minute, true);
}

void dialog_time_get(int *hour, int *minute) {
	int h = col_value(&D.col[0]);
	if (!D.h24) h = h % 12 + (col_value(&D.col[2]) ? 12 : 0);
	*hour = h;
	*minute = col_value(&D.col[1]);
}

void dialog_date(const char *title, int year, int month0, int day, DialogCb cb, void *ud) {
	begin(D_PICK, title ? title : _("choose date"), cb, ud);
	D.pk = PK_DATE;
	D.bar = (AppBar){0};
	D.ncol = 3;
	col_set(&D.col[date_col(0)], 12, month0, true);
	col_set(&D.col[date_col(1)], days_in_month(year, month0), day - 1, true);
	col_set(&D.col[date_col(2)], NYEARS, year - YEAR0, false);
}

void dialog_date_get(int *year, int *month0, int *day) {
	*month0 = col_value(&D.col[date_col(0)]);
	*year = YEAR0 + col_value(&D.col[date_col(2)]);
	*day = MIN(col_value(&D.col[date_col(1)]) + 1, days_in_month(*year, *month0));
}

void dialog_picker(const char *title, const char *const *labels, int n, int selected, DialogCb cb, void *ud) {
	begin(D_PICK, title, cb, ud);
	D.pk = PK_LIST;
	D.bar = (AppBar){0};
	for (int i = 0; i < n && i < 24; i++) D.items[D.n_items++] = xstrdup(labels[i]);
	D.ncol = 1;
	col_set(&D.col[0], D.n_items, selected, D.n_items > 4);
}

int dialog_picker_get(void) { return col_value(&D.col[0]); }

/* label of value v in column c: big text and small caption */
static void pick_label(int c, int v, char *big, size_t nb, char *small, size_t ns) {
	small[0] = 0;
	if (D.pk == PK_LIST) { str_copy(big, nb, D.items[v]); return; }
	if (D.pk == PK_TIME) {
		if (c == 0) snprintf(big, nb, D.h24 ? "%02d" : "%d", D.h24 ? v : (v == 0 ? 12 : v));
		else if (c == 1) snprintf(big, nb, "%02d", v);
		else str_copy(big, nb, v ? (i18n_lang() == LANG_TR ? "ÖS" : "PM") : (i18n_lang() == LANG_TR ? "ÖÖ" : "AM"));
		return;
	}
	if (c == date_col(0)) { snprintf(big, nb, "%02d", v + 1); str_copy(small, ns, i18n_lower(i18n_month(v, false))); }
	else if (c == date_col(1)) {
		int y = YEAR0 + col_value(&D.col[date_col(2)]), m = col_value(&D.col[date_col(0)]);
		struct tm tm = {.tm_year = y - 1900, .tm_mon = m, .tm_mday = v + 1, .tm_hour = 12};
		mktime(&tm);
		snprintf(big, nb, "%02d", v + 1);
		str_copy(small, ns, i18n_lower(i18n_weekday(tm.tm_wday, false)));
	} else snprintf(big, nb, "%d", YEAR0 + v);
}

static void draw_picker(UI *ui, double slide) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect page = R(0, sh.status_h, sh.w, sh.h - sh.status_h - sh.nav_h);
	gfx_fill_rect(cr, page, th->bg);
	gfx_text(cr, i18n_upper(D.title), 24, page.y + 14 + slide, 17, FW_SEMIBOLD, th->fg, TA_LEFT, sh.w - 48, 1);
	if (D.pk == PK_DATE) {
		/* keep the day column valid for the chosen month */
		PCol *dc = &D.col[date_col(1)];
		int dim = days_in_month(YEAR0 + col_value(&D.col[date_col(2)]), col_value(&D.col[date_col(0)]));
		if (dc->n != dim) {
			int d = MIN(col_value(dc), dim - 1);
			dc->n = dim;
			dc->off = dc->target = d;
		}
	}
	Rect body = appbar_content(&D.bar, R(0, page.y + 50, sh.w, page.h - 50));
	double gap = 12, margin = 24;
	double cw = D.pk == PK_LIST ? sh.w - 2 * margin : MIN(150, (sh.w - 2 * margin - (D.ncol - 1) * gap) / D.ncol);
	double ih = D.pk == PK_LIST ? 72 : cw, step = ih + gap;
	double x0 = margin;
	double mid = rect_cy(body);
	ui_clip_push(ui, body);
	for (int c = 0; c < D.ncol; c++) {
		PCol *pc = &D.col[c];
		Rect cr_ = R(x0 + c * (cw + gap), body.y, cw, body.h);
		UiId id = ui_idn("pick-col", c);
		ui_scroller_push(ui, id, cr_, UI_AXIS_Y);
		ui_hit(ui, id, cr_);
		double dx, dy, vx, vy, wx, wy;
		bool dragging = ui_dragging(ui, id);
		if (ui_drag_delta(ui, id, &dx, &dy)) pc->off -= dy / step;
		if (ui_fling(ui, id, &vx, &vy)) pc->target = round(pc->off - vy / step * 0.3);
		else if (dragging) pc->target = round(pc->off);
		if (ui_wheel_for(ui, cr_, &wx, &wy)) pc->target += wy > 0 ? 1 : -1;
		if (!pc->loop) pc->target = CLAMP(pc->target, 0, pc->n - 1);
		if (!dragging) {
			pc->off = ui_smooth(ui, pc->off, pc->target, 16);
		} else if (!pc->loop) {
			pc->off = CLAMP(pc->off, -0.4, pc->n - 0.6);
		}
		bool active = dragging || fabs(pc->off - pc->target) > 0.01;
		int base = (int)floor(pc->off);
		int span = (int)ceil(body.h / 2 / step) + 1;
		for (int j = -span; j <= span; j++) {
			int idx = base + j;
			double pos = idx - pc->off;
			int v = idx;
			if (pc->loop) v = ((idx % pc->n) + pc->n) % pc->n;
			else if (idx < 0 || idx >= pc->n) continue;
			Rect ir = R(cr_.x, mid - ih / 2 + pos * step, cw, ih);
			if (rect_bottom(ir) < body.y || ir.y > rect_bottom(body)) continue;
			bool sel = idx == (int)lround(pc->target);
			UiId iid = ui_idn("pick-item", c * 1000 + idx + 500);
			ui_hit(ui, iid, ir);
			if (ui_clicked(ui, iid)) pc->target = idx;
			char big[64], small[64];
			pick_label(c, v, big, sizeof big, small, sizeof small);
			float fade = sel ? 1.f : (active ? 0.75f : 0.45f);
			if (sel) gfx_fill_rect(cr, ir, th->accent);
			else gfx_stroke_rect(cr, ir, 2, col_alpha(th->dim, fade));
			Color fg = sel ? th->on_accent : col_alpha(th->fg, fade);
			if (D.pk == PK_LIST) {
				gfx_text(cr, big, ir.x + 14, ir.y + (ih - gfx_line_height(cr, 28, FW_SEMILIGHT)) / 2, 28, FW_SEMILIGHT, fg,
				         TA_LEFT, cw - 28, 1);
			} else {
				double bs = cw * 0.36;
				double by = small[0] ? ir.y + ih - 8 - bs * 1.3 - 18 : ir.y + ih - 8 - bs * 1.3;
				gfx_text(cr, big, ir.x + 10, by, bs, FW_SEMILIGHT, fg, TA_LEFT, cw - 16, 1);
				if (small[0]) gfx_text(cr, small, ir.x + 10, ir.y + ih - 28, 15, FW_REGULAR, fg, TA_LEFT, cw - 16, 1);
			}
		}
		ui_scroller_pop(ui);
	}
	ui_clip_pop(ui);
	static const AppBarBtn btns[2] = {{ICON_CHECK, N_("done")}, {ICON_CLOSE, N_("cancel")}};
	AppBarBtn b[2] = {{ICON_CHECK, _(btns[0].label)}, {ICON_CLOSE, _(btns[1].label)}};
	int a = appbar_draw(ui, &D.bar, page, b, 2, NULL, 0);
	if (a == 0) finish(0);
	else if (a == 1) finish(-1);
}
bool dialog_active(void) { return D.kind != D_NONE; }

void dialog_close(void) {
	if (D.kind == D_NONE) return;
	D.kind = D_NONE;
	sh.ui.focus = 0;
	free_items();
	shell_invalidate();
}

static void finish(int result) {
	DialogCb cb = D.cb;
	void *ud = D.ud;
	/* keep the text readable for input dialogs during the callback */
	DKind k = D.kind;
	D.kind = D_NONE;
	sh.ui.focus = 0;
	if (cb) cb(result, ud);
	if (D.kind == D_NONE) free_items();
	(void)k;
	shell_invalidate();
}

bool dialog_back(void) {
	if (D.kind == D_NONE) return false;
	finish(-1);
	return true;
}

void dialog_draw(UI *ui) {
	if (D.kind == D_NONE) return;
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double age = ui->now - D.t0;
	double a = CLAMP(age / 0.18, 0, 1);
	if (a < 1) ui->animating = true;
	/* the navigation bar stays usable: Back cancels the dialog, Start closes it */
	Rect full = R(0, 0, sh.w, sh.locked ? sh.h : sh.h - sh.nav_h);
	UiId scrim = ui_id("dlg-scrim");
	ui_hit(ui, scrim, full);

	if (D.kind == D_PICK) {
		draw_picker(ui, (1 - ease(EASE_OUT_CUBIC, a)) * 20);
		return;
	}

	if (D.kind == D_MENU) {
		gfx_fill_rect(cr, full, RGBA(0x000000, 0.0f));
		if (ui_clicked(ui, scrim)) { finish(-1); return; }
		double w = MIN(280, sh.w - 40), rh = 54, h = D.n_items * rh;
		double x = CLAMP(D.mx - w / 2, 12, sh.w - w - 12);
		double y = CLAMP(D.my - h / 2, sh.status_h + 8, sh.h - sh.nav_h - h - 8);
		Rect m = R(x, y, w, h);
		gfx_fill_rect(cr, R(m.x - 4, m.y - 4, m.w + 8, m.h + 8), col_alpha(th->bg, 0.5f));
		gfx_fill_rect(cr, m, th->dark ? RGBX(0x2a2a2a) : RGBX(0xf0f0f0));
		gfx_stroke_rect(cr, m, 2, th->fg);
		for (int i = 0; i < D.n_items; i++) {
			Rect ir = R(m.x, m.y + i * rh, m.w, rh);
			UiId id = ui_idn("dlg-menu", i);
			ui_hit(ui, id, ir);
			if (ui_pressed(ui, id)) gfx_fill_rect(cr, ir, th->accent);
			gfx_text(cr, D.items[i], ir.x + 18, ir.y + (rh - gfx_line_height(cr, 22, FW_SEMILIGHT)) / 2, 22, FW_SEMILIGHT,
			         ui_pressed(ui, id) ? th->on_accent : th->fg, TA_LEFT, ir.w - 30, 1);
			if (ui_clicked(ui, id)) { finish(i); return; }
		}
		return;
	}

	gfx_fill_rect(cr, full, col_alpha(th->dark ? COL_BLACK : COL_WHITE, (float)(0.72 * a)));
	double slide = (1 - ease(EASE_OUT_CUBIC, a)) * -30;

	if (D.kind == D_LIST) {
		/* full width list picker */
		double y = sh.status_h + 10 + slide;
		gfx_text(cr, i18n_upper(D.title), 24, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, sh.w - 48, 1);
		y += 30;
		double rh = 56;
		Rect view = R(0, y, sh.w, MIN(D.n_items * rh + 8, sh.h - y - sh.nav_h - 10));
		gfx_fill_rect(cr, R(0, y - 10, sh.w, view.h + 18), th->bg);
		double off = ui_scroll_begin(ui, ui_id("dlg-list"), &D.scroll, view, D.n_items * rh);
		cairo_translate(cr, 0, view.y);
		for (int i = 0; i < D.n_items; i++) {
			Rect ir = R(0, i * rh, sh.w, rh);
			if (ir.y + rh < off || ir.y > off + view.h) continue;
			UiId id = ui_idn("dlg-item", i);
			ui_hit(ui, id, ir);
			bool pr = ui_pressed(ui, id);
			if (pr) gfx_fill_rect(cr, ir, col_alpha(th->fg, 0.14f));
			gfx_text(cr, D.items[i], 24, ir.y + (rh - gfx_line_height(cr, 28, FW_SEMILIGHT)) / 2, 28, FW_SEMILIGHT,
			         i == D.selected ? th->accent : th->fg, TA_LEFT, sh.w - 60, 1);
			if (ui_clicked(ui, id)) { ui_scroll_end(ui); finish(i); return; }
		}
		ui_scroll_end(ui);
		/* tap outside the list cancels */
		UiId bg = ui_id("dlg-list-bg");
		ui_hit(ui, bg, R(0, view.y + view.h, sh.w, sh.h));
		if (ui_clicked(ui, bg)) finish(-1);
		return;
	}

	/* message / input dialog: a band across the screen */
	double w = sh.w;
	double top = sh.status_h + 50 + slide;
	cairo_save(cr);
	double text_h = 0;
	if (D.text[0]) text_h = gfx_text_size(cr, D.text, 20, FW_SEMILIGHT, w - 48, 0).h;
	double body = 20 + 30 + (D.text[0] ? text_h + 14 : 0) + (D.kind == D_INPUT ? 66 : 0) + 20;
	double bh = 56;
	Rect panel = R(0, top, w, body + bh + 22);
	gfx_fill_rect(cr, panel, th->dark ? RGBX(0x1f1f1f) : RGBX(0xdddddd));
	double y = top + 20;
	gfx_text(cr, i18n_upper(D.title), 24, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, w - 48, 1);
	y += 34;
	if (D.text[0]) { gfx_text(cr, D.text, 24, y, 20, FW_SEMILIGHT, th->fg, TA_LEFT, w - 48, 0); y += text_h + 14; }
	if (D.kind == D_INPUT) {
		if (D.focus_pending) { ui_focus(ui, ui_id("dlg-input")); D.focus_pending = false; }
		bool changed;
		bool enter = ui_textbox(ui, ui_id("dlg-input"), R(24, y, w - 48, 48), D.input, sizeof D.input, D.hint, D.tb_flags, &changed);
		y += 62;
		if (enter) { cairo_restore(cr); finish(0); return; }
	}
	/* buttons */
	double bw = (w - 48 - (D.nbtn - 1) * 12) / D.nbtn;
	for (int i = 0; i < D.nbtn; i++) {
		Rect br = R(24 + i * (bw + 12), y + 8, bw, 48);
		UiId id = ui_idn("dlg-btn", i);
		if (ui_button(ui, id, br, D.btn[i], BTN_OUTLINE)) { cairo_restore(cr); finish(i); return; }
	}
	cairo_restore(cr);
}

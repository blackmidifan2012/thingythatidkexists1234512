#include "ui.h"
#include "i18n.h"
#include "util.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <pixman.h>

/* ------------------------------------------------------------------ theme */

const uint32_t ACCENT_RGB[ACCENT_COUNT] = {
	0xA4C400, 0x60A917, 0x008A00, 0x00ABA9, 0x1BA1E2, 0x0050EF, 0x6A00FF, 0xAA00FF, 0xF472D0, 0xD80073,
	0xA20025, 0xE51400, 0xFA6800, 0xF0A30A, 0xE3C800, 0x825A2C, 0x6D8764, 0x647687, 0x76608A, 0x87794E,
};

const char *accent_name(int i) {
	static const char *names[ACCENT_COUNT] = {
		N_("lime"), N_("green"), N_("emerald"), N_("teal"), N_("cyan"), N_("cobalt"), N_("indigo"),
		N_("violet"), N_("pink"), N_("magenta"), N_("crimson"), N_("red"), N_("orange"), N_("amber"),
		N_("yellow"), N_("brown"), N_("olive"), N_("steel"), N_("mauve"), N_("taupe")};
	return tr(names[CLAMP(i, 0, ACCENT_COUNT - 1)]);
}

void theme_make(Theme *t, bool dark, Color accent) {
	t->dark = dark;
	t->accent = accent;
	t->on_accent = col_luma(accent) > 0.78f ? COL_BLACK : COL_WHITE;
	if (dark) {
		t->bg = COL_BLACK;
		t->fg = COL_WHITE;
		t->sub = RGBX(0x999999);
		t->dim = RGBX(0x4d4d4d);
		t->chrome = RGBX(0x1f1f1f);
		t->chrome2 = RGBX(0x333333);
		t->tb_fill = RGBX(0xbfbfbf);
		t->tb_fg = COL_BLACK;
	} else {
		t->bg = COL_WHITE;
		t->fg = COL_BLACK;
		t->sub = RGBX(0x666666);
		t->dim = RGBX(0xb3b3b3);
		t->chrome = RGBX(0xdddddd);
		t->chrome2 = RGBX(0xcccccc);
		t->tb_fill = RGBX(0xe6e6e6);
		t->tb_fg = COL_BLACK;
	}
	t->scrim = col_alpha(t->bg, 0.72f);
}

/* ------------------------------------------------------------------- ids */

UiId ui_id(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
	return h ? h : 1;
}
UiId ui_idn(const char *s, int n) {
	uint32_t h = ui_id(s) ^ ((uint32_t)(n + 1) * 2654435761u);
	return h ? h : 1;
}
UiId ui_idp(const void *p) {
	uint32_t h = (uint32_t)((uintptr_t)p >> 3) * 2654435761u;
	return h ? h : 1;
}

/* -------------------------------------------------------------- easing --- */

double ease(Ease e, double t) {
	t = CLAMP(t, 0, 1);
	switch (e) {
	case EASE_LINEAR: return t;
	case EASE_OUT_CUBIC: return 1 - pow(1 - t, 3);
	case EASE_IN_CUBIC: return t * t * t;
	case EASE_INOUT_CUBIC: return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
	case EASE_OUT_QUINT: return 1 - pow(1 - t, 5);
	case EASE_OUT_EXPO: return t >= 1 ? 1 : 1 - pow(2, -10 * t);
	case EASE_OUT_BACK: { double c1 = 1.4, c3 = c1 + 1; return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2); }
	case EASE_IN_QUAD: return t * t;
	case EASE_OUT_QUAD: return 1 - (1 - t) * (1 - t);
	}
	return t;
}

void anim_start(Anim *a, double from, double to, double now, double dur, Ease e) {
	*a = (Anim){from, to, now, dur, e, true};
}
void anim_set(Anim *a, double v) { *a = (Anim){v, v, 0, 0, EASE_LINEAR, false}; }
bool anim_running(const Anim *a, double now) { return a->active && now < a->start + a->dur; }
double anim_value(const Anim *a, double now) {
	if (!a->active || a->dur <= 0 || now >= a->start + a->dur) return a->to;
	return a->from + (a->to - a->from) * ease(a->ease, (now - a->start) / a->dur);
}
double ui_anim(UI *ui, Anim *a) {
	if (anim_running(a, ui->now)) ui->animating = true;
	else if (a->active) { a->from = a->to; a->active = false; }
	return anim_value(a, ui->now);
}
double ui_smooth(UI *ui, double cur, double target, double rate) {
	double d = target - cur;
	if (fabs(d) < 0.002) return target;
	ui->animating = true;
	return cur + d * (1 - exp(-rate * ui->dt));
}

/* ----------------------------------------------------- per-widget state --- */

typedef struct { UiId id; double v[2]; uint32_t last; } State;
static State g_states[512];

static double *state_f(UI *ui, UiId id, double init) {
	State *free_slot = NULL, *oldest = &g_states[0];
	for (int i = 0; i < 512; i++) {
		State *s = &g_states[(id + i) % 512];
		if (s->id == id) { s->last = ui->frame; return s->v; }
		if (!s->id && !free_slot) free_slot = s;
		if (s->last < oldest->last) oldest = s;
		if (i > 16) break;
	}
	State *s = free_slot ? free_slot : oldest;
	s->id = id;
	s->v[0] = s->v[1] = init;
	s->last = ui->frame;
	return s->v;
}

/* ------------------------------------------------------------- lifecycle */

void ui_init(UI *ui) {
	memset(ui, 0, sizeof *ui);
	ui->slop = 10;
	theme_make(&ui->th, true, RGBX(ACCENT_RGB[5]));
}

void ui_begin(UI *ui, cairo_t *cr, double w, double h, double scale, double now) {
	ui->cr = cr;
	ui->w = w;
	ui->h = h;
	ui->scale = scale;
	ui->dt = ui->now > 0 ? CLAMP(now - ui->now, 0.001, 0.05) : 0.016;
	ui->now = now;
	ui->hb ^= 1;
	ui->in_frame = true;
	ui->nhits[ui->hb] = 0;
	ui->nscr[ui->hb] = 0;
	ui->clip = R(0, 0, w, h);
	ui->nclip = 0;
	ui->nscr_stack = 0;
	ui->layer = UL_BASE;
	ui->blocked = false;
	ui->animating = false;
	ui->redraw = false;
	ui->kbd_wanted = false;
	ui->frame++;
	cairo_scale(cr, scale, scale);
}

void ui_end(UI *ui) {
	ui->in_frame = false;
	/* drop events nobody consumed so they do not fire on later screens */
	ui->nclicks = 0;
	ui->longpress = 0;
	ui->released = 0;
	ui->fling_id = 0;
	ui->down_id = 0;
	ui->wheel_x = ui->wheel_y = 0;
	ui->p.fx = ui->p.x;
	ui->p.fy = ui->p.y;
	/* keys nobody consumed (no text field focused, no shortcut handler) are dropped */
	if (!ui->focus) { ui->nkeys = 0; ui->typed[0] = 0; }
}

bool ui_key(UI *ui, UiKey k) {
	if (ui->focus) return false;
	for (int i = 0; i < ui->nkeys; i++) {
		if (ui->keys[i] != k) continue;
		memmove(&ui->keys[i], &ui->keys[i + 1], (size_t)(ui->nkeys - i - 1) * sizeof(UiKey));
		ui->nkeys--;
		return true;
	}
	return false;
}

bool ui_char(UI *ui, uint32_t *cp) {
	if (ui->focus || !ui->typed[0]) return false;
	int n = utf8_decode(ui->typed, cp);
	memmove(ui->typed, ui->typed + n, strlen(ui->typed + n) + 1);
	return true;
}

void ui_request_frame(UI *ui) { ui->redraw = true; }

/* ---------------------------------------------------------------- hits ---- */

Rect ui_to_screen(UI *ui, Rect r) {
	double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
	cairo_user_to_device(ui->cr, &x0, &y0);
	cairo_user_to_device(ui->cr, &x1, &y1);
	double s = ui->scale;
	return R(MIN(x0, x1) / s, MIN(y0, y1) / s, fabs(x1 - x0) / s, fabs(y1 - y0) / s);
}

static void add_hit(UI *ui, UiId id, Rect r, bool captures) {
	if (ui->blocked || !id) return;
	Rect s = rect_intersect(ui_to_screen(ui, r), ui->clip);
	if (s.w <= 0 || s.h <= 0) return;
	int hb = ui->hb;
	if (ui->nhits[hb] >= UI_MAX_HITS) return;
	ui->hits[hb][ui->nhits[hb]++] = (UiHit){id, s, ui->layer,
		ui->nscr_stack ? ui->scr_stack[ui->nscr_stack - 1] : 0, captures};
}

void ui_hit(UI *ui, UiId id, Rect r) { add_hit(ui, id, r, false); }
void ui_hit_drag(UI *ui, UiId id, Rect r) { add_hit(ui, id, r, true); }

/* Hits of the last completed frame: while drawing that is the other buffer, between
 * frames (where input events arrive) it is the one built last. */
static int done_buf(const UI *ui) { return ui->in_frame ? ui->hb ^ 1 : ui->hb; }

static const UiHit *find_top(const UI *ui, double x, double y) {
	int pb = done_buf(ui);
	const UiHit *best = NULL;
	for (int i = 0; i < ui->nhits[pb]; i++) {
		const UiHit *h = &ui->hits[pb][i];
		if (!rect_has(h->r, x, y)) continue;
		if (!best || h->layer >= best->layer) best = h;
	}
	return best;
}

bool ui_hit_test(const UI *ui, double x, double y, int min_layer) {
	const UiHit *h = find_top(ui, x, y);
	return h && h->layer >= min_layer;
}
UiId ui_hit_id_at(const UI *ui, double x, double y) {
	const UiHit *h = find_top(ui, x, y);
	return h ? h->id : 0;
}

static const UiScroller *find_scroller(const UI *ui, UiId id) {
	int pb = done_buf(ui);
	for (int i = ui->nscr[pb] - 1; i >= 0; i--)
		if (ui->scrollers[pb][i].id == id) return &ui->scrollers[pb][i];
	return NULL;
}

static void add_scroller(UI *ui, UiId id, Rect view, int axes) {
	if (ui->blocked) return;
	int hb = ui->hb;
	if (ui->nscr[hb] >= UI_MAX_SCROLLERS) return;
	Rect s = rect_intersect(ui_to_screen(ui, view), ui->clip);
	ui->scrollers[hb][ui->nscr[hb]++] = (UiScroller){id, s, axes,
		ui->nscr_stack ? ui->scr_stack[ui->nscr_stack - 1] : 0};
}

void ui_scroller_push(UI *ui, UiId id, Rect view, int axes) {
	add_scroller(ui, id, view, axes);
	if (ui->nscr_stack < 16) ui->scr_stack[ui->nscr_stack++] = id;
}

void ui_scroller_pop(UI *ui) {
	if (ui->nscr_stack > 0) ui->nscr_stack--;
}

void ui_capture(UI *ui, UiId id) {
	ui->p.captured = id;
	ui->p.fx = ui->p.x;
	ui->p.fy = ui->p.y;
}

double *ui_state(UI *ui, UiId id, double init) { return state_f(ui, id, init); }

/* --------------------------------------------------------- pointer input -- */

static void hist_push(UI *ui, double now, double x, double y) {
	if (ui->p.nhist < 8) ui->p.nhist++;
	for (int i = ui->p.nhist - 1; i > 0; i--) memcpy(ui->p.hist[i], ui->p.hist[i - 1], sizeof ui->p.hist[0]);
	ui->p.hist[0][0] = now; ui->p.hist[0][1] = x; ui->p.hist[0][2] = y;
}

void ui_pointer_hover(UI *ui, double x, double y) {
	ui->mx = x; ui->my = y;
	ui->mouse_present = true;
}

void ui_pointer_down(UI *ui, double x, double y, double now, int source) {
	ui->p.down = true;
	ui->p.moved = false;
	ui->p.long_fired = false;
	ui->p.x = ui->p.sx = ui->p.fx = x;
	ui->p.y = ui->p.sy = ui->p.fy = y;
	ui->p.t0 = now;
	ui->p.source = source;
	ui->p.nhist = 0;
	hist_push(ui, now, x, y);
	const UiHit *h = find_top(ui, x, y);
	ui->p.active = h ? h->id : 0;
	ui->p.active_layer = h ? h->layer : -1;
	ui->p.active_scroller = h ? h->scroller : 0;
	ui->p.captured = (h && h->captures) ? h->id : 0;
	ui->down_id = ui->p.active;
	if (ui->focus && ui->p.active != ui->focus && (!h || h->layer == UL_BASE)) ui->focus = 0;
	ui->mx = x; ui->my = y;
	ui->redraw = true;
}

void ui_pointer_move(UI *ui, double x, double y, double now) {
	ui->mx = x; ui->my = y;
	if (!ui->p.down) return;
	ui->p.x = x;
	ui->p.y = y;
	hist_push(ui, now, x, y);
	if (!ui->p.moved && hypot(x - ui->p.sx, y - ui->p.sy) > ui->slop) {
		ui->p.moved = true;
		if (!ui->p.captured) {
			int axis = fabs(x - ui->p.sx) > fabs(y - ui->p.sy) ? UI_AXIS_X : UI_AXIS_Y;
			UiId s = ui->p.active_scroller;
			for (int guard = 0; s && guard < 8; guard++) {
				const UiScroller *sc = find_scroller(ui, s);
				if (!sc) break;
				if (sc->axes & axis) { ui->p.captured = s; break; }
				s = sc->parent;
			}
			if (ui->p.captured) { ui->p.fx = x; ui->p.fy = y; }
		}
	}
	ui->redraw = true;
}

void ui_pointer_up(UI *ui, double x, double y, double now) {
	if (!ui->p.down) return;
	ui->p.down = false;
	ui->p.x = x;
	ui->p.y = y;
	hist_push(ui, now, x, y);
	if (!ui->p.long_fired && !ui->p.moved && ui->p.active) {
		const UiHit *h = find_top(ui, x, y);
		if (h && h->id == ui->p.active && ui->nclicks < 8) ui->clicks[ui->nclicks++] = h->id;
	}
	if (ui->p.captured) {
		double vx = 0, vy = 0;
		int n = ui->p.nhist;
		if (n >= 2 && now - ui->p.hist[0][0] < 0.09) {
			int j = 1;
			while (j < n - 1 && ui->p.hist[0][0] - ui->p.hist[j][0] < 0.1) j++;
			double dt = ui->p.hist[0][0] - ui->p.hist[j][0];
			if (dt > 0.008) {
				vx = (ui->p.hist[0][1] - ui->p.hist[j][1]) / dt;
				vy = (ui->p.hist[0][2] - ui->p.hist[j][2]) / dt;
			}
		}
		ui->fling_id = ui->p.captured;
		ui->fling_vx = vx;
		ui->fling_vy = vy;
	}
	ui->released = ui->p.captured ? ui->p.captured : ui->p.active;
	ui->p.captured = 0;
	ui->redraw = true;
}

void ui_cancel_pointer(UI *ui) {
	ui->p.down = false;
	ui->p.active = ui->p.captured = 0;
	ui->redraw = true;
}

void ui_longpress_fire(UI *ui) {
	if (!ui->p.down || ui->p.moved || ui->p.long_fired || !ui->p.active) return;
	ui->p.long_fired = true;
	ui->longpress = ui->p.active;
	ui->redraw = true;
}

void ui_wheel(UI *ui, double dx, double dy) {
	ui->wheel_x += dx;
	ui->wheel_y += dy;
	ui->redraw = true;
}

void ui_text_event(UI *ui, const char *utf8) {
	size_t n = strlen(ui->typed);
	if (n + strlen(utf8) + 1 < sizeof ui->typed) strcat(ui->typed, utf8);
	ui->redraw = true;
}

void ui_key_event(UI *ui, UiKey k) {
	if (ui->nkeys < 32) ui->keys[ui->nkeys++] = k;
	ui->redraw = true;
}

/* ------------------------------------------------------------- queries ---- */

bool ui_clicked(UI *ui, UiId id) {
	for (int i = 0; i < ui->nclicks; i++) {
		if (ui->clicks[i] == id) {
			memmove(&ui->clicks[i], &ui->clicks[i + 1], (size_t)(ui->nclicks - i - 1) * sizeof(UiId));
			ui->nclicks--;
			ui->redraw = true;
			return true;
		}
	}
	return false;
}
bool ui_longpressed(UI *ui, UiId id) {
	if (ui->longpress == id && id) { ui->longpress = 0; ui->redraw = true; return true; }
	return false;
}
bool ui_pressed(const UI *ui, UiId id) {
	return ui->p.down && ui->p.active == id && !ui->p.long_fired &&
	       (!ui->p.captured || ui->p.captured == id);
}
bool ui_dragging(const UI *ui, UiId id) { return ui->p.down && ui->p.captured == id; }
bool ui_down_on(UI *ui, UiId id) { return ui->down_id == id && id; }
bool ui_released(UI *ui, UiId id) {
	if (ui->released == id && id) { ui->released = 0; return true; }
	return false;
}
bool ui_drag_delta(UI *ui, UiId id, double *dx, double *dy) {
	if (!ui_dragging(ui, id)) return false;
	*dx = ui->p.x - ui->p.fx;
	*dy = ui->p.y - ui->p.fy;
	ui->p.fx = ui->p.x;
	ui->p.fy = ui->p.y;
	return true;
}
bool ui_fling(UI *ui, UiId id, double *vx, double *vy) {
	if (ui->fling_id != id || !id) return false;
	*vx = ui->fling_vx;
	*vy = ui->fling_vy;
	ui->fling_id = 0;
	return true;
}

void ui_pointer_local(const UI *ui, double *x, double *y) {
	double px = ui->p.x * ui->scale, py = ui->p.y * ui->scale;
	cairo_matrix_t m;
	cairo_get_matrix(ui->cr, &m);
	if (cairo_matrix_invert(&m) == CAIRO_STATUS_SUCCESS) cairo_matrix_transform_point(&m, &px, &py);
	*x = px; *y = py;
}

bool ui_pointer_in(const UI *ui, Rect r) {
	double x, y;
	ui_pointer_local(ui, &x, &y);
	return rect_has(r, x, y);
}

bool ui_hover(UI *ui, Rect r) {
	if (!ui->mouse_present) return false;
	return rect_has(ui_to_screen(ui, r), ui->mx, ui->my);
}

bool ui_wheel_for(UI *ui, Rect r, double *dx, double *dy) {
	if (ui->wheel_x == 0 && ui->wheel_y == 0) return false;
	Rect s = rect_intersect(ui_to_screen(ui, r), ui->clip);
	if (!rect_has(s, ui->mx, ui->my)) return false;
	const UiHit *top = find_top(ui, ui->mx, ui->my);
	if (top && top->layer > ui->layer) return false;
	*dx = ui->wheel_x;
	*dy = ui->wheel_y;
	ui->wheel_x = ui->wheel_y = 0;
	return true;
}

void ui_clip_push(UI *ui, Rect r) {
	cairo_save(ui->cr);
	cairo_rectangle(ui->cr, r.x, r.y, r.w, r.h);
	cairo_clip(ui->cr);
	if (ui->nclip < 16) ui->clip_stack[ui->nclip++] = ui->clip;
	ui->clip = rect_intersect(ui->clip, ui_to_screen(ui, r));
}

void ui_clip_pop(UI *ui) {
	cairo_restore(ui->cr);
	if (ui->nclip > 0) ui->clip = ui->clip_stack[--ui->nclip];
}

/* ------------------------------------------------------------- scrolling -- */

double ui_scroll_begin(UI *ui, UiId id, Scroll *s, Rect view, double content_h) {
	double max = MAX(0, content_h - view.h);
	add_scroller(ui, id, view, UI_AXIS_Y);
	double dx, dy, vx, vy, wx, wy, dt = ui->dt;
	if (ui_drag_delta(ui, id, &dx, &dy)) {
		s->flinging = false;
		s->has_target = false;
		double d = -dy;
		if ((s->off < 0 && d < 0) || (s->off > max && d > 0)) d *= 0.35;
		s->off += d;
	}
	if (ui_fling(ui, id, &vx, &vy)) {
		s->vel = -vy;
		s->flinging = fabs(vy) > 40;
	}
	if (ui_wheel_for(ui, view, &wx, &wy)) {
		s->target = CLAMP((s->has_target ? s->target : s->off) + wy, 0, max);
		s->has_target = true;
		s->flinging = false;
	}
	if (!ui_dragging(ui, id)) {
		if (s->has_target) {
			s->off += (s->target - s->off) * (1 - exp(-14 * dt));
			if (fabs(s->target - s->off) < 0.3) { s->off = s->target; s->has_target = false; }
			ui->animating = true;
		} else if (s->flinging) {
			s->off += s->vel * dt;
			s->vel *= exp(-3.2 * dt);
			if (fabs(s->vel) < 14) s->flinging = false;
			if (s->off < 0 || s->off > max) s->vel *= exp(-12 * dt);
			ui->animating = true;
		}
		if (!s->flinging && !s->has_target) {
			double t = CLAMP(s->off, 0, max);
			if (t != s->off) {
				s->off += (t - s->off) * (1 - exp(-16 * dt));
				if (fabs(t - s->off) < 0.3) s->off = t;
				ui->animating = true;
			}
		}
	}
	if (max <= 0 && !ui_dragging(ui, id)) { s->off = 0; s->flinging = false; }
	double off = s->off;
	ui_clip_push(ui, view);
	if (ui->nscr_stack < 16) ui->scr_stack[ui->nscr_stack++] = id;
	ui_hit(ui, id, view);     /* empty space must be draggable too */
	double snapped = round(off * ui->scale) / ui->scale;
	cairo_translate(ui->cr, 0, -snapped);
	return off;
}

void ui_scroll_end(UI *ui) {
	if (ui->nscr_stack > 0) ui->nscr_stack--;
	ui_clip_pop(ui);
}

void ui_scroll_to(Scroll *s, double off) {
	s->target = off;
	s->has_target = true;
	s->flinging = false;
}

void ui_pager_goto(Pager *pg, int page) { pg->target = page; }

void ui_pager_end(UI *ui) {
	if (ui->nscr_stack > 0) ui->nscr_stack--;
}

void ui_pager_update(UI *ui, UiId id, Pager *pg, Rect view, int npages) {
	add_scroller(ui, id, view, UI_AXIS_X);
	if (ui->nscr_stack < 16) ui->scr_stack[ui->nscr_stack++] = id;
	ui_hit(ui, id, view);
	double dx, dy, vx, vy;
	if (ui_drag_delta(ui, id, &dx, &dy)) {
		double d = -dx / view.w;
		if ((pg->pos < 0 && d < 0) || (pg->pos > npages - 1 && d > 0)) d *= 0.3;
		pg->pos += d;
		pg->dragging = true;
	}
	if (ui_fling(ui, id, &vx, &vy)) {
		pg->dragging = false;
		double pv = -vx / view.w;      /* pages per second */
		int base = (int)floor(pg->pos + 0.5);
		if (pv > 1.2 && pg->pos > base - 0.5) base = (int)floor(pg->pos) + 1;
		else if (pv < -1.2) base = (int)ceil(pg->pos) - 1;
		pg->target = CLAMP(base, 0, npages - 1);
	}
	if (!pg->dragging || !ui_dragging(ui, id)) {
		pg->dragging = false;
		double d = pg->target - pg->pos;
		if (fabs(d) > 0.0008) {
			pg->pos += d * (1 - exp(-12 * ui->dt));
			if (fabs(pg->target - pg->pos) < 0.0008) pg->pos = pg->target;
			ui->animating = true;
		} else {
			pg->pos = pg->target;
		}
	}
}

bool ui_pager_page(const Pager *pg, int index, Rect view, Rect *out) {
	double d = index - pg->pos;
	if (d <= -1 || d >= 1) return false;
	*out = R(view.x + d * view.w, view.y, view.w, view.h);
	return true;
}

/* --------------------------------------------------------------- widgets -- */

bool ui_button_enabled(UI *ui, UiId id, Rect r, const char *label, ButtonStyle st, bool enabled) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	if (enabled) ui_hit(ui, id, r);
	bool pr = enabled && ui_pressed(ui, id);
	Color base = st == BTN_ACCENT ? t->accent : t->fg;
	float a = enabled ? 1.f : 0.4f;
	if (st == BTN_FILLED || st == BTN_ACCENT) {
		gfx_fill_rect(cr, r, col_alpha(pr ? t->fg : base, a));
		gfx_text_in(cr, label, rect_inset(r, 6), 20, FW_SEMILIGHT,
		            col_alpha(pr ? t->bg : (st == BTN_ACCENT ? t->on_accent : t->bg), a), TA_CENTER, true);
	} else {
		if (pr) gfx_fill_rect(cr, r, t->fg);
		else gfx_stroke_rect(cr, r, 2, col_alpha(t->fg, a));
		gfx_text_in(cr, label, rect_inset(r, 6), 20, FW_SEMILIGHT, col_alpha(pr ? t->bg : t->fg, a), TA_CENTER, true);
	}
	return enabled && ui_clicked(ui, id);
}

bool ui_button(UI *ui, UiId id, Rect r, const char *label, ButtonStyle st) {
	return ui_button_enabled(ui, id, r, label, st, true);
}

bool ui_circle_button(UI *ui, UiId id, double cx, double cy, double dia, Icon ic, const char *label) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	Rect hit = R(cx - dia / 2 - 8, cy - dia / 2 - 6, dia + 16, dia + 12 + (label ? 16 : 0));
	ui_hit(ui, id, hit);
	bool pr = ui_pressed(ui, id);
	if (pr) {
		gfx_fill_circle(cr, cx, cy, dia / 2, t->fg);
		icon_draw(cr, ic, cx, cy, dia * 0.5, t->bg);
	} else {
		gfx_stroke_circle(cr, cx, cy, dia / 2, 2, t->fg);
		icon_draw(cr, ic, cx, cy, dia * 0.5, t->fg);
	}
	if (label) gfx_text(cr, label, cx, cy + dia / 2 + 3, 12, FW_REGULAR, t->fg, TA_CENTER, 0, 1);
	return ui_clicked(ui, id);
}

bool ui_icon_button(UI *ui, UiId id, Rect r, Icon ic, Color c) {
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	icon_draw(ui->cr, ic, rect_cx(r), rect_cy(r), MIN(r.w, r.h) * 0.55, pr ? col_alpha(c, 0.55f) : c);
	return ui_clicked(ui, id);
}

bool ui_row_ex(UI *ui, UiId id, Rect r, const char *title, const char *subtitle, Icon ic,
               const char *trailing, bool chevron) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	double pad = 24, x = r.x + pad, right = rect_right(r) - pad;
	if (pr) gfx_fill_rect(cr, r, col_alpha(t->fg, 0.14f));
	if (ic != ICON_NONE) {
		double d = MIN(r.h - 20, 48);
		icon_draw_circle(cr, ic, x + d / 2, rect_cy(r), d, pr ? t->accent : t->fg, 2);
		x += d + 16;
	}
	if (chevron) right -= 4;
	if (trailing) {
		Size ts = gfx_text_size(cr, trailing, 18, FW_SEMILIGHT, 0, 1);
		double tw = MIN(ts.w, (right - x) * 0.5);
		gfx_text(cr, trailing, right, rect_cy(r) - ts.h / 2, 18, FW_SEMILIGHT, t->sub, TA_RIGHT, tw, 1);
		right -= tw + 12;
	}
	double w = right - x;
	if (subtitle && *subtitle) {
		double th = gfx_line_height(cr, 26, FW_SEMILIGHT), sh = gfx_line_height(cr, 15, FW_REGULAR);
		double y = r.y + (r.h - th - sh + 2) / 2;
		gfx_text(cr, title, x, y, 26, FW_SEMILIGHT, t->fg, TA_LEFT, w, 1);
		gfx_text(cr, subtitle, x, y + th - 2, 15, FW_REGULAR, t->sub, TA_LEFT, w, 1);
	} else {
		double th = gfx_line_height(cr, 26, FW_SEMILIGHT);
		gfx_text(cr, title, x, r.y + (r.h - th) / 2, 26, FW_SEMILIGHT, t->fg, TA_LEFT, w, 1);
	}
	if (chevron) icon_draw(cr, ICON_CHEVRON_R, rect_right(r) - pad, rect_cy(r), 18, t->sub);
	return ui_clicked(ui, id);
}

bool ui_row(UI *ui, UiId id, Rect r, const char *title, const char *subtitle, Icon ic) {
	return ui_row_ex(ui, id, r, title, subtitle, ic, NULL, false);
}

bool ui_toggle(UI *ui, UiId id, Rect r, const char *label, bool *v) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	bool changed = false;
	if (ui_clicked(ui, id)) { *v = !*v; changed = true; }
	double *pos = state_f(ui, id, *v ? 1 : 0);
	pos[0] = ui_smooth(ui, pos[0], *v ? 1 : 0, 22);
	double th = gfx_line_height(cr, 20, FW_SEMILIGHT);
	gfx_text(cr, label, r.x, r.y, 20, FW_SEMILIGHT, t->fg, TA_LEFT, r.w, 1);
	double sy = r.y + th + 4, tw = 54, thh = 22;
	Rect track = R(r.x + 2, sy + 4, tw, thh);
	Color frame = *v ? t->accent : t->fg;
	gfx_stroke_rect(cr, track, 2, col_alpha(frame, pr ? 0.7f : 1.f));
	Rect fill = R(track.x + 4, track.y + 4, (tw - 8) * pos[0], thh - 8);
	gfx_fill_rect(cr, fill, t->accent);
	double tx = track.x + 2 + (tw - 4 - 10) * pos[0];
	gfx_fill_rect(cr, R(tx - 1, track.y - 5, 10, thh + 10), pr ? t->sub : t->fg);
	gfx_text(cr, *v ? _("On") : _("Off"), track.x + tw + 14, track.y + thh / 2 - gfx_line_height(cr, 20, FW_SEMILIGHT) / 2,
	         20, FW_SEMILIGHT, t->fg, TA_LEFT, 0, 1);
	return changed;
}

bool ui_slider(UI *ui, UiId id, Rect r, double *v, double lo, double hi) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	ui_hit_drag(ui, id, R(r.x - 8, r.y, r.w + 16, r.h));
	bool changed = false;
	double x0 = r.x + 6, x1 = rect_right(r) - 6;
	if (ui_dragging(ui, id) || ui_pressed(ui, id)) {
		double lx, ly;
		ui_pointer_local(ui, &lx, &ly);
		double nv = lo + CLAMP((lx - x0) / (x1 - x0), 0, 1) * (hi - lo);
		if (nv != *v) { *v = nv; changed = true; }
	}
	double f = (hi > lo) ? CLAMP((*v - lo) / (hi - lo), 0, 1) : 0;
	double cy = rect_cy(r), tx = x0 + (x1 - x0) * f;
	gfx_fill_rect(cr, R(x0, cy - 3, x1 - x0, 6), t->chrome2);
	gfx_fill_rect(cr, R(x0, cy - 3, tx - x0, 6), t->accent);
	gfx_fill_rect(cr, R(tx - 6, cy - 16, 12, 32), ui_pressed(ui, id) ? t->accent : t->fg);
	return changed;
}

bool ui_checkbox(UI *ui, UiId id, Rect r, const char *label, bool *v) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id), changed = false;
	if (ui_clicked(ui, id)) { *v = !*v; changed = true; }
	Rect box = R(r.x + 4, rect_cy(r) - 15, 30, 30);
	gfx_stroke_rect(cr, box, 2, pr ? t->sub : t->fg);
	if (*v) icon_draw(cr, ICON_CHECK, rect_cx(box), rect_cy(box), 22, t->fg);
	gfx_text(cr, label, box.x + 46, rect_cy(r) - gfx_line_height(cr, 22, FW_SEMILIGHT) / 2, 22, FW_SEMILIGHT, t->fg,
	         TA_LEFT, r.w - 50, 1);
	return changed;
}

bool ui_radio(UI *ui, UiId id, Rect r, const char *label, bool selected) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	double cx = r.x + 20, cy = rect_cy(r);
	gfx_stroke_circle(cr, cx, cy, 15, 2, pr ? t->sub : t->fg);
	if (selected) gfx_fill_circle(cr, cx, cy, 8, t->fg);
	gfx_text(cr, label, cx + 30, cy - gfx_line_height(cr, 22, FW_SEMILIGHT) / 2, 22, FW_SEMILIGHT, t->fg, TA_LEFT,
	         r.w - 56, 1);
	return ui_clicked(ui, id);
}

/* ---- text field */

static int g_tb_cursor;     /* byte index of the caret in the focused field */
static double g_tb_blink;

void ui_focus(UI *ui, UiId id) {
	if (ui->focus != id) { ui->focus = id; g_tb_blink = ui->now; }
	ui->redraw = true;
}

bool ui_textbox(UI *ui, UiId id, Rect r, char *buf, size_t cap, const char *hint, int flags, bool *changed) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	bool enter = false;
	if (changed) *changed = false;
	ui_hit(ui, id, r);
	if (ui_clicked(ui, id)) {
		if (ui->focus != id) {
			ui_focus(ui, id);
			g_tb_cursor = (int)strlen(buf);
		}
		/* place caret by tap position */
		double lx, ly;
		ui_pointer_local(ui, &lx, &ly);
		double off = lx - (r.x + 12);
		int best = (int)strlen(buf);
		for (int i = 0, n = (int)strlen(buf); i <= n;) {
			char tmp[512];
			str_copy(tmp, sizeof tmp, buf);
			tmp[MIN(i, 510)] = 0;
			Size sz = gfx_text_size(cr, tmp, 24, FW_REGULAR, 0, 1);
			if (sz.w >= off) { best = i; break; }
			if (i == n) break;
			uint32_t cp;
			i += utf8_decode(buf + i, &cp);
		}
		g_tb_cursor = best;
		g_tb_blink = ui->now;
	}
	bool focused = ui->focus == id;
	if (focused) {
		ui->kbd_wanted = !(flags & TB_NOKBD);
		ui->kbd_purpose = flags;
		int len = (int)strlen(buf);
		g_tb_cursor = CLAMP(g_tb_cursor, 0, len);
		bool dirty = false;
		/* typed characters */
		if (ui->typed[0]) {
			for (const char *p = ui->typed; *p;) {
				uint32_t cp;
				int n = utf8_decode(p, &cp);
				bool ok = !(flags & TB_NUMBER) || (cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '+';
				if (cp == '\n' || cp == '\r') ok = false;
				if (ok && strlen(buf) + n < cap) {
					memmove(buf + g_tb_cursor + n, buf + g_tb_cursor, strlen(buf) - g_tb_cursor + 1);
					memcpy(buf + g_tb_cursor, p, n);
					g_tb_cursor += n;
					dirty = true;
				}
				p += n;
			}
			ui->typed[0] = 0;
		}
		for (int i = 0; i < ui->nkeys; i++) {
			len = (int)strlen(buf);
			switch (ui->keys[i]) {
			case UK_BACKSPACE:
				if (g_tb_cursor > 0) {
					int n = utf8_prev_len(buf, g_tb_cursor);
					memmove(buf + g_tb_cursor - n, buf + g_tb_cursor, len - g_tb_cursor + 1);
					g_tb_cursor -= n;
					dirty = true;
				}
				break;
			case UK_DELETE:
				if (g_tb_cursor < len) {
					uint32_t cp;
					int n = utf8_decode(buf + g_tb_cursor, &cp);
					memmove(buf + g_tb_cursor, buf + g_tb_cursor + n, len - g_tb_cursor - n + 1);
					dirty = true;
				}
				break;
			case UK_LEFT: if (g_tb_cursor > 0) g_tb_cursor -= utf8_prev_len(buf, g_tb_cursor); break;
			case UK_RIGHT:
				if (g_tb_cursor < len) { uint32_t cp; g_tb_cursor += utf8_decode(buf + g_tb_cursor, &cp); }
				break;
			case UK_HOME: g_tb_cursor = 0; break;
			case UK_END: g_tb_cursor = len; break;
			case UK_ENTER: enter = true; break;
			case UK_ESC: ui->focus = 0; break;
			default: break;
			}
			g_tb_blink = ui->now;
		}
		ui->nkeys = 0;
		if (dirty && changed) *changed = true;
		if (dirty) g_tb_blink = ui->now;
	}
	/* drawing */
	Color fill = focused ? (t->dark ? COL_WHITE : RGBX(0xf2f2f2)) : t->tb_fill;
	gfx_fill_rect(cr, r, fill);
	if (focused) gfx_stroke_rect(cr, r, 2, t->accent);
	double size = 24, pad = 12;
	double ty = r.y + (r.h - gfx_line_height(cr, size, FW_REGULAR)) / 2;
	char shown[512];
	if (flags & TB_PASSWORD) {
		int n = utf8_strlen(buf);
		shown[0] = 0;
		for (int i = 0; i < n && (size_t)(i * 3 + 4) < sizeof shown; i++) strcat(shown, "\xE2\x80\xA2");
	} else {
		str_copy(shown, sizeof shown, buf);
	}
	ui_clip_push(ui, R(r.x + 4, r.y, r.w - 8, r.h));
	if (!buf[0] && hint) {
		gfx_text(cr, hint, r.x + pad, ty, size, FW_REGULAR, col_alpha(t->tb_fg, 0.5f), TA_LEFT, 0, 1);
	} else {
		double xoff = 0;
		if (focused) {
			char tmp[512];
			int cb = (flags & TB_PASSWORD) ? (utf8_strlen(buf) ? (int)(utf8_strlen(shown) * 3 * ((double)g_tb_cursor / MAX(1, (int)strlen(buf)))) : 0) : g_tb_cursor;
			if (flags & TB_PASSWORD) {
				int chars = 0;
				for (int i = 0; i < g_tb_cursor;) { uint32_t cp; i += utf8_decode(buf + i, &cp); chars++; }
				cb = chars * 3;
			}
			str_copy(tmp, sizeof tmp, shown);
			tmp[MIN(cb, 510)] = 0;
			Size cs = gfx_text_size(cr, tmp, size, FW_REGULAR, 0, 1);
			if (cs.w > r.w - 2 * pad - 4) xoff = cs.w - (r.w - 2 * pad - 4);
			double blink = fmod(ui->now - g_tb_blink, 1.0);
			if (blink < 0.6) gfx_fill_rect(cr, R(r.x + pad + cs.w - xoff, r.y + 8, 2, r.h - 16), t->tb_fg);
			ui->animating = true;
		}
		gfx_text(cr, shown, r.x + pad - xoff, ty, size, FW_REGULAR, t->tb_fg, TA_LEFT, 0, 1);
	}
	ui_clip_pop(ui);
	return enter;
}

/* ---- multi-line text area */

static int byte_after_chars(const char *s, int at, int nchars) {
	for (int i = 0; i < nchars && s[at]; i++) { uint32_t cp; at += utf8_decode(s + at, &cp); }
	return at;
}

void ui_textarea(UI *ui, UiId id, Rect r, char *buf, size_t cap, const char *hint, double size, int flags,
                 TextAreaInfo *info) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	TextAreaInfo dummy;
	if (!info) info = &dummy;
	memset(info, 0, sizeof *info);
	bool plain = flags & TB_PLAIN;
	int weight = plain ? FW_SEMILIGHT : FW_REGULAR;
	double pad = plain ? 0 : 12, vpad = plain ? 2 : 9;
	double tw = MAX(10, r.w - 2 * pad);
	PangoLayout *l = gfx_layout_new(cr, buf, size, weight, tw);
	PangoRectangle ext;
	pango_layout_get_extents(l, NULL, &ext);
	double text_h = MAX((double)ext.height / PANGO_SCALE, gfx_line_height(cr, size, weight));
	double box_h = MAX(r.h, text_h + 2 * vpad);
	Rect box = R(r.x, r.y, r.w, box_h);
	ui_hit(ui, id, box);

	if (ui_clicked(ui, id)) {
		ui_focus(ui, id);
		double lx, ly;
		ui_pointer_local(ui, &lx, &ly);
		int index = 0, trailing = 0;
		pango_layout_xy_to_index(l, (int)((lx - r.x - pad) * PANGO_SCALE), (int)((ly - r.y - vpad) * PANGO_SCALE),
		                         &index, &trailing);
		g_tb_cursor = byte_after_chars(buf, index, trailing);
		g_tb_blink = ui->now;
	}
	bool focused = ui->focus == id;
	if (focused) {
		ui->kbd_wanted = !(flags & TB_NOKBD);
		ui->kbd_purpose = flags | TB_MULTILINE;
		g_tb_cursor = CLAMP(g_tb_cursor, 0, (int)strlen(buf));
		bool dirty = false, relayout = false;
#define RELAYOUT() do { if (relayout) { pango_layout_set_text(l, buf, -1); relayout = false; } } while (0)
		if (ui->typed[0]) {
			for (const char *p = ui->typed; *p;) {
				uint32_t cp;
				int n = utf8_decode(p, &cp);
				if (cp == '\r') { p += n; continue; }
				if (strlen(buf) + n < cap) {
					memmove(buf + g_tb_cursor + n, buf + g_tb_cursor, strlen(buf) - g_tb_cursor + 1);
					memcpy(buf + g_tb_cursor, p, n);
					g_tb_cursor += n;
					dirty = relayout = true;
				}
				p += n;
			}
			ui->typed[0] = 0;
		}
		for (int i = 0; i < ui->nkeys; i++) {
			int len = (int)strlen(buf);
			switch (ui->keys[i]) {
			case UK_ENTER:
				if ((size_t)len + 1 < cap) {
					memmove(buf + g_tb_cursor + 1, buf + g_tb_cursor, len - g_tb_cursor + 1);
					buf[g_tb_cursor++] = '\n';
					dirty = relayout = true;
				}
				break;
			case UK_BACKSPACE:
				if (g_tb_cursor > 0) {
					int n = utf8_prev_len(buf, g_tb_cursor);
					memmove(buf + g_tb_cursor - n, buf + g_tb_cursor, len - g_tb_cursor + 1);
					g_tb_cursor -= n;
					dirty = relayout = true;
				}
				break;
			case UK_DELETE:
				if (g_tb_cursor < len) {
					uint32_t cp;
					int n = utf8_decode(buf + g_tb_cursor, &cp);
					memmove(buf + g_tb_cursor, buf + g_tb_cursor + n, len - g_tb_cursor - n + 1);
					dirty = relayout = true;
				}
				break;
			case UK_LEFT: if (g_tb_cursor > 0) g_tb_cursor -= utf8_prev_len(buf, g_tb_cursor); break;
			case UK_RIGHT: if (g_tb_cursor < len) { uint32_t cp; g_tb_cursor += utf8_decode(buf + g_tb_cursor, &cp); } break;
			case UK_UP: case UK_DOWN: case UK_PGUP: case UK_PGDN: {
				RELAYOUT();
				PangoRectangle pos;
				pango_layout_get_cursor_pos(l, g_tb_cursor, &pos, NULL);
				int lines = (ui->keys[i] == UK_PGUP || ui->keys[i] == UK_PGDN) ? 8 : 1;
				int dir = (ui->keys[i] == UK_UP || ui->keys[i] == UK_PGUP) ? -1 : 1;
				int ty = pos.y + pos.height / 2 + dir * lines * pos.height;
				if (ty < 0) { g_tb_cursor = 0; break; }
				int index = 0, trailing = 0;
				if (!pango_layout_xy_to_index(l, pos.x, ty, &index, &trailing) && ty > 0 && dir > 0 &&
				    ty >= ext.y + ext.height) { g_tb_cursor = len; break; }
				g_tb_cursor = byte_after_chars(buf, index, trailing);
				break;
			}
			case UK_HOME: case UK_END: {
				RELAYOUT();
				int line = 0, x = 0;
				pango_layout_index_to_line_x(l, g_tb_cursor, FALSE, &line, &x);
				PangoLayoutLine *pl = pango_layout_get_line_readonly(l, line);
				if (pl) g_tb_cursor = ui->keys[i] == UK_HOME ? pl->start_index : pl->start_index + pl->length;
				if (g_tb_cursor > 0 && ui->keys[i] == UK_END && buf[g_tb_cursor - 1] == '\n') g_tb_cursor--;
				break;
			}
			case UK_ESC: ui->focus = 0; break;
			default: break;
			}
			g_tb_blink = ui->now;
		}
		ui->nkeys = 0;
		RELAYOUT();
#undef RELAYOUT
		if (dirty) {
			info->changed = true;
			g_tb_blink = ui->now;
			pango_layout_get_extents(l, NULL, &ext);
			text_h = MAX((double)ext.height / PANGO_SCALE, gfx_line_height(cr, size, weight));
			box_h = MAX(r.h, text_h + 2 * vpad);
			box = R(r.x, r.y, r.w, box_h);
		}
	}
	focused = ui->focus == id;
	info->focused = focused;
	info->height = box_h;
	/* drawing */
	Color fg = plain ? t->fg : t->tb_fg;
	if (!plain) {
		gfx_fill_rect(cr, box, focused ? (t->dark ? COL_WHITE : RGBX(0xf2f2f2)) : t->tb_fill);
		if (focused) gfx_stroke_rect(cr, box, 2, t->accent);
	}
	if (!buf[0] && hint && !focused) {
		gfx_text(cr, hint, r.x + pad, r.y + vpad, size, weight, plain ? t->sub : col_alpha(t->tb_fg, 0.5f), TA_LEFT, tw, 0);
	} else {
		cairo_save(cr);
		gfx_set_color(cr, fg);
		cairo_move_to(cr, r.x + pad, r.y + vpad);
		pango_cairo_update_layout(cr, l);
		pango_cairo_show_layout(cr, l);
		cairo_restore(cr);
	}
	PangoRectangle pos;
	pango_layout_get_cursor_pos(l, focused ? g_tb_cursor : 0, &pos, NULL);
	info->caret_y = r.y + vpad + (double)pos.y / PANGO_SCALE;
	info->caret_h = (double)pos.height / PANGO_SCALE;
	if (focused) {
		if (fmod(ui->now - g_tb_blink, 1.0) < 0.6)
			gfx_fill_rect(cr, R(r.x + pad + (double)pos.x / PANGO_SCALE, info->caret_y, 2, info->caret_h), plain ? t->accent : fg);
		ui->animating = true;
	}
	g_object_unref(l);
}

/* ---- application bar */

Rect appbar_content(const AppBar *ab, Rect area) {
	double h = ab && ab->minimized ? 26 : APPBAR_H;
	return R(area.x, area.y, area.w, area.h - h);
}

bool appbar_back(AppBar *ab) {
	if (!ab->open) return false;
	ab->open = false;
	return true;
}

int appbar_draw(UI *ui, AppBar *ab, Rect area, const AppBarBtn *btns, int nbtn, const char *const *menu, int nmenu) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	int result = -1;
	nbtn = CLAMP(nbtn, 0, 4);
	double closed_h = ab->minimized ? 26 : APPBAR_H;
	const double label_h = 22, item_h = 48;
	double open_h = (nbtn ? APPBAR_H + label_h : 34) + nmenu * item_h + (nmenu ? 14 : 0);
	if (open_h <= closed_h + 1 && !nmenu && !nbtn) return -1;
	ab->pos = ui_smooth(ui, ab->pos, ab->open ? 1 : 0, 24);
	double p = ease(EASE_OUT_CUBIC, ab->pos);
	double h = closed_h + (open_h - closed_h) * p;
	double bottom = rect_bottom(area);
	Rect bar = R(area.x, bottom - h, area.w, h);

	/* tapping anywhere outside an open bar closes it */
	if (ab->open || ab->pos > 0.01) {
		UiId sid = ui_idn("appbar-scrim", (int)ui_idp(ab));
		ui_hit(ui, sid, R(area.x, area.y, area.w, bar.y - area.y));
		if (ui_clicked(ui, sid)) ab->open = false;
	}
	gfx_fill_rect(cr, bar, t->chrome);
	UiId bid = ui_idp(ab);
	ui_hit(ui, bid, bar);
	ui_clip_push(ui, bar);
	if (ui_clicked(ui, bid)) ab->open = !ab->open;

	/* "..." toggle */
	Rect more = R(rect_right(bar) - 52, bar.y, 52, 30);
	UiId mid = ui_idn("appbar-more", (int)bid);
	ui_hit(ui, mid, more);
	icon_draw(cr, ICON_MORE, rect_cx(more), bar.y + 13, 22, ui_pressed(ui, mid) ? t->accent : t->fg);
	if (ui_clicked(ui, mid)) ab->open = !ab->open;

	/* round buttons, centred like on the phone */
	if (nbtn && !(ab->minimized && p < 0.05)) {
		double slot = 80, dia = 42;
		double x0 = area.x + (area.w - nbtn * slot) / 2;
		double cy = bar.y + APPBAR_H / 2 + 2;
		for (int i = 0; i < nbtn; i++) {
			double cx = x0 + slot * (i + 0.5);
			UiId id = ui_idn("appbar-btn", (int)(bid + i));
			bool en = !btns[i].disabled;
			Rect hr = R(cx - slot / 2, bar.y, slot, APPBAR_H + label_h * p);
			if (en) ui_hit(ui, id, hr);
			bool pr = en && ui_pressed(ui, id);
			Color c = en ? t->fg : t->dim;
			if (pr) {
				gfx_fill_circle(cr, cx, cy, dia / 2, t->accent);
				icon_draw(cr, btns[i].icon, cx, cy, dia * 0.48, COL_WHITE);
			} else {
				gfx_stroke_circle(cr, cx, cy, dia / 2, 2, c);
				icon_draw(cr, btns[i].icon, cx, cy, dia * 0.48, c);
			}
			if (p > 0.02 && btns[i].label)
				gfx_text(cr, i18n_lower(btns[i].label), cx, cy + dia / 2 + 3, 12, FW_REGULAR, col_alpha(c, (float)p),
				         TA_CENTER, slot - 4, 1);
			if (en && ui_clicked(ui, id)) { result = i; ab->open = false; }
		}
	}
	/* menu items */
	if (nmenu && p > 0.02) {
		double y = bar.y + (nbtn ? APPBAR_H + label_h : 34) + 4;
		for (int i = 0; i < nmenu; i++) {
			Rect ir = R(area.x, y + i * item_h, area.w, item_h);
			if (ir.y > bottom) break;
			UiId id = ui_idn("appbar-menu", (int)(bid + i));
			ui_hit(ui, id, ir);
			bool pr = ui_pressed(ui, id);
			if (pr) gfx_fill_rect(cr, ir, t->accent);
			gfx_text(cr, i18n_lower(menu[i]), ir.x + 24, ir.y + (item_h - gfx_line_height(cr, 21, FW_SEMILIGHT)) / 2, 21,
			         FW_SEMILIGHT, pr ? t->on_accent : t->fg, TA_LEFT, ir.w - 48, 1);
			if (ui_clicked(ui, id)) { result = APPBAR_MENU + i; ab->open = false; }
		}
	}
	ui_clip_pop(ui);
	return result;
}

/* ---- progress */

void ui_progress_dots(UI *ui, Rect r, Color c) {
	cairo_t *cr = ui->cr;
	ui->animating = true;
	double t = ui->now;
	for (int i = 0; i < 5; i++) {
		double p = fmod(t * 0.42 - i * 0.055 + 10, 1.0);
		double x;
		/* decelerate into the middle, accelerate out - like the Windows Phone dots */
		if (p < 0.5) x = 0.5 * pow(p * 2, 0.55) * 0.5 + p * 0.5 * 0.5;
		else x = 0.5 + 0.5 * (1 - pow((1 - p) * 2, 0.55)) * 0.5 + (p - 0.5) * 0.5 * 0.5;
		x = CLAMP(p < 0.5 ? 0.5 * ease(EASE_OUT_QUAD, p * 2) * 0.5 + p * 0.25 : 0.5 + 0.5 * ease(EASE_IN_QUAD, (p - 0.5) * 2) * 0.5 + (p - 0.5) * 0.25 + 0.0, 0, 1);
		x = p;
		double px = r.x + x * r.w;
		gfx_fill_rect(cr, R(px - 2, rect_cy(r) - 2, 4, 4), c);
	}
}

void ui_progress_bar(UI *ui, Rect r, double v) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	gfx_fill_rect(cr, R(r.x, rect_cy(r) - 2, r.w, 4), t->chrome2);
	if (v < 0) {
		ui_progress_dots(ui, r, t->accent);
	} else {
		gfx_fill_rect(cr, R(r.x, rect_cy(r) - 2, r.w * CLAMP(v, 0, 1), 4), t->accent);
	}
}

/* ---- headings */

double ui_page_header(UI *ui, double x, double y, const char *app_title, const char *title) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	double h1 = 0;
	if (app_title && *app_title)
		h1 = gfx_text(cr, i18n_upper(app_title), x + 4, y, 17, FW_SEMIBOLD, t->fg, TA_LEFT, ui->w - 2 * x, 1);
	double h2 = gfx_text(cr, i18n_lower(title), x, y + h1 - 6, 56, FW_LIGHT, t->fg, TA_LEFT, ui->w - 2 * x + 10, 1);
	return y + h1 - 6 + h2 + 4;
}

double ui_section(UI *ui, Rect r, const char *label) {
	gfx_text(ui->cr, i18n_upper(label), r.x, r.y + 4, 15, FW_SEMIBOLD, ui->th.accent, TA_LEFT, r.w, 1);
	return r.y + 28;
}

void ui_letter_tile(UI *ui, Rect r, const char *letter, bool enabled, bool pressed) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	Color bg = !enabled ? t->chrome2 : pressed ? t->fg : t->accent;
	gfx_fill_rect(cr, r, bg);
	Color fg = !enabled ? t->dim : pressed ? t->bg : t->on_accent;
	/* Windows Phone draws group letters in lower case, bottom-left of the tile */
	const char *l = i18n_lower(letter);
	double sz = MIN(r.h * 0.62, 40);
	gfx_text(cr, l, r.x + r.w * 0.12, rect_bottom(r) - gfx_line_height(cr, sz, FW_SEMILIGHT) + sz * 0.12, sz,
	         FW_SEMILIGHT, fg, TA_LEFT, 0, 1);
}

double ui_pivot_headers(UI *ui, UiId id, Pager *pg, const char *const *titles, int n, double x, double y, double w) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	double size = 46, gap = 22;
	double widths[16], offs[16];
	double acc = 0;
	for (int i = 0; i < n && i < 16; i++) {
		widths[i] = gfx_text_size(cr, i18n_lower(titles[i]), size, FW_LIGHT, 0, 1).w;
		offs[i] = acc;
		acc += widths[i] + gap;
	}
	int a = CLAMP((int)floor(pg->pos), 0, n - 1), b = MIN(a + 1, n - 1);
	double f = pg->pos - a;
	double scroll = offs[a] + (offs[b] - offs[a]) * CLAMP(f, 0, 1);
	double lh = gfx_line_height(cr, size, FW_LIGHT);
	ui_clip_push(ui, R(0, y, ui->w, lh + 4));
	for (int i = 0; i < n && i < 16; i++) {
		double px = x + offs[i] - scroll;
		double dist = fabs(i - pg->pos);
		Color c = col_mix(t->fg, t->dim, (float)CLAMP(dist, 0, 1));
		Rect hr = R(px, y, widths[i] + 10, lh);
		UiId hid = ui_idn("pivot-head", (int)(id + i));
		ui_hit(ui, hid, hr);
		if (ui_clicked(ui, hid)) ui_pager_goto(pg, i);
		gfx_text(cr, i18n_lower(titles[i]), px, y, size, FW_LIGHT, c, TA_LEFT, 0, 1);
	}
	ui_clip_pop(ui);
	return y + lh;
}

/* ------------------------------------------------------------ turnstile --- */

/* The page is cut into vertical strips, each a scaled blit with its own perspective
 * height. When cr draws into an image in pixel space the strips go straight to pixman:
 * through cairo every strip cost a save, a clip and a new source image, which was most
 * of a turnstile frame. Nearest sampling halves what is left (mid-swing it looks the
 * same as bilinear), and an opaque (RGB24) snapshot turns each strip into a plain copy. */
void ui_draw_turnstile(cairo_t *cr, cairo_surface_t *snap, double sw, double sh, Rect region,
                       double angle_deg, double pivot_x, double alpha) {
	if (!snap) return;
	double th = angle_deg * M_PI / 180.0;
	double cam = sw * 2.2;                  /* camera distance */
	double ch = sh / 2;                     /* vertical centre of the perspective */
	/* ~120 strips look the same as 3px ones mid-swing */
	double step = MAX(4.0, sw / 120.0);
	/* single tiles are small and swing fast: ~16 strips each is plenty */
	if (region.w < sw * 0.75) step = MAX(step, MIN(region.w / 16, step * 3));

	cairo_surface_t *dst = cairo_get_group_target(cr);
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);
	/* callers undo the UI scale with its inverse, which is 1 only up to rounding */
	bool direct = fabs(m.xx - 1) < 1e-6 && fabs(m.yy - 1) < 1e-6 && fabs(m.xy) < 1e-6 && fabs(m.yx) < 1e-6 &&
	              cairo_surface_get_type(dst) == CAIRO_SURFACE_TYPE_IMAGE &&
	              cairo_image_surface_get_format(dst) == CAIRO_FORMAT_ARGB32 &&
	              (cairo_image_surface_get_format(snap) == CAIRO_FORMAT_ARGB32 ||
	               cairo_image_surface_get_format(snap) == CAIRO_FORMAT_RGB24);
	pixman_image_t *psrc = NULL, *pdst = NULL, *pmask = NULL;
	cairo_pattern_t *pat = NULL;
	double ox = 0, oy = 0;                  /* user -> device pixel offset */
	int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0; /* clip box, device pixels */
	if (direct) {
		cairo_surface_get_device_offset(dst, &ox, &oy);
		ox += m.x0;
		oy += m.y0;
		double a, b, c, d;
		cairo_clip_extents(cr, &a, &b, &c, &d);
		cx0 = MAX(0, (int)lround(a + ox));
		cy0 = MAX(0, (int)lround(b + oy));
		cx1 = MIN(cairo_image_surface_get_width(dst), (int)lround(c + ox));
		cy1 = MIN(cairo_image_surface_get_height(dst), (int)lround(d + oy));
		cairo_surface_flush(snap);
		cairo_surface_flush(dst);
		pixman_format_code_t sfmt = cairo_image_surface_get_format(snap) == CAIRO_FORMAT_RGB24
		                            ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
		psrc = pixman_image_create_bits(sfmt, cairo_image_surface_get_width(snap),
		                                cairo_image_surface_get_height(snap),
		                                (uint32_t *)cairo_image_surface_get_data(snap),
		                                cairo_image_surface_get_stride(snap));
		pdst = pixman_image_create_bits(PIXMAN_a8r8g8b8, cairo_image_surface_get_width(dst),
		                                cairo_image_surface_get_height(dst),
		                                (uint32_t *)cairo_image_surface_get_data(dst),
		                                cairo_image_surface_get_stride(dst));
		pixman_image_set_filter(psrc, PIXMAN_FILTER_NEAREST, NULL, 0);
		if (alpha < 1) {
			pixman_color_t a8 = {0, 0, 0, (uint16_t)(CLAMP(alpha, 0, 1) * 0xffff)};
			pmask = pixman_image_create_solid_fill(&a8);
		}
	} else {
		pat = cairo_pattern_create_for_surface(snap);
		cairo_pattern_set_filter(pat, CAIRO_FILTER_FAST);
	}
	for (double x = region.x; x < region.x + region.w; x += step) {
		double x1 = MIN(x + step, region.x + region.w);
		double d0 = x - pivot_x, d1 = x1 - pivot_x;
		double X0 = d0 * cos(th), Z0 = d0 * sin(th);
		double X1 = d1 * cos(th), Z1 = d1 * sin(th);
		double k0 = cam / (cam + Z0), k1 = cam / (cam + Z1);
		if (k0 <= 0 || k1 <= 0) continue;
		double sx0 = pivot_x + X0 * k0, sx1 = pivot_x + X1 * k1;
		double km = (k0 + k1) / 2;
		double w = sx1 - sx0;
		if (fabs(w) < 0.01) continue;
		/* map source x -> sx0 (+ scale), source y about ch -> scaled */
		double sxs = w / (x1 - x), sys = km;
		double tx = sx0 - x * sxs, ty = ch - ch * sys;
		double y0 = ch - (ch - region.y) * km, h = region.h * km;
		if (direct) {
			/* whole-pixel strip edges: neighbours share an edge, no seams or double blends */
			int px0 = MAX(cx0, (int)lround(MIN(sx0, sx1) + ox)), px1 = MIN(cx1, (int)lround(MAX(sx0, sx1) + ox));
			int py0 = MAX(cy0, (int)lround(y0 + oy)), py1 = MIN(cy1, (int)lround(y0 + h + oy));
			if (px1 <= px0 || py1 <= py0) continue;
			/* strip pixel -> snapshot pixel, from the strip's corner so that every term stays
			 * within the snapshot's size (pixman matrices are 16.16 fixed point) */
			pixman_transform_t t = {{
				{pixman_double_to_fixed(1 / sxs), 0, pixman_double_to_fixed((px0 - tx - ox) / sxs)},
				{0, pixman_double_to_fixed(1 / sys), pixman_double_to_fixed((py0 - ty - oy) / sys)},
				{0, 0, pixman_fixed_1},
			}};
			pixman_image_set_transform(psrc, &t);
			pixman_image_composite32(PIXMAN_OP_OVER, psrc, pmask, pdst, 0, 0, 0, 0, px0, py0,
			                         px1 - px0, py1 - py0);
		} else {
			cairo_save(cr);
			/* whole-pixel strip edges: no half-blended seams, and a much cheaper clip */
			cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
			cairo_rectangle(cr, sx0, y0, w + 0.5, h);
			cairo_clip(cr);
			cairo_translate(cr, tx, ty);
			cairo_scale(cr, sxs, sys);
			cairo_set_source(cr, pat);
			cairo_paint_with_alpha(cr, alpha);
			cairo_restore(cr);
		}
	}
	if (direct) {
		pixman_image_unref(psrc);
		pixman_image_unref(pdst);
		if (pmask) pixman_image_unref(pmask);
		cairo_surface_mark_dirty(dst);
	} else {
		cairo_pattern_destroy(pat);
	}
}

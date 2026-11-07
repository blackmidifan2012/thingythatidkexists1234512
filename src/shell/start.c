#include "shell.h"
#include "../apps/apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../util.h"

#include <math.h>

/* ------------------------------------------------------------------ model */

typedef struct Tile {
	char *key;               /* "b:settings" or "d:<desktop id>" */
	TileSize size;
	int ux, uy, uw, uh;      /* grid position and size in small units */
	Rect r;                  /* content-space rectangle (before scroll) */
	Rect screen;             /* last drawn screen rectangle */
	bool shown;
	double scale;            /* animated press / edit scale */
} Tile;

static VEC(Tile) g_tiles;
static Scroll g_scroll;
static bool g_edit;
static int g_sel = -1;
static int g_drag = -1;
static double g_drag_x, g_drag_y;     /* finger offset inside the dragged tile */
static bool g_layout_dirty = true;
static double g_content_h;
static int g_cols, g_ucols;
static double g_margin = 12, g_gap = 6, g_unit;

/* feather animation */
static struct { int mode; double t0; int origin; } g_anim;   /* mode 0 none, 1 leave, 2 enter */

static const struct { const char *key; TileSize size; } DEFAULTS[] = {
	{"b:weather", TILE_WIDE}, {"b:clock", TILE_MEDIUM},
	{"b:photos", TILE_WIDE}, {"b:calendar", TILE_MEDIUM},
	{"b:store", TILE_MEDIUM}, {"b:settings", TILE_MEDIUM}, {"b:internet", TILE_MEDIUM},
	{"b:calculator", TILE_SMALL}, {"b:notes", TILE_SMALL}, {"b:music", TILE_SMALL}, {"b:files", TILE_SMALL},
	{"b:people", TILE_MEDIUM}, {"b:camera", TILE_MEDIUM},
};

static void tile_units(TileSize s, int *w, int *h) {
	switch (s) {
	case TILE_SMALL: *w = 1; *h = 1; break;
	case TILE_MEDIUM: *w = 2; *h = 2; break;
	default: *w = 4; *h = 2; break;
	}
}

/* ------------------------------------------------------------ persistence */

void start_save(void) {
	cJSON *arr = config_section("tiles", true);
	while (cJSON_GetArraySize(arr)) cJSON_DeleteItemFromArray(arr, 0);
	for (int i = 0; i < g_tiles.len; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "app", g_tiles.items[i].key);
		cJSON_AddNumberToObject(o, "size", g_tiles.items[i].size);
		cJSON_AddItemToArray(arr, o);
	}
	config_save_soon();
}

static void add_tile(const char *key, TileSize size) {
	Tile t = {.key = xstrdup(key), .size = size, .scale = 1};
	vec_push(&g_tiles, t);
	g_layout_dirty = true;
}

void start_init(void) {
	cJSON *arr = config_section("tiles", true);
	cJSON *it;
	cJSON_ArrayForEach(it, arr) {
		cJSON *app = cJSON_GetObjectItemCaseSensitive(it, "app");
		cJSON *sz = cJSON_GetObjectItemCaseSensitive(it, "size");
		if (cJSON_IsString(app) && cJSON_IsNumber(sz)) add_tile(app->valuestring, CLAMP((int)sz->valuedouble, 0, 2));
	}
	if (!g_tiles.len) {
		for (size_t i = 0; i < ARRAY_LEN(DEFAULTS); i++) {
			if (app_find(DEFAULTS[i].key)) add_tile(DEFAULTS[i].key, DEFAULTS[i].size);
		}
		start_save();
	}
}

void start_reload(void) { g_layout_dirty = true; }

static int find_tile(const char *key) {
	for (int i = 0; i < g_tiles.len; i++) if (str_eq(g_tiles.items[i].key, key)) return i;
	return -1;
}

static char *norm_key(const char *app) {
	if (str_starts(app, "b:") || str_starts(app, "d:")) return xstrdup(app);
	return xasprintf("%s:%s", app_find(app) ? "b" : "d", app);
}

bool start_is_pinned(const char *app) {
	char *k = norm_key(app);
	bool r = find_tile(k) >= 0;
	free(k);
	return r;
}

void start_pin(const char *app) {
	char *k = norm_key(app);
	if (find_tile(k) < 0) {
		add_tile(k, TILE_MEDIUM);
		start_save();
		shell_toast(_("Pinned to Start"), start_app_name(k), NULL);
	}
	free(k);
	shell_invalidate();
}

void start_unpin(const char *app) {
	char *k = norm_key(app);
	int i = find_tile(k);
	if (i >= 0) {
		free(g_tiles.items[i].key);
		vec_remove(&g_tiles, i);
		g_layout_dirty = true;
		if (g_sel == i) g_sel = -1;
		start_save();
	}
	free(k);
	shell_invalidate();
}

const char *start_app_name(const char *key) {
	if (str_starts(key, "b:")) {
		const AppDef *d = app_find(key);
		return d ? tr(d->name) : key;
	}
	DesktopApp *a = desktop_find(key + (str_starts(key, "d:") ? 2 : 0));
	return a ? a->name : key;
}

/* ----------------------------------------------------------------- layout */

static void compute_metrics(double w) {
	int base = (w < 400) ? 2 : (cfg.more_tiles ? 3 : 2);
	int cols = base + (int)MAX(0, floor((w - 480) / 170));
	if (w < 400) cols = 2;
	g_cols = CLAMP(cols, 2, 8);
	g_ucols = g_cols * 2;
	double medium = (w - 2 * g_margin - (g_cols - 1) * g_gap) / g_cols;
	g_unit = (medium - g_gap) / 2;
}

static void do_layout(double w) {
	compute_metrics(w);
	static unsigned char occ[600][16];
	int rows_cap = 600;
	memset(occ, 0, sizeof occ);
	int maxrow = 0;
	for (int i = 0; i < g_tiles.len; i++) {
		Tile *t = &g_tiles.items[i];
		int uw, uh;
		tile_units(t->size, &uw, &uh);
		if (uw > g_ucols) uw = g_ucols;
		t->uw = uw;
		t->uh = uh;
		bool placed = false;
		for (int y = 0; y < rows_cap - 4 && !placed; y++) {
			for (int x = 0; x + uw <= g_ucols && !placed; x++) {
				bool ok = true;
				for (int dy = 0; dy < uh && ok; dy++)
					for (int dx = 0; dx < uw && ok; dx++)
						if (occ[y + dy][x + dx]) ok = false;
				if (!ok) continue;
				for (int dy = 0; dy < uh; dy++)
					for (int dx = 0; dx < uw; dx++) occ[y + dy][x + dx] = 1;
				t->ux = x;
				t->uy = y;
				placed = true;
				if (y + uh > maxrow) maxrow = y + uh;
			}
		}
		t->r = R(g_margin + t->ux * (g_unit + g_gap), 8 + t->uy * (g_unit + g_gap),
		         t->uw * g_unit + (t->uw - 1) * g_gap, t->uh * g_unit + (t->uh - 1) * g_gap);
	}
	g_content_h = 8 + maxrow * (g_unit + g_gap) + 90;
	g_layout_dirty = false;
}

/* --------------------------------------------------------------- tile art */

static Color tile_color(const Tile *t, const Theme *th) {
	if (str_starts(t->key, "b:")) {
		const AppDef *d = app_find(t->key);
		if (d && d->color) return RGBX(d->color);
	}
	return th->accent;
}

static void draw_tile_face(UI *ui, const Tile *t, Rect r, double now) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Color bg = tile_color(t, th);
	Image *wp = cfg.start_wallpaper ? wallpaper_start() : NULL;
	if (wp) {
		/* Windows Phone 8.1 start background: every tile is a window onto one picture
		 * fixed to the screen (moving slowly with the scroll for parallax), the gaps
		 * between tiles stay the theme background. */
		cairo_save(cr);
		cairo_rectangle(cr, r.x, r.y, r.w, r.h);
		cairo_clip(cr);
		cairo_identity_matrix(cr);
		cairo_scale(cr, sh.scale, sh.scale);
		double par = cfg.parallax ? MIN(g_scroll.off * 0.22, sh.h * 0.3) : 0;
		gfx_image_cover(cr, wp, R(-sh.home_pager.pos * 24, -par, sh.w + 24, sh.h * 1.3), 1.0);
		cairo_restore(cr);
		gfx_fill_rect(cr, r, RGBA(0x000000, 0.18f));
	} else {
		gfx_fill_rect(cr, r, bg);
	}
	Color fg = COL_WHITE;
	double s = MIN(r.w, r.h);
	bool builtin = str_starts(t->key, "b:");
	const AppDef *d = builtin ? app_find(t->key) : NULL;
	DesktopApp *da = builtin ? NULL : desktop_find(t->key + 2);
	if (d && d->tile && cfg.live_tiles && t->size != TILE_SMALL) {
		ui_clip_push(ui, r);
		d->tile(ui, r, t->size, now);
		ui_clip_pop(ui);
	} else {
		double isz = t->size == TILE_SMALL ? s * 0.58 : MIN(s * 0.42, 70);
		double cx = rect_cx(r), cy = t->size == TILE_SMALL ? rect_cy(r) : r.y + r.h * 0.46;
		if (d) {
			icon_draw(cr, d->icon, cx, cy, isz * 1.1, fg);
		} else if (da) {
			Image *img = desktop_icon(da, 128);
			if (img && cfg.mono_icons) gfx_image_silhouette(cr, img, R(cx - isz / 2, cy - isz / 2, isz, isz), fg);
			else if (img) gfx_image_contain(cr, img, R(cx - isz / 2, cy - isz / 2, isz, isz), 1.0);
			else {
				char letter[8] = "?";
				uint32_t cp;
				if (da->name && da->name[0]) { int n = utf8_decode(da->name, &cp); memcpy(letter, i18n_upper(da->name), n); letter[n] = 0; }
				gfx_text(cr, letter, cx, cy - isz * 0.55, isz * 1.1, FW_LIGHT, fg, TA_CENTER, 0, 1);
			}
		}
	}
	if (t->size != TILE_SMALL && !(d && d->tile && cfg.live_tiles)) {
		const char *name = start_app_name(t->key);
		gfx_text(cr, name, r.x + 9, rect_bottom(r) - 24, 15, FW_REGULAR, fg, TA_LEFT, r.w - 18, 1);
	}
	/* notification badge */
	int cnt = 0;
	if (d) cnt = d->badge ? d->badge() : 0;
	else if (da) cnt = shell_notify_count_for(da->name, da->id);
	if (cnt > 0) {
		char b[16];
		snprintf(b, sizeof b, cnt > 99 ? "99+" : "%d", cnt);
		if (t->size == TILE_SMALL)
			gfx_text(cr, b, rect_right(r) - 4, rect_bottom(r) - 20, 14, FW_SEMIBOLD, fg, TA_RIGHT, 0, 1);
		else
			gfx_text(cr, b, rect_right(r) - 10, rect_bottom(r) - 34, 30, FW_SEMILIGHT, fg, TA_RIGHT, 0, 1);
	}
}

/* ------------------------------------------------------- edit mode helpers */

bool start_edit_mode(void) { return g_edit; }

static void exit_edit(void) {
	g_edit = false;
	g_sel = -1;
	g_drag = -1;
	start_save();
}

bool start_back(void) {
	if (g_edit) { exit_edit(); shell_invalidate(); return true; }
	return false;
}

void start_back_to_top(void) { ui_scroll_to(&g_scroll, 0); shell_invalidate(); }

Rect start_tile_rect(const char *app) {
	char *k = norm_key(app);
	int i = find_tile(k);
	free(k);
	return i >= 0 ? g_tiles.items[i].screen : R(0, 0, 0, 0);
}

void start_launch_rect(const char *app, Rect *r, Color *c) {
	char *k = norm_key(app);
	int i = find_tile(k);
	if (i >= 0) {
		*r = g_tiles.items[i].screen;
		*c = tile_color(&g_tiles.items[i], &sh.ui.th);
		g_anim.origin = i;
	} else {
		g_anim.origin = -1;
	}
	free(k);
}

int start_tile_count(void) { return g_tiles.len; }
bool start_tile_info(int i, char *key, size_t cap, Rect *r) {
	if (i < 0 || i >= g_tiles.len || !g_tiles.items[i].shown) return false;
	str_copy(key, cap, g_tiles.items[i].key);
	*r = g_tiles.items[i].screen;
	return true;
}

void start_leave_anim(Rect origin) {
	g_anim.mode = 1;
	g_anim.t0 = now_sec();
}

void start_enter_anim(void) {
	g_anim.mode = 2;
	g_anim.t0 = now_sec();
	g_anim.origin = -1;
}

static double tile_delay(int i) {
	const Tile *t = &g_tiles.items[i];
	double d = t->uy * 0.035 + t->ux * 0.018;
	if (g_anim.mode == 1 && i == g_anim.origin) d = 0.2;
	return d;
}

/* ------------------------------------------------------------------ drawing */

typedef struct { double off; double now; } TilesArg;

static void draw_tiles_static(UI *ui, void *arg) {
	TilesArg *a = arg;
	cairo_translate(ui->cr, 0, -a->off);
	for (int i = 0; i < g_tiles.len; i++) {
		Tile *t = &g_tiles.items[i];
		draw_tile_face(ui, t, t->r, a->now);
	}
}

void start_draw(UI *ui, Rect area, double xoff) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double now = ui->now;
	if (g_layout_dirty || g_ucols == 0) do_layout(area.w);
	compute_metrics(area.w);
	if (g_tiles.len && g_tiles.items[0].size == TILE_MEDIUM && fabs(g_unit * 2 + g_gap - g_tiles.items[0].r.w) > 0.5) do_layout(area.w);

	bool animating_tiles = g_anim.mode != 0;
	bool saved_blocked = ui->blocked;

	cairo_save(cr);
	cairo_translate(cr, area.x + 0, 0);
	/* scroll area */
	UiId sid = ui_id("start-scroll");
	double view_h = area.h;
	double content_h = g_content_h;
	if (g_drag >= 0) { content_h += 0; }
	double off = ui_scroll_begin(ui, sid, &g_scroll, area, content_h);
	cairo_translate(cr, 0, area.y);   /* tiles are laid out from y=0 inside the area */

	/* background tap in edit mode exits it */
	if (g_edit) {
		UiId bid = ui_id("start-bg");
		ui_hit(ui, bid, R(0, 0, area.w, content_h));
		if (ui_clicked(ui, bid)) exit_edit();
	}

	if (animating_tiles) {
		double total = 0.36;
		double t = now - g_anim.t0;
		ui->animating = true;
		ui->blocked = true;
		TilesArg arg = {off, now};
		/* offscreen copy of the tiles in pixel space */
		/* the tiles do not change while they swing: draw them once per animation */
		static cairo_surface_t *snap;
		static double snap_t0;
		if (snap && (cairo_image_surface_get_width(snap) != sh.pw || cairo_image_surface_get_height(snap) != sh.ph)) {
			cairo_surface_destroy(snap);
			snap = NULL;
		}
		if (!snap || snap_t0 != g_anim.t0) {
			if (!snap) snap = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sh.pw, sh.ph);
			snap_t0 = g_anim.t0;
			cairo_surface_t *s = snap;
			cairo_t *c2 = cairo_create(s);
			cairo_set_operator(c2, CAIRO_OPERATOR_SOURCE);
			cairo_set_source_rgba(c2, 0, 0, 0, 0);
			cairo_paint(c2);
			cairo_set_operator(c2, CAIRO_OPERATOR_OVER);
			cairo_scale(c2, sh.scale, sh.scale);
			cairo_translate(c2, 0, area.y);
			cairo_t *saved = ui->cr;
			ui->cr = c2;
			TilesArg a2 = {off, now};
			draw_tiles_static(ui, &a2);
			ui->cr = saved;
			cairo_destroy(c2);
			cairo_surface_flush(s);
			(void)arg;
		}
		cairo_save(cr);
		cairo_translate(cr, 0, -area.y);   /* back to screen space for turnstile drawing */
		cairo_scale(cr, 1.0 / sh.scale, 1.0 / sh.scale);
		bool all_done = true;
		for (int i = 0; i < g_tiles.len; i++) {
			Tile *tl = &g_tiles.items[i];
			double d = tile_delay(i);
			double p = CLAMP((t - d) / 0.2, 0, 1);
			if (g_anim.mode == 1 && p < 1) all_done = false;
			if (g_anim.mode == 2 && p < 1) all_done = false;
			double angle = g_anim.mode == 1 ? ease(EASE_IN_QUAD, p) * 80 : (1 - ease(EASE_OUT_CUBIC, p)) * 80;
			Rect pr = R(tl->r.x * sh.scale, (tl->r.y - off + area.y) * sh.scale, tl->r.w * sh.scale, tl->r.h * sh.scale);
			Rect clipr = R(0, area.y * sh.scale, sh.pw, area.h * sh.scale);
			Rect vis = rect_intersect(pr, clipr);
			if (vis.w <= 0 || vis.h <= 0) continue;
			if (angle >= 79.5) continue;
			ui_draw_turnstile(cr, snap, sh.pw, sh.ph, vis, angle, 0, 1.0);
		}
		cairo_restore(cr);
		(void)total;
		if (all_done && t > 0.1) {
			cairo_surface_destroy(snap);
			snap = NULL;
			if (g_anim.mode == 2) g_anim.mode = 0;
			else g_anim.mode = 3;   /* leave finished: stay hidden until the shell switches screens */
		}
		if (g_anim.mode == 3) {
			/* nothing drawn: tiles are gone */
		}
		ui->blocked = saved_blocked;
		ui_scroll_end(ui);
		cairo_restore(cr);
		return;
	}
	if (g_anim.mode == 3) g_anim.mode = 0;

	/* ---- regular drawing with interaction */
	double dx_edge = 0;
	int hover_swap = -1;
	for (int i = 0; i < g_tiles.len; i++) {
		Tile *t = &g_tiles.items[i];
		UiId id = ui_idn("tile", i);
		Rect r = t->r;
		bool dragging_this = g_drag == i;
		if (g_edit) ui_hit_drag(ui, id, r); else ui_hit(ui, id, r);
		bool pressed = ui_pressed(ui, id);

		/* scale animation: pressed / edit mode */
		double target = g_edit ? (g_sel == i ? 1.0 : 0.88) : (pressed ? 0.93 : 1.0);
		if (dragging_this) target = 1.04;
		t->scale = ui_smooth(ui, t->scale ? t->scale : 1, target, 26);

		/* interactions */
		if (ui_clicked(ui, id)) {
			if (g_edit) { g_sel = i; }
			else shell_launch(t->key);
		}
		if (!g_edit && ui_longpressed(ui, id)) {
			g_edit = true;
			g_sel = i;
			g_drag = i;
			ui_capture(ui, id);
			g_drag_x = ui->p.x - (r.x);
			g_drag_y = (ui->p.y - area.y + off) - r.y;
			ui_scroll_to(&g_scroll, g_scroll.off);
		}
		if (g_edit && ui_dragging(ui, id) && ui->p.moved) {
			g_drag = i;
			g_sel = i;
		}
		if (g_edit && g_drag == i && !ui_dragging(ui, id) && !ui->p.down) g_drag = -1;

		Rect sr = r;
		if (dragging_this && ui_dragging(ui, id)) {
			double fx = ui->p.x, fy = ui->p.y - area.y + off;
			sr.x = fx - g_drag_x;
			sr.y = fy - g_drag_y;
			/* find the tile under the finger to swap with */
			for (int j = 0; j < g_tiles.len; j++) {
				if (j == i) continue;
				if (rect_has(g_tiles.items[j].r, fx, fy)) { hover_swap = j; break; }
			}
			if (ui->p.y < area.y + 60) dx_edge = -1; else if (ui->p.y > area.y + area.h - 60) dx_edge = 1;
		}
		/* draw with scale about its centre */
		cairo_save(cr);
		double cx = rect_cx(sr), cy = rect_cy(sr);
		cairo_translate(cr, cx, cy);
		cairo_scale(cr, t->scale, t->scale);
		cairo_translate(cr, -cx, -cy);
		Rect face = sr;
		/* record screen rectangle for transitions / IPC */
		t->screen = ui_to_screen(ui, sr);
		t->shown = t->screen.y + t->screen.h > area.y - 4 && t->screen.y < area.y + area.h + 4;
		if (t->shown) draw_tile_face(ui, t, face, now);
		/* edit buttons on the selected tile */
		if (g_edit && g_sel == i) {
			double bs = 34;
			Rect ub = R(rect_right(sr) - bs / 2 - 4, sr.y - bs / 2 + 4, bs, bs);
			UiId uid = ui_idn("tile-unpin", i);
			ui_hit(ui, uid, ub);
			gfx_fill_circle(cr, rect_cx(ub), rect_cy(ub), bs / 2, ui_pressed(ui, uid) ? COL_WHITE : RGBX(0x1f1f1f));
			gfx_stroke_circle(cr, rect_cx(ub), rect_cy(ub), bs / 2, 2, COL_WHITE);
			icon_draw_ex(cr, ICON_PIN, rect_cx(ub), rect_cy(ub), 18, ui_pressed(ui, uid) ? COL_BLACK : COL_WHITE, ICON_FLAG_SLASH);
			if (ui_clicked(ui, uid)) {
				cairo_restore(cr);
				free(t->key);
				vec_remove(&g_tiles, i);
				g_layout_dirty = true;
				g_sel = -1;
				start_save();
				ui->redraw = true;
				goto done_tiles;
			}
			if (t->size != TILE_WIDE || true) {
				Rect rb = R(rect_right(sr) - bs / 2 - 4, rect_bottom(sr) - bs / 2 - 4, bs, bs);
				UiId rid = ui_idn("tile-resize", i);
				ui_hit(ui, rid, rb);
				gfx_fill_circle(cr, rect_cx(rb), rect_cy(rb), bs / 2, ui_pressed(ui, rid) ? COL_WHITE : RGBX(0x1f1f1f));
				gfx_stroke_circle(cr, rect_cx(rb), rect_cy(rb), bs / 2, 2, COL_WHITE);
				icon_draw(cr, t->size == TILE_WIDE ? ICON_CHEVRON_L : ICON_CHEVRON_R, rect_cx(rb), rect_cy(rb), 18, ui_pressed(ui, rid) ? COL_BLACK : COL_WHITE);
				if (ui_clicked(ui, rid)) {
					t->size = t->size == TILE_SMALL ? TILE_MEDIUM : t->size == TILE_MEDIUM ? TILE_WIDE : TILE_SMALL;
					g_layout_dirty = true;
					start_save();
				}
			}
		}
		cairo_restore(cr);
	}
	if (hover_swap >= 0 && g_drag >= 0 && g_drag != hover_swap) {
		Tile moved = g_tiles.items[g_drag];
		vec_remove(&g_tiles, g_drag);
		vec_push(&g_tiles, moved);   /* placeholder to grow, then rotate into place */
		g_tiles.len--;
		/* insert at hover_swap */
		if (g_tiles.len >= g_tiles.cap) { g_tiles.cap *= 2; g_tiles.items = xrealloc(g_tiles.items, (size_t)g_tiles.cap * sizeof(Tile)); }
		memmove(&g_tiles.items[hover_swap + 1], &g_tiles.items[hover_swap], (size_t)(g_tiles.len - hover_swap) * sizeof(Tile));
		g_tiles.items[hover_swap] = moved;
		g_tiles.len++;
		g_drag = hover_swap;
		g_sel = hover_swap;
		g_layout_dirty = true;
	}
	if (dx_edge != 0 && g_drag >= 0) {
		g_scroll.off = CLAMP(g_scroll.off + dx_edge * 6, 0, MAX(0, g_content_h - area.h));
		ui->animating = true;
	}
done_tiles:

	/* "all apps" arrow */
	{
		double by = 8;
		int maxrow = 0;
		for (int i = 0; i < g_tiles.len; i++) maxrow = MAX(maxrow, g_tiles.items[i].uy + g_tiles.items[i].uh);
		by = 8 + maxrow * (g_unit + g_gap) + 14;
		double bs = 46;
		Rect ar = R(area.w - 12 - bs, by, bs, bs);
		UiId aid = ui_id("start-allapps");
		if (!g_edit) {
			ui_hit(ui, aid, R(ar.x - 10, ar.y - 10, bs + 20, bs + 20));
			bool pr = ui_pressed(ui, aid);
			gfx_stroke_circle(cr, rect_cx(ar), rect_cy(ar), bs / 2, 2, pr ? th->sub : th->fg);
			icon_draw(cr, ICON_CHEVRON_R, rect_cx(ar) + 1, rect_cy(ar), 20, pr ? th->sub : th->fg);
			if (ui_clicked(ui, aid)) ui_pager_goto(&sh.home_pager, 1);
		}
	}
	ui_scroll_end(ui);
	cairo_restore(cr);
	(void)view_h; (void)xoff;
}

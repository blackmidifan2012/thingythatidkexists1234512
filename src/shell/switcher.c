#include "shell.h"
#include "../apps/apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../util.h"

#include <math.h>

/* Task switcher: a horizontal stack of cards for every running app. */

typedef struct {
	const AppDef *def;
	View *view;
	char *name;
	DesktopApp *da;
	bool current;              /* the app that was in front when the switcher opened */
	cairo_surface_t *face;     /* the card as drawn by card_face_cb(), see draw_card() */
	Image *face_icon;          /* da's icon when the face was drawn (NULL while it loads) */
} Item;

static VEC(Item) g_items;
static Pager g_pager;
static double g_dismiss[32];    /* per-card slide-away progress */
static int g_dismissing = -1;

static void item_free(Item *it) {
	free(it->name);
	if (it->face) cairo_surface_destroy(it->face);
}

static void clear_items(void) {
	for (int i = 0; i < g_items.len; i++) item_free(&g_items.items[i]);
	g_items.len = 0;
}

static void collect(void) {
	clear_items();
	/* the foreground app first, then the rest */
	if (sh.screen == SCR_APP && sh.cur_app) {
		Item it = {.def = sh.cur_app, .name = xstrdup(tr(sh.cur_app->name)), .current = true};
		vec_push(&g_items, it);
	} else if (sh.screen == SCR_EXT && sh.cur_view) {
		Item it = {.view = sh.cur_view, .da = sh.cur_view->app, .current = true,
		           .name = xstrdup(sh.cur_view->app ? sh.cur_view->app->name : sh.cur_view->title ? sh.cur_view->title : "?")};
		vec_push(&g_items, it);
	}
	for (int i = sh.app_stack.len - 1; i >= 0; i--) {
		const AppDef *d = sh.app_stack.items[i];
		if (d == sh.cur_app && sh.screen == SCR_APP) continue;
		Item it = {.def = d, .name = xstrdup(tr(d->name))};
		vec_push(&g_items, it);
	}
	View *v;
	wl_list_for_each(v, &srv.views, link) {
		if (!v->mapped || v->is_dialog) continue;
		if (v == sh.cur_view && sh.screen == SCR_EXT) continue;
		Item it = {.view = v, .da = v->app,
		           .name = xstrdup(v->app ? v->app->name : v->title && *v->title ? v->title : (v->app_id ? v->app_id : "?"))};
		vec_push(&g_items, it);
	}
}

void switcher_open(void) {
	collect();
	g_pager = (Pager){0};
	g_dismissing = -1;
	memset(g_dismiss, 0, sizeof g_dismiss);
}

/* The card faces are a few MB each: drop them once the switcher is gone. */
void switcher_close(void) { clear_items(); }

static void activate(Item *it) {
	if (it->def) shell_open_builtin(it->def->id, NULL);
	else if (it->view && it->view->mapped) shell_transition(SCR_EXT, NULL, it->view, R(0, 0, 0, 0), sh.ui.th.accent, false);
}

static void close_item(int i) {
	Item *it = &g_items.items[i];
	if (it->def) shell_close_app(it->def);
	else if (it->view) view_close(it->view);
	item_free(it);
	vec_remove(&g_items, i);
	if (g_pager.target >= g_items.len) g_pager.target = MAX(0, g_items.len - 1);
	memset(g_dismiss, 0, sizeof g_dismiss);
	g_dismissing = -1;
}

#define FACE_PAD 2.0       /* room for the border around the card */
#define FACE_LABEL 40.0    /* room for the small tile and name under a miniature */

typedef struct { Item *it; double cw, ch; } FaceArg;

/* Everything on a card but the close button, in dp with the card at (0, 0). */
static void card_face_cb(UI *ui, void *arg) {
	FaceArg *a = arg;
	Item *it = a->it;
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect card = R(0, 0, a->cw, a->ch);
	cairo_translate(cr, FACE_PAD, FACE_PAD);
	Color bg = it->def && it->def->color ? RGBX(it->def->color) : th->accent;
	Color fg = COL_WHITE;
	double cw = card.w, isz = cw * 0.42;
	if (it->def && it->def->draw) {
		/* built-in apps show their page in miniature, without a focused field or keyboard */
		UiId focus = ui->focus;
		bool kbd = ui->kbd_wanted;
		int purpose = ui->kbd_purpose;
		ui->focus = 0;
		ui_clip_push(ui, card);
		cairo_save(cr);
		double s = card.w / sh.w;
		cairo_scale(cr, s, s);
		gfx_fill_rect(cr, R(0, 0, sh.w, sh.h), th->bg);
		it->def->draw(ui, R(0, sh.status_h, sh.w, sh.h - sh.status_h - sh.nav_h));
		cairo_restore(cr);
		ui_clip_pop(ui);
		ui->focus = focus;
		ui->kbd_wanted = kbd;
		ui->kbd_purpose = purpose;
		gfx_stroke_rect(cr, R(card.x - 1, card.y - 1, card.w + 2, card.h + 2), 1, th->dim);
		/* small tile + name under the miniature */
		Rect tr = R(card.x, rect_bottom(card) + 10, 26, 26);
		gfx_fill_rect(cr, tr, bg);
		icon_draw(cr, it->def->icon, rect_cx(tr), rect_cy(tr), 17, fg);
		gfx_text(cr, it->name, tr.x + 34, tr.y + 1, 18, FW_SEMILIGHT, th->fg, TA_LEFT, cw - 34, 1);
		return;
	}
	gfx_fill_rect(cr, card, bg);
	if (it->def) icon_draw(cr, it->def->icon, rect_cx(card), rect_cy(card) - 14, isz, fg);
	else if (it->da) {
		Image *img = desktop_icon(it->da, 128);
		it->face_icon = img;
		if (img && cfg.mono_icons) gfx_image_silhouette(cr, img, R(rect_cx(card) - isz / 2, rect_cy(card) - isz / 2 - 14, isz, isz), fg);
		else if (img) gfx_image_contain(cr, img, R(rect_cx(card) - isz / 2, rect_cy(card) - isz / 2 - 14, isz, isz), 1.0);
		else gfx_text_in(cr, i18n_upper(it->name), R(card.x, card.y, card.w, card.h - 30), isz, FW_LIGHT, fg, TA_CENTER, true);
	} else icon_draw(cr, ICON_TASKS, rect_cx(card), rect_cy(card) - 14, isz, fg);
	gfx_text(cr, it->name, rect_cx(card), rect_bottom(card) - 46, 20, FW_SEMILIGHT, fg, TA_CENTER, cw - 24, 1);
	if (it->view && it->view->title && *it->view->title && it->view->app)
		gfx_text(cr, it->view->title, rect_cx(card), rect_bottom(card) - 22, 13, FW_REGULAR, col_alpha(fg, 0.8f), TA_CENTER, cw - 24, 1);
	if (it->current) gfx_text(cr, i18n_upper(_("current")), card.x + 10, card.y + 8, 11, FW_SEMIBOLD, col_alpha(fg, 0.7f), TA_LEFT, 0, 1);
}

/* Draws the card into `card`. Its face is rendered once and then only moved and scaled:
 * drawing every app's page and the labels again on each frame of a swipe, at a new scale
 * each time (so no cached text either), made the switcher crawl. */
static void draw_card(UI *ui, Item *it, Rect card, bool moving) {
	cairo_t *cr = ui->cr;
	int pw = (int)ceil((card.w + 2 * FACE_PAD) * sh.scale), ph = (int)ceil((card.h + FACE_PAD + FACE_LABEL) * sh.scale);
	/* redrawn when the size changes or the app's icon has finished loading meanwhile */
	if (it->face && (cairo_image_surface_get_width(it->face) != pw || cairo_image_surface_get_height(it->face) != ph ||
	                 (it->da && it->da->icon != it->face_icon))) {
		cairo_surface_destroy(it->face);
		it->face = NULL;
	}
	if (!it->face) {
		it->face = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
		FaceArg a = {it, card.w, card.h};
		shell_draw_into(it->face, sh.scale, card_face_cb, &a);
	}
	double x = card.x - FACE_PAD, y = card.y - FACE_PAD, dw = 1, dh = 1;
	cairo_user_to_device(cr, &x, &y);
	cairo_user_to_device_distance(cr, &dw, &dh);
	cairo_save(cr);
	cairo_identity_matrix(cr);
	if (fabs(dw - sh.scale) < 1e-3 && fabs(dh - sh.scale) < 1e-3) {
		/* full size: a straight copy on whole pixels stays sharp */
		cairo_translate(cr, round(x), round(y));
	} else {
		cairo_translate(cr, x, y);
		cairo_scale(cr, dw / sh.scale, dh / sh.scale);
	}
	cairo_set_source_surface(cr, it->face, 0, 0);
	/* nearest sampling is much cheaper and looks the same while the cards move */
	cairo_pattern_set_filter(cairo_get_source(cr), moving ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD);
	cairo_rectangle(cr, 0, 0, pw, ph);
	cairo_fill(cr);
	cairo_restore(cr);
}

void switcher_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	/* external windows may have disappeared meanwhile */
	for (int i = 0; i < g_items.len; i++) {
		Item *it = &g_items.items[i];
		if (it->view) {
			bool alive = false;
			View *v;
			wl_list_for_each(v, &srv.views, link) if (v == it->view && v->mapped) alive = true;
			if (!alive) { item_free(it); vec_remove(&g_items, i); i--; }
		}
	}
	gfx_text(cr, i18n_upper(_("running apps")), 24, area.y + 8, 15, FW_SEMIBOLD, th->sub, TA_LEFT, area.w - 48, 1);
	if (!g_items.len) {
		gfx_text(cr, _("No apps are running"), area.w / 2, area.y + area.h / 2 - 20, 24, FW_SEMILIGHT, th->sub, TA_CENTER, area.w - 40, 1);
		return;
	}
	/* cards keep the screen's aspect ratio so built-in apps can show a live miniature */
	double ch = MIN(area.h * 0.72, area.w * 0.66 * sh.h / sh.w), cw = ch * sh.w / sh.h;
	double cy = area.y + (area.h - ch) / 2 + 4;
	Rect view = R(0, area.y, area.w, area.h);
	ui_pager_update(ui, ui_id("switcher-pager"), &g_pager, view, g_items.len);
	g_pager.pos = CLAMP(g_pager.pos, -0.2, g_items.len - 0.8);
	bool moving = g_pager.dragging || g_pager.pos != g_pager.target;
	for (int i = 0; i < g_items.len && i < 32; i++) if (g_dismiss[i] > 0) moving = true;

	for (int i = 0; i < g_items.len; i++) {
		Item *it = &g_items.items[i];
		double d = i - g_pager.pos;
		if (d < -1.6 || d > 1.6) continue;
		double spacing = cw + 14;
		double x = (area.w - cw) / 2 + d * spacing;
		double scale = 1 - MIN(fabs(d), 1) * 0.1;
		double dis = (i < 32) ? g_dismiss[i] : 0;
		Rect card = R(x, cy - dis * (ch + 80), cw, ch);
		cairo_save(cr);
		double ccx = rect_cx(card), ccy = rect_cy(card);
		cairo_translate(cr, ccx, ccy);
		cairo_scale(cr, scale, scale);
		cairo_translate(cr, -ccx, -ccy);
		UiId cid = ui_idn("switch-card", i);
		ui_scroller_push(ui, cid, card, UI_AXIS_Y);
		UiId hid = ui_idn("switch-hit", i);
		ui_hit(ui, hid, card);
		/* swipe up to close */
		double dx, dy, vx, vy;
		if (ui_drag_delta(ui, cid, &dx, &dy) && i < 32) { g_dismiss[i] = CLAMP(g_dismiss[i] - dy / (ch + 80), 0, 1); g_dismissing = i; }
		if (ui_fling(ui, cid, &vx, &vy) && i < 32) {
			if (g_dismiss[i] > 0.2 || vy < -600) { g_dismiss[i] = 0.2; ui->animating = true; g_dismissing = -2 - i; }
			else g_dismiss[i] = 0;
		}
		ui_scroller_pop(ui);
		if (i < 32 && g_dismissing == -2 - i) {
			g_dismiss[i] += ui->dt * 3.2;
			ui->animating = true;
			if (g_dismiss[i] >= 1) { cairo_restore(cr); close_item(i); ui->redraw = true; return; }
		} else if (i < 32 && !ui_dragging(ui, cid) && g_dismiss[i] > 0 && g_dismissing != -2 - i) {
			g_dismiss[i] = ui_smooth(ui, g_dismiss[i], 0, 14);
		}
		draw_card(ui, it, card, moving);
		/* close button */
		Rect xr = R(rect_right(card) - 40, card.y + 6, 34, 34);
		UiId xid = ui_idn("switch-x", i);
		ui_hit(ui, xid, R(xr.x - 6, xr.y - 6, xr.w + 12, xr.h + 12));
		gfx_fill_circle(cr, rect_cx(xr), rect_cy(xr), 15, ui_pressed(ui, xid) ? COL_WHITE : RGBA(0x000000, 0.35f));
		icon_draw(cr, ICON_CLOSE, rect_cx(xr), rect_cy(xr), 14, ui_pressed(ui, xid) ? COL_BLACK : COL_WHITE);
		cairo_restore(cr);
		if (ui_clicked(ui, xid)) { close_item(i); ui->redraw = true; return; }
		if (ui_clicked(ui, hid)) { Item copy = *it; activate(&copy); return; }
	}
	/* page dots */
	for (int i = 0; i < g_items.len && g_items.len > 1; i++) {
		double dx = area.w / 2 + (i - (g_items.len - 1) / 2.0) * 14;
		gfx_fill_circle(cr, dx, area.y + area.h - 18, 3.5, i == (int)floor(g_pager.pos + 0.5) ? th->fg : th->dim);
	}
	ui_pager_end(ui);
}

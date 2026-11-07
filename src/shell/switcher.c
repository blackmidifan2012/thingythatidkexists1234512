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
} Item;

static VEC(Item) g_items;
static Pager g_pager;
static double g_dismiss[32];    /* per-card slide-away progress */
static int g_dismissing = -1;

static void clear_items(void) {
	for (int i = 0; i < g_items.len; i++) free(g_items.items[i].name);
	g_items.len = 0;
}

static void collect(void) {
	clear_items();
	/* the foreground app first, then the rest */
	if (sh.screen == SCR_APP && sh.cur_app) {
		Item it = {.def = sh.cur_app, .name = xstrdup(tr(sh.cur_app->name))};
		vec_push(&g_items, it);
	} else if (sh.screen == SCR_EXT && sh.cur_view) {
		Item it = {.view = sh.cur_view, .da = sh.cur_view->app,
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

static void activate(Item *it) {
	if (it->def) shell_open_builtin(it->def->id, NULL);
	else if (it->view && it->view->mapped) shell_transition(SCR_EXT, NULL, it->view, R(0, 0, 0, 0), sh.ui.th.accent, false);
}

static void close_item(int i) {
	Item *it = &g_items.items[i];
	if (it->def) shell_close_app(it->def);
	else if (it->view) view_close(it->view);
	free(it->name);
	vec_remove(&g_items, i);
	if (g_pager.target >= g_items.len) g_pager.target = MAX(0, g_items.len - 1);
	memset(g_dismiss, 0, sizeof g_dismiss);
	g_dismissing = -1;
}

/* Paints a built-in app's page scaled down into `card` with input disabled. */
static void draw_thumbnail(UI *ui, const AppDef *def, Rect card) {
	cairo_t *cr = ui->cr;
	UiId focus = ui->focus;
	bool kbd = ui->kbd_wanted, blocked = ui->blocked;
	int purpose = ui->kbd_purpose;
	ui->focus = 0;
	ui->blocked = true;
	ui_clip_push(ui, card);
	cairo_save(cr);
	double s = card.w / sh.w;
	cairo_translate(cr, card.x, card.y);
	cairo_scale(cr, s, s);
	gfx_fill_rect(cr, R(0, 0, sh.w, sh.h), ui->th.bg);
	def->draw(ui, R(0, sh.status_h, sh.w, sh.h - sh.status_h - sh.nav_h));
	cairo_restore(cr);
	ui_clip_pop(ui);
	gfx_stroke_rect(cr, R(card.x - 1, card.y - 1, card.w + 2, card.h + 2), 1, ui->th.dim);
	ui->blocked = blocked;
	ui->focus = focus;
	ui->kbd_wanted = kbd;
	ui->kbd_purpose = purpose;
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
			if (!alive) { free(it->name); vec_remove(&g_items, i); i--; }
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
		/* card */
		Color bg = it->def && it->def->color ? RGBX(it->def->color) : th->accent;
		Color fg = COL_WHITE;
		double isz = cw * 0.42;
		bool live = it->def && it->def->draw;
		if (live) draw_thumbnail(ui, it->def, card);
		else gfx_fill_rect(cr, card, bg);
		if (live) {
			/* small tile + name under the miniature */
			Rect tr = R(card.x, rect_bottom(card) + 10, 26, 26);
			gfx_fill_rect(cr, tr, bg);
			icon_draw(cr, it->def->icon, rect_cx(tr), rect_cy(tr), 17, fg);
			gfx_text(cr, it->name, tr.x + 34, tr.y + 1, 18, FW_SEMILIGHT, th->fg, TA_LEFT, cw - 34, 1);
		} else if (it->def) icon_draw(cr, it->def->icon, rect_cx(card), rect_cy(card) - 14, isz, fg);
		else if (it->da) {
			Image *img = desktop_icon(it->da, 128);
			if (img && cfg.mono_icons) gfx_image_silhouette(cr, img, R(rect_cx(card) - isz / 2, rect_cy(card) - isz / 2 - 14, isz, isz), fg);
			else if (img) gfx_image_contain(cr, img, R(rect_cx(card) - isz / 2, rect_cy(card) - isz / 2 - 14, isz, isz), 1.0);
			else gfx_text_in(cr, i18n_upper(it->name), R(card.x, card.y, card.w, card.h - 30), isz, FW_LIGHT, fg, TA_CENTER, true);
		} else icon_draw(cr, ICON_TASKS, rect_cx(card), rect_cy(card) - 14, isz, fg);
		if (!live) gfx_text(cr, it->name, rect_cx(card), rect_bottom(card) - 46, 20, FW_SEMILIGHT, fg, TA_CENTER, cw - 24, 1);
		if (it->view && it->view->title && *it->view->title && it->view->app)
			gfx_text(cr, it->view->title, rect_cx(card), rect_bottom(card) - 22, 13, FW_REGULAR, col_alpha(fg, 0.8f), TA_CENTER, cw - 24, 1);
		/* close button */
		Rect xr = R(rect_right(card) - 40, card.y + 6, 34, 34);
		UiId xid = ui_idn("switch-x", i);
		ui_hit(ui, xid, R(xr.x - 6, xr.y - 6, xr.w + 12, xr.h + 12));
		gfx_fill_circle(cr, rect_cx(xr), rect_cy(xr), 15, ui_pressed(ui, xid) ? COL_WHITE : RGBA(0x000000, 0.35f));
		icon_draw(cr, ICON_CLOSE, rect_cx(xr), rect_cy(xr), 14, ui_pressed(ui, xid) ? COL_BLACK : COL_WHITE);
		/* current-app tag */
		if (i == 0 && !live) gfx_text(cr, i18n_upper(_("current")), card.x + 10, card.y + 8, 11, FW_SEMIBOLD, col_alpha(fg, 0.7f), TA_LEFT, 0, 1);
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

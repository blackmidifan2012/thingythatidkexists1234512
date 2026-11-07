/* Minesweeper: three difficulties, flat Metro board, first tap always safe, flood fill,
 * chording, flag mode, timer and best times per level. */
#include "apps.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <math.h>

enum { C_MINE = 1, C_OPEN = 2, C_FLAG = 4, C_BOOM = 8 };
enum { G_READY, G_PLAY, G_WON, G_LOST };
#define MAXC (30 * 16)

typedef struct { int cols, rows, mines; const char *name; } Level;
static const Level LEVELS[3] = {
	{9, 9, 10, N_("beginner")}, {16, 16, 40, N_("intermediate")}, {16, 30, 99, N_("expert")},
};

static struct {
	bool loaded;
	int level, state, opened, flags;
	uint8_t cell[MAXC], cnt[MAXC];
	bool flag_mode;
	double elapsed, last_draw, seen;   /* seen: now_sec() of the last draw */
	int best[3];                /* seconds, 0 = none */
	Timer *timer;
	Scroll scroll;
	AppBar bar;
} G;

static const Level *L(void) { return &LEVELS[G.level]; }
static int ncells(void) { return L()->cols * L()->rows; }

static void save(void) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "level", G.level);
	cJSON *b = cJSON_AddArrayToObject(o, "best");
	for (int i = 0; i < 3; i++) cJSON_AddItemToArray(b, cJSON_CreateNumber(G.best[i]));
	app_json_save("minesweeper.json", o);
	cJSON_Delete(o);
}

static void load(void) {
	if (G.loaded) return;
	G.loaded = true;
	cJSON *o = app_json_load("minesweeper.json");
	if (!o) return;
	cJSON *lv = cJSON_GetObjectItem(o, "level"), *b = cJSON_GetObjectItem(o, "best");
	if (cJSON_IsNumber(lv)) G.level = CLAMP(lv->valueint, 0, 2);
	for (int i = 0; i < 3 && cJSON_IsArray(b); i++) {
		cJSON *v = cJSON_GetArrayItem(b, i);
		if (cJSON_IsNumber(v)) G.best[i] = MAX(0, v->valueint);
	}
	cJSON_Delete(o);
}

static void new_game(void) {
	memset(G.cell, 0, sizeof G.cell);
	memset(G.cnt, 0, sizeof G.cnt);
	G.state = G_READY;
	G.opened = G.flags = 0;
	G.elapsed = 0;
	G.flag_mode = false;
	ui_scroll_to(&G.scroll, 0);
	if (G.timer) timer_set(G.timer, 0);
	shell_invalidate();
}

#define FOR_NB(i, j) \
	for (int dy_ = -1; dy_ <= 1; dy_++) for (int dx_ = -1; dx_ <= 1; dx_++) \
		if ((dx_ || dy_) && (i) % L()->cols + dx_ >= 0 && (i) % L()->cols + dx_ < L()->cols && \
		    (i) / L()->cols + dy_ >= 0 && (i) / L()->cols + dy_ < L()->rows && \
		    ((j) = (i) + dy_ * L()->cols + dx_, 1))

static bool near(int a, int b) {
	int c = L()->cols;
	return abs(a % c - b % c) <= 1 && abs(a / c - b / c) <= 1;
}

/* Mines are placed after the first tap, never on or next to it. */
static void generate(int safe) {
	int n = ncells(), placed = 0;
	unsigned seed = (unsigned)time(NULL) ^ (now_msec() * 2654435761u);
	while (placed < L()->mines) {
		seed = seed * 1103515245u + 12345u;
		int i = (int)((seed >> 8) % (unsigned)n);
		if ((G.cell[i] & C_MINE) || near(i, safe)) continue;
		G.cell[i] |= C_MINE;
		placed++;
	}
	for (int i = 0; i < n; i++) {
		int j;
		FOR_NB(i, j) if (G.cell[j] & C_MINE) G.cnt[i]++;
	}
}

static void tick(void *ud) {
	(void)ud;
	/* not visible for a while: stop waking up, draw() re-arms the timer */
	if (G.state != G_PLAY || now_sec() - G.seen > 3) timer_set(G.timer, 0);
	shell_invalidate();
}

static void finish_cb(int b, void *ud) {
	(void)ud;
	if (b == 0) new_game();
}

static void game_over(bool won) {
	G.state = won ? G_WON : G_LOST;
	timer_set(G.timer, 0);
	int secs = (int)G.elapsed;
	char body[256];
	if (won) {
		bool record = !G.best[G.level] || secs < G.best[G.level];
		if (record) { G.best[G.level] = MAX(secs, 1); save(); }
		if (record) snprintf(body, sizeof body, _("you cleared the board in %d seconds. that's a new best time!"), secs);
		else snprintf(body, sizeof body, _("you cleared the board in %d seconds. your best time is %d seconds."), secs,
		              G.best[G.level]);
		for (int i = 0; i < ncells(); i++)
			if ((G.cell[i] & C_MINE) && !(G.cell[i] & C_FLAG)) { G.cell[i] |= C_FLAG; G.flags++; }
	} else {
		snprintf(body, sizeof body, _("you hit a mine after %d seconds."), secs);
	}
	dialog_message(won ? _("you win!") : _("game over"), body, _("play again"), _("close"), finish_cb, NULL);
}

static void reveal(int i) {
	if (G.cell[i] & (C_OPEN | C_FLAG)) return;
	if (G.state == G_READY) {
		generate(i);
		G.state = G_PLAY;
		if (!G.timer) G.timer = timer_add(1000, true, tick, NULL);
		timer_set(G.timer, 1000);
	}
	if (G.cell[i] & C_MINE) {
		G.cell[i] |= C_BOOM;
		for (int k = 0; k < ncells(); k++)
			if ((G.cell[k] & C_MINE) && !(G.cell[k] & C_FLAG)) G.cell[k] |= C_OPEN;
		game_over(false);
		return;
	}
	static int stack[MAXC];
	int sp = 0;
	stack[sp++] = i;
	G.cell[i] |= C_OPEN;
	while (sp) {
		int c = stack[--sp], j;
		G.opened++;
		if (G.cnt[c]) continue;
		FOR_NB(c, j) if (!(G.cell[j] & (C_OPEN | C_FLAG))) { G.cell[j] |= C_OPEN; stack[sp++] = j; }
	}
	if (G.opened == ncells() - L()->mines) game_over(true);
}

static void toggle_flag(int i) {
	if (G.cell[i] & C_OPEN) return;
	G.cell[i] ^= C_FLAG;
	G.flags += (G.cell[i] & C_FLAG) ? 1 : -1;
}

/* Tapping a number whose flags are all placed opens the remaining neighbours. */
static void chord(int i) {
	int j, f = 0;
	FOR_NB(i, j) if (G.cell[j] & C_FLAG) f++;
	if (f != G.cnt[i]) return;
	FOR_NB(i, j) if (G.state == G_PLAY && !(G.cell[j] & (C_OPEN | C_FLAG))) reveal(j);
}

static void tap(int i, bool flag) {
	if (G.state == G_WON || G.state == G_LOST) return;
	if (G.cell[i] & C_OPEN) { if (G.cnt[i]) chord(i); }
	else if (flag) toggle_flag(i);
	else reveal(i);
	shell_invalidate();
}

static void level_cb(int b, void *ud) {
	(void)ud;
	if (b < 0 || b > 2) return;
	G.level = b;
	save();
	new_game();
}

static void best_cb(int b, void *ud) {
	(void)ud;
	if (b != 1) return;
	memset(G.best, 0, sizeof G.best);
	save();
}

static void show_best(void) {
	char body[400], v[3][64];
	for (int i = 0; i < 3; i++) {
		if (G.best[i]) snprintf(v[i], sizeof v[i], _("%d seconds"), G.best[i]);
		else str_copy(v[i], sizeof v[i], _("not yet played"));
	}
	snprintf(body, sizeof body, "%s: %s\n%s: %s\n%s: %s", tr(LEVELS[0].name), v[0], tr(LEVELS[1].name), v[1],
	         tr(LEVELS[2].name), v[2]);
	dialog_message(_("best times"), body, _("close"), _("reset"), best_cb, NULL);
}

static Color num_color(const Theme *th, int n) {
	static const uint32_t c[9] = {0, 0x1BA1E2, 0x60A917, 0xE51400, 0xAA00FF, 0xF0A30A, 0x00ABA9, 0, 0};
	if (n == 7) return th->fg;
	if (n == 8) return th->sub;
	return RGBX(c[n]);
}

static void draw_cell(UI *ui, int i, Rect r, double gap) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	uint8_t c = G.cell[i];
	UiId id = ui_idn("ms-cell", i);
	ui_hit(ui, id, R(r.x - gap / 2, r.y - gap / 2, r.w + gap, r.h + gap));   /* no dead gaps */
	bool over = G.state == G_WON || G.state == G_LOST;
	double s = r.w;
	if (c & C_OPEN) {
		gfx_fill_rect(cr, r, (c & C_BOOM) ? RGBX(0xE51400) : th->chrome2);
		if (c & C_MINE) icon_draw(cr, ICON_MINE, rect_cx(r), rect_cy(r), s * 0.62, (c & C_BOOM) ? COL_WHITE : th->fg);
		else if (G.cnt[i]) {
			char t[4];
			snprintf(t, sizeof t, "%d", G.cnt[i]);
			gfx_text_in(cr, t, r, s * 0.6, FW_SEMIBOLD, num_color(th, G.cnt[i]), TA_CENTER, true);
		}
	} else {
		bool pr = ui_pressed(ui, id) && !over;
		Color bg = pr ? col_mix(th->accent, th->fg, 0.3f) : th->accent;
		gfx_fill_rect(cr, r, bg);
		if (c & C_FLAG) {
			bool wrong = G.state == G_LOST && !(c & C_MINE);
			icon_draw_ex(cr, ICON_FLAG, rect_cx(r), rect_cy(r), s * 0.58, th->on_accent, wrong ? ICON_FLAG_SLASH : 0);
		}
	}
	if (ui_longpressed(ui, id) && !over && !(c & C_OPEN)) { toggle_flag(i); shell_invalidate(); }
	else if (ui_clicked(ui, id)) tap(i, G.flag_mode);
}

static void stat_item(UI *ui, double x, double y, Icon ic, const char *v) {
	icon_draw(ui->cr, ic, x + 14, y + 22, 26, ui->th.sub);
	gfx_text(ui->cr, v, x + 36, y, 36, FW_LIGHT, ui->th.fg, TA_LEFT, 0, 1);
}

static void minesweeper_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	load();
	if (G.state == G_PLAY) {
		double gap = ui->now - G.last_draw;
		if (G.last_draw > 0 && gap > 0 && gap < 1.5) G.elapsed += gap;
		if (G.timer) timer_set(G.timer, 1000 - (int)(fmod(G.elapsed, 1.0) * 1000) + 5);
	}
	G.last_draw = ui->now;
	G.seen = now_sec();

	Rect a = appbar_content(&G.bar, area);
	double y = ui_page_header(ui, 14, a.y + 6, _("Minesweeper"), tr(L()->name));
	char buf[32];
	snprintf(buf, sizeof buf, "%d", L()->mines - G.flags);
	stat_item(ui, 18, y, ICON_MINE, buf);
	int secs = (int)G.elapsed;
	snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
	stat_item(ui, 140, y, ICON_TIMER, buf);
	if (G.flag_mode) {
		const char *fm = _("tap to flag");
		gfx_text(cr, fm, rect_right(a) - 24, y + 14, 17, FW_SEMIBOLD, th->accent, TA_RIGHT, a.w - 300, 1);
	}
	y += 56;

	int cols = L()->cols, rows = L()->rows;
	double gap = 2;
	double cs = floor((a.w - 24 + gap) / cols) - gap;
	double fit_h = floor((rect_bottom(a) - y - 12 + gap) / rows) - gap;
	if (fit_h >= 28) cs = MIN(cs, fit_h);   /* whole board on screen when cells stay large enough */
	cs = CLAMP(cs, 24, 72);
	double bw = cols * (cs + gap) - gap, bh = rows * (cs + gap) - gap;
	Rect view = R(a.x, y, a.w, rect_bottom(a) - y);
	double off = ui_scroll_begin(ui, ui_id("ms-board"), &G.scroll, view, bh + 12);
	cairo_translate(cr, view.x, view.y);
	double x0 = floor((view.w - bw) / 2);
	for (int r = 0; r < rows; r++) {
		double cy = r * (cs + gap);
		if (cy + cs < off || cy > off + view.h) continue;
		for (int c = 0; c < cols; c++) draw_cell(ui, r * cols + c, R(x0 + c * (cs + gap), cy, cs, cs), gap);
	}
	ui_scroll_end(ui);

	AppBarBtn btns[2] = {{ICON_REFRESH, _("new game")},
	                     {G.flag_mode ? ICON_MINE : ICON_FLAG, G.flag_mode ? _("dig mode") : _("flag mode")}};
	const char *menu[2] = {_("difficulty"), _("best times")};
	int act = appbar_draw(ui, &G.bar, area, btns, 2, menu, 2);
	if (act == 0) new_game();
	else if (act == 1) G.flag_mode = !G.flag_mode;
	else if (act == APPBAR_MENU) {
		const char *items[3] = {tr(LEVELS[0].name), tr(LEVELS[1].name), tr(LEVELS[2].name)};
		dialog_list(_("difficulty"), items, 3, G.level, level_cb, NULL);
	} else if (act == APPBAR_MENU + 1) show_best();
}

static bool minesweeper_back(void) { return appbar_back(&G.bar); }

static void minesweeper_close(void) {
	if (G.timer) { timer_free(G.timer); G.timer = NULL; }
	new_game();
}

const AppDef app_minesweeper = {
	.id = "minesweeper", .name = N_("Minesweeper"), .icon = ICON_MINE, .color = 0,
	.keywords = "game mines puzzle",
	.draw = minesweeper_draw, .back = minesweeper_back, .close = minesweeper_close,
};

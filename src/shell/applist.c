#include "shell.h"
#include "../apps/apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <glib.h>
#include <math.h>

typedef struct {
	const AppDef *def;
	DesktopApp *da;
	char *name;
	char letter[8];
	char *fold;
	uint32_t *key;           /* collation key */
	int keylen;
} Entry;

static VEC(Entry) g_entries;
static bool g_dirty = true;
static int g_built_lang = -1, g_built_count = -1;
static Scroll g_scroll;
static char g_search[96];
static bool g_jump_open;
static Anim g_jump_anim;
static Entry *g_menu_entry;

void applist_reset(void) {
	g_search[0] = 0;
	g_jump_open = false;
	g_scroll = (Scroll){0};
	g_dirty = true;
}

bool applist_back(void) {
	if (g_jump_open) { g_jump_open = false; return true; }
	if (g_search[0]) { g_search[0] = 0; return true; }
	return false;
}

/* ------------------------------------------------------------- collation -- */

static const char *TR_ALPHABET = "abcçdefgğhıijklmnoöprsştuüvyzqwx";

static uint32_t char_rank(uint32_t cp) {
	cp = g_unichar_tolower(cp);
	if (cp == 0x130) cp = 'i';
	if (i18n_lang() == LANG_TR) {
		int idx = 0;
		for (const char *p = TR_ALPHABET; *p; idx++) {
			uint32_t a;
			p += utf8_decode(p, &a);
			if (a == cp) return 0x100 + idx;
		}
	}
	/* strip diacritics via the folding table */
	char buf[8];
	utf8_encode(cp, buf);
	char *f = str_fold(buf);
	uint32_t r = (uint8_t)f[0];
	free(f);
	if (r >= 'a' && r <= 'z') return 0x100 + (r - 'a') * 2;
	if (r >= '0' && r <= '9') return 0x80 + (r - '0');
	return 0x200 + cp;
}

static void make_key(Entry *e) {
	int n = utf8_strlen(e->name);
	e->key = xmalloc((size_t)(n + 1) * sizeof(uint32_t));
	int i = 0;
	for (const char *p = e->name; *p && i < n;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		e->key[i++] = char_rank(cp);
	}
	e->keylen = i;
}

static int entry_cmp(const void *x, const void *y) {
	const Entry *a = x, *b = y;
	int n = MIN(a->keylen, b->keylen);
	for (int i = 0; i < n; i++) if (a->key[i] != b->key[i]) return a->key[i] < b->key[i] ? -1 : 1;
	return a->keylen - b->keylen;
}

static void free_entries(void) {
	for (int i = 0; i < g_entries.len; i++) { free(g_entries.items[i].name); free(g_entries.items[i].fold); free(g_entries.items[i].key); }
	g_entries.len = 0;
}

static void build(void) {
	free_entries();
	for (int i = 0; i < app_count(); i++) {
		const AppDef *d = app_get(i);
		if (d->hidden) continue;
		Entry e = {.def = d, .name = xstrdup(tr(d->name))};
		vec_push(&g_entries, e);
	}
	for (int i = 0; i < desktop_count(); i++) {
		DesktopApp *a = desktop_get(i);
		Entry e = {.da = a, .name = xstrdup(a->name)};
		vec_push(&g_entries, e);
	}
	for (int i = 0; i < g_entries.len; i++) {
		Entry *e = &g_entries.items[i];
		e->fold = str_fold(e->name);
		make_key(e);
		uint32_t cp = 0;
		utf8_decode(e->name, &cp);
		const char *up = i18n_upper(e->name);
		uint32_t ucp;
		int n = utf8_decode(up, &ucp);
		if (g_unichar_isalpha(cp)) { memcpy(e->letter, up, (size_t)n); e->letter[n] = 0; }
		else str_copy(e->letter, sizeof e->letter, "#");
	}
	qsort(g_entries.items, (size_t)g_entries.len, sizeof(Entry), entry_cmp);
	g_built_lang = i18n_lang();
	g_built_count = desktop_count();
	g_dirty = false;
}

/* --------------------------------------------------------------- helpers -- */

static const char *entry_key(const Entry *e, char *buf, size_t n) {
	if (e->def) snprintf(buf, n, "b:%s", e->def->id);
	else snprintf(buf, n, "d:%s", e->da->id);
	return buf;
}

static void entry_launch(const Entry *e) {
	char k[160];
	shell_launch(entry_key(e, k, sizeof k));
}

static void draw_icon_tile(UI *ui, const Entry *e, Rect r) {
	cairo_t *cr = ui->cr;
	Color bg = ui->th.accent;
	if (e->def && e->def->color) bg = RGBX(e->def->color);
	gfx_fill_rect(cr, r, bg);
	Color fg = COL_WHITE;
	if (e->def) {
		icon_draw(cr, e->def->icon, rect_cx(r), rect_cy(r), r.w * 0.58, fg);
	} else {
		Image *img = desktop_icon(e->da, 96);
		Rect ir = rect_inset(r, r.w * 0.2);
		if (img && cfg.mono_icons) gfx_image_silhouette(cr, img, ir, fg);
		else if (img) { gfx_fill_rect(cr, r, ui->th.chrome2); gfx_image_contain(cr, img, rect_inset(r, 4), 1.0); }
		else gfx_text_in(cr, e->letter, r, r.w * 0.55, FW_SEMILIGHT, fg, TA_CENTER, true);
	}
}

static void menu_cb(int idx, void *ud) {
	Entry *e = g_menu_entry;
	if (!e) return;
	char k[160];
	entry_key(e, k, sizeof k);
	bool pinned = start_is_pinned(k);
	if (idx == 0) { if (pinned) start_unpin(k); else start_pin(k); }
	else if (idx == 1 && e->da && e->da->is_flatpak) {
		char *cmd = xasprintf("flatpak uninstall -y --user %s || flatpak uninstall -y %s", e->da->id, e->da->id);
		proc_spawn_sh(cmd);
		free(cmd);
		shell_toast(_("Uninstalling"), e->name, NULL);
	}
}

static const char *g_menu_items[3];

static void open_menu(Entry *e, double x, double y) {
	char k[160];
	entry_key(e, k, sizeof k);
	g_menu_items[0] = start_is_pinned(k) ? _("unpin from start") : _("pin to start");
	int n = 1;
	if (e->da && e->da->is_flatpak) g_menu_items[n++] = _("uninstall");
	g_menu_entry = e;
	dialog_menu(x, y, g_menu_items, n, menu_cb, NULL);
}

/* ---------------------------------------------------------------- drawing - */

void applist_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (g_dirty || g_built_lang != (int)i18n_lang() || g_built_count != desktop_count()) build();

	bool searching = g_search[0] != 0;
	const double ROW = 62, HDR = 56, SEARCH_H = 64;
	/* collect visible rows */
	int nrows = 0;
	static int *idx;
	static int idx_cap;
	if (idx_cap < g_entries.len + 1) { idx_cap = g_entries.len + 16; idx = xrealloc(idx, (size_t)idx_cap * sizeof(int)); }
	for (int i = 0; i < g_entries.len; i++) {
		Entry *e = &g_entries.items[i];
		if (searching) {
			bool m = strstr(e->fold, g_search) != NULL;
			if (!m) { char *f = str_fold(g_search); m = strstr(e->fold, f) != NULL; free(f); }
			if (!m && e->def && e->def->keywords) m = str_contains_fold(e->def->keywords, g_search);
			if (!m) continue;
		}
		idx[nrows++] = i;
	}
	/* total height */
	double total = SEARCH_H;
	for (int k = 0; k < nrows; k++) {
		bool newletter = !searching && (k == 0 || strcmp(g_entries.items[idx[k]].letter, g_entries.items[idx[k - 1]].letter) != 0);
		total += (newletter ? HDR - 8 : 0) + ROW;
	}
	total += 30;

	UiId sid = ui_id("applist-scroll");
	double off = ui_scroll_begin(ui, sid, &g_scroll, area, total);
	cairo_translate(cr, 0, area.y);
	double y = 0;

	/* search box */
	{
		Rect sb = R(20, y + 10, area.w - 40, 44);
		bool changed = false;
		char tmp[96];
		str_copy(tmp, sizeof tmp, g_search);
		ui_textbox(ui, ui_id("applist-search"), sb, tmp, sizeof tmp, _("search apps"), 0, &changed);
		if (changed) { str_copy(g_search, sizeof g_search, tmp); g_scroll.off = 0; }
		y += SEARCH_H;
	}

	/* rows */
	const char *last_letter = "";
	for (int k = 0; k < nrows; k++) {
		Entry *e = &g_entries.items[idx[k]];
		bool newletter = !searching && strcmp(e->letter, last_letter) != 0;
		if (newletter) {
			double hy = y;
			Rect hr = R(20, hy + 4, 44, 44);
			bool visible = hy + HDR > off && hy < off + area.h;
			if (visible) {
				UiId hid = ui_idn("applist-head", k);
				ui_hit(ui, hid, hr);
				bool pr = ui_pressed(ui, hid);
				ui_letter_tile(ui, hr, e->letter, true, pr);
				if (ui_clicked(ui, hid)) { g_jump_open = true; anim_start(&g_jump_anim, 0, 1, ui->now, 0.2, EASE_OUT_CUBIC); }
			}
			y += HDR - 8;
			last_letter = e->letter;
		}
		Rect rr = R(0, y, area.w, ROW);
		if (y + ROW > off && y < off + area.h) {
			UiId id = ui_idn("applist-row", idx[k]);
			ui_hit(ui, id, rr);
			bool pr = ui_pressed(ui, id);
			if (pr) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
			Rect ic = R(20, y + 7, 48, 48);
			draw_icon_tile(ui, e, ic);
			gfx_text(cr, e->name, 84, y + (ROW - gfx_line_height(cr, 26, FW_SEMILIGHT)) / 2, 26, FW_SEMILIGHT, th->fg, TA_LEFT,
			         area.w - 100, 1);
			if (ui_clicked(ui, id)) entry_launch(e);
			if (ui_longpressed(ui, id)) {
				Rect sr = ui_to_screen(ui, rr);
				open_menu(e, ui->p.x, sr.y + ROW / 2);
			}
		}
		y += ROW;
	}
	if (!nrows) {
		gfx_text(cr, searching ? _("no results") : _("no apps"), area.w / 2, y + 40, 22, FW_SEMILIGHT, th->sub, TA_CENTER, area.w - 40, 1);
	}
	ui_scroll_end(ui);

	/* jump list overlay */
	if (g_jump_open) {
		double a = ui_anim(ui, &g_jump_anim);
		Rect full = R(0, area.y, area.w, area.h);
		gfx_fill_rect(cr, full, col_alpha(th->bg, (float)(0.96 * a)));
		UiId bg = ui_id("jump-bg");
		ui_hit(ui, bg, full);
		if (ui_clicked(ui, bg)) g_jump_open = false;
		/* letters */
		static char letters[64][8];
		int nl = 0;
		const char *az = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		letters[nl][0] = '#'; letters[nl++][1] = 0;
		for (const char *p = az; *p && nl < 60; p++) {
			if (i18n_lang() == LANG_TR && (*p == 'C' || *p == 'G' || *p == 'O' || *p == 'S' || *p == 'U' || *p == 'I')) {
				letters[nl][0] = *p; letters[nl++][1] = 0;
				const char *extra = *p == 'C' ? "Ç" : *p == 'G' ? "Ğ" : *p == 'O' ? "Ö" : *p == 'S' ? "Ş" : *p == 'U' ? "Ü" : "İ";
				if (*p == 'I') { /* Turkish order: I, İ */ }
				str_copy(letters[nl++], 8, extra);
			} else { letters[nl][0] = *p; letters[nl++][1] = 0; }
		}
		int cols = 4;
		double cell = MIN(72, (area.w - 60) / cols);
		double gx = (area.w - cols * cell - (cols - 1) * 8) / 2;
		double gy = area.y + 24;
		for (int i = 0; i < nl; i++) {
			int r = i / cols, c = i % cols;
			Rect lr = R(gx + c * (cell + 8), gy + r * (cell + 8) + (1 - a) * 30, cell, cell);
			bool present = false;
			int target = -1;
			double ty = SEARCH_H;
			const char *ll = "";
			for (int k = 0; k < nrows; k++) {
				Entry *e = &g_entries.items[idx[k]];
				bool nlw = strcmp(e->letter, ll) != 0;
				if (nlw) { if (strcmp(e->letter, letters[i]) == 0 && !present) { present = true; target = k; } ll = e->letter; ty += HDR - 8; }
				ty += ROW;
			}
			(void)target;
			UiId lid = ui_idn("jump-letter", i);
			if (present) ui_hit(ui, lid, lr);
			ui_letter_tile(ui, lr, letters[i], present, present && ui_pressed(ui, lid));
			if (present && ui_clicked(ui, lid)) {
				/* compute the y offset of the first row with this letter */
				double yy = SEARCH_H;
				const char *l2 = "";
				for (int k = 0; k < nrows; k++) {
					Entry *e = &g_entries.items[idx[k]];
					if (strcmp(e->letter, l2) != 0) {
						if (strcmp(e->letter, letters[i]) == 0) break;
						l2 = e->letter; yy += HDR - 8;
					}
					yy += ROW;
				}
				ui_scroll_to(&g_scroll, yy);
				g_jump_open = false;
			}
		}
	}
}

#include "shell.h"
#include "../apps/apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../util.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <sys/stat.h>

/* Search: apps, settings pages, files and the web. */

typedef struct { char *name; char *path; bool is_dir; } FileHit;
static VEC(FileHit) g_files;
static bool g_files_scanned;
static char g_q[128];
static Scroll g_scroll;
static bool g_focus_pending;

static void free_files(void) {
	for (int i = 0; i < g_files.len; i++) { free(g_files.items[i].name); free(g_files.items[i].path); }
	g_files.len = 0;
	g_files_scanned = false;
}

static void scan_dir(const char *dir, int depth) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && g_files.len < 4000) {
		if (e->d_name[0] == '.') continue;
		char *p = path_join(dir, e->d_name);
		bool isd = is_dir(p);
		FileHit h = {.name = xstrdup(e->d_name), .path = p, .is_dir = isd};
		vec_push(&g_files, h);
		if (isd && depth > 0) scan_dir(p, depth - 1);
	}
	closedir(d);
}

static void scan_files(void) {
	free_files();
	g_files_scanned = true;
	static const char *dirs[] = {"Documents", "Downloads", "Pictures", "Music", "Videos", "Desktop"};
	for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
		char *p = path_join(home_dir(), dirs[i]);
		scan_dir(p, 2);
		free(p);
	}
}

void search_open(void) {
	g_q[0] = 0;
	g_scroll = (Scroll){0};
	g_focus_pending = true;
	g_files_scanned = false;
}

bool search_back(void) {
	if (g_q[0]) { g_q[0] = 0; sh.ui.focus = 0; return true; }
	return false;
}

static void open_web(const char *q) {
	char *enc = xmalloc(strlen(q) * 3 + 1), *w = enc;
	for (const unsigned char *p = (const unsigned char *)q; *p; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') *w++ = (char)*p;
		else if (*p == ' ') *w++ = '+';
		else w += sprintf(w, "%%%02X", *p);
	}
	*w = 0;
	char *url = xasprintf("https://duckduckgo.com/?q=%s", enc);
	open_uri(url);
	free(url);
	free(enc);
}

typedef enum { R_APP, R_SETTING, R_FILE, R_WEB, R_URL } RKind;
typedef struct {
	RKind kind;
	const AppDef *def;
	DesktopApp *da;
	const char *link;
	char *title;
	char *sub;
	char *path;
} Result;

void search_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = area.y;
	y = ui_page_header(ui, 14, y + 6, _("search"), _("search"));
	Rect tb = R(16, y + 6, area.w - 32, 48);
	if (g_focus_pending) { ui_focus(ui, ui_id("search-box")); g_focus_pending = false; }
	bool changed = false;
	bool enter = ui_textbox(ui, ui_id("search-box"), tb, g_q, sizeof g_q, _("search"), 0, &changed);
	if (changed) g_scroll.off = 0;
	y = tb.y + tb.h + 10;

	/* gather results */
	static Result res[40];
	static int nres;
	for (int i = 0; i < nres; i++) { free(res[i].title); free(res[i].sub); free(res[i].path); }
	nres = 0;
	if (g_q[0]) {
		char *f = str_fold(g_q);
		for (int i = 0; i < app_count() && nres < 38; i++) {
			const AppDef *d = app_get(i);
			if (d->hidden) continue;
			if (str_contains_fold(tr(d->name), g_q) || (d->keywords && str_contains_fold(d->keywords, g_q)))
				res[nres++] = (Result){.kind = R_APP, .def = d, .title = xstrdup(tr(d->name)), .sub = xstrdup(_("App"))};
		}
		for (int i = 0; i < desktop_count() && nres < 38; i++) {
			DesktopApp *a = desktop_get(i);
			if (strstr(a->fold_name, f) || (a->comment && str_contains_fold(a->comment, g_q)))
				res[nres++] = (Result){.kind = R_APP, .da = a, .title = xstrdup(a->name), .sub = xstrdup(_("App"))};
		}
		const SettingsPageInfo *pages;
		int np = settings_page_list(&pages);
		for (int i = 0; i < np && nres < 38; i++)
			if (str_contains_fold(tr(pages[i].title), g_q) || (pages[i].keywords && str_contains_fold(pages[i].keywords, g_q)))
				res[nres++] = (Result){.kind = R_SETTING, .link = pages[i].link, .title = xstrdup(tr(pages[i].title)), .sub = xstrdup(_("Setting"))};
		if (!g_files_scanned) scan_files();
		int nf = 0;
		for (int i = 0; i < g_files.len && nres < 38 && nf < 8; i++) {
			FileHit *h = &g_files.items[i];
			if (!h->is_dir && str_contains_fold(h->name, g_q)) {
				res[nres++] = (Result){.kind = R_FILE, .title = xstrdup(h->name), .sub = xstrdup(h->path), .path = xstrdup(h->path)};
				nf++;
			}
		}
		free(f);
		bool urlish = strchr(g_q, '.') && !strchr(g_q, ' ') && strlen(g_q) > 3;
		if (urlish) res[nres++] = (Result){.kind = R_URL, .title = xasprintf(_("Open %s"), g_q), .sub = xstrdup(_("Web")), .path = xstrdup(g_q)};
		res[nres++] = (Result){.kind = R_WEB, .title = xasprintf(_("Search the web for \"%s\""), g_q), .sub = xstrdup(_("Web"))};
	}

	Rect view = R(0, y, area.w, area.y + area.h - y);
	if (!nres) {
		gfx_text(cr, g_q[0] ? _("no results") : _("Type to search apps, settings, files and the web"), area.w / 2, y + 30, 18,
		         FW_SEMILIGHT, th->sub, TA_CENTER, area.w - 60, 3);
		return;
	}
	const double ROW = 64;
	double off = ui_scroll_begin(ui, ui_id("search-scroll"), &g_scroll, view, nres * ROW + 20);
	cairo_translate(cr, 0, view.y);
	for (int i = 0; i < nres; i++) {
		Result *r = &res[i];
		Rect rr = R(0, i * ROW, area.w, ROW);
		if (rr.y + ROW < off || rr.y > off + view.h) continue;
		UiId id = ui_idn("search-row", i);
		ui_hit(ui, id, rr);
		bool pr = ui_pressed(ui, id);
		if (pr) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
		Rect ic = R(20, rr.y + 8, 48, 48);
		gfx_fill_rect(cr, ic, r->kind == R_APP && r->def && r->def->color ? RGBX(r->def->color) : th->accent);
		if (r->kind == R_APP && r->def) icon_draw(cr, r->def->icon, rect_cx(ic), rect_cy(ic), 28, COL_WHITE);
		else if (r->kind == R_APP) {
			Image *img = desktop_icon(r->da, 96);
			if (img && cfg.mono_icons) gfx_image_silhouette(cr, img, rect_inset(ic, 9), COL_WHITE);
			else if (img) gfx_image_contain(cr, img, rect_inset(ic, 5), 1.0);
		} else if (r->kind == R_SETTING) icon_draw(cr, ICON_SETTINGS, rect_cx(ic), rect_cy(ic), 28, COL_WHITE);
		else if (r->kind == R_FILE) icon_draw(cr, ICON_FILE, rect_cx(ic), rect_cy(ic), 26, COL_WHITE);
		else icon_draw(cr, ICON_GLOBE, rect_cx(ic), rect_cy(ic), 28, COL_WHITE);
		gfx_text(cr, r->title, 84, rr.y + 8, 22, FW_SEMILIGHT, th->fg, TA_LEFT, area.w - 100, 1);
		gfx_text(cr, r->sub, 84, rr.y + 36, 14, FW_REGULAR, th->sub, TA_LEFT, area.w - 100, 1);
		bool go = ui_clicked(ui, id) || (enter && i == 0);
		if (go) {
			switch (r->kind) {
			case R_APP: {
				char key[200];
				if (r->def) snprintf(key, sizeof key, "b:%s", r->def->id); else snprintf(key, sizeof key, "d:%s", r->da->id);
				shell_launch(key);
				break;
			}
			case R_SETTING: shell_open_builtin("settings", r->link); break;
			case R_FILE: open_uri(r->path); break;
			case R_URL: {
				char *u = strstr(r->path, "://") ? xstrdup(r->path) : xasprintf("https://%s", r->path);
				open_uri(u);
				free(u);
				break;
			}
			case R_WEB: open_web(g_q); break;
			}
			ui->focus = 0;
			ui_scroll_end(ui);
			return;
		}
	}
	ui_scroll_end(ui);
}

static void app_open(const char *arg) { search_open(); }
static void app_draw(UI *ui, Rect area) { search_draw(ui, area); }
static bool app_back(void) { return search_back(); }

const AppDef app_search = {
	.id = "search", .name = N_("Search"), .icon = ICON_SEARCH, .hidden = true,
	.open = app_open, .draw = app_draw, .back = app_back,
};

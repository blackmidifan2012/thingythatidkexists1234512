/* Store: browse a curated Flathub catalog, install / open / uninstall apps with flatpak. */
#include "apps.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

typedef struct {
	char *id, *name, *dev, *cat, *summary, *desc;
	uint32_t color;
	bool featured, installed, known;   /* known = from the catalog */
} Item;

enum { P_ROOT, P_CAT, P_APP, P_SEARCH };
enum { OP_NONE, OP_INSTALL, OP_UNINSTALL, OP_UPDATE };

static const struct { const char *key, *label; } CATS[] = {
	{"games", N_("games")}, {"music+video", N_("music + video")}, {"photo", N_("photo")},
	{"productivity", N_("productivity")}, {"social", N_("social")}, {"internet", N_("internet")},
	{"tools", N_("tools")}, {"education", N_("education")}, {"development", N_("developer tools")},
};
#define NCATS ((int)ARRAY_LEN(CATS))

static VEC(Item) g_items;
static bool g_loaded, g_have_flatpak, g_listing, g_listed;
static bool g_have_remote;            /* flathub is set up: installs skip remote-add */
static VEC(int) g_queue;              /* installs waiting for the running operation */
static int g_cat, g_sel, g_op = OP_NONE, g_op_item = -1;
static char g_query[128];
static bool g_focus_search;
static Nav g_nav;
static Pager g_pivot;
static AppBar g_bar, g_dbar;
static Scroll g_scroll[3], g_cscroll, g_dscroll, g_sscroll;

static char *jstr(cJSON *o, const char *k) {
	cJSON *v = cJSON_GetObjectItem(o, k);
	return xstrdup(cJSON_IsString(v) ? v->valuestring : "");
}

static int add_item(Item it) { vec_push(&g_items, it); return g_items.len - 1; }

static void load_catalog(void) {
	char *path = data_path("store/catalog.json");
	char *txt = path ? file_read_all(path, NULL) : NULL;
	free(path);
	cJSON *root = txt ? cJSON_Parse(txt) : NULL;
	free(txt);
	cJSON *apps = cJSON_GetObjectItem(root, "apps"), *a;
	cJSON_ArrayForEach(a, apps) {
		Item it = {.id = jstr(a, "id"), .name = jstr(a, "name"), .dev = jstr(a, "developer"), .cat = jstr(a, "category"),
		           .summary = jstr(a, "summary"), .desc = jstr(a, "description"), .known = true,
		           .featured = cJSON_IsTrue(cJSON_GetObjectItem(a, "featured"))};
		cJSON *c = cJSON_GetObjectItem(a, "color");
		if (!cJSON_IsString(c) || !parse_hex_color(c->valuestring, &it.color)) it.color = 0x1BA1E2;
		add_item(it);
	}
	cJSON_Delete(root);
}

static int find_item(const char *id) {
	for (int i = 0; i < g_items.len; i++) if (str_eq(g_items.items[i].id, id)) return i;
	return -1;
}

static void list_done(int status, char *out, size_t len, void *ud) {
	(void)len; (void)ud;
	g_listing = false;
	if (!g_loaded) return;   /* closed meanwhile */
	g_listed = true;
	for (int i = 0; i < g_items.len; i++) g_items.items[i].installed = false;
	if (status == 0 && out) {
		for (char *save = NULL, *l = strtok_r(out, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			l = str_trim(l);
			if (!*l || str_eq(l, "Application ID")) continue;
			int i = find_item(l);
			if (i < 0) {
				/* installed from elsewhere: still manageable here */
				i = add_item((Item){.id = xstrdup(l), .name = xstrdup(path_basename(l)), .dev = xstrdup(""), .cat = xstrdup(""),
				                    .summary = xstrdup(l), .desc = xstrdup(""), .color = 0x647687});
				char *dot = strrchr(g_items.items[i].name, '.');
				if (dot && dot[1]) memmove(g_items.items[i].name, dot + 1, strlen(dot));
			}
			g_items.items[i].installed = true;
		}
	}
	shell_invalidate();
}

static void refresh_installed(void) {
	if (!g_have_flatpak || g_listing) return;
	char *argv[] = {"flatpak", "list", "--app", "--columns=application", NULL};
	g_listing = proc_run(argv, list_done, NULL);
}

static void remotes_done(int status, char *out, size_t len, void *ud) {
	(void)len; (void)ud;
	g_have_remote = status == 0 && out && strstr(out, "flathub");
}

static void ensure_loaded(void) {
	if (g_loaded) return;
	g_loaded = true;
	nav_init(&g_nav, P_ROOT);
	load_catalog();
	g_have_flatpak = proc_exists("flatpak");
	refresh_installed();
	if (g_have_flatpak && !g_have_remote) {
		char *argv[] = {"flatpak", "remotes", "--columns=name", NULL};
		proc_run(argv, remotes_done, NULL);
	}
}

static bool queued(int item) {
	for (int i = 0; i < g_queue.len; i++) if (g_queue.items[i] == item) return true;
	return false;
}

/* ---------------------------------------------------------------- operations */

static void op_done(int status, char *out, size_t len, void *ud);

static void op_step(int step) {
	const char *id = g_op_item >= 0 ? g_items.items[g_op_item].id : "";
	char **argv = NULL;
	char *remote[] = {"flatpak", "remote-add", "--user", "--if-not-exists", "flathub",
	                  "https://dl.flathub.org/repo/flathub.flatpakrepo", NULL};
	char *inst[] = {"flatpak", "install", "-y", "--user", "--noninteractive", "flathub", (char *)id, NULL};
	char *uninst[] = {"flatpak", "uninstall", "-y", "--noninteractive", (char *)id, NULL};
	char *upd[] = {"flatpak", "update", "-y", "--noninteractive", NULL};
	if (g_op == OP_INSTALL) argv = step == 0 ? remote : inst;
	else if (g_op == OP_UNINSTALL) argv = uninst;
	else argv = upd;
	if (!proc_run(argv, op_done, (void *)(intptr_t)step)) op_done(-1, NULL, 0, (void *)(intptr_t)99);
}

static void op_done(int status, char *out, size_t len, void *ud) {
	(void)out; (void)len;
	int step = (int)(intptr_t)ud;
	if (g_op == OP_INSTALL && step == 0) {   /* remote added (or present) */
		g_have_remote = status == 0;
		op_step(1);
		return;
	}
	const char *name = g_op_item >= 0 ? g_items.items[g_op_item].name : _("Store");
	const char *msg;
	if (g_op == OP_INSTALL) msg = status == 0 ? _("installed. you'll find it in your apps list.") :
	                              _("couldn't install this app. check your internet connection and try again.");
	else if (g_op == OP_UNINSTALL) msg = status == 0 ? _("uninstalled.") : _("couldn't uninstall this app.");
	else msg = status == 0 ? _("your apps are up to date.") : _("some apps couldn't be updated.");
	shell_post(tr("Store"), name, msg);
	g_op = OP_NONE;
	g_op_item = -1;
	desktop_scan();
	refresh_installed();
	if (g_queue.len) {
		int next = g_queue.items[0];
		vec_remove(&g_queue, 0);
		g_op = OP_INSTALL;
		g_op_item = next;
		op_step(g_have_remote ? 1 : 0);
	}
	shell_invalidate();
}

static void start_op(int op, int item) {
	if (!g_have_flatpak) return;
	/* installs can be lined up while another one downloads */
	if (g_op != OP_NONE) {
		if (op == OP_INSTALL && item != g_op_item && !queued(item)) vec_push(&g_queue, item);
		shell_invalidate();
		return;
	}
	g_op = op;
	g_op_item = item;
	/* adding the remote fetches flathub's repo file every time: only when it is missing */
	op_step(op == OP_INSTALL && g_have_remote ? 1 : 0);
	shell_invalidate();
}

static void uninstall_cb(int b, void *ud) { if (b == 0) start_op(OP_UNINSTALL, (int)(intptr_t)ud); }

static void open_app(const Item *it) {
	if (desktop_find(it->id)) {
		char *k = xasprintf("d:%s", it->id);
		shell_launch(k);
		free(k);
	} else {
		char *argv[] = {"flatpak", "run", it->id, NULL};
		proc_spawn(argv);
	}
}

/* ---------------------------------------------------------------- drawing */

static void letter_tile(UI *ui, Rect r, const Item *it, double size) {
	gfx_fill_rect(ui->cr, r, RGBX(it->color));
	/* installed apps show their own icon, like third-party tiles on the phone */
	DesktopApp *da = desktop_find(it->id);
	Image *img = da ? desktop_icon(da, 128) : NULL;
	if (img) { gfx_image_contain(ui->cr, img, rect_inset(r, r.w * 0.12), 1.0); return; }
	char l[8] = "?";
	uint32_t cp;
	int n = utf8_decode(it->name, &cp);
	if (*it->name && n < 5) { memcpy(l, it->name, (size_t)n); l[n] = 0; }
	gfx_text_in(ui->cr, i18n_upper(l), r, size, FW_SEMILIGHT, COL_WHITE, TA_CENTER, true);
}

static const char *status_text(int i) {
	if (g_op_item == i) return g_op == OP_INSTALL ? _("installing...") : _("uninstalling...");
	if (queued(i)) return _("waiting to install...");
	return g_items.items[i].installed ? _("installed") : NULL;
}

#define ROW_H 84.0

static void app_row(UI *ui, int i, Rect r) {
	cairo_t *cr = ui->cr;
	const Item *it = &g_items.items[i];
	UiId id = ui_idn("st-row", i);
	ui_hit(ui, id, r);
	if (ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(ui->th.fg, 0.14f));
	letter_tile(ui, R(r.x + 24, r.y + 10, 64, 64), it, 32);
	double x = r.x + 104, w = r.w - 104 - 18;
	gfx_text(cr, it->name, x, r.y + 8, 24, FW_SEMILIGHT, ui->th.fg, TA_LEFT, w, 1);
	gfx_text(cr, it->summary, x, r.y + 40, 15, FW_REGULAR, ui->th.sub, TA_LEFT, w, 1);
	const char *st = status_text(i);
	if (st) gfx_text(cr, st, x, r.y + 59, 15, FW_REGULAR, ui->th.accent, TA_LEFT, w, 1);
	if (ui_clicked(ui, id)) {
		g_sel = i;
		g_dscroll = (Scroll){0};
		nav_push(&g_nav, ui, P_APP);
	}
}

/* Scrolling list of item indices with an optional explanatory text on top. */
static void app_list(UI *ui, const char *key, Scroll *s, Rect view, const int *idx, int n, const char *note,
                     const char *empty) {
	cairo_t *cr = ui->cr;
	double top = 0;
	if (note) top = gfx_text_size(cr, note, 17, FW_REGULAR, view.w - 48, 0).h + 20;
	double off = ui_scroll_begin(ui, ui_id(key), s, view, top + n * ROW_H + 20);
	cairo_translate(cr, view.x, view.y);
	if (note) gfx_text(cr, note, 24, 0, 17, FW_REGULAR, ui->th.accent, TA_LEFT, view.w - 48, 0);
	for (int k = 0; k < n; k++) {
		double y = top + k * ROW_H;
		if (y + ROW_H < off || y > off + view.h) continue;
		app_row(ui, idx[k], R(0, y, view.w, ROW_H));
	}
	if (!n && empty) app_empty(ui, R(0, top - 14, view.w, 100), empty);
	ui_scroll_end(ui);
}

static int cmp_name(const void *a, const void *b) {
	return app_collate(g_items.items[*(const int *)a].name, g_items.items[*(const int *)b].name);
}

static int *collect(int *n, int mode) {   /* 0 featured, 1 category g_cat, 2 installed, 3 search */
	int *idx = xmalloc(sizeof(int) * (size_t)(g_items.len + 1));
	*n = 0;
	for (int i = 0; i < g_items.len; i++) {
		const Item *it = &g_items.items[i];
		bool ok = mode == 0 ? it->featured : mode == 1 ? str_eq(it->cat, CATS[g_cat].key) : mode == 2 ? it->installed :
		          (g_query[0] && (str_contains_fold(it->name, g_query) || str_contains_fold(it->summary, g_query) ||
		                          str_contains_fold(it->dev, g_query) || str_contains_fold(it->id, g_query)));
		if (ok) idx[(*n)++] = i;
	}
	if (mode) qsort(idx, (size_t)*n, sizeof(int), cmp_name);
	return idx;
}

static void pivot_page(UI *ui, int page, Rect r, void *ud) {
	(void)ud;
	const char *nofp = g_have_flatpak ? NULL :
	                   _("flatpak isn't installed, so apps can't be installed from here. install flatpak with your "
	                     "distribution's package manager, then come back.");
	if (page == 1) {
		cairo_t *cr = ui->cr;
		double off = ui_scroll_begin(ui, ui_id("st-cats"), &g_scroll[1], r, NCATS * 72 + 20);
		(void)off;
		cairo_translate(cr, r.x, r.y);
		for (int c = 0; c < NCATS; c++) {
			int n = 0;
			for (int i = 0; i < g_items.len; i++) n += str_eq(g_items.items[i].cat, CATS[c].key);
			char sub[48];
			snprintf(sub, sizeof sub, _("%d apps"), n);
			if (ui_row_ex(ui, ui_idn("st-cat", c), R(0, c * 72, r.w, 72), tr(CATS[c].label), sub, ICON_NONE, NULL, false)) {
				g_cat = c;
				g_cscroll = (Scroll){0};
				nav_push(&g_nav, ui, P_CAT);
			}
		}
		ui_scroll_end(ui);
		return;
	}
	if (page == 2 && g_have_flatpak && !g_listed) {
		ui_progress_dots(ui, R(r.x + 24, r.y + 20, r.w - 48, 8), ui->th.accent);
		return;
	}
	int n;
	int *idx = collect(&n, page == 0 ? 0 : 2);
	if (page == 0) app_list(ui, "st-feat", &g_scroll[0], r, idx, n, nofp, NULL);
	else app_list(ui, "st-inst", &g_scroll[2], r, idx, n, NULL,
	              g_have_flatpak ? _("no apps from Flathub are installed yet.") : nofp);
	free(idx);
}

static void open_search(UI *ui) {
	g_query[0] = 0;
	g_sscroll = (Scroll){0};
	g_focus_search = true;
	nav_push(&g_nav, ui, P_SEARCH);
}

static void root_page(UI *ui, Rect area) {
	Rect c = appbar_content(&g_bar, area);
	const char *titles[3] = {_("featured"), _("categories"), _("installed")};
	app_pivot(ui, "st-pivot", &g_pivot, c, _("Store"), titles, 3, pivot_page, NULL);
	AppBarBtn b[2] = {{ICON_SEARCH, _("search")}, {ICON_REFRESH, _("refresh"), !g_have_flatpak}};
	const char *menu[2] = {_("update all apps"), _("open flathub.org")};
	int a = appbar_draw(ui, &g_bar, area, b, 2, menu, 2);
	if (a == 0) open_search(ui);
	else if (a == 1) refresh_installed();
	else if (a == APPBAR_MENU) {
		if (g_have_flatpak) start_op(OP_UPDATE, -1);
	} else if (a == APPBAR_MENU + 1) open_uri("https://flathub.org");
}

static void category_page(UI *ui, Rect area) {
	double y = ui_page_header(ui, 14, area.y + 6, _("Store"), tr(CATS[g_cat].label));
	int n;
	int *idx = collect(&n, 1);
	app_list(ui, "st-catl", &g_cscroll, R(area.x, y + 6, area.w, rect_bottom(area) - y - 6), idx, n, NULL, NULL);
	free(idx);
}

static void search_page(UI *ui, Rect area) {
	double y = ui_page_header(ui, 14, area.y + 6, _("Store"), _("search"));
	UiId tid = ui_id("st-q");
	if (g_focus_search) { ui_focus(ui, tid); g_focus_search = false; }
	bool ch = false;
	ui_textbox(ui, tid, R(24, y + 4, area.w - 48, 46), g_query, sizeof g_query, _("search apps"), 0, &ch);
	if (ch) ui_scroll_to(&g_sscroll, 0);
	y += 62;
	int n;
	int *idx = collect(&n, 3);
	app_list(ui, "st-res", &g_sscroll, R(area.x, y, area.w, rect_bottom(area) - y), idx, n, NULL,
	         g_query[0] ? _("no matching apps.") : NULL);
	free(idx);
}

static void detail_page(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (g_sel < 0 || g_sel >= g_items.len) return;
	Item *it = &g_items.items[g_sel];
	Rect a = appbar_content(&g_dbar, area);
	gfx_text(cr, i18n_upper(_("Store")), 18, a.y + 6, 17, FW_SEMIBOLD, th->fg, TA_LEFT, a.w - 36, 1);
	Rect view = R(a.x, a.y + 34, a.w, a.h - 34);
	static double content_h = 600;
	ui_scroll_begin(ui, ui_id("st-det"), &g_dscroll, view, content_h);
	cairo_translate(cr, view.x, view.y);
	double x = 24, w = view.w - 48, y = 8;
	letter_tile(ui, R(x, y, 120, 120), it, 64);
	double tx = x + 136, tw = w - 136;
	double h = gfx_text(cr, it->name, tx, y - 8, 36, FW_LIGHT, th->fg, TA_LEFT, tw, 2);
	if (*it->dev) h += gfx_text(cr, it->dev, tx, y - 8 + h, 17, FW_REGULAR, th->sub, TA_LEFT, tw, 2);
	const char *cat = NULL;
	for (int c = 0; c < NCATS; c++) if (str_eq(it->cat, CATS[c].key)) cat = tr(CATS[c].label);
	if (cat) gfx_text(cr, cat, tx, y - 6 + h, 15, FW_REGULAR, th->accent, TA_LEFT, tw, 1);
	y += 140;

	bool busy = g_op != OP_NONE;
	double bw = MIN(200, (w - 12) / 2);
	if (g_op_item == g_sel || queued(g_sel)) {
		gfx_text(cr, status_text(g_sel), x, y, 20, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		if (g_op_item == g_sel) ui_progress_bar(ui, R(x, y + 34, w, 10), -1);
		y += 60;
	} else if (it->installed) {
		if (ui_button_enabled(ui, ui_id("st-open"), R(x, y, bw, 52), _("open"), BTN_OUTLINE, true)) open_app(it);
		if (ui_button_enabled(ui, ui_id("st-rm"), R(x + bw + 12, y, bw, 52), _("uninstall"), BTN_OUTLINE, !busy)) {
			char *body = xasprintf(_("%s will be removed from this device."), it->name);
			dialog_message(_("uninstall?"), body, _("uninstall"), _("cancel"), uninstall_cb, (void *)(intptr_t)g_sel);
			free(body);
		}
		y += 72;
	} else {
		if (ui_button_enabled(ui, ui_id("st-inst"), R(x, y, bw, 52), _("install"), BTN_OUTLINE, g_have_flatpak))
			start_op(OP_INSTALL, g_sel);
		y += 72;
	}
	if (!g_have_flatpak)
		y += gfx_text(cr, _("install flatpak to get this app."), x, y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 0) + 12;
	y += gfx_text(cr, it->summary, x, y, 22, FW_SEMILIGHT, th->fg, TA_LEFT, w, 0) + 10;
	if (*it->desc) y += gfx_text(cr, it->desc, x, y, 17, FW_REGULAR, th->fg, TA_LEFT, w, 0) + 16;
	y += gfx_text(cr, _("app id"), x, y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	y += gfx_text(cr, it->id, x, y, 17, FW_REGULAR, th->fg, TA_LEFT, w, 2) + 20;
	content_h = y;
	ui_scroll_end(ui);
	AppBarBtn b[1] = {{ICON_GLOBE, _("flathub page")}};
	if (appbar_draw(ui, &g_dbar, area, b, 1, NULL, 0) == 0) {
		char *u = xasprintf("https://flathub.org/apps/%s", it->id);
		open_uri(u);
		free(u);
	}
}

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	(void)ud;
	if (page == P_CAT) category_page(ui, area);
	else if (page == P_APP) detail_page(ui, area);
	else if (page == P_SEARCH) search_page(ui, area);
	else root_page(ui, area);
}

static void store_draw(UI *ui, Rect area) {
	ensure_loaded();
	nav_draw(ui, &g_nav, area, page_fn, NULL);
}

static void store_open(const char *arg) {
	ensure_loaded();
	if (arg && *arg) {
		int i = find_item(arg);
		if (i >= 0) { nav_init(&g_nav, P_ROOT); g_sel = i; nav_push(&g_nav, &sh.ui, P_APP); }
	}
	if (g_listed) refresh_installed();
}

static bool store_back(void) {
	int p = nav_page(&g_nav);
	if (p == P_ROOT && appbar_back(&g_bar)) return true;
	if (p == P_APP && appbar_back(&g_dbar)) return true;
	return nav_pop(&g_nav, &sh.ui);
}

static void store_close(void) {
	if (g_op != OP_NONE || g_queue.len) return;   /* a running install still needs the items */
	for (int i = 0; i < g_items.len; i++) {
		Item *it = &g_items.items[i];
		free(it->id); free(it->name); free(it->dev); free(it->cat); free(it->summary); free(it->desc);
	}
	vec_free(&g_items);
	g_loaded = g_listed = false;
	g_pivot = (Pager){0};
}

const AppDef app_store = {
	.id = "store", .name = N_("Store"), .icon = ICON_STORE, .color = 0,
	.keywords = "flatpak flathub install apps games marketplace",
	.open = store_open, .draw = store_draw, .back = store_back, .close = store_close,
};

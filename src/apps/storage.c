/* Storage Sense: free space on every storage and what is using it, with quick clean-up of
 * temporary files and the trash. */
#include "apps.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

enum { P_ROOT, P_VOL };
enum { K_PIC, K_MUS, K_VID, K_DOC, K_DL, K_TMP, K_TRASH, K_OTHER, K_FREE, NK };
static const char *const KNAMES[NK] = {
	N_("pictures"), N_("music"), N_("videos"), N_("documents"), N_("downloads"),
	N_("temporary files"), N_("trash"), N_("other"), N_("free space"),
};
static const uint32_t KCOLS[NK] = {0xE51400, 0xF09609, 0x8CBF26, 0x1BA1E2, 0xA05000, 0x00ABA9, 0xD80073, 0x7A7A7A, 0};

typedef struct { char name[64], path[512]; double total, free; bool home; } Vol;

#define MAX_VOLS 12
#define WALK_CAP 400000

typedef struct {
	int gen;
	char *dirs[K_OTHER];
	double size[K_OTHER];
	bool capped;
} Scan;

static Vol g_vols[MAX_VOLS];
static int g_nvols, g_vol, g_gen;
static bool g_scanning, g_scanned, g_capped, g_cleaning;
static double g_size[K_OTHER];
static char *g_dirs[K_OTHER];
static Nav g_nav;
static AppBar g_bar;
static Scroll g_rscroll, g_vscroll;

/* ---------------------------------------------------------------- volumes */

static bool vol_stat(Vol *v, const char *path) {
	struct statvfs s;
	if (statvfs(path, &s) != 0 || !s.f_blocks) return false;
	v->total = (double)s.f_blocks * s.f_frsize;
	v->free = (double)s.f_bavail * s.f_frsize;
	str_copy(v->path, sizeof v->path, path);
	return true;
}

static void add_mounts(const char *base, dev_t parent) {
	DIR *d = opendir(base);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && g_nvols < MAX_VOLS) {
		if (e->d_name[0] == '.') continue;
		char *p = path_join(base, e->d_name);
		struct stat st;
		Vol *v = &g_vols[g_nvols];
		memset(v, 0, sizeof *v);
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev != parent && vol_stat(v, p)) {
			str_copy(v->name, sizeof v->name, e->d_name);
			g_nvols++;
		}
		free(p);
	}
	closedir(d);
}

static void find_volumes(void) {
	g_nvols = 0;
	struct stat hs, rs;
	const char *home = home_dir();
	Vol *v = &g_vols[0];
	memset(v, 0, sizeof *v);
	if (vol_stat(v, home)) {
		str_copy(v->name, sizeof v->name, _("this device"));
		v->home = true;
		g_nvols++;
	}
	if (stat(home, &hs) == 0 && stat("/", &rs) == 0 && hs.st_dev != rs.st_dev) {
		v = &g_vols[g_nvols];
		memset(v, 0, sizeof *v);
		if (vol_stat(v, "/")) { str_copy(v->name, sizeof v->name, _("system")); g_nvols++; }
	}
	const char *user = g_get_user_name();
	char base[300];
	snprintf(base, sizeof base, "/media/%s", user);
	if (stat("/media", &rs) == 0) add_mounts(base, rs.st_dev);
	snprintf(base, sizeof base, "/run/media/%s", user);
	if (stat("/run/media", &rs) == 0) add_mounts(base, rs.st_dev);
}

/* ---------------------------------------------------------------- scanning */

static double walk(int dfd, dev_t dev, int depth, long *budget, bool *capped) {
	DIR *d = fdopendir(dfd);
	if (!d) { close(dfd); return 0; }
	double sum = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (--*budget < 0) { *capped = true; break; }
		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
		sum += (double)st.st_blocks * 512;
		if (S_ISDIR(st.st_mode) && st.st_dev == dev && depth < 48) {
			int fd = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (fd >= 0) sum += walk(fd, dev, depth + 1, budget, capped);
		}
	}
	closedir(d);
	return sum;
}

static void scan_run(void *arg) {
	Scan *s = arg;
	long budget = WALK_CAP;
	for (int k = 0; k < K_OTHER; k++) {
		struct stat st;
		if (!s->dirs[k] || lstat(s->dirs[k], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		int fd = open(s->dirs[k], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd >= 0) s->size[k] = walk(fd, st.st_dev, 0, &budget, &s->capped);
	}
}

static void scan_done(void *arg) {
	Scan *s = arg;
	if (s->gen == g_gen) {
		memcpy(g_size, s->size, sizeof g_size);
		g_capped = s->capped;
		g_scanning = false;
		g_scanned = true;
		shell_invalidate();
	}
	for (int k = 0; k < K_OTHER; k++) free(s->dirs[k]);
	free(s);
}

static char *user_dir(GUserDirectory d, const char *fallback) {
	const char *p = g_get_user_special_dir(d);
	if (p && !str_eq(p, home_dir())) return xstrdup(p);
	return path_join(home_dir(), fallback);
}

static void start_scan(void) {
	if (!g_dirs[K_PIC]) {
		g_dirs[K_PIC] = user_dir(G_USER_DIRECTORY_PICTURES, "Pictures");
		g_dirs[K_MUS] = user_dir(G_USER_DIRECTORY_MUSIC, "Music");
		g_dirs[K_VID] = user_dir(G_USER_DIRECTORY_VIDEOS, "Videos");
		g_dirs[K_DOC] = user_dir(G_USER_DIRECTORY_DOCUMENTS, "Documents");
		g_dirs[K_DL] = user_dir(G_USER_DIRECTORY_DOWNLOAD, "Downloads");
		g_dirs[K_TMP] = xstrdup(g_get_user_cache_dir());
		g_dirs[K_TRASH] = path_join(g_get_user_data_dir(), "Trash");
	}
	find_volumes();
	Scan *s = xcalloc(1, sizeof *s);
	s->gen = ++g_gen;
	for (int k = 0; k < K_OTHER; k++) s->dirs[k] = xstrdup(g_dirs[k]);
	g_scanning = true;
	job_submit(scan_run, scan_done, s);
	shell_invalidate();
}

/* ---------------------------------------------------------------- clean up */

static void clean_done(int status, char *out, size_t len, void *ud) {
	(void)out; (void)len;
	g_cleaning = false;
	shell_toast(tr("Storage Sense"), status == 0 ? (const char *)ud : _("some files couldn't be deleted."),
	            tr("Storage Sense"));
	start_scan();
}

/* rm -rf the contents of the given folders (not the folders themselves). */
static void clear_dirs(const char *const *dirs, int n, const char *done_msg) {
	VEC(char *) argv = {0};
	vec_push(&argv, xstrdup("rm"));
	vec_push(&argv, xstrdup("-rf"));
	vec_push(&argv, xstrdup("--"));
	for (int i = 0; i < n; i++) {
		DIR *d = opendir(dirs[i]);
		struct dirent *e;
		while (d && (e = readdir(d)) && argv.len < 4000)
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) vec_push(&argv, path_join(dirs[i], e->d_name));
		if (d) closedir(d);
	}
	vec_push(&argv, NULL);
	if (argv.len > 4) g_cleaning = proc_run(argv.items, clean_done, (void *)done_msg);
	else clean_done(0, NULL, 0, (void *)done_msg);
	for (int i = 0; i < argv.len; i++) free(argv.items[i]);
	vec_free(&argv);
	shell_invalidate();
}

static void clean_cb(int b, void *ud) {
	if (b != 0 || g_cleaning) return;
	if (ud) {
		char *t[3] = {path_join(g_dirs[K_TRASH], "files"), path_join(g_dirs[K_TRASH], "info"),
		              path_join(g_dirs[K_TRASH], "expunged")};
		clear_dirs((const char *const *)t, 3, _("the trash is empty."));
		for (int i = 0; i < 3; i++) free(t[i]);
	} else {
		const char *t[1] = {g_dirs[K_TMP]};
		clear_dirs(t, 1, _("temporary files deleted."));
	}
}

static void ask_clean(bool trash) {
	if (trash) dialog_message(_("empty trash?"), _("everything in the trash will be deleted permanently."), _("empty"),
	                          _("cancel"), clean_cb, (void *)1);
	else dialog_message(_("delete temporary files?"),
	                    _("apps keep temporary files to work faster. they'll be created again when needed."),
	                    _("delete"), _("cancel"), clean_cb, NULL);
}

/* ---------------------------------------------------------------- drawing */

static void fmt_free(const Vol *v, char *buf, size_t cap) {
	char f[32], t[32];
	snprintf(buf, cap, _("%s free of %s"), human_size(v->free, f, sizeof f), human_size(v->total, t, sizeof t));
}

static void root_page(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = ui_page_header(ui, 14, area.y + 6, _("Storage Sense"), _("storage"));
	Rect view = R(area.x, y + 4, area.w, rect_bottom(area) - y - 4);
	double rh = 108;
	ui_scroll_begin(ui, ui_id("ss-root"), &g_rscroll, view, g_nvols * rh + 120);
	cairo_translate(cr, view.x, view.y);
	char buf[96];
	for (int i = 0; i < g_nvols; i++) {
		const Vol *v = &g_vols[i];
		Rect r = R(0, i * rh, view.w, rh);
		UiId id = ui_idn("ss-vol", i);
		ui_hit(ui, id, r);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.14f));
		double w = view.w - 48;
		gfx_text(cr, v->name, 24, r.y + 8, 30, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		gfx_fill_rect(cr, R(24, r.y + 54, w, 14), th->chrome2);
		gfx_fill_rect(cr, R(24, r.y + 54, w * CLAMP(1 - v->free / v->total, 0, 1), 14), th->accent);
		fmt_free(v, buf, sizeof buf);
		gfx_text(cr, buf, 24, r.y + 74, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		if (ui_clicked(ui, id)) {
			g_vol = i;
			g_vscroll = (Scroll){0};
			if (v->home && !g_scanning) start_scan();
			nav_push(&g_nav, ui, P_VOL);
		}
	}
	gfx_text(cr, _("tap a storage to see what's using space on it."), 24, g_nvols * rh + 12, 17, FW_REGULAR, th->sub,
	         TA_LEFT, view.w - 48, 0);
	ui_scroll_end(ui);
}

static void vol_page(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (g_vol >= g_nvols) return;
	const Vol *v = &g_vols[g_vol];
	bool home = v->home;
	Rect a = home ? appbar_content(&g_bar, area) : area;
	double y = ui_page_header(ui, 14, a.y + 6, _("Storage Sense"), v->name);
	Rect view = R(a.x, y + 4, a.w, rect_bottom(a) - y - 4);
	static double content_h = 600;
	ui_scroll_begin(ui, ui_id("ss-vol"), &g_vscroll, view, content_h);
	cairo_translate(cr, view.x, view.y);
	double x = 24, w = view.w - 48;
	char buf[96], sz[32];
	double used = MAX(0, v->total - v->free), val[NK] = {0};
	bool ready = home ? g_scanned : true;
	if (home) {
		double sum = 0;
		for (int k = 0; k < K_OTHER; k++) { val[k] = g_size[k]; sum += g_size[k]; }
		val[K_OTHER] = MAX(0, used - sum);
	} else {
		val[K_OTHER] = used;
	}
	val[K_FREE] = v->free;
	/* stacked usage bar */
	y = 0;
	gfx_fill_rect(cr, R(x, y, w, 24), th->chrome2);
	if (ready) {
		double bx = x;
		for (int k = 0; k < K_FREE; k++) {
			double sw = w * val[k] / v->total;
			gfx_fill_rect(cr, R(bx, y, MIN(sw, x + w - bx), 24), k == K_OTHER && !home ? th->accent : RGBX(KCOLS[k]));
			bx += sw;
		}
	}
	fmt_free(v, buf, sizeof buf);
	gfx_text(cr, buf, x, y + 30, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	y += 64;
	if (home && (g_scanning || g_cleaning)) {
		gfx_text(cr, g_cleaning ? _("deleting...") : _("calculating..."), x, y, 17, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		ui_progress_dots(ui, R(x, y + 30, w, 8), th->accent);
		y += 48;
	}
	if (ready) {
		for (int k = home ? 0 : K_OTHER; k < NK; k++) {
			Rect r = R(0, y, view.w, 64);
			UiId id = ui_idn("ss-cat", k);
			bool tappable = home && k < K_OTHER;
			if (tappable) ui_hit(ui, id, r);
			if (tappable && ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.14f));
			Color c = k == K_FREE ? th->chrome2 : (k == K_OTHER && !home) ? th->accent : RGBX(KCOLS[k]);
			gfx_fill_rect(cr, R(x, y + 18, 28, 28), c);
			const char *label = k == K_OTHER && !home ? _("used") : tr(KNAMES[k]);
			gfx_text(cr, label, x + 44, y + 14, 24, FW_SEMILIGHT, th->fg, TA_LEFT, w - 160, 1);
			gfx_text(cr, human_size(val[k], sz, sizeof sz), x + w, y + 18, 20, FW_SEMILIGHT, th->sub, TA_RIGHT, 120, 1);
			if (tappable && ui_clicked(ui, id)) {
				if (k == K_TMP || k == K_TRASH) ask_clean(k == K_TRASH);
				else shell_open_builtin("files", g_dirs[k]);
			}
			y += 64;
		}
		if (g_capped)
			y += gfx_text(cr, _("some folders have too many files to count, so these sizes are estimates."), x, y + 8, 15,
			              FW_REGULAR, th->sub, TA_LEFT, w, 0) + 8;
	}
	if (!home) {
		y += 16;
		if (ui_button(ui, ui_id("ss-files"), R(x, y, MIN(w, 240), 52), _("open in files"), BTN_OUTLINE))
			shell_open_builtin("files", v->path);
		y += 60;
	}
	content_h = y + 24;
	ui_scroll_end(ui);
	if (!home) return;
	AppBarBtn b[1] = {{ICON_REFRESH, _("refresh"), g_scanning || g_cleaning}};
	const char *menu[2] = {_("delete temporary files"), _("empty trash")};
	int act = appbar_draw(ui, &g_bar, area, b, 1, menu, 2);
	if (act == 0) start_scan();
	else if (act == APPBAR_MENU) ask_clean(false);
	else if (act == APPBAR_MENU + 1) ask_clean(true);
}

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	(void)ud;
	if (page == P_VOL) vol_page(ui, area);
	else root_page(ui, area);
}

static void storage_draw(UI *ui, Rect area) { nav_draw(ui, &g_nav, area, page_fn, NULL); }

static void storage_open(const char *arg) {
	(void)arg;
	if (!g_scanning) find_volumes();
	if (g_scanned && !g_scanning && !g_cleaning) start_scan();   /* sizes may have changed meanwhile */
}

static bool storage_back(void) {
	if (nav_page(&g_nav) == P_VOL && appbar_back(&g_bar)) return true;
	return nav_pop(&g_nav, &sh.ui);
}

static void storage_close(void) {
	g_gen++;   /* drop a running scan's result */
	g_scanning = g_scanned = false;
	nav_init(&g_nav, P_ROOT);
}

const AppDef app_storage = {
	.id = "storage", .name = N_("Storage Sense"), .icon = ICON_STORAGE, .color = 0,
	.keywords = "disk space free clean temporary files trash sd card",
	.open = storage_open, .draw = storage_draw, .back = storage_back, .close = storage_close,
};

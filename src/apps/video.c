/* Video: lists the videos in ~/Videos (recursively) and ~/Downloads and plays them in an
 * installed Linux video player. */
#include "apps.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_VIDEOS 2000

typedef struct { char *path, *name; double size; time_t mtime; } Vid;
typedef VEC(Vid) VidList;
typedef struct { VidList list; } ScanJob;

static VidList g_vids;
static bool g_loaded, g_scanning;
static AppBar g_bar;
static Scroll g_scroll;
static int g_menu_i = -1;

/* ------------------------------------------------------------------ scan -- */

static bool is_video(const char *name) {
	static const char *ext[] = {".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v"};
	const char *dot = strrchr(name, '.');
	if (!dot) return false;
	for (size_t i = 0; i < ARRAY_LEN(ext); i++)
		if (!strcasecmp(dot, ext[i])) return true;
	return false;
}

static void scan_dir(VidList *l, const char *dir, int depth) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && l->len < MAX_VIDEOS) {
		if (e->d_name[0] == '.') continue;
		char *p = path_join(dir, e->d_name);
		struct stat st;
		if (stat(p, &st) == 0) {
			if (S_ISDIR(st.st_mode) && depth > 0) scan_dir(l, p, depth - 1);
			else if (S_ISREG(st.st_mode) && is_video(e->d_name)) {
				const char *dot = strrchr(e->d_name, '.');
				Vid v = {p, xasprintf("%.*s", (int)(dot - e->d_name), e->d_name), (double)st.st_size, st.st_mtime};
				vec_push(l, v);
				p = NULL;
			}
		}
		free(p);
	}
	closedir(d);
}

static void list_free(VidList *l) {
	for (int i = 0; i < l->len; i++) { free(l->items[i].path); free(l->items[i].name); }
	vec_free(l);
}

static int cmp_vid(const void *a, const void *b) {
	time_t x = ((const Vid *)a)->mtime, y = ((const Vid *)b)->mtime;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void scan_run(void *arg) {
	ScanJob *j = arg;
	char *videos = path_join(home_dir(), "Videos"), *dl = path_join(home_dir(), "Downloads");
	scan_dir(&j->list, videos, 4);
	scan_dir(&j->list, dl, 0);
	free(videos);
	free(dl);
	qsort(j->list.items, j->list.len, sizeof(Vid), cmp_vid);
}

static void scan_done(void *arg) {
	ScanJob *j = arg;
	list_free(&g_vids);
	g_vids = j->list;
	free(j);
	g_scanning = false;
	g_loaded = true;
	shell_invalidate();
}

static void rescan(void) {
	if (g_scanning) return;
	g_scanning = true;
	job_submit(scan_run, scan_done, xcalloc(1, sizeof(ScanJob)));
}

/* --------------------------------------------------------------- actions -- */

static void play(const Vid *v) {
	static const char *ids[] = {"mpv", "io.mpv.Mpv", "io.github.celluloid_player.Celluloid", "celluloid",
	                            "vlc", "org.videolan.VLC", "org.gnome.Totem", "totem"};
	for (size_t i = 0; i < ARRAY_LEN(ids); i++) {
		DesktopApp *a = desktop_find(ids[i]);
		if (a && desktop_launch_uri(a, v->path)) return;
	}
	static const char *bins[] = {"mpv", "celluloid", "vlc", "totem"};
	for (size_t i = 0; i < ARRAY_LEN(bins); i++) {
		if (!proc_exists(bins[i])) continue;
		char *argv[] = {(char *)bins[i], v->path, NULL};
		if (proc_spawn(argv)) return;
	}
	if (proc_exists("xdg-open")) { open_uri(v->path); return; }
	shell_toast(_("can't play this video"), _("install a video player such as mpv, celluloid, vlc or totem."), _("Video"));
}

static void remove_path(const char *path) {
	for (int i = 0; i < g_vids.len; i++) {
		if (!str_eq(g_vids.items[i].path, path)) continue;
		free(g_vids.items[i].path);
		free(g_vids.items[i].name);
		vec_remove(&g_vids, i);
		break;
	}
}

static void trash_done(int status, char *out, size_t len, void *ud) {
	char *path = ud;
	if (status != 0 && file_exists(path)) {
		if (unlink(path) != 0) { shell_toast(_("couldn't delete the video"), path_basename(path), _("Video")); rescan(); }
	}
	free(path);
	shell_invalidate();
}

static void delete_cb(int b, void *ud) {
	char *path = ud;
	if (b == 0) {
		char *argv[] = {"gio", "trash", "--", path, NULL};
		if (proc_exists("gio") && proc_run(argv, trash_done, xstrdup(path))) remove_path(path);
		else if (unlink(path) == 0) remove_path(path);
		else shell_toast(_("couldn't delete the video"), path_basename(path), _("Video"));
	}
	free(path);
	shell_invalidate();
}

static void menu_cb(int b, void *ud) {
	if (g_menu_i < 0 || g_menu_i >= g_vids.len) return;
	Vid *v = &g_vids.items[g_menu_i];
	if (b == 0) play(v);
	else if (b == 1)
		dialog_message(_("delete video?"), v->name, _("delete"), _("cancel"), delete_cb, xstrdup(v->path));
}

/* ------------------------------------------------------------------ page -- */

static void fmt_sub(const Vid *v, char *buf, size_t n) {
	char size[32], when[64];
	struct tm tm, now;
	time_t tn = time(NULL);
	localtime_r(&v->mtime, &tm);
	localtime_r(&tn, &now);
	if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) str_copy(when, sizeof when, _("today"));
	else i18n_fmt_date_short(when, sizeof when, &tm);
	snprintf(buf, n, "%s • %s", human_size(v->size, size, sizeof size), when);
}

static void video_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (!g_loaded && !g_scanning) rescan();
	Rect a = appbar_content(&g_bar, area);
	double y = ui_page_header(ui, 14, a.y + 6, _("Video"), _("videos"));
	Rect view = R(a.x, y + 4, a.w, rect_bottom(a) - y - 4);
	if (g_scanning) ui_progress_dots(ui, R(a.x + 24, y - 6, a.w - 48, 8), th->accent);
	if (g_loaded && !g_vids.len)
		app_empty(ui, R(a.x, y - 10, a.w, 100),
		          _("no videos found. videos in your Videos and Downloads folders show up here."));
	const double RH = 84, T = 64;
	double off = ui_scroll_begin(ui, ui_id("video-list"), &g_scroll, view, g_vids.len * RH + 20);
	cairo_translate(cr, view.x, view.y);
	for (int i = 0; i < g_vids.len; i++) {
		double ry = i * RH;
		if (ry + RH < off || ry > off + view.h) continue;
		Vid *v = &g_vids.items[i];
		Rect rr = R(0, ry, view.w, RH);
		UiId id = ui_idn("video-row", i);
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.14f));
		Rect t = R(24, ry + (RH - T) / 2, T, T);
		gfx_fill_rect(cr, t, th->accent);
		icon_draw(cr, ICON_VIDEO, rect_cx(t), rect_cy(t), 30, th->on_accent);
		char sub[128];
		fmt_sub(v, sub, sizeof sub);
		double x = t.x + T + 14, w = view.w - x - 16;
		gfx_text(cr, v->name, x, ry + 14, 24, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		gfx_text(cr, sub, x, ry + 46, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		if (ui_clicked(ui, id)) play(v);
		if (ui_longpressed(ui, id)) {
			static const char *items[2];
			items[0] = _("play");
			items[1] = _("delete");
			g_menu_i = i;
			dialog_menu(ui->p.x, ui->p.y, items, 2, menu_cb, NULL);
		}
	}
	ui_scroll_end(ui);
	AppBarBtn btns[] = {{ICON_REFRESH, _("refresh"), g_scanning}};
	if (appbar_draw(ui, &g_bar, area, btns, 1, NULL, 0) == 0) rescan();
}

static void video_open(const char *arg) {
	if (g_loaded) rescan();   /* pick up new downloads when the app comes back */
}

static bool video_back(void) { return appbar_back(&g_bar); }

static void video_close(void) {
	if (!g_scanning) { list_free(&g_vids); g_loaded = false; }
	g_scroll = (Scroll){0};
	g_bar = (AppBar){0};
}

const AppDef app_video = {
	.id = "video", .name = N_("Video"), .icon = ICON_VIDEO, .color = 0,
	.keywords = "movies films player mp4 mkv",
	.open = video_open, .draw = video_draw, .back = video_back, .close = video_close,
};

/* Photos: the Windows Phone 8.1 picture hub. A worker thread indexes ~/Pictures (plus
 * DCIM, Downloads and Desktop); only metadata stays in memory. Thumbnails come from a
 * bounded ImgCache and the viewer decodes at most the current picture and its
 * neighbours at screen resolution. */
#include "apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"
#include "stb_image.h"

#include <dirent.h>
#include <glib.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PHOTOS 4000
#define MAX_DIRS 1500
#define MAX_DEPTH 5
#define GAP 4.0

enum { AL_CAMERA, AL_SCREENSHOTS, AL_SAVED, AL_FOLDER };
enum { SRC_ALL, SRC_ALBUM, SRC_FAV };
enum { PG_ROOT, PG_ALBUM, PG_VIEWER };

typedef struct {
	char *path;
	int64_t mtime, size;
	int album, month;           /* month = year * 12 + mon */
	bool fav, sel;
} Photo;

typedef struct { char *name, *dir; int kind, count, cover; } Album;
typedef struct { char *path; int64_t mtime; } Stamp;

typedef struct {
	VEC(Photo) photos;          /* newest first */
	VEC(Album) albums;
	VEC(Stamp) dirs;
} Library;

static Library *g_lib;
static bool g_active;           /* app open (keep the library in memory) */
static bool g_scanning, g_scan_again, g_scan_tried;
static ImgCache *g_thumbs, *g_view, *g_tile_cache;

static VEC(char *) g_favs;
static bool g_favs_loaded;

/* recent pictures for the live tile survive close() (paths only) */
static char *g_recent[9];
static int g_nrecent;

static Nav g_nav;
static Pager g_pivot, g_pick_pivot;
static Scroll g_sc[3], g_album_sc;
static AppBar g_bar;
static bool g_nav_ready;
static int g_album_kind = -1;
static char g_album_name[256];
static bool g_select;
static int g_select_page;       /* pivot page the selection belongs to (-1 = album page) */

static struct {
	bool active;
	char ret[48];
	void (*cb)(const char *path, void *ud);
	void *ud;
} g_pick;

static struct {
	int *seq, n, cap, src;
	char *ext;                  /* a picture outside the library (deep link) */
	Pager pg;
	int cur;
	bool chrome, info, slideshow, bad;
	double zoom, panx, pany;
	Anim zanim;
	double z0, z1, px0, px1, py0, py1;
	Anim fade;
	int fade_from;
	Timer *show_timer, *tap_timer;
	bool tap_pending;
	double tap_t, tap_x, tap_y;
	AppBar bar;
	int checked;                /* index whose decodability was checked */
} V;

static char *g_pending_open;    /* deep link waiting for the first scan */
static char *g_menu_path;       /* context menu target */
static int *g_tmp, g_tmp_cap;   /* scratch sequence for grids */

static void scan_start(bool force);
static void viewer_rebuild(const char *keep);
static void photos_open_path(const char *path);
static void set_wallpaper(const char *path, bool lock);
static void slideshow_start(void);

/* ---------------------------------------------------------------- helpers */

static bool is_image_name(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot) return false;
	static const char *ext[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".svg", NULL};
	for (int i = 0; ext[i]; i++)
		if (!g_ascii_strcasecmp(dot, ext[i])) return true;
	return false;
}

/* XDG user directory (~/.config/user-dirs.dirs), e.g. ~/Resimler on Turkish systems */
static char *user_dir(const char *key, const char *fallback) {
	const char *cfgh = getenv("XDG_CONFIG_HOME");
	char *f = cfgh && *cfgh ? path_join(cfgh, "user-dirs.dirs") : path_join(home_dir(), ".config/user-dirs.dirs");
	char *data = file_read_all(f, NULL);
	free(f);
	char *res = NULL;
	if (data) {
		size_t kl = strlen(key);
		for (char *line = strtok(data, "\n"); line && !res; line = strtok(NULL, "\n")) {
			while (*line == ' ' || *line == '\t') line++;
			if (strncmp(line, key, kl) || line[kl] != '=') continue;
			char *v = line + kl + 1;
			if (*v == '"') v++;
			char *e = strchr(v, '"');
			if (e) *e = 0;
			if (str_starts(v, "$HOME/")) res = path_join(home_dir(), v + 6);
			else if (v[0] == '/') res = xstrdup(v);
		}
		free(data);
	}
	return res ? res : path_join(home_dir(), fallback);
}

static bool is_fav(const char *path) {
	for (int i = 0; i < g_favs.len; i++)
		if (str_eq(g_favs.items[i], path)) return true;
	return false;
}

static void favs_load(void) {
	if (g_favs_loaded) return;
	g_favs_loaded = true;
	cJSON *root = app_json_load("photos.json");
	cJSON *a = root ? cJSON_GetObjectItem(root, "favorites") : NULL;
	cJSON *it;
	cJSON_ArrayForEach(it, a)
		if (cJSON_IsString(it) && g_favs.len < 2000) vec_push(&g_favs, xstrdup(it->valuestring));
	cJSON_Delete(root);
}

static void favs_save(void) {
	cJSON *root = cJSON_CreateObject();
	cJSON *a = cJSON_AddArrayToObject(root, "favorites");
	for (int i = 0; i < g_favs.len; i++) cJSON_AddItemToArray(a, cJSON_CreateString(g_favs.items[i]));
	app_json_save("photos.json", root);
	cJSON_Delete(root);
}

static void lib_mark_fav(const char *path, bool fav) {
	if (!g_lib) return;
	for (int i = 0; i < g_lib->photos.len; i++)
		if (str_eq(g_lib->photos.items[i].path, path)) g_lib->photos.items[i].fav = fav;
}

static void fav_set(const char *path, bool fav) {
	favs_load();
	for (int i = 0; i < g_favs.len; i++) {
		if (str_eq(g_favs.items[i], path)) {
			if (fav) return;
			free(g_favs.items[i]);
			vec_remove(&g_favs, i);
			lib_mark_fav(path, false);
			favs_save();
			return;
		}
	}
	if (!fav) return;
	vec_push(&g_favs, xstrdup(path));
	lib_mark_fav(path, true);
	favs_save();
}

static void lib_free(Library *l) {
	if (!l) return;
	for (int i = 0; i < l->photos.len; i++) free(l->photos.items[i].path);
	for (int i = 0; i < l->albums.len; i++) { free(l->albums.items[i].name); free(l->albums.items[i].dir); }
	for (int i = 0; i < l->dirs.len; i++) free(l->dirs.items[i].path);
	vec_free(&l->photos);
	vec_free(&l->albums);
	vec_free(&l->dirs);
	free(l);
}

static const char *album_title(const Album *a) {
	switch (a->kind) {
	case AL_CAMERA: return _("Camera Roll");
	case AL_SCREENSHOTS: return _("Screenshots");
	case AL_SAVED: return _("Saved Pictures");
	default: return a->name;
	}
}

static void lib_recount(Library *l) {
	for (int i = 0; i < l->albums.len; i++) { l->albums.items[i].count = 0; l->albums.items[i].cover = -1; }
	for (int i = 0; i < l->photos.len; i++) {
		Album *a = &l->albums.items[l->photos.items[i].album];
		if (a->cover < 0) a->cover = i;
		a->count++;
	}
}

static int album_current(void) {
	if (!g_lib) return -1;
	for (int i = 0; i < g_lib->albums.len; i++) {
		Album *a = &g_lib->albums.items[i];
		if (a->kind == g_album_kind && (a->kind != AL_FOLDER || str_eq(a->name, g_album_name))) return a->count ? i : -1;
	}
	return -1;
}

/* fills `*buf` with photo indices of a source; returns the count */
static int seq_build(int src, int album, int **buf, int *cap) {
	int n = g_lib ? g_lib->photos.len : 0;
	if (*cap < n + 1) { *cap = n + 64; *buf = xrealloc(*buf, (size_t)*cap * sizeof(int)); }
	int k = 0;
	for (int i = 0; i < n; i++) {
		Photo *p = &g_lib->photos.items[i];
		if (src == SRC_ALBUM && p->album != album) continue;
		if (src == SRC_FAV && !p->fav) continue;
		(*buf)[k++] = i;
	}
	return k;
}

static int photo_find(const char *path) {
	if (!g_lib || !path) return -1;
	for (int i = 0; i < g_lib->photos.len; i++)
		if (str_eq(g_lib->photos.items[i].path, path)) return i;
	return -1;
}

static int thumb_px(UI *ui, double w) {
	double px = w * ui->scale;
	return px <= 160 ? 160 : px <= 256 ? 256 : 384;
}

static void tilde_path(const char *p, char *out, size_t cap) {
	const char *h = home_dir();
	size_t hl = strlen(h);
	if (!strncmp(p, h, hl) && (p[hl] == '/' || !p[hl])) snprintf(out, cap, "~%s", p + hl);
	else str_copy(out, cap, p);
}

/* "Saturday, November 7, 2026" / "7 Kasım 2026 Cumartesi", year omitted for this year when `short_year` */
static void fmt_date(char *buf, size_t n, const struct tm *tm, bool short_year) {
	time_t now = time(NULL);
	struct tm cur;
	localtime_r(&now, &cur);
	if (short_year && cur.tm_year == tm->tm_year) { i18n_fmt_date_long(buf, n, tm); return; }
	if (i18n_lang() == LANG_TR)
		snprintf(buf, n, "%d %s %d %s", tm->tm_mday, i18n_month(tm->tm_mon, false), tm->tm_year + 1900, i18n_weekday(tm->tm_wday, false));
	else
		snprintf(buf, n, "%s, %s %d, %d", i18n_weekday(tm->tm_wday, false), i18n_month(tm->tm_mon, false), tm->tm_mday, tm->tm_year + 1900);
}

static void dir_of(const char *p, char *out, size_t cap) {
	str_copy(out, cap, p);
	char *s = strrchr(out, '/');
	if (s && s != out) *s = 0;
}

/* ------------------------------------------------------------------- scan */

typedef struct {
	Stamp *old;                 /* previous directory stamps (owned) */
	int nold;
	bool force, changed;
	Library *lib;
} ScanJob;

static int64_t dir_mtime(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
	return (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
}

static int album_get(Library *l, int kind, const char *name, const char *dir) {
	for (int i = 0; i < l->albums.len; i++) {
		Album *a = &l->albums.items[i];
		if (a->kind == kind && (kind != AL_FOLDER || str_eq(a->name, name))) return i;
	}
	Album a = {.name = name ? xstrdup(name) : NULL, .dir = dir ? xstrdup(dir) : NULL, .kind = kind, .cover = -1};
	vec_push(&l->albums, a);
	return l->albums.len - 1;
}

static int album_for_subdir(Library *l, const char *name, const char *dir) {
	if (!g_ascii_strcasecmp(name, "Camera") || !g_ascii_strcasecmp(name, "Camera Roll") ||
	    !g_ascii_strcasecmp(name, "DCIM") || !g_ascii_strcasecmp(name, "Kamera"))
		return album_get(l, AL_CAMERA, NULL, dir);
	if (!g_ascii_strcasecmp(name, "Screenshots") || !g_ascii_strcasecmp(name, "Ekran görüntüleri"))
		return album_get(l, AL_SCREENSHOTS, NULL, dir);
	return album_get(l, AL_FOLDER, name, dir);
}

/* album < 0: decided per first-level subfolder (the Pictures root) */
static void scan_dir(Library *l, const char *dir, int depth, int max_depth, int album) {
	if (l->dirs.len >= MAX_DIRS) return;
	vec_push(&l->dirs, ((Stamp){xstrdup(dir), dir_mtime(dir)}));
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && l->photos.len < MAX_PHOTOS) {
		if (e->d_name[0] == '.') continue;
		char *p = path_join(dir, e->d_name);
		struct stat st;
		if (lstat(p, &st) != 0) { free(p); continue; }
		bool link = S_ISLNK(st.st_mode);
		if (link && stat(p, &st) != 0) { free(p); continue; }
		if (S_ISDIR(st.st_mode)) {
			if (!link && depth < max_depth) {
				int al = album >= 0 ? album : album_for_subdir(l, e->d_name, p);
				scan_dir(l, p, depth + 1, max_depth, al);
			}
		} else if (S_ISREG(st.st_mode) && st.st_size > 0 && is_image_name(e->d_name)) {
			int w, h, n;
			if (str_ends(e->d_name, ".webp") || str_ends(e->d_name, ".WEBP")) {
				if (!stbi_info(p, &w, &h, &n)) { free(p); continue; }
			}
			struct tm tm;
			time_t t = st.st_mtime;
			localtime_r(&t, &tm);
			Photo ph = {.path = p, .mtime = (int64_t)st.st_mtime, .size = (int64_t)st.st_size,
				.album = album >= 0 ? album : album_get(l, AL_SAVED, NULL, dir),
				.month = (tm.tm_year + 1900) * 12 + tm.tm_mon};
			vec_push(&l->photos, ph);
			p = NULL;
		}
		free(p);
	}
	closedir(d);
}

static int cmp_photo(const void *a, const void *b) {
	const Photo *x = a, *y = b;
	if (x->mtime != y->mtime) return x->mtime > y->mtime ? -1 : 1;
	return strcmp(x->path, y->path);
}

static void scan_run(void *arg) {
	ScanJob *j = arg;
	if (!j->force && j->nold) {
		bool stale = false;
		for (int i = 0; i < j->nold && !stale; i++)
			if (dir_mtime(j->old[i].path) != j->old[i].mtime) stale = true;
		if (!stale) return;
	}
	j->changed = true;
	Library *l = j->lib = xcalloc(1, sizeof *l);
	/* fixed albums first, in WP order */
	album_get(l, AL_CAMERA, NULL, NULL);
	album_get(l, AL_SCREENSHOTS, NULL, NULL);
	album_get(l, AL_SAVED, NULL, NULL);
	char *pics = user_dir("XDG_PICTURES_DIR", "Pictures");
	char *dl = user_dir("XDG_DOWNLOAD_DIR", "Downloads");
	char *desk = user_dir("XDG_DESKTOP_DIR", "Desktop");
	char *dcim = path_join(home_dir(), "DCIM");
	scan_dir(l, pics, 0, MAX_DEPTH, -1);
	scan_dir(l, dcim, 0, MAX_DEPTH, 0);
	if (!str_eq(dl, home_dir())) scan_dir(l, dl, 0, 0, 2);
	if (!str_eq(desk, home_dir())) scan_dir(l, desk, 0, 0, 2);
	free(pics); free(dl); free(desk); free(dcim);
	qsort(l->photos.items, (size_t)l->photos.len, sizeof(Photo), cmp_photo);
	/* folder albums sorted by name (stable index remap) */
	int na = l->albums.len;
	int *order = xcalloc((size_t)na, sizeof(int)), *remap = xcalloc((size_t)na, sizeof(int));
	for (int i = 0; i < na; i++) order[i] = i;
	for (int i = 4; i < na; i++)
		for (int k = i; k > 3 && g_utf8_collate(l->albums.items[order[k - 1]].name, l->albums.items[order[k]].name) > 0; k--) {
			int t = order[k]; order[k] = order[k - 1]; order[k - 1] = t;
		}
	Album *sorted = xcalloc((size_t)na + 1, sizeof(Album));
	for (int i = 0; i < na; i++) { sorted[i] = l->albums.items[order[i]]; remap[order[i]] = i; }
	memcpy(l->albums.items, sorted, (size_t)na * sizeof(Album));
	for (int i = 0; i < l->photos.len; i++) l->photos.items[i].album = remap[l->photos.items[i].album];
	free(sorted); free(order); free(remap);
	lib_recount(l);
}

static void tile_update_recent(void) {
	for (int i = 0; i < g_nrecent; i++) free(g_recent[i]);
	g_nrecent = 0;
	if (!g_lib) return;
	for (int i = 0; i < g_lib->photos.len && g_nrecent < (int)ARRAY_LEN(g_recent); i++)
		g_recent[g_nrecent++] = xstrdup(g_lib->photos.items[i].path);
}

static char *viewer_path_dup(void);

static void scan_done(void *arg) {
	ScanJob *j = arg;
	g_scanning = false;
	if (j->changed) {
		char *keep = viewer_path_dup();
		lib_free(g_lib);
		g_lib = j->lib;
		favs_load();
		for (int i = 0; i < g_lib->photos.len; i++) g_lib->photos.items[i].fav = is_fav(g_lib->photos.items[i].path);
		tile_update_recent();
		viewer_rebuild(keep);
		free(keep);
		if (g_pending_open) {
			char *p = g_pending_open;
			g_pending_open = NULL;
			photos_open_path(p);
			free(p);
		}
	}
	for (int i = 0; i < j->nold; i++) free(j->old[i].path);
	free(j->old);
	free(j);
	if (!g_active) { lib_free(g_lib); g_lib = NULL; }
	if (g_scan_again) { g_scan_again = false; scan_start(true); }
	shell_invalidate();
}

static void scan_start(bool force) {
	g_scan_tried = true;
	if (g_scanning) { if (force) g_scan_again = true; return; }
	g_scanning = true;
	ScanJob *j = xcalloc(1, sizeof *j);
	j->force = force || !g_lib;
	if (g_lib && !j->force) {
		j->nold = g_lib->dirs.len;
		j->old = xcalloc((size_t)j->nold + 1, sizeof(Stamp));
		for (int i = 0; i < j->nold; i++) j->old[i] = (Stamp){xstrdup(g_lib->dirs.items[i].path), g_lib->dirs.items[i].mtime};
	}
	job_submit(scan_run, scan_done, j);
}

/* Called after a new picture (e.g. a screenshot) was saved. */
void photos_rescan(void) {
	if (g_active || g_scan_tried) scan_start(true);
}

/* ------------------------------------------------------------------ trash */

static bool trash_manual(const char *path) {
	char *files = xdg_dir("XDG_DATA_HOME", ".local/share", "Trash/files");
	char *info = xdg_dir("XDG_DATA_HOME", ".local/share", "Trash/info");
	const char *base = path_basename(path);
	char *dst = NULL, *inf = NULL;
	for (int i = 1; i < 1000; i++) {
		char name[512];
		if (i == 1) str_copy(name, sizeof name, base);
		else snprintf(name, sizeof name, "%s.%d", base, i);
		dst = path_join(files, name);
		char *in = xasprintf("%s.trashinfo", name);
		inf = path_join(info, in);
		free(in);
		if (!file_exists(dst) && !file_exists(inf)) break;
		free(dst); free(inf);
		dst = inf = NULL;
	}
	bool ok = false;
	if (dst) {
		char *esc = g_uri_escape_string(path, "/", FALSE);
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		char date[32];
		strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
		char *body = xasprintf("[Trash Info]\nPath=%s\nDeletionDate=%s\n", esc, date);
		g_free(esc);
		if (file_write_all(inf, body, strlen(body))) {
			ok = rename(path, dst) == 0;
			if (!ok) unlink(inf);
		}
		free(body);
	}
	free(dst); free(inf); free(files); free(info);
	return ok;
}

static void trash_done(int status, char *out, size_t len, void *ud) {
	char *path = ud;
	if (status != 0 && file_exists(path) && !trash_manual(path))
		shell_toast(_("Couldn't delete the picture"), path_basename(path), _("Photos"));
	free(path);
	scan_start(true);
}

static void lib_remove(const char *path) {
	int i = photo_find(path);
	if (i < 0) return;
	char *keep = viewer_path_dup();
	free(g_lib->photos.items[i].path);
	vec_remove(&g_lib->photos, i);
	lib_recount(g_lib);
	tile_update_recent();
	viewer_rebuild(keep);
	free(keep);
}

static void photo_delete(const char *path) {
	fav_set(path, false);
	if (proc_exists("gio")) {
		char *argv[] = {"gio", "trash", "--", (char *)path, NULL};
		if (proc_run(argv, trash_done, xstrdup(path))) { lib_remove(path); return; }
	}
	if (!trash_manual(path)) { shell_toast(_("Couldn't delete the picture"), path_basename(path), _("Photos")); return; }
	lib_remove(path);
	scan_start(true);
}

static void delete_one_cb(int btn, void *ud) {
	char *p = ud;
	if (btn == 0) photo_delete(p);
	free(p);
	shell_invalidate();
}

static void confirm_delete(const char *path) {
	dialog_message(_("delete picture?"), path_basename(path), _("delete"), _("cancel"), delete_one_cb, xstrdup(path));
}

static void delete_sel_cb(int btn, void *ud) {
	if (btn != 0 || !g_lib) return;
	int n = 0;
	char **paths = xcalloc((size_t)g_lib->photos.len + 1, sizeof(char *));
	for (int i = 0; i < g_lib->photos.len; i++)
		if (g_lib->photos.items[i].sel) paths[n++] = xstrdup(g_lib->photos.items[i].path);
	for (int i = 0; i < n; i++) { photo_delete(paths[i]); free(paths[i]); }
	free(paths);
	g_select = false;
	shell_invalidate();
}

void photos_set_wallpaper(const char *path, bool lock) { set_wallpaper(path, lock); }

static void set_wallpaper(const char *path, bool lock) {
	if (lock) str_copy(cfg.lock_wallpaper, sizeof cfg.lock_wallpaper, path);
	else { str_copy(cfg.wallpaper, sizeof cfg.wallpaper, path); cfg.start_wallpaper = true; }
	wallpaper_changed();
	config_save_soon();
	shell_toast(lock ? _("Lock screen picture changed") : _("Start background changed"), path_basename(path), _("Photos"));
}

/* ----------------------------------------------------------------- picker */

static void pick_finish(const char *path) {
	void (*cb)(const char *, void *) = g_pick.cb;
	void *ud = g_pick.ud;
	char ret[48];
	str_copy(ret, sizeof ret, g_pick.ret);
	g_pick.active = false;
	g_pick.cb = NULL;
	nav_init(&g_nav, PG_ROOT);
	if (cb) cb(path, ud);
	if (ret[0]) shell_open_builtin(ret, NULL);
	else shell_go_home();
}

void photos_pick(const char *return_app, void (*cb)(const char *path, void *ud), void *ud) {
	if (g_pick.active && g_pick.cb) g_pick.cb(NULL, g_pick.ud);
	g_pick.active = true;
	str_copy(g_pick.ret, sizeof g_pick.ret, return_app ? return_app : "");
	g_pick.cb = cb;
	g_pick.ud = ud;
	shell_open_builtin("photos", "pick");
}

/* ----------------------------------------------------------------- viewer */

static const char *seq_path(int k) {
	if (k < 0 || k >= V.n) return NULL;
	if (V.seq[k] < 0) return V.ext;
	return g_lib && V.seq[k] < g_lib->photos.len ? g_lib->photos.items[V.seq[k]].path : NULL;
}

static char *viewer_path_dup(void) {
	if (g_nav_ready && nav_page(&g_nav) == PG_VIEWER) {
		const char *p = seq_path(V.cur);
		return p ? xstrdup(p) : NULL;
	}
	return NULL;
}

static void zoom_reset(void) {
	V.zoom = 1;
	V.panx = V.pany = 0;
	anim_set(&V.zanim, 1);
}

static void slideshow_stop(void) {
	V.slideshow = false;
	if (V.show_timer) timer_set(V.show_timer, 0);
}

/* rebuilds the viewer sequence after the library changed, keeping `keep` current */
static void viewer_rebuild(const char *keep) {
	if (!g_nav_ready || nav_page(&g_nav) != PG_VIEWER || (V.n == 1 && V.seq[0] < 0)) return;
	int album = V.src == SRC_ALBUM ? album_current() : -1;
	V.n = (V.src == SRC_ALBUM && album < 0) ? 0 : seq_build(V.src, album, &V.seq, &V.cap);
	int idx = -1;
	for (int k = 0; keep && k < V.n; k++)
		if (str_eq(seq_path(k), keep)) { idx = k; break; }
	if (idx < 0) idx = CLAMP(V.cur, 0, MAX(0, V.n - 1));
	if (idx != V.cur) zoom_reset();
	V.cur = idx;
	V.pg.pos = V.pg.target = idx;
	V.checked = -1;
	if (!V.n) {
		slideshow_stop();
		V.info = false;
		nav_pop(&g_nav, &sh.ui);
	}
}

static void viewer_open(UI *ui, int src, const char *path, bool slideshow) {
	free(V.ext);
	V.ext = NULL;
	V.src = src;
	int album = src == SRC_ALBUM ? album_current() : -1;
	if (path && photo_find(path) < 0) {
		V.ext = xstrdup(path);
		if (V.cap < 1) { V.cap = 8; V.seq = xrealloc(V.seq, (size_t)V.cap * sizeof(int)); }
		V.seq[0] = -1;
		V.n = 1;
	} else {
		V.n = seq_build(src, album, &V.seq, &V.cap);
	}
	if (!V.n) return;
	int idx = 0;
	for (int k = 0; path && k < V.n; k++)
		if (str_eq(seq_path(k), path)) { idx = k; break; }
	V.cur = idx;
	V.pg = (Pager){.pos = idx, .target = idx};
	V.chrome = !slideshow;
	V.info = false;
	V.bar = (AppBar){0};
	V.checked = -1;
	zoom_reset();
	anim_set(&V.fade, 1);
	g_select = false;
	if (nav_page(&g_nav) != PG_VIEWER) nav_push(&g_nav, ui, PG_VIEWER);
	if (slideshow) slideshow_start();
}

static void show_tick(void *ud) {
	if (!V.slideshow || V.n < 1) { slideshow_stop(); return; }
	V.fade_from = V.cur;
	V.cur = (V.cur + 1) % V.n;
	V.pg.pos = V.pg.target = V.cur;
	zoom_reset();
	anim_start(&V.fade, 0, 1, now_sec(), cfg.animations ? 0.9 : 0.001, EASE_INOUT_CUBIC);
	shell_invalidate();
}

static void slideshow_start(void) {
	if (V.n < 1) return;
	V.slideshow = true;
	V.chrome = false;
	V.info = false;
	zoom_reset();
	if (!V.show_timer) V.show_timer = timer_add(0, true, show_tick, NULL);
	timer_set(V.show_timer, 4000);
	shell_invalidate();
}

static void tap_fire(void *ud) {
	if (!V.tap_pending) return;
	V.tap_pending = false;
	if (V.slideshow) { slideshow_stop(); V.chrome = true; }
	else if (V.info) V.info = false;
	else { V.chrome = !V.chrome; V.bar.open = false; }
	shell_invalidate();
}

/* geometry of picture k fitted into r */
static void fit_size(const Image *img, Rect r, double *w, double *h) {
	double s = MIN(r.w / img->w, r.h / img->h);
	*w = img->w * s;
	*h = img->h * s;
}

static void pan_clamp(const Image *img, Rect r, double z, double *px, double *py) {
	if (!img) { *px = *py = 0; return; }
	double w, h;
	fit_size(img, r, &w, &h);
	double mx = MAX(0, (w * z - r.w) / 2), my = MAX(0, (h * z - r.h) / 2);
	*px = CLAMP(*px, -mx, mx);
	*py = CLAMP(*py, -my, my);
}

static int view_px(void) { return MIN(2048, MAX(sh.pw, sh.ph)); }

static Image *viewer_image(UI *ui, int k, bool *full) {
	const char *p = seq_path(k);
	if (!p) return NULL;
	Image *img = imgcache_get(g_view, p, view_px());
	*full = img != NULL;
	if (!img) img = imgcache_get(g_thumbs, p, 160);
	return img;
}

static void viewer_picture(UI *ui, int k, Rect r, double z, double px, double py, double alpha) {
	bool full = false;
	Image *img = viewer_image(ui, k, &full);
	if (!img) {
		if (k == V.cur && V.bad)
			gfx_text(ui->cr, _("can't show this picture"), rect_cx(r), rect_cy(r) - 12, 20, FW_SEMILIGHT,
			         RGBX(0x999999), TA_CENTER, r.w - 40, 2);
		return;
	}
	double w, h;
	fit_size(img, r, &w, &h);
	w *= z; h *= z;
	Rect ir = R(rect_cx(r) - w / 2 + px, rect_cy(r) - h / 2 + py, w, h);
	gfx_image_contain(ui->cr, img, ir, alpha);
}

static void info_line(UI *ui, double x, double *y, double w, const char *label, const char *value) {
	gfx_text(ui->cr, label, x, *y, 15, FW_REGULAR, RGBX(0x9A9A9A), TA_LEFT, w, 1);
	*y += 20;
	*y += gfx_text(ui->cr, value, x, *y, 22, FW_SEMILIGHT, COL_WHITE, TA_LEFT, w, 3) + 12;
}

static void viewer_info(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const char *p = seq_path(V.cur);
	if (!p) return;
	gfx_fill_rect(cr, area, RGBA(0x000000, 0.86f));
	UiId id = ui_id("ph-info");
	ui_hit(ui, id, area);
	if (ui_clicked(ui, id)) V.info = false;
	double y = ui_page_header(ui, 14, area.y + 6, _("Photos"), _("info"));
	double x = 24, w = area.w - 48;
	struct stat st;
	bool have = stat(p, &st) == 0;
	info_line(ui, x, &y, w, _("file name"), path_basename(p));
	char buf[256];
	if (have) {
		struct tm tm;
		localtime_r(&st.st_mtime, &tm);
		char d[96], t[32];
		fmt_date(d, sizeof d, &tm, false);
		i18n_fmt_time(t, sizeof t, &tm, cfg.h24);
		snprintf(buf, sizeof buf, "%s  %s", d, t);
		info_line(ui, x, &y, w, _("date taken"), buf);
	}
	int iw = 0, ih = 0, n;
	if (stbi_info(p, &iw, &ih, &n)) {
		snprintf(buf, sizeof buf, "%d × %d", iw, ih);
		info_line(ui, x, &y, w, _("dimensions"), buf);
	}
	if (have) info_line(ui, x, &y, w, _("size"), human_size((double)st.st_size, buf, sizeof buf));
	char dir[512], td[512];
	dir_of(p, dir, sizeof dir);
	tilde_path(dir, td, sizeof td);
	info_line(ui, x, &y, w, _("folder"), td);
}

static void viewer_menu_action(int idx, const char *p) {
	switch (idx) {
	case 0: fav_set(p, !is_fav(p)); break;
	case 1: confirm_delete(p); break;
	case 2: slideshow_start(); break;
	case 3: V.info = true; break;
	case APPBAR_MENU + 0: set_wallpaper(p, true); break;
	case APPBAR_MENU + 1: set_wallpaper(p, false); break;
	case APPBAR_MENU + 2: open_uri(p); break;
	}
}

static void viewer_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	Theme saved = ui->th;
	theme_make(&ui->th, true, saved.accent);   /* the viewer is always dark */
	gfx_fill_rect(cr, area, COL_BLACK);
	if (V.n <= 0) { ui->th = saved; return; }
	V.cur = CLAMP(V.cur, 0, V.n - 1);

	/* zoom animation */
	if (anim_running(&V.zanim, ui->now)) {
		double e = ui_anim(ui, &V.zanim);
		V.zoom = V.z0 + (V.z1 - V.z0) * e;
		V.panx = V.px0 + (V.px1 - V.px0) * e;
		V.pany = V.py0 + (V.py1 - V.py0) * e;
	}
	bool zoomed = V.zoom > 1.01;
	bool use_pager = !zoomed && !V.slideshow && !V.info;
	UiId pid = ui_id("ph-viewer"), iid = ui_id("ph-vimg");
	if (use_pager) {
		ui_pager_update(ui, pid, &V.pg, area, V.n);
		if (V.pg.target != V.cur) { V.cur = V.pg.target; zoom_reset(); V.checked = -1; }
	}
	if (V.checked != V.cur) {
		V.checked = V.cur;
		const char *p = seq_path(V.cur);
		int w, h, n;
		V.bad = p && !str_ends(p, ".svg") && !str_ends(p, ".SVG") && !stbi_info(p, &w, &h, &n);
	}
	/* keep neighbours decoded for instant swipes */
	bool dummy;
	if (V.cur + 1 < V.n) viewer_image(ui, V.cur + 1, &dummy);
	if (V.cur > 0) viewer_image(ui, V.cur - 1, &dummy);

	if (V.slideshow && anim_running(&V.fade, ui->now)) {
		double e = ui_anim(ui, &V.fade);
		viewer_picture(ui, V.fade_from, area, 1, 0, 0, 1 - e);
		viewer_picture(ui, V.cur, area, 1, 0, 0, e);
	} else if (use_pager) {
		for (int k = MAX(0, V.cur - 1); k <= MIN(V.n - 1, V.cur + 1); k++) {
			Rect pr;
			if (!ui_pager_page(&V.pg, k, area, &pr)) continue;
			/* small gap between pictures while sliding, like the phone */
			pr.x += (k - V.pg.pos) * 16;
			ui_clip_push(ui, area);
			viewer_picture(ui, k, pr, 1, 0, 0, 1);
			ui_clip_pop(ui);
		}
	} else {
		viewer_picture(ui, V.cur, area, V.zoom, V.panx, V.pany, 1);
	}

	/* gestures on the picture */
	bool full;
	Image *img = viewer_image(ui, V.cur, &full);
	if (!V.info) {
		if (zoomed) ui_hit_drag(ui, iid, area);
		else ui_hit(ui, iid, area);
	}
	double dx, dy, wx, wy;
	if (zoomed && ui_drag_delta(ui, iid, &dx, &dy)) {
		V.panx += dx;
		V.pany += dy;
		pan_clamp(img, area, V.zoom, &V.panx, &V.pany);
	}
	if (ui_clicked(ui, iid)) {
		double x = ui->p.x, y = ui->p.y;
		if (V.tap_pending && ui->now - V.tap_t < 0.4 && hypot(x - V.tap_x, y - V.tap_y) < 40) {
			V.tap_pending = false;
			if (V.slideshow) slideshow_stop();
			V.z0 = V.zoom; V.px0 = V.panx; V.py0 = V.pany;
			if (zoomed) { V.z1 = 1; V.px1 = V.py1 = 0; }
			else {
				V.z1 = 2.5;
				double qx = (x - rect_cx(area) - V.panx) / V.zoom, qy = (y - rect_cy(area) - V.pany) / V.zoom;
				V.px1 = x - rect_cx(area) - qx * V.z1;
				V.py1 = y - rect_cy(area) - qy * V.z1;
				pan_clamp(img, area, V.z1, &V.px1, &V.py1);
			}
			anim_start(&V.zanim, 0, 1, ui->now, cfg.animations ? 0.28 : 0.001, EASE_OUT_CUBIC);
		} else {
			V.tap_pending = true;
			V.tap_t = ui->now;
			V.tap_x = x;
			V.tap_y = y;
			if (!V.tap_timer) V.tap_timer = timer_add(0, false, tap_fire, NULL);
			timer_set(V.tap_timer, 330);
		}
	}
	if (img && !V.info && ui_wheel_for(ui, area, &wx, &wy)) {
		double z = CLAMP(V.zoom * exp(-wy / 240.0), 1, 5);
		double mx = ui->mx - rect_cx(area), my = ui->my - rect_cy(area);
		double qx = (mx - V.panx) / V.zoom, qy = (my - V.pany) / V.zoom;
		V.panx = mx - qx * z;
		V.pany = my - qy * z;
		V.zoom = z;
		anim_set(&V.zanim, 1);
		if (z <= 1.001) V.panx = V.pany = 0;
		pan_clamp(img, area, V.zoom, &V.panx, &V.pany);
	}
	if (!V.slideshow && !zoomed) {
		if (ui_key(ui, UK_RIGHT) && V.cur + 1 < V.n) ui_pager_goto(&V.pg, V.cur + 1);
		if (ui_key(ui, UK_LEFT) && V.cur > 0) ui_pager_goto(&V.pg, V.cur - 1);
	}
	if (use_pager) ui_pager_end(ui);

	const char *p = seq_path(V.cur);
	if (V.chrome && !V.slideshow && !V.info && p) {
		/* date strip at the top */
		struct stat st;
		if (stat(p, &st) == 0) {
			struct tm tm;
			localtime_r(&st.st_mtime, &tm);
			char d[96], t[32], line[160];
			fmt_date(d, sizeof d, &tm, true);
			i18n_fmt_time(t, sizeof t, &tm, cfg.h24);
			snprintf(line, sizeof line, "%s  %s", d, t);
			Rect strip = R(area.x, area.y, area.w, 40);
			gfx_fill_rect(cr, strip, RGBA(0x000000, 0.5f));
			gfx_text(cr, i18n_lower(line), area.x + 18, area.y + 10, 17, FW_SEMILIGHT, COL_WHITE, TA_LEFT, area.w - 100, 1);
			char cnt[32];
			snprintf(cnt, sizeof cnt, "%d/%d", V.cur + 1, V.n);
			gfx_text(cr, cnt, rect_right(area) - 18, area.y + 12, 15, FW_REGULAR, RGBX(0xBBBBBB), TA_RIGHT, 80, 1);
		}
	}
	if (V.info) viewer_info(ui, area);
	if (V.chrome && !V.slideshow && !V.info && p) {
		bool fav = is_fav(p);
		AppBarBtn b[] = {
			{fav ? ICON_STAR_FILLED : ICON_STAR, fav ? _("unfavorite") : _("favorite"), false},
			{ICON_DELETE, _("delete"), false},
			{ICON_SLIDESHOW, _("slideshow"), false},
			{ICON_INFO, _("info"), false},
		};
		const char *menu[] = {_("use as lock screen"), _("use as start background"), _("open with…")};
		int r = appbar_draw(ui, &V.bar, area, b, 4, menu, 3);
		if (r >= 0) {
			char *pc = xstrdup(p);
			viewer_menu_action(r, pc);
			free(pc);
		}
	}
	ui->th = saved;
}

/* ------------------------------------------------------------------ grids */

static void ctx_cb(int idx, void *ud) {
	char *p = g_menu_path;
	g_menu_path = NULL;
	if (!p) return;
	if (idx == 0) fav_set(p, !is_fav(p));
	else if (idx == 1) confirm_delete(p);
	else if (idx == 2) set_wallpaper(p, true);
	else if (idx == 3) set_wallpaper(p, false);
	free(p);
	shell_invalidate();
}

static void open_context(UI *ui, const char *path) {
	static const char *items[4];
	items[0] = is_fav(path) ? _("remove from favorites") : _("add to favorites");
	items[1] = _("delete");
	items[2] = _("use as lock screen");
	items[3] = _("use as start background");
	free(g_menu_path);
	g_menu_path = xstrdup(path);
	dialog_menu(ui->p.x, ui->p.y, items, 4, ctx_cb, NULL);
}

static void thumb_draw(UI *ui, Rect r, const char *path, bool pressed) {
	if (pressed) r = rect_inset(r, 3);
	Image *img = imgcache_get(g_thumbs, path, thumb_px(ui, r.w));
	if (img) gfx_image_cover(ui->cr, img, r, 1);
	else gfx_fill_rect(ui->cr, r, ui->th.chrome2);
}

static void tap_photo(UI *ui, int src, int idx) {
	Photo *ph = &g_lib->photos.items[idx];
	if (g_pick.active) {
		char *p = xstrdup(ph->path);
		pick_finish(p);
		free(p);
		return;
	}
	if (g_select) { ph->sel = !ph->sel; return; }
	viewer_open(ui, src, ph->path, false);
}

static void empty_or_loading(UI *ui, Rect view, const char *text) {
	if (!g_lib) ui_progress_bar(ui, R(view.x + 24, view.y + 6, view.w - 48, 10), -1);
	else app_empty(ui, R(view.x, view.y - 10, view.w, view.h), text);
}

/* square thumbnail grid, optionally grouped by month */
static void grid_draw(UI *ui, Rect view, const char *key, Scroll *sc, int src, int album, bool headers, const char *empty) {
	cairo_t *cr = ui->cr;
	int n = g_lib ? seq_build(src, album, &g_tmp, &g_tmp_cap) : 0;
	if (!n) { empty_or_loading(ui, view, empty); return; }
	double left = 12, inner = view.w - 2 * left;
	int cols = MAX(4, (int)((inner + GAP) / 118));
	double cell = floor((inner - (cols - 1) * GAP) / cols);
	const double HDR = 50;
	/* pass 1: height */
	double y = 0;
	int col = 0, prev = -1;
	for (int k = 0; k < n; k++) {
		int m = g_lib->photos.items[g_tmp[k]].month;
		if (headers && m != prev) { if (col) y += cell + GAP; col = 0; y += HDR; prev = m; }
		if (++col == cols) { col = 0; y += cell + GAP; }
	}
	if (col) y += cell + GAP;
	double total = y + 24;
	double off = ui_scroll_begin(ui, ui_id(key), sc, view, total);
	cairo_translate(cr, 0, view.y);
	y = 0; col = 0; prev = -1;
	int sel_src = src;
	for (int k = 0; k < n; k++) {
		int idx = g_tmp[k];
		Photo *ph = &g_lib->photos.items[idx];
		if (headers && ph->month != prev) {
			if (col) y += cell + GAP;
			col = 0;
			prev = ph->month;
			if (y + HDR > off && y < off + view.h) {
				struct tm tm = {.tm_year = ph->month / 12 - 1900, .tm_mon = ph->month % 12, .tm_mday = 1};
				char b[64];
				i18n_fmt_month_year(b, sizeof b, &tm);
				gfx_text(cr, i18n_lower(b), view.x + left + 2, y + 10, 24, FW_SEMILIGHT, ui->th.fg, TA_LEFT, inner, 1);
			}
			y += HDR;
		}
		Rect r = R(view.x + left + col * (cell + GAP), y, cell, cell);
		if (r.y + cell > off && r.y < off + view.h) {
			UiId id = ui_idn(key, idx);
			ui_hit(ui, id, r);
			thumb_draw(ui, r, ph->path, ui_pressed(ui, id));
			if (g_select) {
				Rect cb = R(rect_right(r) - 30, r.y + 6, 24, 24);
				if (ph->sel) {
					gfx_stroke_rect(cr, r, 3, ui->th.accent);
					gfx_fill_rect(cr, cb, ui->th.accent);
					icon_draw(cr, ICON_CHECK, rect_cx(cb), rect_cy(cb), 18, COL_WHITE);
				} else {
					gfx_fill_rect(cr, cb, RGBA(0x000000, 0.4f));
					gfx_stroke_rect(cr, cb, 2, COL_WHITE);
				}
			} else if (ph->fav && src != SRC_FAV) {
				icon_draw(cr, ICON_STAR_FILLED, rect_right(r) - 14, rect_bottom(r) - 14, 14, COL_WHITE);
			}
			if (ui_clicked(ui, id)) tap_photo(ui, sel_src, idx);
			if (!g_pick.active && !g_select && ui_longpressed(ui, id)) open_context(ui, ph->path);
		}
		if (++col == cols) { col = 0; y += cell + GAP; }
		if (y > off + view.h + HDR) break;
	}
	ui_scroll_end(ui);
}

static void albums_draw(UI *ui, Rect view) {
	cairo_t *cr = ui->cr;
	int na = 0;
	for (int i = 0; g_lib && i < g_lib->albums.len; i++) na += g_lib->albums.items[i].count > 0;
	if (!na) { empty_or_loading(ui, view, _("no albums yet. pictures you save or take show up here.")); return; }
	double left = 12, gap = 12, inner = view.w - 2 * left;
	int cols = MAX(2, (int)((inner + gap) / 180));
	double tile = floor((inner - (cols - 1) * gap) / cols);
	double row = tile + 66;
	double total = ((na + cols - 1) / cols) * row + 20;
	double off = ui_scroll_begin(ui, ui_id("ph-albums"), &g_sc[1], view, total);
	cairo_translate(cr, 0, view.y);
	int k = 0;
	for (int i = 0; i < g_lib->albums.len; i++) {
		Album *a = &g_lib->albums.items[i];
		if (!a->count) continue;
		int c = k % cols, rw = k / cols;
		k++;
		Rect r = R(view.x + left + c * (tile + gap), rw * row + 6, tile, tile);
		if (r.y + row < off || r.y > off + view.h) continue;
		UiId id = ui_idn("ph-album", i);
		ui_hit(ui, id, R(r.x, r.y, r.w, row - 6));
		bool pr = ui_pressed(ui, id);
		thumb_draw(ui, r, g_lib->photos.items[a->cover].path, pr);
		gfx_text(cr, album_title(a), r.x, rect_bottom(r) + 6, 20, FW_SEMILIGHT, ui->th.fg, TA_LEFT, tile, 1);
		char cnt[32];
		snprintf(cnt, sizeof cnt, "%d", a->count);
		gfx_text(cr, cnt, r.x, rect_bottom(r) + 32, 15, FW_REGULAR, ui->th.sub, TA_LEFT, tile, 1);
		if (ui_clicked(ui, id)) {
			g_album_kind = a->kind;
			str_copy(g_album_name, sizeof g_album_name, a->name ? a->name : "");
			g_album_sc = (Scroll){0};
			g_select = false;
			nav_push(&g_nav, ui, PG_ALBUM);
		}
	}
	ui_scroll_end(ui);
}

static void root_page(UI *ui, int page, Rect r, void *ud) {
	switch (page) {
	case 0:
		grid_draw(ui, r, "ph-col", &g_sc[0], SRC_ALL, -1, true,
		          _("no pictures yet. pictures you take or save, and screenshots, show up here."));
		break;
	case 1: albums_draw(ui, r); break;
	default:
		grid_draw(ui, r, "ph-fav", &g_sc[2], SRC_FAV, -1, false,
		          _("no favorites yet. open a picture and tap favorite to see it here."));
		break;
	}
}

static int count_selected(void) {
	int n = 0;
	for (int i = 0; g_lib && i < g_lib->photos.len; i++) n += g_lib->photos.items[i].sel;
	return n;
}

static void select_set(bool on, int page) {
	g_select = on;
	g_select_page = page;
	for (int i = 0; g_lib && i < g_lib->photos.len; i++) g_lib->photos.items[i].sel = false;
}

/* app bar shared by the root and album pages */
static void grid_appbar(UI *ui, Rect area, int src, int album, bool can_select) {
	if (g_select) {
		int ns = count_selected();
		bool all_fav = ns > 0;
		for (int i = 0; g_lib && i < g_lib->photos.len; i++)
			if (g_lib->photos.items[i].sel && !g_lib->photos.items[i].fav) all_fav = false;
		AppBarBtn b[] = {
			{ICON_DELETE, _("delete"), ns == 0},
			{all_fav ? ICON_STAR_FILLED : ICON_STAR, all_fav ? _("unfavorite") : _("favorite"), ns == 0},
		};
		const char *menu[] = {_("select all"), _("clear selection")};
		int r = appbar_draw(ui, &g_bar, area, b, 2, menu, 2);
		if (r == 0) {
			char *msg = ns == 1 ? xstrdup(_("1 picture will be moved to the trash.")) :
				xasprintf(_("%d pictures will be moved to the trash."), ns);
			dialog_message(_("delete pictures?"), msg, _("delete"), _("cancel"), delete_sel_cb, NULL);
			free(msg);
		} else if (r == 1) {
			for (int i = 0; i < g_lib->photos.len; i++)
				if (g_lib->photos.items[i].sel) {
					char *p = xstrdup(g_lib->photos.items[i].path);
					fav_set(p, !all_fav);
					free(p);
				}
			select_set(false, 0);
		} else if (r == APPBAR_MENU) {
			int n = seq_build(src, album, &g_tmp, &g_tmp_cap);
			for (int k = 0; k < n; k++) g_lib->photos.items[g_tmp[k]].sel = true;
		} else if (r == APPBAR_MENU + 1) {
			for (int i = 0; i < g_lib->photos.len; i++) g_lib->photos.items[i].sel = false;
		}
		return;
	}
	int n = g_lib ? seq_build(src, album, &g_tmp, &g_tmp_cap) : 0;
	AppBarBtn b[] = {
		{ICON_SELECT, _("select"), !can_select || n == 0},
		{ICON_SLIDESHOW, _("slideshow"), n == 0},
		{ICON_CAMERA, _("camera"), false},
	};
	const char *menu_root[] = {_("refresh")};
	const char *menu_album[] = {_("refresh"), _("open folder")};
	bool is_album = src == SRC_ALBUM;
	int r = appbar_draw(ui, &g_bar, area, b, 3, is_album ? menu_album : menu_root, is_album ? 2 : 1);
	if (r == 0) select_set(true, is_album ? -1 : g_pivot.target);
	else if (r == 1 && n) viewer_open(ui, src, g_lib->photos.items[g_tmp[0]].path, true);
	else if (r == 2) shell_launch("b:camera");
	else if (r == APPBAR_MENU) scan_start(true);
	else if (r == APPBAR_MENU + 1 && album >= 0) {
		const char *p = g_lib->albums.items[album].dir;
		char d[512];
		if (!p && g_lib->albums.items[album].cover >= 0) { dir_of(g_lib->photos.items[g_lib->albums.items[album].cover].path, d, sizeof d); p = d; }
		if (p) open_uri(p);
	}
}

static void root_draw(UI *ui, Rect area) {
	if (g_pick.active) {
		const char *titles[] = {_("collection"), _("albums")};
		app_pivot(ui, "ph-pick-pivot", &g_pick_pivot, area, _("choose a picture"), titles, 2, root_page, NULL);
		return;
	}
	Rect content = appbar_content(&g_bar, area);
	const char *titles[] = {_("collection"), _("albums"), _("favorites")};
	app_pivot(ui, "ph-pivot", &g_pivot, content, _("Photos"), titles, 3, root_page, NULL);
	if (g_select && g_select_page != g_pivot.target) select_set(false, 0);
	int src = g_pivot.target == 2 ? SRC_FAV : SRC_ALL;
	grid_appbar(ui, area, src, -1, g_pivot.target != 1);
}

static void album_draw(UI *ui, Rect area) {
	int al = album_current();
	bool pick = g_pick.active;
	Rect content = pick ? area : appbar_content(&g_bar, area);
	const char *title = al >= 0 ? album_title(&g_lib->albums.items[al]) : "";
	double y = ui_page_header(ui, 14, content.y + 6, pick ? _("choose a picture") : _("Photos"), title);
	Rect view = R(content.x, y + 4, content.w, rect_bottom(content) - y - 4);
	if (al < 0) app_empty(ui, view, _("this album is empty."));
	else grid_draw(ui, view, "ph-alb", &g_album_sc, SRC_ALBUM, al, false, "");
	if (!pick) grid_appbar(ui, area, SRC_ALBUM, al, al >= 0);
}

static void page_draw(UI *ui, int page, Rect area, void *ud) {
	switch (page) {
	case PG_ROOT: root_draw(ui, area); break;
	case PG_ALBUM: album_draw(ui, area); break;
	case PG_VIEWER: viewer_draw(ui, area); break;
	}
}

static void photos_draw(UI *ui, Rect area) {
	if (!g_nav_ready) { nav_init(&g_nav, PG_ROOT); g_nav_ready = true; }
	if (!g_lib && !g_scanning) scan_start(false);
	bool busy = nav_busy(&g_nav, ui);
	nav_draw(ui, &g_nav, area, page_draw, NULL);
	/* touches are resolved against the hit areas of the frame before the last one: after
	 * a page transition (drawn with input blocked) or a layout change (app bar shown,
	 * overlay opened...) draw two settled frames so the next touch lands correctly */
	static int settle;
	static unsigned last_sig;
	unsigned sig = (unsigned)nav_page(&g_nav) | V.chrome << 3 | V.info << 4 | V.slideshow << 5 | g_select << 6 |
		V.bar.open << 7 | g_bar.open << 8 | (V.zoom > 1.01) << 9 | g_pick.active << 10 | (g_lib != NULL) << 11;
	if (busy || sig != last_sig) settle = 2;
	if (!busy && settle > 0) { settle--; ui->animating = true; }
	last_sig = sig;
}

/* opens the viewer on a file (deep link from Files, search...) */
static void photos_open_path(const char *path) {
	if (!g_nav_ready) { nav_init(&g_nav, PG_ROOT); g_nav_ready = true; }
	if (!g_lib) { free(g_pending_open); g_pending_open = xstrdup(path); return; }
	int idx = photo_find(path);
	viewer_open(&sh.ui, SRC_ALL, idx >= 0 ? g_lib->photos.items[idx].path : path, false);
}

static void photos_open(const char *arg) {
	g_active = true;
	favs_load();
	if (!g_thumbs) g_thumbs = imgcache_new(96);
	if (!g_view) g_view = imgcache_new(4);
	if (!g_nav_ready) { nav_init(&g_nav, PG_ROOT); g_nav_ready = true; }
	scan_start(false);
	if (arg && str_eq(arg, "pick")) {
		slideshow_stop();
		select_set(false, 0);
		nav_init(&g_nav, PG_ROOT);
		g_pick_pivot = (Pager){0};
		g_bar.open = false;
	} else if (arg && arg[0] == '/' && file_exists(arg)) {
		photos_open_path(arg);
	}
}

static bool photos_back(void) {
	if (!g_nav_ready) return false;
	int page = nav_page(&g_nav);
	if (page == PG_VIEWER) {
		if (V.info) { V.info = false; return true; }
		if (V.slideshow) { slideshow_stop(); V.chrome = true; return true; }
		if (appbar_back(&V.bar)) return true;
		if (V.zoom > 1.01) { zoom_reset(); return true; }
		slideshow_stop();
		V.tap_pending = false;
		nav_pop(&g_nav, &sh.ui);
		if (g_view) imgcache_clear(g_view);
		free(V.ext);
		V.ext = NULL;
		return true;
	}
	if (appbar_back(&g_bar)) return true;
	if (g_select) { select_set(false, 0); return true; }
	if (nav_pop(&g_nav, &sh.ui)) return true;
	if (g_pick.active) { pick_finish(NULL); return true; }
	return false;
}

static void photos_close(void) {
	g_active = false;
	slideshow_stop();
	V.tap_pending = false;
	if (g_pick.active) {
		void (*cb)(const char *, void *) = g_pick.cb;
		g_pick.active = false;
		g_pick.cb = NULL;
		if (cb) cb(NULL, g_pick.ud);
	}
	if (g_nav_ready) nav_init(&g_nav, PG_ROOT);
	g_select = false;
	g_bar = (AppBar){0};
	free(V.seq);
	V.seq = NULL;
	V.n = V.cap = 0;
	free(V.ext);
	V.ext = NULL;
	free(g_tmp);
	g_tmp = NULL;
	g_tmp_cap = 0;
	if (!g_scanning) { lib_free(g_lib); g_lib = NULL; }
	if (g_thumbs) imgcache_clear(g_thumbs);
	if (g_view) imgcache_clear(g_view);
	for (int i = 0; i < g_favs.len; i++) free(g_favs.items[i]);
	vec_free(&g_favs);
	g_favs_loaded = false;
	for (int i = 0; i < 3; i++) g_sc[i] = (Scroll){0};
	g_pivot = (Pager){0};
}

/* -------------------------------------------------------------- live tile */

static Timer *g_tile_timer;
static bool g_tile_armed;

static void tile_tick(void *ud) {
	g_tile_armed = false;
	shell_invalidate();
}

static void tile_img(UI *ui, Rect r, int i) {
	Image *img = g_nrecent ? imgcache_get(g_tile_cache, g_recent[i % g_nrecent], r.w * ui->scale > 180 ? 320 : 160) : NULL;
	if (img) gfx_image_cover(ui->cr, img, r, 1);
}

static void tile_face(UI *ui, Rect r, int face, void *ud) {
	TileSize size = (TileSize)(intptr_t)ud;
	if (size == TILE_WIDE && g_nrecent >= 5) {
		/* one large picture with a 2x2 mosaic beside it */
		double big = MIN(r.h, r.w / 2), sm = big / 2;
		tile_img(ui, R(r.x, r.y, big, big), face * 5);
		for (int k = 0; k < 4; k++)
			tile_img(ui, R(r.x + big + (k % 2) * sm, r.y + (k / 2) * sm, sm, sm), face * 5 + 1 + k);
	} else {
		tile_img(ui, r, face);
	}
}

static void photos_tile(UI *ui, Rect r, TileSize size, double now) {
	if (!g_nrecent && !g_scan_tried) scan_start(false);
	if (!g_nrecent) {
		icon_draw(ui->cr, ICON_PHOTO, rect_cx(r), r.y + r.h * 0.46, MIN(r.h * 0.42, 70) * 1.1, COL_WHITE);
		app_tile_name(ui, r, _("Photos"));
		return;
	}
	if (!g_tile_cache) g_tile_cache = imgcache_new(12);
	int faces = size == TILE_WIDE && g_nrecent >= 5 ? MAX(1, g_nrecent / 5) + (g_nrecent >= 6) : MIN(g_nrecent, 6);
	const double period = 8;
	const char *key = size == TILE_WIDE ? "photos-tile-w" : "photos-tile-m";
	app_tile_faces(ui, r, key, faces, period, tile_face, (void *)(intptr_t)size);
	app_tile_name(ui, r, _("Photos"));
	if (faces > 1 && cfg.animations && !g_tile_armed) {
		double stagger = (ui_id(key) % 997) / 997.0 * period;
		double left = period - fmod(ui->now + stagger, period);
		if (!g_tile_timer) g_tile_timer = timer_add(0, false, tile_tick, NULL);
		timer_set(g_tile_timer, (int)(left * 1000) + 40);
		g_tile_armed = true;
	}
}

const AppDef app_photos = {
	.id = "photos", .name = N_("Photos"), .icon = ICON_PHOTO, .color = 0,
	.keywords = "pictures images gallery camera roll screenshots albums",
	.open = photos_open, .draw = photos_draw, .back = photos_back, .close = photos_close,
	.tile = photos_tile, .live = true,
};

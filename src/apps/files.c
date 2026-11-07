/* Files: browse folders and drives, open files, copy / move / rename / delete
 * (the Windows Phone 8.1 "Files" app). Directory reads run on a worker thread,
 * copy / move / delete run as child processes so the compositor never blocks. */
#include "apps.h"
#include "terminal.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <grp.h>
#include <math.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ENTRIES 5000
#define MAX_LV 48
#define ROW_H 72.0
#define TILE 52.0

/* ------------------------------------------------------------ file types -- */

enum { K_FOLDER, K_IMAGE, K_MUSIC, K_VIDEO, K_DOC, K_SHEET, K_SLIDES, K_PDF, K_TEXT, K_CODE, K_ARCHIVE, K_APP, K_OTHER, K_COUNT };

static const struct { Icon icon; uint32_t color; const char *name; } KINDS[K_COUNT] = {
	[K_FOLDER] = {ICON_FOLDER, 0, N_("folder")},
	[K_IMAGE] = {ICON_PHOTO, 0xD80073, N_("picture")},
	[K_MUSIC] = {ICON_MUSIC, 0xFA6800, N_("music")},
	[K_VIDEO] = {ICON_VIDEO, 0x6A00FF, N_("video")},
	[K_DOC] = {ICON_NOTE, 0x2B579A, N_("document")},
	[K_SHEET] = {ICON_TILES, 0x217346, N_("spreadsheet")},
	[K_SLIDES] = {ICON_SLIDESHOW, 0xD24726, N_("presentation")},
	[K_PDF] = {ICON_NOTE, 0xA4373A, N_("PDF document")},
	[K_TEXT] = {ICON_NOTE, 0x647687, N_("text file")},
	[K_CODE] = {ICON_TERMINAL, 0x008A00, N_("source code")},
	[K_ARCHIVE] = {ICON_STORAGE, 0x825A2C, N_("archive")},
	[K_APP] = {ICON_APPS, 0x1BA1E2, N_("program")},
	[K_OTHER] = {ICON_FILE, 0x76608A, N_("file")},
};

static const struct { const char *ext; int kind; } EXTS[] = {
	{"jpg", K_IMAGE}, {"jpeg", K_IMAGE}, {"png", K_IMAGE}, {"gif", K_IMAGE}, {"bmp", K_IMAGE}, {"webp", K_IMAGE},
	{"svg", K_IMAGE}, {"tif", K_IMAGE}, {"tiff", K_IMAGE}, {"heic", K_IMAGE}, {"heif", K_IMAGE}, {"avif", K_IMAGE},
	{"ico", K_IMAGE}, {"xcf", K_IMAGE}, {"raw", K_IMAGE}, {"cr2", K_IMAGE}, {"nef", K_IMAGE}, {"dng", K_IMAGE},
	{"mp3", K_MUSIC}, {"flac", K_MUSIC}, {"ogg", K_MUSIC}, {"oga", K_MUSIC}, {"opus", K_MUSIC}, {"wav", K_MUSIC},
	{"m4a", K_MUSIC}, {"aac", K_MUSIC}, {"wma", K_MUSIC}, {"aiff", K_MUSIC}, {"mid", K_MUSIC}, {"midi", K_MUSIC},
	{"mp4", K_VIDEO}, {"mkv", K_VIDEO}, {"webm", K_VIDEO}, {"avi", K_VIDEO}, {"mov", K_VIDEO}, {"wmv", K_VIDEO},
	{"m4v", K_VIDEO}, {"mpg", K_VIDEO}, {"mpeg", K_VIDEO}, {"3gp", K_VIDEO}, {"ogv", K_VIDEO}, {"flv", K_VIDEO},
	{"pdf", K_PDF}, {"doc", K_DOC}, {"docx", K_DOC}, {"odt", K_DOC}, {"rtf", K_DOC}, {"xls", K_SHEET},
	{"xlsx", K_SHEET}, {"ods", K_SHEET}, {"ppt", K_SLIDES}, {"pptx", K_SLIDES}, {"odp", K_SLIDES}, {"epub", K_DOC},
	{"djvu", K_PDF}, {"pages", K_DOC}, {"key", K_SLIDES}, {"numbers", K_SHEET}, {"xps", K_PDF}, {"oxps", K_PDF},
	{"txt", K_TEXT}, {"md", K_TEXT}, {"log", K_TEXT}, {"csv", K_TEXT}, {"nfo", K_TEXT}, {"srt", K_TEXT},
	{"ini", K_TEXT}, {"conf", K_TEXT}, {"cfg", K_TEXT}, {"rst", K_TEXT}, {"tex", K_TEXT},
	{"c", K_CODE}, {"h", K_CODE}, {"cpp", K_CODE}, {"hpp", K_CODE}, {"cc", K_CODE}, {"py", K_CODE},
	{"js", K_CODE}, {"ts", K_CODE}, {"json", K_CODE}, {"xml", K_CODE}, {"html", K_CODE}, {"htm", K_CODE},
	{"css", K_CODE}, {"sh", K_CODE}, {"rs", K_CODE}, {"go", K_CODE}, {"java", K_CODE}, {"kt", K_CODE},
	{"yml", K_CODE}, {"yaml", K_CODE}, {"toml", K_CODE}, {"lua", K_CODE}, {"rb", K_CODE}, {"php", K_CODE},
	{"pl", K_CODE}, {"cs", K_CODE}, {"swift", K_CODE}, {"vala", K_CODE}, {"sql", K_CODE}, {"build", K_CODE},
	{"zip", K_ARCHIVE}, {"tar", K_ARCHIVE}, {"gz", K_ARCHIVE}, {"tgz", K_ARCHIVE}, {"bz2", K_ARCHIVE},
	{"xz", K_ARCHIVE}, {"7z", K_ARCHIVE}, {"rar", K_ARCHIVE}, {"zst", K_ARCHIVE}, {"lz", K_ARCHIVE},
	{"iso", K_ARCHIVE}, {"img", K_ARCHIVE}, {"cab", K_ARCHIVE},
	{"deb", K_APP}, {"rpm", K_APP}, {"apk", K_APP}, {"appimage", K_APP}, {"flatpak", K_APP},
	{"flatpakref", K_APP}, {"desktop", K_APP}, {"exe", K_APP}, {"msi", K_APP}, {"jar", K_APP},
};

static int kind_for(const char *name, mode_t mode) {
	if (S_ISDIR(mode)) return K_FOLDER;
	const char *dot = strrchr(name, '.');
	if (dot && dot != name && dot[1]) {
		char ext[16];
		size_t n = strlen(dot + 1);
		if (n < sizeof ext) {
			for (size_t i = 0; i <= n; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
			for (size_t i = 0; i < ARRAY_LEN(EXTS); i++)
				if (!strcmp(EXTS[i].ext, ext)) return EXTS[i].kind;
		}
	}
	if (S_ISREG(mode) && (mode & 0111)) return K_APP;
	return K_OTHER;
}

static Color kind_color(const UI *ui, int k) {
	return KINDS[k].color ? RGBX(KINDS[k].color) : ui->th.accent;
}

/* ------------------------------------------------------------------ data -- */

typedef struct {
	char *name, *disp, *fold;    /* raw name, valid UTF-8 display name, folded for search/sort */
	int64_t size;
	time_t mtime;
	mode_t mode;
	int kind;
	int count, count_all;        /* folder items (visible / including hidden), -1 unknown */
	bool dir, link, hidden;
} Entry;

typedef enum { LV_ROOT, LV_DIR, LV_PROPS } LvKind;

typedef struct {
	char *name, *path;
	Icon icon;
	bool std;                    /* name is an English key, translate when drawing */
	bool drive;
	int count;
	double free, total;
} Loc;

typedef struct {
	LvKind kind;
	char *path;
	char *loc_name, *loc_root;   /* location the browse started from (page header) */
	bool loc_std;
	uint32_t token;
	/* LV_DIR */
	Entry *e;
	int n;
	int *view, nview;
	bool *sel;
	int nsel;
	int err;
	bool loaded, loading, truncated, reload;
	double load_t0;
	Scroll scroll;
	char *highlight;
	double highlight_t;
	/* LV_PROPS */
	struct stat st;
	bool st_ok;
	double du_size, content_h;
	long du_items;
	int du_state;                /* 0 idle, 1 running, 2 done, 3 failed */
	char *link_target;
} Level;

static Level *g_lv[MAX_LV];
static int g_depth;
static Nav g_nav;
static AppBar g_bar;
static uint32_t g_token_seq;
static bool g_inited;

static int g_sort;                     /* SORT_* */
static bool g_hidden;
enum { SORT_NAME, SORT_DATE, SORT_SIZE, SORT_TYPE, SORT_COUNT };
static const char *SORT_NAMES[SORT_COUNT] = {N_("name"), N_("date"), N_("size"), N_("type")};

static bool g_select;
static double g_sel_anim;
static bool g_search_on, g_search_focus;
static char g_filter[128];

static Loc g_locs[40];
static int g_nlocs;
static double g_dev_free, g_dev_total;
static bool g_root_loading, g_root_loaded;

static ImgCache *g_thumbs;

static struct { char **paths; int n; bool cut; } g_clip;

static char *g_pending;                /* path a dialog / context menu works on */

/* ------------------------------------------------------------- settings -- */

static int cfg_get(const char *k, int def) {
	cJSON *v = cJSON_GetObjectItemCaseSensitive(config_section("files", false), k);
	if (cJSON_IsNumber(v)) return v->valueint;
	if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
	return def;
}

static void cfg_set(const char *k, int v) {
	cJSON *s = config_section("files", false);
	cJSON_DeleteItemFromObjectCaseSensitive(s, k);
	cJSON_AddNumberToObject(s, k, v);
	config_save_soon();
}

/* ------------------------------------------------------------- locations - */

/* XDG user directory (honours ~/.config/user-dirs.dirs, so localised folder names work) */
static char *user_dir(const char *key, const char *fallback) {
	const char *xch = getenv("XDG_CONFIG_HOME");
	char *cfgdir = xch && *xch ? xstrdup(xch) : path_join(home_dir(), ".config");
	char *file = path_join(cfgdir, "user-dirs.dirs");
	free(cfgdir);
	char *txt = file_read_all(file, NULL);
	free(file);
	char *res = NULL;
	if (txt) {
		char want[64];
		snprintf(want, sizeof want, "XDG_%s_DIR=", key);
		for (char *line = strtok(txt, "\n"); line && !res; line = strtok(NULL, "\n")) {
			line = str_trim(line);
			if (!str_starts(line, want)) continue;
			char *v = line + strlen(want);
			if (*v == '"') v++;
			char *q = strchr(v, '"');
			if (q) *q = 0;
			if (str_starts(v, "$HOME")) res = xasprintf("%s%s", home_dir(), v + 5);
			else if (*v == '/') res = xstrdup(v);
			/* XDG_x_DIR="$HOME/" means "disabled" */
			if (res && str_eq(res, home_dir())) { free(res); res = NULL; line = NULL; break; }
		}
		free(txt);
	}
	return res ? res : path_join(home_dir(), fallback);
}

static void unescape_mount(char *s) {
	char *w = s;
	for (char *r = s; *r;) {
		if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] && r[3]) {
			*w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
			r += 4;
		} else *w++ = *r++;
	}
	*w = 0;
}

/* Mounted removable / extra drives (mount points under /media, /run/media and /mnt). */
static int list_drives(char paths[][256], int max) {
	char *txt = file_read_all("/proc/self/mounts", NULL);
	if (!txt) return 0;
	int n = 0;
	for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save)) {
		char dev[256], mp[256], fs[64];
		if (sscanf(line, "%255s %255s %63s", dev, mp, fs) != 3) continue;
		unescape_mount(mp);
		if (!str_starts(mp, "/media/") && !str_starts(mp, "/run/media/") && !str_starts(mp, "/mnt/") && !str_eq(mp, "/mnt"))
			continue;
		if (str_eq(fs, "autofs") || str_eq(fs, "proc") || str_eq(fs, "sysfs")) continue;
		bool dup = false;
		for (int i = 0; i < n; i++) dup |= str_eq(paths[i], mp);
		if (!dup) str_copy(paths[n++], 256, mp);
	}
	free(txt);
	return n;
}

static const struct { const char *key, *fallback, *name; Icon icon; } STD_LOCS[] = {
	{NULL, "", N_("Home"), ICON_HOME},
	{"DOCUMENTS", "Documents", N_("Documents"), ICON_NOTE},
	{"DOWNLOAD", "Downloads", N_("Downloads"), ICON_DOWNLOAD},
	{"PICTURES", "Pictures", N_("Pictures"), ICON_PHOTO},
	{"MUSIC", "Music", N_("Music"), ICON_MUSIC},
	{"VIDEOS", "Videos", N_("Videos"), ICON_VIDEO},
	{"DESKTOP", "Desktop", N_("Desktop"), ICON_TILES},
};

/* Which location a path belongs to (longest matching root). Cheap: no blocking IO. */
static void loc_for_path(const char *path, char **name, bool *std, char **root) {
	char drives[16][256];
	int nd = list_drives(drives, 16);
	size_t best = 0;
	*name = xstrdup(N_("File system"));
	*root = xstrdup("/");
	*std = true;
	for (size_t i = 0; i < ARRAY_LEN(STD_LOCS) + (size_t)nd; i++) {
		char *p;
		if (i < ARRAY_LEN(STD_LOCS)) p = STD_LOCS[i].key ? user_dir(STD_LOCS[i].key, STD_LOCS[i].fallback) : xstrdup(home_dir());
		else p = xstrdup(drives[i - ARRAY_LEN(STD_LOCS)]);
		size_t l = strlen(p);
		if (l > best && !strncmp(path, p, l) && (path[l] == 0 || path[l] == '/')) {
			best = l;
			free(*name);
			free(*root);
			*std = i < ARRAY_LEN(STD_LOCS);
			*name = xstrdup(*std ? STD_LOCS[i].name : path_basename(p));
			*root = xstrdup(p);
		}
		free(p);
	}
}

static int count_dir(int dfd, const char *name, int *all, int cap) {
	int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) { *all = -1; return -1; }
	DIR *d = fdopendir(fd);
	if (!d) { close(fd); *all = -1; return -1; }
	int vis = 0, n = 0;
	struct dirent *de;
	while ((de = readdir(d)) && n < cap) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		n++;
		if (de->d_name[0] != '.') vis++;
	}
	closedir(d);
	*all = n;
	return vis;
}

typedef struct { Loc l[40]; int n; double free, total; } RootJob;

static void root_run(void *arg) {
	RootJob *j = arg;
	for (size_t i = 0; i < ARRAY_LEN(STD_LOCS); i++) {
		char *p = STD_LOCS[i].key ? user_dir(STD_LOCS[i].key, STD_LOCS[i].fallback) : xstrdup(home_dir());
		bool dup = false;
		for (int k = 0; k < j->n; k++) dup |= str_eq(j->l[k].path, p);
		if (dup || !is_dir(p)) { free(p); continue; }
		Loc *l = &j->l[j->n++];
		*l = (Loc){.name = xstrdup(STD_LOCS[i].name), .path = p, .icon = STD_LOCS[i].icon, .std = true, .count = -1};
		int all;
		l->count = count_dir(AT_FDCWD, p, &all, 100000);
	}
	j->l[j->n++] = (Loc){.name = xstrdup(N_("File system")), .path = xstrdup("/"), .icon = ICON_STORAGE, .std = true,
	                     .count = -1};
	struct statvfs vs;
	if (statvfs(home_dir(), &vs) == 0) {
		j->free = (double)vs.f_bavail * vs.f_frsize;
		j->total = (double)vs.f_blocks * vs.f_frsize;
	}
	char drives[24][256];
	int nd = list_drives(drives, 24);
	for (int i = 0; i < nd && j->n < 40; i++) {
		Loc *l = &j->l[j->n++];
		*l = (Loc){.name = xstrdup(path_basename(drives[i])), .path = xstrdup(drives[i]), .icon = ICON_USB, .drive = true,
		           .count = -1};
		if (statvfs(drives[i], &vs) == 0) {
			l->free = (double)vs.f_bavail * vs.f_frsize;
			l->total = (double)vs.f_blocks * vs.f_frsize;
		}
	}
}

static void locs_free(Loc *l, int n) {
	for (int i = 0; i < n; i++) { free(l[i].name); free(l[i].path); }
}

static void root_done(void *arg) {
	RootJob *j = arg;
	g_root_loading = false;
	if (g_inited) {
		locs_free(g_locs, g_nlocs);
		memcpy(g_locs, j->l, sizeof(Loc) * (size_t)j->n);
		g_nlocs = j->n;
		g_dev_free = j->free;
		g_dev_total = j->total;
		g_root_loaded = true;
	} else locs_free(j->l, j->n);
	free(j);
	shell_invalidate();
}

static void root_refresh(void) {
	if (g_root_loading) return;
	g_root_loading = true;
	job_submit(root_run, root_done, xcalloc(1, sizeof(RootJob)));
}

/* ------------------------------------------------------- reading folders -- */

static void entry_free(Entry *e) {
	if (e->disp != e->name) free(e->disp);
	free(e->name);
	free(e->fold);
}

typedef struct {
	char *path;
	uint32_t token;
	Entry *e;
	int n, cap, err;
	bool truncated;
} ReadJob;

static void read_run(void *arg) {
	ReadJob *j = arg;
	int fd = open(j->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) { j->err = errno; return; }
	DIR *d = fdopendir(fd);
	if (!d) { j->err = errno; close(fd); return; }
	struct dirent *de;
	int ndirs = 0;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		if (j->n >= MAX_ENTRIES) { j->truncated = true; break; }
		struct stat st;
		if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
		bool link = S_ISLNK(st.st_mode);
		if (link) {
			struct stat t;
			if (fstatat(fd, de->d_name, &t, 0) == 0) st = t;
		}
		if (j->n >= j->cap) {
			j->cap = j->cap ? j->cap * 2 : 64;
			j->e = xrealloc(j->e, sizeof(Entry) * (size_t)j->cap);
		}
		Entry *e = &j->e[j->n++];
		memset(e, 0, sizeof *e);
		e->name = xstrdup(de->d_name);
		e->disp = g_utf8_validate(e->name, -1, NULL) ? e->name : g_utf8_make_valid(e->name, -1);
		if (e->disp != e->name) {   /* glib allocation -> malloc for entry_free */
			char *c = xstrdup(e->disp);
			g_free(e->disp);
			e->disp = c;
		}
		e->fold = str_fold(e->disp);
		e->dir = S_ISDIR(st.st_mode);
		e->link = link;
		e->hidden = de->d_name[0] == '.';
		e->size = e->dir ? 0 : (int64_t)st.st_size;
		e->mtime = st.st_mtime;
		e->mode = st.st_mode;
		e->kind = kind_for(e->name, st.st_mode);
		e->count = e->count_all = -1;
		if (e->dir) ndirs++;
	}
	/* item counts of subfolders (bounded, so huge trees stay fast) */
	if (ndirs <= 400) {
		for (int i = 0; i < j->n; i++)
			if (j->e[i].dir) j->e[i].count = count_dir(fd, j->e[i].name, &j->e[i].count_all, 10000);
	}
	closedir(d);
}

static Level *level_find(uint32_t token) {
	for (int i = 0; i < MAX_LV; i++)
		if (g_lv[i] && g_lv[i]->token == token) return g_lv[i];
	return NULL;
}

static int g_cmp_sort;
static Entry *g_cmp_e;

static int natcmp(const char *a, const char *b) {
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0' && isdigit((unsigned char)a[1])) a++;
			while (*b == '0' && isdigit((unsigned char)b[1])) b++;
			const char *ae = a, *be = b;
			while (isdigit((unsigned char)*ae)) ae++;
			while (isdigit((unsigned char)*be)) be++;
			if (ae - a != be - b) return ae - a < be - b ? -1 : 1;
			int c = strncmp(a, b, (size_t)(ae - a));
			if (c) return c;
			a = ae;
			b = be;
			continue;
		}
		if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		a++;
		b++;
	}
	return *a ? 1 : *b ? -1 : 0;
}

static const char *ext_of(const Entry *e) {
	const char *d = strrchr(e->fold, '.');
	return d && d != e->fold ? d + 1 : "";
}

static int entry_cmp(const void *pa, const void *pb) {
	const Entry *a = &g_cmp_e[*(const int *)pa], *b = &g_cmp_e[*(const int *)pb];
	if (a->dir != b->dir) return a->dir ? -1 : 1;
	int c = 0;
	switch (g_cmp_sort) {
	case SORT_DATE: c = a->mtime > b->mtime ? -1 : a->mtime < b->mtime ? 1 : 0; break;
	case SORT_SIZE: c = a->dir ? 0 : a->size > b->size ? -1 : a->size < b->size ? 1 : 0; break;
	case SORT_TYPE: c = a->dir ? 0 : strcmp(ext_of(a), ext_of(b)); break;
	default: break;
	}
	if (!c) c = natcmp(a->fold, b->fold);
	if (!c) c = strcmp(a->name, b->name);
	return c;
}

static void level_rebuild_view(Level *l) {
	if (l->kind != LV_DIR) return;
	free(l->view);
	l->view = xmalloc(sizeof(int) * (size_t)(l->n + 1));
	l->nview = 0;
	bool cur = g_lv[g_depth] == l;
	char *f = cur && g_search_on && g_filter[0] ? str_fold(g_filter) : NULL;
	for (int i = 0; i < l->n; i++) {
		if (l->e[i].hidden && !g_hidden) continue;
		if (f && !strstr(l->e[i].fold, f)) continue;
		l->view[l->nview++] = i;
	}
	free(f);
	g_cmp_sort = g_sort;
	g_cmp_e = l->e;
	qsort(l->view, (size_t)l->nview, sizeof(int), entry_cmp);
}

static void level_clear_sel(Level *l) {
	free(l->sel);
	l->sel = NULL;
	l->nsel = 0;
}

static void level_load(Level *l);

static void read_done(void *arg) {
	ReadJob *j = arg;
	Level *l = level_find(j->token);
	if (l && l->kind == LV_DIR) {
		for (int i = 0; i < l->n; i++) entry_free(&l->e[i]);
		free(l->e);
		l->e = j->e;
		l->n = j->n;
		l->err = j->err;
		l->truncated = j->truncated;
		l->loaded = true;
		l->loading = false;
		j->e = NULL;
		j->n = 0;
		level_clear_sel(l);
		level_rebuild_view(l);
		if (l->highlight) {
			for (int k = 0; k < l->nview; k++) {
				if (str_eq(l->e[l->view[k]].name, l->highlight)) {
					l->scroll.off = MAX(0, k * ROW_H - ROW_H * 1.5);
					l->highlight_t = now_sec();
					break;
				}
			}
		}
		/* a refresh was requested while this read ran */
		if (l->reload) { l->reload = false; level_load(l); }
	}
	for (int i = 0; i < j->n; i++) entry_free(&j->e[i]);
	free(j->e);
	free(j->path);
	free(j);
	shell_invalidate();
}

static void level_load(Level *l) {
	if (l->kind != LV_DIR) return;
	if (l->loading) { l->reload = true; return; }
	l->loading = true;
	l->load_t0 = now_sec();
	ReadJob *j = xcalloc(1, sizeof *j);
	j->path = xstrdup(l->path);
	j->token = l->token;
	job_submit(read_run, read_done, j);
}

static void refresh_all(void) {
	for (int i = 0; i <= g_depth; i++)
		if (g_lv[i] && g_lv[i]->kind == LV_DIR) level_load(g_lv[i]);
	root_refresh();
}

/* --------------------------------------------------------------- levels -- */

static void level_free(Level *l) {
	if (!l) return;
	for (int i = 0; i < l->n; i++) entry_free(&l->e[i]);
	free(l->e);
	free(l->view);
	free(l->sel);
	free(l->path);
	free(l->loc_name);
	free(l->loc_root);
	free(l->highlight);
	free(l->link_target);
	free(l);
}

static Level *level_new(LvKind k, const char *path, const char *loc_name, bool loc_std, const char *loc_root) {
	Level *l = xcalloc(1, sizeof *l);
	l->kind = k;
	l->token = ++g_token_seq;
	l->path = path ? xstrdup(path) : NULL;
	l->loc_name = xstrdup(loc_name ? loc_name : "");
	l->loc_std = loc_std;
	l->loc_root = xstrdup(loc_root ? loc_root : "/");
	return l;
}

static void reset_page_state(void) {
	g_select = false;
	g_sel_anim = 0;
	g_search_on = false;
	g_filter[0] = 0;
	g_bar.open = false;
	sh.ui.focus = 0;
}

static void go_push(Level *l) {
	if (g_depth + 1 >= MAX_LV) { level_free(l); return; }
	Level *cur = g_lv[g_depth];
	if (cur && cur->kind == LV_DIR) level_clear_sel(cur);
	bool rebuild = g_search_on && g_filter[0];
	reset_page_state();
	if (rebuild && cur) level_rebuild_view(cur);
	level_free(g_lv[g_depth + 1]);
	g_lv[++g_depth] = l;
	if (g_nav.depth >= 6) {
		memmove(g_nav.stack, g_nav.stack + 1, sizeof(int) * (size_t)g_nav.depth);
		g_nav.depth--;
	}
	nav_push(&g_nav, &sh.ui, g_depth);
	level_load(l);
}

static bool go_pop(void) {
	if (g_depth == 0) return false;
	Level *cur = g_lv[g_depth];
	if (cur && cur->kind == LV_DIR) level_clear_sel(cur);
	reset_page_state();
	if (g_nav.depth == 0) {
		g_nav.stack[1] = g_nav.stack[0];
		g_nav.stack[0] = g_depth - 1;
		g_nav.depth = 1;
	}
	g_depth--;
	nav_pop(&g_nav, &sh.ui);
	Level *back = g_lv[g_depth];
	if (back && back->kind == LV_DIR) level_rebuild_view(back);
	return true;
}

static void open_dir(const char *path, const char *loc_name, bool loc_std, const char *loc_root) {
	go_push(level_new(LV_DIR, path, loc_name, loc_std, loc_root));
}

/* --------------------------------------------------------------- helpers -- */

static char *parent_of(const char *p) {
	char *d = xstrdup(p);
	char *s = strrchr(d, '/');
	if (s == d) d[1] = 0;
	else if (s) *s = 0;
	return d;
}

static bool lexists(const char *p) {
	struct stat st;
	return lstat(p, &st) == 0;
}

static void fmt_when(char *buf, size_t n, time_t t) {
	struct tm tm, now_tm;
	time_t now = time(NULL);
	localtime_r(&t, &tm);
	localtime_r(&now, &now_tm);
	if (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday) i18n_fmt_time(buf, n, &tm, cfg.h24);
	else i18n_fmt_date_short(buf, n, &tm);
}

static void fmt_count(char *buf, size_t n, long c) {
	if (c == 1) str_copy(buf, n, _("1 item"));
	else snprintf(buf, n, _("%ld items"), c);
}

static void toast(const char *body) { shell_toast(_("Files"), body, _("Files")); }

static bool can_open_files(void) { return proc_exists("xdg-open") || desktop_default_browser(); }

static void open_file(const char *path) {
	if (!can_open_files()) { toast(_("There's no app installed that can open this file.")); return; }
	open_uri(path);
}

static Image *thumb_for(UI *ui, const char *dir, const Entry *e) {
	if (e->kind != K_IMAGE || e->size <= 0 || e->size > (40 << 20)) return NULL;
	if (!g_thumbs) g_thumbs = imgcache_new(48);
	char *p = path_join(dir, e->name);
	Image *img = imgcache_get(g_thumbs, p, (int)ceil(TILE * ui->scale));
	free(p);
	return img;
}

/* ---------------------------------------------------- clipboard and ops -- */

static void clip_clear(void) {
	for (int i = 0; i < g_clip.n; i++) free(g_clip.paths[i]);
	free(g_clip.paths);
	g_clip.paths = NULL;
	g_clip.n = 0;
}

static void clip_set(char **paths, int n, bool cut) {
	clip_clear();
	g_clip.paths = paths;
	g_clip.n = n;
	g_clip.cut = cut;
	char msg[128];
	if (n == 1) str_copy(msg, sizeof msg, cut ? _("1 item cut. Open a folder and tap paste.") : _("1 item copied. Open a folder and tap paste."));
	else snprintf(msg, sizeof msg, cut ? _("%d items cut. Open a folder and tap paste.") : _("%d items copied. Open a folder and tap paste."), n);
	toast(msg);
}

enum { OP_COPY, OP_MOVE, OP_TRASH, OP_DELETE };

typedef struct {
	int kind;
	char **src, **dst;
	int n, i, failed;
	char *highlight;               /* name to highlight in the target folder afterwards */
} Op;

static Op *g_op;

static void op_free(Op *op) {
	for (int i = 0; i < op->n; i++) { free(op->src[i]); if (op->dst) free(op->dst[i]); }
	free(op->src);
	free(op->dst);
	free(op->highlight);
	free(op);
}

static void op_add(Op *op, const char *src, const char *dst) {
	op->src = xrealloc(op->src, sizeof(char *) * (size_t)(op->n + 1));
	op->src[op->n] = xstrdup(src);
	if (dst) {
		op->dst = xrealloc(op->dst, sizeof(char *) * (size_t)(op->n + 1));
		op->dst[op->n] = xstrdup(dst);
	}
	op->n++;
}

static void op_step(void);
static void delete_paths(char **paths, int n, bool permanent);

static void op_finish(void) {
	Op *op = g_op;
	g_op = NULL;
	char msg[160];
	int ok = op->n - op->failed;
	bool batch = op->kind == OP_TRASH || op->kind == OP_DELETE;
	if (batch && op->failed) ok = 0;
	if (op->kind == OP_TRASH && op->failed) {
		/* e.g. no trash on this drive: offer to delete permanently */
		char **left = NULL;
		int nl = 0;
		for (int i = 0; i < op->n; i++) {
			if (!lexists(op->src[i])) continue;
			left = xrealloc(left, sizeof(char *) * (size_t)(nl + 1));
			left[nl++] = xstrdup(op->src[i]);
		}
		if (nl) {
			delete_paths(left, nl, true);
			for (int i = 0; i < nl; i++) free(left[i]);
			free(left);
			op_free(op);
			refresh_all();
			return;
		}
		ok = op->n;
		op->failed = 0;
	}
	if (op->failed && !batch) {
		if (op->failed == 1) str_copy(msg, sizeof msg, _("1 item couldn't be copied or moved."));
		else snprintf(msg, sizeof msg, _("%d items couldn't be copied or moved."), op->failed);
	} else if (op->failed) {
		str_copy(msg, sizeof msg, _("Some items couldn't be deleted."));
	} else {
		const char *one = op->kind == OP_COPY ? _("1 item copied") : op->kind == OP_MOVE ? _("1 item moved")
		                  : op->kind == OP_TRASH ? _("1 item moved to the trash") : _("1 item deleted");
		const char *many = op->kind == OP_COPY ? _("%d items copied") : op->kind == OP_MOVE ? _("%d items moved")
		                   : op->kind == OP_TRASH ? _("%d items moved to the trash") : _("%d items deleted");
		if (ok == 1) str_copy(msg, sizeof msg, one);
		else snprintf(msg, sizeof msg, many, ok);
	}
	toast(msg);
	if (op->highlight && g_lv[g_depth] && g_lv[g_depth]->kind == LV_DIR) {
		free(g_lv[g_depth]->highlight);
		g_lv[g_depth]->highlight = xstrdup(op->highlight);
	}
	op_free(op);
	refresh_all();
}

static void op_proc_done(int status, char *out, size_t len, void *ud) {
	Op *op = g_op;
	if (!op) return;
	bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (op->kind == OP_TRASH || op->kind == OP_DELETE) {
		if (!ok) op->failed = op->n;
		op->i = op->n;
	} else {
		if (!ok) op->failed++;
		op->i++;
	}
	op_step();
	shell_invalidate();
}

static void op_step(void) {
	Op *op = g_op;
	if (op->i >= op->n) { op_finish(); return; }
	bool started;
	if (op->kind == OP_COPY) {
		char *argv[] = {"cp", "-R", "-p", "--", op->src[op->i], op->dst[op->i], NULL};
		started = proc_run(argv, op_proc_done, NULL);
	} else if (op->kind == OP_MOVE) {
		char *argv[] = {"mv", "--", op->src[op->i], op->dst[op->i], NULL};
		started = proc_run(argv, op_proc_done, NULL);
	} else {
		char **argv = xmalloc(sizeof(char *) * (size_t)(op->n + 5));
		int k = 0;
		if (op->kind == OP_TRASH) { argv[k++] = "gio"; argv[k++] = "trash"; }
		else { argv[k++] = "rm"; argv[k++] = "-rf"; }
		argv[k++] = "--";
		for (int i = 0; i < op->n; i++) argv[k++] = op->src[i];
		argv[k] = NULL;
		started = proc_run(argv, op_proc_done, NULL);
		free(argv);
	}
	if (!started) {
		if (op->kind == OP_TRASH || op->kind == OP_DELETE) { op->failed = op->n; op->i = op->n; }
		else { op->failed++; op->i++; }
		op_step();
	}
}

static void op_start(Op *op) {
	g_op = op;
	g_select = false;
	op_step();
	shell_invalidate();
}

/* "name (2).ext" style name that does not exist yet in dir (and is not taken by `taken`). */
static char *unique_dest(const char *dir, const char *name, bool is_folder, char **taken, int ntaken) {
	char *base = xstrdup(name), *ext = xstrdup("");
	const char *dot = is_folder ? NULL : strrchr(name, '.');
	if (dot && dot != name) {
		free(ext);
		ext = xstrdup(dot);
		base[dot - name] = 0;
	}
	char *p = path_join(dir, name);
	for (int k = 2; k < 10000; k++) {
		bool used = lexists(p);
		for (int i = 0; i < ntaken && !used; i++) used = str_eq(taken[i], p);
		if (!used) break;
		free(p);
		char *nm = xasprintf("%s (%d)%s", base, k, ext);
		p = path_join(dir, nm);
		free(nm);
	}
	free(base);
	free(ext);
	return p;
}

static void paste_into(const char *dir) {
	if (g_op) { toast(_("Please wait until the current operation is finished.")); return; }
	if (!g_clip.n) return;
	Op *op = xcalloc(1, sizeof *op);
	op->kind = g_clip.cut ? OP_MOVE : OP_COPY;
	bool into_self = false;
	for (int i = 0; i < g_clip.n; i++) {
		const char *src = g_clip.paths[i];
		if (!lexists(src)) continue;
		size_t sl = strlen(src);
		if (!strncmp(dir, src, sl) && (dir[sl] == 0 || dir[sl] == '/')) { into_self = true; continue; }
		char *par = parent_of(src);
		bool same = str_eq(par, dir);
		free(par);
		if (same && g_clip.cut) continue;
		char *dst = unique_dest(dir, path_basename(src), is_dir(src), op->dst, op->n);
		op_add(op, src, dst);
		if (!op->highlight) op->highlight = xstrdup(path_basename(dst));
		free(dst);
	}
	if (into_self) toast(_("A folder can't be pasted into itself."));
	if (!op->n) { op_free(op); return; }
	if (g_clip.cut) clip_clear();
	op_start(op);
}

static struct { char **paths; int n; bool permanent; } g_del;

static void del_free(void) {
	for (int i = 0; i < g_del.n; i++) free(g_del.paths[i]);
	free(g_del.paths);
	g_del.paths = NULL;
	g_del.n = 0;
}

static void delete_cb(int button, void *ud) {
	if (button == 0 && g_del.n) {
		if (g_op) toast(_("Please wait until the current operation is finished."));
		else {
			Op *op = xcalloc(1, sizeof *op);
			op->kind = g_del.permanent ? OP_DELETE : OP_TRASH;
			for (int i = 0; i < g_del.n; i++) op_add(op, g_del.paths[i], NULL);
			op_start(op);
		}
	}
	del_free();
}

/* Asks for confirmation, then trashes (gio) or deletes permanently. */
static void delete_paths(char **paths, int n, bool permanent) {
	if (!n) return;
	del_free();
	g_del.paths = xmalloc(sizeof(char *) * (size_t)n);
	for (int i = 0; i < n; i++) g_del.paths[i] = xstrdup(paths[i]);
	g_del.n = n;
	g_del.permanent = permanent || !proc_exists("gio");
	char text[640], title[128];
	if (n == 1) {
		bool d = is_dir(paths[0]);
		str_copy(title, sizeof title, d ? _("delete folder?") : _("delete file?"));
		char *disp = g_utf8_make_valid(path_basename(paths[0]), -1);
		snprintf(text, sizeof text, "%s\n%s", disp,
		         g_del.permanent ? (permanent ? _("It couldn't be moved to the trash. Delete it permanently? This can't be undone.")
		                                      : _("It will be deleted permanently. This can't be undone."))
		                         : _("It will be moved to the trash."));
		g_free(disp);
	} else {
		str_copy(title, sizeof title, _("delete items?"));
		snprintf(text, sizeof text,
		         g_del.permanent ? (permanent ? _("%d items couldn't be moved to the trash. Delete them permanently? This can't be undone.")
		                                      : _("%d items will be deleted permanently. This can't be undone."))
		                         : _("%d items will be moved to the trash."), n);
	}
	dialog_message(title, text, _("delete"), _("cancel"), delete_cb, NULL);
}

/* ------------------------------------------------------ rename / mkdir -- */

static bool valid_name(const char *s) {
	if (!*s || str_eq(s, ".") || str_eq(s, "..")) return false;
	if (strchr(s, '/')) { toast(_("A name can't contain \"/\".")); return false; }
	return true;
}

static void rename_cb(int button, void *ud) {
	if (button != 0 || !g_pending) return;
	char name[256];
	str_copy(name, sizeof name, dialog_input_text());
	str_trim(name);
	if (!valid_name(name) || str_eq(name, path_basename(g_pending))) return;
	char *par = parent_of(g_pending);
	char *dst = path_join(par, name);
	if (lexists(dst)) toast(_("There's already an item with that name."));
	else if (rename(g_pending, dst) != 0) toast(_("Couldn't rename the item."));
	else {
		for (int i = 0; i < g_clip.n; i++)
			if (str_eq(g_clip.paths[i], g_pending)) { free(g_clip.paths[i]); g_clip.paths[i] = xstrdup(dst); }
		Level *l = g_lv[g_depth];
		if (l && l->kind == LV_DIR) { free(l->highlight); l->highlight = xstrdup(name); }
		if (l && l->kind == LV_PROPS && str_eq(l->path, g_pending)) { free(l->path); l->path = xstrdup(dst); }
		refresh_all();
	}
	free(par);
	free(dst);
}

static void start_rename(const char *path) {
	free(g_pending);
	g_pending = xstrdup(path);
	char *disp = g_utf8_make_valid(path_basename(path), -1);
	dialog_input(_("rename"), _("name"), disp, 0, rename_cb, NULL);
	g_free(disp);
}

static void mkdir_cb(int button, void *ud) {
	Level *l = g_lv[g_depth];
	if (button != 0 || !l || l->kind != LV_DIR) return;
	char name[256];
	str_copy(name, sizeof name, dialog_input_text());
	str_trim(name);
	if (!valid_name(name)) return;
	char *p = path_join(l->path, name);
	if (lexists(p)) toast(_("There's already an item with that name."));
	else if (mkdir(p, 0755) != 0) toast(errno == EACCES || errno == EPERM ? _("You don't have permission to create a folder here.")
	                                                                        : _("Couldn't create the folder."));
	else {
		free(l->highlight);
		l->highlight = xstrdup(name);
		level_load(l);
	}
	free(p);
}

/* ------------------------------------------------------------ properties - */

static void du_done(int status, char *out, size_t len, void *ud) {
	Level *l = level_find((uint32_t)(uintptr_t)ud);
	if (!l || l->kind != LV_PROPS) return;
	double v = 0;
	if (WIFEXITED(status) && out && sscanf(out, "%lf", &v) == 1) {
		l->du_size = v;
		l->du_state = 2;
	} else if (out && sscanf(out, "%lf", &v) == 1) {   /* partial (unreadable subfolders) */
		l->du_size = v;
		l->du_state = 2;
	} else l->du_state = 3;
	shell_invalidate();
}

static void du_items_done(int status, char *out, size_t len, void *ud) {
	Level *l = level_find((uint32_t)(uintptr_t)ud);
	if (!l || l->kind != LV_PROPS) return;
	long v;
	if (out && sscanf(out, "%ld", &v) == 1) l->du_items = MAX(0, v - 1);
	shell_invalidate();
}

static void open_props(const char *path, const Level *from) {
	Level *l = level_new(LV_PROPS, path, from ? from->loc_name : "", from ? from->loc_std : false,
	                     from ? from->loc_root : "/");
	struct stat st;
	if (lstat(path, &st) == 0) {
		if (S_ISLNK(st.st_mode)) {
			char buf[1024];
			ssize_t n = readlink(path, buf, sizeof buf - 1);
			if (n > 0) { buf[n] = 0; l->link_target = xstrdup(buf); }
			struct stat t;
			if (stat(path, &t) == 0) st = t;
		}
		l->st = st;
		l->st_ok = true;
	}
	l->du_items = -1;
	if (l->st_ok && S_ISDIR(l->st.st_mode) && proc_exists("du")) {
		char *a1[] = {"du", "-s", "-b", "-x", "--", (char *)path, NULL};
		char *a2[] = {"du", "-s", "-x", "--inodes", "--", (char *)path, NULL};
		l->du_state = proc_run(a1, du_done, (void *)(uintptr_t)l->token) ? 1 : 3;
		proc_run(a2, du_items_done, (void *)(uintptr_t)l->token);
	}
	go_push(l);
}

/* ---------------------------------------------------------- context menu - */

enum { A_OPEN, A_TERMINAL, A_RENAME, A_COPY, A_CUT, A_DELETE, A_PROPS, A_PASTE_INTO };
static int g_menu_act[10];

static char **one_path(const char *p) {
	char **v = xmalloc(sizeof(char *));
	v[0] = xstrdup(p);
	return v;
}

static void menu_cb(int idx, void *ud) {
	if (idx < 0 || idx >= 10 || !g_pending) return;
	Level *l = g_lv[g_depth];
	char *p = xstrdup(g_pending);
	switch (g_menu_act[idx]) {
	case A_OPEN:
		if (is_dir(p)) {
			if (l && l->kind == LV_ROOT) {
				for (int i = 0; i < g_nlocs; i++)
					if (str_eq(g_locs[i].path, p)) { open_dir(p, g_locs[i].name, g_locs[i].std, p); break; }
			} else if (l) open_dir(p, l->loc_name, l->loc_std, l->loc_root);
		} else open_file(p);
		break;
	case A_TERMINAL: terminal_open(p); break;
	case A_RENAME: start_rename(p); break;
	case A_COPY: clip_set(one_path(p), 1, false); break;
	case A_CUT: clip_set(one_path(p), 1, true); break;
	case A_DELETE: delete_paths(&p, 1, false); break;
	case A_PROPS: open_props(p, l); break;
	case A_PASTE_INTO: paste_into(p); break;
	}
	free(p);
}

static void open_menu(const char *path, bool folder, bool location, double x, double y) {
	free(g_pending);
	g_pending = xstrdup(path);
	const char *items[10];
	int n = 0;
	static const char *labels[] = {N_("open"), N_("open terminal here"), N_("rename"), N_("copy"), N_("cut"),
		N_("delete"), N_("properties"), N_("paste into folder")};
#define ADD(a) do { g_menu_act[n] = (a); items[n++] = tr(labels[a]); } while (0)
	ADD(A_OPEN);
	if (folder && g_clip.n && !g_op) ADD(A_PASTE_INTO);
	if (folder && terminal_available()) ADD(A_TERMINAL);
	if (!location) {
		ADD(A_RENAME);
		ADD(A_COPY);
		ADD(A_CUT);
		ADD(A_DELETE);
	}
	ADD(A_PROPS);
#undef ADD
	dialog_menu(x, y, items, n, menu_cb, NULL);
}

/* ------------------------------------------------------------- drawing --- */

/* square type tile (or thumbnail) + name + subtitle */
static bool tile_row(UI *ui, UiId id, Rect r, Icon ic, Color tile, Image *thumb, const char *title, const char *sub,
                     double title_size, float alpha, double hl) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	if (pr) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.14f));
	if (hl > 0) gfx_fill_rect(cr, r, col_alpha(th->accent, (float)(0.35 * hl)));
	Rect t = R(r.x + 24, r.y + (r.h - TILE) / 2, TILE, TILE);
	if (thumb) {
		gfx_fill_rect(cr, t, th->chrome2);
		gfx_image_cover(cr, thumb, t, alpha);
	} else {
		gfx_fill_rect(cr, t, col_alpha(tile, alpha));
		icon_draw(cr, ic, rect_cx(t), rect_cy(t), 28, col_alpha(COL_WHITE, alpha));
	}
	double x = t.x + TILE + 14, w = rect_right(r) - x - 16;
	double h1 = gfx_line_height(cr, title_size, FW_SEMILIGHT), h2 = sub && *sub ? gfx_line_height(cr, 15, FW_REGULAR) : 0;
	double y = r.y + (r.h - h1 - h2 + 2) / 2;
	gfx_text(cr, title, x, y, title_size, FW_SEMILIGHT, col_alpha(th->fg, alpha), TA_LEFT, w, 1);
	if (h2) gfx_text(cr, sub, x, y + h1 - 2, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	return ui_clicked(ui, id);
}

static const char *loc_title(const Level *l) {
	return l->loc_std ? tr(l->loc_name) : l->loc_name;
}

/* small header: the device or drive the folder is on */
static const char *device_title(const Level *l) {
	return l->loc_std ? _("this device") : l->loc_name;
}

static const char *level_title(const Level *l) {
	if (str_eq(l->path, l->loc_root)) return loc_title(l);
	return path_basename(l->path);
}

/* "Documents › work › 2026", dropping leading parts that do not fit */
static void draw_crumbs(UI *ui, const Level *l, double x, double y, double w) {
	const char *rel = l->path + strlen(l->loc_root);
	while (*rel == '/') rel++;
	GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(parts, g_strdup(loc_title(l)));
	char **comp = g_strsplit(rel, "/", -1);
	for (int i = 0; comp[i]; i++)
		if (*comp[i]) g_ptr_array_add(parts, g_utf8_make_valid(comp[i], -1));
	g_strfreev(comp);
	for (guint start = 0; start < parts->len; start++) {
		GString *s = g_string_new(start ? "… › " : "");
		for (guint i = start; i < parts->len; i++) {
			if (i > start) g_string_append(s, " › ");
			g_string_append(s, g_ptr_array_index(parts, i));
		}
		bool fits = start + 1 >= parts->len || gfx_text_size(ui->cr, s->str, 15, FW_REGULAR, 0, 1).w <= w;
		if (fits) gfx_text(ui->cr, s->str, x, y, 15, FW_REGULAR, ui->th.sub, TA_LEFT, w, 1);
		g_string_free(s, TRUE);
		if (fits) break;
	}
	g_ptr_array_free(parts, TRUE);
}

static double draw_op_progress(UI *ui, Rect area, double y) {
	if (!g_op) return y;
	char msg[128];
	int i = MIN(g_op->i + 1, g_op->n);
	if (g_op->kind == OP_COPY) snprintf(msg, sizeof msg, _("copying %d of %d…"), i, g_op->n);
	else if (g_op->kind == OP_MOVE) snprintf(msg, sizeof msg, _("moving %d of %d…"), i, g_op->n);
	else str_copy(msg, sizeof msg, _("deleting…"));
	gfx_text(ui->cr, msg, area.x + 24, y, 15, FW_REGULAR, ui->th.fg, TA_LEFT, area.w - 48, 1);
	ui_progress_dots(ui, R(area.x + 24, y + 20, area.w - 48, 8), ui->th.accent);
	return y + 34;
}

static void draw_root(UI *ui, Level *l, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = ui_page_header(ui, 14, area.y + 6, _("Files"), _("this device"));
	if (g_dev_total > 0) {
		char a[32], b[32], msg[128];
		human_size(g_dev_free, a, sizeof a);
		human_size(g_dev_total, b, sizeof b);
		snprintf(msg, sizeof msg, _("%s free of %s"), a, b);
		gfx_text(cr, msg, area.x + 24, y - 2, 15, FW_REGULAR, th->sub, TA_LEFT, area.w - 48, 1);
		double bw = area.w - 48;
		gfx_fill_rect(cr, R(area.x + 24, y + 20, bw, 6), th->chrome2);
		gfx_fill_rect(cr, R(area.x + 24, y + 20, bw * CLAMP(1 - g_dev_free / g_dev_total, 0, 1), 6), th->accent);
		y += 34;
	} else y += 4;
	y = draw_op_progress(ui, area, y);
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	int ndrives = 0;
	for (int i = 0; i < g_nlocs; i++) ndrives += g_locs[i].drive;
	double content = g_nlocs * ROW_H + (ndrives ? 44 : 0) + 20;
	double off = ui_scroll_begin(ui, ui_id("files-root"), &l->scroll, view, content);
	cairo_translate(cr, 0, view.y);
	double ry = 0;
	if (!g_root_loaded) ui_progress_dots(ui, R(area.x + 24, 10, area.w - 48, 8), th->accent);
	for (int i = 0; i < g_nlocs; i++) {
		Loc *lc = &g_locs[i];
		if (lc->drive && (i == 0 || !g_locs[i - 1].drive)) {
			gfx_text(cr, _("removable storage"), area.x + 24, ry + 12, 20, FW_SEMILIGHT, th->sub, TA_LEFT, area.w - 48, 1);
			ry += 44;
		}
		Rect rr = R(area.x, ry, area.w, ROW_H);
		ry += ROW_H;
		if (rect_bottom(rr) < off || rr.y > off + view.h) continue;
		char sub[128] = "";
		if (lc->drive && lc->total > 0) {
			char a[32], b[32];
			human_size(lc->free, a, sizeof a);
			human_size(lc->total, b, sizeof b);
			snprintf(sub, sizeof sub, _("%s free of %s"), a, b);
		} else if (lc->count >= 0) fmt_count(sub, sizeof sub, lc->count);
		else if (str_eq(lc->path, "/")) str_copy(sub, sizeof sub, "/");
		UiId id = ui_idn("files-loc", i);
		const char *name = lc->std ? tr(lc->name) : lc->name;
		if (tile_row(ui, id, rr, lc->icon, th->accent, NULL, name, sub, 26, 1.f, 0))
			open_dir(lc->path, lc->name, lc->std, lc->path);
		if (ui_longpressed(ui, id)) {
			Rect sr = ui_to_screen(ui, rr);
			open_menu(lc->path, true, true, ui->p.x, sr.y + ROW_H / 2);
		}
	}
	ui_scroll_end(ui);
}

static void entry_subtitle(const Entry *e, char *buf, size_t n) {
	char when[64], what[64] = "";
	fmt_when(when, sizeof when, e->mtime);
	if (e->dir) {
		int c = g_hidden ? e->count_all : e->count;
		if (c >= 10000) snprintf(what, sizeof what, _("%d+ items"), 10000);
		else if (c >= 0) fmt_count(what, sizeof what, c);
	} else human_size((double)e->size, what, sizeof what);
	if (e->link) snprintf(buf, n, "%s%s%s • %s", what, *what ? " • " : "", when, _("link"));
	else snprintf(buf, n, "%s%s%s", what, *what ? " • " : "", when);
}

static void toggle_sel(Level *l, int ei) {
	if (!l->sel) l->sel = xcalloc((size_t)l->n + 1, sizeof(bool));
	l->sel[ei] = !l->sel[ei];
	l->nsel += l->sel[ei] ? 1 : -1;
}

static void draw_dir(UI *ui, Level *l, Rect area, int depth) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	bool cur = depth == g_depth;
	double y = ui_page_header(ui, 14, area.y + 6, device_title(l), level_title(l));
	if (cur && g_select) {
		char msg[64];
		snprintf(msg, sizeof msg, _("%d selected"), l->nsel);
		gfx_text(cr, msg, area.x + 24, y - 4, 15, FW_REGULAR, th->accent, TA_LEFT, area.w - 48, 1);
	} else draw_crumbs(ui, l, area.x + 24, y - 4, area.w - 48);
	y += 22;
	if (cur && g_search_on) {
		bool changed = false;
		UiId fid = ui_id("files-filter");
		if (g_search_focus) { ui_focus(ui, fid); g_search_focus = false; }
		if (ui_textbox(ui, fid, R(area.x + 24, y, area.w - 48, 44), g_filter, sizeof g_filter, _("search this folder"), 0,
		               &changed))
			ui->focus = 0;
		if (changed) level_rebuild_view(l);
		y += 54;
	}
	y = draw_op_progress(ui, area, y);
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	if (!l->loaded) {
		if (l->loading && ui->now - l->load_t0 > 0.15) ui_progress_dots(ui, R(area.x + 24, y + 10, area.w - 48, 8), th->accent);
		else ui->animating = true;
		return;
	}
	if (l->err) {
		const char *msg = l->err == EACCES || l->err == EPERM ? _("You don't have permission to see what's in this folder.")
		                  : l->err == ENOENT ? _("This folder doesn't exist anymore.")
		                  : _("This folder couldn't be opened.");
		app_empty(ui, R(area.x, y - 8, area.w, 100), msg);
		return;
	}
	if (!l->nview) {
		const char *msg = g_search_on && g_filter[0] && cur ? _("No items match your search.")
		                  : l->n ? _("This folder only has hidden items.") : _("This folder is empty.");
		app_empty(ui, R(area.x, y - 8, area.w, 100), msg);
		return;
	}
	double sel_off = cur ? g_sel_anim * 46 : 0;
	double content = l->nview * ROW_H + (l->truncated ? 50 : 0) + 20;
	double off = ui_scroll_begin(ui, ui_idn("files-list", depth), &l->scroll, view, content);
	cairo_translate(cr, 0, view.y);
	int first = MAX(0, (int)(off / ROW_H)), last = MIN(l->nview - 1, (int)((off + view.h) / ROW_H) + 1);
	double hl = l->highlight_t > 0 ? 1 - (ui->now - l->highlight_t) / 2.5 : 0;
	if (hl > 0) ui->animating = true;
	else if (l->highlight_t > 0) { l->highlight_t = 0; free(l->highlight); l->highlight = NULL; }
	for (int k = first; k <= last; k++) {
		int ei = l->view[k];
		Entry *e = &l->e[ei];
		Rect rr = R(area.x, k * ROW_H, area.w, ROW_H);
		char sub[160];
		entry_subtitle(e, sub, sizeof sub);
		UiId id = ui_idn("files-row", k * MAX_LV + depth);
		bool selected = l->sel && l->sel[ei];
		if (sel_off > 0.5) {
			Rect box = R(area.x + 24 - 46 + sel_off, rr.y + (ROW_H - 30) / 2, 30, 30);
			gfx_stroke_rect(cr, box, 2, th->fg);
			if (selected) {
				gfx_fill_rect(cr, rect_inset(box, 2), th->accent);
				icon_draw(cr, ICON_CHECK, rect_cx(box), rect_cy(box), 22, th->on_accent);
			}
		}
		bool hlrow = hl > 0 && l->highlight && str_eq(l->highlight, e->name);
		Rect cr_r = R(rr.x + sel_off, rr.y, rr.w - sel_off, rr.h);
		Image *thumb = thumb_for(ui, l->path, e);
		bool clicked = tile_row(ui, id, cr_r, KINDS[e->kind].icon, kind_color(ui, e->kind), thumb, e->disp, sub, 22,
		                        e->hidden ? 0.55f : 1.f, hlrow ? hl : 0);
		if (!cur) continue;
		if (clicked) {
			char *p = path_join(l->path, e->name);
			if (g_select) toggle_sel(l, ei);
			else if (e->dir) open_dir(p, l->loc_name, l->loc_std, l->loc_root);
			else open_file(p);
			free(p);
		}
		if (ui_longpressed(ui, id)) {
			if (g_select) toggle_sel(l, ei);
			else {
				char *p = path_join(l->path, e->name);
				Rect sr = ui_to_screen(ui, rr);
				open_menu(p, e->dir, false, ui->p.x, sr.y + ROW_H / 2);
				free(p);
			}
		}
	}
	if (l->truncated) {
		char msg[128];
		snprintf(msg, sizeof msg, _("Only the first %d items are shown."), MAX_ENTRIES);
		gfx_text(cr, msg, area.x + 24, l->nview * ROW_H + 14, 15, FW_REGULAR, th->sub, TA_LEFT, area.w - 48, 0);
	}
	ui_scroll_end(ui);
}

static void perm_string(mode_t m, char out[11]) {
	out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
	const char *rwx = "rwxrwxrwx";
	for (int i = 0; i < 9; i++) out[i + 1] = (m & (1 << (8 - i))) ? rwx[i] : '-';
	out[10] = 0;
}

static double prop_row(UI *ui, Rect area, double y, const char *label, const char *value, int lines) {
	gfx_text(ui->cr, label, area.x + 24, y, 15, FW_REGULAR, ui->th.sub, TA_LEFT, area.w - 48, 1);
	double h = gfx_text(ui->cr, value, area.x + 24, y + 19, 20, FW_SEMILIGHT, ui->th.fg, TA_LEFT, area.w - 48, lines);
	return y + 19 + h + 14;
}

static void draw_props(UI *ui, Level *l, Rect area) {
	cairo_t *cr = ui->cr;
	char *disp = g_utf8_make_valid(path_basename(l->path), -1);
	double y = ui_page_header(ui, 14, area.y + 6, _("properties"), str_eq(l->path, "/") ? _("File system") : disp);
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	ui_scroll_begin(ui, ui_id("files-props"), &l->scroll, view, l->content_h + 20);
	cairo_translate(cr, 0, view.y);
	double py = 6;
	char buf[256], a[64], b[64];
	if (!l->st_ok) {
		app_empty(ui, R(area.x, 0, area.w, 80), _("This item doesn't exist anymore."));
	} else {
		bool dir = S_ISDIR(l->st.st_mode);
		py = prop_row(ui, area, py, _("name"), disp, 3);
		int k = kind_for(path_basename(l->path), l->st.st_mode);
		py = prop_row(ui, area, py, _("type"), tr(KINDS[k].name), 1);
		char *full = g_utf8_make_valid(l->path, -1);
		py = prop_row(ui, area, py, _("full path"), full, 4);
		g_free(full);
		if (l->link_target) {
			char *t = g_utf8_make_valid(l->link_target, -1);
			py = prop_row(ui, area, py, _("link to"), t, 3);
			g_free(t);
		}
		if (dir) {
			if (l->du_state == 1) {
				py = prop_row(ui, area, py, _("size"), _("calculating…"), 1);
			} else if (l->du_state == 2) {
				snprintf(buf, sizeof buf, _("%s (%.0f bytes)"), human_size(l->du_size, a, sizeof a), l->du_size);
				py = prop_row(ui, area, py, _("size"), buf, 2);
			}
			if (l->du_items >= 0) {
				fmt_count(buf, sizeof buf, l->du_items);
				py = prop_row(ui, area, py, _("contains"), buf, 1);
			}
		} else {
			snprintf(buf, sizeof buf, _("%s (%.0f bytes)"), human_size((double)l->st.st_size, a, sizeof a), (double)l->st.st_size);
			py = prop_row(ui, area, py, _("size"), buf, 2);
		}
		struct tm tm;
		localtime_r(&l->st.st_mtime, &tm);
		i18n_fmt_date_short(a, sizeof a, &tm);
		i18n_fmt_time(b, sizeof b, &tm, cfg.h24);
		snprintf(buf, sizeof buf, "%s %s", a, b);
		py = prop_row(ui, area, py, _("modified"), buf, 1);
		char perm[11];
		perm_string(l->st.st_mode, perm);
		struct passwd *pw = getpwuid(l->st.st_uid);
		struct group *gr = getgrgid(l->st.st_gid);
		bool r = access(l->path, R_OK) == 0, w = access(l->path, W_OK) == 0;
		snprintf(buf, sizeof buf, "%s  %s:%s\n%s", perm, pw ? pw->pw_name : "?", gr ? gr->gr_name : "?",
		         r && w ? _("you can read and change it") : r ? _("read only for you") : _("you can't open it"));
		py = prop_row(ui, area, py, _("permissions"), buf, 2);
	}
	g_free(disp);
	ui_scroll_end(ui);
	l->content_h = py;   /* used for the scroll range next frame */
}

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	Level *l = page >= 0 && page < MAX_LV ? g_lv[page] : NULL;
	if (!l) return;
	if (l->kind == LV_ROOT) draw_root(ui, l, area);
	else if (l->kind == LV_DIR) draw_dir(ui, l, area, page);
	else draw_props(ui, l, area);
}

/* ------------------------------------------------------------- app bar --- */

static void sort_cb(int idx, void *ud) {
	if (idx < 0 || idx >= SORT_COUNT) return;
	g_sort = idx;
	cfg_set("sort", idx);
	for (int i = 0; i <= g_depth; i++) level_rebuild_view(g_lv[i]);
}

static char **selected_paths(Level *l, int *n) {
	char **v = xmalloc(sizeof(char *) * (size_t)(l->nsel + 1));
	*n = 0;
	for (int k = 0; k < l->nview && l->sel; k++)
		if (l->sel[l->view[k]]) v[(*n)++] = path_join(l->path, l->e[l->view[k]].name);
	return v;
}

static void free_paths(char **v, int n) {
	for (int i = 0; i < n; i++) free(v[i]);
	free(v);
}

static void set_select(bool on) {
	g_select = on;
	Level *l = g_lv[g_depth];
	if (!on && l && l->kind == LV_DIR) level_clear_sel(l);
}

static void draw_appbar(UI *ui, Rect area) {
	Level *l = g_lv[g_depth];
	if (!l) return;
	AppBarBtn btns[4];
	const char *menu[8];
	int acts[12], nb = 0, nm = 0;
	enum { B_PASTE, B_NEWFOLDER, B_SELECT, B_SEARCH, B_COPY, B_CUT, B_DELETE, B_RENAME,
	       M_SORT, M_HIDDEN, M_TERMINAL, M_REFRESH, M_PROPS, M_CLEARCLIP, M_SELALL, M_SELNONE };
	int macts[8];
	bool busy = g_op != NULL;
	if (l->kind == LV_DIR && g_select) {
		bool none = l->nsel == 0;
		btns[nb] = (AppBarBtn){ICON_COPY, _("copy"), none || busy}; acts[nb++] = B_COPY;
		btns[nb] = (AppBarBtn){ICON_CUT, _("cut"), none || busy}; acts[nb++] = B_CUT;
		btns[nb] = (AppBarBtn){ICON_DELETE, _("delete"), none || busy}; acts[nb++] = B_DELETE;
		btns[nb] = (AppBarBtn){ICON_RENAME, _("rename"), l->nsel != 1}; acts[nb++] = B_RENAME;
		macts[nm] = M_SELALL; menu[nm++] = _("select all");
		if (!none) { macts[nm] = M_SELNONE; menu[nm++] = _("clear selection"); }
	} else if (l->kind == LV_DIR) {
		if (g_clip.n) { btns[nb] = (AppBarBtn){ICON_PASTE, _("paste"), busy}; acts[nb++] = B_PASTE; }
		btns[nb] = (AppBarBtn){ICON_NEW_FOLDER, _("new folder"), l->err != 0}; acts[nb++] = B_NEWFOLDER;
		btns[nb] = (AppBarBtn){ICON_SELECT, _("select"), !l->nview}; acts[nb++] = B_SELECT;
		btns[nb] = (AppBarBtn){ICON_SEARCH, _("search"), l->err != 0}; acts[nb++] = B_SEARCH;
		macts[nm] = M_SORT; menu[nm++] = _("sort by…");
		macts[nm] = M_HIDDEN; menu[nm++] = g_hidden ? _("hide hidden files") : _("show hidden files");
		if (terminal_available()) { macts[nm] = M_TERMINAL; menu[nm++] = _("open terminal here"); }
		macts[nm] = M_REFRESH; menu[nm++] = _("refresh");
		macts[nm] = M_PROPS; menu[nm++] = _("properties");
		if (g_clip.n) { macts[nm] = M_CLEARCLIP; menu[nm++] = _("clear clipboard"); }
	} else if (l->kind == LV_ROOT) {
		macts[nm] = M_REFRESH; menu[nm++] = _("refresh");
		if (terminal_available()) { macts[nm] = M_TERMINAL; menu[nm++] = _("open terminal"); }
		if (g_clip.n) { macts[nm] = M_CLEARCLIP; menu[nm++] = _("clear clipboard"); }
	} else return;
	int r = appbar_draw(ui, &g_bar, area, btns, nb, menu, nm);
	if (r < 0) return;
	int act = r >= APPBAR_MENU ? macts[r - APPBAR_MENU] : acts[r];
	int n;
	char **v;
	switch (act) {
	case B_PASTE: paste_into(l->path); break;
	case B_NEWFOLDER: dialog_input(_("new folder"), _("folder name"), _("New folder"), 0, mkdir_cb, NULL); break;
	case B_SELECT: set_select(true); break;
	case B_SEARCH:
		g_search_on = !g_search_on;
		g_search_focus = g_search_on;
		if (!g_search_on) { g_filter[0] = 0; ui->focus = 0; }
		level_rebuild_view(l);
		break;
	case B_COPY: case B_CUT:
		v = selected_paths(l, &n);
		if (n) clip_set(v, n, act == B_CUT);
		else free(v);
		set_select(false);
		break;
	case B_DELETE:
		v = selected_paths(l, &n);
		delete_paths(v, n, false);
		free_paths(v, n);
		break;
	case B_RENAME:
		v = selected_paths(l, &n);
		if (n == 1) start_rename(v[0]);
		free_paths(v, n);
		set_select(false);
		break;
	case M_SORT: {
		const char *items[SORT_COUNT];
		for (int i = 0; i < SORT_COUNT; i++) items[i] = tr(SORT_NAMES[i]);
		dialog_list(_("sort by"), items, SORT_COUNT, g_sort, sort_cb, NULL);
		break;
	}
	case M_HIDDEN:
		g_hidden = !g_hidden;
		cfg_set("hidden", g_hidden);
		for (int i = 0; i <= g_depth; i++) level_rebuild_view(g_lv[i]);
		break;
	case M_TERMINAL: terminal_open(l->kind == LV_DIR ? l->path : home_dir()); break;
	case M_REFRESH: refresh_all(); break;
	case M_PROPS: open_props(l->path, l); break;
	case M_CLEARCLIP: clip_clear(); break;
	case M_SELALL:
		if (!l->sel) l->sel = xcalloc((size_t)l->n + 1, sizeof(bool));
		l->nsel = 0;
		for (int k = 0; k < l->nview; k++) { l->sel[l->view[k]] = true; l->nsel++; }
		break;
	case M_SELNONE: level_clear_sel(l); break;
	}
}

/* ------------------------------------------------------------ lifecycle -- */

static void ensure_init(void) {
	if (g_inited) return;
	g_inited = true;
	g_sort = CLAMP(cfg_get("sort", SORT_NAME), 0, SORT_COUNT - 1);
	g_hidden = cfg_get("hidden", 0) != 0;
	g_lv[0] = level_new(LV_ROOT, NULL, "", false, "/");
	g_depth = 0;
	nav_init(&g_nav, 0);
	g_root_loaded = false;
	root_refresh();
}

static void files_open(const char *arg) {
	ensure_init();
	if (!arg || !*arg) { refresh_all(); return; }
	char *path = NULL;
	if (str_starts(arg, "file://")) path = g_uri_unescape_string(arg + 7, NULL);
	if (!path) path = g_strdup(arg);
	char *highlight = NULL;
	if (!is_dir(path)) {
		if (!lexists(path)) { g_free(path); refresh_all(); return; }
		highlight = xstrdup(path_basename(path));
		char *par = parent_of(path);
		g_free(path);
		path = g_strdup(par);
		free(par);
	}
	/* jump straight there: root + that folder, Back goes up to the locations */
	for (int i = 1; i < MAX_LV; i++) { level_free(g_lv[i]); g_lv[i] = NULL; }
	reset_page_state();
	char *name, *root;
	bool std;
	loc_for_path(path, &name, &std, &root);
	Level *l = level_new(LV_DIR, path, name, std, root);
	l->highlight = highlight;
	free(name);
	free(root);
	g_lv[1] = l;
	g_depth = 1;
	nav_init(&g_nav, 0);
	g_nav.stack[1] = 1;
	g_nav.depth = 1;
	level_load(l);
	root_refresh();
	g_free(path);
}

static void files_draw(UI *ui, Rect area) {
	ensure_init();
	g_sel_anim = ui_smooth(ui, g_sel_anim, g_select ? 1 : 0, 18);
	/* free the page that was popped once its transition is over */
	if (!nav_busy(&g_nav, ui))
		for (int i = g_depth + 1; i < MAX_LV && g_lv[i]; i++) { level_free(g_lv[i]); g_lv[i] = NULL; }
	Level *l = g_lv[g_depth];
	g_bar.minimized = l && l->kind != LV_DIR;
	Rect content = l && l->kind == LV_PROPS ? area : appbar_content(&g_bar, area);
	nav_draw(ui, &g_nav, content, page_fn, NULL);
	draw_appbar(ui, area);
}

static bool files_back(void) {
	if (appbar_back(&g_bar)) return true;
	if (g_select) { set_select(false); return true; }
	if (g_search_on) {
		g_search_on = false;
		g_filter[0] = 0;
		sh.ui.focus = 0;
		if (g_lv[g_depth]) level_rebuild_view(g_lv[g_depth]);
		return true;
	}
	return go_pop();
}

static void files_close(void) {
	for (int i = 0; i < MAX_LV; i++) { level_free(g_lv[i]); g_lv[i] = NULL; }
	locs_free(g_locs, g_nlocs);
	g_nlocs = 0;
	g_depth = 0;
	clip_clear();
	free(g_pending);
	g_pending = NULL;
	if (g_thumbs) imgcache_clear(g_thumbs);
	reset_page_state();
	g_inited = false;
	g_root_loaded = false;
}

const AppDef app_files = {
	.id = "files", .name = N_("Files"), .icon = ICON_FOLDER, .color = 0,
	.keywords = "file manager explorer folders documents downloads",
	.open = files_open, .draw = files_draw, .back = files_back, .close = files_close,
};

#include "desktop.h"
#include "i18n.h"
#include "proc.h"
#include "util.h"

#include <dirent.h>
#include <glib.h>
#include <sys/stat.h>

static VEC(DesktopApp *) g_apps;

/* ----------------------------------------------------------- data dirs --- */

static char **data_dirs(int *n) {
	static char *dirs[24];
	static int cnt;
	if (cnt) { *n = cnt; return dirs; }
	const char *xh = getenv("XDG_DATA_HOME");
	dirs[cnt++] = (xh && *xh) ? xstrdup(xh) : path_join(home_dir(), ".local/share");
	dirs[cnt++] = path_join(home_dir(), ".local/share/flatpak/exports/share");
	dirs[cnt++] = xstrdup("/var/lib/flatpak/exports/share");
	const char *xd = getenv("XDG_DATA_DIRS");
	char *copy = xstrdup(xd && *xd ? xd : "/usr/local/share:/usr/share"), *save = NULL;
	for (char *d = strtok_r(copy, ":", &save); d && cnt < 22; d = strtok_r(NULL, ":", &save)) {
		bool dup = false;
		for (int i = 0; i < cnt; i++) if (str_eq(dirs[i], d)) dup = true;
		if (!dup) dirs[cnt++] = xstrdup(d);
	}
	free(copy);
	*n = cnt;
	return dirs;
}

/* ------------------------------------------------------ desktop parsing --- */

static void app_free(DesktopApp *a) {
	free(a->id); free(a->name); free(a->comment); free(a->exec); free(a->icon_key);
	free(a->wm_class); free(a->categories); free(a->file); free(a->fold_name);
	image_free(a->icon);
	free(a);
}

static char *unescape(const char *v) {
	char *o = xmalloc(strlen(v) + 1), *w = o;
	for (; *v; v++) {
		if (*v == '\\' && v[1]) {
			v++;
			*w++ = *v == 'n' ? '\n' : *v == 't' ? '\t' : *v == 's' ? ' ' : *v;
		} else {
			*w++ = *v;
		}
	}
	*w = 0;
	return o;
}

static bool list_has(const char *list, const char *item) {
	if (!list) return false;
	char *copy = xstrdup(list), *save = NULL;
	bool found = false;
	for (char *t = strtok_r(copy, ";", &save); t && !found; t = strtok_r(NULL, ";", &save))
		found = str_eq(t, item);
	free(copy);
	return found;
}

static DesktopApp *parse_desktop(const char *path, const char *id) {
	char *txt = file_read_all(path, NULL);
	if (!txt) return NULL;
	DesktopApp *a = xcalloc(1, sizeof *a);
	char *name_loc = NULL, *name_def = NULL, *only = NULL, *notin = NULL, *tryexec = NULL;
	char *comment_loc = NULL, *comment_def = NULL;
	bool in_entry = false, nodisplay = false, hidden = false, is_app = false;
	const char *lang = i18n_code(i18n_lang());
	char key_lang[16], key_lang_full[24];
	snprintf(key_lang, sizeof key_lang, "Name[%s]", lang);
	snprintf(key_lang_full, sizeof key_lang_full, "Name[%s_", lang);
	char ckey[24];
	snprintf(ckey, sizeof ckey, "Comment[%s]", lang);

	char *save = NULL;
	for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		line = str_trim(line);
		if (!*line || *line == '#') continue;
		if (*line == '[') { in_entry = str_eq(line, "[Desktop Entry]"); continue; }
		if (!in_entry) continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = str_trim(line), *v = str_trim(eq + 1);
		if (str_eq(k, "Type")) is_app = str_eq(v, "Application");
		else if (str_eq(k, "Name")) { free(name_def); name_def = unescape(v); }
		else if (str_eq(k, key_lang) || (str_starts(k, key_lang_full) && !name_loc)) { free(name_loc); name_loc = unescape(v); }
		else if (str_eq(k, "Comment")) { free(comment_def); comment_def = unescape(v); }
		else if (str_eq(k, ckey)) { free(comment_loc); comment_loc = unescape(v); }
		else if (str_eq(k, "Exec")) { free(a->exec); a->exec = xstrdup(v); }
		else if (str_eq(k, "Icon")) { free(a->icon_key); a->icon_key = xstrdup(v); }
		else if (str_eq(k, "StartupWMClass")) { free(a->wm_class); a->wm_class = xstrdup(v); }
		else if (str_eq(k, "Categories")) { free(a->categories); a->categories = xstrdup(v); }
		else if (str_eq(k, "NoDisplay")) nodisplay = str_eq(v, "true");
		else if (str_eq(k, "Hidden")) hidden = str_eq(v, "true");
		else if (str_eq(k, "Terminal")) a->terminal = str_eq(v, "true");
		else if (str_eq(k, "OnlyShowIn")) { free(only); only = xstrdup(v); }
		else if (str_eq(k, "NotShowIn")) { free(notin); notin = xstrdup(v); }
		else if (str_eq(k, "TryExec")) { free(tryexec); tryexec = xstrdup(v); }
	}
	free(txt);
	bool ok = is_app && !nodisplay && !hidden && a->exec && (name_loc || name_def);
	if (ok && only && *only && !list_has(only, "metrowl")) ok = false;
	if (ok && notin && list_has(notin, "metrowl")) ok = false;
	if (ok && tryexec && *tryexec && !proc_exists(tryexec)) ok = false;
	free(only); free(notin); free(tryexec);
	if (!ok) {
		free(name_loc); free(name_def); free(comment_loc); free(comment_def);
		app_free(a);
		return NULL;
	}
	a->name = name_loc ? name_loc : name_def;
	if (name_loc) free(name_def);
	a->comment = comment_loc ? comment_loc : comment_def;
	if (comment_loc) free(comment_def);
	a->id = xstrdup(id);
	a->file = xstrdup(path);
	a->fold_name = str_fold(a->name);
	a->is_flatpak = strstr(path, "flatpak/exports") != NULL;
	return a;
}

static void scan_dir(const char *base, const char *dir, const char *prefix) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char *full = path_join(dir, e->d_name);
		if (is_dir(full)) {
			char *np = xasprintf("%s%s-", prefix, e->d_name);
			scan_dir(base, full, np);
			free(np);
		} else if (str_ends(e->d_name, ".desktop")) {
			char *id = xasprintf("%s%.*s", prefix, (int)(strlen(e->d_name) - 8), e->d_name);
			if (!desktop_find(id)) {
				DesktopApp *a = parse_desktop(full, id);
				if (a) vec_push(&g_apps, a);
			}
			free(id);
		}
		free(full);
	}
	closedir(d);
}

static int app_cmp(const void *x, const void *y) {
	const DesktopApp *a = *(DesktopApp *const *)x, *b = *(DesktopApp *const *)y;
	return strcmp(a->fold_name, b->fold_name);
}

void desktop_scan(void) {
	for (int i = 0; i < g_apps.len; i++) app_free(g_apps.items[i]);
	g_apps.len = 0;
	int n;
	char **dirs = data_dirs(&n);
	for (int i = 0; i < n; i++) {
		char *ad = path_join(dirs[i], "applications");
		scan_dir(ad, ad, "");
		free(ad);
	}
	if (g_apps.len) qsort(g_apps.items, (size_t)g_apps.len, sizeof(DesktopApp *), app_cmp);
	LOGI("found %d desktop applications", g_apps.len);
}

int desktop_count(void) { return g_apps.len; }
DesktopApp *desktop_get(int i) { return (i >= 0 && i < g_apps.len) ? g_apps.items[i] : NULL; }

DesktopApp *desktop_find(const char *id) {
	for (int i = 0; i < g_apps.len; i++)
		if (str_eq(g_apps.items[i]->id, id)) return g_apps.items[i];
	return NULL;
}

static char *exec_basename(const char *exec) {
	char *c = xstrdup(exec);
	char *sp = c;
	while (*sp && *sp != ' ') sp++;
	*sp = 0;
	char *r = xstrdup(path_basename(c));
	free(c);
	return r;
}

DesktopApp *desktop_find_by_app_id(const char *app_id) {
	if (!app_id || !*app_id) return NULL;
	DesktopApp *a = desktop_find(app_id);
	if (a) return a;
	for (int i = 0; i < g_apps.len; i++) {
		a = g_apps.items[i];
		if (a->wm_class && g_ascii_strcasecmp(a->wm_class, app_id) == 0) return a;
	}
	for (int i = 0; i < g_apps.len; i++) {
		a = g_apps.items[i];
		const char *dot = strrchr(a->id, '.');
		if (g_ascii_strcasecmp(dot ? dot + 1 : a->id, app_id) == 0) return a;
		char *eb = exec_basename(a->exec);
		bool m = g_ascii_strcasecmp(eb, app_id) == 0;
		free(eb);
		if (m) return a;
	}
	return NULL;
}

/* --------------------------------------------------------------- launching */

/* Splits an Exec line into argv honouring quotes and dropping %-field codes. */
static char **exec_to_argv(const char *exec, const char *uri) {
	char **argv = xcalloc(64, sizeof(char *));
	int n = 0;
	const char *p = exec;
	while (*p && n < 60) {
		while (*p == ' ') p++;
		if (!*p) break;
		char *tok = xmalloc(strlen(p) + (uri ? strlen(uri) : 0) + 8), *w = tok;
		bool quote = false;
		for (; *p && (quote || *p != ' '); p++) {
			if (*p == '"') { quote = !quote; continue; }
			if (*p == '\\' && quote && p[1]) { p++; *w++ = *p; continue; }
			if (*p == '%' && p[1]) {
				p++;
				if (*p == '%') *w++ = '%';
				else if ((*p == 'u' || *p == 'U' || *p == 'f' || *p == 'F') && uri) { strcpy(w, uri); w += strlen(uri); }
				continue;
			}
			*w++ = *p;
		}
		*w = 0;
		if (*tok) argv[n++] = tok; else free(tok);
	}
	argv[n] = NULL;
	return argv;
}

static bool launch(const DesktopApp *a, const char *uri) {
	char **argv = exec_to_argv(a->exec, uri);
	if (!argv[0]) { free(argv); return false; }
	char *term_argv[72];
	char **use = argv;
	if (a->terminal) {
		const char *term = getenv("TERMINAL");
		const char *cands[] = {term, "foot", "alacritty", "kitty", "wezterm", "xterm", NULL};
		const char *t = NULL;
		for (int i = 0; cands[i]; i++) if (cands[i] && *cands[i] && proc_exists(cands[i])) { t = cands[i]; break; }
		if (t) {
			int n = 0;
			term_argv[n++] = (char *)t;
			term_argv[n++] = "-e";
			for (int i = 0; argv[i] && n < 70; i++) term_argv[n++] = argv[i];
			term_argv[n] = NULL;
			use = term_argv;
		}
	}
	LOGI("launching %s (%s)", a->name, use[0]);
	bool ok = proc_spawn(use);
	for (int i = 0; argv[i]; i++) free(argv[i]);
	free(argv);
	return ok;
}

bool desktop_launch(const DesktopApp *a) { return launch(a, NULL); }
bool desktop_launch_uri(const DesktopApp *a, const char *uri) { return launch(a, uri); }

DesktopApp *desktop_default_browser(void) {
	static const char *known[] = {"firefox", "org.mozilla.firefox", "chromium", "org.chromium.Chromium",
		"google-chrome", "brave-browser", "falkon", "epiphany", "org.gnome.Epiphany", "org.kde.falkon", "midori", NULL};
	for (int i = 0; known[i]; i++) {
		DesktopApp *a = desktop_find(known[i]);
		if (a) return a;
	}
	for (int i = 0; i < g_apps.len; i++)
		if (g_apps.items[i]->categories && strstr(g_apps.items[i]->categories, "WebBrowser"))
			return g_apps.items[i];
	return NULL;
}

void open_uri(const char *uri) {
	if (proc_exists("xdg-open")) {
		char *argv[] = {"xdg-open", (char *)uri, NULL};
		proc_spawn(argv);
		return;
	}
	DesktopApp *b = desktop_default_browser();
	if (b) desktop_launch_uri(b, uri);
}

/* ------------------------------------------------------------------ icons -- */

static char *find_icon_file(const char *key) {
	if (key[0] == '/') return file_exists(key) ? xstrdup(key) : NULL;
	static const char *sizes[] = {"256x256", "512x512", "128x128", "192x192", "96x96", "64x64", "72x72",
		"48x48", "scalable", NULL};
	static const char *exts[] = {"png", "svg", NULL};
	int n;
	char **dirs = data_dirs(&n);
	for (int s = 0; sizes[s]; s++) {
		for (int i = 0; i < n; i++) {
			for (int e = 0; exts[e]; e++) {
				char *rel = xasprintf("icons/hicolor/%s/apps/%s.%s", sizes[s], key, exts[e]);
				char *p = path_join(dirs[i], rel);
				free(rel);
				if (file_exists(p)) return p;
				free(p);
			}
		}
	}
	static const char *themes[] = {"Adwaita", "breeze", "Papirus", "gnome", "elementary", NULL};
	for (int t = 0; themes[t]; t++) {
		for (int i = 0; i < n; i++) {
			for (int s = 0; sizes[s]; s++) {
				for (int e = 0; exts[e]; e++) {
					char *rel = xasprintf("icons/%s/%s/apps/%s.%s", themes[t], sizes[s], key, exts[e]);
					char *p = path_join(dirs[i], rel);
					free(rel);
					if (file_exists(p)) return p;
					free(p);
				}
			}
		}
	}
	const char *pix_ext[] = {"png", "svg", NULL};
	for (int e = 0; pix_ext[e]; e++) {
		char *p = xasprintf("/usr/share/pixmaps/%s.%s", key, pix_ext[e]);
		if (file_exists(p)) return p;
		free(p);
	}
	return NULL;
}

Image *desktop_icon(DesktopApp *a, int px) {
	if (a->icon || a->icon_tried) return a->icon;
	a->icon_tried = true;
	if (!a->icon_key) return NULL;
	char *f = find_icon_file(a->icon_key);
	if (!f) return NULL;
	a->icon = image_load_icon(f, px);
	free(f);
	return a->icon;
}

/* Small cache for icons requested by name (notifications etc.). */
typedef struct { char *name; int px; Image *img; } NamedIcon;
static NamedIcon g_named[64];
static int g_named_n;

Image *desktop_icon_by_name(const char *name, int px) {
	if (!name || !*name) return NULL;
	for (int i = 0; i < g_named_n; i++)
		if (g_named[i].px == px && str_eq(g_named[i].name, name)) return g_named[i].img;
	char *f = find_icon_file(name);
	Image *img = f ? image_load_icon(f, px) : NULL;
	free(f);
	if (g_named_n < 64) g_named[g_named_n++] = (NamedIcon){xstrdup(name), px, img};
	return img;
}

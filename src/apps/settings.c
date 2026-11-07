/* Settings: the Windows Phone 8.1 settings hub. The root is a "system" / "applications"
 * pivot of plain text rows (title + status line); every row opens an inner page with
 * the turnstile transition. Pages can be deep-linked with open("wifi") etc. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../sys.h"
#include "../util.h"

#include <dirent.h>
#include <glib.h>
#include <math.h>
#include <unistd.h>

/* implemented by Photos; weak so Settings still links (and falls back) without it */
#pragma weak photos_pick

enum {
	P_ROOT, P_THEME, P_ACCENT, P_SOUNDS, P_NOTIF, P_LOCK, P_WIFI, P_AIR, P_BT, P_DISPLAY,
	P_DATETIME, P_TZ, P_LANG, P_KEYBOARD, P_EASE, P_ABOUT, P_WEATHER, P_PHOTOS, P_COUNT
};

static const char *const PAGE_TITLE[P_COUNT] = {
	[P_ROOT] = N_("settings"), [P_THEME] = N_("start+theme"), [P_ACCENT] = N_("accents"),
	[P_SOUNDS] = N_("ringtones+sounds"), [P_NOTIF] = N_("notifications+actions"),
	[P_LOCK] = N_("lock screen"), [P_WIFI] = N_("wi-fi"), [P_AIR] = N_("airplane mode"),
	[P_BT] = N_("bluetooth"), [P_DISPLAY] = N_("display"), [P_DATETIME] = N_("date+time"),
	[P_TZ] = N_("time zone"), [P_LANG] = N_("language+region"), [P_KEYBOARD] = N_("keyboard"),
	[P_EASE] = N_("ease of access"), [P_ABOUT] = N_("about"), [P_WEATHER] = N_("weather"),
	[P_PHOTOS] = N_("photos+camera"),
};

/* deep links: open(link) shows `page` with `parent` (or the root) under it */
static const struct { const char *link; int page, parent; } LINKS[] = {
	{"theme", P_THEME, 0}, {"start", P_THEME, 0}, {"accent", P_ACCENT, P_THEME}, {"lock", P_LOCK, 0},
	{"wifi", P_WIFI, 0}, {"wi-fi", P_WIFI, 0}, {"bluetooth", P_BT, 0}, {"bt", P_BT, 0},
	{"airplane", P_AIR, 0}, {"notifications", P_NOTIF, 0}, {"datetime", P_DATETIME, 0},
	{"timezone", P_TZ, P_DATETIME}, {"language", P_LANG, 0}, {"region", P_LANG, 0},
	{"keyboard", P_KEYBOARD, 0}, {"ease", P_EASE, 0}, {"display", P_DISPLAY, 0},
	{"brightness", P_DISPLAY, 0}, {"sounds", P_SOUNDS, 0}, {"sound", P_SOUNDS, 0},
	{"about", P_ABOUT, 0}, {"weather", P_WEATHER, 0}, {"photos", P_PHOTOS, 0},
};

/* time zones offered by date+time (only those present in the system tzdata are listed) */
static const struct { const char *id, *city; } TZS[] = {
	{"UTC", N_("Coordinated Universal Time")}, {"Pacific/Honolulu", N_("Honolulu")},
	{"America/Anchorage", N_("Anchorage")}, {"America/Los_Angeles", N_("Los Angeles")},
	{"America/Denver", N_("Denver")}, {"America/Chicago", N_("Chicago")},
	{"America/Mexico_City", N_("Mexico City")}, {"America/Bogota", N_("Bogotá")},
	{"America/New_York", N_("New York")}, {"America/Halifax", N_("Halifax")},
	{"America/Sao_Paulo", N_("São Paulo")}, {"America/Argentina/Buenos_Aires", N_("Buenos Aires")},
	{"Atlantic/Reykjavik", N_("Reykjavík")}, {"Europe/London", N_("London")}, {"Europe/Dublin", N_("Dublin")},
	{"Europe/Lisbon", N_("Lisbon")}, {"Africa/Lagos", N_("Lagos")}, {"Europe/Paris", N_("Paris")},
	{"Europe/Berlin", N_("Berlin")}, {"Europe/Madrid", N_("Madrid")}, {"Europe/Rome", N_("Rome")},
	{"Europe/Amsterdam", N_("Amsterdam")}, {"Europe/Stockholm", N_("Stockholm")},
	{"Europe/Warsaw", N_("Warsaw")}, {"Europe/Athens", N_("Athens")}, {"Europe/Helsinki", N_("Helsinki")},
	{"Europe/Kyiv", N_("Kyiv")}, {"Africa/Cairo", N_("Cairo")}, {"Africa/Johannesburg", N_("Johannesburg")},
	{"Europe/Istanbul", N_("Istanbul")}, {"Europe/Moscow", N_("Moscow")}, {"Asia/Riyadh", N_("Riyadh")},
	{"Asia/Tehran", N_("Tehran")}, {"Asia/Dubai", N_("Dubai")}, {"Asia/Baku", N_("Baku")},
	{"Asia/Karachi", N_("Karachi")}, {"Asia/Kolkata", N_("Kolkata")}, {"Asia/Dhaka", N_("Dhaka")},
	{"Asia/Bangkok", N_("Bangkok")}, {"Asia/Shanghai", N_("Shanghai")}, {"Asia/Hong_Kong", N_("Hong Kong")},
	{"Asia/Singapore", N_("Singapore")}, {"Asia/Tokyo", N_("Tokyo")}, {"Asia/Seoul", N_("Seoul")},
	{"Australia/Perth", N_("Perth")}, {"Australia/Sydney", N_("Sydney")}, {"Pacific/Auckland", N_("Auckland")},
};
#define NTZ ((int)ARRAY_LEN(TZS))

/* physical keyboard layouts (xkb "layout(variant)"); names are shown in their own language */
static const struct { const char *code, *name; } KBL[] = {
	{"", N_("automatic")}, {"us", "English (US)"}, {"us(intl)", "English (US, international)"},
	{"gb", "English (UK)"}, {"tr", "Türkçe Q"}, {"tr(f)", "Türkçe F"}, {"de", "Deutsch"},
	{"fr", "Français"}, {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"},
	{"br", "Português (Brasil)"}, {"nl", "Nederlands"}, {"se", "Svenska"}, {"pl", "Polski"},
	{"ru", "Русский"}, {"gr", "Ελληνικά"}, {"us(dvorak)", "Dvorak"}, {"us(colemak)", "Colemak"},
};

static const double SCALES[] = {0, 1.0, 1.25, 1.5, 1.75, 2.0};
static const int TIMEOUTS[] = {30, 60, 180, 300, 600, 0};
static const char *const TIMEOUT_NAMES[] = {N_("30 seconds"), N_("1 minute"), N_("3 minutes"),
                                            N_("5 minutes"), N_("10 minutes"), N_("never")};

enum { PIN_NONE, PIN_SET, PIN_CHANGE, PIN_REMOVE };
enum { PICK_START = 1, PICK_LOCK = 2 };

typedef struct { char mac[18]; char name[64]; bool connected, busy; } BtDev;

static struct {
	bool ready;
	Nav nav;
	Pager pivot;
	Scroll root_scroll[2];
	Scroll scroll[P_COUNT];
	double content_h[P_COUNT];
	AppBar bar;
	SysInfo si;
	double si_t;
	/* lock screen password */
	int pin_mode;
	char pin_new[24];
	/* wi-fi */
	char wifi_target[64];     /* network being connected to */
	char wifi_pw_ssid[64];    /* network the password dialog is for */
	double wifi_t0;
	/* bluetooth paired devices (bluetoothctl) */
	BtDev bt[16];
	int nbt;
	bool bt_loading, bt_loaded;
	/* sliders being dragged */
	double vol, bright, text_scale;
	bool text_drag;
	int pend_vol, pend_bright;
	Timer *throttle, *defer, *apply;
	bool throttle_armed;
	const char *defer_app;
	/* time zones */
	bool tz_ready;
	int tz_off[NTZ];          /* seconds east of UTC, now */
	bool tz_ok[NTZ];
	int tz_order[NTZ], ntz;
	char sys_tz[64];
	int shots;                /* screenshots in ~/Pictures/Screenshots */
	char kbd_test[128];
	int last_sig;
} S;

static UI *ui_now(void) { return &sh.ui; }

/* ------------------------------------------------------------- helpers --- */

static void apply_cb(void *ud) { shell_apply_settings(); }

/* Settings that need shell_apply_settings() are applied right after the current frame
 * (dialog callbacks run while drawing; changing the output scale mid-frame is unsafe). */
static void apply_soon(void) {
	config_save_soon();
	if (!S.apply) S.apply = timer_add(0, false, apply_cb, NULL);
	timer_set(S.apply, 1);
}

static void throttle_cb(void *ud) {
	S.throttle_armed = false;
	if (S.pend_vol >= 0) { sys_set_volume(S.pend_vol); S.pend_vol = -1; }
	if (S.pend_bright >= 0) { sys_set_brightness(S.pend_bright); S.pend_bright = -1; }
	shell_invalidate();
}

/* slider drags spawn helper processes (wpctl, brightnessctl): at most ~11 per second */
static void throttle_kick(void) {
	if (!S.throttle) S.throttle = timer_add(0, false, throttle_cb, NULL);
	if (!S.throttle_armed) { S.throttle_armed = true; timer_set(S.throttle, 90); }
}

static void defer_cb(void *ud) {
	if (S.defer_app) shell_open_builtin(S.defer_app, NULL);
	S.defer_app = NULL;
}

static void open_app_soon(const char *id) {
	S.defer_app = id;
	if (!S.defer) S.defer = timer_add(0, false, defer_cb, NULL);
	timer_set(S.defer, 1);
}

static void refresh_info(double now) {
	if (S.si_t > 0 && now - S.si_t < 10) return;
	sys_info(&S.si);
	S.si_t = now;
}

static int notif_count(void) {
	int c = 0;
	for (int i = 0; i < sh.notifs.len; i++) if (!sh.notifs.items[i]->internal) c++;
	return c;
}

static const char *on_off(bool v) { return v ? _("on") : _("off"); }

/* ------------------------------------------------------------ time zones - */

static char g_env_tz[128];
static bool g_env_tz_saved, g_env_tz_had;

static void tz_apply(void) {
	if (!g_env_tz_saved) {
		const char *e = getenv("TZ");
		g_env_tz_had = e != NULL;
		str_copy(g_env_tz, sizeof g_env_tz, e ? e : "");
		g_env_tz_saved = true;
	}
	if (cfg.home_tz[0]) setenv("TZ", cfg.home_tz, 1);
	else if (g_env_tz_had) setenv("TZ", g_env_tz, 1);
	else unsetenv("TZ");
	tzset();
	shell_invalidate();
}

static void detect_sys_tz(void) {
	S.sys_tz[0] = 0;
	if (g_env_tz_had && g_env_tz[0]) { str_copy(S.sys_tz, sizeof S.sys_tz, g_env_tz[0] == ':' ? g_env_tz + 1 : g_env_tz); return; }
	char buf[256];
	ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = 0;
		char *z = strstr(buf, "zoneinfo/");
		if (z) { str_copy(S.sys_tz, sizeof S.sys_tz, z + 9); return; }
	}
	char *t = file_read_all("/etc/timezone", NULL);
	if (t) { str_copy(S.sys_tz, sizeof S.sys_tz, str_trim(t)); free(t); }
}

static int tz_cmp(const void *a, const void *b) {
	int i = *(const int *)a, j = *(const int *)b;
	if (S.tz_off[i] != S.tz_off[j]) return S.tz_off[i] < S.tz_off[j] ? -1 : 1;
	return app_collate(tr(TZS[i].city), tr(TZS[j].city));
}

static void tz_prepare(void) {
	if (S.tz_ready) return;
	S.tz_ready = true;
	if (!g_env_tz_saved) tz_apply();
	detect_sys_tz();
	gint64 now = (gint64)time(NULL);
	S.ntz = 0;
	for (int i = 0; i < NTZ; i++) {
		GTimeZone *z = g_time_zone_new_identifier(TZS[i].id);
		S.tz_ok[i] = z != NULL;
		if (!z) continue;
		int iv = g_time_zone_find_interval(z, G_TIME_TYPE_UNIVERSAL, now);
		S.tz_off[i] = iv >= 0 ? g_time_zone_get_offset(z, iv) : 0;
		g_time_zone_unref(z);
		S.tz_order[S.ntz++] = i;
	}
	qsort(S.tz_order, (size_t)S.ntz, sizeof(int), tz_cmp);
}

static int tz_find(const char *id) {
	for (int i = 0; i < NTZ; i++) if (str_eq(TZS[i].id, id)) return i;
	return -1;
}

static void fmt_utc(char *buf, size_t n, int off) {
	int a = abs(off) / 60;
	snprintf(buf, n, "UTC%c%02d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
}

/* "(UTC+03:00) Istanbul" for a configured zone, "automatic" otherwise */
static void tz_label(char *buf, size_t n) {
	if (!cfg.home_tz[0]) { snprintf(buf, n, "%s", _("automatic")); return; }
	tz_prepare();
	int i = tz_find(cfg.home_tz);
	if (i < 0 || !S.tz_ok[i]) { str_copy(buf, n, cfg.home_tz); return; }
	char u[16];
	fmt_utc(u, sizeof u, S.tz_off[i]);
	snprintf(buf, n, "(%s) %s", u, tr(TZS[i].city));
}

/* -------------------------------------------------------------- bluetooth - */

static void bt_done(int status, char *out, size_t len, void *ud) {
	S.bt_loading = false;
	S.bt_loaded = true;
	S.nbt = 0;
	bool conn_part = false;
	for (char *save = NULL, *l = strtok_r(out ? out : (char[]){0}, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (str_starts(l, "---")) { conn_part = true; continue; }
		if (!str_starts(l, "Device ") || strlen(l) < 24) continue;
		char mac[18];
		memcpy(mac, l + 7, 17);
		mac[17] = 0;
		if (conn_part) {
			for (int i = 0; i < S.nbt; i++) if (str_eq(S.bt[i].mac, mac)) S.bt[i].connected = true;
			continue;
		}
		bool dup = false;
		for (int i = 0; i < S.nbt; i++) if (str_eq(S.bt[i].mac, mac)) dup = true;
		if (dup || S.nbt >= (int)ARRAY_LEN(S.bt)) continue;
		BtDev *d = &S.bt[S.nbt++];
		*d = (BtDev){0};
		str_copy(d->mac, sizeof d->mac, mac);
		str_copy(d->name, sizeof d->name, l[24] ? str_trim(l + 25) : mac);
	}
	shell_invalidate();
}

static void bt_refresh(void) {
	if (S.bt_loading || !proc_exists("bluetoothctl")) return;
	S.bt_loading = true;
	const char *to = proc_exists("timeout") ? "timeout 6 " : "";
	char *cmd = xasprintf("%sbluetoothctl devices Paired 2>/dev/null || %sbluetoothctl paired-devices 2>/dev/null; "
	                      "echo ---; %sbluetoothctl devices Connected 2>/dev/null", to, to, to);
	if (!proc_run_sh(cmd, bt_done, NULL)) S.bt_loading = false;
	free(cmd);
}

static void bt_action_done(int status, char *out, size_t len, void *ud) {
	for (int i = 0; i < S.nbt; i++) S.bt[i].busy = false;
	bt_refresh();
}

static void bt_toggle_device(BtDev *d) {
	if (d->busy) return;
	d->busy = true;
	char *cmd = xasprintf("%sbluetoothctl %s %s", proc_exists("timeout") ? "timeout 15 " : "",
	                      d->connected ? "disconnect" : "connect", d->mac);
	if (!proc_run_sh(cmd, bt_action_done, NULL)) d->busy = false;
	free(cmd);
}

/* ------------------------------------------------------- screenshot count - */

static char *shots_dir(void) { return path_join(home_dir(), "Pictures/Screenshots"); }

static void count_shots(void) {
	S.shots = 0;
	char *dir = shots_dir();
	DIR *d = opendir(dir);
	free(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && S.shots < 100000)
		if (e->d_name[0] != '.' && (str_ends(e->d_name, ".png") || str_ends(e->d_name, ".jpg"))) S.shots++;
	closedir(d);
}

/* ------------------------------------------------------------ navigation - */

static void page_enter(int page) {
	S.scroll[page] = (Scroll){0};
	S.bar = (AppBar){0};
	switch (page) {
	case P_WIFI: {
		int n;
		sys_wifi_list(&n);
		if (sys_wifi().enabled && !sys_wifi_scanning()) sys_wifi_scan();
		break;
	}
	case P_BT: bt_refresh(); break;
	case P_ABOUT: S.si_t = 0; break;
	case P_TZ: case P_DATETIME: S.tz_ready = false; tz_prepare(); break;
	case P_PHOTOS: count_shots(); break;
	default: break;
	}
}

static void go(int page) {
	page_enter(page);
	nav_push(&S.nav, ui_now(), page);
}

static void ensure_ready(void) {
	if (S.ready) return;
	nav_init(&S.nav, P_ROOT);
	S.pend_vol = S.pend_bright = -1;
	S.ready = true;
}

/* --------------------------------------------------------- page widgets -- */

typedef struct { UI *ui; cairo_t *cr; const Theme *th; double x, w, y; } Pg;

static void pg_text(Pg *p, const char *s) {
	p->y += gfx_text(p->cr, s, p->x, p->y, 18, FW_SEMILIGHT, p->th->sub, TA_LEFT, p->w, 0) + 16;
}

static void pg_label(Pg *p, const char *s) {
	p->y += gfx_text(p->cr, s, p->x, p->y, 16, FW_REGULAR, p->th->sub, TA_LEFT, p->w, 1) + 4;
}

static bool pg_toggle(Pg *p, const char *key, const char *label, bool *v) {
	double h = gfx_line_height(p->cr, 20, FW_SEMILIGHT) + 40;
	bool ch = ui_toggle(p->ui, ui_id(key), R(p->x, p->y, p->w, h), label, v);
	p->y += h + 14;
	return ch;
}

/* Windows Phone combo box: grey header, bordered box showing the current value */
static bool pg_combo(Pg *p, const char *key, const char *label, const char *value, const Color *swatch) {
	if (label) pg_label(p, label);
	cairo_t *cr = p->cr;
	const Theme *th = p->th;
	double h = MAX(46, gfx_line_height(cr, 20, FW_SEMILIGHT) + 16);
	Rect b = R(p->x, p->y, p->w, h);
	UiId id = ui_id(key);
	ui_hit(p->ui, id, b);
	bool pr = ui_pressed(p->ui, id);
	gfx_fill_rect(cr, b, pr ? th->accent : th->dark ? COL_BLACK : COL_WHITE);
	gfx_stroke_rect(cr, b, 2, pr ? th->accent : col_alpha(th->fg, 0.75f));
	double tx = b.x + 12;
	if (swatch) {
		gfx_fill_rect(cr, R(tx, rect_cy(b) - 12, 24, 24), *swatch);
		if (pr) gfx_stroke_rect(cr, R(tx, rect_cy(b) - 12, 24, 24), 2, th->on_accent);
		tx += 36;
	}
	gfx_text(cr, value, tx, rect_cy(b) - gfx_line_height(cr, 20, FW_SEMILIGHT) / 2, 20, FW_SEMILIGHT,
	         pr ? th->on_accent : th->fg, TA_LEFT, rect_right(b) - tx - 12, 1);
	p->y += h + 20;
	return ui_clicked(p->ui, id);
}

static double button_w(Pg *p, const char *label) {
	return CLAMP(gfx_text_size(p->cr, label, 20, FW_SEMILIGHT, 0, 1).w + 44, 150, p->w);
}

static bool pg_button(Pg *p, const char *key, const char *label, bool enabled) {
	double h = MAX(50, gfx_line_height(p->cr, 20, FW_SEMILIGHT) + 20);
	bool c = ui_button_enabled(p->ui, ui_id(key), R(p->x, p->y, button_w(p, label), h), label, BTN_OUTLINE, enabled);
	p->y += h + 18;
	return c;
}

/* two buttons side by side when they fit, stacked otherwise; returns 1, 2 or 0 */
static int pg_buttons2(Pg *p, const char *k1, const char *l1, bool e1, const char *k2, const char *l2, bool e2) {
	double w1 = button_w(p, l1), w2 = button_w(p, l2);
	if (w1 + w2 + 12 > p->w) {
		int r = pg_button(p, k1, l1, e1) ? 1 : 0;
		if (pg_button(p, k2, l2, e2)) r = 2;
		return r;
	}
	double h = MAX(50, gfx_line_height(p->cr, 20, FW_SEMILIGHT) + 20);
	int r = 0;
	if (ui_button_enabled(p->ui, ui_id(k1), R(p->x, p->y, w1, h), l1, BTN_OUTLINE, e1)) r = 1;
	if (ui_button_enabled(p->ui, ui_id(k2), R(p->x + w1 + 12, p->y, w2, h), l2, BTN_OUTLINE, e2)) r = 2;
	p->y += h + 18;
	return r;
}

/* labelled slider with the value on the right; *released is set when the finger lifts */
static bool pg_slider(Pg *p, const char *key, const char *label, const char *value, double *v, double lo, double hi,
                      bool *released) {
	if (label) {
		gfx_text(p->cr, label, p->x, p->y, 20, FW_SEMILIGHT, p->th->fg, TA_LEFT, p->w * 0.7, 1);
		if (value) gfx_text(p->cr, value, p->x + p->w, p->y, 20, FW_SEMILIGHT, p->th->sub, TA_RIGHT, p->w * 0.3, 1);
		p->y += gfx_line_height(p->cr, 20, FW_SEMILIGHT) + 4;
	}
	UiId id = ui_id(key);
	bool ch = ui_slider(p->ui, id, R(p->x, p->y, p->w, 44), v, lo, hi);
	if (released) *released = ui_released(p->ui, id);
	p->y += 60;
	return ch;
}

/* a row like the root list: title + optional status line, tappable */
static bool pg_row(Pg *p, const char *key, const char *title, const char *sub, Color title_c) {
	cairo_t *cr = p->cr;
	double th = gfx_line_height(cr, 24, FW_SEMILIGHT), sh_ = sub ? gfx_line_height(cr, 15, FW_REGULAR) : 0;
	double h = th + sh_ + 18;
	Rect r = R(p->x - 24, p->y, p->w + 48, h);
	UiId id = ui_id(key);
	ui_hit(p->ui, id, r);
	if (ui_pressed(p->ui, id)) gfx_fill_rect(cr, r, col_alpha(p->th->fg, 0.14f));
	gfx_text(cr, title, p->x, p->y + 8, 24, FW_SEMILIGHT, title_c, TA_LEFT, p->w, 1);
	if (sub) gfx_text(cr, sub, p->x, p->y + 8 + th - 2, 15, FW_REGULAR, p->th->sub, TA_LEFT, p->w, 1);
	p->y += h;
	return ui_clicked(p->ui, id);
}

/* label + value pair (about page) */
static void pg_info(Pg *p, const char *label, const char *value) {
	if (!value || !*value) return;
	p->y += gfx_text(p->cr, label, p->x, p->y, 16, FW_REGULAR, p->th->sub, TA_LEFT, p->w, 1);
	p->y += gfx_text(p->cr, value, p->x, p->y, 22, FW_SEMILIGHT, p->th->fg, TA_LEFT, p->w, 3) + 14;
}

/* ---------------------------------------------------------------- dialogs - */

static void theme_list_cb(int i, void *ud) {
	if (i < 0 || (i == 0) == cfg.dark) return;
	cfg.dark = i == 0;
	apply_soon();
}

static void icon_mode_cb(int i, void *ud) {
	static const char *const M[] = {"auto", "font", "vector"};
	if (i < 0 || i > 2) return;
	str_copy(cfg.icon_mode, sizeof cfg.icon_mode, M[i]);
	apply_soon();
}

static void timeout_cb(int b, void *ud) {
	if (b != 0) return;
	int i = dialog_picker_get();
	if (i < 0 || i >= (int)ARRAY_LEN(TIMEOUTS)) return;
	cfg.screen_timeout = TIMEOUTS[i];
	config_save_soon();
}

static void lockbg_cb(int i, void *ud);

static void picked_cb(const char *path, void *ud) {
	if (!path || !*path) return;
	if ((intptr_t)ud == PICK_START) {
		str_copy(cfg.wallpaper, sizeof cfg.wallpaper, path);
		cfg.start_wallpaper = true;
	} else {
		str_copy(cfg.lock_wallpaper, sizeof cfg.lock_wallpaper, path);
	}
	wallpaper_changed();
	config_save_soon();
	shell_invalidate();
}

static void path_input_cb(int b, void *ud) {
	if (b != 0) return;
	char path[512];
	str_copy(path, sizeof path, dialog_input_text());
	str_trim(path);
	if (str_starts(path, "~/")) {
		char *full = path_join(home_dir(), path + 2);
		str_copy(path, sizeof path, full);
		free(full);
	}
	if (!file_exists(path) || is_dir(path)) {
		dialog_message(_("picture not found"), _("Check the path and try again."), _("ok"), NULL, NULL, NULL);
		return;
	}
	picked_cb(path, ud);
}

static void choose_picture(int target) {
	if (photos_pick) { photos_pick("settings", picked_cb, (void *)(intptr_t)target); return; }
	/* Photos is not built in: ask for a file path instead */
	dialog_input(_("choose a picture"), _("path to an image file"), target == PICK_START ? cfg.wallpaper : cfg.lock_wallpaper,
	             TB_URL, path_input_cb, (void *)(intptr_t)target);
}

static void lockbg_cb(int i, void *ud) {
	if (i == 0 && cfg.lock_wallpaper[0]) {
		cfg.lock_wallpaper[0] = 0;
		wallpaper_changed();
		config_save_soon();
	} else if (i == 1) {
		choose_picture(PICK_LOCK);
	}
}

static void scale_cb(int i, void *ud) {
	if (i < 0 || i >= (int)ARRAY_LEN(SCALES)) return;
	cfg.ui_scale = SCALES[i];
	apply_soon();
}

static void kbl_cb(int i, void *ud) {
	if (i < 0 || i >= (int)ARRAY_LEN(KBL)) return;
	str_copy(cfg.kb_layout, sizeof cfg.kb_layout, KBL[i].code);
	apply_soon();
}

/* ---- lock screen password (a numeric PIN, stored as SHA-256) */

static void pin_ask_current(void);
static void pin_ask_new(void);

static bool pin_valid(const char *s) {
	size_t n = strlen(s);
	if (n < 4 || n > 12) return false;
	for (const char *c = s; *c; c++) if (*c < '0' || *c > '9') return false;
	return true;
}

static void pin_end(void) {
	S.pin_mode = PIN_NONE;
	memset(S.pin_new, 0, sizeof S.pin_new);
	config_save_soon();
	shell_invalidate();
}

static void pin_retry_current_cb(int b, void *ud) { pin_ask_current(); }
static void pin_retry_new_cb(int b, void *ud) { pin_ask_new(); }

static void pin_confirm_cb(int b, void *ud) {
	if (b != 0) { pin_end(); return; }
	if (!str_eq(dialog_input_text(), S.pin_new)) {
		dialog_message(_("passwords don't match"), _("The passwords you typed don't match. Type your new password again."),
		               _("ok"), NULL, pin_retry_new_cb, NULL);
		return;
	}
	sha256_hex(S.pin_new, strlen(S.pin_new), cfg.pin_hash);
	pin_end();
}

static void pin_new_cb(int b, void *ud) {
	if (b != 0) { pin_end(); return; }
	const char *t = dialog_input_text();
	if (!pin_valid(t)) {
		dialog_message(_("password"), _("Use 4 to 12 digits for your password."), _("ok"), NULL, pin_retry_new_cb, NULL);
		return;
	}
	str_copy(S.pin_new, sizeof S.pin_new, t);
	dialog_input(_("confirm password"), _("type the new password again"), "", TB_PASSWORD | TB_NUMBER, pin_confirm_cb, NULL);
}

static void pin_ask_new(void) {
	dialog_input(_("new password"), _("4 to 12 digits"), "", TB_PASSWORD | TB_NUMBER, pin_new_cb, NULL);
}

static void pin_current_cb(int b, void *ud) {
	if (b != 0) { pin_end(); return; }
	const char *t = dialog_input_text();
	char h[65];
	sha256_hex(t, strlen(t), h);
	if (!str_eq(h, cfg.pin_hash)) {
		dialog_message(_("incorrect password"), _("The password you typed isn't right. Try again."), _("ok"), NULL,
		               pin_retry_current_cb, NULL);
		return;
	}
	if (S.pin_mode == PIN_REMOVE) { cfg.pin_hash[0] = 0; pin_end(); return; }
	pin_ask_new();
}

static void pin_ask_current(void) {
	dialog_input(_("current password"), _("type your current password"), "", TB_PASSWORD | TB_NUMBER, pin_current_cb, NULL);
}

static void pin_start(int mode) {
	S.pin_mode = mode;
	if (mode == PIN_SET) pin_ask_new();
	else pin_ask_current();
}

/* ---- wi-fi */

static void wifi_connect(const char *ssid, const char *pw) {
	str_copy(S.wifi_target, sizeof S.wifi_target, ssid);
	S.wifi_t0 = now_sec();
	sys_wifi_connect(ssid, pw);
}

static void wifi_pw_cb(int b, void *ud) {
	if (b != 0 || !S.wifi_pw_ssid[0]) return;
	wifi_connect(S.wifi_pw_ssid, dialog_input_text());
	S.wifi_pw_ssid[0] = 0;
}

static void wifi_tap(const WifiNet *w, bool connected) {
	if (connected) {
		char *txt = xasprintf(_("Signal strength: %d%%\nSecurity: %s"), w->signal, w->secured ? _("secured") : _("open"));
		dialog_message(w->ssid, txt, _("ok"), NULL, NULL, NULL);
		free(txt);
		return;
	}
	if (w->secured && !w->known) {
		str_copy(S.wifi_pw_ssid, sizeof S.wifi_pw_ssid, w->ssid);
		dialog_input(w->ssid, _("password"), "", TB_PASSWORD, wifi_pw_cb, NULL);
	} else {
		wifi_connect(w->ssid, NULL);
	}
}

/* ----------------------------------------------------------- root pivot -- */

typedef struct { const char *title; char sub[192]; int page; const char *app; } Item;

static int root_items(int pivot, Item *it) {
	int n = 0;
	char t[64];
#define ADD(T, P, A, ...) do { it[n] = (Item){.title = (T), .page = (P), .app = (A)}; \
		snprintf(it[n].sub, sizeof it[n].sub, __VA_ARGS__); n++; } while (0)
	if (pivot == 0) {
		ADD(_("start+theme"), P_THEME, NULL, "%s, %s", cfg.dark ? _("dark") : _("light"), accent_name(cfg.accent));
		if (!sys_has_audio()) ADD(_("ringtones+sounds"), P_SOUNDS, NULL, "%s", _("no sound device"));
		else if (sys_muted()) ADD(_("ringtones+sounds"), P_SOUNDS, NULL, "%s", _("muted"));
		else ADD(_("ringtones+sounds"), P_SOUNDS, NULL, _("volume %d%%"), sys_volume());
		int nn = notif_count();
		if (cfg.quiet_hours) ADD(_("notifications+actions"), P_NOTIF, NULL, "%s", _("quiet hours on"));
		else if (nn) ADD(_("notifications+actions"), P_NOTIF, NULL, nn == 1 ? _("%d notification") : _("%d notifications"), nn);
		else ADD(_("notifications+actions"), P_NOTIF, NULL, "%s", cfg.toasts ? _("banners on") : _("banners off"));
		ADD(_("lock screen"), P_LOCK, NULL, "%s", cfg.pin_hash[0] ? _("password on") : _("password off"));
		WifiInfo w = sys_wifi();
		if (!w.available && !sys_demo()) ADD(_("wi-fi"), P_WIFI, NULL, "%s", _("not available"));
		else if (!w.enabled) ADD(_("wi-fi"), P_WIFI, NULL, "%s", _("off"));
		else if (w.connected && w.ssid[0]) ADD(_("wi-fi"), P_WIFI, NULL, _("connected to %s"), w.ssid);
		else ADD(_("wi-fi"), P_WIFI, NULL, "%s", _("on, not connected"));
		ADD(_("airplane mode"), P_AIR, NULL, "%s", on_off(sys_airplane()));
		ADD(_("bluetooth"), P_BT, NULL, "%s", sys_bluetooth_available() ? on_off(sys_bluetooth_enabled()) : _("not available"));
		Battery b = sys_battery();
		if (b.present) ADD(_("battery saver"), -1, "battery", _("%s, %d%% battery remaining"), on_off(cfg.battery_saver), b.pct);
		else ADD(_("battery saver"), -1, "battery", "%s", on_off(cfg.battery_saver));
		char fr[32];
		human_size(S.si.disk_free, fr, sizeof fr);
		if (S.si.disk_total > 0) ADD(_("storage sense"), -1, "storage", _("%s free"), fr);
		else ADD(_("storage sense"), -1, "storage", "%s", _("see what's using space"));
		char sc[32];
		if (cfg.ui_scale > 0) snprintf(sc, sizeof sc, "%d%%", (int)lround(cfg.ui_scale * 100));
		else str_copy(sc, sizeof sc, _("automatic"));
		if (sys_has_backlight()) ADD(_("display"), P_DISPLAY, NULL, _("brightness %d%%, scale %s"), sys_brightness(), sc);
		else ADD(_("display"), P_DISPLAY, NULL, _("scale %s"), sc);
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		i18n_fmt_time(t, sizeof t, &tm, cfg.h24);
		char tz[96];
		tz_label(tz, sizeof tz);
		ADD(_("date+time"), P_DATETIME, NULL, "%s, %s", t, tz);
		if (cfg.lang[0]) ADD(_("language+region"), P_LANG, NULL, "%s", i18n_native_name(i18n_lang()));
		else ADD(_("language+region"), P_LANG, NULL, "%s (%s)", i18n_native_name(i18n_lang()), _("automatic"));
		bool en = strstr(cfg.osk_layouts, "en") != NULL, trk = strstr(cfg.osk_layouts, "tr") != NULL;
		ADD(_("keyboard"), P_KEYBOARD, NULL, "%s%s%s", en ? "English" : "", en && trk ? ", " : "", trk ? "Türkçe" : "");
		ADD(_("ease of access"), P_EASE, NULL, _("text size %d%%"), (int)lround(cfg.text_scale * 100));
		ADD(_("about"), P_ABOUT, NULL, "metrowl %s", METROWL_VERSION);
	} else {
		ADD(_("weather"), P_WEATHER, NULL, "%s%s%s", cfg.celsius ? "°C" : "°F", cfg.city[0] ? ", " : "", cfg.city);
		ADD(_("photos+camera"), P_PHOTOS, NULL, "%s", _("screenshots"));
		ADD(_("store"), -1, "store", "%s", _("apps and games"));
	}
#undef ADD
	return n;
}

static void root_page(UI *ui, int pivot, Rect r, void *ud) {
	cairo_t *cr = ui->cr;
	Item it[24];
	int n = root_items(pivot, it);
	double rh = gfx_line_height(cr, 26, FW_SEMILIGHT) + gfx_line_height(cr, 15, FW_REGULAR) + 22;
	double off = ui_scroll_begin(ui, ui_idn("set-root", pivot), &S.root_scroll[pivot], r, n * rh + 24);
	cairo_translate(cr, r.x, r.y);
	double w = MIN(r.w, 620);
	for (int i = 0; i < n; i++) {
		Rect rr = R(0, i * rh, w, rh);
		if (rr.y + rh < off || rr.y > off + r.h) continue;
		if (ui_row_ex(ui, ui_idn(pivot ? "set-arow" : "set-srow", i), rr, it[i].title, it[i].sub, ICON_NONE, NULL, false)) {
			if (it[i].page > 0) go(it[i].page);
			else if (it[i].app) shell_open_builtin(it[i].app, NULL);
		}
	}
	ui_scroll_end(ui);
}

/* ----------------------------------------------------------- inner pages - */

static void page_theme(Pg *p) {
	UI *ui = p->ui;
	if (pg_combo(p, "set-th-bg", _("Background"), cfg.dark ? _("dark") : _("light"), NULL)) {
		const char *items[] = {_("dark"), _("light")};
		dialog_list(_("Background"), items, 2, cfg.dark ? 0 : 1, theme_list_cb, NULL);
	}
	Color ac = RGBX(ACCENT_RGB[CLAMP(cfg.accent, 0, ACCENT_COUNT - 1)]);
	if (pg_combo(p, "set-th-accent", _("Accent color"), accent_name(cfg.accent), &ac)) go(P_ACCENT);
	if (pg_toggle(p, "set-th-more", _("Show more tiles"), &cfg.more_tiles)) apply_soon();

	/* start background picture */
	pg_label(p, _("Start background"));
	Image *wp = cfg.start_wallpaper ? wallpaper_start() : NULL;
	if (cfg.start_wallpaper && cfg.wallpaper[0]) {
		double tw = 96, th = 160;
		Rect pr = R(p->x, p->y + 4, tw, th);
		gfx_fill_rect(p->cr, pr, p->th->chrome2);
		if (wp) gfx_image_cover(p->cr, wp, pr, 1);
		/* a few see-through tiles, the way Start shows the picture */
		for (int i = 0; i < 6; i++) {
			Rect t = R(pr.x + 8 + (i % 2) * 42, pr.y + 10 + (i / 2) * 42, 38, 38);
			gfx_fill_rect(p->cr, t, col_alpha(p->th->accent, 0.25f));
		}
		gfx_text(p->cr, path_basename(cfg.wallpaper), p->x + tw + 16, p->y + 4, 16, FW_REGULAR, p->th->sub, TA_LEFT,
		         p->w - tw - 16, 3);
		p->y += th + 20;
	} else {
		pg_text(p, _("Choose a photo to show through the tiles on Start."));
	}
	int b = pg_buttons2(p, "set-th-pick", _("choose photo"), true, "set-th-rm", _("remove photo"), cfg.start_wallpaper);
	if (b == 1) choose_picture(PICK_START);
	else if (b == 2) {
		cfg.start_wallpaper = false;
		cfg.wallpaper[0] = 0;
		wallpaper_changed();
		config_save_soon();
	}
	if (pg_toggle(p, "set-th-parallax", _("Parallax effect"), &cfg.parallax)) config_save_soon();
	if (pg_toggle(p, "set-th-live", _("Live tiles"), &cfg.live_tiles)) { config_save_soon(); shell_invalidate(); }
	if (pg_toggle(p, "set-th-mono", _("Monochrome app icons"), &cfg.mono_icons)) apply_soon();
	pg_text(p, _("Draws the icons of other apps in white on their tiles, like the built-in apps."));
	int mi = str_eq(cfg.icon_mode, "font") ? 1 : str_eq(cfg.icon_mode, "vector") ? 2 : 0;
	const char *modes[] = {_("automatic"), _("Segoe MDL2 font"), _("built-in vector icons")};
	if (pg_combo(p, "set-th-icons", _("Icon style"), modes[mi], NULL))
		dialog_list(_("Icon style"), modes, 3, mi, icon_mode_cb, NULL);
	pg_text(p, icons_using_font() ? _("Icons are drawn with the Segoe MDL2 Assets font.")
	                              : _("Icons are drawn from built-in outlines. Install the Segoe MDL2 Assets font to use it."));
	(void)ui;
}

static void page_accent(Pg *p) {
	UI *ui = p->ui;
	cairo_t *cr = p->cr;
	const Theme *th = p->th;
	int cols = MAX(4, (int)((p->w + 12) / (112 + 12)));
	double gap = 12, cell = MIN(112, (p->w - (cols - 1) * gap) / cols);
	int shown = cfg.accent;
	double name_y = p->y;
	p->y += gfx_line_height(cr, 26, FW_SEMILIGHT) + 14;
	for (int i = 0; i < ACCENT_COUNT; i++) {
		Rect r = R(p->x + (i % cols) * (cell + gap), p->y + (i / cols) * (cell + gap), cell, cell);
		UiId id = ui_idn("set-accent", i);
		ui_hit(ui, id, r);
		bool pr = ui_pressed(ui, id);
		if (pr) shown = i;
		Rect fr = pr ? rect_inset(r, 3) : r;
		gfx_fill_rect(cr, fr, RGBX(ACCENT_RGB[i]));
		if (i == cfg.accent) gfx_stroke_rect(cr, rect_inset(r, -5), 3, th->fg);
		if (ui_clicked(ui, id)) {
			if (i != cfg.accent) { cfg.accent = i; apply_soon(); }
			nav_pop(&S.nav, ui);
		}
	}
	gfx_text(cr, accent_name(shown), p->x, name_y, 26, FW_SEMILIGHT, RGBX(ACCENT_RGB[shown]), TA_LEFT, p->w, 1);
	p->y += ((ACCENT_COUNT + cols - 1) / cols) * (cell + gap);
}

/* sound pickers: "none", "default", then the installed sound files */
static int g_snd_ev;
static char **g_snd_paths;
static int g_snd_n;

static void sound_label(const char *path, char *out, size_t cap) {
	str_copy(out, cap, path_basename(path));
	char *dot = strrchr(out, '.');
	if (dot) *dot = 0;
	for (char *c = out; *c; c++) if (*c == '-' || *c == '_') *c = ' ';
}

static void sound_pick_cb(int i, void *ud) {
	char *dst = g_snd_ev == EV_NOTIF ? cfg.notif_sound : cfg.warn_sound;
	if (i == 0) str_copy(dst, sizeof cfg.notif_sound, "none");
	else if (i == 1) dst[0] = 0;
	else if (i >= 2 && i - 2 < g_snd_n) str_copy(dst, sizeof cfg.notif_sound, g_snd_paths[i - 2]);
	else return;
	config_save_soon();
	app_play_event(g_snd_ev);   /* let the user hear the choice */
}

static void sound_combo(Pg *p, const char *key, const char *label, const char *value, int ev) {
	char cur[128];
	if (str_eq(value, "none")) str_copy(cur, sizeof cur, _("none"));
	else if (!*value) str_copy(cur, sizeof cur, _("default"));
	else sound_label(value, cur, sizeof cur);
	if (!pg_combo(p, key, label, cur, NULL)) return;
	g_snd_ev = ev;
	g_snd_n = app_sound_list(&g_snd_paths);
	static char labels[96][96];
	static const char *items[96];
	int n = 0, sel = str_eq(value, "none") ? 0 : !*value ? 1 : -1;
	items[n++] = _("none");
	items[n++] = _("default");
	for (int i = 0; i < g_snd_n && n < 96; i++, n++) {
		sound_label(g_snd_paths[i], labels[n], sizeof labels[n]);
		items[n] = labels[n];
		if (str_eq(value, g_snd_paths[i])) sel = n;
	}
	dialog_list(label, items, n, sel, sound_pick_cb, NULL);
}

static void page_sounds(Pg *p) {
	if (!sys_has_audio()) {
		pg_text(p, _("No sound system was found. Install PipeWire (wpctl), PulseAudio (pactl) or ALSA (amixer) to control the volume."));
		return;
	}
	if (!ui_dragging(p->ui, ui_id("set-vol")) && S.pend_vol < 0) S.vol = sys_volume();
	char v[16];
	snprintf(v, sizeof v, "%d%%", (int)lround(S.vol));
	if (pg_slider(p, "set-vol", _("Volume"), v, &S.vol, 0, 100, NULL)) {
		S.pend_vol = (int)lround(S.vol);
		throttle_kick();
	}
	bool m = sys_muted();
	if (pg_toggle(p, "set-mute", _("Mute"), &m)) sys_set_mute(m);
	pg_text(p, _("When muted, apps and notifications don't play sounds. Alarms still ring."));
	if (pg_button(p, "set-snd-test", _("play a sound"), !m)) app_sound(SND_CHIME, false);
	sound_combo(p, "set-snd-notif", _("Notification sound"), cfg.notif_sound, EV_NOTIF);
	sound_combo(p, "set-snd-warn", _("Warning sound"), cfg.warn_sound, EV_WARN);
	if (pg_toggle(p, "set-snd-key", _("Key press sounds"), &cfg.key_sound)) config_save_soon();
	pg_text(p, _("Sounds come from the sound themes installed on the phone (for example sound-theme-freedesktop) and from ~/.local/share/sounds."));
}

static void page_notif(Pg *p) {
	if (pg_toggle(p, "set-n-toasts", _("Show notification banners"), &cfg.toasts)) config_save_soon();
	pg_text(p, _("Banners appear at the top of the screen when a notification arrives. Everything still goes to the action center."));
	if (pg_toggle(p, "set-n-quiet", _("Quiet hours"), &cfg.quiet_hours)) config_save_soon();
	pg_text(p, _("During quiet hours only urgent notifications show a banner."));
	int n = notif_count();
	char *t = n ? xasprintf(n == 1 ? _("%d notification in the action center") : _("%d notifications in the action center"), n)
	            : xstrdup(_("No notifications in the action center"));
	pg_text(p, t);
	free(t);
	if (pg_button(p, "set-n-clear", _("clear all notifications"), n > 0)) shell_notify_clear();
	pg_text(p, _("Pull down from the top of the screen to open the action center. Press and hold a quick action to open its settings."));
}

static void page_lock(Pg *p) {
	const char *bgs[] = {_("accent pattern"), _("photo")};
	bool photo = cfg.lock_wallpaper[0] != 0;
	if (pg_combo(p, "set-l-bg", _("Background"), bgs[photo], NULL))
		dialog_list(_("Background"), bgs, 2, photo, lockbg_cb, NULL);
	/* small preview of the lock screen */
	double tw = 110, th = 184;
	Rect pr = R(p->x, p->y, tw, th);
	Image *img = photo ? wallpaper_lock(sh.w, sh.h) : NULL;
	if (img) gfx_image_cover(p->cr, img, pr, 1);
	else if (photo) gfx_fill_rect(p->cr, pr, p->th->chrome2);
	else wallpaper_draw_generated(p->cr, pr, p->th->accent, p->th->dark, 7);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char t[32], d[64];
	i18n_fmt_time(t, sizeof t, &tm, cfg.h24);
	double ts = gfx_text_scale() > 0 ? gfx_text_scale() : 1;   /* a picture: keep it to scale */
	gfx_text(p->cr, t, pr.x + 8, pr.y + th - 62, 24 / ts, FW_LIGHT, COL_WHITE, TA_LEFT, 0, 1);
	if (i18n_lang() == LANG_TR) snprintf(d, sizeof d, "%s\n%d %s", i18n_weekday(tm.tm_wday, false), tm.tm_mday, i18n_month(tm.tm_mon, false));
	else snprintf(d, sizeof d, "%s\n%s %d", i18n_weekday(tm.tm_wday, false), i18n_month(tm.tm_mon, false), tm.tm_mday);
	gfx_text(p->cr, d, pr.x + 9, pr.y + th - 30, 10 / ts, FW_REGULAR, COL_WHITE, TA_LEFT, tw - 14, 2);
	if (photo) {
		if (pg_button(&(Pg){p->ui, p->cr, p->th, p->x + tw + 16, p->w - tw - 16, p->y}, "set-l-change", _("change photo"), true))
			choose_picture(PICK_LOCK);
	}
	p->y += th + 22;

	int ti = 1;
	for (int i = 0; i < (int)ARRAY_LEN(TIMEOUTS); i++) if (TIMEOUTS[i] == cfg.screen_timeout) ti = i;
	if (pg_combo(p, "set-l-timeout", _("Screen times out after"), tr(TIMEOUT_NAMES[ti]), NULL)) {
		const char *names[ARRAY_LEN(TIMEOUT_NAMES)];
		for (size_t i = 0; i < ARRAY_LEN(names); i++) names[i] = tr(TIMEOUT_NAMES[i]);
		dialog_picker(_("Screen times out after"), names, (int)ARRAY_LEN(names), ti, timeout_cb, NULL);
	}
	bool has = cfg.pin_hash[0] != 0, v = has;
	if (pg_toggle(p, "set-l-pin", _("Password"), &v)) pin_start(has ? PIN_REMOVE : PIN_SET);
	if (has && pg_button(p, "set-l-pinchg", _("change password"), true)) pin_start(PIN_CHANGE);
	pg_text(p, has ? _("You'll need to type your password to unlock the phone.")
	               : _("Set a numeric password so that only you can unlock the phone."));
}

static void page_wifi(Pg *p) {
	UI *ui = p->ui;
	cairo_t *cr = p->cr;
	const Theme *th = p->th;
	WifiInfo w = sys_wifi();
	if (!w.available && !sys_demo()) {
		pg_text(p, _("Wi-Fi can't be managed here because NetworkManager (nmcli) isn't available."));
		return;
	}
	bool on = w.enabled;
	if (pg_toggle(p, "set-w-on", _("Wi-Fi networking"), &on)) {
		sys_wifi_set_enabled(on);
		if (on) sys_wifi_scan();
	}
	if (!on) {
		pg_text(p, _("Turn on Wi-Fi to see available networks."));
		return;
	}
	const char *msg = sys_wifi_status_msg();
	if (msg && *msg) {
		p->y += gfx_text(cr, msg, p->x, p->y, 18, FW_SEMILIGHT, th->accent, TA_LEFT, p->w, 3) + 12;
		S.wifi_target[0] = 0;
	}
	/* "connecting" lasts until we are on the target network (or ~30 s pass) */
	if (S.wifi_target[0] && ((w.connected && str_eq(w.ssid, S.wifi_target)) || ui->now - S.wifi_t0 > 30))
		S.wifi_target[0] = 0;
	gfx_text(cr, _("Networks"), p->x, p->y, 16, FW_REGULAR, th->sub, TA_LEFT, p->w, 1);
	if (sys_wifi_scanning()) ui_progress_dots(ui, R(p->x + 120, p->y + 2, p->w - 120, 16), th->accent);
	p->y += 30;
	int n;
	const WifiNet *nets = sys_wifi_list(&n);
	if (!n) pg_text(p, sys_wifi_scanning() ? _("Looking for networks…") : _("No networks found. Tap refresh to look again."));
	double lh = gfx_line_height(cr, 24, FW_SEMILIGHT), sl = gfx_line_height(cr, 15, FW_REGULAR);
	/* the connected network is listed first, like on the phone */
	int order[64], no = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n && no < 64; i++) {
			bool c = (w.connected && str_eq(w.ssid, nets[i].ssid)) || (nets[i].active && !w.ssid[0]);
			if (c == (pass == 0)) order[no++] = i;
		}
	for (int k = 0; k < no; k++) {
		int i = order[k];
		const WifiNet *net = &nets[i];
		bool conn = (w.connected && str_eq(w.ssid, net->ssid)) || (net->active && !w.ssid[0]);
		double h = lh + sl + 20;
		Rect r = R(p->x - 24, p->y, p->w + 48, h);
		UiId id = ui_idn("set-w-net", i);
		ui_hit(ui, id, r);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.14f));
		int lvl = net->signal >= 75 ? 3 : net->signal >= 50 ? 2 : net->signal >= 25 ? 1 : 0;
		Color ic = conn ? th->accent : th->fg;
		icon_draw_wifi_level(cr, p->x + 16, r.y + h / 2 - 2, 30, ic, lvl);
		if (net->secured) icon_draw(cr, ICON_LOCK, p->x + 30, r.y + h / 2 + 10, 12, ic);
		double tx = p->x + 52;
		gfx_text(cr, net->ssid, tx, r.y + 9, 24, FW_SEMILIGHT, conn ? th->accent : th->fg, TA_LEFT, p->w - 52, 1);
		const char *st;
		if (str_eq(S.wifi_target, net->ssid)) st = _("connecting…");
		else if (conn) st = _("connected");
		else if (net->known) st = _("saved");
		else st = net->secured ? _("secured") : _("open");
		gfx_text(cr, st, tx, r.y + 9 + lh - 2, 15, FW_REGULAR, th->sub, TA_LEFT, p->w - 52, 1);
		p->y += h;
		if (ui_clicked(ui, id)) wifi_tap(net, conn);
	}
	p->y += 10;
}

static void page_airplane(Pg *p) {
	bool v = sys_airplane();
	if (pg_toggle(p, "set-air", _("Status"), &v)) sys_set_airplane(v);
	pg_text(p, _("Turn on airplane mode to stop all wireless communication, including Wi-Fi and Bluetooth. You can turn Wi-Fi and Bluetooth back on while airplane mode is on."));
}

static void page_bt(Pg *p) {
	UI *ui = p->ui;
	if (!sys_bluetooth_available()) {
		pg_text(p, _("No Bluetooth adapter was found."));
		return;
	}
	bool v = sys_bluetooth_enabled();
	if (pg_toggle(p, "set-bt", _("Status"), &v)) { sys_bluetooth_set(v); if (v) bt_refresh(); }
	if (!v) {
		pg_text(p, _("Turn on Bluetooth to use headsets, speakers, keyboards and other devices."));
		return;
	}
	if (!proc_exists("bluetoothctl")) {
		pg_text(p, _("To see paired devices here, install bluetoothctl (BlueZ)."));
		return;
	}
	gfx_text(p->cr, _("Paired devices"), p->x, p->y, 16, FW_REGULAR, p->th->sub, TA_LEFT, p->w, 1);
	if (S.bt_loading) ui_progress_dots(ui, R(p->x + 150, p->y + 2, p->w - 150, 16), p->th->accent);
	p->y += 30;
	if (!S.nbt && S.bt_loaded) pg_text(p, _("No paired devices. Pair a device with bluetoothctl and it will show up here."));
	for (int i = 0; i < S.nbt; i++) {
		BtDev *d = &S.bt[i];
		char key[32];
		snprintf(key, sizeof key, "set-bt-dev%d", i);
		const char *st = d->busy ? (d->connected ? _("disconnecting…") : _("connecting…"))
		                         : d->connected ? _("connected") : _("not connected");
		if (pg_row(p, key, d->name, st, d->connected ? p->th->accent : p->th->fg)) bt_toggle_device(d);
	}
	if (S.nbt) pg_text(p, _("Tap a device to connect or disconnect it."));
}

static void page_display(Pg *p) {
	if (sys_has_backlight()) {
		if (!ui_dragging(p->ui, ui_id("set-bright")) && S.pend_bright < 0) S.bright = sys_brightness();
		char v[16];
		snprintf(v, sizeof v, "%d%%", (int)lround(S.bright));
		if (pg_slider(p, "set-bright", _("Brightness"), v, &S.bright, 1, 100, NULL)) {
			S.pend_bright = (int)lround(S.bright);
			throttle_kick();
		}
		if (cfg.battery_saver) pg_text(p, _("Battery saver is on, so brightness was lowered."));
	} else {
		pg_text(p, _("The brightness of this screen can't be changed here because no backlight control was found."));
	}
	int si = 0;
	for (int i = 1; i < (int)ARRAY_LEN(SCALES); i++) if (fabs(SCALES[i] - cfg.ui_scale) < 0.01) si = i;
	char names[ARRAY_LEN(SCALES)][48];
	const char *items[ARRAY_LEN(SCALES)];
	for (int i = 0; i < (int)ARRAY_LEN(SCALES); i++) {
		if (i == 0) snprintf(names[i], sizeof names[i], _("automatic (%d%%)"), (int)lround(sh.scale * 100));
		else snprintf(names[i], sizeof names[i], "%d%%", (int)lround(SCALES[i] * 100));
		items[i] = names[i];
	}
	if (pg_combo(p, "set-d-scale", _("Display scale"), items[si], NULL))
		dialog_list(_("Display scale"), items, (int)ARRAY_LEN(SCALES), si, scale_cb, NULL);
	pg_text(p, _("Makes everything on the screen bigger or smaller. Automatic picks a size that suits the screen."));
	if (pg_toggle(p, "set-d-apps", _("Phone-size Linux apps"), &cfg.phone_apps)) apply_soon();
	pg_text(p, _("Shows GNOME, KDE and other Linux apps at the size they are made for on phones, so they use their phone layouts. Turn off to fit more into them."));
}

static void page_datetime(Pg *p) {
	cairo_t *cr = p->cr;
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char t[32], d[64];
	i18n_fmt_time(t, sizeof t, &tm, cfg.h24);
	i18n_fmt_date_long(d, sizeof d, &tm);
	p->y += gfx_text(cr, t, p->x - 2, p->y - 6, 60, FW_LIGHT, p->th->fg, TA_LEFT, p->w, 1) - 6;
	p->y += gfx_text(cr, d, p->x, p->y, 20, FW_SEMILIGHT, p->th->sub, TA_LEFT, p->w, 1) + 22;
	if (pg_toggle(p, "set-dt-24", _("24-hour clock"), &cfg.h24)) { config_save_soon(); shell_invalidate(); }
	char tz[96];
	tz_label(tz, sizeof tz);
	if (pg_combo(p, "set-dt-tz", _("Time zone"), tz, NULL)) go(P_TZ);
	if (!cfg.home_tz[0] && S.sys_tz[0]) {
		char *s = xasprintf(_("Automatic uses the system time zone (%s)."), S.sys_tz);
		pg_text(p, s);
		free(s);
	}
	pg_text(p, _("The date and time come from the system clock."));
}

static void page_tz(Pg *p) {
	tz_prepare();
	char sub[96];
	snprintf(sub, sizeof sub, "%s", S.sys_tz[0] ? S.sys_tz : _("system time zone"));
	if (pg_row(p, "set-tz-auto", _("automatic"), sub, cfg.home_tz[0] ? p->th->fg : p->th->accent)) {
		cfg.home_tz[0] = 0;
		tz_apply();
		config_save_soon();
		nav_pop(&S.nav, p->ui);
	}
	for (int k = 0; k < S.ntz; k++) {
		int i = S.tz_order[k];
		char u[16], title[96], key[32];
		fmt_utc(u, sizeof u, S.tz_off[i]);
		snprintf(title, sizeof title, "(%s) %s", u, tr(TZS[i].city));
		snprintf(key, sizeof key, "set-tz-%d", i);
		bool sel = str_eq(cfg.home_tz, TZS[i].id);
		if (pg_row(p, key, title, TZS[i].id, sel ? p->th->accent : p->th->fg)) {
			str_copy(cfg.home_tz, sizeof cfg.home_tz, TZS[i].id);
			tz_apply();
			config_save_soon();
			nav_pop(&S.nav, p->ui);
		}
	}
}

static void page_lang(Pg *p) {
	UI *ui = p->ui;
	pg_label(p, _("Display language"));
	char autol[96];
	snprintf(autol, sizeof autol, _("automatic (%s)"), i18n_native_name(i18n_detect_env()));
	const char *codes[] = {"", "en", "tr"};
	const char *labels[] = {autol, i18n_native_name(LANG_EN), i18n_native_name(LANG_TR)};
	double h = MAX(52, gfx_line_height(p->cr, 22, FW_SEMILIGHT) + 16);
	for (int i = 0; i < 3; i++) {
		bool sel = str_eq(cfg.lang, codes[i]);
		if (ui_radio(ui, ui_idn("set-lang", i), R(p->x, p->y, p->w, h), labels[i], sel) && !sel) {
			str_copy(cfg.lang, sizeof cfg.lang, codes[i]);
			apply_soon();
		}
		p->y += h + 6;
	}
	p->y += 10;
	pg_text(p, _("The new language is used right away. There's no need to restart."));
	/* regional format preview */
	pg_label(p, _("Regional format"));
	{
		const char *codes[] = {"", "en", "tr"};
		const char *labels[] = {_("same as display language"), _("United States"), _("Türkiye")};
		double h = MAX(52, gfx_line_height(p->cr, 22, FW_SEMILIGHT) + 16);
		for (int i = 0; i < 3; i++) {
			bool sel = str_eq(cfg.region, codes[i]);
			if (ui_radio(p->ui, ui_idn("set-region", i), R(p->x, p->y, p->w, h), labels[i], sel) && !sel) {
				str_copy(cfg.region, sizeof cfg.region, codes[i]);
				i18n_set_region(cfg.region);
				config_save_soon();
				shell_invalidate();
			}
			p->y += h;
		}
		p->y += 6;
		if (ui_toggle(p->ui, ui_id("set-region-h24"), R(p->x, p->y, p->w, 70), _("24-hour clock"), &cfg.h24)) config_save_soon();
		p->y += 76;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char a[64], b[64], c[32];
	i18n_fmt_date_long(a, sizeof a, &tm);
	i18n_fmt_date_short(b, sizeof b, &tm);
	i18n_fmt_time(c, sizeof c, &tm, cfg.h24);
	pg_info(p, _("Long date"), a);
	pg_info(p, _("Short date"), b);
	pg_info(p, _("Time"), c);
	pg_info(p, _("First day of week"), i18n_weekday(i18n_first_weekday(), false));
	pg_text(p, _("Dates and times follow the regional format."));
}

static void page_keyboard(Pg *p) {
	UI *ui = p->ui;
	pg_label(p, _("On-screen keyboard layouts"));
	bool en = strstr(cfg.osk_layouts, "en") != NULL, trk = strstr(cfg.osk_layouts, "tr") != NULL;
	double h = MAX(52, gfx_line_height(p->cr, 22, FW_SEMILIGHT) + 16);
	bool ch = ui_checkbox(ui, ui_id("set-k-en"), R(p->x, p->y, p->w, h), "English", &en);
	p->y += h + 4;
	ch |= ui_checkbox(ui, ui_id("set-k-tr"), R(p->x, p->y, p->w, h), "Türkçe", &trk);
	p->y += h + 10;
	if (ch && (en || trk)) {   /* at least one layout stays on */
		snprintf(cfg.osk_layouts, sizeof cfg.osk_layouts, "%s%s%s", en ? "en" : "", en && trk ? "," : "", trk ? "tr" : "");
		apply_soon();
	}
	pg_text(p, _("With more than one layout, the keyboard has a language key to switch between them."));
	if (pg_toggle(p, "set-k-sugg", _("Suggest text"), &cfg.osk_suggest)) config_save_soon();
	if (pg_toggle(p, "set-k-cap", _("Capitalize the first letter of a sentence"), &cfg.osk_autocap)) config_save_soon();

	int ki = 0;
	for (int i = 0; i < (int)ARRAY_LEN(KBL); i++) if (str_eq(KBL[i].code, cfg.kb_layout)) ki = i;
	char autol[96];
	snprintf(autol, sizeof autol, _("automatic (%s)"), i18n_lang() == LANG_TR ? "Türkçe Q" : "English (US)");
	const char *items[ARRAY_LEN(KBL)];
	items[0] = autol;
	for (int i = 1; i < (int)ARRAY_LEN(KBL); i++) items[i] = KBL[i].name;
	if (pg_combo(p, "set-k-hw", _("Hardware keyboard layout"), items[ki], NULL))
		dialog_list(_("Hardware keyboard layout"), items, (int)ARRAY_LEN(KBL), ki, kbl_cb, NULL);
	pg_label(p, _("Try it"));
	bool changed;
	ui_textbox(ui, ui_id("set-k-test"), R(p->x, p->y, p->w, 48), S.kbd_test, sizeof S.kbd_test, _("Type here"), 0, &changed);
	p->y += 64;
}

static void page_ease(Pg *p) {
	if (!S.text_drag) S.text_scale = cfg.text_scale > 0 ? cfg.text_scale : 1;
	char v[16];
	snprintf(v, sizeof v, "%d%%", (int)lround(S.text_scale * 100));
	bool rel = false;
	if (pg_slider(p, "set-e-text", _("Text size"), v, &S.text_scale, 0.8, 1.5, &rel)) S.text_drag = true;
	S.text_scale = round(S.text_scale * 20) / 20;
	if (rel) S.text_drag = false;
	/* preview at the size the text will have (gfx scales by the current factor) */
	double cur = gfx_text_scale() > 0 ? gfx_text_scale() : 1;
	p->y += gfx_text(p->cr, _("This is how text will look in apps and on Start."), p->x, p->y,
	                 20 * S.text_scale / cur, FW_SEMILIGHT, p->th->fg, TA_LEFT, p->w, 0) + 22;
	if (rel && fabs(S.text_scale - cfg.text_scale) > 0.001) {
		cfg.text_scale = S.text_scale;
		gfx_set_text_scale(cfg.text_scale);
		apply_soon();
	}
	if (pg_toggle(p, "set-e-anim", _("Animations"), &cfg.animations)) config_save_soon();
	pg_text(p, _("Turn off animations to make page changes, tiles and menus appear instantly."));
}

static void page_about(Pg *p) {
	refresh_info(p->ui->now);
	const SysInfo *s = &S.si;
	char a[64], b[64], *t;
	pg_info(p, _("Device name"), s->hostname);
	t = xasprintf("metrowl %s", METROWL_VERSION);
	pg_info(p, _("Software"), t);
	free(t);
	pg_info(p, _("Operating system"), s->os);
	pg_info(p, _("Kernel"), s->kernel);
	pg_info(p, _("Processor"), s->cpu);
	pg_info(p, _("Architecture"), s->arch);
	if (s->mem_total > 0) {
		human_size(s->mem_total, a, sizeof a);
		human_size(s->mem_avail, b, sizeof b);
		t = xasprintf(_("%s (%s available)"), a, b);
		pg_info(p, _("Memory"), t);
		free(t);
	}
	if (s->disk_total > 0) {
		human_size(s->disk_total, a, sizeof a);
		human_size(s->disk_free, b, sizeof b);
		t = xasprintf(_("%s (%s free)"), a, b);
		pg_info(p, _("Storage"), t);
		free(t);
	}
	if (s->uptime > 0) {
		int up = (int)s->uptime, d = up / 86400, h = up % 86400 / 3600, m = up % 3600 / 60;
		t = d ? xasprintf(_("%d d %d h %d min"), d, h, m) : xasprintf(_("%d h %d min"), h, m);
		pg_info(p, _("Up time"), t);
		free(t);
	}
	if (s->wlroots[0]) {
		t = xasprintf("wlroots %s", s->wlroots);
		pg_info(p, _("Compositor library"), t);
		free(t);
	}
	char sz[48];
	snprintf(sz, sizeof sz, "%.0f × %.0f, %d%%", sh.w * sh.scale, sh.h * sh.scale, (int)lround(sh.scale * 100));
	pg_info(p, _("Screen"), sz);
	if (sys_demo()) pg_text(p, _("Running in demo mode: hardware values are simulated."));
}

static void page_weather(Pg *p) {
	UI *ui = p->ui;
	pg_label(p, _("Show temperature in"));
	double h = MAX(52, gfx_line_height(p->cr, 22, FW_SEMILIGHT) + 16);
	if (ui_radio(ui, ui_id("set-wx-c"), R(p->x, p->y, p->w, h), _("Celsius (°C)"), cfg.celsius) && !cfg.celsius) {
		cfg.celsius = true;
		config_save_soon();
	}
	p->y += h + 6;
	if (ui_radio(ui, ui_id("set-wx-f"), R(p->x, p->y, p->w, h), _("Fahrenheit (°F)"), !cfg.celsius) && cfg.celsius) {
		cfg.celsius = false;
		config_save_soon();
	}
	p->y += h + 20;
	pg_label(p, _("Location"));
	p->y += gfx_text(p->cr, cfg.city[0] ? cfg.city : _("not set"), p->x, p->y, 22, FW_SEMILIGHT, p->th->fg, TA_LEFT, p->w, 1) + 14;
	pg_text(p, _("To change the location, search for a city in the weather app."));
	if (pg_button(p, "set-wx-open", _("open weather"), true)) shell_open_builtin("weather", NULL);
}

static void page_photos(Pg *p) {
	char *dir = shots_dir();
	const char *home = home_dir();
	char shown[600];
	if (str_starts(dir, home) && dir[strlen(home)] == '/') snprintf(shown, sizeof shown, "~%s", dir + strlen(home));
	else str_copy(shown, sizeof shown, dir);
	free(dir);
	pg_label(p, _("Screenshots are saved to"));
	p->y += gfx_text(p->cr, shown, p->x, p->y, 22, FW_SEMILIGHT, p->th->fg, TA_LEFT, p->w, 2) + 6;
	char *n = S.shots == 1 ? xstrdup(_("1 screenshot")) : xasprintf(_("%d screenshots"), S.shots);
	pg_text(p, n);
	free(n);
	pg_text(p, _("Take a screenshot with the screenshot quick action in the action center."));
	if (pg_button(p, "set-ph-open", _("open photos"), true)) shell_open_builtin("photos", NULL);
}

/* ------------------------------------------------------------ page frame - */

static double page_header(UI *ui, Rect area, const char *title) {
	cairo_t *cr = ui->cr;
	double x = area.x + 14, y = area.y + 6;
	double h1 = gfx_text(cr, i18n_upper(_("settings")), x + 4, y, 17, FW_SEMIBOLD, ui->th.fg, TA_LEFT, area.w - 36, 1);
	/* long titles run off the edge like on the phone instead of being ellipsised */
	cairo_save(cr);
	cairo_rectangle(cr, area.x, y, area.w, 200);
	cairo_clip(cr);
	double h2 = gfx_text(cr, i18n_lower(title), x, y + h1 - 6, 56, FW_LIGHT, ui->th.fg, TA_LEFT, 0, 1);
	cairo_restore(cr);
	return y + h1 - 6 + h2 + 4;
}

static void wifi_appbar(UI *ui, Rect area) {
	WifiInfo w = sys_wifi();
	if (!w.enabled || (!w.available && !sys_demo())) return;
	AppBarBtn b[] = {{ICON_REFRESH, _("refresh"), sys_wifi_scanning()}};
	if (appbar_draw(ui, &S.bar, area, b, 1, NULL, 0) == 0) sys_wifi_scan();
}

static void inner_page(UI *ui, int page, Rect area) {
	cairo_t *cr = ui->cr;
	WifiInfo w = sys_wifi();
	bool bar = page == P_WIFI && w.enabled && (w.available || sys_demo());
	Rect a = bar ? appbar_content(&S.bar, area) : area;
	double y = page_header(ui, area, tr(PAGE_TITLE[page]));
	Rect view = R(a.x, y + 8, a.w, rect_bottom(a) - y - 8);
	ui_scroll_begin(ui, ui_idn("set-page", page), &S.scroll[page], view, S.content_h[page]);
	cairo_translate(cr, view.x, view.y);
	Pg p = {ui, cr, &ui->th, 24, MIN(view.w - 48, 560), 4};
	switch (page) {
	case P_THEME: page_theme(&p); break;
	case P_ACCENT: page_accent(&p); break;
	case P_SOUNDS: page_sounds(&p); break;
	case P_NOTIF: page_notif(&p); break;
	case P_LOCK: page_lock(&p); break;
	case P_WIFI: page_wifi(&p); break;
	case P_AIR: page_airplane(&p); break;
	case P_BT: page_bt(&p); break;
	case P_DISPLAY: page_display(&p); break;
	case P_DATETIME: page_datetime(&p); break;
	case P_TZ: page_tz(&p); break;
	case P_LANG: page_lang(&p); break;
	case P_KEYBOARD: page_keyboard(&p); break;
	case P_EASE: page_ease(&p); break;
	case P_ABOUT: page_about(&p); break;
	case P_WEATHER: page_weather(&p); break;
	case P_PHOTOS: page_photos(&p); break;
	default: break;
	}
	S.content_h[page] = p.y + 30;
	ui_scroll_end(ui);
	if (bar) wifi_appbar(ui, area);
}

static void draw_page(UI *ui, int page, Rect area, void *ud) {
	if (page == P_ROOT) {
		static const char *titles[2];
		titles[0] = _("system");
		titles[1] = _("applications");
		app_pivot(ui, "set-pivot", &S.pivot, area, _("settings"), titles, 2, root_page, NULL);
	} else {
		inner_page(ui, page, area);
	}
}

/* ---------------------------------------------------------------- AppDef - */

static void settings_init(void) {
	if (cfg.home_tz[0]) tz_apply();
}

static void settings_open(const char *arg) {
	ensure_ready();
	if (!arg || !*arg) return;   /* resume where the user left off */
	if (str_eq(arg, "battery") || str_eq(arg, "storage")) {
		/* battery saver and storage sense are their own apps, as on the phone */
		nav_init(&S.nav, P_ROOT);
		ui_pager_goto(&S.pivot, 0);
		S.pivot.pos = 0;
		open_app_soon(str_eq(arg, "battery") ? "battery" : "storage");
		return;
	}
	for (size_t i = 0; i < ARRAY_LEN(LINKS); i++) {
		if (!str_eq(arg, LINKS[i].link)) continue;
		nav_init(&S.nav, P_ROOT);
		bool app_page = LINKS[i].page == P_WEATHER || LINKS[i].page == P_PHOTOS;
		ui_pager_goto(&S.pivot, app_page ? 1 : 0);
		S.pivot.pos = app_page ? 1 : 0;
		if (LINKS[i].parent) {
			page_enter(LINKS[i].parent);
			S.nav.stack[++S.nav.depth] = LINKS[i].parent;
		}
		page_enter(LINKS[i].page);
		S.nav.stack[++S.nav.depth] = LINKS[i].page;
		anim_set(&S.nav.anim, 1);
		shell_invalidate();
		return;
	}
	LOGE("settings: unknown page '%s'", arg);
}

static void settings_draw(UI *ui, Rect area) {
	ensure_ready();
	if (S.si_t <= 0) refresh_info(ui->now);
	nav_draw(ui, &S.nav, area, draw_page, NULL);
	/* touches are matched against the hit areas of the frame before the last one: after a
	 * page change draw once more so the very first tap on the new page is not lost */
	int sig = ui->blocked ? -2 : dialog_active() ? -3 : nav_busy(&S.nav, ui) ? -1 : nav_page(&S.nav);
	if (sig != S.last_sig) ui_request_frame(ui);
	S.last_sig = sig;
}

static bool settings_back(void) {
	if (appbar_back(&S.bar)) return true;
	return nav_pop(&S.nav, ui_now());
}

static void settings_close(void) {
	if (!S.ready) return;
	nav_init(&S.nav, P_ROOT);
	S.pivot = (Pager){0};
	memset(S.root_scroll, 0, sizeof S.root_scroll);
	S.bar = (AppBar){0};
	S.nbt = 0;
	S.bt_loaded = false;
	S.si_t = 0;
	S.kbd_test[0] = 0;
	S.wifi_target[0] = 0;
}

/* Pages the global search can jump to (titles are translated when shown). */
int settings_page_list(const SettingsPageInfo **out) {
	static const SettingsPageInfo p[] = {
		{N_("start+theme"), "theme", "theme accent color colour background dark light start tiles wallpaper picture "
		                             "parallax icons tema vurgu renk arka plan koyu açık başlangıç kutucuk duvar kağıdı resim simge"},
		{N_("accent color"), "accent", "accent color colour theme vurgu rengi renk tema"},
		{N_("ringtones+sounds"), "sounds", "sound volume mute ringtone audio speaker ses sesler zil sessiz hoparlör"},
		{N_("notifications+actions"), "notifications", "notifications banner toast quiet hours action center "
		                                              "bildirim bildirimler afiş sessiz saatler eylem merkezi"},
		{N_("lock screen"), "lock", "lock screen password pin timeout background kilit ekranı parola şifre zaman aşımı"},
		{N_("wi-fi"), "wifi", "wifi wi-fi wireless network internet hotspot kablosuz ağ internet"},
		{N_("airplane mode"), "airplane", "airplane flight mode radio uçak modu uçuş"},
		{N_("bluetooth"), "bluetooth", "bluetooth headset speaker pair kulaklık hoparlör eşleştir"},
		{N_("battery saver"), "battery", "battery saver power energy pil tasarruf güç enerji"},
		{N_("storage sense"), "storage", "storage disk space free memory depolama disk alan boş"},
		{N_("display"), "display", "display brightness scale screen size dpi ekran parlaklık ölçek boyut"},
		{N_("date+time"), "datetime", "date time clock 24-hour time zone tarih saat saat dilimi 24 saat"},
		{N_("time zone"), "timezone", "time zone timezone utc gmt saat dilimi"},
		{N_("language+region"), "language", "language region english turkish locale format dil bölge ingilizce türkçe biçim"},
		{N_("keyboard"), "keyboard", "keyboard layout typing suggestions osk klavye düzen yazma öneri"},
		{N_("ease of access"), "ease", "ease of access text size accessibility animations erişim kolaylığı metin boyutu yazı animasyon"},
		{N_("about"), "about", "about version device info system kernel memory hakkında sürüm cihaz bilgi sistem çekirdek bellek"},
		{N_("weather"), "weather", "weather celsius fahrenheit temperature units hava durumu sıcaklık birim"},
		{N_("photos+camera"), "photos", "photos camera screenshots fotoğraflar kamera ekran görüntüsü"},
	};
	*out = p;
	return (int)(sizeof p / sizeof p[0]);
}

const AppDef app_settings = {
	.id = "settings", .name = N_("Settings"), .icon = ICON_SETTINGS, .color = 0,
	.keywords = "settings ayarlar preferences tercihler",
	.init = settings_init, .open = settings_open, .draw = settings_draw, .back = settings_back,
	.close = settings_close,
};

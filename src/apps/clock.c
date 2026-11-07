/* Alarms & Clock: Windows Phone 8.1 alarms plus the Windows 10 Mobile world clock, timer
 * and stopwatch pivots. Alarms and the timer ring through a full-screen overlay that also
 * shows above the lock screen; a cheap 1 Hz timer registered in init() drives them. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <glib.h>
#include <math.h>

#define MAX_ALARMS 40
#define MAX_WORLD 24
#define MAX_LAPS 99
#define RING_SECS 600          /* a ringing alarm stops by itself after 10 minutes */
#define DATA_FILE "alarms.json"
#define APP_NAME "Alarms & Clock"

typedef struct {
	char name[64];
	int hour, min;
	int days;                  /* bit 0 = Sunday .. bit 6 = Saturday; 0 = only once */
	int snooze;                /* minutes */
	bool on, sound;
} Alarm;

typedef struct { const char *city, *country, *tz; } City;

static const City CITIES[] = {
	{N_("UTC"), N_("Coordinated Universal Time"), "UTC"},
	{N_("London"), N_("United Kingdom"), "Europe/London"},
	{N_("Dublin"), N_("Ireland"), "Europe/Dublin"},
	{N_("Lisbon"), N_("Portugal"), "Europe/Lisbon"},
	{N_("Reykjavik"), N_("Iceland"), "Atlantic/Reykjavik"},
	{N_("Madrid"), N_("Spain"), "Europe/Madrid"},
	{N_("Barcelona"), N_("Spain"), "Europe/Madrid"},
	{N_("Paris"), N_("France"), "Europe/Paris"},
	{N_("Brussels"), N_("Belgium"), "Europe/Brussels"},
	{N_("Amsterdam"), N_("Netherlands"), "Europe/Amsterdam"},
	{N_("Berlin"), N_("Germany"), "Europe/Berlin"},
	{N_("Munich"), N_("Germany"), "Europe/Berlin"},
	{N_("Frankfurt"), N_("Germany"), "Europe/Berlin"},
	{N_("Zurich"), N_("Switzerland"), "Europe/Zurich"},
	{N_("Vienna"), N_("Austria"), "Europe/Vienna"},
	{N_("Rome"), N_("Italy"), "Europe/Rome"},
	{N_("Milan"), N_("Italy"), "Europe/Rome"},
	{N_("Prague"), N_("Czechia"), "Europe/Prague"},
	{N_("Warsaw"), N_("Poland"), "Europe/Warsaw"},
	{N_("Budapest"), N_("Hungary"), "Europe/Budapest"},
	{N_("Copenhagen"), N_("Denmark"), "Europe/Copenhagen"},
	{N_("Stockholm"), N_("Sweden"), "Europe/Stockholm"},
	{N_("Oslo"), N_("Norway"), "Europe/Oslo"},
	{N_("Helsinki"), N_("Finland"), "Europe/Helsinki"},
	{N_("Athens"), N_("Greece"), "Europe/Athens"},
	{N_("Bucharest"), N_("Romania"), "Europe/Bucharest"},
	{N_("Sofia"), N_("Bulgaria"), "Europe/Sofia"},
	{N_("Belgrade"), N_("Serbia"), "Europe/Belgrade"},
	{N_("Kyiv"), N_("Ukraine"), "Europe/Kyiv"},
	{N_("Minsk"), N_("Belarus"), "Europe/Minsk"},
	{N_("Moscow"), N_("Russia"), "Europe/Moscow"},
	{N_("Istanbul"), N_("Türkiye"), "Europe/Istanbul"},
	{N_("Ankara"), N_("Türkiye"), "Europe/Istanbul"},
	{N_("Izmir"), N_("Türkiye"), "Europe/Istanbul"},
	{N_("Antalya"), N_("Türkiye"), "Europe/Istanbul"},
	{N_("Nicosia"), N_("Cyprus"), "Asia/Nicosia"},
	{N_("Cairo"), N_("Egypt"), "Africa/Cairo"},
	{N_("Jerusalem"), N_("Israel"), "Asia/Jerusalem"},
	{N_("Beirut"), N_("Lebanon"), "Asia/Beirut"},
	{N_("Amman"), N_("Jordan"), "Asia/Amman"},
	{N_("Baghdad"), N_("Iraq"), "Asia/Baghdad"},
	{N_("Tehran"), N_("Iran"), "Asia/Tehran"},
	{N_("Riyadh"), N_("Saudi Arabia"), "Asia/Riyadh"},
	{N_("Mecca"), N_("Saudi Arabia"), "Asia/Riyadh"},
	{N_("Dubai"), N_("United Arab Emirates"), "Asia/Dubai"},
	{N_("Doha"), N_("Qatar"), "Asia/Qatar"},
	{N_("Baku"), N_("Azerbaijan"), "Asia/Baku"},
	{N_("Tbilisi"), N_("Georgia"), "Asia/Tbilisi"},
	{N_("Yerevan"), N_("Armenia"), "Asia/Yerevan"},
	{N_("Casablanca"), N_("Morocco"), "Africa/Casablanca"},
	{N_("Algiers"), N_("Algeria"), "Africa/Algiers"},
	{N_("Tunis"), N_("Tunisia"), "Africa/Tunis"},
	{N_("Lagos"), N_("Nigeria"), "Africa/Lagos"},
	{N_("Accra"), N_("Ghana"), "Africa/Accra"},
	{N_("Addis Ababa"), N_("Ethiopia"), "Africa/Addis_Ababa"},
	{N_("Nairobi"), N_("Kenya"), "Africa/Nairobi"},
	{N_("Johannesburg"), N_("South Africa"), "Africa/Johannesburg"},
	{N_("Cape Town"), N_("South Africa"), "Africa/Johannesburg"},
	{N_("Kabul"), N_("Afghanistan"), "Asia/Kabul"},
	{N_("Karachi"), N_("Pakistan"), "Asia/Karachi"},
	{N_("Islamabad"), N_("Pakistan"), "Asia/Karachi"},
	{N_("Tashkent"), N_("Uzbekistan"), "Asia/Tashkent"},
	{N_("Almaty"), N_("Kazakhstan"), "Asia/Almaty"},
	{N_("New Delhi"), N_("India"), "Asia/Kolkata"},
	{N_("Mumbai"), N_("India"), "Asia/Kolkata"},
	{N_("Bangalore"), N_("India"), "Asia/Kolkata"},
	{N_("Kolkata"), N_("India"), "Asia/Kolkata"},
	{N_("Colombo"), N_("Sri Lanka"), "Asia/Colombo"},
	{N_("Kathmandu"), N_("Nepal"), "Asia/Kathmandu"},
	{N_("Dhaka"), N_("Bangladesh"), "Asia/Dhaka"},
	{N_("Yangon"), N_("Myanmar"), "Asia/Yangon"},
	{N_("Bangkok"), N_("Thailand"), "Asia/Bangkok"},
	{N_("Hanoi"), N_("Vietnam"), "Asia/Ho_Chi_Minh"},
	{N_("Ho Chi Minh City"), N_("Vietnam"), "Asia/Ho_Chi_Minh"},
	{N_("Kuala Lumpur"), N_("Malaysia"), "Asia/Kuala_Lumpur"},
	{N_("Singapore"), N_("Singapore"), "Asia/Singapore"},
	{N_("Jakarta"), N_("Indonesia"), "Asia/Jakarta"},
	{N_("Bali"), N_("Indonesia"), "Asia/Makassar"},
	{N_("Manila"), N_("Philippines"), "Asia/Manila"},
	{N_("Hong Kong"), N_("China"), "Asia/Hong_Kong"},
	{N_("Beijing"), N_("China"), "Asia/Shanghai"},
	{N_("Shanghai"), N_("China"), "Asia/Shanghai"},
	{N_("Taipei"), N_("Taiwan"), "Asia/Taipei"},
	{N_("Seoul"), N_("South Korea"), "Asia/Seoul"},
	{N_("Tokyo"), N_("Japan"), "Asia/Tokyo"},
	{N_("Osaka"), N_("Japan"), "Asia/Tokyo"},
	{N_("Ulaanbaatar"), N_("Mongolia"), "Asia/Ulaanbaatar"},
	{N_("Vladivostok"), N_("Russia"), "Asia/Vladivostok"},
	{N_("Perth"), N_("Australia"), "Australia/Perth"},
	{N_("Adelaide"), N_("Australia"), "Australia/Adelaide"},
	{N_("Brisbane"), N_("Australia"), "Australia/Brisbane"},
	{N_("Sydney"), N_("Australia"), "Australia/Sydney"},
	{N_("Melbourne"), N_("Australia"), "Australia/Melbourne"},
	{N_("Auckland"), N_("New Zealand"), "Pacific/Auckland"},
	{N_("Wellington"), N_("New Zealand"), "Pacific/Auckland"},
	{N_("Suva"), N_("Fiji"), "Pacific/Fiji"},
	{N_("Honolulu"), N_("United States"), "Pacific/Honolulu"},
	{N_("Anchorage"), N_("United States"), "America/Anchorage"},
	{N_("Vancouver"), N_("Canada"), "America/Vancouver"},
	{N_("Seattle"), N_("United States"), "America/Los_Angeles"},
	{N_("San Francisco"), N_("United States"), "America/Los_Angeles"},
	{N_("Los Angeles"), N_("United States"), "America/Los_Angeles"},
	{N_("Las Vegas"), N_("United States"), "America/Los_Angeles"},
	{N_("Phoenix"), N_("United States"), "America/Phoenix"},
	{N_("Denver"), N_("United States"), "America/Denver"},
	{N_("Mexico City"), N_("Mexico"), "America/Mexico_City"},
	{N_("Chicago"), N_("United States"), "America/Chicago"},
	{N_("Dallas"), N_("United States"), "America/Chicago"},
	{N_("Houston"), N_("United States"), "America/Chicago"},
	{N_("Toronto"), N_("Canada"), "America/Toronto"},
	{N_("Montreal"), N_("Canada"), "America/Toronto"},
	{N_("New York"), N_("United States"), "America/New_York"},
	{N_("Washington"), N_("United States"), "America/New_York"},
	{N_("Boston"), N_("United States"), "America/New_York"},
	{N_("Atlanta"), N_("United States"), "America/New_York"},
	{N_("Miami"), N_("United States"), "America/New_York"},
	{N_("Havana"), N_("Cuba"), "America/Havana"},
	{N_("Panama"), N_("Panama"), "America/Panama"},
	{N_("Bogotá"), N_("Colombia"), "America/Bogota"},
	{N_("Lima"), N_("Peru"), "America/Lima"},
	{N_("Caracas"), N_("Venezuela"), "America/Caracas"},
	{N_("Halifax"), N_("Canada"), "America/Halifax"},
	{N_("St. John's"), N_("Canada"), "America/St_Johns"},
	{N_("Santiago"), N_("Chile"), "America/Santiago"},
	{N_("Buenos Aires"), N_("Argentina"), "America/Argentina/Buenos_Aires"},
	{N_("São Paulo"), N_("Brazil"), "America/Sao_Paulo"},
	{N_("Rio de Janeiro"), N_("Brazil"), "America/Sao_Paulo"},
};
#define NCITIES ((int)ARRAY_LEN(CITIES))

typedef struct { int city; GTimeZone *zone; } World;

enum { TM_IDLE, TM_RUNNING, TM_PAUSED };
enum { RING_NONE, RING_ALARM, RING_TIMER };
enum { P_ROOT, P_EDIT, P_CITY, P_TIMER_SET };
enum { PV_ALARMS, PV_WORLD, PV_TIMER, PV_STOPWATCH, PV_COUNT };

static const int SNOOZE_MIN[] = {5, 10, 15, 20, 30};
static const int PRESET_MIN[] = {1, 3, 5, 10, 15, 30, 45, 60};

/* ---- data (always resident: alarms must ring while the app is closed) */
static Alarm g_alarms[MAX_ALARMS];
static int g_nalarms;
static World g_world[MAX_WORLD];
static int g_nworld;
static bool g_loaded;
static unsigned g_gen;                 /* bumped when alarms change (tile cache) */

/* timer: `end` is CLOCK_BOOTTIME based so it keeps counting through suspend */
static struct { int dur, state; double end, left; } g_tmr = {300, TM_IDLE, 0, 300};
static struct { bool running; double t0, acc; double laps[MAX_LAPS]; int nlaps; } g_sw;
static struct {
	int kind;
	char name[64];
	int hour, min, snooze;
	bool sound;
	time_t since;
	uint32_t notif;
	bool pressed_snooze;
} g_ring;
static struct { bool active; time_t at; Alarm a; } g_snz;

static Timer *g_tick, *g_precise, *g_frame;
static time_t g_last;                  /* last second checked for alarms */
static time_t g_due;                   /* next alarm after g_last, 0 = recompute */

/* ---- UI state */
static Nav g_nav;
static Pager g_pivot;
static AppBar g_bar, g_ed_bar, g_city_bar, g_set_bar;
static Scroll g_al_scroll, g_wc_scroll, g_sw_scroll, g_ed_scroll, g_city_scroll;
static Alarm g_ed;
static int g_ed_idx = -1;
static char g_city_q[64];
static bool g_city_focus;
static GTimeZone *g_city_zone[NCITIES];
static int g_set_h, g_set_m, g_set_s, g_set_field;
static int g_menu_idx;

/* ------------------------------------------------------------- helpers -- */

static double boot_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double wall_now(void) { return (double)g_get_real_time() / 1e6; }

static GTimeZone *zone_new(const char *id) {
#if GLIB_CHECK_VERSION(2, 68, 0)
	GTimeZone *z = g_time_zone_new_identifier(id);
	return z ? z : g_time_zone_new_utc();
#else
	return g_time_zone_new(id);
#endif
}

/* "7:30" + "AM" (suffix empty in 24 hour mode) */
static void split_time(int h, int m, char *big, size_t nb, char *suf, size_t ns) {
	struct tm tm = {.tm_hour = h, .tm_min = m};
	char b[32];
	i18n_fmt_time(b, sizeof b, &tm, cfg.h24);
	char *sp = strchr(b, ' ');
	if (sp) { *sp = 0; str_copy(suf, ns, sp + 1); }
	else suf[0] = 0;
	str_copy(big, nb, b);
}

static void fmt_hm(char *buf, size_t n, int h, int m) {
	struct tm tm = {.tm_hour = h, .tm_min = m};
	i18n_fmt_time(buf, n, &tm, cfg.h24);
}

/* Big time with a smaller AM/PM next to it. Returns the height. */
static double draw_big_time(cairo_t *cr, int h, int m, double x, double y, double size, Color c, int align) {
	char big[16], suf[8];
	split_time(h, m, big, sizeof big, suf, sizeof suf);
	Size bs = gfx_text_size(cr, big, size, FW_LIGHT, 0, 1);
	double ss = MAX(12, size * 0.36);
	Size ssz = suf[0] ? gfx_text_size(cr, suf, ss, FW_SEMILIGHT, 0, 1) : (Size){0, 0};
	double total = bs.w + (suf[0] ? ssz.w + size * 0.1 : 0);
	double x0 = align == TA_RIGHT ? x - total : align == TA_CENTER ? x - total / 2 : x;
	gfx_text(cr, big, x0, y, size, FW_LIGHT, c, TA_LEFT, 0, 1);
	if (suf[0]) gfx_text(cr, suf, x0 + bs.w + size * 0.1, y + bs.h - ssz.h - size * 0.14, ss, FW_SEMILIGHT, c, TA_LEFT, 0, 1);
	return bs.h;
}

static void fmt_hms(char *buf, size_t n, double secs) {
	int s = (int)ceil(MAX(0, secs) - 0.001);
	snprintf(buf, n, "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
}

static void fmt_sw(char *buf, size_t n, double t) {
	long cs = (long)floor(MAX(0, t) * 100);
	long h = cs / 360000, m = cs / 6000 % 60, s = cs / 100 % 60, c = cs % 100;
	if (h) snprintf(buf, n, "%ld:%02ld:%02ld.%02ld", h, m, s, c);
	else snprintf(buf, n, "%02ld:%02ld.%02ld", m, s, c);
}

static const char *snooze_label(int min) {
	static char b[32];
	snprintf(b, sizeof b, _("%d minutes"), min);
	return b;
}

/* "weekdays", "every day", "only once", "Mon, Wed" */
static void repeat_text(int days, char *buf, size_t n) {
	if (!days) { str_copy(buf, n, _("only once")); return; }
	if (days == 0x7f) { str_copy(buf, n, _("every day")); return; }
	if (days == 0x3e) { str_copy(buf, n, _("weekdays")); return; }
	if (days == 0x41) { str_copy(buf, n, _("weekends")); return; }
	buf[0] = 0;
	int first = i18n_first_weekday();
	for (int k = 0; k < 7; k++) {
		int d = (first + k) % 7;
		if (!(days & (1 << d))) continue;
		if (buf[0]) g_strlcat(buf, ", ", n);
		g_strlcat(buf, i18n_weekday(d, true), n);
	}
}

/* "5 hours 3 minutes" */
static void fmt_span(char *buf, size_t n, long secs) {
	long mins = (secs + 59) / 60, d = mins / 1440, h = mins / 60 % 24, m = mins % 60;
	char part[48];
	buf[0] = 0;
	if (d) { snprintf(part, sizeof part, d == 1 ? _("%d day") : _("%d days"), (int)d); g_strlcat(buf, part, n); }
	if (h) {
		snprintf(part, sizeof part, h == 1 ? _("%d hour") : _("%d hours"), (int)h);
		if (buf[0]) g_strlcat(buf, " ", n);
		g_strlcat(buf, part, n);
	}
	if (m || !buf[0]) {
		snprintf(part, sizeof part, m == 1 ? _("%d minute") : _("%d minutes"), (int)MAX(m, 1));
		if (buf[0]) g_strlcat(buf, " ", n);
		g_strlcat(buf, part, n);
	}
}

/* ---------------------------------------------------------- persistence - */

static int city_find(const char *name, const char *tz) {
	for (int i = 0; i < NCITIES; i++)
		if (str_eq(CITIES[i].city, name) && (!tz || str_eq(CITIES[i].tz, tz))) return i;
	return -1;
}

static double jnum(const cJSON *o, const char *k, double def) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *k, bool def) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static const char *jstr(const cJSON *o, const char *k) {
	const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
	return s ? s : "";
}

static void save(void) {
	g_gen++;
	g_due = 0;
	cJSON *root = cJSON_CreateObject();
	cJSON *arr = cJSON_AddArrayToObject(root, "alarms");
	for (int i = 0; i < g_nalarms; i++) {
		Alarm *a = &g_alarms[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", a->name);
		cJSON_AddNumberToObject(o, "hour", a->hour);
		cJSON_AddNumberToObject(o, "min", a->min);
		cJSON_AddNumberToObject(o, "days", a->days);
		cJSON_AddNumberToObject(o, "snooze", a->snooze);
		cJSON_AddBoolToObject(o, "on", a->on);
		cJSON_AddBoolToObject(o, "sound", a->sound);
		cJSON_AddItemToArray(arr, o);
	}
	cJSON *w = cJSON_AddArrayToObject(root, "world");
	for (int i = 0; i < g_nworld; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "city", CITIES[g_world[i].city].city);
		cJSON_AddStringToObject(o, "tz", CITIES[g_world[i].city].tz);
		cJSON_AddItemToArray(w, o);
	}
	cJSON *t = cJSON_AddObjectToObject(root, "timer");
	cJSON_AddNumberToObject(t, "dur", g_tmr.dur);
	cJSON_AddNumberToObject(t, "state", g_tmr.state);
	cJSON_AddNumberToObject(t, "left", g_tmr.left);
	if (g_tmr.state == TM_RUNNING) cJSON_AddNumberToObject(t, "end", wall_now() + (g_tmr.end - boot_now()));
	app_json_save(DATA_FILE, root);
	cJSON_Delete(root);
}

static void ensure_loaded(void) {
	if (g_loaded) return;
	g_loaded = true;
	cJSON *root = app_json_load(DATA_FILE);
	if (!root) return;
	const cJSON *o;
	cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(root, "alarms")) {
		if (g_nalarms >= MAX_ALARMS) break;
		Alarm *a = &g_alarms[g_nalarms++];
		str_copy(a->name, sizeof a->name, jstr(o, "name"));
		a->hour = CLAMP((int)jnum(o, "hour", 7), 0, 23);
		a->min = CLAMP((int)jnum(o, "min", 0), 0, 59);
		a->days = (int)jnum(o, "days", 0) & 0x7f;
		a->snooze = CLAMP((int)jnum(o, "snooze", 10), 1, 60);
		a->on = jbool(o, "on", true);
		a->sound = jbool(o, "sound", true);
	}
	cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(root, "world")) {
		int c = city_find(jstr(o, "city"), jstr(o, "tz"));
		if (c >= 0 && g_nworld < MAX_WORLD) g_world[g_nworld++] = (World){c, NULL};
	}
	const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "timer");
	if (t) {
		g_tmr.dur = CLAMP((int)jnum(t, "dur", 300), 1, 100 * 3600 - 1);
		g_tmr.state = CLAMP((int)jnum(t, "state", TM_IDLE), TM_IDLE, TM_PAUSED);
		g_tmr.left = CLAMP(jnum(t, "left", g_tmr.dur), 0, g_tmr.dur);
		if (g_tmr.state == TM_RUNNING) {
			double rem = jnum(t, "end", 0) - wall_now();
			/* finished long ago while we were not running: forget it */
			if (rem < -RING_SECS) { g_tmr.state = TM_IDLE; g_tmr.left = g_tmr.dur; }
			else g_tmr.end = boot_now() + rem;
		}
	}
	cJSON_Delete(root);
}

/* ------------------------------------------------------------ scheduling */

/* First time strictly after `after` the alarm rings (ignores `on`). */
static time_t alarm_next(const Alarm *a, time_t after) {
	struct tm base;
	localtime_r(&after, &base);
	for (int d = 0; d <= 8; d++) {
		struct tm tm = base;
		tm.tm_mday += d;
		tm.tm_hour = a->hour;
		tm.tm_min = a->min;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		time_t t = mktime(&tm);
		if (t > after && (!a->days || (a->days & (1 << tm.tm_wday)))) return t;
	}
	return 0;
}

static time_t compute_due(time_t after) {
	time_t best = 0;
	for (int i = 0; i < g_nalarms; i++) {
		if (!g_alarms[i].on) continue;
		time_t t = alarm_next(&g_alarms[i], after);
		if (t && (!best || t < best)) best = t;
	}
	if (g_snz.active && (!best || g_snz.at < best)) best = MAX(g_snz.at, after + 1);
	return best ? best : after + 3600;
}

/* Next alarm (or snoozed alarm) for the tile and toasts; cached per minute. */
static time_t next_alarm(const char **name) {
	static time_t cache_min = -1, cache_t;
	static unsigned cache_gen = ~0u;
	static char cache_name[64];
	time_t now = time(NULL);
	if (cache_min != now / 60 || cache_gen != g_gen) {
		cache_min = now / 60;
		cache_gen = g_gen;
		cache_t = 0;
		cache_name[0] = 0;
		for (int i = 0; i < g_nalarms; i++) {
			if (!g_alarms[i].on) continue;
			time_t t = alarm_next(&g_alarms[i], now);
			if (t && (!cache_t || t < cache_t)) { cache_t = t; str_copy(cache_name, sizeof cache_name, g_alarms[i].name); }
		}
		if (g_snz.active && (!cache_t || g_snz.at < cache_t)) { cache_t = g_snz.at; str_copy(cache_name, sizeof cache_name, g_snz.a.name); }
	}
	if (name) *name = cache_name;
	return cache_t;
}

static uint32_t notify_quiet(const char *summary, const char *body, uint32_t replaces) {
	uint32_t id = shell_notify(APP_NAME, summary, body, "alarm-symbolic", "", 2, replaces, false);
	/* the overlay is the alert: keep the notification in the action center, but no toast */
	for (int i = 0; i < sh.toasts.len; i++) {
		if (sh.toasts.items[i]->id != id) continue;
		vec_remove(&sh.toasts, i);
		if (i == 0 && sh.toasts.len) sh.toasts.items[0]->toast_t0 = 0;
		break;
	}
	return id;
}

static void ring_end(bool remove_notif) {
	if (!g_ring.kind) return;
	app_sound_stop();
	if (remove_notif && g_ring.notif) shell_notify_remove(g_ring.notif);
	g_ring.kind = RING_NONE;
	g_ring.notif = 0;
	shell_invalidate();
}

static void ring_begin(int kind, const char *name, int h, int m, int snooze, bool sound) {
	ring_end(true);
	g_ring.kind = kind;
	str_copy(g_ring.name, sizeof g_ring.name, name && *name ? name : (kind == RING_TIMER ? _("Timer") : _("Alarm")));
	g_ring.hour = h;
	g_ring.min = m;
	g_ring.snooze = snooze > 0 ? snooze : 10;
	g_ring.sound = sound;
	g_ring.since = time(NULL);
	shell_wake();
	input_notify_activity();
	if (sound) app_sound(kind == RING_TIMER ? SND_TIMER : SND_ALARM, true);
	char body[64];
	if (kind == RING_TIMER) str_copy(body, sizeof body, _("time's up"));
	else fmt_hm(body, sizeof body, h, m);
	g_ring.notif = notify_quiet(g_ring.name, body, 0);
	shell_invalidate();
}

static void ring_alarm(const Alarm *a) {
	ring_begin(RING_ALARM, a->name, a->hour, a->min, a->snooze, a->sound);
}

static void check_alarms(time_t after, time_t now) {
	int best = -1, missed = -1;
	time_t best_t = 0;
	bool changed = false;
	Alarm once_copy = {0};
	for (int i = 0; i < g_nalarms; i++) {
		Alarm *a = &g_alarms[i];
		if (!a->on) continue;
		time_t t = alarm_next(a, after);
		if (!t || t > now) continue;
		if (now - t <= RING_SECS) {
			if (t >= best_t) { best_t = t; best = i; }
		} else {
			missed = i;
		}
		if (!a->days) { a->on = false; changed = true; }   /* "only once" alarms are used up */
	}
	if (missed >= 0 && best < 0) {
		char body[96], tm[32];
		fmt_hm(tm, sizeof tm, g_alarms[missed].hour, g_alarms[missed].min);
		snprintf(body, sizeof body, _("missed alarm · %s"), tm);
		shell_notify(APP_NAME, g_alarms[missed].name, body, "alarm-symbolic", "", 1, 0, false);
	}
	if (best >= 0) {
		once_copy = g_alarms[best];
		ring_alarm(&once_copy);
	}
	if (g_snz.active && g_snz.at <= now) {
		g_snz.active = false;
		changed = true;
		if (now - g_snz.at <= RING_SECS && best < 0) ring_alarm(&g_snz.a);
	}
	if (changed) save();
}

static void timer_finish(void) {
	g_tmr.state = TM_IDLE;
	g_tmr.left = g_tmr.dur;
	save();
	ring_begin(RING_TIMER, _("Timer"), 0, 0, 0, true);
}

static void precise_cb(void *ud) {
	if (g_tmr.state == TM_RUNNING && boot_now() >= g_tmr.end - 0.02) timer_finish();
	shell_invalidate();
}

static void frame_cb(void *ud) { shell_invalidate(); }

static bool app_visible(void) {
	return sh.screen == SCR_APP && sh.cur_app == &app_clock && !sh.screen_off;
}

static void tick_cb(void *ud) {
	ensure_loaded();
	time_t now = time(NULL);
	if (!g_last || now < g_last) { g_last = now; g_due = 0; }   /* first run or clock set back */
	if (now > g_last) {
		if (!g_due) g_due = compute_due(g_last);
		if (now >= g_due || now - g_last > 5) {
			check_alarms(g_last, now);
			g_due = compute_due(now);
		} else if (now / 60 != g_last / 60) {
			g_due = 0;   /* re-evaluate every minute (time zone or DST changes) */
		}
		g_last = now;
	}
	/* timer: hand over to a precise one-shot near the end */
	if (g_tmr.state == TM_RUNNING) {
		double rem = g_tmr.end - boot_now();
		if (rem <= 0) timer_finish();
		else if (rem < 1.5) timer_set(g_precise, MAX(1, (int)(rem * 1000)));
	}
	if (g_ring.kind) {
		input_notify_activity();          /* keep the screen on while ringing */
		if (now - g_ring.since >= RING_SECS) {
			if (g_ring.kind == RING_ALARM) notify_quiet(g_ring.name, _("missed alarm"), g_ring.notif);
			ring_end(false);
		}
	}
	if (app_visible() && (g_tmr.state == TM_RUNNING) && !g_nav.depth && (int)lround(g_pivot.pos) == PV_TIMER) shell_invalidate();
}

/* ----------------------------------------------------------------- overlay */

bool clock_overlay_active(void) { return g_ring.kind != RING_NONE; }

static void do_snooze(void) {
	Alarm a = {0};
	str_copy(a.name, sizeof a.name, g_ring.name);
	a.hour = g_ring.hour;
	a.min = g_ring.min;
	a.snooze = g_ring.snooze;
	a.sound = g_ring.sound;
	g_snz.active = true;
	g_snz.at = time(NULL) + g_ring.snooze * 60;
	g_snz.a = a;
	g_gen++;
	g_due = 0;
	ring_end(true);
}

static void do_restart_timer(void) {
	ring_end(true);
	g_tmr.state = TM_RUNNING;
	g_tmr.end = boot_now() + g_tmr.dur;
	save();
}

bool clock_overlay_back(void) {
	/* Back must not silently kill an alarm: it snoozes (the timer is just dismissed) */
	if (g_ring.kind == RING_ALARM) do_snooze();
	else ring_end(true);
	return true;
}

static bool overlay_button(UI *ui, UiId id, Rect r, const char *label, Color fg, Color bg) {
	cairo_t *cr = ui->cr;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	if (pr) gfx_fill_rect(cr, r, fg);
	else gfx_stroke_rect(cr, r, 2, fg);
	gfx_text_in(cr, label, rect_inset(r, 6), 22, FW_SEMILIGHT, pr ? bg : fg, TA_CENTER, true);
	return ui_clicked(ui, id);
}

void clock_overlay_draw(UI *ui, Rect full) {
	if (!g_ring.kind) return;
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Color bg = th->accent, fg = th->on_accent;
	gfx_fill_rect(cr, full, bg);
	ui_hit(ui, ui_id("ring-bg"), full);
	bool timer = g_ring.kind == RING_TIMER;
	double x = full.x + 24, w = full.w - 48;
	bool wide = full.w > full.h * 1.2;
	double y = full.y + (wide ? 24 : 48);
	icon_draw(cr, timer ? ICON_TIMER : ICON_ALARM, x + 12, y + 12, 26, fg);
	gfx_text(cr, i18n_upper(timer ? _("Timer") : _("Alarm")), x + 36, y + 2, 17, FW_SEMIBOLD, fg, TA_LEFT, w - 40, 1);
	y += wide ? 40 : 70;
	double big = MIN(120, MIN(full.w, full.h) * 0.24);
	if (timer) {
		char b[32];
		fmt_hms(b, sizeof b, g_tmr.dur);
		y += gfx_text(cr, b, x - 4, y, big * 0.8, FW_LIGHT, fg, TA_LEFT, w + 8, 1);
		y += gfx_text(cr, _("time's up"), x, y, 30, FW_SEMILIGHT, fg, TA_LEFT, w, 1) + 12;
	} else {
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		y += draw_big_time(cr, tm.tm_hour, tm.tm_min, x - 4, y, big, fg, TA_LEFT);
		char d[96];
		i18n_fmt_date_long(d, sizeof d, &tm);
		y += gfx_text(cr, d, x, y, 24, FW_SEMILIGHT, fg, TA_LEFT, w, 1) + 18;
		y += gfx_text(cr, g_ring.name, x, y, 34, FW_SEMILIGHT, fg, TA_LEFT, w, 2) + 12;
	}
	double bh = 64, by = rect_bottom(full) - bh - (wide ? 20 : 40), gap = 16;
	double bw = MIN(220, (w - gap) / 2);
	if (!timer) {
		/* snooze length: tap to cycle (dialogs would open below the overlay) */
		double fy = by - 104;
		if (fy > y) {
			gfx_text(cr, _("snooze for"), x, fy, 15, FW_REGULAR, col_alpha(fg, 0.85f), TA_LEFT, w, 1);
			Rect box = R(x, fy + 24, MIN(w, 2 * bw + gap), 52);
			UiId sid = ui_id("ring-snooze-len");
			ui_hit(ui, sid, box);
			bool pr = ui_pressed(ui, sid);
			if (pr) gfx_fill_rect(cr, box, col_alpha(fg, 0.25f));
			gfx_stroke_rect(cr, box, 2, fg);
			gfx_text(cr, snooze_label(g_ring.snooze), box.x + 12, box.y + (box.h - gfx_line_height(cr, 22, FW_SEMILIGHT)) / 2,
			         22, FW_SEMILIGHT, fg, TA_LEFT, box.w - 50, 1);
			icon_draw(cr, ICON_CHEVRON_D, rect_right(box) - 24, rect_cy(box), 16, fg);
			if (ui_clicked(ui, sid)) {
				int k = 0;
				for (int i = 0; i < (int)ARRAY_LEN(SNOOZE_MIN); i++) if (SNOOZE_MIN[i] == g_ring.snooze) k = i + 1;
				g_ring.snooze = SNOOZE_MIN[k % ARRAY_LEN(SNOOZE_MIN)];
				shell_invalidate();
			}
		}
	}
	Rect b1 = R(x, by, bw, bh), b2 = R(x + bw + gap, by, bw, bh);
	if (timer) {
		if (overlay_button(ui, ui_id("ring-restart"), b1, _("restart"), fg, bg)) do_restart_timer();
		else if (overlay_button(ui, ui_id("ring-dismiss"), b2, _("dismiss"), fg, bg)) ring_end(true);
	} else {
		if (overlay_button(ui, ui_id("ring-snooze"), b1, _("snooze"), fg, bg)) do_snooze();
		else if (overlay_button(ui, ui_id("ring-dismiss"), b2, _("dismiss"), fg, bg)) ring_end(true);
	}
}

/* --------------------------------------------------------- small widgets */

/* Compact Windows Phone toggle switch (no label) with its own hit area. */
static bool mini_toggle(UI *ui, UiId id, Rect hit, double right, double cy, bool *v) {
	cairo_t *cr = ui->cr;
	const Theme *t = &ui->th;
	ui_hit(ui, id, hit);
	bool pr = ui_pressed(ui, id), changed = false;
	if (ui_clicked(ui, id)) { *v = !*v; changed = true; }
	double *pos = ui_state(ui, id, *v ? 1 : 0);
	pos[0] = cfg.animations ? ui_smooth(ui, pos[0], *v ? 1 : 0, 22) : (*v ? 1 : 0);
	double tw = 54, th = 22;
	Rect track = R(right - tw, cy - th / 2, tw, th);
	gfx_stroke_rect(cr, track, 2, col_alpha(*v ? t->accent : t->fg, pr ? 0.7f : 1.f));
	gfx_fill_rect(cr, R(track.x + 4, track.y + 4, (tw - 8) * pos[0], th - 8), t->accent);
	double tx = track.x + 2 + (tw - 4 - 10) * pos[0];
	gfx_fill_rect(cr, R(tx - 1, track.y - 5, 10, th + 10), pr ? t->sub : t->fg);
	return changed;
}

/* Label + bordered value box that opens a picker (Windows Phone 8.1 picker look). */
static bool pick_field(UI *ui, UiId id, Rect r, const char *label, const char *value) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	gfx_text(cr, label, r.x, r.y, 15, FW_REGULAR, th->sub, TA_LEFT, r.w, 1);
	Rect box = R(r.x, r.y + 22, r.w, 50);
	ui_hit(ui, id, box);
	bool pr = ui_pressed(ui, id);
	if (pr) gfx_fill_rect(cr, box, th->accent);
	gfx_stroke_rect(cr, box, 2, pr ? th->accent : th->fg);
	gfx_text(cr, value, box.x + 12, box.y + (box.h - gfx_line_height(cr, 22, FW_SEMILIGHT)) / 2, 22, FW_SEMILIGHT,
	         pr ? th->on_accent : th->fg, TA_LEFT, box.w - 24, 1);
	return ui_clicked(ui, id);
}

static void toast_alarm_set(const Alarm *a) {
	time_t now = time(NULL), t = alarm_next(a, now);
	if (!t) return;
	char span[96], msg[160];
	fmt_span(span, sizeof span, (long)(t - now));
	snprintf(msg, sizeof msg, _("alarm set for %s from now"), span);
	shell_toast(a->name, msg, tr(APP_NAME));
}

/* ------------------------------------------------------------ alarms page */

static void open_editor(int idx) {
	g_ed_idx = idx;
	if (idx >= 0) {
		g_ed = g_alarms[idx];
	} else {
		time_t now = time(NULL) + 3600;
		struct tm tm;
		localtime_r(&now, &tm);
		g_ed = (Alarm){.hour = tm.tm_hour, .min = 0, .days = 0, .snooze = 10, .on = true, .sound = true};
		str_copy(g_ed.name, sizeof g_ed.name, _("Alarm"));
	}
	g_ed_scroll = (Scroll){0};
	g_ed_bar = (AppBar){0};
	nav_push(&g_nav, &sh.ui, P_EDIT);
}

static void delete_alarm(int i) {
	if (i < 0 || i >= g_nalarms) return;
	memmove(&g_alarms[i], &g_alarms[i + 1], (size_t)(g_nalarms - i - 1) * sizeof(Alarm));
	g_nalarms--;
	save();
}

static void alarm_menu_cb(int b, void *ud) {
	if (b == 0) open_editor(g_menu_idx);
	else if (b == 1) delete_alarm(g_menu_idx);
}

static void snooze_menu_cb(int b, void *ud) {
	if (b == 0) { g_snz.active = false; g_gen++; g_due = 0; shell_invalidate(); }
}

static void page_alarms(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	const double ROW = 96, SNZ = 72;
	double top = g_snz.active ? SNZ : 0;
	if (!g_nalarms && !g_snz.active) {
		app_empty(ui, r, _("no alarms yet. tap + to add one."));
		return;
	}
	double off = ui_scroll_begin(ui, ui_id("al-scroll"), &g_al_scroll, r, top + g_nalarms * ROW + 24);
	cairo_translate(cr, r.x, r.y);
	if (g_snz.active) {
		Rect rr = R(0, 0, r.w, SNZ);
		UiId id = ui_id("al-snoozed");
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
		icon_draw_circle(cr, ICON_SNOOZE, 24 + 22, SNZ / 2, 44, th->accent, 2);
		struct tm tm;
		localtime_r(&g_snz.at, &tm);
		char when[32], line[128];
		fmt_hm(when, sizeof when, tm.tm_hour, tm.tm_min);
		snprintf(line, sizeof line, _("snoozed until %s"), when);
		gfx_text(cr, g_snz.a.name, 84, 10, 22, FW_SEMILIGHT, th->fg, TA_LEFT, r.w - 108, 1);
		gfx_text(cr, line, 84, 40, 15, FW_REGULAR, th->accent, TA_LEFT, r.w - 108, 1);
		if (ui_clicked(ui, id) || ui_longpressed(ui, id)) {
			static const char *items[1];
			items[0] = _("dismiss");
			dialog_menu(ui->p.x, ui->p.y, items, 1, snooze_menu_cb, NULL);
		}
	}
	for (int i = 0; i < g_nalarms; i++) {
		Alarm *a = &g_alarms[i];
		Rect rr = R(0, top + i * ROW, r.w, ROW);
		if (rr.y + ROW < off || rr.y > off + r.h) continue;
		UiId id = ui_idn("al-row", i);
		ui_hit(ui, id, rr);
		bool pr = ui_pressed(ui, id);
		if (pr) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
		Color c = a->on ? th->fg : th->sub;
		double tw = r.w - 24 - 96;
		double h = draw_big_time(cr, a->hour, a->min, 22, rr.y + 6, 44, c, TA_LEFT);
		char rep[96], line[192];
		repeat_text(a->days, rep, sizeof rep);
		snprintf(line, sizeof line, "%s, %s", a->name, rep);
		gfx_text(cr, line, 24, rr.y + 6 + h - 2, 15, FW_REGULAR, th->sub, TA_LEFT, tw, 1);
		bool on = a->on;
		if (mini_toggle(ui, ui_idn("al-toggle", i), R(r.w - 100, rr.y, 100, ROW), r.w - 24, rect_cy(rr), &on)) {
			a->on = on;
			save();
			if (on) toast_alarm_set(a);
		}
		if (ui_clicked(ui, id)) open_editor(i);
		if (ui_longpressed(ui, id)) {
			static const char *items[2];
			items[0] = _("edit");
			items[1] = _("delete");
			g_menu_idx = i;
			dialog_menu(ui->p.x, ui->p.y, items, 2, alarm_menu_cb, NULL);
		}
	}
	ui_scroll_end(ui);
}

/* ------------------------------------------------------- world clock page */

static GTimeZone *world_zone(World *w) {
	if (!w->zone) w->zone = zone_new(CITIES[w->city].tz);
	return w->zone;
}

/* "+3 hours", "-5:30 hours", "same time" relative to local time */
static void offset_text(GDateTime *there, GDateTime *here, char *buf, size_t n) {
	long diff = (long)((g_date_time_get_utc_offset(there) - g_date_time_get_utc_offset(here)) / G_TIME_SPAN_SECOND);
	if (!diff) { str_copy(buf, n, _("same time")); return; }
	const char *sign = diff > 0 ? "+" : "-";
	long a = labs(diff), h = a / 3600, m = a / 60 % 60;
	if (m) snprintf(buf, n, _("%s%d:%02d hours"), sign, (int)h, (int)m);
	else snprintf(buf, n, h == 1 ? _("%s%d hour") : _("%s%d hours"), sign, (int)h);
}

static void world_menu_cb(int b, void *ud) {
	int i = g_menu_idx;
	if (i < 0 || i >= g_nworld) return;
	if (b == 0) {
		if (g_world[i].zone) g_time_zone_unref(g_world[i].zone);
		memmove(&g_world[i], &g_world[i + 1], (size_t)(g_nworld - i - 1) * sizeof(World));
		g_nworld--;
	} else if (b == 1 && i > 0) {
		World w = g_world[i];
		memmove(&g_world[1], &g_world[0], (size_t)i * sizeof(World));
		g_world[0] = w;
	}
	save();
	shell_invalidate();
}

static void page_world(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	const double ROW = 90;
	double off = ui_scroll_begin(ui, ui_id("wc-scroll"), &g_wc_scroll, r, (g_nworld + 1) * ROW + 24 + (g_nworld ? 0 : 60));
	cairo_translate(cr, r.x, r.y);
	GDateTime *here = g_date_time_new_now_local();
	/* local time first */
	{
		Rect rr = R(0, 0, r.w, ROW);
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		char d[96];
		i18n_fmt_date_long(d, sizeof d, &tm);
		double tw = draw_big_time(cr, tm.tm_hour, tm.tm_min, r.w - 24, rr.y + 14, 40, th->accent, TA_RIGHT);
		(void)tw;
		gfx_text(cr, _("local time"), 24, rr.y + 14, 24, FW_SEMILIGHT, th->fg, TA_LEFT, r.w - 200, 1);
		gfx_text(cr, d, 24, rr.y + 46, 15, FW_REGULAR, th->sub, TA_LEFT, r.w - 200, 1);
	}
	if (!g_nworld) {
		gfx_text(cr, _("see the time around the world. tap + to add a city."), 24, ROW + 10, 20, FW_SEMILIGHT, th->sub,
		         TA_LEFT, r.w - 48, 0);
	}
	for (int i = 0; i < g_nworld; i++) {
		Rect rr = R(0, (i + 1) * ROW, r.w, ROW);
		if (rr.y + ROW < off || rr.y > off + r.h) continue;
		World *w = &g_world[i];
		UiId id = ui_idn("wc-row", i);
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
		GDateTime *there = g_date_time_new_now(world_zone(w));
		int hy, hm, hd, ty, tm_, td;
		g_date_time_get_ymd(here, &hy, &hm, &hd);
		g_date_time_get_ymd(there, &ty, &tm_, &td);
		int cmp = ty != hy ? (ty > hy ? 1 : -1) : tm_ != hm ? (tm_ > hm ? 1 : -1) : td != hd ? (td > hd ? 1 : -1) : 0;
		char offs[48], sub[128];
		offset_text(there, here, offs, sizeof offs);
		snprintf(sub, sizeof sub, "%s, %s", cmp > 0 ? _("tomorrow") : cmp < 0 ? _("yesterday") : _("today"), offs);
		draw_big_time(cr, g_date_time_get_hour(there), g_date_time_get_minute(there), r.w - 24, rr.y + 14, 40, th->fg, TA_RIGHT);
		gfx_text(cr, tr(CITIES[w->city].city), 24, rr.y + 14, 24, FW_SEMILIGHT, th->fg, TA_LEFT, r.w - 200, 1);
		gfx_text(cr, sub, 24, rr.y + 46, 15, FW_REGULAR, th->sub, TA_LEFT, r.w - 200, 1);
		g_date_time_unref(there);
		if (ui_longpressed(ui, id) || ui_clicked(ui, id)) {
			static const char *items[2];
			items[0] = _("remove");
			items[1] = _("move to top");
			g_menu_idx = i;
			dialog_menu(ui->p.x, ui->p.y, items, i > 0 ? 2 : 1, world_menu_cb, NULL);
		}
	}
	g_date_time_unref(here);
	ui_scroll_end(ui);
}

/* ------------------------------------------------------------- timer page */

static double timer_left(void) {
	if (g_tmr.state == TM_RUNNING) return MAX(0, g_tmr.end - boot_now());
	if (g_tmr.state == TM_PAUSED) return g_tmr.left;
	return g_tmr.dur;
}

static void timer_toggle(void) {
	if (g_tmr.state == TM_RUNNING) {
		g_tmr.left = timer_left();
		g_tmr.state = TM_PAUSED;
	} else {
		double left = g_tmr.state == TM_PAUSED ? g_tmr.left : g_tmr.dur;
		g_tmr.end = boot_now() + left;
		g_tmr.state = TM_RUNNING;
	}
	save();
	shell_invalidate();
}

static void timer_reset(void) {
	g_tmr.state = TM_IDLE;
	g_tmr.left = g_tmr.dur;
	timer_set(g_precise, 0);
	save();
	shell_invalidate();
}

static void timer_set_dur(int secs) {
	g_tmr.dur = CLAMP(secs, 1, 100 * 3600 - 1);
	g_tmr.state = TM_IDLE;
	g_tmr.left = g_tmr.dur;
	save();
	shell_invalidate();
}

static void open_timer_set(void) {
	g_set_h = g_tmr.dur / 3600;
	g_set_m = g_tmr.dur / 60 % 60;
	g_set_s = g_tmr.dur % 60;
	g_set_bar = (AppBar){0};
	nav_push(&g_nav, &sh.ui, P_TIMER_SET);
}

static void preset_cb(int b, void *ud) {
	if (b < 0) return;
	int i = dialog_picker_get();
	if (i < (int)ARRAY_LEN(PRESET_MIN)) timer_set_dur(PRESET_MIN[i] * 60);
	else open_timer_set();
}

static void open_presets(void) {
	static char labels[ARRAY_LEN(PRESET_MIN) + 1][32];
	static const char *ptr[ARRAY_LEN(PRESET_MIN) + 1];
	int sel = (int)ARRAY_LEN(PRESET_MIN);
	for (int i = 0; i < (int)ARRAY_LEN(PRESET_MIN); i++) {
		if (PRESET_MIN[i] == 60) str_copy(labels[i], sizeof labels[i], _("1 hour"));
		else snprintf(labels[i], sizeof labels[i], PRESET_MIN[i] == 1 ? _("%d minute") : _("%d minutes"), PRESET_MIN[i]);
		if (g_tmr.dur == PRESET_MIN[i] * 60) sel = i;
		ptr[i] = labels[i];
	}
	str_copy(labels[ARRAY_LEN(PRESET_MIN)], sizeof labels[0], _("custom"));
	ptr[ARRAY_LEN(PRESET_MIN)] = labels[ARRAY_LEN(PRESET_MIN)];
	dialog_picker(_("timer"), ptr, (int)ARRAY_LEN(ptr), sel, preset_cb, NULL);
}

static void arm_frame_timer(double secs_to_next) {
	if (!g_frame) g_frame = timer_add(0, false, frame_cb, NULL);
	timer_set(g_frame, MAX(10, (int)(secs_to_next * 1000) + 5));
}

static void page_timer(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double left = timer_left();
	char b[32];
	fmt_hms(b, sizeof b, left);
	double size = MIN(88, r.w * 0.19);
	double y = r.y + MAX(20, r.h * 0.16);
	Size ts = gfx_text_size(cr, b, size, FW_LIGHT, 0, 1);
	Rect tr_ = R(r.x + (r.w - ts.w) / 2 - 12, y, ts.w + 24, ts.h);
	UiId tid = ui_id("tm-display");
	ui_hit(ui, tid, tr_);
	bool pr = ui_pressed(ui, tid);
	Color c = g_tmr.state == TM_PAUSED ? th->sub : pr ? th->accent : th->fg;
	gfx_text(cr, b, r.x + r.w / 2, y, size, FW_LIGHT, c, TA_CENTER, 0, 1);
	y += ts.h + 10;
	Rect bar = R(r.x + 24, y, r.w - 48, 6);
	gfx_fill_rect(cr, bar, th->chrome2);
	double frac = g_tmr.dur > 0 ? left / g_tmr.dur : 0;
	gfx_fill_rect(cr, R(bar.x, bar.y, bar.w * CLAMP(frac, 0, 1), bar.h), th->accent);
	y += 24;
	char status[96];
	if (g_tmr.state == TM_RUNNING) {
		time_t end = time(NULL) + (time_t)ceil(left);
		struct tm tm;
		localtime_r(&end, &tm);
		char e[32];
		fmt_hm(e, sizeof e, tm.tm_hour, tm.tm_min);
		snprintf(status, sizeof status, _("rings at %s"), e);
		/* redraw exactly when the shown second changes */
		arm_frame_timer(left - floor(left));
	} else if (g_tmr.state == TM_PAUSED) {
		str_copy(status, sizeof status, _("paused"));
	} else {
		str_copy(status, sizeof status, _("tap the time to change it"));
	}
	gfx_text(cr, status, r.x + r.w / 2, y, 18, FW_SEMILIGHT, g_tmr.state == TM_RUNNING ? th->accent : th->sub, TA_CENTER, r.w - 48, 2);
	if (ui_clicked(ui, tid) && g_tmr.state != TM_RUNNING) open_presets();
}

/* --------------------------------------------------------- stopwatch page */

static double sw_now(void) { return g_sw.acc + (g_sw.running ? boot_now() - g_sw.t0 : 0); }

static void sw_toggle(void) {
	if (g_sw.running) { g_sw.acc = sw_now(); g_sw.running = false; }
	else { g_sw.t0 = boot_now(); g_sw.running = true; }
	shell_invalidate();
}

static void sw_lap(void) {
	if (!g_sw.running) return;
	if (g_sw.nlaps >= MAX_LAPS) {
		memmove(g_sw.laps, g_sw.laps + 1, (MAX_LAPS - 1) * sizeof(double));
		g_sw.nlaps--;
	}
	g_sw.laps[g_sw.nlaps++] = sw_now();
	shell_invalidate();
}

static void sw_reset(void) {
	memset(&g_sw, 0, sizeof g_sw);
	g_sw_scroll = (Scroll){0};
	shell_invalidate();
}

static void page_stopwatch(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double t = sw_now();
	char b[32];
	fmt_sw(b, sizeof b, t);
	double size = MIN(80, r.w * 0.17);
	double y = r.y + 10;
	y += gfx_text(cr, b, r.x + 20, y, size, FW_LIGHT, g_sw.running || t == 0 ? th->fg : th->sub, TA_LEFT, r.w - 40, 1);
	double prev = g_sw.nlaps ? g_sw.laps[g_sw.nlaps - 1] : 0;
	if (g_sw.nlaps) {
		char lb[32], line[64];
		fmt_sw(lb, sizeof lb, t - prev);
		snprintf(line, sizeof line, _("lap %d"), g_sw.nlaps + 1);
		gfx_text(cr, line, r.x + 24, y, 20, FW_SEMILIGHT, th->sub, TA_LEFT, r.w / 2, 1);
		gfx_text(cr, lb, r.x + r.w - 24, y, 20, FW_SEMILIGHT, th->sub, TA_RIGHT, r.w / 2, 1);
	}
	y += 40;
	if (g_sw.running) ui->animating = true;
	if (!g_sw.nlaps) return;
	/* fastest / slowest laps are marked like on the phone */
	int fast = 0, slow = 0;
	for (int i = 1; i < g_sw.nlaps; i++) {
		double li = g_sw.laps[i] - g_sw.laps[i - 1];
		double lf = g_sw.laps[fast] - (fast ? g_sw.laps[fast - 1] : 0), ls = g_sw.laps[slow] - (slow ? g_sw.laps[slow - 1] : 0);
		if (li < lf) fast = i;
		if (li > ls) slow = i;
	}
	gfx_fill_rect(cr, R(r.x + 24, y - 8, r.w - 48, 1), th->chrome2);
	Rect view = R(r.x, y, r.w, rect_bottom(r) - y);
	const double ROW = 48;
	double off = ui_scroll_begin(ui, ui_id("sw-scroll"), &g_sw_scroll, view, g_sw.nlaps * ROW + 16);
	cairo_translate(cr, view.x, view.y);
	for (int k = 0; k < g_sw.nlaps; k++) {
		int i = g_sw.nlaps - 1 - k;   /* newest first */
		double ry = k * ROW;
		if (ry + ROW < off || ry > off + view.h) continue;
		double lap = g_sw.laps[i] - (i ? g_sw.laps[i - 1] : 0);
		char name[32], lb[32], tb[32];
		snprintf(name, sizeof name, _("lap %d"), i + 1);
		fmt_sw(lb, sizeof lb, lap);
		fmt_sw(tb, sizeof tb, g_sw.laps[i]);
		Color c = g_sw.nlaps > 2 && i == fast ? th->accent : th->fg;
		double ty = ry + (ROW - gfx_line_height(cr, 22, FW_SEMILIGHT)) / 2;
		gfx_text(cr, name, 24, ty, 22, FW_SEMILIGHT, c, TA_LEFT, view.w * 0.3, 1);
		gfx_text(cr, lb, view.w * 0.62, ty, 22, FW_SEMILIGHT, c, TA_RIGHT, view.w * 0.3, 1);
		gfx_text(cr, tb, view.w - 24, ty + 3, 18, FW_REGULAR, th->sub, TA_RIGHT, view.w * 0.34, 1);
		if (g_sw.nlaps > 2 && i == slow)
			gfx_text(cr, _("slowest"), 24 + gfx_text_size(cr, name, 22, FW_SEMILIGHT, 0, 1).w + 10, ty + 6, 13, FW_REGULAR, th->sub, TA_LEFT, 90, 1);
		else if (g_sw.nlaps > 2 && i == fast)
			gfx_text(cr, _("fastest"), 24 + gfx_text_size(cr, name, 22, FW_SEMILIGHT, 0, 1).w + 10, ty + 6, 13, FW_REGULAR, th->accent, TA_LEFT, 90, 1);
	}
	ui_scroll_end(ui);
}

/* ---------------------------------------------------------- root (pivot) */

static void pivot_page(UI *ui, int page, Rect r, void *ud) {
	switch (page) {
	case PV_ALARMS: page_alarms(ui, r); break;
	case PV_WORLD: page_world(ui, r); break;
	case PV_TIMER: page_timer(ui, r); break;
	default: page_stopwatch(ui, r); break;
	}
}

static void open_city_page(void) {
	g_city_q[0] = 0;
	g_city_scroll = (Scroll){0};
	g_city_bar = (AppBar){0};
	g_city_focus = true;
	nav_push(&g_nav, &sh.ui, P_CITY);
}

static void draw_root(UI *ui, Rect area) {
	static const char *TITLES[PV_COUNT] = {N_("alarms"), N_("world clock"), N_("timer"), N_("stopwatch")};
	const char *titles[PV_COUNT];
	for (int i = 0; i < PV_COUNT; i++) titles[i] = tr(TITLES[i]);
	Rect content = appbar_content(&g_bar, area);
	app_pivot(ui, "clock-pivot", &g_pivot, content, tr(APP_NAME), titles, PV_COUNT, pivot_page, NULL);
	int page = CLAMP(g_pivot.target, 0, PV_COUNT - 1);
	AppBarBtn btns[3];
	int nb = 0;
	switch (page) {
	case PV_ALARMS: btns[nb++] = (AppBarBtn){ICON_ADD, _("new")}; break;
	case PV_WORLD: btns[nb++] = (AppBarBtn){ICON_ADD, _("add")}; break;
	case PV_TIMER:
		btns[nb++] = g_tmr.state == TM_RUNNING ? (AppBarBtn){ICON_PAUSE, _("pause")} : (AppBarBtn){ICON_PLAY, _("start")};
		btns[nb++] = (AppBarBtn){ICON_REFRESH, _("reset"), g_tmr.state == TM_IDLE};
		btns[nb++] = (AppBarBtn){ICON_EDIT, _("edit"), g_tmr.state == TM_RUNNING};
		break;
	default:
		btns[nb++] = g_sw.running ? (AppBarBtn){ICON_PAUSE, _("pause")} : (AppBarBtn){ICON_PLAY, _("start")};
		btns[nb++] = (AppBarBtn){ICON_FLAG, _("lap"), !g_sw.running};
		btns[nb++] = (AppBarBtn){ICON_REFRESH, _("reset"), !g_sw.running && sw_now() == 0};
		break;
	}
	int a = appbar_draw(ui, &g_bar, area, btns, nb, NULL, 0);
	/* keyboard shortcuts for the stopwatch / timer on desktops */
	uint32_t cp;
	if ((page == PV_STOPWATCH || page == PV_TIMER) && ui_char(ui, &cp)) {
		if (cp == ' ') a = 0;
		else if (cp == 'l' || cp == 'L') a = page == PV_STOPWATCH ? 1 : -1;
		else if (cp == 'r' || cp == 'R') a = page == PV_STOPWATCH ? 2 : 1;
	}
	if (a < 0) return;
	switch (page) {
	case PV_ALARMS: if (a == 0) open_editor(-1); break;
	case PV_WORLD: if (a == 0) open_city_page(); break;
	case PV_TIMER:
		if (a == 0) timer_toggle();
		else if (a == 1 && g_tmr.state != TM_IDLE) timer_reset();
		else if (a == 2 && g_tmr.state != TM_RUNNING) open_presets();
		break;
	default:
		if (a == 0) sw_toggle();
		else if (a == 1) sw_lap();
		else if (a == 2 && !g_sw.running) sw_reset();
		break;
	}
}

/* ------------------------------------------------------------ alarm editor */

static void time_cb(int b, void *ud) {
	if (b == 0) dialog_time_get(&g_ed.hour, &g_ed.min);
}

static void snooze_cb(int b, void *ud) {
	if (b == 0) g_ed.snooze = SNOOZE_MIN[CLAMP(dialog_picker_get(), 0, (int)ARRAY_LEN(SNOOZE_MIN) - 1)];
}

static int alarm_cmp(const void *a, const void *b) {
	const Alarm *x = a, *y = b;
	return (x->hour * 60 + x->min) - (y->hour * 60 + y->min);
}

static void editor_save(void) {
	if (!str_trim(g_ed.name)[0]) str_copy(g_ed.name, sizeof g_ed.name, _("Alarm"));
	else {
		char tmp[64];
		str_copy(tmp, sizeof tmp, str_trim(g_ed.name));
		str_copy(g_ed.name, sizeof g_ed.name, tmp);
	}
	g_ed.on = true;
	if (g_ed_idx >= 0 && g_ed_idx < g_nalarms) g_alarms[g_ed_idx] = g_ed;
	else if (g_nalarms < MAX_ALARMS) g_alarms[g_nalarms++] = g_ed;
	else { shell_toast(tr(APP_NAME), _("you have too many alarms. delete one first."), NULL); return; }
	qsort(g_alarms, (size_t)g_nalarms, sizeof(Alarm), alarm_cmp);
	save();
	toast_alarm_set(&g_ed);
	sh.ui.focus = 0;
	nav_pop(&g_nav, &sh.ui);
}

static void draw_editor(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect content = appbar_content(&g_ed_bar, area);
	double y = ui_page_header(ui, 14, area.y + 6, tr(APP_NAME), g_ed_idx < 0 ? _("new alarm") : _("edit alarm"));
	Rect view = R(area.x, y, area.w, rect_bottom(content) - y);
	double x = 24, w = MIN(area.w - 48, 560);
	const double CB = 46;
	double total = 80 + 82 + 36 + 8 * CB + 12 + 82 + 90 + 20;
	ui_scroll_begin(ui, ui_id("ed-scroll"), &g_ed_scroll, view, total);
	cairo_translate(cr, view.x, view.y);
	double yy = 4;
	gfx_text(cr, _("name"), x, yy, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	ui_textbox(ui, ui_id("ed-name"), R(x, yy + 22, w, 48), g_ed.name, sizeof g_ed.name, _("Alarm"), 0, NULL);
	yy += 80;
	char tbuf[32];
	fmt_hm(tbuf, sizeof tbuf, g_ed.hour, g_ed.min);
	if (pick_field(ui, ui_id("ed-time"), R(x, yy, w, 74), _("time"), tbuf))
		dialog_time(_("alarm time"), g_ed.hour, g_ed.min, time_cb, NULL);
	yy += 82;
	gfx_text(cr, _("repeats"), x, yy + 8, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	yy += 36;
	bool once = g_ed.days == 0;
	if (ui_checkbox(ui, ui_id("ed-once"), R(x - 4, yy, w, CB), _("only once"), &once)) g_ed.days = once ? 0 : 0x7f;
	yy += CB;
	int first = i18n_first_weekday();
	for (int k = 0; k < 7; k++) {
		int d = (first + k) % 7;
		bool v = (g_ed.days >> d) & 1;
		const char *wd = i18n_lower(i18n_weekday(d, false));
		if (ui_checkbox(ui, ui_idn("ed-day", d), R(x - 4, yy, w, CB), wd, &v)) g_ed.days = v ? (g_ed.days | (1 << d)) : (g_ed.days & ~(1 << d));
		yy += CB;
	}
	yy += 12;
	if (pick_field(ui, ui_id("ed-snooze"), R(x, yy, w, 74), _("snooze time"), snooze_label(g_ed.snooze))) {
		static char labels[ARRAY_LEN(SNOOZE_MIN)][32];
		static const char *ptr[ARRAY_LEN(SNOOZE_MIN)];
		int sel = 1;
		for (int i = 0; i < (int)ARRAY_LEN(SNOOZE_MIN); i++) {
			str_copy(labels[i], sizeof labels[i], snooze_label(SNOOZE_MIN[i]));
			ptr[i] = labels[i];
			if (SNOOZE_MIN[i] == g_ed.snooze) sel = i;
		}
		dialog_picker(_("snooze time"), ptr, (int)ARRAY_LEN(ptr), sel, snooze_cb, NULL);
	}
	yy += 82;
	ui_toggle(ui, ui_id("ed-sound"), R(x, yy, w, 70), _("sound"), &g_ed.sound);
	ui_scroll_end(ui);

	AppBarBtn btns[3] = {{ICON_SAVE, _("save")}, {ICON_DELETE, _("delete"), g_ed_idx < 0}, {ICON_CLOSE, _("cancel")}};
	int a = appbar_draw(ui, &g_ed_bar, area, btns, 3, NULL, 0);
	if (a == 0) editor_save();
	else if (a == 1) { delete_alarm(g_ed_idx); sh.ui.focus = 0; nav_pop(&g_nav, &sh.ui); }
	else if (a == 2) { sh.ui.focus = 0; nav_pop(&g_nav, &sh.ui); }
}

/* --------------------------------------------------------------- city list */

static void city_free_zones(void) {
	for (int i = 0; i < NCITIES; i++) {
		if (g_city_zone[i]) { g_time_zone_unref(g_city_zone[i]); g_city_zone[i] = NULL; }
	}
}

static void draw_city(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = ui_page_header(ui, 14, area.y + 6, tr(APP_NAME), _("add city"));
	UiId tb = ui_id("city-search");
	if (g_city_focus) { ui_focus(ui, tb); g_city_focus = false; }
	bool changed = false;
	ui_textbox(ui, tb, R(24, y + 4, area.w - 48, 48), g_city_q, sizeof g_city_q, _("search for a city"), 0, &changed);
	if (changed) g_city_scroll = (Scroll){0};
	y += 64;
	static int idx[NCITIES];
	int n = 0;
	for (int i = 0; i < NCITIES; i++) {
		if (g_city_q[0] && !str_contains_fold(tr(CITIES[i].city), g_city_q) && !str_contains_fold(CITIES[i].city, g_city_q) &&
		    !str_contains_fold(tr(CITIES[i].country), g_city_q) && !str_contains_fold(CITIES[i].tz, g_city_q))
			continue;
		idx[n++] = i;
	}
	/* sorted by the shown (translated) name */
	for (int i = 1; i < n; i++) {
		int v = idx[i], j = i - 1;
		while (j >= 0 && app_collate(tr(CITIES[idx[j]].city), tr(CITIES[v].city)) > 0) { idx[j + 1] = idx[j]; j--; }
		idx[j + 1] = v;
	}
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	if (!n) {
		gfx_text(cr, _("no matching cities"), 24, y + 10, 20, FW_SEMILIGHT, th->sub, TA_LEFT, area.w - 48, 0);
		return;
	}
	const double ROW = 68;
	double off = ui_scroll_begin(ui, ui_id("city-scroll"), &g_city_scroll, view, n * ROW + 16);
	cairo_translate(cr, view.x, view.y);
	int chosen = -1;
	for (int k = 0; k < n; k++) {
		Rect rr = R(0, k * ROW, view.w, ROW);
		if (rr.y + ROW < off || rr.y > off + view.h) continue;
		int c = idx[k];
		if (!g_city_zone[c]) g_city_zone[c] = zone_new(CITIES[c].tz);
		GDateTime *dt = g_date_time_new_now(g_city_zone[c]);
		long offs = (long)(g_date_time_get_utc_offset(dt) / G_TIME_SPAN_SECOND);
		char off_s[32], sub[160];
		if (offs % 3600) snprintf(off_s, sizeof off_s, "UTC%c%ld:%02ld", offs < 0 ? '-' : '+', labs(offs) / 3600, labs(offs) / 60 % 60);
		else if (offs) snprintf(off_s, sizeof off_s, "UTC%c%ld", offs < 0 ? '-' : '+', labs(offs) / 3600);
		else str_copy(off_s, sizeof off_s, "UTC");
		snprintf(sub, sizeof sub, "%s · %s", tr(CITIES[c].country), off_s);
		UiId id = ui_idn("city-row", c);
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
		char tbuf[32];
		fmt_hm(tbuf, sizeof tbuf, g_date_time_get_hour(dt), g_date_time_get_minute(dt));
		g_date_time_unref(dt);
		gfx_text(cr, tbuf, view.w - 24, rr.y + 12, 20, FW_SEMILIGHT, th->sub, TA_RIGHT, 120, 1);
		gfx_text(cr, tr(CITIES[c].city), 24, rr.y + 8, 24, FW_SEMILIGHT, th->fg, TA_LEFT, view.w - 170, 1);
		gfx_text(cr, sub, 24, rr.y + 40, 15, FW_REGULAR, th->sub, TA_LEFT, view.w - 170, 1);
		if (ui_clicked(ui, id)) chosen = c;
	}
	ui_scroll_end(ui);
	if (chosen >= 0) {
		bool dup = false;
		for (int i = 0; i < g_nworld; i++) if (g_world[i].city == chosen) dup = true;
		if (!dup && g_nworld < MAX_WORLD) {
			g_world[g_nworld++] = (World){chosen, NULL};
			save();
		}
		sh.ui.focus = 0;
		nav_pop(&g_nav, &sh.ui);
	}
}

/* ----------------------------------------------------------- custom timer */

static void set_pick_cb(int b, void *ud) {
	if (b != 0) return;
	int v = dialog_picker_get();
	if (g_set_field == 0) g_set_h = v;
	else if (g_set_field == 1) g_set_m = v * 5;
	else g_set_s = v * 5;
}

static void draw_timer_set(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = ui_page_header(ui, 14, area.y + 6, tr(APP_NAME), _("set timer"));
	static const char *LABELS[3] = {N_("hours"), N_("minutes"), N_("seconds")};
	int *vals[3] = {&g_set_h, &g_set_m, &g_set_s};
	const int MAXV[3] = {99, 59, 59};
	double colw = MIN(130, (area.w - 48) / 3);
	double x0 = area.x + (area.w - colw * 3) / 2;
	y += 20;
	for (int c = 0; c < 3; c++) {
		double cx = x0 + colw * (c + 0.5);
		gfx_text(cr, tr(LABELS[c]), cx, y, 15, FW_REGULAR, th->sub, TA_CENTER, colw - 8, 1);
		if (ui_circle_button(ui, ui_idn("set-up", c), cx, y + 58, 44, ICON_CHEVRON_U, NULL))
			*vals[c] = *vals[c] >= MAXV[c] ? 0 : *vals[c] + 1;
		char b[8];
		snprintf(b, sizeof b, "%02d", *vals[c]);
		Rect nr = R(cx - colw / 2 + 6, y + 92, colw - 12, 92);
		UiId nid = ui_idn("set-num", c);
		ui_hit(ui, nid, nr);
		bool pr = ui_pressed(ui, nid);
		if (pr) gfx_fill_rect(cr, nr, th->accent);
		else gfx_stroke_rect(cr, nr, 2, th->fg);
		gfx_text_in(cr, b, nr, 56, FW_LIGHT, pr ? th->on_accent : th->fg, TA_CENTER, true);
		if (ui_circle_button(ui, ui_idn("set-down", c), cx, y + 218, 44, ICON_CHEVRON_D, NULL))
			*vals[c] = *vals[c] <= 0 ? MAXV[c] : *vals[c] - 1;
		if (ui_clicked(ui, nid)) {
			/* coarse values in a looping picker; the chevrons fine-tune */
			static char labels[24][8];
			static const char *ptr[24];
			int n = c == 0 ? 24 : 12, sel = c == 0 ? MIN(*vals[c], 23) : *vals[c] / 5;
			for (int i = 0; i < n; i++) {
				snprintf(labels[i], sizeof labels[i], "%02d", c == 0 ? i : i * 5);
				ptr[i] = labels[i];
			}
			g_set_field = c;
			dialog_picker(tr(LABELS[c]), ptr, n, sel, set_pick_cb, NULL);
		}
	}
	AppBarBtn btns[2] = {{ICON_CHECK, _("done")}, {ICON_CLOSE, _("cancel")}};
	int a = appbar_draw(ui, &g_set_bar, area, btns, 2, NULL, 0);
	int total = g_set_h * 3600 + g_set_m * 60 + g_set_s;
	if (a == 0) {
		if (total > 0) timer_set_dur(total);
		nav_pop(&g_nav, &sh.ui);
	} else if (a == 1) {
		nav_pop(&g_nav, &sh.ui);
	}
}

/* ------------------------------------------------------------------- app -- */

static void nav_page_fn(UI *ui, int page, Rect area, void *ud) {
	switch (page) {
	case P_EDIT: draw_editor(ui, area); break;
	case P_CITY: draw_city(ui, area); break;
	case P_TIMER_SET: draw_timer_set(ui, area); break;
	default: draw_root(ui, area); break;
	}
}

static void clock_draw(UI *ui, Rect area) {
	ensure_loaded();
	nav_draw(ui, &g_nav, area, nav_page_fn, NULL);
	if (g_nav.depth == 0 && !nav_busy(&g_nav, ui)) city_free_zones();
}

static void clock_init(void) {
	g_tick = timer_add(1000, true, tick_cb, NULL);
	g_precise = timer_add(0, false, precise_cb, NULL);
	nav_init(&g_nav, P_ROOT);
}

static void clock_open(const char *arg) {
	ensure_loaded();
	if (!arg || !*arg) return;
	int page = -1;
	if (str_eq(arg, "alarms")) page = PV_ALARMS;
	else if (str_eq(arg, "world")) page = PV_WORLD;
	else if (str_eq(arg, "timer")) page = PV_TIMER;
	else if (str_eq(arg, "stopwatch")) page = PV_STOPWATCH;
	else if (str_eq(arg, "new")) { nav_init(&g_nav, P_ROOT); g_pivot = (Pager){0}; open_editor(-1); return; }
	if (page >= 0) {
		nav_init(&g_nav, P_ROOT);
		g_pivot.pos = page;
		g_pivot.target = page;
	}
}

static bool clock_back(void) {
	switch (nav_page(&g_nav)) {
	case P_EDIT: if (appbar_back(&g_ed_bar)) return true; break;
	case P_CITY: if (appbar_back(&g_city_bar)) return true; break;
	case P_TIMER_SET: if (appbar_back(&g_set_bar)) return true; break;
	default: if (appbar_back(&g_bar)) return true; break;
	}
	if (nav_pop(&g_nav, &sh.ui)) { sh.ui.focus = 0; return true; }
	return false;
}

static void clock_close(void) {
	nav_init(&g_nav, P_ROOT);
	city_free_zones();
	for (int i = 0; i < g_nworld; i++) {
		if (g_world[i].zone) { g_time_zone_unref(g_world[i].zone); g_world[i].zone = NULL; }
	}
	if (g_frame) timer_set(g_frame, 0);
	g_bar = (AppBar){0};
}

/* --------------------------------------------------------------- live tile */

static void clock_tile(UI *ui, Rect r, TileSize size, double now) {
	ensure_loaded();
	cairo_t *cr = ui->cr;
	const char *name = NULL;
	time_t t = next_alarm(&name);
	double pad = 12, w = r.w - 2 * pad;
	bool wide = size == TILE_WIDE;
	double big = MIN(r.h * (wide ? 0.34 : 0.27), 64);
	if (t) {
		struct tm tm, today;
		localtime_r(&t, &tm);
		time_t tn = time(NULL);
		localtime_r(&tn, &today);
		icon_draw(cr, ICON_ALARM, rect_right(r) - 22, r.y + 22, 20, COL_WHITE);
		double y = r.y + 8;
		y += draw_big_time(cr, tm.tm_hour, tm.tm_min, r.x + pad - 2, y, big, COL_WHITE, TA_LEFT);
		const char *day;
		if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year) day = _("today");
		else if (t - tn < 86400 * 2 && (tm.tm_wday == (today.tm_wday + 1) % 7)) day = _("tomorrow");
		else day = i18n_weekday(tm.tm_wday, false);
		double ds = MIN(20, r.h * 0.1);
		y += gfx_text(cr, i18n_lower(day), r.x + pad, y, ds, FW_SEMILIGHT, COL_WHITE, TA_LEFT, w, 1);
		if (name && *name) gfx_text(cr, name, r.x + pad, y, MIN(16, r.h * 0.08), FW_REGULAR, col_alpha(COL_WHITE, 0.85f), TA_LEFT, w, 1);
	} else {
		time_t tn = time(NULL);
		struct tm tm;
		localtime_r(&tn, &tm);
		double y = r.y + 8;
		y += draw_big_time(cr, tm.tm_hour, tm.tm_min, r.x + pad - 2, y, big, COL_WHITE, TA_LEFT);
		char d[96];
		if (wide) i18n_fmt_date_long(d, sizeof d, &tm);
		else snprintf(d, sizeof d, "%s %d", i18n_weekday(tm.tm_wday, false), tm.tm_mday);
		gfx_text(cr, i18n_lower(d), r.x + pad, y, MIN(20, r.h * 0.1), FW_SEMILIGHT, COL_WHITE, TA_LEFT, w, 2);
	}
	app_tile_name(ui, r, tr(APP_NAME));
}

const AppDef app_clock = {
	.id = "clock", .name = N_("Alarms & Clock"), .icon = ICON_CLOCK, .color = 0,
	.keywords = "alarm clock timer stopwatch world time zone wake",
	.init = clock_init, .open = clock_open, .draw = clock_draw, .back = clock_back, .close = clock_close,
	.tile = clock_tile,
};

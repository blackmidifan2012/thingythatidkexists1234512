/* Weather: Bing Weather style current conditions, hourly and 10 day forecasts from
 * Open-Meteo (no API key), favourite places and a live tile. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../sys.h"
#include "../util.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <sys/wait.h>

#define MAXH 72
#define MAXD 10
#define MAXPLACES 16
#define MAXGEO 10
#define STALE_SEC (30 * 60)
#define RETRY_SEC 300.0

typedef struct {
	time_t fetched;              /* wall clock time of the download */
	bool fahr;                   /* values are in °F / mph / inch */
	bool sample;                 /* demo data while offline, never saved */
	int utc_off;                 /* seconds; place local time = UTC + utc_off */
	double temp, feels, hum, wind, wdir, precip, pres;
	int code;
	bool day;
	int nh;
	time_t ht[MAXH];             /* place-local times expressed as UTC epochs */
	float htemp[MAXH];
	short hcode[MAXH];
	signed char hpop[MAXH];
	int nd;
	time_t dd[MAXD], drise[MAXD], dset[MAXD];
	float dhi[MAXD], dlo[MAXD], duv[MAXD];
	short dcode[MAXD];
	signed char dpop[MAXD];
} Forecast;

typedef struct {
	char name[64], region[96];
	double lat, lon;
	Forecast *fc;
	bool fetching, failed;
	double last_try;
} Place;

typedef struct { char name[64], region[96]; double lat, lon; } GeoHit;

enum { PG_MAIN, PG_SEARCH };

static Place g_pl[MAXPLACES];
static int g_npl;                 /* g_pl[0] is the home place when cfg.city is set */
static int g_cur;                 /* place shown on the main page */
static bool g_loaded;
static Nav g_nav;
static Pager g_pivot;
static Scroll g_scr[4], g_search_scroll;
static AppBar g_bar;
static int g_expanded = -1;       /* expanded day row */
static Timer *g_refresh_timer;

static char g_q[96];
static GeoHit g_geo[MAXGEO];
static int g_ngeo;
static bool g_geo_busy, g_geo_err, g_geo_done;
static uint32_t g_geo_gen;
static Timer *g_geo_timer;
static bool g_focus_search;

static void fetch_place(int i);

/* ------------------------------------------------------------- helpers -- */

static bool same_pos(double a, double b, double c, double d) { return fabs(a - c) < 1e-3 && fabs(b - d) < 1e-3; }

static bool has_home(void) { return cfg.city[0] && !(cfg.lat == 0 && cfg.lon == 0); }

static char *url_encode(const char *s) {
	char *out = xmalloc(strlen(s) * 3 + 1), *w = out;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') *w++ = (char)*p;
		else w += sprintf(w, "%%%02X", *p);
	}
	*w = 0;
	return out;
}

/* Downloads url to stdout with curl (or wget). done gets the body. */
static bool http_get(const char *url, ProcDone done, void *ud) {
	static int tool = -1;   /* 0 none, 1 curl, 2 wget */
	if (tool < 0) tool = proc_exists("curl") ? 1 : proc_exists("wget") ? 2 : 0;
	if (tool == 1) {
		char *argv[] = {"curl", "-s", "-f", "-L", "--max-time", "15", (char *)url, NULL};
		return proc_run(argv, done, ud);
	}
	if (tool == 2) {
		char *argv[] = {"wget", "-q", "-T", "15", "-O-", (char *)url, NULL};
		return proc_run(argv, done, ud);
	}
	return false;
}

static bool proc_ok(int status, size_t len) { return WIFEXITED(status) && WEXITSTATUS(status) == 0 && len > 0; }

static time_t parse_iso(const char *s) {
	struct tm tm = {0};
	int y = 0, mo = 0, d = 0, h = 0, mi = 0;
	if (!s || sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) < 3) return 0;
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	return timegm(&tm);
}

static time_t place_now(const Forecast *f) { return time(NULL) + (f ? f->utc_off : 0); }

static struct tm utc_tm(time_t t) {
	struct tm tm;
	gmtime_r(&t, &tm);
	return tm;
}

/* ---------------------------------------------------- conditions / units -- */

static bool is_snow(int c) { return (c >= 71 && c <= 77) || c == 85 || c == 86; }

static Icon cond_icon(int c, bool day) {
	if (c <= 1) return day ? ICON_SUN : ICON_MOON;
	if (c == 2) return day ? ICON_PARTLY : ICON_MOON;
	if (c == 3) return ICON_CLOUD;
	if (c == 45 || c == 48) return ICON_FOG;
	if (is_snow(c)) return ICON_SNOW;
	if (c >= 95) return ICON_STORM;
	return ICON_RAIN;
}

/* Weather glyph; the crescent is drawn here so it reads well at large sizes. */
static void wx_icon(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c) {
	if (ic != ICON_MOON) { icon_draw(cr, ic, cx, cy, size, c); return; }
	double r = size * 0.36;
	cairo_save(cr);
	cairo_push_group(cr);
	gfx_fill_circle(cr, cx, cy, r, c);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	gfx_fill_circle(cr, cx + r * 0.55, cy - r * 0.35, r * 0.82, COL_BLACK);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
	cairo_restore(cr);
}

static const char *cond_text(int c, bool day) {
	switch (c) {
	case 0: return day ? _("Sunny") : _("Clear");
	case 1: return day ? _("Mostly sunny") : _("Mostly clear");
	case 2: return _("Partly cloudy");
	case 3: return _("Cloudy");
	case 45: return _("Fog");
	case 48: return _("Freezing fog");
	case 51: return _("Light drizzle");
	case 53: return _("Drizzle");
	case 55: return _("Heavy drizzle");
	case 56: case 57: return _("Freezing drizzle");
	case 61: return _("Light rain");
	case 63: return _("Rain");
	case 65: return _("Heavy rain");
	case 66: case 67: return _("Freezing rain");
	case 71: return _("Light snow");
	case 73: return _("Snow");
	case 75: return _("Heavy snow");
	case 77: return _("Snow grains");
	case 80: return _("Light showers");
	case 81: return _("Showers");
	case 82: return _("Heavy showers");
	case 85: return _("Snow showers");
	case 86: return _("Heavy snow showers");
	case 95: return _("Thunderstorm");
	case 96: case 99: return _("Thunderstorm with hail");
	default: return _("Unknown");
	}
}

/* Flat page colours like the Bing Weather backgrounds */
static Color cond_bg(const Forecast *f) {
	if (!f) return RGBX(0x1BA1E2);
	int c = f->code;
	if (!f->day) {
		if (c <= 1) return RGBX(0x10203F);
		if (c == 2) return RGBX(0x1B2A44);
		if (c >= 95) return RGBX(0x241F38);
		if (is_snow(c)) return RGBX(0x2D3A50);
		if (c == 3 || c == 45 || c == 48) return RGBX(0x283039);
		return RGBX(0x1C2A3A);
	}
	if (c <= 1) return RGBX(0x1E8AD6);
	if (c == 2) return RGBX(0x3B7DBF);
	if (c == 3) return RGBX(0x66717E);
	if (c == 45 || c == 48) return RGBX(0x76808B);
	if (c >= 95) return RGBX(0x3E3C5E);
	if (is_snow(c)) return RGBX(0x5F7D9C);
	return RGBX(0x45607A);
}

static double conv_t(const Forecast *f, double v) {
	bool want_f = !cfg.celsius;
	if (f->fahr == want_f) return v;
	return want_f ? v * 9 / 5 + 32 : (v - 32) * 5 / 9;
}

static void fmt_temp(char *b, size_t n, const Forecast *f, double v) {
	long t = lround(conv_t(f, v));
	snprintf(b, n, "%ld°", t);
}

static void fmt_wind(char *b, size_t n, const Forecast *f) {
	static const char *EN[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
	static const char *TR[] = {"K", "KD", "D", "GD", "G", "GB", "B", "KB"};
	int d = (int)lround(fmod(f->wdir + 360, 360) / 45) % 8;
	double km = f->fahr ? f->wind * 1.609344 : f->wind;
	const char *dir = i18n_lang() == LANG_TR ? TR[d] : EN[d];
	if (cfg.celsius) snprintf(b, n, "%.0f %s %s", km, _("km/h"), dir);
	else snprintf(b, n, "%.0f %s %s", km / 1.609344, _("mph"), dir);
}

static void fmt_precip(char *b, size_t n, const Forecast *f) {
	double mm = f->fahr ? f->precip * 25.4 : f->precip;
	if (cfg.celsius) snprintf(b, n, "%.1f %s", mm, _("mm"));
	else snprintf(b, n, "%.2f %s", mm / 25.4, _("in"));
}

static void fmt_pressure(char *b, size_t n, const Forecast *f) {
	if (cfg.celsius) snprintf(b, n, "%.0f %s", f->pres, _("hPa"));
	else snprintf(b, n, "%.2f %s", f->pres * 0.0295300, _("inHg"));
}

static const char *uv_text(double uv) {
	if (uv < 3) return _("low");
	if (uv < 6) return _("moderate");
	if (uv < 8) return _("high");
	if (uv < 11) return _("very high");
	return _("extreme");
}

static void fmt_clock(char *b, size_t n, time_t t) {
	struct tm tm = utc_tm(t);
	i18n_fmt_time(b, n, &tm, cfg.h24);
}

/* index of today's entry in the daily list */
static int today_index(const Forecast *f) {
	time_t day = place_now(f) / 86400 * 86400;
	for (int i = 0; i < f->nd; i++)
		if (f->dd[i] >= day) return i;
	return f->nd ? f->nd - 1 : 0;
}

static int hour_start(const Forecast *f) {
	time_t now = place_now(f) / 3600 * 3600;
	for (int i = 0; i < f->nh; i++)
		if (f->ht[i] >= now) return i;
	return f->nh;
}

static bool hour_is_day(const Forecast *f, time_t t) {
	for (int i = 0; i < f->nd; i++)
		if (t >= f->dd[i] && t < f->dd[i] + 86400) return t >= f->drise[i] && t < f->dset[i];
	struct tm tm = utc_tm(t);
	return tm.tm_hour >= 7 && tm.tm_hour < 19;
}

/* ------------------------------------------------------------ persistence -- */

static void add_num_array(cJSON *o, const char *key, int n, double (*get)(const Forecast *, int), const Forecast *f) {
	cJSON *a = cJSON_AddArrayToObject(o, key);
	for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(get(f, i)));
}

static double g_ht(const Forecast *f, int i) { return (double)f->ht[i]; }
static double g_htemp(const Forecast *f, int i) { return f->htemp[i]; }
static double g_hcode(const Forecast *f, int i) { return f->hcode[i]; }
static double g_hpop(const Forecast *f, int i) { return f->hpop[i]; }
static double g_dd(const Forecast *f, int i) { return (double)f->dd[i]; }
static double g_drise(const Forecast *f, int i) { return (double)f->drise[i]; }
static double g_dset(const Forecast *f, int i) { return (double)f->dset[i]; }
static double g_dhi(const Forecast *f, int i) { return f->dhi[i]; }
static double g_dlo(const Forecast *f, int i) { return f->dlo[i]; }
static double g_duv(const Forecast *f, int i) { return f->duv[i]; }
static double g_dcode(const Forecast *f, int i) { return f->dcode[i]; }
static double g_dpop(const Forecast *f, int i) { return f->dpop[i]; }

static cJSON *fc_to_json(const Forecast *f) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "fetched", (double)f->fetched);
	cJSON_AddBoolToObject(o, "fahr", f->fahr);
	cJSON_AddNumberToObject(o, "off", f->utc_off);
	double cur[] = {f->temp, f->feels, f->hum, f->wind, f->wdir, f->precip, f->pres, f->code, f->day};
	cJSON_AddItemToObject(o, "cur", cJSON_CreateDoubleArray(cur, (int)ARRAY_LEN(cur)));
	add_num_array(o, "ht", f->nh, g_ht, f);
	add_num_array(o, "htemp", f->nh, g_htemp, f);
	add_num_array(o, "hcode", f->nh, g_hcode, f);
	add_num_array(o, "hpop", f->nh, g_hpop, f);
	add_num_array(o, "dd", f->nd, g_dd, f);
	add_num_array(o, "drise", f->nd, g_drise, f);
	add_num_array(o, "dset", f->nd, g_dset, f);
	add_num_array(o, "dhi", f->nd, g_dhi, f);
	add_num_array(o, "dlo", f->nd, g_dlo, f);
	add_num_array(o, "duv", f->nd, g_duv, f);
	add_num_array(o, "dcode", f->nd, g_dcode, f);
	add_num_array(o, "dpop", f->nd, g_dpop, f);
	return o;
}

static double arr_at(const cJSON *o, const char *key, int i) {
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
	const cJSON *v = cJSON_GetArrayItem(a, i);
	return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int arr_len(const cJSON *o, const char *key) { return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, key)); }

static Forecast *fc_from_json(const cJSON *o) {
	if (!cJSON_IsObject(o) || arr_len(o, "cur") < 9) return NULL;
	Forecast *f = xcalloc(1, sizeof *f);
	f->fetched = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "fetched"));
	f->fahr = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "fahr"));
	f->utc_off = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "off"));
	f->temp = arr_at(o, "cur", 0); f->feels = arr_at(o, "cur", 1); f->hum = arr_at(o, "cur", 2);
	f->wind = arr_at(o, "cur", 3); f->wdir = arr_at(o, "cur", 4); f->precip = arr_at(o, "cur", 5);
	f->pres = arr_at(o, "cur", 6); f->code = (int)arr_at(o, "cur", 7); f->day = arr_at(o, "cur", 8) != 0;
	f->nh = MIN(arr_len(o, "ht"), MAXH);
	for (int i = 0; i < f->nh; i++) {
		f->ht[i] = (time_t)arr_at(o, "ht", i);
		f->htemp[i] = (float)arr_at(o, "htemp", i);
		f->hcode[i] = (short)arr_at(o, "hcode", i);
		f->hpop[i] = (signed char)arr_at(o, "hpop", i);
	}
	f->nd = MIN(arr_len(o, "dd"), MAXD);
	for (int i = 0; i < f->nd; i++) {
		f->dd[i] = (time_t)arr_at(o, "dd", i);
		f->drise[i] = (time_t)arr_at(o, "drise", i);
		f->dset[i] = (time_t)arr_at(o, "dset", i);
		f->dhi[i] = (float)arr_at(o, "dhi", i);
		f->dlo[i] = (float)arr_at(o, "dlo", i);
		f->duv[i] = (float)arr_at(o, "duv", i);
		f->dcode[i] = (short)arr_at(o, "dcode", i);
		f->dpop[i] = (signed char)arr_at(o, "dpop", i);
	}
	return f;
}

static void save_all(void) {
	cJSON *root = cJSON_CreateObject();
	cJSON *pl = cJSON_AddArrayToObject(root, "places");
	for (int i = 0; i < g_npl; i++) {
		Place *p = &g_pl[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", p->name);
		cJSON_AddStringToObject(o, "region", p->region);
		cJSON_AddNumberToObject(o, "lat", p->lat);
		cJSON_AddNumberToObject(o, "lon", p->lon);
		if (p->fc && !p->fc->sample) cJSON_AddItemToObject(o, "fc", fc_to_json(p->fc));
		cJSON_AddItemToArray(pl, o);
	}
	app_json_save("weather.json", root);
	cJSON_Delete(root);
}

static int find_place(double lat, double lon) {
	for (int i = 0; i < g_npl; i++)
		if (same_pos(g_pl[i].lat, g_pl[i].lon, lat, lon)) return i;
	return -1;
}

/* Makes g_pl[0] match cfg.city/lat/lon (the home place). */
static void sync_home(void) {
	if (!has_home()) return;
	int i = find_place(cfg.lat, cfg.lon);
	if (i == 0) { str_copy(g_pl[0].name, sizeof g_pl[0].name, cfg.city); return; }
	Place home;
	if (i > 0) {
		home = g_pl[i];
		memmove(&g_pl[1], &g_pl[0], (size_t)i * sizeof(Place));
	} else {
		if (g_npl >= MAXPLACES) { free(g_pl[MAXPLACES - 1].fc); g_npl--; }
		home = (Place){.lat = cfg.lat, .lon = cfg.lon};
		memmove(&g_pl[1], &g_pl[0], (size_t)g_npl * sizeof(Place));
		g_npl++;
	}
	str_copy(home.name, sizeof home.name, cfg.city);
	g_pl[0] = home;
}

static void ensure_loaded(void) {
	if (g_loaded) return;
	g_loaded = true;
	cJSON *root = app_json_load("weather.json");
	const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "places"), *o;
	cJSON_ArrayForEach(o, pl) {
		if (g_npl >= MAXPLACES) break;
		const cJSON *lat = cJSON_GetObjectItemCaseSensitive(o, "lat"), *lon = cJSON_GetObjectItemCaseSensitive(o, "lon");
		if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;
		Place *p = &g_pl[g_npl++];
		*p = (Place){.lat = lat->valuedouble, .lon = lon->valuedouble};
		const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name"));
		const char *r = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "region"));
		str_copy(p->name, sizeof p->name, n ? n : "");
		str_copy(p->region, sizeof p->region, r ? r : "");
		p->fc = fc_from_json(cJSON_GetObjectItemCaseSensitive(o, "fc"));
	}
	cJSON_Delete(root);
	sync_home();
}

static void free_all(void) {
	for (int i = 0; i < g_npl; i++) free(g_pl[i].fc);
	g_npl = 0;
	g_loaded = false;
}

/* ------------------------------------------------------------- sample data -- */

/* Clearly labelled demo forecast, only used when offline in --demo mode. */
static Forecast *sample_forecast(void) {
	Forecast *f = xcalloc(1, sizeof *f);
	f->sample = true;
	f->fahr = false;
	f->fetched = time(NULL);
	f->utc_off = 0;
	struct tm lt;
	time_t now = time(NULL);
	localtime_r(&now, &lt);
	f->utc_off = (int)lt.tm_gmtoff;
	time_t pn = now + f->utc_off;
	int hour = utc_tm(pn).tm_hour;
	f->day = hour >= 7 && hour < 18;
	f->code = 2;
	f->temp = 17 + 5 * sin((hour - 9) / 24.0 * 2 * M_PI);
	f->feels = f->temp - 1;
	f->hum = 62;
	f->wind = 14;
	f->wdir = 225;
	f->precip = 0;
	f->pres = 1016;
	static const short codes[MAXD] = {2, 1, 3, 61, 80, 2, 0, 1, 95, 3};
	time_t day0 = pn / 86400 * 86400;
	f->nd = MAXD;
	for (int i = 0; i < MAXD; i++) {
		f->dd[i] = day0 + (time_t)i * 86400;
		f->drise[i] = f->dd[i] + 7 * 3600 + 12 * 60;
		f->dset[i] = f->dd[i] + 17 * 3600 + 48 * 60;
		f->dcode[i] = codes[i];
		f->dhi[i] = (float)(21 + 3 * sin(i * 0.9));
		f->dlo[i] = f->dhi[i] - 8 - (i % 3);
		f->dpop[i] = (signed char)(codes[i] >= 61 ? 70 : codes[i] == 3 ? 20 : 5);
		f->duv[i] = codes[i] <= 1 ? 6 : 3;
	}
	f->nh = MAXH;
	time_t h0 = pn / 3600 * 3600;
	for (int i = 0; i < MAXH; i++) {
		f->ht[i] = h0 + (time_t)i * 3600;
		int h = utc_tm(f->ht[i]).tm_hour, d = MIN(i / 24, MAXD - 1);
		f->htemp[i] = (float)(17 + 5 * sin((h - 9) / 24.0 * 2 * M_PI));
		f->hcode[i] = codes[d];
		f->hpop[i] = f->dpop[d];
	}
	return f;
}

/* ---------------------------------------------------------------- fetching -- */

typedef struct { double lat, lon; bool fahr; } FetchCtx;

static double json_num(const cJSON *o, const char *k) { return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, k)); }

static Forecast *parse_forecast(const char *txt, bool fahr) {
	cJSON *root = cJSON_Parse(txt);
	const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
	const cJSON *hr = cJSON_GetObjectItemCaseSensitive(root, "hourly");
	const cJSON *dy = cJSON_GetObjectItemCaseSensitive(root, "daily");
	if (!cJSON_IsObject(cur) || !cJSON_IsObject(hr) || !cJSON_IsObject(dy)) { cJSON_Delete(root); return NULL; }
	Forecast *f = xcalloc(1, sizeof *f);
	f->fetched = time(NULL);
	f->fahr = fahr;
	f->utc_off = (int)json_num(root, "utc_offset_seconds");
	f->temp = json_num(cur, "temperature_2m");
	f->feels = json_num(cur, "apparent_temperature");
	f->hum = json_num(cur, "relative_humidity_2m");
	f->wind = json_num(cur, "wind_speed_10m");
	f->wdir = json_num(cur, "wind_direction_10m");
	f->precip = json_num(cur, "precipitation");
	f->pres = json_num(cur, "pressure_msl");
	f->code = (int)json_num(cur, "weather_code");
	f->day = json_num(cur, "is_day") != 0;
	if (isnan(f->temp)) { free(f); cJSON_Delete(root); return NULL; }
	time_t now_h = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cur, "time"))) / 3600 * 3600;
	int n = arr_len(hr, "time");
	const cJSON *times = cJSON_GetObjectItemCaseSensitive(hr, "time");
	for (int i = 0; i < n && f->nh < MAXH; i++) {
		time_t t = parse_iso(cJSON_GetStringValue(cJSON_GetArrayItem(times, i)));
		if (t < now_h) continue;
		f->ht[f->nh] = t;
		f->htemp[f->nh] = (float)arr_at(hr, "temperature_2m", i);
		f->hcode[f->nh] = (short)arr_at(hr, "weather_code", i);
		f->hpop[f->nh] = (signed char)CLAMP(arr_at(hr, "precipitation_probability", i), 0, 100);
		f->nh++;
	}
	const cJSON *dt = cJSON_GetObjectItemCaseSensitive(dy, "time");
	const cJSON *rise = cJSON_GetObjectItemCaseSensitive(dy, "sunrise"), *set = cJSON_GetObjectItemCaseSensitive(dy, "sunset");
	f->nd = MIN(arr_len(dy, "time"), MAXD);
	for (int i = 0; i < f->nd; i++) {
		f->dd[i] = parse_iso(cJSON_GetStringValue(cJSON_GetArrayItem(dt, i)));
		f->drise[i] = parse_iso(cJSON_GetStringValue(cJSON_GetArrayItem(rise, i)));
		f->dset[i] = parse_iso(cJSON_GetStringValue(cJSON_GetArrayItem(set, i)));
		f->dhi[i] = (float)arr_at(dy, "temperature_2m_max", i);
		f->dlo[i] = (float)arr_at(dy, "temperature_2m_min", i);
		f->duv[i] = (float)arr_at(dy, "uv_index_max", i);
		f->dcode[i] = (short)arr_at(dy, "weather_code", i);
		f->dpop[i] = (signed char)CLAMP(arr_at(dy, "precipitation_probability_max", i), 0, 100);
	}
	cJSON_Delete(root);
	return f;
}

static void fetch_done(int status, char *out, size_t len, void *ud) {
	FetchCtx *c = ud;
	int i = find_place(c->lat, c->lon);
	bool fahr = c->fahr;
	free(c);
	if (i < 0) return;
	Place *p = &g_pl[i];
	p->fetching = false;
	Forecast *f = proc_ok(status, len) ? parse_forecast(out, fahr) : NULL;
	if (f) {
		free(p->fc);
		p->fc = f;
		p->failed = false;
		save_all();
	} else {
		p->failed = true;
		if (!p->fc && sys_demo()) p->fc = sample_forecast();
	}
	shell_invalidate();
}

static void fetch_place(int i) {
	if (i < 0 || i >= g_npl) return;
	Place *p = &g_pl[i];
	if (p->fetching) return;
	p->last_try = now_sec();
	char *url = xasprintf("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
		"&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,"
		"wind_direction_10m,precipitation,is_day,pressure_msl"
		"&hourly=temperature_2m,weather_code,precipitation_probability"
		"&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,precipitation_probability_max,uv_index_max"
		"&timezone=auto&forecast_days=10%s", p->lat, p->lon,
		cfg.celsius ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch");
	FetchCtx *c = xcalloc(1, sizeof *c);
	c->lat = p->lat;
	c->lon = p->lon;
	c->fahr = !cfg.celsius;
	if (http_get(url, fetch_done, c)) {
		p->fetching = true;
	} else {
		free(c);
		p->failed = true;
		if (!p->fc && sys_demo()) p->fc = sample_forecast();
	}
	free(url);
	shell_invalidate();
}

static bool is_stale(const Place *p) {
	return !p->fc || p->fc->sample || time(NULL) - p->fc->fetched > STALE_SEC || p->fc->fahr == cfg.celsius;
}

/* Cheap check used by draw paths: refresh stale data, at most every few minutes. */
static void maybe_refresh(int i) {
	if (i < 0 || i >= g_npl) return;
	Place *p = &g_pl[i];
	if (p->fetching || !is_stale(p)) return;
	if (p->last_try > 0 && now_sec() - p->last_try < RETRY_SEC) return;
	fetch_place(i);
}

static void refresh_timer_cb(void *ud) {
	ensure_loaded();
	if (has_home() && g_npl) maybe_refresh(0);
}

static void ensure_timer(void) {
	if (!g_refresh_timer && has_home()) g_refresh_timer = timer_add(45 * 60 * 1000, true, refresh_timer_cb, NULL);
}

/* ---------------------------------------------------------------- geocoding -- */

static void geo_done(int status, char *out, size_t len, void *ud) {
	uint32_t gen = (uint32_t)(uintptr_t)ud;
	if (gen != g_geo_gen) return;
	g_geo_busy = false;
	g_geo_done = true;
	g_ngeo = 0;
	cJSON *root = proc_ok(status, len) ? cJSON_Parse(out) : NULL;
	g_geo_err = root == NULL;
	const cJSON *r;
	cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(root, "results")) {
		if (g_ngeo >= MAXGEO) break;
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "name"));
		const cJSON *lat = cJSON_GetObjectItemCaseSensitive(r, "latitude"), *lon = cJSON_GetObjectItemCaseSensitive(r, "longitude");
		if (!name || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;
		GeoHit *g = &g_geo[g_ngeo++];
		str_copy(g->name, sizeof g->name, name);
		g->lat = lat->valuedouble;
		g->lon = lon->valuedouble;
		const char *a1 = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "admin1"));
		const char *co = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "country"));
		if (a1 && co && !str_eq(a1, name)) snprintf(g->region, sizeof g->region, "%s, %s", a1, co);
		else str_copy(g->region, sizeof g->region, co ? co : a1 ? a1 : "");
	}
	cJSON_Delete(root);
	shell_invalidate();
}

static void geo_search(void) {
	char q[96];
	str_copy(q, sizeof q, g_q);
	str_trim(q);
	g_geo_gen++;
	if (utf8_strlen(q) < 2) { g_geo_busy = false; g_geo_done = false; g_ngeo = 0; return; }
	char *enc = url_encode(q);
	char *url = xasprintf("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&language=%s&format=json", enc,
	                      i18n_lang() == LANG_TR ? "tr" : "en");
	g_geo_busy = http_get(url, geo_done, (void *)(uintptr_t)g_geo_gen);
	g_geo_err = !g_geo_busy;
	g_geo_done = !g_geo_busy;
	free(url);
	free(enc);
	shell_invalidate();
}

static void geo_timer_cb(void *ud) { geo_search(); }

/* Home place configured by name only (e.g. typed in Settings): resolve it once. */
static void home_resolve_done(int status, char *out, size_t len, void *ud) {
	cJSON *root = proc_ok(status, len) ? cJSON_Parse(out) : NULL;
	const cJSON *r = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "results"), 0);
	const cJSON *lat = cJSON_GetObjectItemCaseSensitive(r, "latitude"), *lon = cJSON_GetObjectItemCaseSensitive(r, "longitude");
	if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon) && cfg.city[0] && cfg.lat == 0 && cfg.lon == 0) {
		cfg.lat = lat->valuedouble;
		cfg.lon = lon->valuedouble;
		config_save_soon();
		ensure_loaded();
		sync_home();
		const char *co = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "country"));
		if (co && g_npl) str_copy(g_pl[0].region, sizeof g_pl[0].region, co);
		ensure_timer();
		fetch_place(0);
	}
	cJSON_Delete(root);
}

static void resolve_home_name(void) {
	static bool tried;
	if (tried || !cfg.city[0] || has_home()) return;
	tried = true;
	char *enc = url_encode(cfg.city);
	char *url = xasprintf("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=%s&format=json", enc,
	                      i18n_lang() == LANG_TR ? "tr" : "en");
	http_get(url, home_resolve_done, NULL);
	free(url);
	free(enc);
}

/* ----------------------------------------------------------- place actions -- */

static void view_place(int i) {
	g_cur = CLAMP(i, 0, MAX(0, g_npl - 1));
	g_expanded = -1;
	for (int k = 0; k < 3; k++) g_scr[k] = (Scroll){0};
	ui_pager_goto(&g_pivot, 0);
	maybe_refresh(g_cur);
	shell_invalidate();
}

static void set_home(int i) {
	if (i < 0 || i >= g_npl) return;
	str_copy(cfg.city, sizeof cfg.city, g_pl[i].name);
	cfg.lat = g_pl[i].lat;
	cfg.lon = g_pl[i].lon;
	config_save_soon();
	sync_home();
	g_cur = 0;
	ensure_timer();
	save_all();
	shell_toast(_("Weather"), _("Home place changed"), _("Weather"));
}

static void remove_place(int i) {
	if (i < 0 || i >= g_npl || (i == 0 && has_home())) return;
	free(g_pl[i].fc);
	memmove(&g_pl[i], &g_pl[i + 1], (size_t)(g_npl - i - 1) * sizeof(Place));
	g_npl--;
	if (g_cur >= i && g_cur > 0) g_cur--;
	save_all();
	shell_invalidate();
}

static void add_place(const GeoHit *g) {
	int i = find_place(g->lat, g->lon);
	if (i < 0) {
		if (g_npl >= MAXPLACES) { shell_toast(_("Weather"), _("You can save up to 16 places."), _("Weather")); return; }
		i = g_npl++;
		g_pl[i] = (Place){.lat = g->lat, .lon = g->lon};
		str_copy(g_pl[i].name, sizeof g_pl[i].name, g->name);
		str_copy(g_pl[i].region, sizeof g_pl[i].region, g->region);
	}
	if (!has_home()) {
		str_copy(cfg.city, sizeof cfg.city, g_pl[i].name);
		cfg.lat = g_pl[i].lat;
		cfg.lon = g_pl[i].lon;
		config_save_soon();
		sync_home();
		ensure_timer();
		i = 0;
	}
	save_all();
	view_place(i);
	fetch_place(i);
}

static void place_menu_cb(int button, void *ud) {
	int i = (int)(intptr_t)ud;
	if (i < 0 || i >= g_npl || button < 0) return;
	bool home = i == 0 && has_home();
	if (home) { if (button == 0) fetch_place(i); return; }
	if (button == 0) set_home(i);
	else if (button == 1) remove_place(i);
}

static void open_search(UI *ui) {
	g_q[0] = 0;
	g_ngeo = 0;
	g_geo_done = g_geo_err = g_geo_busy = false;
	g_search_scroll = (Scroll){0};
	g_focus_search = true;
	nav_push(&g_nav, ui, PG_SEARCH);
}

/* ------------------------------------------------------------------ drawing -- */

/* white-on-colour variant of the theme for the coloured weather pages */
static Theme white_theme(const Theme *t) {
	Theme w = *t;
	w.fg = COL_WHITE;
	w.sub = col_alpha(COL_WHITE, 0.72f);
	w.dim = col_alpha(COL_WHITE, 0.4f);
	return w;
}

static void info_line(UI *ui, const Place *p, double x, double y, double w) {
	cairo_t *cr = ui->cr;
	char buf[160], ago[64];
	const Forecast *f = p->fc;
	if (f->sample) str_copy(buf, sizeof buf, _("sample data — offline"));
	else {
		i18n_fmt_ago(ago, sizeof ago, f->fetched, time(NULL));
		if (!p->failed && time(NULL) - f->fetched < 60) str_copy(buf, sizeof buf, _("updated just now"));
		else if (p->failed) snprintf(buf, sizeof buf, _("offline · updated %s"), ago);
		else snprintf(buf, sizeof buf, _("updated %s"), ago);
	}
	if (f->sample) {
		Size s = gfx_text_size(cr, buf, 15, FW_SEMIBOLD, w, 1);
		gfx_fill_rect(cr, R(x - 6, y - 2, s.w + 12, s.h + 4), col_alpha(COL_BLACK, 0.35f));
		gfx_text(cr, buf, x, y, 15, FW_SEMIBOLD, COL_WHITE, TA_LEFT, w, 1);
	} else {
		gfx_text(cr, buf, x, y, 15, FW_REGULAR, ui->th.sub, TA_LEFT, w, 1);
	}
}

static void detail_cell(UI *ui, double x, double y, double w, const char *label, const char *value) {
	gfx_text(ui->cr, label, x, y, 15, FW_REGULAR, ui->th.sub, TA_LEFT, w - 8, 1);
	gfx_text(ui->cr, value, x, y + 20, 22, FW_SEMILIGHT, ui->th.fg, TA_LEFT, w - 8, 1);
}

static void page_now(UI *ui, Rect view, const Place *p) {
	cairo_t *cr = ui->cr;
	const Forecast *f = p->fc;
	const double W = view.w, M = 24;
	char b[200], b2[96];
	int ti = today_index(f);
	bool wide = W >= 640;
	static double content = 800;
	ui_scroll_begin(ui, ui_id("wx-now"), &g_scr[0], view, content);
	cairo_translate(cr, view.x, view.y);
	/* big temperature and condition */
	fmt_temp(b, sizeof b, f, f->temp);
	Size ts = gfx_text_size(cr, b, 110, FW_LIGHT, 0, 1);
	gfx_text(cr, b, M - 6, -14, 110, FW_LIGHT, COL_WHITE, TA_LEFT, 0, 1);
	wx_icon(cr, cond_icon(f->code, f->day), M + ts.w + 52, 58, 84, COL_WHITE);
	double y = ts.h - 22;
	y += gfx_text(cr, cond_text(f->code, f->day), M, y, 30, FW_SEMILIGHT, COL_WHITE, TA_LEFT, W - 2 * M, 1);
	if (f->nd) {
		char hi[16], lo[16], fl[16];
		fmt_temp(hi, sizeof hi, f, f->dhi[ti]);
		fmt_temp(lo, sizeof lo, f, f->dlo[ti]);
		fmt_temp(fl, sizeof fl, f, f->feels);
		snprintf(b2, sizeof b2, _("feels like %s"), fl);
		snprintf(b, sizeof b, "%s / %s   ·   %s", hi, lo, b2);
	} else {
		char fl[16];
		fmt_temp(fl, sizeof fl, f, f->feels);
		snprintf(b, sizeof b, _("feels like %s"), fl);
	}
	y += gfx_text(cr, b, M, y + 2, 20, FW_SEMILIGHT, ui->th.sub, TA_LEFT, W - 2 * M, 1) + 6;
	info_line(ui, p, M, y, W - 2 * M);
	y += 40;

	/* next hours strip */
	int h0 = hour_start(f);
	int ncol = wide ? 8 : 5;
	double cw = (W - 2 * M) / ncol;
	if (h0 < f->nh) {
		gfx_text(cr, i18n_upper(_("next hours")), M, y, 15, FW_SEMIBOLD, COL_WHITE, TA_LEFT, W - 2 * M, 1);
		y += 28;
		for (int k = 0; k < ncol && h0 + k < f->nh; k++) {
			int i = h0 + k;
			double cx = M + cw * (k + 0.5);
			if (k == 0) str_copy(b, sizeof b, _("now"));
			else fmt_clock(b, sizeof b, f->ht[i]);
			gfx_text(cr, b, cx, y, 15, FW_REGULAR, ui->th.sub, TA_CENTER, cw, 1);
			wx_icon(cr, cond_icon(f->hcode[i], hour_is_day(f, f->ht[i])), cx, y + 40, 32, COL_WHITE);
			fmt_temp(b, sizeof b, f, k == 0 ? f->temp : f->htemp[i]);
			gfx_text(cr, b, cx, y + 60, 20, FW_SEMILIGHT, COL_WHITE, TA_CENTER, cw, 1);
		}
		y += 100;
	}

	/* details */
	gfx_text(cr, i18n_upper(_("details")), M, y, 15, FW_SEMIBOLD, COL_WHITE, TA_LEFT, W - 2 * M, 1);
	y += 30;
	int cols = wide ? 4 : 2;
	double colw = (W - 2 * M) / cols;
	const char *labels[8];
	char vals[8][48];
	int n = 0;
	labels[n] = _("humidity"); snprintf(vals[n++], 48, "%.0f%%", f->hum);
	labels[n] = _("wind"); fmt_wind(vals[n++], 48, f);
	labels[n] = _("pressure"); fmt_pressure(vals[n++], 48, f);
	labels[n] = _("precipitation"); fmt_precip(vals[n++], 48, f);
	if (f->nd) {
		labels[n] = _("chance of rain"); snprintf(vals[n++], 48, "%d%%", f->dpop[ti]);
		labels[n] = _("uv index"); snprintf(vals[n++], 48, "%.0f (%s)", f->duv[ti], uv_text(f->duv[ti]));
		labels[n] = _("sunrise"); fmt_clock(vals[n++], 48, f->drise[ti]);
		labels[n] = _("sunset"); fmt_clock(vals[n++], 48, f->dset[ti]);
	}
	for (int i = 0; i < n; i++)
		detail_cell(ui, M + (i % cols) * colw, y + (i / cols) * 64, colw, labels[i], vals[i]);
	y += ((n + cols - 1) / cols) * 64 + 10;
	gfx_text(cr, _("Data: Open-Meteo.com"), M, y, 13, FW_REGULAR, ui->th.dim, TA_LEFT, W - 2 * M, 1);
	content = y + 40;
	ui_scroll_end(ui);
}

static void page_hourly(UI *ui, Rect view, const Place *p) {
	cairo_t *cr = ui->cr;
	const Forecast *f = p->fc;
	const double ROW = 58, HDR = 44, M = 24, W = view.w;
	int h0 = hour_start(f), n = MIN(24, f->nh - h0);
	if (n <= 0) { app_empty(ui, view, _("no hourly forecast available")); return; }
	double total = 0;
	for (int k = 0; k < n; k++) {
		if (k > 0 && f->ht[h0 + k] / 86400 != f->ht[h0 + k - 1] / 86400) total += HDR;
		total += ROW;
	}
	double off = ui_scroll_begin(ui, ui_id("wx-hourly"), &g_scr[1], view, total + 24);
	cairo_translate(cr, view.x, view.y);
	double y = 0;
	char b[64];
	for (int k = 0; k < n; k++) {
		int i = h0 + k;
		if (k > 0 && f->ht[i] / 86400 != f->ht[i - 1] / 86400) {
			struct tm tm = utc_tm(f->ht[i]);
			if (y + HDR > off && y < off + view.h)
				gfx_text(cr, i18n_upper(i18n_weekday(tm.tm_wday, false)), M, y + 16, 15, FW_SEMIBOLD, COL_WHITE, TA_LEFT, W - 2 * M, 1);
			y += HDR;
		}
		if (y + ROW > off && y < off + view.h) {
			double cy = y + ROW / 2;
			if (k == 0) str_copy(b, sizeof b, _("now"));
			else fmt_clock(b, sizeof b, f->ht[i]);
			gfx_text(cr, b, M, cy - 14, 22, FW_SEMILIGHT, COL_WHITE, TA_LEFT, 120, 1);
			bool day = hour_is_day(f, f->ht[i]);
			wx_icon(cr, cond_icon(f->hcode[i], day), M + 150, cy, 30, COL_WHITE);
			fmt_temp(b, sizeof b, f, k == 0 ? f->temp : f->htemp[i]);
			gfx_text(cr, b, M + 190, cy - 15, 24, FW_SEMILIGHT, COL_WHITE, TA_LEFT, 70, 1);
			if (W >= 420) gfx_text(cr, cond_text(f->hcode[i], day), M + 260, cy - 10, 15, FW_REGULAR, ui->th.sub, TA_LEFT,
			                       W - M - 80 - (M + 260), 1);
			snprintf(b, sizeof b, "%d%%", f->hpop[i]);
			gfx_text(cr, b, W - M, cy - 11, 18, FW_SEMILIGHT, f->hpop[i] >= 30 ? COL_WHITE : ui->th.sub, TA_RIGHT, 60, 1);
			icon_draw(cr, ICON_RAIN, W - M - 66, cy, 18, ui->th.sub);
		}
		y += ROW;
	}
	ui_scroll_end(ui);
}

static void page_daily(UI *ui, Rect view, const Place *p) {
	cairo_t *cr = ui->cr;
	const Forecast *f = p->fc;
	const double ROW = 76, EXTRA = 84, M = 24, W = view.w;
	int d0 = today_index(f), n = f->nd - d0;
	if (n <= 0) { app_empty(ui, view, _("no daily forecast available")); return; }
	double total = n * ROW + (g_expanded >= 0 && g_expanded < n ? EXTRA : 0) + 24;
	double off = ui_scroll_begin(ui, ui_id("wx-daily"), &g_scr[2], view, total);
	cairo_translate(cr, view.x, view.y);
	double y = 0;
	char b[96], hi[16], lo[16];
	for (int k = 0; k < n; k++) {
		int i = d0 + k;
		bool open = g_expanded == k;
		double h = ROW + (open ? EXTRA : 0);
		if (y + h > off && y < off + view.h) {
			Rect rr = R(0, y, W, h);
			UiId id = ui_idn("wx-day", k);
			ui_hit(ui, id, rr);
			if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(COL_WHITE, 0.12f));
			if (ui_clicked(ui, id)) g_expanded = open ? -1 : k;
			struct tm tm = utc_tm(f->dd[i]);
			const char *wd = k == 0 ? _("today") : i18n_weekday(tm.tm_wday, false);
			double right = W - M;
			fmt_temp(hi, sizeof hi, f, f->dhi[i]);
			fmt_temp(lo, sizeof lo, f, f->dlo[i]);
			gfx_text(cr, lo, right, y + 18, 22, FW_SEMILIGHT, ui->th.sub, TA_RIGHT, 60, 1);
			gfx_text(cr, hi, right - 58, y + 18, 26, FW_SEMILIGHT, COL_WHITE, TA_RIGHT, 60, 1);
			wx_icon(cr, cond_icon(f->dcode[i], true), right - 150, y + ROW / 2, 40, COL_WHITE);
			double tw = right - 185 - M;
			gfx_text(cr, wd, M, y + 10, 24, FW_SEMILIGHT, COL_WHITE, TA_LEFT, tw, 1);
			snprintf(b, sizeof b, "%d %s · %s", tm.tm_mday, i18n_month(tm.tm_mon, true), cond_text(f->dcode[i], true));
			gfx_text(cr, b, M, y + 42, 15, FW_REGULAR, ui->th.sub, TA_LEFT, tw, 1);
			if (open) {
				double ey = y + ROW - 4, cw = (W - 2 * M) / 4;
				char v[4][32];
				snprintf(v[0], 32, "%d%%", f->dpop[i]);
				snprintf(v[1], 32, "%.0f", f->duv[i]);
				fmt_clock(v[2], 32, f->drise[i]);
				fmt_clock(v[3], 32, f->dset[i]);
				const char *lb[4] = {_("rain"), _("uv index"), _("sunrise"), _("sunset")};
				for (int c = 0; c < 4; c++) detail_cell(ui, M + c * cw, ey + 8, cw, lb[c], v[c]);
			}
		}
		y += h;
	}
	ui_scroll_end(ui);
}

static void page_places(UI *ui, Rect view) {
	cairo_t *cr = ui->cr;
	const double ROW = 84, M = 24, W = view.w;
	double total = (g_npl + 1) * ROW + 24;
	double off = ui_scroll_begin(ui, ui_id("wx-places"), &g_scr[3], view, total);
	cairo_translate(cr, view.x, view.y);
	char b[32];
	for (int i = 0; i <= g_npl; i++) {
		double y = i * ROW;
		if (y + ROW < off || y > off + view.h) continue;
		Rect rr = R(0, y, W, ROW);
		UiId id = ui_idn("wx-place", i);
		ui_hit(ui, id, rr);
		bool pr = ui_pressed(ui, id);
		if (pr) gfx_fill_rect(cr, rr, col_alpha(COL_WHITE, 0.12f));
		Rect sq = R(M, y + 8, 68, 68);
		if (i == g_npl) {
			gfx_stroke_rect(cr, sq, 2, col_alpha(COL_WHITE, 0.7f));
			icon_draw(cr, ICON_ADD, rect_cx(sq), rect_cy(sq), 30, COL_WHITE);
			gfx_text(cr, _("add a place"), M + 86, y + 26, 26, FW_SEMILIGHT, COL_WHITE, TA_LEFT, W - M - 110, 1);
			if (ui_clicked(ui, id)) open_search(ui);
			continue;
		}
		Place *p = &g_pl[i];
		gfx_fill_rect(cr, sq, p->fc ? cond_bg(p->fc) : col_alpha(COL_WHITE, 0.15f));
		gfx_stroke_rect(cr, sq, 1, col_alpha(COL_WHITE, 0.35f));
		if (p->fc) {
			wx_icon(cr, cond_icon(p->fc->code, p->fc->day), rect_cx(sq), sq.y + 24, 28, COL_WHITE);
			fmt_temp(b, sizeof b, p->fc, p->fc->temp);
			gfx_text(cr, b, rect_cx(sq), sq.y + 38, 20, FW_SEMILIGHT, COL_WHITE, TA_CENTER, sq.w, 1);
		} else {
			icon_draw(cr, ICON_LOCATION, rect_cx(sq), rect_cy(sq), 28, COL_WHITE);
		}
		double tx = M + 86, tw = W - tx - M;
		bool home = i == 0 && has_home();
		if (home) {
			icon_draw(cr, ICON_HOME, tx + 10, y + 28, 20, COL_WHITE);
			gfx_text(cr, p->name, tx + 28, y + 12, 26, FW_SEMILIGHT, COL_WHITE, TA_LEFT, tw - 28, 1);
		} else {
			gfx_text(cr, p->name, tx, y + 12, 26, FW_SEMILIGHT, COL_WHITE, TA_LEFT, tw, 1);
		}
		char sub[160];
		if (p->fc) snprintf(sub, sizeof sub, "%s%s%s", cond_text(p->fc->code, p->fc->day), p->region[0] ? " · " : "", p->region);
		else str_copy(sub, sizeof sub, p->region);
		gfx_text(cr, sub, tx, y + 46, 15, FW_REGULAR, i == g_cur ? COL_WHITE : ui->th.sub, TA_LEFT, tw, 1);
		if (ui_clicked(ui, id)) view_place(i);
		if (ui_longpressed(ui, id)) {
			static const char *home_items[] = {N_("refresh")};
			static const char *items[] = {N_("set as home"), N_("remove")};
			const char *tr_items[2];
			int nitems = home ? 1 : 2;
			for (int k = 0; k < nitems; k++) tr_items[k] = tr(home ? home_items[k] : items[k]);
			dialog_menu(ui->p.x, ui->p.y, tr_items, nitems, place_menu_cb, (void *)(intptr_t)i);
		}
	}
	ui_scroll_end(ui);
}

static void pivot_page(UI *ui, int page, Rect r, void *ud) {
	const Place *p = ud;
	if (page == 3) { page_places(ui, r); return; }
	if (!p->fc) {
		if (p->fetching) app_empty(ui, r, _("getting the forecast..."));
		else if (p->failed) app_empty(ui, r, _("couldn't get the weather. check your connection and tap refresh."));
		else app_empty(ui, r, _("no forecast yet. tap refresh."));
		return;
	}
	if (page == 0) page_now(ui, r, p);
	else if (page == 1) page_hourly(ui, r, p);
	else page_daily(ui, r, p);
}

static void draw_welcome(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	gfx_fill_rect(cr, area, RGBX(0x1BA1E2));
	Theme saved = ui->th;
	ui->th = white_theme(&saved);
	double y = ui_page_header(ui, 14, area.y + 6, _("Weather"), _("welcome"));
	y += 10;
	y += gfx_text(cr, _("Find your city to see current conditions, an hourly forecast and the next 10 days."), 24, y, 22,
	              FW_SEMILIGHT, COL_WHITE, TA_LEFT, area.w - 48, 0) + 24;
	if (ui_button(ui, ui_id("wx-find"), R(24, y, MIN(260, area.w - 48), 52), _("find a city"), BTN_OUTLINE)) open_search(ui);
	ui->th = saved;
}

static void draw_main(UI *ui, Rect area) {
	if (!g_npl) { draw_welcome(ui, area); return; }
	g_cur = CLAMP(g_cur, 0, g_npl - 1);
	Place *p = &g_pl[g_cur];
	gfx_fill_rect(ui->cr, area, cond_bg(p->fc));
	Theme saved = ui->th;
	ui->th = white_theme(&saved);
	Rect content = appbar_content(&g_bar, area);
	static const char *titles[] = {N_("now"), N_("hourly"), N_("daily"), N_("places")};
	const char *tt[4];
	for (int i = 0; i < 4; i++) tt[i] = tr(titles[i]);
	app_pivot(ui, "wx-pivot", &g_pivot, content, p->name, tt, 4, pivot_page, p);
	if (p->fetching) ui_progress_dots(ui, R(area.x, area.y + 2, area.w, 6), COL_WHITE);
	ui->th = saved;

	bool home = g_cur == 0 && has_home();
	AppBarBtn btns[] = {{ICON_REFRESH, _("refresh"), p->fetching}, {ICON_ADD, _("add place"), false}};
	const char *menu[3];
	int nm = 0, act[3];
	menu[nm] = cfg.celsius ? _("show fahrenheit") : _("show celsius"); act[nm++] = 0;
	if (!home) { menu[nm] = _("set as home"); act[nm++] = 1; menu[nm] = _("remove place"); act[nm++] = 2; }
	int r = appbar_draw(ui, &g_bar, area, btns, 2, menu, nm);
	if (r == 0) fetch_place(g_cur);
	else if (r == 1) open_search(ui);
	else if (r >= APPBAR_MENU && r < APPBAR_MENU + nm) {
		switch (act[r - APPBAR_MENU]) {
		case 0:
			cfg.celsius = !cfg.celsius;
			config_save_soon();
			break;
		case 1: set_home(g_cur); break;
		case 2: remove_place(g_cur); break;
		}
	}
}

static void draw_search(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = ui_page_header(ui, 14, area.y + 6, _("Weather"), _("add a place"));
	UiId tid = ui_id("wx-search-box");
	if (g_focus_search) { ui_focus(ui, tid); g_focus_search = false; }
	bool changed = false;
	bool enter = ui_textbox(ui, tid, R(24, y + 4, area.w - 48, 48), g_q, sizeof g_q, _("city name"), 0, &changed);
	if (changed) {
		if (!g_geo_timer) g_geo_timer = timer_add(450, false, geo_timer_cb, NULL);
		timer_set(g_geo_timer, 450);
		g_search_scroll.off = 0;
	}
	if (enter) {
		if (g_geo_timer) timer_set(g_geo_timer, 0);
		if (g_ngeo > 0 && !g_geo_busy) { GeoHit g = g_geo[0]; ui->focus = 0; nav_pop(&g_nav, ui); add_place(&g); return; }
		geo_search();
	}
	y += 64;
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	if (g_geo_busy) ui_progress_dots(ui, R(24, y - 8, area.w - 48, 6), th->accent);
	if (!g_ngeo) {
		const char *msg = g_geo_err ? _("couldn't search for places. check your connection.")
		                : g_geo_done ? _("no places found")
		                : _("type the name of a city, for example Istanbul or London");
		if (!g_geo_busy) app_empty(ui, view, msg);
		return;
	}
	const double ROW = 76;
	double off = ui_scroll_begin(ui, ui_id("wx-geo-scroll"), &g_search_scroll, view, g_ngeo * ROW + 20);
	cairo_translate(cr, view.x, view.y);
	for (int i = 0; i < g_ngeo; i++) {
		Rect rr = R(0, i * ROW, view.w, ROW);
		if (rr.y + ROW < off || rr.y > off + view.h) continue;
		bool known = find_place(g_geo[i].lat, g_geo[i].lon) >= 0;
		if (ui_row_ex(ui, ui_idn("wx-geo", i), rr, g_geo[i].name, g_geo[i].region, ICON_NONE, known ? _("added") : NULL, false)) {
			GeoHit g = g_geo[i];
			ui->focus = 0;
			nav_pop(&g_nav, ui);
			add_place(&g);
			break;
		}
	}
	ui_scroll_end(ui);
}

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	if (page == PG_SEARCH) draw_search(ui, area);
	else draw_main(ui, area);
}

static void weather_draw(UI *ui, Rect area) {
	ensure_loaded();
	nav_draw(ui, &g_nav, area, page_fn, NULL);
}

/* --------------------------------------------------------------- lifecycle -- */

static void weather_init(void) {
	ensure_timer();
}

static void weather_open(const char *arg) {
	ensure_loaded();
	sync_home();
	resolve_home_name();
	nav_init(&g_nav, PG_MAIN);
	g_bar = (AppBar){0};
	g_pivot = (Pager){0};
	g_cur = 0;
	g_expanded = -1;
	for (int i = 0; i < 4; i++) g_scr[i] = (Scroll){0};
	if (arg && *arg) {
		for (int i = 0; i < g_npl; i++)
			if (str_contains_fold(g_pl[i].name, arg)) { g_cur = i; break; }
	}
	if (g_npl) {
		Place *p = &g_pl[g_cur];
		if (is_stale(p) && !p->fetching) fetch_place(g_cur);
	}
}

static bool weather_back(void) {
	if (appbar_back(&g_bar)) return true;
	if (nav_pop(&g_nav, &sh.ui)) { sh.ui.focus = 0; return true; }
	return false;
}

static void weather_close(void) {
	free_all();
	g_ngeo = 0;
	g_q[0] = 0;
}

/* ----------------------------------------------------------------- live tile -- */

static void tile_face(UI *ui, Rect r, int face, void *ud) {
	cairo_t *cr = ui->cr;
	const Place *p = ud;
	const Forecast *f = p->fc;
	bool wide = r.w > r.h * 1.5;
	char b[64], hi[16], lo[16];
	int ti = today_index(f);
	if (face == 0) {
		fmt_temp(b, sizeof b, f, f->temp);
		if (wide) {
			wx_icon(cr, cond_icon(f->code, f->day), r.x + r.w * 0.2, r.y + r.h * 0.4, r.h * 0.44, COL_WHITE);
			double x = r.x + r.w * 0.42;
			gfx_text(cr, b, x, r.y + 8, r.h * 0.34, FW_LIGHT, COL_WHITE, TA_LEFT, 0, 1);
			gfx_text(cr, cond_text(f->code, f->day), x + 2, r.y + r.h * 0.5, 18, FW_SEMILIGHT, COL_WHITE, TA_LEFT,
			         rect_right(r) - x - 10, 1);
			if (f->nd) {
				fmt_temp(hi, sizeof hi, f, f->dhi[ti]);
				fmt_temp(lo, sizeof lo, f, f->dlo[ti]);
				snprintf(b, sizeof b, "%s / %s", hi, lo);
				gfx_text(cr, b, x + 2, r.y + r.h * 0.5 + 24, 16, FW_REGULAR, col_alpha(COL_WHITE, 0.85f), TA_LEFT, 0, 1);
			}
		} else {
			wx_icon(cr, cond_icon(f->code, f->day), rect_cx(r), r.y + r.h * 0.32, r.h * 0.34, COL_WHITE);
			gfx_text(cr, b, rect_cx(r), r.y + r.h * 0.5, r.h * 0.2, FW_LIGHT, COL_WHITE, TA_CENTER, 0, 1);
		}
	} else {
		int d0 = ti, n = MIN(3, f->nd - d0);
		if (wide) {
			double cw = (r.w - 16) / 3;
			for (int k = 0; k < n; k++) {
				int i = d0 + k;
				double cx = r.x + 8 + cw * (k + 0.5);
				struct tm tm = utc_tm(f->dd[i]);
				gfx_text(cr, k == 0 ? _("today") : i18n_weekday(tm.tm_wday, true), cx, r.y + 10, 16, FW_SEMILIGHT, COL_WHITE,
				         TA_CENTER, cw - 4, 1);
				wx_icon(cr, cond_icon(f->dcode[i], true), cx, r.y + r.h * 0.42, r.h * 0.26, COL_WHITE);
				fmt_temp(hi, sizeof hi, f, f->dhi[i]);
				fmt_temp(lo, sizeof lo, f, f->dlo[i]);
				snprintf(b, sizeof b, "%s / %s", hi, lo);
				gfx_text(cr, b, cx, r.y + r.h * 0.6, 17, FW_SEMILIGHT, COL_WHITE, TA_CENTER, cw - 4, 1);
			}
		} else {
			double rh = (r.h - 40) / 3;
			for (int k = 0; k < n; k++) {
				int i = d0 + k;
				double y = r.y + 8 + k * rh;
				struct tm tm = utc_tm(f->dd[i]);
				gfx_text(cr, k == 0 ? _("today") : i18n_weekday(tm.tm_wday, true), r.x + 10, y + rh / 2 - 10, 15,
				         FW_SEMILIGHT, COL_WHITE, TA_LEFT, r.w * 0.36, 1);
				wx_icon(cr, cond_icon(f->dcode[i], true), r.x + r.w * 0.5, y + rh / 2, rh * 0.6, COL_WHITE);
				fmt_temp(hi, sizeof hi, f, f->dhi[i]);
				fmt_temp(lo, sizeof lo, f, f->dlo[i]);
				snprintf(b, sizeof b, "%s/%s", hi, lo);
				gfx_text(cr, b, rect_right(r) - 10, y + rh / 2 - 10, 15, FW_SEMILIGHT, COL_WHITE, TA_RIGHT, r.w * 0.4, 1);
			}
		}
	}
	app_tile_name(ui, r, p->name);
}

static void weather_tile(UI *ui, Rect r, TileSize size, double now) {
	ensure_loaded();
	resolve_home_name();
	int i = g_npl ? 0 : -1;
	maybe_refresh(i);
	if (i < 0 || !g_pl[i].fc || g_pl[i].fc->sample) {
		icon_draw(ui->cr, ICON_SUN, rect_cx(r), r.y + r.h * 0.46, MIN(MIN(r.w, r.h) * 0.42, 70) * 1.1, COL_WHITE);
		app_tile_name(ui, r, i >= 0 ? g_pl[i].name : tr(app_weather.name));
		return;
	}
	app_tile_faces(ui, r, "weather-tile", 2, 8, tile_face, &g_pl[i]);
}

const AppDef app_weather = {
	.id = "weather", .name = N_("Weather"), .icon = ICON_SUN, .color = 0,
	.keywords = "forecast temperature rain hava durumu sıcaklık",
	.init = weather_init, .open = weather_open, .draw = weather_draw, .back = weather_back, .close = weather_close,
	.tile = weather_tile, .live = true,
};

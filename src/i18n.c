#include "i18n.h"
#include "util.h"

#include <glib.h>

typedef struct { const char *en, *tr; } Entry;

static const Entry TABLE[] = {
#include "i18n_tr.inc"
#include "apps/settings.tr.inc"
#include "apps/store.tr.inc"
#include "apps/photos.tr.inc"
#include "apps/calculator.tr.inc"
#include "apps/clock.tr.inc"
#include "apps/calendar.tr.inc"
#include "apps/weather.tr.inc"
#include "apps/people.tr.inc"
#include "apps/notes.tr.inc"
#include "apps/files.tr.inc"
#include "apps/music.tr.inc"
#include "apps/video.tr.inc"
#include "apps/battery.tr.inc"
#include "apps/storage.tr.inc"
#include "apps/terminal.tr.inc"
#include "apps/minesweeper.tr.inc"
#include "apps/recorder.tr.inc"
};

static Lang g_lang = LANG_EN;

/* open-addressing hash from English text to table index */
#define HSIZE 4096
static int g_hash[HSIZE];
static bool g_hash_ready;

static uint32_t hash_str(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
	return h;
}

static void build_hash(void) {
	for (int i = 0; i < HSIZE; i++) g_hash[i] = -1;
	for (size_t i = 0; i < ARRAY_LEN(TABLE); i++) {
		uint32_t h = hash_str(TABLE[i].en) % HSIZE;
		while (g_hash[h] >= 0) h = (h + 1) % HSIZE;
		g_hash[h] = (int)i;
	}
	g_hash_ready = true;
}

Lang i18n_from_code(const char *code) {
	if (code && (str_starts(code, "tr"))) return LANG_TR;
	return LANG_EN;
}

Lang i18n_detect_env(void) {
	const char *vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
	for (size_t i = 0; i < ARRAY_LEN(vars); i++) {
		const char *v = getenv(vars[i]);
		if (v && *v && !str_eq(v, "C") && !str_eq(v, "POSIX")) return i18n_from_code(v);
	}
	return LANG_EN;
}

void i18n_init(const char *configured) {
	if (!g_hash_ready) build_hash();
	g_lang = (configured && *configured) ? i18n_from_code(configured) : i18n_detect_env();
}

void i18n_set_lang(Lang l) { g_lang = l; }

static int g_region = -1;   /* -1 = same as the display language */
static Lang region(void) { return g_region < 0 ? g_lang : (Lang)g_region; }
void i18n_set_region(const char *code) { g_region = code && *code ? (int)i18n_from_code(code) : -1; }
Lang i18n_lang(void) { return g_lang; }
const char *i18n_code(Lang l) { return l == LANG_TR ? "tr" : "en"; }
const char *i18n_native_name(Lang l) { return l == LANG_TR ? "Türkçe" : "English"; }

const char *tr(const char *en) {
	if (g_lang == LANG_EN || !en) return en;
	if (!g_hash_ready) build_hash();
	uint32_t h = hash_str(en) % HSIZE;
	while (g_hash[h] >= 0) {
		const Entry *e = &TABLE[g_hash[h]];
		if (strcmp(e->en, en) == 0) return e->tr ? e->tr : en;
		h = (h + 1) % HSIZE;
	}
	return en;
}

/* --------------------------------------------------------------- case maps */

static const char *ring_store(const char *s) {
	static char *ring[8];
	static int idx;
	idx = (idx + 1) % 8;
	free(ring[idx]);
	ring[idx] = xstrdup(s);
	return ring[idx];
}

const char *i18n_upper(const char *s) {
	if (!s) return "";
	GString *out = g_string_new(NULL);
	for (const char *p = s; *p;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if (g_lang == LANG_TR && cp == 'i') cp = 0x130;      /* i -> İ */
		else cp = g_unichar_toupper(cp);                      /* ı -> I works as-is */
		char b[5];
		utf8_encode(cp, b);
		g_string_append(out, b);
	}
	const char *r = ring_store(out->str);
	g_string_free(out, TRUE);
	return r;
}

const char *i18n_lower(const char *s) {
	if (!s) return "";
	GString *out = g_string_new(NULL);
	for (const char *p = s; *p;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if (g_lang == LANG_TR && cp == 'I') cp = 0x131;      /* I -> ı */
		else if (cp == 0x130) cp = 'i';                       /* İ -> i */
		else cp = g_unichar_tolower(cp);
		char b[5];
		utf8_encode(cp, b);
		g_string_append(out, b);
	}
	const char *r = ring_store(out->str);
	g_string_free(out, TRUE);
	return r;
}

/* ---------------------------------------------------------- date and time */

static const char *WD_EN[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *WD_TR[] = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
static const char *WDA_EN[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *WDA_TR[] = {"Paz", "Pzt", "Sal", "Çar", "Per", "Cum", "Cmt"};
static const char *MO_EN[] = {"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"};
static const char *MO_TR[] = {"Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran", "Temmuz",
	"Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"};
static const char *MOA_EN[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *MOA_TR[] = {"Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"};

const char *i18n_weekday(int w, bool abbr) {
	w = ((w % 7) + 7) % 7;
	if (g_lang == LANG_TR) return abbr ? WDA_TR[w] : WD_TR[w];
	return abbr ? WDA_EN[w] : WD_EN[w];
}

const char *i18n_month(int m, bool abbr) {
	m = ((m % 12) + 12) % 12;
	if (g_lang == LANG_TR) return abbr ? MOA_TR[m] : MO_TR[m];
	return abbr ? MOA_EN[m] : MO_EN[m];
}

int i18n_first_weekday(void) { return region() == LANG_TR ? 1 : 0; }

void i18n_fmt_time(char *buf, size_t n, const struct tm *tm, bool h24) {
	if (h24) { snprintf(buf, n, "%02d:%02d", tm->tm_hour, tm->tm_min); return; }
	int h = tm->tm_hour % 12;
	if (h == 0) h = 12;
	const char *suffix = tm->tm_hour < 12 ? (region() == LANG_TR ? "ÖÖ" : "AM")
	                                      : (region() == LANG_TR ? "ÖS" : "PM");
	snprintf(buf, n, "%d:%02d %s", h, tm->tm_min, suffix);
}

void i18n_fmt_date_long(char *buf, size_t n, const struct tm *tm) {
	if (region() == LANG_TR)
		snprintf(buf, n, "%d %s %s", tm->tm_mday, i18n_month(tm->tm_mon, false), i18n_weekday(tm->tm_wday, false));
	else
		snprintf(buf, n, "%s, %s %d", i18n_weekday(tm->tm_wday, false), i18n_month(tm->tm_mon, false), tm->tm_mday);
}

void i18n_fmt_date_short(char *buf, size_t n, const struct tm *tm) {
	if (region() == LANG_TR)
		snprintf(buf, n, "%02d.%02d.%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);
	else
		snprintf(buf, n, "%d/%d/%04d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
}

void i18n_fmt_month_year(char *buf, size_t n, const struct tm *tm) {
	snprintf(buf, n, "%s %d", i18n_month(tm->tm_mon, false), tm->tm_year + 1900);
}

void i18n_fmt_ago(char *buf, size_t n, time_t then, time_t now) {
	long d = (long)(now - then);
	if (d < 0) d = 0;
	if (d < 60) snprintf(buf, n, "%s", _("now"));
	else if (d < 3600) snprintf(buf, n, _("%d min ago"), (int)(d / 60));
	else if (d < 86400) snprintf(buf, n, _("%d h ago"), (int)(d / 3600));
	else snprintf(buf, n, _("%d d ago"), (int)(d / 86400));
}

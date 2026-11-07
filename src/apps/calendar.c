/* Calendar: month / week / day / agenda views, event editor and details, reminders and
 * .ics import/export (Windows Phone 8.1 Calendar). */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <dirent.h>
#include <glib.h>
#include <math.h>
#include <strings.h>

#define DATA_FILE "calendar.json"
#define APP_NAME "Calendar"
#define MAX_EVENTS 5000
#define MAX_FILES 24

enum { RP_NONE, RP_DAILY, RP_WEEKLY, RP_MONTHLY, RP_YEARLY, RP_COUNT };
enum { V_DAY, V_WEEK, V_MONTH, V_AGENDA, V_COUNT };
enum { P_ROOT, P_DETAILS, P_EDIT };

typedef struct {
	int id;
	char *title, *location, *notes;
	time_t start, end;          /* all-day: local midnight of the first day / of the day after */
	bool allday;
	int repeat, reminder;       /* reminder: minutes before the start, -1 = none */
	int color;                  /* index into ACCENT_RGB */
} Event;

typedef struct { int y, m, d; } Date;           /* m = 0..11 */
typedef struct { int ev; time_t s, e; } Occ;
typedef VEC(Occ) OccVec;
typedef struct { time_t t0, t1; unsigned gen; OccVec v; } OccSet;
typedef VEC(Event) EventVec;

static const int REM_MIN[] = {-1, 0, 5, 15, 30, 60, 1440};
static const char *const REM_LABEL[] = {N_("none"), N_("at start time"), N_("5 minutes before"), N_("15 minutes before"),
	N_("30 minutes before"), N_("1 hour before"), N_("1 day before")};
static const char *const REPEAT_LABEL[RP_COUNT] = {N_("never"), N_("daily"), N_("weekly"), N_("monthly"), N_("yearly")};
static const char *const VIEW_LABEL[V_COUNT] = {N_("day"), N_("week"), N_("month"), N_("agenda")};

/* ---- data (kept resident once loaded: reminders need it) */
static EventVec g_ev;
static int g_next_id = 1;
static bool g_loaded;
static unsigned g_gen = 1;
static Timer *g_tick;
static time_t g_rem_last;
static OccSet g_set_month, g_set_week, g_set_day, g_set_agenda, g_set_tile;

/* ---- UI state */
typedef struct { double dx, dy, sx, sy; } Swipe;
static Nav g_nav;
static AppBar g_bar, g_det_bar, g_ed_bar;
static int g_view = V_MONTH, g_view_back = -1;
static bool g_view_loaded;
static Date g_sel;
static Swipe g_sw_month, g_sw_week, g_sw_day;
static Scroll g_day_scroll, g_week_scroll, g_agenda_scroll, g_det_scroll, g_ed_scroll;
static time_t g_day_scroll_key, g_week_scroll_key;
static int g_det_id;
static time_t g_det_s;
static struct {
	int id;                     /* -1 = new event */
	char title[128], location[128], notes[2048];
	Date date;
	int sh, sm, eh, em, span;   /* span: extra days the event lasts beyond its first day */
	bool allday;
	int repeat, reminder, color;
	bool focus_title;
} g_ed;
static char *g_files[MAX_FILES];
static int g_nfiles;
static bool g_busy;

/* ------------------------------------------------------------ date utils -- */

static time_t date_time(Date d, int h, int m) {
	struct tm tm = {.tm_year = d.y - 1900, .tm_mon = d.m, .tm_mday = d.d, .tm_hour = h, .tm_min = m, .tm_isdst = -1};
	return mktime(&tm);
}

static Date date_of(time_t t) {
	struct tm tm;
	localtime_r(&t, &tm);
	return (Date){tm.tm_year + 1900, tm.tm_mon, tm.tm_mday};
}

static Date date_add(Date d, int days) {
	struct tm tm = {.tm_year = d.y - 1900, .tm_mon = d.m, .tm_mday = d.d + days, .tm_hour = 12, .tm_isdst = -1};
	mktime(&tm);
	return (Date){tm.tm_year + 1900, tm.tm_mon, tm.tm_mday};
}

static Date date_today(void) { return date_of(time(NULL)); }
static bool date_eq(Date a, Date b) { return a.y == b.y && a.m == b.m && a.d == b.d; }

static struct tm date_tm(Date d) {
	struct tm tm = {.tm_year = d.y - 1900, .tm_mon = d.m, .tm_mday = d.d, .tm_hour = 12, .tm_isdst = -1};
	mktime(&tm);
	return tm;
}

static int date_wday(Date d) { return date_tm(d).tm_wday; }

static int days_in_month(int y, int m) {
	static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	return m == 1 && leap ? 29 : D[m];
}

static Date date_next(Date d) {
	if (++d.d > days_in_month(d.y, d.m)) { d.d = 1; if (++d.m > 11) { d.m = 0; d.y++; } }
	return d;
}

/* "November 7, 2026" / "7 Kasım 2026" */
static void fmt_date_noday(char *buf, size_t n, Date d) {
	if (i18n_lang() == LANG_TR) snprintf(buf, n, "%d %s %d", d.d, i18n_month(d.m, false), d.y);
	else snprintf(buf, n, "%s %d, %d", i18n_month(d.m, false), d.d, d.y);
}

static void fmt_clock(char *buf, size_t n, time_t t) {
	struct tm tm;
	localtime_r(&t, &tm);
	i18n_fmt_time(buf, n, &tm, cfg.h24);
}

static void fmt_hm(char *buf, size_t n, int h, int m) {
	struct tm tm = {.tm_hour = h, .tm_min = m};
	i18n_fmt_time(buf, n, &tm, cfg.h24);
}

/* "today", "tomorrow", "yesterday" or "saturday, november 7" */
static void fmt_day_label(char *buf, size_t n, Date d) {
	Date t = date_today();
	if (date_eq(d, t)) str_copy(buf, n, _("today"));
	else if (date_eq(d, date_add(t, 1))) str_copy(buf, n, _("tomorrow"));
	else if (date_eq(d, date_add(t, -1))) str_copy(buf, n, _("yesterday"));
	else {
		struct tm tm = date_tm(d);
		char b[96];
		i18n_fmt_date_long(b, sizeof b, &tm);
		if (d.y != t.y) { size_t l = strlen(b); snprintf(b + l, sizeof b - l, ", %d", d.y); }
		str_copy(buf, n, i18n_lower(b));
	}
}

/* ------------------------------------------------------------ occurrences */

static Color ev_color(const Event *e) { return RGBX(ACCENT_RGB[CLAMP(e->color, 0, ACCENT_COUNT - 1)]); }
static const char *ev_title(const Event *e) { return e->title && *e->title ? e->title : _("untitled"); }
static int ev_days(const Event *e) { return MAX(1, (int)lround((double)(e->end - e->start) / 86400.0)); }

/* Start of the k-th occurrence, (time_t)-1 when that month/year has no such day. */
static time_t occ_start(const Event *e, long k) {
	if (k == 0 || e->repeat == RP_NONE) return k == 0 ? e->start : (time_t)-1;
	struct tm tm;
	localtime_r(&e->start, &tm);
	int mday = tm.tm_mday;
	switch (e->repeat) {
	case RP_DAILY: tm.tm_mday += (int)k; break;
	case RP_WEEKLY: tm.tm_mday += (int)(7 * k); break;
	case RP_MONTHLY: tm.tm_mon += (int)k; break;
	default: tm.tm_year += (int)k; break;
	}
	tm.tm_isdst = -1;
	time_t t = mktime(&tm);
	if ((e->repeat == RP_MONTHLY || e->repeat == RP_YEARLY) && tm.tm_mday != mday) return (time_t)-1;
	return t;
}

static time_t occ_end(const Event *e, time_t s) {
	if (e->allday) return date_time(date_add(date_of(s), ev_days(e)), 0, 0);
	return s + (e->end - e->start);
}

static bool overlaps(time_t s, time_t e, time_t t0, time_t t1) {
	return s < t1 && (e > t0 || (e <= s && s >= t0));
}

static void collect_event(int i, time_t t0, time_t t1, OccVec *out) {
	const Event *e = &g_ev.items[i];
	if (e->repeat == RP_NONE) {
		if (overlaps(e->start, e->end, t0, t1)) vec_push(out, ((Occ){i, e->start, e->end}));
		return;
	}
	if (e->start >= t1) return;
	static const double PERIOD[RP_COUNT] = {1, 86400, 604800, 2629746, 31556952};
	long dur = (long)(e->end - e->start);
	long k0 = (long)floor((double)(t0 - dur - e->start) / PERIOD[e->repeat]) - 1;
	if (k0 < 0) k0 = 0;
	for (long k = k0; k < k0 + 1200; k++) {
		time_t s = occ_start(e, k);
		if (s == (time_t)-1) continue;
		if (s >= t1) break;
		time_t en = occ_end(e, s);
		if (overlaps(s, en, t0, t1)) vec_push(out, ((Occ){i, s, en}));
	}
}

static int occ_cmp(const void *a, const void *b) {
	const Occ *x = a, *y = b;
	bool ax = g_ev.items[x->ev].allday, ay = g_ev.items[y->ev].allday;
	if (ax != ay) return ax ? -1 : 1;
	if (x->s != y->s) return x->s < y->s ? -1 : 1;
	if (x->e != y->e) return x->e > y->e ? -1 : 1;
	return app_collate(ev_title(&g_ev.items[x->ev]), ev_title(&g_ev.items[y->ev]));
}

/* Occurrences overlapping [t0, t1), sorted; cached until the data or range changes. */
static const OccVec *occ_range(OccSet *c, time_t t0, time_t t1) {
	if (c->gen == g_gen && c->t0 == t0 && c->t1 == t1) return &c->v;
	c->v.len = 0;
	for (int i = 0; i < g_ev.len; i++) collect_event(i, t0, t1, &c->v);
	if (c->v.len > 1) qsort(c->v.items, (size_t)c->v.len, sizeof(Occ), occ_cmp);
	c->gen = g_gen;
	c->t0 = t0;
	c->t1 = t1;
	return &c->v;
}

static void occ_sets_free(void) {
	OccSet *sets[] = {&g_set_month, &g_set_week, &g_set_day, &g_set_agenda, &g_set_tile};
	for (size_t i = 0; i < ARRAY_LEN(sets); i++) { vec_free(&sets[i]->v); sets[i]->gen = 0; }
}

/* "10:00 AM – 11:00 AM", "all day" */
static void fmt_when(char *buf, size_t n, const Event *e, time_t s, time_t en) {
	if (e->allday) {
		int days = ev_days(e);
		if (days <= 1) { str_copy(buf, n, _("all day")); return; }
		snprintf(buf, n, _("all day, %d days"), days);
		return;
	}
	char a[32], b[32];
	fmt_clock(a, sizeof a, s);
	fmt_clock(b, sizeof b, en);
	if (en <= s) str_copy(buf, n, a);
	else snprintf(buf, n, "%s – %s", a, b);
}

/* ----------------------------------------------------------- persistence */

static void ev_free(Event *e) {
	free(e->title);
	free(e->location);
	free(e->notes);
	e->title = e->location = e->notes = NULL;
}

static int ev_find(int id) {
	for (int i = 0; i < g_ev.len; i++) if (g_ev.items[i].id == id) return i;
	return -1;
}

static void save(void) {
	g_gen++;
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "next_id", g_next_id);
	cJSON *arr = cJSON_AddArrayToObject(root, "events");
	for (int i = 0; i < g_ev.len; i++) {
		const Event *e = &g_ev.items[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", e->id);
		cJSON_AddStringToObject(o, "title", e->title ? e->title : "");
		if (e->location && *e->location) cJSON_AddStringToObject(o, "location", e->location);
		if (e->notes && *e->notes) cJSON_AddStringToObject(o, "notes", e->notes);
		if (e->allday) {
			/* all-day events are stored as dates so they survive time zone changes */
			Date d = date_of(e->start);
			char b[40];
			snprintf(b, sizeof b, "%04d%02d%02d", d.y, d.m + 1, d.d);
			cJSON_AddStringToObject(o, "date", b);
			cJSON_AddNumberToObject(o, "days", ev_days(e));
		} else {
			cJSON_AddNumberToObject(o, "start", (double)e->start);
			cJSON_AddNumberToObject(o, "end", (double)e->end);
		}
		if (e->repeat) cJSON_AddNumberToObject(o, "repeat", e->repeat);
		cJSON_AddNumberToObject(o, "reminder", e->reminder);
		cJSON_AddNumberToObject(o, "color", e->color);
		cJSON_AddItemToArray(arr, o);
	}
	app_json_save(DATA_FILE, root);
	cJSON_Delete(root);
	shell_invalidate();
}

static double jnum(const cJSON *o, const char *k, double def) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static char *jdup(const cJSON *o, const char *k) {
	const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
	return s && *s ? xstrdup(s) : NULL;
}

static void ensure_loaded(void) {
	if (g_loaded) return;
	g_loaded = true;
	cJSON *root = app_json_load(DATA_FILE);
	if (!root) return;
	g_next_id = MAX(1, (int)jnum(root, "next_id", 1));
	const cJSON *o;
	cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(root, "events")) {
		if (g_ev.len >= MAX_EVENTS) break;
		Event e = {0};
		e.id = (int)jnum(o, "id", 0);
		if (e.id <= 0 || ev_find(e.id) >= 0) e.id = g_next_id;
		g_next_id = MAX(g_next_id, e.id + 1);
		const char *date = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "date"));
		int y, m, d;
		if (date && sscanf(date, "%4d%2d%2d", &y, &m, &d) == 3) {
			Date dt = {y, CLAMP(m - 1, 0, 11), CLAMP(d, 1, 31)};
			e.allday = true;
			e.start = date_time(dt, 0, 0);
			e.end = date_time(date_add(dt, MAX(1, (int)jnum(o, "days", 1))), 0, 0);
		} else {
			e.start = (time_t)jnum(o, "start", 0);
			e.end = MAX(e.start, (time_t)jnum(o, "end", 0));
			if (!e.start) continue;
		}
		e.title = jdup(o, "title");
		e.location = jdup(o, "location");
		e.notes = jdup(o, "notes");
		e.repeat = CLAMP((int)jnum(o, "repeat", 0), 0, RP_COUNT - 1);
		e.reminder = CLAMP((int)jnum(o, "reminder", -1), -1, 40320);
		e.color = CLAMP((int)jnum(o, "color", 5), 0, ACCENT_COUNT - 1);
		vec_push(&g_ev, e);
	}
	cJSON_Delete(root);
	g_gen++;
}

static void load_view(void) {
	if (g_view_loaded) return;
	g_view_loaded = true;
	cJSON *c = config_section("calendar", false);
	g_view = CLAMP((int)jnum(c, "view", V_MONTH), 0, V_COUNT - 1);
}

static void set_view(int v) {
	g_view = CLAMP(v, 0, V_COUNT - 1);
	g_view_back = -1;
	cJSON *c = config_section("calendar", false);
	cJSON_DeleteItemFromObjectCaseSensitive(c, "view");
	cJSON_AddNumberToObject(c, "view", g_view);
	config_save_soon();
	shell_invalidate();
}

/* -------------------------------------------------------------- reminders */

static void remind(const Event *e, time_t s, time_t en) {
	char when[160], body[400];
	Date d = date_of(s);
	if (date_eq(d, date_today())) fmt_when(when, sizeof when, e, s, en);
	else {
		char day[64], w[64];
		fmt_day_label(day, sizeof day, d);
		fmt_when(w, sizeof w, e, s, en);
		snprintf(when, sizeof when, "%s, %s", day, w);
	}
	if (e->location && *e->location) snprintf(body, sizeof body, "%s • %s", when, e->location);
	else str_copy(body, sizeof body, when);
	shell_notify(APP_NAME, ev_title(e), body, "x-office-calendar-symbolic", "", 1, 0, false);
}

static void tick_cb(void *ud) {
	time_t now = time(NULL);
	if (now <= g_rem_last) { g_rem_last = now; return; }   /* clock set back */
	ensure_loaded();
	int fired = 0;
	OccVec v = {0};
	for (int i = 0; i < g_ev.len && fired < 5; i++) {
		const Event *e = &g_ev.items[i];
		if (e->reminder < 0) continue;
		time_t lead = (time_t)e->reminder * 60;
		/* occurrences whose reminder time falls into (last, now] */
		v.len = 0;
		collect_event(i, g_rem_last + lead + 1, now + lead + 1, &v);
		for (int k = 0; k < v.len && fired < 5; k++) {
			time_t s = v.items[k].s;
			if (s - lead <= g_rem_last || s - lead > now) continue;
			if (v.items[k].e < now && s < now) continue;   /* woke up after it was over */
			remind(e, s, v.items[k].e);
			fired++;
		}
	}
	vec_free(&v);
	if (fired) app_sound(SND_CHIME, false);
	if (date_of(now).d != date_of(g_rem_last).d) shell_invalidate();   /* new day: tile, today marker */
	g_rem_last = now;
}

/* ---------------------------------------------------------- .ics support */

static void ics_unescape(char *s) {
	char *w = s;
	for (char *p = s; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			*w++ = (*p == 'n' || *p == 'N') ? '\n' : *p;
		} else {
			*w++ = *p;
		}
	}
	*w = 0;
}

/* ISO 8601 duration like -PT15M, P1D, PT1H30M -> seconds */
static long ics_duration(const char *v) {
	int sign = 1;
	if (*v == '-') { sign = -1; v++; }
	else if (*v == '+') v++;
	if (*v++ != 'P') return 0;
	long total = 0;
	bool t = false;
	while (*v) {
		if (*v == 'T') { t = true; v++; continue; }
		char *end;
		long n = strtol(v, &end, 10);
		if (end == v) break;
		switch (*end) {
		case 'W': total += n * 604800; break;
		case 'D': total += n * 86400; break;
		case 'H': total += n * 3600; break;
		case 'M': total += t ? n * 60 : n * 2592000; break;
		case 'S': total += n; break;
		default: return sign * total;
		}
		v = end + 1;
	}
	return sign * total;
}

static bool ics_time(const char *params, const char *v, time_t *out, bool *date_only) {
	int y, mo, d, h = 0, mi = 0, s = 0;
	if (sscanf(v, "%4d%2d%2d", &y, &mo, &d) != 3) return false;
	const char *t = strchr(v, 'T');
	*date_only = !t;
	if (t && sscanf(t + 1, "%2d%2d%2d", &h, &mi, &s) < 2) return false;
	if (*date_only) {
		*out = date_time((Date){y, CLAMP(mo - 1, 0, 11), d}, 0, 0);
		return true;
	}
	if (v[strlen(v) - 1] == 'Z') {
		struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s};
		*out = timegm(&tm);
		return true;
	}
	const char *tz = params ? strstr(params, "TZID=") : NULL;
	if (tz) {
		char id[96];
		tz += 5;
		if (*tz == '"') tz++;
		size_t n = strcspn(tz, ";\":");
		str_copy(id, MIN(sizeof id, n + 1), tz);
#if GLIB_CHECK_VERSION(2, 68, 0)
		GTimeZone *z = g_time_zone_new_identifier(id);
#else
		GTimeZone *z = g_time_zone_new(id);
#endif
		if (z) {
			GDateTime *dt = g_date_time_new(z, y, mo, d, h, mi, s);
			g_time_zone_unref(z);
			if (dt) {
				*out = (time_t)g_date_time_to_unix(dt);
				g_date_time_unref(dt);
				return true;
			}
		}
	}
	struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = -1};
	*out = mktime(&tm);
	return true;
}

typedef struct { char *path; EventVec out; int added; } ImportJob;

/* Runs on a worker thread: no access to global state. */
static void ics_parse(const char *txt, EventVec *out) {
	/* unfold continuation lines */
	GString *u = g_string_new(NULL);
	for (const char *p = txt; *p; p++) {
		if ((*p == '\r' || *p == '\n')) {
			const char *q = p;
			if (*q == '\r' && q[1] == '\n') q++;
			if (q[1] == ' ' || q[1] == '\t') { p = q + 1; continue; }
			g_string_append_c(u, '\n');
			p = q;
			continue;
		}
		g_string_append_c(u, *p);
	}
	bool in_ev = false, in_alarm = false, has_start = false, has_end = false;
	long dur = -1;
	Event cur = {0};
	char *save_ptr = NULL;
	for (char *line = strtok_r(u->str, "\n", &save_ptr); line; line = strtok_r(NULL, "\n", &save_ptr)) {
		/* NAME;PARAMS:VALUE (colons inside quoted parameters do not count) */
		char *colon = NULL;
		bool q = false;
		for (char *c = line; *c; c++) {
			if (*c == '"') q = !q;
			else if (*c == ':' && !q) { colon = c; break; }
		}
		if (!colon) continue;
		*colon = 0;
		char *value = colon + 1, *params = strchr(line, ';');
		if (params) *params++ = 0;
		const char *name = line;
		if (!strcasecmp(name, "BEGIN") && !strcasecmp(value, "VEVENT")) {
			in_ev = true;
			has_start = has_end = false;
			dur = -1;
			memset(&cur, 0, sizeof cur);
			cur.reminder = -1;
			cur.color = -1;
			continue;
		}
		if (!in_ev) continue;
		if (!strcasecmp(name, "BEGIN") && !strcasecmp(value, "VALARM")) { in_alarm = true; continue; }
		if (!strcasecmp(name, "END") && !strcasecmp(value, "VALARM")) { in_alarm = false; continue; }
		if (!strcasecmp(name, "END") && !strcasecmp(value, "VEVENT")) {
			in_ev = false;
			if (has_start && out->len < MAX_EVENTS) {
				if (!has_end) {
					if (dur > 0) cur.end = cur.start + dur;
					else cur.end = cur.allday ? date_time(date_add(date_of(cur.start), 1), 0, 0) : cur.start;
				}
				if (cur.allday && cur.end <= cur.start) cur.end = date_time(date_add(date_of(cur.start), 1), 0, 0);
				if (cur.end < cur.start) cur.end = cur.start;
				vec_push(out, cur);
			} else {
				ev_free(&cur);
			}
			memset(&cur, 0, sizeof cur);
			continue;
		}
		if (in_alarm) {
			if (!strcasecmp(name, "TRIGGER") && !(params && strcasestr(params, "VALUE=DATE-TIME"))) {
				long secs = ics_duration(value);
				int mins = secs < 0 ? (int)(-secs / 60) : 0;
				if (cur.reminder < 0 || mins < cur.reminder) cur.reminder = mins;
			}
			continue;
		}
		if (!strcasecmp(name, "SUMMARY")) { ics_unescape(value); free(cur.title); cur.title = xstrdup(str_trim(value)); }
		else if (!strcasecmp(name, "LOCATION")) { ics_unescape(value); free(cur.location); cur.location = xstrdup(str_trim(value)); }
		else if (!strcasecmp(name, "DESCRIPTION")) { ics_unescape(value); free(cur.notes); cur.notes = xstrdup(str_trim(value)); }
		else if (!strcasecmp(name, "DTSTART")) {
			bool d;
			if (ics_time(params, value, &cur.start, &d)) { has_start = true; cur.allday = d; }
		} else if (!strcasecmp(name, "DTEND")) {
			bool d;
			if (ics_time(params, value, &cur.end, &d)) has_end = true;
		} else if (!strcasecmp(name, "DURATION")) {
			dur = ics_duration(value);
		} else if (!strcasecmp(name, "RRULE")) {
			const char *f = strcasestr(value, "FREQ=");
			if (f) {
				f += 5;
				if (!strncasecmp(f, "DAILY", 5)) cur.repeat = RP_DAILY;
				else if (!strncasecmp(f, "WEEKLY", 6)) cur.repeat = RP_WEEKLY;
				else if (!strncasecmp(f, "MONTHLY", 7)) cur.repeat = RP_MONTHLY;
				else if (!strncasecmp(f, "YEARLY", 6)) cur.repeat = RP_YEARLY;
			}
		}
	}
	if (in_ev) ev_free(&cur);
	g_string_free(u, TRUE);
}

static void import_run(void *arg) {
	ImportJob *j = arg;
	size_t len = 0;
	char *txt = file_read_all(j->path, &len);
	if (!txt) return;
	ics_parse(txt, &j->out);
	free(txt);
}

static void import_done(void *arg) {
	ImportJob *j = arg;
	g_busy = false;
	ensure_loaded();
	int added = 0, skipped = 0;
	for (int i = 0; i < j->out.len; i++) {
		Event e = j->out.items[i];
		bool dup = false;
		for (int k = 0; k < g_ev.len && !dup; k++) {
			const Event *x = &g_ev.items[k];
			dup = x->start == e.start && x->allday == e.allday && str_eq(x->title ? x->title : "", e.title ? e.title : "");
		}
		if (dup || g_ev.len >= MAX_EVENTS) { ev_free(&e); skipped++; continue; }
		e.id = g_next_id++;
		if (e.color < 0) e.color = cfg.accent;
		vec_push(&g_ev, e);
		added++;
	}
	vec_free(&j->out);
	char msg[160];
	if (added) {
		save();
		snprintf(msg, sizeof msg, added == 1 ? _("%d event imported") : _("%d events imported"), added);
	} else if (skipped) {
		str_copy(msg, sizeof msg, _("these events are already in your calendar"));
	} else {
		str_copy(msg, sizeof msg, _("no events found in this file"));
	}
	shell_toast(tr(APP_NAME), msg, tr(APP_NAME));
	free(j->path);
	free(j);
	shell_invalidate();
}

typedef struct { char *paths[MAX_FILES]; int n; } ScanJob;

static int str_ptr_cmp(const void *a, const void *b) {
	return strcasecmp(path_basename(*(char *const *)a), path_basename(*(char *const *)b));
}

static void scan_run(void *arg) {
	ScanJob *j = arg;
	static const char *dirs[] = {"Downloads", "Documents", "Desktop"};
	for (size_t i = 0; i < ARRAY_LEN(dirs) && j->n < MAX_FILES; i++) {
		char *dir = path_join(home_dir(), dirs[i]);
		DIR *d = opendir(dir);
		struct dirent *de;
		int seen = 0;
		while (d && (de = readdir(d)) && j->n < MAX_FILES && seen++ < 5000) {
			if (de->d_name[0] == '.' || strlen(de->d_name) < 5) continue;
			if (strcasecmp(de->d_name + strlen(de->d_name) - 4, ".ics")) continue;
			j->paths[j->n++] = path_join(dir, de->d_name);
		}
		if (d) closedir(d);
		free(dir);
	}
	qsort(j->paths, (size_t)j->n, sizeof(char *), str_ptr_cmp);
}

static void file_chosen(int b, void *ud) {
	if (b < 0 || b >= g_nfiles) return;
	ImportJob *j = xcalloc(1, sizeof *j);
	j->path = xstrdup(g_files[b]);
	g_busy = true;
	job_submit(import_run, import_done, j);
}

static void scan_done(void *arg) {
	ScanJob *j = arg;
	g_busy = false;
	for (int i = 0; i < g_nfiles; i++) free(g_files[i]);
	g_nfiles = 0;
	const char *names[MAX_FILES];
	for (int i = 0; i < j->n; i++) {
		g_files[g_nfiles] = j->paths[i];
		names[g_nfiles++] = path_basename(j->paths[i]);
	}
	free(j);
	if (!g_nfiles) {
		dialog_message(_("no calendar files"), _("Save .ics files in your Downloads or Documents folder to import them."), NULL, NULL, NULL, NULL);
		return;
	}
	dialog_list(_("import from file"), names, g_nfiles, -1, file_chosen, NULL);
}

static void start_import(void) {
	if (g_busy) return;
	g_busy = true;
	job_submit(scan_run, scan_done, xcalloc(1, sizeof(ScanJob)));
}

static void ics_text(GString *s, const char *name, const char *v) {
	GString *line = g_string_new(name);
	g_string_append_c(line, ':');
	for (const char *p = v; *p; p++) {
		if (*p == '\n') g_string_append(line, "\\n");
		else if (*p == '\r') continue;
		else if (*p == ',' || *p == ';' || *p == '\\') { g_string_append_c(line, '\\'); g_string_append_c(line, *p); }
		else g_string_append_c(line, *p);
	}
	/* fold at 74 octets without splitting UTF-8 sequences */
	size_t col = 0;
	for (size_t i = 0; i < line->len; i++) {
		unsigned char c = (unsigned char)line->str[i];
		if (col >= 74 && (c & 0xC0) != 0x80) { g_string_append(s, "\r\n "); col = 1; }
		g_string_append_c(s, (char)c);
		col++;
	}
	g_string_append(s, "\r\n");
	g_string_free(line, TRUE);
}

static void ics_time_out(GString *s, const char *name, time_t t, bool date_only) {
	struct tm tm;
	if (date_only) {
		localtime_r(&t, &tm);
		g_string_append_printf(s, "%s;VALUE=DATE:%04d%02d%02d\r\n", name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	} else {
		gmtime_r(&t, &tm);
		g_string_append_printf(s, "%s:%04d%02d%02dT%02d%02d%02dZ\r\n", name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		                       tm.tm_hour, tm.tm_min, tm.tm_sec);
	}
}

typedef struct { char *path, *data; size_t len; bool ok; int n; } ExportJob;

static void export_run(void *arg) {
	ExportJob *j = arg;
	char *dir = path_join(home_dir(), "Documents");
	mkdir_p(dir);
	free(dir);
	j->ok = file_write_all(j->path, j->data, j->len);
}

static void export_done(void *arg) {
	ExportJob *j = arg;
	char msg[200];
	if (j->ok) snprintf(msg, sizeof msg, j->n == 1 ? _("%d event saved to Documents/calendar.ics") : _("%d events saved to Documents/calendar.ics"), j->n);
	else str_copy(msg, sizeof msg, _("couldn't save the calendar file"));
	shell_toast(tr(APP_NAME), msg, tr(APP_NAME));
	free(j->path);
	free(j->data);
	free(j);
}

static void start_export(void) {
	ensure_loaded();
	static const char *FREQ[RP_COUNT] = {"", "DAILY", "WEEKLY", "MONTHLY", "YEARLY"};
	GString *s = g_string_new("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//metrowl//Calendar//EN\r\nCALSCALE:GREGORIAN\r\n");
	time_t now = time(NULL);
	for (int i = 0; i < g_ev.len; i++) {
		const Event *e = &g_ev.items[i];
		g_string_append(s, "BEGIN:VEVENT\r\n");
		g_string_append_printf(s, "UID:%d-%lld@metrowl\r\n", e->id, (long long)e->start);
		ics_time_out(s, "DTSTAMP", now, false);
		ics_time_out(s, "DTSTART", e->start, e->allday);
		ics_time_out(s, "DTEND", e->end, e->allday);
		ics_text(s, "SUMMARY", e->title ? e->title : "");
		if (e->location && *e->location) ics_text(s, "LOCATION", e->location);
		if (e->notes && *e->notes) ics_text(s, "DESCRIPTION", e->notes);
		if (e->repeat) g_string_append_printf(s, "RRULE:FREQ=%s\r\n", FREQ[e->repeat]);
		if (e->reminder >= 0) {
			g_string_append(s, "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\n");
			if (e->reminder == 0) g_string_append(s, "TRIGGER:PT0S\r\n");
			else g_string_append_printf(s, "TRIGGER:-PT%dM\r\n", e->reminder);
			g_string_append(s, "END:VALARM\r\n");
		}
		g_string_append(s, "END:VEVENT\r\n");
	}
	g_string_append(s, "END:VCALENDAR\r\n");
	ExportJob *j = xcalloc(1, sizeof *j);
	j->n = g_ev.len;
	j->len = s->len;
	j->data = g_string_free(s, FALSE);
	char *dir = path_join(home_dir(), "Documents");
	j->path = path_join(dir, "calendar.ics");
	free(dir);
	job_submit(export_run, export_done, j);
}

/* ------------------------------------------------------- shared drawing -- */

/* Follows the finger, then decides on release. Returns +1 (next) / -1 (previous) / 0. */
static int swipe_update(UI *ui, UiId id, Swipe *s, bool horiz, bool vert, double w, double h, double *ox, double *oy) {
	double ddx, ddy, vx, vy;
	int r = 0;
	if (ui_drag_delta(ui, id, &ddx, &ddy)) { s->dx += ddx; s->dy += ddy; }
	bool hx = fabs(s->dx) >= fabs(s->dy);
	if (ui_fling(ui, id, &vx, &vy)) {
		if (horiz && hx && (fabs(s->dx) > 70 || (fabs(vx) > 600 && fabs(s->dx) > 20))) r = s->dx < 0 ? 1 : -1;
		else if (vert && !hx && (fabs(s->dy) > 70 || (fabs(vy) > 600 && fabs(s->dy) > 20))) r = s->dy < 0 ? 1 : -1;
		if (r && hx) { s->sx = r * w * 0.35; s->sy = 0; }
		else if (r) { s->sy = r * h * 0.35; s->sx = 0; }
		else { s->sx = horiz && hx ? s->dx * 0.5 : 0; s->sy = vert && !hx ? s->dy * 0.5 : 0; }
		s->dx = s->dy = 0;
		if (!cfg.animations) s->sx = s->sy = 0;
	}
	if (ui_dragging(ui, id)) {
		*ox = horiz && hx ? s->dx * 0.5 : 0;
		*oy = vert && !hx ? s->dy * 0.5 : 0;
	} else {
		s->dx = s->dy = 0;
		s->sx = ui_smooth(ui, s->sx, 0, 16);
		s->sy = ui_smooth(ui, s->sy, 0, 16);
		*ox = s->sx;
		*oy = s->sy;
	}
	return r;
}

static void open_details(int id, time_t s);
static void open_editor(int id, Date d, int hour);

/* Coloured event block (day / week timelines). */
static void event_block(UI *ui, UiId id, Rect r, const Event *e, time_t s, time_t en, bool compact) {
	cairo_t *cr = ui->cr;
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	Color c = ev_color(e);
	gfx_fill_rect(cr, r, pr ? col_mix(c, ui->th.fg, 0.3f) : c);
	Color fg = col_contrast(c);
	ui_clip_push(ui, r);
	double fs = compact ? 11 : 15, pad = compact ? 3 : 8;
	double y = r.y + (compact ? 2 : 4);
	y += gfx_text(cr, ev_title(e), r.x + pad, y, fs, compact ? FW_REGULAR : FW_SEMIBOLD, fg, TA_LEFT, r.w - 2 * pad, compact ? 3 : 2);
	if (!compact && r.h > 44) {
		char when[64];
		fmt_when(when, sizeof when, e, s, en);
		y += gfx_text(cr, when, r.x + pad, y, 13, FW_REGULAR, col_alpha(fg, 0.85f), TA_LEFT, r.w - 2 * pad, 1);
		if (e->location && *e->location && r.h > 64)
			gfx_text(cr, e->location, r.x + pad, y, 13, FW_REGULAR, col_alpha(fg, 0.85f), TA_LEFT, r.w - 2 * pad, 1);
	}
	ui_clip_pop(ui);
	if (ui_clicked(ui, id)) open_details(e->id, s);
}

typedef struct { int occ, col, cluster; time_t s, e; } Lay;

/* Side-by-side columns for overlapping timed events. Returns the count laid out. */
static int layout_day(const OccVec *ov, time_t ds, time_t de, Lay *lay, int max, int *ncols) {
	int n = 0;
	for (int i = 0; i < ov->len && n < max; i++) {
		const Occ *o = &ov->items[i];
		if (g_ev.items[o->ev].allday) continue;
		if (o->s <= ds && o->e >= de) continue;        /* spans the whole day: shown with all-day items */
		if (!overlaps(o->s, o->e, ds, de)) continue;
		time_t s = MAX(o->s, ds), e = MIN(MAX(o->e, o->s + 1800), de);
		lay[n++] = (Lay){i, 0, 0, s, e};
	}
	time_t col_end[16];
	int cols = 0, cluster = 0;
	time_t cluster_end = 0;
	for (int i = 0; i < n; i++) {
		if (lay[i].s >= cluster_end && i > 0) { cluster++; cols = 0; }
		int c = 0;
		while (c < cols && col_end[c] > lay[i].s) c++;
		if (c >= 16) c = 15;
		if (c == cols && cols < 16) cols++;
		col_end[c] = lay[i].e;
		lay[i].col = c;
		lay[i].cluster = cluster;
		ncols[cluster] = MAX(ncols[cluster], cols);
		cluster_end = MAX(cluster_end, lay[i].e);
	}
	return n;
}

static void hour_label(char *buf, size_t n, int h) {
	if (cfg.h24) { snprintf(buf, n, "%02d:00", h); return; }
	int hh = h % 12 ? h % 12 : 12;
	snprintf(buf, n, "%d %s", hh, h < 12 ? (i18n_lang() == LANG_TR ? "ÖÖ" : "AM") : (i18n_lang() == LANG_TR ? "ÖS" : "PM"));
}

/* ------------------------------------------------------------ month view -- */

static void draw_month(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	struct tm mt = {.tm_year = g_sel.y - 1900, .tm_mon = g_sel.m, .tm_mday = 1};
	char title[64];
	i18n_fmt_month_year(title, sizeof title, &mt);
	double y = ui_page_header(ui, 14, area.y + 6, tr(APP_NAME), title);
	int first = i18n_first_weekday();
	Date d1 = {g_sel.y, g_sel.m, 1};
	int lead = (date_wday(d1) - first + 7) % 7;
	Date start = date_add(d1, -lead);
	double gap = 4;
	Rect grid = R(area.x + 12, y + 26, area.w - 24, rect_bottom(area) - y - 34);
	double cw = (grid.w - 6 * gap) / 7, ch = MIN((grid.h - 5 * gap) / 6, cw * 1.3);
	for (int c = 0; c < 7; c++)
		gfx_text(cr, i18n_lower(i18n_weekday((first + c) % 7, true)), grid.x + c * (cw + gap) + 4, y + 2, 15, FW_REGULAR,
		         th->sub, TA_LEFT, cw, 1);
	time_t t0 = date_time(start, 0, 0), t1 = date_time(date_add(start, 42), 0, 0);
	const OccVec *ov = occ_range(&g_set_month, t0, t1);
	Rect gr = R(grid.x, grid.y, grid.w, 6 * ch + 5 * gap);
	UiId gid = ui_id("cal-month");
	ui_scroller_push(ui, gid, gr, UI_AXIS_X | UI_AXIS_Y);
	ui_hit(ui, gid, gr);
	double ox, oy;
	int sw = swipe_update(ui, gid, &g_sw_month, true, true, gr.w, gr.h, &ox, &oy);
	ui_clip_push(ui, R(area.x, gr.y, area.w, rect_bottom(area) - gr.y));
	cairo_save(cr);
	cairo_translate(cr, ox, oy);
	Date today = date_today(), d = start;
	int clicked = -1;
	Date clicked_d = d;
	double lh = gfx_line_height(cr, 12, FW_REGULAR);
	for (int k = 0; k < 42; k++, d = date_next(d)) {
		Rect cell = R(grid.x + (k % 7) * (cw + gap), grid.y + (k / 7) * (ch + gap), cw, ch);
		bool in = d.m == g_sel.m, is_today = date_eq(d, today), sel = date_eq(d, g_sel);
		UiId id = ui_idn("cal-cell", k);
		ui_hit(ui, id, cell);
		bool pr = ui_pressed(ui, id);
		Color fill = is_today ? th->accent : in ? th->chrome : COL_CLEAR;
		if (pr) fill = col_mix(fill.a > 0 ? fill : th->bg, th->fg, 0.25f);
		if (fill.a > 0) gfx_fill_rect(cr, cell, fill);
		if (sel && !is_today && g_view_back < 0) gfx_stroke_rect(cr, cell, 2, th->fg);
		Color fg = is_today ? th->on_accent : in ? th->fg : th->dim;
		char num[8];
		snprintf(num, sizeof num, "%d", d.d);
		gfx_text(cr, num, cell.x + 5, cell.y + 2, cw > 54 ? 20 : 16, FW_SEMILIGHT, fg, TA_LEFT, cw - 6, 1);
		/* events of the day: short titles when there is room, else dots */
		time_t ds = date_time(d, 0, 0), de = date_time(date_next(d), 0, 0);
		int cnt = 0;
		for (int i = 0; i < ov->len; i++) if (overlaps(ov->items[i].s, ov->items[i].e, ds, de)) cnt++;
		if (cnt) {
			double ty = cell.y + (cw > 54 ? 26 : 21);
			int lines = (int)((rect_bottom(cell) - ty - 2) / lh);
			if (cw >= 50 && lines >= 1) {
				int shown = 0;
				for (int i = 0; i < ov->len && shown < lines; i++) {
					const Occ *o = &ov->items[i];
					if (!overlaps(o->s, o->e, ds, de)) continue;
					if (shown == lines - 1 && cnt > lines) {
						char more[16];
						snprintf(more, sizeof more, "+%d", cnt - shown);
						gfx_text(cr, more, cell.x + 5, ty, 12, FW_SEMIBOLD, is_today ? fg : th->sub, TA_LEFT, cw - 8, 1);
						shown++;
						break;
					}
					const Event *e = &g_ev.items[o->ev];
					gfx_fill_rect(cr, R(cell.x + 2, ty + 2, 3, lh - 3), is_today ? fg : ev_color(e));
					gfx_text(cr, ev_title(e), cell.x + 8, ty, 12, FW_REGULAR, fg, TA_LEFT, cw - 10, 1);
					ty += lh;
					shown++;
				}
			} else {
				for (int i = 0, n = 0; i < ov->len && n < 3; i++) {
					const Occ *o = &ov->items[i];
					if (!overlaps(o->s, o->e, ds, de)) continue;
					gfx_fill_rect(cr, R(cell.x + 5 + n * 9, rect_bottom(cell) - 10, 6, 6), is_today ? fg : ev_color(&g_ev.items[o->ev]));
					n++;
				}
			}
		}
		if (ui_clicked(ui, id)) { clicked = k; clicked_d = d; }
	}
	cairo_restore(cr);
	ui_clip_pop(ui);
	ui_scroller_pop(ui);
	if (sw) {
		int m = g_sel.m + sw, yy = g_sel.y;
		if (m < 0) { m = 11; yy--; }
		if (m > 11) { m = 0; yy++; }
		g_sel = (Date){yy, m, MIN(g_sel.d, days_in_month(yy, m))};
		shell_invalidate();
	}
	if (clicked >= 0) {
		g_sel = clicked_d;
		g_view = V_DAY;
		g_view_back = V_MONTH;
		shell_invalidate();
	}
}

/* -------------------------------------------------------------- day view -- */

static void draw_day(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	char small[64];
	fmt_date_noday(small, sizeof small, g_sel);
	struct tm stm = date_tm(g_sel);
	Date today = date_today();
	bool is_today = date_eq(g_sel, today);
	double y = area.y + 6;
	y += gfx_text(cr, i18n_upper(small), 18, y, 17, FW_SEMIBOLD, is_today ? th->accent : th->fg, TA_LEFT, area.w - 36, 1) - 6;
	y += gfx_text(cr, i18n_lower(i18n_weekday(stm.tm_wday, false)), 14, y, 56, FW_LIGHT, th->fg, TA_LEFT, area.w - 28, 1) + 4;
	time_t ds = date_time(g_sel, 0, 0), de = date_time(date_next(g_sel), 0, 0);
	const OccVec *ov = occ_range(&g_set_day, ds, de);
	Rect body = R(area.x, y, area.w, rect_bottom(area) - y);
	UiId sid = ui_id("cal-day-swipe");
	ui_scroller_push(ui, sid, body, UI_AXIS_X);
	ui_hit(ui, sid, body);
	double ox, oy;
	int sw = swipe_update(ui, sid, &g_sw_day, true, false, body.w, body.h, &ox, &oy);
	ui_clip_push(ui, body);
	cairo_save(cr);
	cairo_translate(cr, ox, 0);
	/* all-day strip */
	int nall = 0;
	for (int i = 0; i < ov->len; i++) {
		const Occ *o = &ov->items[i];
		const Event *e = &g_ev.items[o->ev];
		if (!e->allday && !(o->s <= ds && o->e >= de)) continue;
		if (nall >= 3) { nall++; continue; }
		Rect r = R(64, y + nall * 36, area.w - 64 - 16, 32);
		event_block(ui, ui_idn("cal-day-all", i), r, e, o->s, o->e, false);
		nall++;
	}
	if (nall) {
		gfx_text(cr, _("all day"), 12, y + 7, 13, FW_REGULAR, th->sub, TA_LEFT, 50, 2);
		if (nall > 3) {
			char more[32];
			snprintf(more, sizeof more, _("+%d more"), nall - 3);
			gfx_text(cr, more, area.w - 16, y + 3 * 36, 13, FW_REGULAR, th->sub, TA_RIGHT, 120, 1);
			y += 18;
		}
		y += MIN(nall, 3) * 36 + 6;
	}
	const double HOUR = 60, GUT = 64;
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	if (g_day_scroll_key != ds) {
		g_day_scroll_key = ds;
		time_t now = time(NULL);
		struct tm nt;
		localtime_r(&now, &nt);
		double h = is_today ? MAX(0, nt.tm_hour - 1) : 7;
		/* first event of the day if it is earlier */
		for (int i = 0; i < ov->len; i++) {
			const Event *e = &g_ev.items[ov->items[i].ev];
			if (e->allday || ov->items[i].s < ds) continue;
			h = MIN(h, (double)(ov->items[i].s - ds) / 3600.0);
			break;
		}
		g_day_scroll = (Scroll){.off = MIN(h * HOUR, 24 * HOUR + 16 - view.h)};
		if (g_day_scroll.off < 0) g_day_scroll.off = 0;
	}
	double off = ui_scroll_begin(ui, ui_id("cal-day-scroll"), &g_day_scroll, view, 24 * HOUR + 16);
	cairo_translate(cr, view.x, view.y);
	for (int h = 0; h < 24; h++) {
		double hy = h * HOUR;
		if (hy + HOUR < off || hy > off + view.h) continue;
		gfx_fill_rect(cr, R(GUT, hy, view.w - GUT - 12, 1), th->chrome2);
		char lb[16];
		hour_label(lb, sizeof lb, h);
		gfx_text(cr, lb, GUT - 8, hy + 2, 13, FW_REGULAR, th->sub, TA_RIGHT, GUT - 10, 1);
		UiId hid = ui_idn("cal-day-hour", h);
		ui_hit(ui, hid, R(GUT, hy, view.w - GUT, HOUR));
		if (ui_pressed(ui, hid)) gfx_fill_rect(cr, R(GUT, hy + 1, view.w - GUT - 12, HOUR - 1), col_alpha(th->fg, 0.08f));
		if (ui_clicked(ui, hid)) open_editor(-1, g_sel, h);
	}
	Lay lay[64];
	int ncols[64] = {0};
	int n = layout_day(ov, ds, de, lay, 64, ncols);
	double colw_all = view.w - GUT - 16;
	for (int i = 0; i < n; i++) {
		const Occ *o = &ov->items[lay[i].occ];
		double cw = colw_all / MAX(1, ncols[lay[i].cluster]);
		double y0 = (double)(lay[i].s - ds) / 3600.0 * HOUR, y1 = (double)(lay[i].e - ds) / 3600.0 * HOUR;
		Rect r = R(GUT + 2 + lay[i].col * cw, y0 + 1, cw - 3, MAX(24, y1 - y0 - 2));
		if (rect_bottom(r) < off || r.y > off + view.h) continue;
		event_block(ui, ui_idn("cal-day-ev", lay[i].occ), r, &g_ev.items[o->ev], o->s, o->e, cw < 90);
	}
	if (is_today) {
		time_t now = time(NULL);
		double ny = (double)(now - ds) / 3600.0 * HOUR;
		gfx_fill_rect(cr, R(GUT - 4, ny - 1, view.w - GUT - 8, 2), th->accent);
		gfx_fill_rect(cr, R(GUT - 8, ny - 5, 6, 10), th->accent);
	}
	ui_scroll_end(ui);
	cairo_restore(cr);
	ui_clip_pop(ui);
	ui_scroller_pop(ui);
	if (sw) { g_sel = date_add(g_sel, sw); shell_invalidate(); }
}

/* ------------------------------------------------------------- week view -- */

static Date week_start(Date d) { return date_add(d, -((date_wday(d) - i18n_first_weekday() + 7) % 7)); }

static void draw_week(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Date ws = week_start(g_sel), we = date_add(ws, 6);
	char title[96];
	if (ws.m == we.m) {
		if (i18n_lang() == LANG_TR) snprintf(title, sizeof title, "%d – %d %s", ws.d, we.d, i18n_month(ws.m, false));
		else snprintf(title, sizeof title, "%s %d – %d", i18n_month(ws.m, false), ws.d, we.d);
	} else {
		if (i18n_lang() == LANG_TR) snprintf(title, sizeof title, "%d %s – %d %s", ws.d, i18n_month(ws.m, true), we.d, i18n_month(we.m, true));
		else snprintf(title, sizeof title, "%s %d – %s %d", i18n_month(ws.m, true), ws.d, i18n_month(we.m, true), we.d);
	}
	char app[64];
	snprintf(app, sizeof app, "%s %d", tr(APP_NAME), ws.y);
	double y = area.y + 6;
	y += gfx_text(cr, i18n_upper(app), 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, area.w - 36, 1) - 6;
	y += gfx_text(cr, i18n_lower(title), 14, y, 44, FW_LIGHT, th->fg, TA_LEFT, area.w - 28, 1) + 6;
	time_t t0 = date_time(ws, 0, 0), t1 = date_time(date_add(ws, 7), 0, 0);
	const OccVec *ov = occ_range(&g_set_week, t0, t1);
	Rect body = R(area.x, y, area.w, rect_bottom(area) - y);
	UiId sid = ui_id("cal-week-swipe");
	ui_scroller_push(ui, sid, body, UI_AXIS_X);
	ui_hit(ui, sid, body);
	double ox, oy;
	int sw = swipe_update(ui, sid, &g_sw_week, true, false, body.w, body.h, &ox, &oy);
	ui_clip_push(ui, body);
	cairo_save(cr);
	cairo_translate(cr, ox, 0);
	const double GUT = 44, HOUR = 48;
	double colw = (area.w - GUT - 8) / 7;
	Date today = date_today(), d = ws;
	time_t dstart[8];
	for (int c = 0; c < 7; c++, d = date_next(d)) {
		dstart[c] = date_time(d, 0, 0);
		Rect hr = R(GUT + c * colw, y, colw, 50);
		UiId hid = ui_idn("cal-week-head", c);
		ui_hit(ui, hid, hr);
		bool td = date_eq(d, today);
		if (td) gfx_fill_rect(cr, rect_inset(hr, 1), th->accent);
		else if (ui_pressed(ui, hid)) gfx_fill_rect(cr, rect_inset(hr, 1), th->chrome);
		Color fg = td ? th->on_accent : th->fg;
		gfx_text(cr, i18n_lower(i18n_weekday(date_wday(d), true)), hr.x + 4, hr.y + 3, 12, FW_REGULAR, td ? fg : th->sub, TA_LEFT, colw - 6, 1);
		char num[8];
		snprintf(num, sizeof num, "%d", d.d);
		gfx_text(cr, num, hr.x + 4, hr.y + 18, 22, FW_SEMILIGHT, fg, TA_LEFT, colw - 6, 1);
		if (ui_clicked(ui, hid)) { g_sel = d; g_view = V_DAY; g_view_back = V_WEEK; shell_invalidate(); }
	}
	dstart[7] = t1;
	y += 54;
	/* all-day row: one thin bar per event (max two), "+n" when more */
	int maxall = 0;
	for (int c = 0; c < 7; c++) {
		int k = 0;
		for (int i = 0; i < ov->len; i++) {
			const Occ *o = &ov->items[i];
			const Event *e = &g_ev.items[o->ev];
			if (!(e->allday || (o->s <= dstart[c] && o->e >= dstart[c + 1])) || !overlaps(o->s, o->e, dstart[c], dstart[c + 1])) continue;
			if (k < 2) {
				Rect r = R(GUT + c * colw + 1, y + k * 20, colw - 2, 18);
				event_block(ui, ui_idn("cal-week-all", i * 8 + c), r, e, o->s, o->e, true);
			} else if (k == 2) {
				gfx_text(cr, "+", GUT + c * colw + colw - 10, y + 22, 13, FW_SEMIBOLD, th->sub, TA_LEFT, 10, 1);
			}
			k++;
		}
		maxall = MAX(maxall, MIN(k, 2));
	}
	y += maxall * 20 + (maxall ? 4 : 0);
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	if (g_week_scroll_key != t0) {
		g_week_scroll_key = t0;
		g_week_scroll = (Scroll){.off = MAX(0, MIN(7 * HOUR, 24 * HOUR + 16 - view.h))};
	}
	double off = ui_scroll_begin(ui, ui_id("cal-week-scroll"), &g_week_scroll, view, 24 * HOUR + 16);
	cairo_translate(cr, view.x, view.y);
	for (int h = 0; h < 24; h++) {
		double hy = h * HOUR;
		if (hy + HOUR < off || hy > off + view.h) continue;
		gfx_fill_rect(cr, R(GUT, hy, view.w - GUT - 8, 1), th->chrome2);
		char lb[16];
		if (cfg.h24) snprintf(lb, sizeof lb, "%02d", h);
		else snprintf(lb, sizeof lb, "%d%s", h % 12 ? h % 12 : 12, h < 12 ? "a" : "p");
		gfx_text(cr, lb, GUT - 6, hy + 2, 12, FW_REGULAR, th->sub, TA_RIGHT, GUT - 8, 1);
	}
	for (int c = 1; c < 7; c++) gfx_fill_rect(cr, R(GUT + c * colw, off, 1, view.h), col_alpha(th->chrome2, 0.6f));
	for (int c = 0; c < 7; c++) {
		Lay lay[32];
		int ncols[32] = {0};
		int n = layout_day(ov, dstart[c], dstart[c + 1], lay, 32, ncols);
		for (int i = 0; i < n; i++) {
			const Occ *o = &ov->items[lay[i].occ];
			double cw = (colw - 2) / MAX(1, ncols[lay[i].cluster]);
			double y0 = (double)(lay[i].s - dstart[c]) / 3600.0 * HOUR, y1 = (double)(lay[i].e - dstart[c]) / 3600.0 * HOUR;
			Rect r = R(GUT + c * colw + 1 + lay[i].col * cw, y0 + 1, cw - 1, MAX(18, y1 - y0 - 2));
			if (rect_bottom(r) < off || r.y > off + view.h) continue;
			event_block(ui, ui_idn("cal-week-ev", lay[i].occ * 8 + c), r, &g_ev.items[o->ev], o->s, o->e, true);
		}
		if (date_eq(date_add(ws, c), today)) {
			time_t now = time(NULL);
			double ny = (double)(now - dstart[c]) / 3600.0 * HOUR;
			gfx_fill_rect(cr, R(GUT + c * colw, ny - 1, colw, 2), th->accent);
		}
	}
	ui_scroll_end(ui);
	cairo_restore(cr);
	ui_clip_pop(ui);
	ui_scroller_pop(ui);
	if (sw) { g_sel = date_add(g_sel, 7 * sw); shell_invalidate(); }
}

/* ----------------------------------------------------------- agenda view -- */

static void draw_agenda(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = ui_page_header(ui, 14, area.y + 6, tr(APP_NAME), _("agenda"));
	Date today = date_today();
	time_t t0 = date_time(today, 0, 0), t1 = date_time(date_add(today, 120), 0, 0);
	const OccVec *ov = occ_range(&g_set_agenda, t0, t1);
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	if (!ov->len) {
		app_empty(ui, R(area.x, y, area.w, view.h), _("no upcoming events. tap + to add one."));
		return;
	}
	const double ROW = 70, HEAD = 46;
	/* rows: occurrences that started earlier are listed under today */
	double total = 0;
	Date prev = {0, 0, 0};
	for (int i = 0; i < ov->len; i++) {
		Date d = date_of(MAX(ov->items[i].s, t0));
		if (!date_eq(d, prev)) { total += HEAD; prev = d; }
		total += ROW;
	}
	double off = ui_scroll_begin(ui, ui_id("cal-agenda"), &g_agenda_scroll, view, total + 24);
	cairo_translate(cr, view.x, view.y);
	double yy = 0;
	prev = (Date){0, 0, 0};
	for (int i = 0; i < ov->len; i++) {
		const Occ *o = &ov->items[i];
		const Event *e = &g_ev.items[o->ev];
		Date d = date_of(MAX(o->s, t0));
		if (!date_eq(d, prev)) {
			prev = d;
			if (yy + HEAD > off && yy < off + view.h) {
				char lb[96];
				fmt_day_label(lb, sizeof lb, d);
				gfx_text(cr, lb, 24, yy + 12, 22, FW_SEMILIGHT, th->accent, TA_LEFT, view.w - 48, 1);
			}
			yy += HEAD;
		}
		Rect rr = R(0, yy, view.w, ROW);
		yy += ROW;
		if (rr.y + ROW < off || rr.y > off + view.h) continue;
		UiId id = ui_idn("cal-ag-row", i);
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.12f));
		gfx_fill_rect(cr, R(24, rr.y + 8, 6, ROW - 16), ev_color(e));
		char when[64];
		if (e->allday) str_copy(when, sizeof when, _("all day"));
		else if (o->s < t0 || !date_eq(date_of(o->s), d)) fmt_when(when, sizeof when, e, o->s, o->e);
		else fmt_when(when, sizeof when, e, o->s, o->e);
		gfx_text(cr, ev_title(e), 44, rr.y + 6, 22, FW_SEMILIGHT, th->fg, TA_LEFT, view.w - 68, 1);
		char sub[256];
		if (e->location && *e->location) snprintf(sub, sizeof sub, "%s · %s", when, e->location);
		else str_copy(sub, sizeof sub, when);
		gfx_text(cr, sub, 44, rr.y + 38, 15, FW_REGULAR, th->sub, TA_LEFT, view.w - 68, 1);
		if (ui_clicked(ui, id)) open_details(e->id, o->s);
	}
	ui_scroll_end(ui);
}

/* ------------------------------------------------------------- root page -- */

static void draw_root(UI *ui, Rect area) {
	Rect content = appbar_content(&g_bar, area);
	switch (g_view) {
	case V_DAY: draw_day(ui, content); break;
	case V_WEEK: draw_week(ui, content); break;
	case V_AGENDA: draw_agenda(ui, content); break;
	default: draw_month(ui, content); break;
	}
	AppBarBtn btns[2] = {{ICON_ADD, _("new")}, {ICON_CALENDAR, _("today")}};
	const char *menu[V_COUNT + 2];
	for (int i = 0; i < V_COUNT; i++) menu[i] = tr(VIEW_LABEL[i]);
	menu[V_COUNT] = _("import from .ics file");
	menu[V_COUNT + 1] = _("export to .ics file");
	int a = appbar_draw(ui, &g_bar, area, btns, 2, menu, V_COUNT + 2);
	if (a == 0) open_editor(-1, g_sel, -1);
	else if (a == 1) {
		g_sel = date_today();
		g_day_scroll_key = g_week_scroll_key = 0;
		g_agenda_scroll = (Scroll){0};
		shell_invalidate();
	} else if (a >= APPBAR_MENU && a < APPBAR_MENU + V_COUNT) set_view(a - APPBAR_MENU);
	else if (a == APPBAR_MENU + V_COUNT) start_import();
	else if (a == APPBAR_MENU + V_COUNT + 1) start_export();
}

/* --------------------------------------------------------------- details -- */

static void open_details(int id, time_t s) {
	g_det_id = id;
	g_det_s = s;
	g_det_scroll = (Scroll){0};
	g_det_bar = (AppBar){0};
	nav_push(&g_nav, &sh.ui, P_DETAILS);
}

static void delete_cb(int b, void *ud) {
	if (b != 0) return;
	int i = ev_find(g_det_id);
	if (i >= 0) {
		ev_free(&g_ev.items[i]);
		vec_remove(&g_ev, i);
		save();
	}
	if (nav_page(&g_nav) == P_DETAILS) nav_pop(&g_nav, &sh.ui);
}

static const char *reminder_label(int mins) {
	static char b[64];
	for (size_t i = 0; i < ARRAY_LEN(REM_MIN); i++) if (REM_MIN[i] == mins) return tr(REM_LABEL[i]);
	snprintf(b, sizeof b, _("%d minutes before"), mins);
	return b;
}

static double info_line(UI *ui, Icon ic, const char *text, double y, double w) {
	cairo_t *cr = ui->cr;
	icon_draw(cr, ic, 24 + 12, y + 14, 22, ui->th.sub);
	return gfx_text(cr, text, 60, y + 2, 20, FW_SEMILIGHT, ui->th.fg, TA_LEFT, w - 84, 4) + 14;
}

static void draw_details(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	int i = ev_find(g_det_id);
	if (i < 0) {
		app_empty(ui, area, _("this event was deleted."));
		return;
	}
	Event *e = &g_ev.items[i];
	time_t s = g_det_s, en = occ_end(e, s);
	Rect content = appbar_content(&g_det_bar, area);
	double off = ui_scroll_begin(ui, ui_id("cal-det"), &g_det_scroll, content, 2000);
	(void)off;
	cairo_translate(cr, content.x, content.y);
	double w = content.w, y = 6;
	y += gfx_text(cr, i18n_upper(tr(APP_NAME)), 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, w - 36, 1);
	gfx_fill_rect(cr, R(24, y + 10, 12, 40), ev_color(e));
	y += gfx_text(cr, ev_title(e), 46, y, 40, FW_LIGHT, th->fg, TA_LEFT, w - 70, 3) + 18;
	char day[96], when[96], line[320];
	struct tm stm;
	localtime_r(&s, &stm);
	i18n_fmt_date_long(day, sizeof day, &stm);
	fmt_when(when, sizeof when, e, s, en);
	snprintf(line, sizeof line, "%s\n%s", day, when);
	if (!e->allday && !date_eq(date_of(s), date_of(en > s ? en - 1 : en))) {
		struct tm etm;
		localtime_r(&en, &etm);
		char eday[96], a[32], b[32];
		i18n_fmt_date_long(eday, sizeof eday, &etm);
		fmt_clock(a, sizeof a, s);
		fmt_clock(b, sizeof b, en);
		snprintf(line, sizeof line, "%s, %s –\n%s, %s", day, a, eday, b);
	}
	y += info_line(ui, ICON_CLOCK, line, y, w);
	if (e->location && *e->location) y += info_line(ui, ICON_LOCATION, e->location, y, w);
	if (e->repeat) {
		static const char *const RP[RP_COUNT] = {"", N_("repeats every day"), N_("repeats every week"), N_("repeats every month"), N_("repeats every year")};
		y += info_line(ui, ICON_REPEAT, tr(RP[e->repeat]), y, w);
	}
	if (e->reminder >= 0) {
		snprintf(line, sizeof line, _("reminder: %s"), reminder_label(e->reminder));
		y += info_line(ui, ICON_BELL, line, y, w);
	}
	if (e->notes && *e->notes) {
		y += 8;
		gfx_text(cr, _("notes"), 24, y, 15, FW_REGULAR, th->sub, TA_LEFT, w - 48, 1);
		y += 22;
		y += gfx_text(cr, e->notes, 24, y, 20, FW_SEMILIGHT, th->fg, TA_LEFT, w - 48, 0);
	}
	ui_scroll_end(ui);
	g_det_scroll.off = MIN(g_det_scroll.off, MAX(0, y + 30 - content.h));
	AppBarBtn btns[2] = {{ICON_EDIT, _("edit")}, {ICON_DELETE, _("delete")}};
	int a = appbar_draw(ui, &g_det_bar, area, btns, 2, NULL, 0);
	if (a == 0) open_editor(e->id, date_of(s), -1);
	else if (a == 1)
		dialog_message(_("delete event?"), e->repeat ? _("All occurrences of this repeating event will be deleted.") : ev_title(e),
		               _("delete"), _("cancel"), delete_cb, NULL);
}

/* ---------------------------------------------------------------- editor -- */

static void open_editor(int id, Date d, int hour) {
	ensure_loaded();
	memset(&g_ed, 0, sizeof g_ed);
	g_ed.id = -1;
	int i = id >= 0 ? ev_find(id) : -1;
	if (i >= 0) {
		const Event *e = &g_ev.items[i];
		g_ed.id = e->id;
		str_copy(g_ed.title, sizeof g_ed.title, e->title ? e->title : "");
		str_copy(g_ed.location, sizeof g_ed.location, e->location ? e->location : "");
		str_copy(g_ed.notes, sizeof g_ed.notes, e->notes ? e->notes : "");
		struct tm st, et;
		localtime_r(&e->start, &st);
		localtime_r(&e->end, &et);
		g_ed.date = date_of(e->start);
		g_ed.sh = st.tm_hour;
		g_ed.sm = st.tm_min;
		g_ed.eh = et.tm_hour;
		g_ed.em = et.tm_min;
		g_ed.allday = e->allday;
		Date ed = date_of(e->end);
		int span = 0;
		for (Date x = g_ed.date; !date_eq(x, ed) && span < 400; x = date_next(x)) span++;
		g_ed.span = e->allday ? MAX(0, span - 1) : span;
		g_ed.repeat = e->repeat;
		g_ed.reminder = e->reminder;
		g_ed.color = e->color;
	} else {
		time_t now = time(NULL);
		struct tm nt;
		localtime_r(&now, &nt);
		g_ed.date = d;
		g_ed.sh = hour >= 0 ? hour : MIN(nt.tm_hour + 1, 23);
		g_ed.eh = MIN(g_ed.sh + 1, 23);
		g_ed.em = g_ed.sh == 23 ? 59 : 0;
		g_ed.reminder = 15;
		g_ed.color = cfg.accent;
		g_ed.focus_title = true;
	}
	g_ed_scroll = (Scroll){0};
	g_ed_bar = (AppBar){0};
	nav_push(&g_nav, &sh.ui, P_EDIT);
}

static void ed_date_cb(int b, void *ud) {
	if (b == 0) dialog_date_get(&g_ed.date.y, &g_ed.date.m, &g_ed.date.d);
}

static void ed_start_cb(int b, void *ud) {
	if (b != 0) return;
	/* moving the start keeps the length, like on the phone */
	int dur = g_ed.span * 1440 + g_ed.eh * 60 + g_ed.em - (g_ed.sh * 60 + g_ed.sm);
	dialog_time_get(&g_ed.sh, &g_ed.sm);
	if (dur < 0) dur = 60;
	int end = g_ed.sh * 60 + g_ed.sm + dur;
	g_ed.span = end / 1440;
	g_ed.eh = end / 60 % 24;
	g_ed.em = end % 60;
}

static void ed_end_cb(int b, void *ud) {
	if (b != 0) return;
	dialog_time_get(&g_ed.eh, &g_ed.em);
	int s = g_ed.sh * 60 + g_ed.sm, e = g_ed.eh * 60 + g_ed.em;
	if (g_ed.span <= 1) g_ed.span = e < s ? 1 : 0;   /* earlier end time = ends the next day */
}

static void ed_repeat_cb(int b, void *ud) {
	if (b == 0) g_ed.repeat = CLAMP(dialog_picker_get(), 0, RP_COUNT - 1);
}

static void ed_rem_cb(int b, void *ud) {
	if (b == 0) g_ed.reminder = REM_MIN[CLAMP(dialog_picker_get(), 0, (int)ARRAY_LEN(REM_MIN) - 1)];
}

static void editor_save(void) {
	Event e = {0};
	char *t = str_trim(g_ed.title);
	e.title = xstrdup(*t ? t : _("untitled"));
	t = str_trim(g_ed.location);
	e.location = *t ? xstrdup(t) : NULL;
	e.notes = g_ed.notes[0] ? xstrdup(g_ed.notes) : NULL;
	e.allday = g_ed.allday;
	if (e.allday) {
		e.start = date_time(g_ed.date, 0, 0);
		e.end = date_time(date_add(g_ed.date, g_ed.span + 1), 0, 0);
	} else {
		e.start = date_time(g_ed.date, g_ed.sh, g_ed.sm);
		e.end = date_time(date_add(g_ed.date, g_ed.span), g_ed.eh, g_ed.em);
		if (e.end < e.start) e.end = date_time(date_add(g_ed.date, g_ed.span + 1), g_ed.eh, g_ed.em);
		if (e.end < e.start) e.end = e.start;
	}
	e.repeat = g_ed.repeat;
	e.reminder = g_ed.reminder;
	e.color = g_ed.color;
	int i = g_ed.id >= 0 ? ev_find(g_ed.id) : -1;
	if (i >= 0) {
		e.id = g_ed.id;
		ev_free(&g_ev.items[i]);
		g_ev.items[i] = e;
		g_det_s = e.repeat ? g_det_s : e.start;
		if (e.repeat && date_eq(date_of(g_det_s), g_ed.date)) g_det_s = e.start;
	} else {
		if (g_ev.len >= MAX_EVENTS) { ev_free(&e); return; }
		e.id = g_next_id++;
		vec_push(&g_ev, e);
	}
	g_sel = g_ed.date;
	save();
	sh.ui.focus = 0;
	nav_pop(&g_nav, &sh.ui);
}

static void draw_editor(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect content = appbar_content(&g_ed_bar, area);
	double y = ui_page_header(ui, 14, area.y + 6, tr(APP_NAME), g_ed.id < 0 ? _("new event") : _("edit event"));
	Rect view = R(area.x, y, area.w, rect_bottom(content) - y);
	double x = 24, w = MIN(area.w - 48, 600);
	static double total = 1200;
	double off = ui_scroll_begin(ui, ui_id("cal-ed"), &g_ed_scroll, view, total);
	cairo_translate(cr, view.x, view.y);
	double yy = 4;
	UiId tid = ui_id("cal-ed-title");
	if (g_ed.focus_title) { ui_focus(ui, tid); g_ed.focus_title = false; }
	gfx_text(cr, _("subject"), x, yy, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	ui_textbox(ui, tid, R(x, yy + 22, w, 48), g_ed.title, sizeof g_ed.title, NULL, 0, NULL);
	yy += 80;
	gfx_text(cr, _("location"), x, yy, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	ui_textbox(ui, ui_id("cal-ed-loc"), R(x, yy + 22, w, 48), g_ed.location, sizeof g_ed.location, NULL, 0, NULL);
	yy += 80;
	/* picker fields */
	struct tm dt = date_tm(g_ed.date);
	char buf[96];
	i18n_fmt_date_long(buf, sizeof buf, &dt);
	Rect fr = R(x, yy, w, 74);
	{
		UiId id = ui_id("cal-ed-date");
		gfx_text(cr, _("when"), fr.x, fr.y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		Rect box = R(fr.x, fr.y + 22, w, 50);
		ui_hit(ui, id, box);
		bool pr = ui_pressed(ui, id);
		if (pr) gfx_fill_rect(cr, box, th->accent);
		gfx_stroke_rect(cr, box, 2, pr ? th->accent : th->fg);
		gfx_text(cr, buf, box.x + 12, box.y + 12, 22, FW_SEMILIGHT, pr ? th->on_accent : th->fg, TA_LEFT, box.w - 24, 1);
		if (ui_clicked(ui, id)) dialog_date(_("choose date"), g_ed.date.y, g_ed.date.m, g_ed.date.d, ed_date_cb, NULL);
	}
	yy += 82;
	ui_toggle(ui, ui_id("cal-ed-allday"), R(x, yy, w, 66), _("all day"), &g_ed.allday);
	yy += 72;
	if (!g_ed.allday) {
		double hw = (w - 12) / 2;
		const char *labels[2] = {_("start time"), _("end time")};
		int hh[2] = {g_ed.sh, g_ed.eh}, mm[2] = {g_ed.sm, g_ed.em};
		for (int k = 0; k < 2; k++) {
			UiId id = ui_idn("cal-ed-time", k);
			Rect r = R(x + k * (hw + 12), yy, hw, 74);
			gfx_text(cr, labels[k], r.x, r.y, 15, FW_REGULAR, th->sub, TA_LEFT, hw, 1);
			Rect box = R(r.x, r.y + 22, hw, 50);
			ui_hit(ui, id, box);
			bool pr = ui_pressed(ui, id);
			if (pr) gfx_fill_rect(cr, box, th->accent);
			gfx_stroke_rect(cr, box, 2, pr ? th->accent : th->fg);
			fmt_hm(buf, sizeof buf, hh[k], mm[k]);
			if (k == 1 && g_ed.span > 0) {
				size_t l = strlen(buf);
				snprintf(buf + l, sizeof buf - l, " (+%d)", g_ed.span);
			}
			gfx_text(cr, buf, box.x + 12, box.y + 12, 22, FW_SEMILIGHT, pr ? th->on_accent : th->fg, TA_LEFT, box.w - 24, 1);
			if (ui_clicked(ui, id))
				dialog_time(labels[k], hh[k], mm[k], k ? ed_end_cb : ed_start_cb, NULL);
		}
		yy += 82;
	}
	const char *pick_labels[2] = {_("repeats"), _("reminder")};
	const char *pick_vals[2] = {tr(REPEAT_LABEL[g_ed.repeat]), reminder_label(g_ed.reminder)};
	for (int k = 0; k < 2; k++) {
		UiId id = ui_idn("cal-ed-pick", k);
		gfx_text(cr, pick_labels[k], x, yy, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		Rect box = R(x, yy + 22, w, 50);
		ui_hit(ui, id, box);
		bool pr = ui_pressed(ui, id);
		if (pr) gfx_fill_rect(cr, box, th->accent);
		gfx_stroke_rect(cr, box, 2, pr ? th->accent : th->fg);
		gfx_text(cr, pick_vals[k], box.x + 12, box.y + 12, 22, FW_SEMILIGHT, pr ? th->on_accent : th->fg, TA_LEFT, box.w - 24, 1);
		if (ui_clicked(ui, id)) {
			static const char *items[ARRAY_LEN(REM_MIN)];
			if (k == 0) {
				for (int i = 0; i < RP_COUNT; i++) items[i] = tr(REPEAT_LABEL[i]);
				dialog_picker(_("repeats"), items, RP_COUNT, g_ed.repeat, ed_repeat_cb, NULL);
			} else {
				int sel = 3;
				for (int i = 0; i < (int)ARRAY_LEN(REM_MIN); i++) {
					items[i] = tr(REM_LABEL[i]);
					if (REM_MIN[i] == g_ed.reminder) sel = i;
				}
				dialog_picker(_("reminder"), items, (int)ARRAY_LEN(REM_MIN), sel, ed_rem_cb, NULL);
			}
		}
		yy += 82;
	}
	/* calendar colour: the twenty accent colours */
	gfx_text(cr, _("colour"), x, yy, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	yy += 24;
	int per = w > 420 ? 10 : 5;
	double gap = 6, cs = MIN(44, (w - (per - 1) * gap) / per);
	for (int i = 0; i < ACCENT_COUNT; i++) {
		Rect r = R(x + (i % per) * (cs + gap), yy + (i / per) * (cs + gap), cs, cs);
		UiId id = ui_idn("cal-ed-col", i);
		ui_hit(ui, id, r);
		gfx_fill_rect(cr, r, RGBX(ACCENT_RGB[i]));
		if (i == g_ed.color) {
			gfx_stroke_rect(cr, rect_inset(r, -3), 2, th->fg);
			icon_draw(cr, ICON_CHECK, rect_cx(r), rect_cy(r), cs * 0.5, COL_WHITE);
		}
		if (ui_clicked(ui, id)) g_ed.color = i;
	}
	yy += ((ACCENT_COUNT + per - 1) / per) * (cs + gap) + 16;
	gfx_text(cr, _("notes"), x, yy, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	yy += 22;
	TextAreaInfo info = {0};
	ui_textarea(ui, ui_id("cal-ed-notes"), R(x, yy, w, 120), g_ed.notes, sizeof g_ed.notes, NULL, 20, 0, &info);
	/* keep the caret visible while typing notes */
	if (info.focused && info.changed) {
		double cy = info.caret_y + info.caret_h + 20;
		if (cy > off + view.h) ui_scroll_to(&g_ed_scroll, cy - view.h);
	}
	yy += MAX(120, info.height) + 30;
	total = yy;
	ui_scroll_end(ui);
	AppBarBtn btns[2] = {{ICON_SAVE, _("save")}, {ICON_CLOSE, _("cancel")}};
	int a = appbar_draw(ui, &g_ed_bar, area, btns, 2, NULL, 0);
	if (a == 0) editor_save();
	else if (a == 1) { sh.ui.focus = 0; nav_pop(&g_nav, &sh.ui); }
}

/* ------------------------------------------------------------------- app -- */

static void nav_page_fn(UI *ui, int page, Rect area, void *ud) {
	switch (page) {
	case P_DETAILS: draw_details(ui, area); break;
	case P_EDIT: draw_editor(ui, area); break;
	default: draw_root(ui, area); break;
	}
}

static void cal_draw(UI *ui, Rect area) {
	ensure_loaded();
	load_view();
	if (!g_sel.y) g_sel = date_today();
	nav_draw(ui, &g_nav, area, nav_page_fn, NULL);
}

static void cal_init(void) {
	nav_init(&g_nav, P_ROOT);
	g_rem_last = time(NULL);     /* reminders that were due before start-up are not replayed */
	g_tick = timer_add(30000, true, tick_cb, NULL);
}

static void cal_open(const char *arg) {
	ensure_loaded();
	load_view();
	if (!g_sel.y || nav_page(&g_nav) == P_ROOT) g_sel = date_today();
	if (!arg || !*arg) return;
	nav_init(&g_nav, P_ROOT);
	for (int i = 0; i < V_COUNT; i++) if (str_eq(arg, VIEW_LABEL[i])) g_view = i;
	if (str_eq(arg, "new")) open_editor(-1, g_sel, -1);
	else if (str_eq(arg, "import")) start_import();
}

static bool cal_back(void) {
	switch (nav_page(&g_nav)) {
	case P_DETAILS: if (appbar_back(&g_det_bar)) return true; break;
	case P_EDIT: if (appbar_back(&g_ed_bar)) return true; break;
	default: if (appbar_back(&g_bar)) return true; break;
	}
	if (nav_pop(&g_nav, &sh.ui)) { sh.ui.focus = 0; return true; }
	if (g_view_back >= 0) { g_view = g_view_back; g_view_back = -1; shell_invalidate(); return true; }
	return false;
}

static void cal_close(void) {
	nav_init(&g_nav, P_ROOT);
	occ_sets_free();
	g_bar = (AppBar){0};
	if (g_view_back >= 0) { g_view = g_view_back; g_view_back = -1; }
	for (int i = 0; i < g_nfiles; i++) free(g_files[i]);
	g_nfiles = 0;
}

/* --------------------------------------------------------------- live tile */

static void cal_tile(UI *ui, Rect r, TileSize size, double now) {
	ensure_loaded();
	cairo_t *cr = ui->cr;
	time_t tn = time(NULL);
	Date today = date_today();
	struct tm tm = date_tm(today);
	char num[8];
	snprintf(num, sizeof num, "%d", today.d);
	const char *wd = i18n_lower(i18n_weekday(tm.tm_wday, false));
	if (size != TILE_WIDE) {
		double big = MIN(r.h * 0.5, 110);
		double y = rect_bottom(r) - 34 - gfx_line_height(cr, big, FW_LIGHT) * 0.9;
		gfx_text(cr, num, r.x + 10, y, big, FW_LIGHT, COL_WHITE, TA_LEFT, r.w - 20, 1);
		gfx_text(cr, wd, r.x + 12, rect_bottom(r) - 30, MIN(20, r.h * 0.11), FW_SEMILIGHT, COL_WHITE, TA_LEFT, r.w - 24, 1);
		return;
	}
	/* wide: next event on the left, the date on the right */
	time_t t0 = date_time(today, 0, 0);
	const OccVec *ov = occ_range(&g_set_tile, t0, t0 + 14 * 86400);
	const Occ *next = NULL;
	for (int i = 0; i < ov->len && !next; i++) {
		const Occ *o = &ov->items[i];
		const Event *e = &g_ev.items[o->ev];
		if (e->allday ? o->e > tn : (o->e > tn || o->s >= tn)) next = o;
	}
	double rw = MIN(r.w * 0.34, 130), lx = r.x + 12, lw = r.w - rw - 24;
	double big = MIN(r.h * 0.46, 80);
	gfx_text(cr, num, rect_right(r) - 12, r.y + 4, big, FW_LIGHT, COL_WHITE, TA_RIGHT, rw, 1);
	gfx_text(cr, wd, rect_right(r) - 12, r.y + 8 + gfx_line_height(cr, big, FW_LIGHT) * 0.92, 17, FW_SEMILIGHT, COL_WHITE, TA_RIGHT, rw, 1);
	if (next) {
		const Event *e = &g_ev.items[next->ev];
		double y = r.y + 10;
		y += gfx_text(cr, ev_title(e), lx, y, 19, FW_SEMIBOLD, COL_WHITE, TA_LEFT, lw, 2);
		char when[160], w[64];
		Date d = date_of(next->s);
		fmt_when(w, sizeof w, e, next->s, next->e);
		if (date_eq(d, today) || next->s < tn) str_copy(when, sizeof when, w);
		else {
			char dl[64];
			fmt_day_label(dl, sizeof dl, d);
			snprintf(when, sizeof when, "%s, %s", dl, w);
		}
		y += gfx_text(cr, when, lx, y, 15, FW_REGULAR, COL_WHITE, TA_LEFT, lw, 1);
		if (e->location && *e->location) gfx_text(cr, e->location, lx, y, 15, FW_REGULAR, col_alpha(COL_WHITE, 0.85f), TA_LEFT, lw, 1);
	} else {
		gfx_text(cr, _("no upcoming events"), lx, r.y + 10, 17, FW_SEMILIGHT, COL_WHITE, TA_LEFT, lw, 2);
	}
	app_tile_name(ui, r, tr(APP_NAME));
}

const AppDef app_calendar = {
	.id = "calendar", .name = N_("Calendar"), .icon = ICON_CALENDAR, .color = 0,
	.keywords = "calendar events appointments agenda schedule ics",
	.init = cal_init, .open = cal_open, .draw = cal_draw, .back = cal_back, .close = cal_close,
	.tile = cal_tile,
};

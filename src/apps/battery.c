/* Battery Saver: battery status, estimates, health, the battery saver switch with
 * automatic low-battery mode, and a lightweight process / memory usage monitor. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../sys.h"
#include "../util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

/* ---------------------------------------------------------------- sampling */

typedef struct {
	bool real;                  /* a battery was found in sysfs */
	bool ac_online;
	char status[24];
	int pct;
	bool charge_based;          /* charge_* (µAh / µA) instead of energy_* (µWh / µW) */
	double now, full, design;   /* Wh or Ah */
	double rate;                /* W or A, >0 */
	double voltage;             /* V */
	int cycles;                 /* -1 unknown */
	char tech[24], model[48], vendor[48];
} BatInfo;

typedef struct {
	int pid;
	char name[32];
	char state;
	unsigned long long ticks;
	long rss_kb;
	int threads;
	int uid;
} PSamp;

typedef struct {
	bool with_procs;
	BatInfo bat;
	PSamp *p;
	int n;
	unsigned long long total;   /* all-CPU jiffies */
	double mem_total, mem_avail, swap_total, swap_free;   /* bytes */
	long self_rss_kb;
	double t;
} Sample;

typedef struct {
	int pid;
	char name[32];
	double cpu;                 /* percent of total CPU capacity */
	long rss_kb;
	int threads;
	char state;
	int uid;
} ProcRow;

static double read_num(const char *dir, const char *file, double def) {
	char *p = path_join(dir, file);
	char *s = file_read_all(p, NULL);
	free(p);
	if (!s) return def;
	char *end;
	double v = strtod(s, &end);
	bool ok = end != s;
	free(s);
	return ok ? v : def;
}

static void read_str(const char *dir, const char *file, char *out, size_t cap) {
	char *p = path_join(dir, file);
	char *s = file_read_all(p, NULL);
	free(p);
	out[0] = 0;
	if (!s) return;
	str_copy(out, cap, str_trim(s));
	free(s);
}

static void sample_battery(BatInfo *b) {
	memset(b, 0, sizeof *b);
	b->cycles = -1;
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char *base = path_join("/sys/class/power_supply", e->d_name);
		char type[24], scope[24];
		read_str(base, "type", type, sizeof type);
		read_str(base, "scope", scope, sizeof scope);
		if (str_eq(type, "Mains") || str_eq(type, "USB")) {
			if (read_num(base, "online", 0) > 0) b->ac_online = true;
		} else if (str_eq(type, "Battery") && !str_eq(scope, "Device") && !b->real) {
			b->real = true;
			read_str(base, "status", b->status, sizeof b->status);
			b->pct = (int)CLAMP(read_num(base, "capacity", -1), -1, 100);
			double en = read_num(base, "energy_now", -1);
			if (en >= 0) {
				b->now = en / 1e6;
				b->full = read_num(base, "energy_full", -1) / 1e6;
				b->design = read_num(base, "energy_full_design", -1) / 1e6;
				b->rate = fabs(read_num(base, "power_now", 0)) / 1e6;
			} else {
				double cn = read_num(base, "charge_now", -1);
				if (cn >= 0) {
					b->charge_based = true;
					b->now = cn / 1e6;
					b->full = read_num(base, "charge_full", -1) / 1e6;
					b->design = read_num(base, "charge_full_design", -1) / 1e6;
					b->rate = fabs(read_num(base, "current_now", 0)) / 1e6;
				}
			}
			b->voltage = read_num(base, "voltage_now", 0) / 1e6;
			if (b->charge_based == false && b->rate == 0 && b->voltage > 0) {
				/* some drivers report current but energy */
				double cur = fabs(read_num(base, "current_now", 0)) / 1e6;
				b->rate = cur * b->voltage;
			}
			b->cycles = (int)read_num(base, "cycle_count", -1);
			if (b->cycles == 0) b->cycles = -1;   /* 0 usually means "not reported" */
			read_str(base, "technology", b->tech, sizeof b->tech);
			read_str(base, "model_name", b->model, sizeof b->model);
			read_str(base, "manufacturer", b->vendor, sizeof b->vendor);
			if (b->pct < 0 && b->full > 0) b->pct = (int)CLAMP(b->now / b->full * 100 + 0.5, 0, 100);
		}
		free(base);
	}
	closedir(d);
}

static long status_kb(const char *path, const char *key) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[256];
	long v = 0;
	size_t kl = strlen(key);
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
	}
	fclose(f);
	return v;
}

static void sample_procs(Sample *s) {
	FILE *f = fopen("/proc/stat", "r");
	if (f) {
		char line[512];
		if (fgets(line, sizeof line, f) && str_starts(line, "cpu ")) {
			char *p = line + 4;
			while (*p) {
				char *end;
				unsigned long long v = strtoull(p, &end, 10);
				if (end == p) break;
				s->total += v;
				p = end;
			}
		}
		fclose(f);
	}
	s->mem_total = status_kb("/proc/meminfo", "MemTotal") * 1024.0;
	s->mem_avail = status_kb("/proc/meminfo", "MemAvailable") * 1024.0;
	s->swap_total = status_kb("/proc/meminfo", "SwapTotal") * 1024.0;
	s->swap_free = status_kb("/proc/meminfo", "SwapFree") * 1024.0;
	s->self_rss_kb = status_kb("/proc/self/status", "VmRSS");
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	DIR *d = opendir("/proc");
	if (!d) return;
	int cap = 256;
	s->p = xmalloc((size_t)cap * sizeof(PSamp));
	struct dirent *e;
	while ((e = readdir(d)) && s->n < 8192) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char path[300], buf[1024];
		snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
		FILE *sf = fopen(path, "r");
		if (!sf) continue;
		size_t len = fread(buf, 1, sizeof buf - 1, sf);
		fclose(sf);
		buf[len] = 0;
		char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
		if (!lp || !rp || rp < lp) continue;
		PSamp ps = {.pid = atoi(e->d_name)};
		size_t nl = MIN((size_t)(rp - lp - 1), sizeof ps.name - 1);
		memcpy(ps.name, lp + 1, nl);
		ps.name[nl] = 0;
		/* fields after the name start at field 3 (state) */
		unsigned long long ut = 0, st = 0;
		long threads = 0, rss = 0;
		char state = '?';
		int nf = sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %ld %*d %*u %*u %ld",
		                &state, &ut, &st, &threads, &rss);
		if (nf < 5) continue;
		ps.state = state;
		ps.ticks = ut + st;
		ps.threads = (int)threads;
		ps.rss_kb = rss * page_kb;
		snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
		ps.uid = (int)status_kb(path, "Uid");
		if (s->n >= cap) { cap *= 2; s->p = xrealloc(s->p, (size_t)cap * sizeof(PSamp)); }
		s->p[s->n++] = ps;
	}
	closedir(d);
}

static void sample_run(void *arg) {
	Sample *s = arg;
	sample_battery(&s->bat);
	if (s->with_procs) sample_procs(s);
}

/* ------------------------------------------------------------------ state - */

enum { PG_MAIN, PG_PROC };

static struct {
	bool have;
	BatInfo bat;
	double bat_t;
	/* process monitor */
	Sample *prev;
	ProcRow rows[25];
	int nrows, nprocs;
	ProcRow *all;               /* every process of the last sample (for the details page) */
	int nall;
	double mem_total, mem_avail, swap_total, swap_free;
	long self_rss_kb;
	bool procs_ready;
	double procs_t;
	bool pending;
	/* % based fallback estimate when the driver does not report power */
	double est_t0;
	int est_p0;
	bool est_charging;
	double est_rate;            /* % per hour */
} B;

static Timer *g_timer;
static bool g_timer_armed;
static Timer *g_auto_timer, *g_quick;
static Pager g_pager;
static Nav g_nav;
static Scroll g_scroll[2], g_dscroll;
static AppBar g_dbar;
static int g_sel_pid = -1;
static char g_sel_name[32];

static bool visible(void) {
	return sh.screen == SCR_APP && sh.cur_app == &app_battery && !sh.locked;
}

static bool want_procs(void) { return visible() && (g_pager.target == 1 || nav_page(&g_nav) == PG_PROC); }

static int pcmp(const void *a, const void *b) {
	return ((const PSamp *)a)->pid - ((const PSamp *)b)->pid;
}

static int rowcmp(const void *a, const void *b) {
	const ProcRow *x = a, *y = b;
	if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
	if (x->rss_kb != y->rss_kb) return x->rss_kb < y->rss_kb ? 1 : -1;
	return x->pid - y->pid;
}

static void sample_free(Sample *s) {
	if (!s) return;
	free(s->p);
	free(s);
}

static void update_estimate(void) {
	Battery sb = sys_battery();
	int pct = B.bat.real ? B.bat.pct : sb.pct;
	bool charging = B.bat.real ? str_eq(B.bat.status, "Charging") : sb.charging;
	double t = now_sec();
	if (B.est_t0 == 0 || charging != B.est_charging || (charging ? pct < B.est_p0 : pct > B.est_p0)) {
		B.est_t0 = t;
		B.est_p0 = pct;
		B.est_charging = charging;
		B.est_rate = 0;
		return;
	}
	int dp = abs(pct - B.est_p0);
	if (dp >= 2 && t - B.est_t0 > 300) B.est_rate = dp / ((t - B.est_t0) / 3600);
}

static void request_sample(bool procs);

static void quick_cb(void *ud) {
	if (want_procs()) request_sample(true);
}

static void sample_done(void *arg) {
	Sample *s = arg;
	B.pending = false;
	B.bat = s->bat;
	B.bat_t = now_sec();
	B.have = true;
	update_estimate();
	if (s->with_procs) {
		qsort(s->p, (size_t)s->n, sizeof(PSamp), pcmp);
		unsigned long long dt = B.prev ? s->total - B.prev->total : 0;
		free(B.all);
		B.all = xmalloc((size_t)MAX(1, s->n) * sizeof(ProcRow));
		B.nall = 0;
		for (int i = 0; i < s->n; i++) {
			PSamp *c = &s->p[i];
			double cpu = 0;
			if (B.prev && dt > 0) {
				PSamp key = {.pid = c->pid};
				PSamp *o = bsearch(&key, B.prev->p, (size_t)B.prev->n, sizeof(PSamp), pcmp);
				if (o && str_eq(o->name, c->name) && c->ticks >= o->ticks) cpu = (double)(c->ticks - o->ticks) * 100.0 / (double)dt;
			}
			ProcRow r = {.pid = c->pid, .cpu = CLAMP(cpu, 0, 100), .rss_kb = c->rss_kb, .threads = c->threads,
				.state = c->state, .uid = c->uid};
			str_copy(r.name, sizeof r.name, c->name);
			B.all[B.nall++] = r;
		}
		qsort(B.all, (size_t)B.nall, sizeof(ProcRow), rowcmp);
		/* keep the list still while a finger is on it so taps hit the intended row */
		if (!sh.ui.p.down || !B.nrows) {
			B.nrows = MIN(B.nall, 25);
			memcpy(B.rows, B.all, (size_t)B.nrows * sizeof(ProcRow));
		}
		/* the details page looks processes up by pid */
		B.nprocs = s->n;
		B.mem_total = s->mem_total;
		B.mem_avail = s->mem_avail;
		B.swap_total = s->swap_total;
		B.swap_free = s->swap_free;
		B.self_rss_kb = s->self_rss_kb;
		B.procs_ready = B.prev != NULL;
		B.procs_t = now_sec();
		sample_free(B.prev);
		B.prev = s;
		s = NULL;
		/* first sample only gives a baseline: take the second one soon */
		if (!B.procs_ready && want_procs()) {
			if (!g_quick) g_quick = timer_add(0, false, quick_cb, NULL);
			timer_set(g_quick, 700);
		}
	}
	sample_free(s);
	shell_invalidate();
}

static void request_sample(bool procs) {
	if (B.pending) return;
	Sample *s = xcalloc(1, sizeof *s);
	s->with_procs = procs;
	B.pending = true;
	job_submit(sample_run, sample_done, s);
}

static void tick(void *ud) {
	if (!visible()) {
		timer_set(g_timer, 0);
		g_timer_armed = false;
		return;
	}
	bool procs = want_procs();
	if (procs || now_sec() - B.bat_t > 9.5) request_sample(procs);
}

/* ------------------------------------------------------- automatic saver -- */

static const int THRESHOLDS[] = {10, 15, 20, 30};

static cJSON *settings(void) { return config_section("battery", false); }

static bool auto_enabled(void) {
	cJSON *j = cJSON_GetObjectItem(settings(), "auto");
	return j ? cJSON_IsTrue(j) : true;
}

static int auto_threshold(void) {
	cJSON *j = cJSON_GetObjectItem(settings(), "threshold");
	return cJSON_IsNumber(j) ? CLAMP(j->valueint, 5, 50) : 20;
}

static bool auto_was_on(void) { return cJSON_IsTrue(cJSON_GetObjectItem(settings(), "auto_on")); }

static void set_item(const char *key, cJSON *v) {
	cJSON *s = settings();
	if (cJSON_GetObjectItem(s, key)) cJSON_ReplaceItemInObject(s, key, v);
	else cJSON_AddItemToObject(s, key, v);
	config_save_soon();
}

static void set_saver(bool on, bool automatic) {
	if (cfg.battery_saver != on) {
		cfg.battery_saver = on;
		if (on) sys_set_brightness(MIN(sys_brightness(), 40));
		config_save_soon();
	}
	set_item("auto_on", cJSON_CreateBool(on && automatic));
	shell_invalidate();
}

static void auto_check(void *ud) {
	if (!auto_enabled()) return;
	Battery b = sys_battery();
	if (!b.present) return;
	bool plugged = b.charging || b.full;
	if (!plugged && !cfg.battery_saver && b.pct <= auto_threshold()) {
		set_saver(true, true);
		char body[160];
		snprintf(body, sizeof body, _("your battery is at %d%%, so battery saver is now on to make it last longer."), b.pct);
		shell_toast(_("battery saver is on"), body, _("Battery Saver"));
	} else if (plugged && cfg.battery_saver && auto_was_on()) {
		set_saver(false, false);
		shell_toast(_("battery saver is off"), _("your phone is charging."), _("Battery Saver"));
	}
}

/* ------------------------------------------------------------- formatting - */

static void fmt_duration(double hours, char *buf, size_t cap) {
	long min = lround(hours * 60);
	if (min < 1) min = 1;
	long d = min / 1440, h = (min % 1440) / 60, m = min % 60;
	if (d > 0) snprintf(buf, cap, _("%ld d %ld h"), d, h);
	else if (h > 0) snprintf(buf, cap, _("%ld h %ld min"), h, m);
	else snprintf(buf, cap, _("%ld min"), m);
}

static void fmt_kb(long kb, char *buf, size_t cap) { human_size(kb * 1024.0, buf, cap); }

typedef struct {
	bool present, charging, full, demo;
	int pct;
	double hours;               /* estimate, <0 unknown */
	bool to_full;
} Status;

static Status cur_status(void) {
	Status st = {.hours = -1};
	Battery sb = sys_battery();
	if (B.have && B.bat.real) {
		st.present = true;
		st.pct = B.bat.pct >= 0 ? B.bat.pct : sb.pct;
		st.charging = str_eq(B.bat.status, "Charging");
		st.full = str_eq(B.bat.status, "Full") || (st.pct >= 100 && B.bat.ac_online);
		if (B.bat.rate > 0.01 && B.bat.now > 0) {
			if (st.charging && B.bat.full > B.bat.now) { st.hours = (B.bat.full - B.bat.now) / B.bat.rate; st.to_full = true; }
			else if (!st.charging && !st.full) st.hours = B.bat.now / B.bat.rate;
		}
	} else if (sb.present) {
		st.present = true;
		st.pct = sb.pct;
		st.charging = sb.charging;
		st.full = sb.full;
		st.demo = sys_demo() && !(B.have && B.bat.real);
		if (st.demo && !st.charging && !st.full) st.hours = st.pct * 0.1;   /* sample: 10 %/h drain */
		else if (st.demo && st.charging) { st.hours = (100 - st.pct) / 60.0; st.to_full = true; }
	}
	/* fall back to the observed rate of change */
	if (st.present && st.hours < 0 && B.est_rate > 0 && !st.full) {
		st.to_full = st.charging;
		st.hours = (st.charging ? 100 - st.pct : st.pct) / B.est_rate;
	}
	if (st.hours > 24 * 14) st.hours = -1;
	return st;
}

/* -------------------------------------------------------------- overview -- */

static void threshold_cb(int b, void *ud) {
	if (b != 0) return;
	int i = CLAMP(dialog_picker_get(), 0, (int)ARRAY_LEN(THRESHOLDS) - 1);
	set_item("threshold", cJSON_CreateNumber(THRESHOLDS[i]));
	auto_check(NULL);
	shell_invalidate();
}

/* Check box with a label that wraps over several lines. Returns the height used. */
static double wrap_checkbox(UI *ui, UiId id, double x, double y, double w, const char *label, bool *v, bool *changed) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Size ts = gfx_text_size(cr, label, 20, FW_SEMILIGHT, w - 50, 3);
	double h = MAX(44, ts.h + 12);
	Rect r = R(x, y, w, h);
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	*changed = false;
	if (ui_clicked(ui, id)) { *v = !*v; *changed = true; }
	Rect box = R(x + 2, y + 6, 30, 30);
	if (pr) gfx_fill_rect(cr, box, col_alpha(th->fg, 0.2f));
	gfx_stroke_rect(cr, box, 2, pr ? th->sub : th->fg);
	if (*v) icon_draw(cr, ICON_CHECK, rect_cx(box), rect_cy(box), 22, th->fg);
	gfx_text(cr, label, x + 46, y + 6, 20, FW_SEMILIGHT, th->fg, TA_LEFT, w - 50, 3);
	return h;
}

/* Windows Phone list picker field: caption above, value in a bordered box. */
static bool picker_field(UI *ui, UiId id, Rect r, const char *caption, const char *value) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	gfx_text(cr, caption, r.x, r.y, 15, FW_REGULAR, th->sub, TA_LEFT, r.w, 1);
	Rect box = R(r.x, r.y + 22, r.w, 42);
	ui_hit(ui, id, box);
	bool pr = ui_pressed(ui, id);
	if (pr) gfx_fill_rect(cr, box, th->accent);
	gfx_stroke_rect(cr, box, 2, pr ? th->accent : th->fg);
	gfx_text(cr, value, box.x + 10, box.y + (box.h - gfx_line_height(cr, 20, FW_SEMILIGHT)) / 2, 20, FW_SEMILIGHT,
	         pr ? th->on_accent : th->fg, TA_LEFT, box.w - 20, 1);
	return ui_clicked(ui, id);
}

static double info_line(UI *ui, double x, double y, double w, const char *k, const char *v) {
	cairo_t *cr = ui->cr;
	double h = gfx_text(cr, k, x, y, 15, FW_REGULAR, ui->th.sub, TA_LEFT, w, 1);
	h += gfx_text(cr, v, x, y + h - 2, 22, FW_SEMILIGHT, ui->th.fg, TA_LEFT, w, 3);
	return h + 10;
}

static void overview_page(UI *ui, Rect view) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Status st = cur_status();
	static double content_h = 800;
	double off = ui_scroll_begin(ui, ui_id("bat-ov"), &g_scroll[0], view, content_h);
	(void)off;
	cairo_translate(cr, view.x, view.y);
	double x = 24, w = view.w - 48, y = 0;
	char buf[160], v[160];

	if (!st.present) {
		icon_draw(cr, ICON_POWER, x + 30, y + 40, 56, th->fg);
		y += 90;
		y += gfx_text(cr, _("no battery found. this device is running on external power."), x, y, 20, FW_SEMILIGHT, th->sub,
		              TA_LEFT, w, 0) + 20;
	} else {
		/* big percentage with the battery glyph */
		snprintf(buf, sizeof buf, _("%d%%"), st.pct);
		Size ps = gfx_text_size(cr, buf, 110, FW_LIGHT, 0, 1);
		gfx_text(cr, buf, x - 6, y - 18, 110, FW_LIGHT, th->fg, TA_LEFT, 0, 1);
		icon_draw_battery(cr, x + ps.w + 34, y + ps.h * 0.5 - 8, 44, th->fg, st.pct, st.charging);
		y += ps.h - 22;
		const char *state = st.full ? _("fully charged") : st.charging ? _("charging") :
		                    (B.bat.real && B.bat.ac_online) ? _("plugged in, not charging") : _("on battery");
		y += gfx_text(cr, state, x, y, 22, FW_SEMILIGHT, cfg.battery_saver ? th->accent : th->sub, TA_LEFT, w, 1) + 18;
		if (!st.full) {
			const char *cap = st.to_full ? _("time until fully charged") : _("estimated time remaining");
			if (st.hours >= 0) fmt_duration(st.hours, v, sizeof v);
			else str_copy(v, sizeof v, _("estimating..."));
			y = y + info_line(ui, x, y, w, cap, v);
		}
	}
	y += 8;
	/* battery saver switch */
	bool on = cfg.battery_saver;
	if (ui_toggle(ui, ui_id("bat-saver"), R(x, y, w, 64), _("Battery Saver"), &on)) set_saver(on, false);
	y += 68;
	y += gfx_text(cr, _("battery saver turns off animations and live tile flips and dims the screen so your battery lasts longer."),
	              x, y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 0) + 20;
	bool au = auto_enabled(), changed;
	y += wrap_checkbox(ui, ui_id("bat-auto"), x - 2, y, w, _("turn battery saver on automatically if my battery is low"), &au,
	                   &changed) + 8;
	if (changed) { set_item("auto", cJSON_CreateBool(au)); if (au) auto_check(NULL); }
	if (au) {
		snprintf(v, sizeof v, _("%d%%"), auto_threshold());
		if (picker_field(ui, ui_id("bat-thr"), R(x, y, MIN(w, 300), 64), _("when my battery level is at or below"), v)) {
			static char lab[4][8];
			static const char *labels[4];
			int sel = 2;
			for (int i = 0; i < 4; i++) {
				snprintf(lab[i], sizeof lab[i], _("%d%%"), THRESHOLDS[i]);
				labels[i] = lab[i];
				if (THRESHOLDS[i] == auto_threshold()) sel = i;
			}
			dialog_picker(_("battery level"), labels, 4, sel, threshold_cb, NULL);
		}
		y += 76;
	}
	y += 14;
	/* battery details */
	if (B.bat.real) {
		y = ui_section(ui, R(x, y, w, 30), _("battery details")) + 6;
		const char *unit = B.bat.charge_based ? "Ah" : "Wh";
		if (B.bat.full > 0 && B.bat.design > 0) {
			double hp = B.bat.full / B.bat.design * 100;
			snprintf(v, sizeof v, _("%.0f%% (%.1f %s of %.1f %s design capacity)"), MIN(hp, 100), B.bat.full, unit, B.bat.design, unit);
			y += info_line(ui, x, y, w, _("battery health"), v);
		}
		if (B.bat.now > 0) {
			snprintf(v, sizeof v, "%.1f %s", B.bat.now, unit);
			y += info_line(ui, x, y, w, _("remaining capacity"), v);
		}
		if (B.bat.rate > 0.01) {
			if (B.bat.charge_based) snprintf(v, sizeof v, "%.2f A", B.bat.rate);
			else snprintf(v, sizeof v, "%.1f W", B.bat.rate);
			y += info_line(ui, x, y, w, st.charging ? _("charge rate") : _("power draw"), v);
		}
		if (B.bat.cycles > 0) {
			snprintf(v, sizeof v, "%d", B.bat.cycles);
			y += info_line(ui, x, y, w, _("charge cycles"), v);
		}
		if (B.bat.voltage > 0) {
			snprintf(v, sizeof v, "%.2f V", B.bat.voltage);
			y += info_line(ui, x, y, w, _("voltage"), v);
		}
		if (B.bat.tech[0] && !str_eq(B.bat.tech, "Unknown")) y += info_line(ui, x, y, w, _("technology"), B.bat.tech);
		if (B.bat.model[0] || B.bat.vendor[0]) {
			snprintf(v, sizeof v, "%s%s%s", B.bat.vendor, B.bat.vendor[0] && B.bat.model[0] ? " " : "", B.bat.model);
			y += info_line(ui, x, y, w, _("model"), v);
		}
	} else if (st.demo) {
		y += gfx_text(cr, _("demo mode: no battery hardware was found, so these are sample values."), x, y, 15, FW_REGULAR,
		              th->sub, TA_LEFT, w, 0) + 10;
	}
	content_h = y + 30;
	ui_scroll_end(ui);
}

/* ----------------------------------------------------------------- usage -- */

static const ProcRow *find_proc(int pid) {
	for (int i = 0; i < B.nall; i++) if (B.all[i].pid == pid) return &B.all[i];
	return NULL;
}

static void usage_page(UI *ui, Rect view) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	const double RH = 66, TOP = 200;
	char a[32], b[32], buf[200];
	if (!B.procs_t) {
		ui_progress_dots(ui, R(view.x, view.y + 20, view.w, 10), th->accent);
		return;
	}
	double off = ui_scroll_begin(ui, ui_id("bat-use"), &g_scroll[1], view, TOP + B.nrows * RH + 24);
	cairo_translate(cr, view.x, view.y);
	double x = 24, w = view.w - 48, y = 0;
	if (off < TOP) {
		/* memory overview */
		double used = B.mem_total - B.mem_avail;
		gfx_text(cr, _("memory"), x, y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		human_size(used, a, sizeof a);
		human_size(B.mem_total, b, sizeof b);
		snprintf(buf, sizeof buf, _("%s used of %s"), a, b);
		gfx_text(cr, buf, x, y + 18, 26, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		Rect bar = R(x, y + 56, w, 10);
		gfx_fill_rect(cr, bar, th->chrome2);
		if (B.mem_total > 0) gfx_fill_rect(cr, R(bar.x, bar.y, bar.w * CLAMP(used / B.mem_total, 0, 1), bar.h), th->accent);
		y += 76;
		if (B.swap_total > 0) {
			human_size(B.swap_total - B.swap_free, a, sizeof a);
			human_size(B.swap_total, b, sizeof b);
			snprintf(buf, sizeof buf, _("swap: %s used of %s"), a, b);
			gfx_text(cr, buf, x, y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		}
		y += 24;
		fmt_kb(B.self_rss_kb, a, sizeof a);
		snprintf(buf, sizeof buf, _("metrowl itself is using %s"), a);
		gfx_text(cr, buf, x, y, 20, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		y += 44;
		snprintf(buf, sizeof buf, _("%d processes · top %d by CPU"), B.nprocs, B.nrows);
		gfx_text(cr, i18n_upper(buf), x, y + 10, 13, FW_SEMIBOLD, th->accent, TA_LEFT, w, 1);
	}
	y = TOP;
	for (int i = 0; i < B.nrows; i++) {
		double ry = y + i * RH;
		if (ry + RH < off || ry > off + view.h) continue;
		const ProcRow *p = &B.rows[i];
		Rect rr = R(0, ry, view.w, RH);
		UiId id = ui_idn("bat-proc", p->pid);
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.14f));
		snprintf(buf, sizeof buf, "%.1f%%", p->cpu);
		Size cs = gfx_text_size(cr, "100.0%", 22, FW_SEMILIGHT, 0, 1);
		gfx_text(cr, buf, x + w, ry + 8, 22, FW_SEMILIGHT, p->cpu >= 10 ? th->accent : th->fg, TA_RIGHT, 0, 1);
		bool self = p->pid == getpid();
		gfx_text(cr, p->name, x, ry + 6, 24, FW_SEMILIGHT, self ? th->accent : th->fg, TA_LEFT, w - cs.w - 12, 1);
		fmt_kb(p->rss_kb, a, sizeof a);
		snprintf(buf, sizeof buf, _("%s · pid %d"), a, p->pid);
		gfx_text(cr, buf, x, ry + 38, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		if (ui_clicked(ui, id)) {
			g_sel_pid = p->pid;
			str_copy(g_sel_name, sizeof g_sel_name, p->name);
			g_dscroll = (Scroll){0};
			g_dbar = (AppBar){0};
			nav_push(&g_nav, ui, PG_PROC);
		}
	}
	ui_scroll_end(ui);
}

static void pivot_fn(UI *ui, int page, Rect r, void *ud) {
	if (page == 0) overview_page(ui, r);
	else usage_page(ui, r);
}

/* ------------------------------------------------------- process details -- */

static void kill_cb(int b, void *ud) {
	if (b != 0 || g_sel_pid <= 1 || g_sel_pid == getpid()) return;
	if (kill(g_sel_pid, SIGTERM) != 0) {
		char body[200];
		snprintf(body, sizeof body, _("couldn't end %s: %s"), g_sel_name, strerror(errno));
		shell_toast(_("Battery Saver"), body, _("Battery Saver"));
	} else {
		nav_pop(&g_nav, &sh.ui);
		request_sample(true);
	}
	shell_invalidate();
}

static void details_page(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect a = appbar_content(&g_dbar, area);
	double y = ui_page_header(ui, 14, a.y + 6, _("Battery Saver"), g_sel_name);
	Rect view = R(a.x, y + 8, a.w, rect_bottom(a) - y - 8);
	const ProcRow *p = find_proc(g_sel_pid);
	bool self = g_sel_pid == getpid();
	static double content_h = 600;
	ui_scroll_begin(ui, ui_id("bat-det"), &g_dscroll, view, content_h);
	cairo_translate(cr, view.x, view.y);
	double x = 24, w = view.w - 48;
	char v[256], path[64];
	y = 0;
	if (!p) {
		y += gfx_text(cr, _("this process has ended."), x, y, 20, FW_SEMILIGHT, th->sub, TA_LEFT, w, 0);
	} else {
		snprintf(v, sizeof v, "%d", p->pid);
		y += info_line(ui, x, y, w, _("process id"), v);
		const char *state;
		switch (p->state) {
		case 'R': state = _("running"); break;
		case 'S': case 'I': state = _("sleeping"); break;
		case 'D': state = _("waiting for the disk"); break;
		case 'T': case 't': state = _("stopped"); break;
		case 'Z': state = _("zombie"); break;
		default: state = "?"; break;
		}
		y += info_line(ui, x, y, w, _("status"), state);
		snprintf(v, sizeof v, "%.1f%%", p->cpu);
		y += info_line(ui, x, y, w, _("CPU"), v);
		fmt_kb(p->rss_kb, v, sizeof v);
		y += info_line(ui, x, y, w, _("memory"), v);
		snprintf(v, sizeof v, "%d", p->threads);
		y += info_line(ui, x, y, w, _("threads"), v);
		snprintf(v, sizeof v, "%d", p->uid);
		y += info_line(ui, x, y, w, _("user id"), v);
		snprintf(path, sizeof path, "/proc/%d/cmdline", p->pid);
		size_t len = 0;
		char *cl = file_read_all(path, &len);
		if (cl && len) {
			for (size_t i = 0; i + 1 < len; i++) if (!cl[i]) cl[i] = ' ';
			if (strlen(cl) > 600) cl[600] = 0;
			double h = gfx_text(cr, _("command line"), x, y, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
			y += h - 2 + gfx_text(cr, cl, x, y + h - 2, 17, FW_REGULAR, th->fg, TA_LEFT, w, 12) + 10;
		}
		free(cl);
		if (self) y += gfx_text(cr, _("this is metrowl itself, so it can't be ended here."), x, y + 6, 15, FW_REGULAR, th->sub,
		                        TA_LEFT, w, 0) + 6;
	}
	content_h = y + 30;
	ui_scroll_end(ui);
	AppBarBtn btns[] = {{ICON_CLOSE, _("end task"), !p || self || g_sel_pid <= 1}};
	if (appbar_draw(ui, &g_dbar, area, btns, 1, NULL, 0) == 0) {
		char body[200];
		snprintf(body, sizeof body, _("if you end %s, any unsaved data in it will be lost."), g_sel_name);
		dialog_message(_("end task?"), body, _("end task"), _("cancel"), kill_cb, NULL);
	}
}

/* ------------------------------------------------------------------ app ---- */

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	if (page == PG_PROC) { details_page(ui, area); return; }
	static const char *titles[2];
	titles[0] = _("overview");
	titles[1] = _("usage");
	app_pivot(ui, "bat-pivot", &g_pager, area, _("Battery Saver"), titles, 2, pivot_fn, NULL);
}

static void battery_draw(UI *ui, Rect area) {
	if (!g_timer) g_timer = timer_add(0, true, tick, NULL);
	if (!g_timer_armed) {
		g_timer_armed = true;
		timer_set(g_timer, 2000);
		if (!B.have || now_sec() - B.bat_t > 5 || (want_procs() && now_sec() - B.procs_t > 2.5)) request_sample(want_procs());
	}
	/* switching to the usage page starts sampling right away */
	if (want_procs() && !B.procs_t && !B.pending) request_sample(true);
	nav_draw(ui, &g_nav, area, page_fn, NULL);
}

static void battery_open(const char *arg) {
	if (arg && str_eq(arg, "usage")) ui_pager_goto(&g_pager, 1);
	g_timer_armed = false;
}

static bool battery_back(void) {
	if (nav_page(&g_nav) == PG_PROC && appbar_back(&g_dbar)) return true;
	return nav_pop(&g_nav, &sh.ui);
}

static void battery_close(void) {
	if (g_timer) { timer_free(g_timer); g_timer = NULL; }
	if (g_quick) { timer_free(g_quick); g_quick = NULL; }
	g_timer_armed = false;
	sample_free(B.prev);
	B.prev = NULL;
	free(B.all);
	B.all = NULL;
	B.nall = B.nrows = 0;
	B.procs_t = 0;
	B.procs_ready = false;
	nav_init(&g_nav, PG_MAIN);
	g_pager = (Pager){0};
}

static void battery_init(void) {
	nav_init(&g_nav, PG_MAIN);
	/* low battery check, cheap: reads the cached sys_battery() state */
	g_auto_timer = timer_add(60000, true, auto_check, NULL);
}

/* ------------------------------------------------------------- live tile -- */

static void battery_tile(UI *ui, Rect r, TileSize size, double now) {
	cairo_t *cr = ui->cr;
	Battery b = sys_battery();
	Color fg = COL_WHITE;
	char buf[16];
	if (b.present) snprintf(buf, sizeof buf, _("%d%%"), b.pct);
	else str_copy(buf, sizeof buf, "AC");
	double big = size == TILE_WIDE ? MIN(r.h * 0.5, 72) : MIN(r.w * 0.36, 56);
	icon_draw(cr, ICON_LEAF, rect_right(r) - 26, r.y + 24, 26, cfg.battery_saver ? fg : col_alpha(fg, 0.55f));
	gfx_text(cr, buf, r.x + 10, r.y + r.h * 0.3 - big * 0.25, big, FW_LIGHT, fg, TA_LEFT, r.w - 20, 1);
	if (b.present && size == TILE_WIDE) {
		const char *s = b.full ? _("fully charged") : b.charging ? _("charging") : NULL;
		if (s) gfx_text(cr, s, r.x + 12, r.y + r.h * 0.3 + big * 0.85, 15, FW_REGULAR, fg, TA_LEFT, r.w - 24, 1);
	}
	if (cfg.battery_saver) gfx_text(cr, _("saver on"), r.x + 9, rect_bottom(r) - 24, 15, FW_REGULAR, fg, TA_LEFT, r.w - 18, 1);
	else app_tile_name(ui, r, tr("Battery Saver"));
}

const AppDef app_battery = {
	.id = "battery", .name = N_("Battery Saver"), .icon = ICON_LEAF, .color = 0,
	.keywords = "power battery saver task manager processes memory",
	.init = battery_init, .open = battery_open, .draw = battery_draw, .back = battery_back, .close = battery_close,
	.tile = battery_tile,
};

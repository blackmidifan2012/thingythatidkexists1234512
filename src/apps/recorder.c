/* Voice Recorder: records from the microphone with pw-record / parecord / arecord into
 * ~/Music/Recordings and plays the recordings back through the shared player. */
#include "apps.h"
#include "player.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { char *path, *name; time_t mtime; double dur; } Rec;

static VEC(Rec) g_recs;
static bool g_loaded;
static AppBar g_bar;
static Scroll g_scroll;
static int g_menu_i = -1;

static pid_t g_pid;
static bool g_stopping;
static double g_t0, g_stop_t, g_elapsed;
static char *g_rec_path;
static Timer *g_tick;

/* ------------------------------------------------------------------ list -- */

static char *rec_dir(void) { return path_join(home_dir(), "Music/Recordings"); }

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* duration from the RIFF header; falls back to the file size when the data size is unset */
static double wav_duration(const char *path, double fsize) {
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	unsigned char b[4096];
	size_t n = fread(b, 1, sizeof b, f);
	fclose(f);
	if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return 0;
	uint32_t rate = 0;
	for (size_t pos = 12; pos + 8 <= n;) {
		uint32_t sz = le32(b + pos + 4);
		if (!memcmp(b + pos, "fmt ", 4) && pos + 20 <= n) rate = le32(b + pos + 16);
		else if (!memcmp(b + pos, "data", 4)) {
			double avail = fsize - (double)(pos + 8);
			double data = (sz == 0 || sz == 0xFFFFFFFFu || sz > avail) ? avail : sz;
			return rate ? data / rate : 0;
		}
		pos += 8 + (size_t)sz + (sz & 1);
	}
	return 0;
}

static void list_free(void) {
	for (int i = 0; i < g_recs.len; i++) { free(g_recs.items[i].path); free(g_recs.items[i].name); }
	vec_free(&g_recs);
}

static int cmp_rec(const void *a, const void *b) {
	time_t x = ((const Rec *)a)->mtime, y = ((const Rec *)b)->mtime;
	return x < y ? 1 : x > y ? -1 : 0;
}

/* the recordings folder is small and app-owned, so it is read directly */
static void load_list(void) {
	list_free();
	g_loaded = true;
	char *dir = rec_dir();
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && g_recs.len < 1000) {
		if (e->d_name[0] == '.' || !str_ends(e->d_name, ".wav")) continue;
		char *p = path_join(dir, e->d_name);
		struct stat st;
		if (str_eq(p, g_rec_path ? g_rec_path : "") || stat(p, &st) != 0 || !S_ISREG(st.st_mode)) { free(p); continue; }
		Rec r = {p, xasprintf("%.*s", (int)(strlen(e->d_name) - 4), e->d_name), st.st_mtime,
		         wav_duration(p, (double)st.st_size)};
		vec_push(&g_recs, r);
	}
	if (d) closedir(d);
	free(dir);
	qsort(g_recs.items, g_recs.len, sizeof(Rec), cmp_rec);
}

/* ------------------------------------------------------------- recording -- */

static const char *recorder_prog(void) {
	static const char *progs[] = {"pw-record", "parecord", "arecord"};
	for (size_t i = 0; i < ARRAY_LEN(progs); i++)
		if (proc_exists(progs[i])) return progs[i];
	return NULL;
}

static void finished(void) {
	struct stat st;
	bool ok = g_rec_path && stat(g_rec_path, &st) == 0 && st.st_size > 1024;
	if (!ok) {
		if (g_rec_path) unlink(g_rec_path);
		shell_toast(_("couldn't record"), _("the microphone couldn't be opened."), _("Voice Recorder"));
	}
	free(g_rec_path);
	g_rec_path = NULL;
	g_pid = 0;
	g_stopping = false;
	timer_set(g_tick, 0);
	load_list();
}

static void tick_cb(void *ud) {
	int st;
	if (g_pid > 0 && waitpid(g_pid, &st, WNOHANG) == g_pid) finished();
	else if (g_stopping && now_sec() - g_stop_t > 3) kill(g_pid, SIGKILL);
	shell_invalidate();
}

static void start_record(void) {
	const char *prog = recorder_prog();
	if (!prog || g_pid > 0) return;
	char *dir = rec_dir();
	mkdir_p(dir);
	char name[96];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(name, sizeof name, "Recording %Y-%m-%d %H.%M.%S.wav", &tm);
	g_rec_path = path_join(dir, name);
	free(dir);
	if (player_owner() && str_eq(player_owner(), "recorder")) player_stop();
	else if (player_state() == PL_PLAYING) player_pause(true);
	char *pw[] = {"pw-record", g_rec_path, NULL};
	char *pa[] = {"parecord", "--file-format=wav", g_rec_path, NULL};
	char *al[] = {"arecord", "-q", "-f", "cd", "-t", "wav", g_rec_path, NULL};
	char **argv = str_eq(prog, "pw-record") ? pw : str_eq(prog, "parecord") ? pa : al;
	g_pid = audio_spawn(argv);
	if (g_pid <= 0) {
		g_pid = 0;
		free(g_rec_path);
		g_rec_path = NULL;
		shell_toast(_("couldn't record"), prog, _("Voice Recorder"));
		return;
	}
	g_t0 = now_sec();
	g_elapsed = 0;
	g_stopping = false;
	if (!g_tick) g_tick = timer_add(0, true, tick_cb, NULL);
	timer_set(g_tick, 250);
}

static void stop_record(void) {
	if (g_pid <= 0 || g_stopping) return;
	g_elapsed = now_sec() - g_t0;
	g_stopping = true;
	g_stop_t = now_sec();
	kill(g_pid, SIGINT);
}

/* --------------------------------------------------------------- actions -- */

static void fmt_dur(char *buf, size_t n, double sec) {
	int s = (int)MAX(sec, 0);
	if (s >= 3600) snprintf(buf, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

static bool is_current(const Rec *r) {
	return player_active() && player_path() && str_eq(player_path(), r->path);
}

static void play(const Rec *r) {
	if (is_current(r)) { player_toggle(); return; }
	if (!player_available() || !player_play(r->path)) {
		shell_toast(_("can't play the recording"), _("install mpv, ffplay or pw-play to play recordings."), _("Voice Recorder"));
		return;
	}
	player_set_owner("recorder");
	player_set_meta(r->name, tr(app_recorder.name));
	if (r->dur > 0) player_set_duration(r->dur);
}

static void release(const char *path) {
	if (player_path() && str_eq(player_path(), path)) player_stop();
}

static void delete_cb(int b, void *ud) {
	char *path = ud;
	if (b == 0) {
		release(path);
		if (unlink(path) != 0) shell_toast(_("couldn't delete the recording"), path_basename(path), _("Voice Recorder"));
		load_list();
	}
	free(path);
	shell_invalidate();
}

static void rename_cb(int b, void *ud) {
	char *path = ud;
	const char *txt = dialog_input_text();
	if (b == 0 && txt && *txt) {
		char name[200];
		str_copy(name, sizeof name, txt);
		for (char *c = name; *c; c++) if (*c == '/') *c = '-';
		char *dir = rec_dir(), *file = xasprintf("%s.wav", str_trim(name)), *dst = path_join(dir, file);
		if (!str_eq(dst, path)) {
			if (!*str_trim(name) || file_exists(dst)) shell_toast(_("couldn't rename the recording"), _("a recording with that name already exists."), _("Voice Recorder"));
			else {
				release(path);
				if (rename(path, dst) != 0) shell_toast(_("couldn't rename the recording"), path_basename(path), _("Voice Recorder"));
			}
		}
		free(dir);
		free(file);
		free(dst);
		load_list();
	}
	free(path);
	shell_invalidate();
}

static void menu_cb(int b, void *ud) {
	if (g_menu_i < 0 || g_menu_i >= g_recs.len) return;
	Rec *r = &g_recs.items[g_menu_i];
	if (b == 0) dialog_input(_("rename"), _("name"), r->name, 0, rename_cb, xstrdup(r->path));
	else if (b == 1) dialog_message(_("delete recording?"), r->name, _("delete"), _("cancel"), delete_cb, xstrdup(r->path));
}

/* ------------------------------------------------------------------ page -- */

static void recorder_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (!g_loaded) load_list();
	Rect a = appbar_content(&g_bar, area);
	double y = ui_page_header(ui, 14, a.y + 6, _("Voice Recorder"), _("recording"));
	const char *prog = recorder_prog();
	bool rec = g_pid > 0;
	Color red = col_from_u32(0xE51400);
	/* record button + elapsed time */
	double dia = 104, cx = a.x + a.w / 2, cy = y + 8 + dia / 2;
	UiId bid = ui_id("rec-btn");
	Rect br = R(cx - dia / 2, cy - dia / 2, dia, dia);
	ui_hit(ui, bid, br);
	Color bc = !prog ? th->dim : ui_pressed(ui, bid) ? col_mix(red, th->fg, 0.3f) : red;
	gfx_fill_circle(cr, cx, cy, dia / 2, bc);
	if (rec) gfx_fill_rect(cr, R(cx - 17, cy - 17, 34, 34), COL_WHITE);
	else gfx_fill_circle(cr, cx, cy, 18, COL_WHITE);
	if (ui_clicked(ui, bid)) {
		if (rec) stop_record();
		else if (prog) start_record();
	}
	y = cy + dia / 2 + 6;
	char t[32];
	fmt_dur(t, sizeof t, rec ? (g_stopping ? g_elapsed : now_sec() - g_t0) : 0);
	y += gfx_text(cr, t, cx, y, 64, FW_LIGHT, rec ? th->fg : th->sub, TA_CENTER, a.w - 48, 1);
	const char *status = !prog ? _("voice recording needs pw-record (PipeWire), parecord (PulseAudio) or arecord (ALSA). install one of them to record.")
	                     : g_stopping ? _("saving...") : rec ? _("recording... tap to stop") : _("tap to record");
	y += gfx_text(cr, status, prog ? cx : a.x + 24, y, 15, FW_REGULAR, th->sub, prog ? TA_CENTER : TA_LEFT, a.w - 48, 0) + 12;
	/* recordings */
	y = ui_section(ui, R(a.x + 24, y, a.w - 48, 28), _("recordings"));
	Rect view = R(a.x, y, a.w, rect_bottom(a) - y);
	if (!g_recs.len) app_empty(ui, R(a.x, y - 14, a.w, 60), _("no recordings yet"));
	const double RH = 72;
	double off = ui_scroll_begin(ui, ui_id("rec-list"), &g_scroll, view, g_recs.len * RH + 16);
	cairo_translate(cr, view.x, view.y);
	for (int i = 0; i < g_recs.len; i++) {
		double ry = i * RH;
		if (ry + RH < off || ry > off + view.h) continue;
		Rec *r = &g_recs.items[i];
		bool cur = is_current(r), playing = cur && player_state() == PL_PLAYING;
		char sub[96], dur[32] = "", pos[32];
		if (cur) {
			player_watch();
			fmt_dur(pos, sizeof pos, player_position());
			fmt_dur(dur, sizeof dur, r->dur > 0 ? r->dur : player_duration());
			snprintf(sub, sizeof sub, "%s / %s", pos, dur);
		} else {
			struct tm tm;
			char d[48], tt[32];
			localtime_r(&r->mtime, &tm);
			i18n_fmt_date_short(d, sizeof d, &tm);
			i18n_fmt_time(tt, sizeof tt, &tm, cfg.h24);
			snprintf(sub, sizeof sub, "%s %s", d, tt);
			if (r->dur > 0) fmt_dur(dur, sizeof dur, r->dur);
		}
		UiId id = ui_idn("rec-row", i);
		if (ui_row_ex(ui, id, R(0, ry, view.w, RH), r->name, sub, playing ? ICON_PAUSE : ICON_PLAY,
		              cur || !*dur ? NULL : dur, false))
			play(r);
		if (ui_longpressed(ui, id)) {
			static const char *items[2];
			items[0] = _("rename");
			items[1] = _("delete");
			g_menu_i = i;
			dialog_menu(ui->p.x, ui->p.y, items, 2, menu_cb, NULL);
		}
	}
	ui_scroll_end(ui);
	AppBarBtn btns[] = {{rec ? ICON_STOP : ICON_RECORD, rec ? _("stop") : _("record"), !prog}};
	if (appbar_draw(ui, &g_bar, area, btns, 1, NULL, 0) == 0) {
		if (rec) stop_record();
		else start_record();
	}
}

static void recorder_open(const char *arg) { load_list(); }

static bool recorder_back(void) { return appbar_back(&g_bar); }

static void recorder_close(void) {
	stop_record();   /* the timer keeps reaping it and saves the file */
	if (g_pid <= 0) { list_free(); g_loaded = false; }
	g_scroll = (Scroll){0};
	g_bar = (AppBar){0};
}

/* --------------------------------------------------------------- live tile */

static void recorder_tile(UI *ui, Rect r, TileSize size, double now) {
	cairo_t *cr = ui->cr;
	if (g_pid > 0 && size != TILE_SMALL) {
		char t[32];
		fmt_dur(t, sizeof t, g_stopping ? g_elapsed : now_sec() - g_t0);
		gfx_fill_circle(cr, r.x + 22, r.y + 22, 8, COL_WHITE);
		gfx_text(cr, t, r.x + 12, r.y + 36, MIN(48, r.h * 0.3), FW_LIGHT, COL_WHITE, TA_LEFT, r.w - 24, 1);
	} else icon_draw(cr, ICON_MIC, rect_cx(r), rect_cy(r) - (size == TILE_SMALL ? 0 : 8), MIN(r.w, r.h) * 0.36, COL_WHITE);
	if (size != TILE_SMALL) app_tile_name(ui, r, tr(app_recorder.name));
}

const AppDef app_recorder = {
	.id = "recorder", .name = N_("Voice Recorder"), .icon = ICON_MIC, .color = 0,
	.keywords = "voice recorder microphone audio record memo",
	.open = recorder_open, .draw = recorder_draw, .back = recorder_back, .close = recorder_close,
	.tile = recorder_tile,
};

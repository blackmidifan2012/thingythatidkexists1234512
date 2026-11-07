#include "apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <glib.h>
#include <math.h>

/* ---------------------------------------------------------------- page nav */

void nav_init(Nav *n, int root) {
	memset(n, 0, sizeof *n);
	n->stack[0] = root;
	anim_set(&n->anim, 1);
}

int nav_page(const Nav *n) { return n->stack[n->depth]; }

void nav_push(Nav *n, UI *ui, int page) {
	if (n->depth >= 7) return;
	n->from = n->stack[n->depth];
	n->stack[++n->depth] = page;
	n->to = page;
	n->dir = 1;
	anim_start(&n->anim, 0, 1, ui->now ? ui->now : now_sec(), cfg.animations ? 0.36 : 0.001, EASE_LINEAR);
	shell_invalidate();
}

bool nav_pop(Nav *n, UI *ui) {
	if (n->depth <= 0) return false;
	n->from = n->stack[n->depth];
	n->depth--;
	n->to = n->stack[n->depth];
	n->dir = -1;
	anim_start(&n->anim, 0, 1, ui->now ? ui->now : now_sec(), cfg.animations ? 0.36 : 0.001, EASE_LINEAR);
	shell_invalidate();
	return true;
}

bool nav_busy(const Nav *n, UI *ui) { return anim_running(&n->anim, ui->now); }

typedef struct { PageDrawFn fn; int page; Rect area; void *ud; } NavArg;

static void nav_snap_cb(UI *ui, void *arg) {
	NavArg *a = arg;
	gfx_fill_rect(ui->cr, R(0, 0, sh.w, sh.h), ui->th.bg);
	a->fn(ui, a->page, a->area, a->ud);
}

void nav_draw(UI *ui, Nav *n, Rect area, PageDrawFn fn, void *ud) {
	if (!nav_busy(n, ui)) {
		fn(ui, nav_page(n), area, ud);
		return;
	}
	double p = anim_value(&n->anim, ui->now);
	ui->animating = true;
	bool out_phase = p < 0.42;
	int page = out_phase ? n->from : n->to;
	double angle;
	if (out_phase) angle = ease(EASE_IN_QUAD, p / 0.42) * 76;
	else angle = (1 - ease(EASE_OUT_CUBIC, (p - 0.42) / 0.58)) * 76;
	angle *= (out_phase ? -1 : 1) * n->dir * -1;
	NavArg a = {fn, page, area, ud};
	cairo_surface_t *s = shell_draw_snapshot(nav_snap_cb, &a);
	cairo_save(ui->cr);
	cairo_scale(ui->cr, 1.0 / sh.scale, 1.0 / sh.scale);
	Rect pa = R(area.x * sh.scale, area.y * sh.scale, area.w * sh.scale, area.h * sh.scale);
	gfx_fill_rect(ui->cr, pa, ui->th.bg);
	ui_draw_turnstile(ui->cr, s, sh.pw, sh.ph, pa, angle, 0, 1.0);
	cairo_restore(ui->cr);
}

Rect app_page_rect(UI *ui, Rect area) {
	return R(area.x + 12, area.y, area.w - 24, area.h);
}

void app_empty(UI *ui, Rect r, const char *text) {
	gfx_text(ui->cr, text, r.x + 24, r.y + 14, 20, FW_SEMILIGHT, ui->th.sub, TA_LEFT, r.w - 48, 0);
}

/* ------------------------------------------------------------------ pivot - */

void app_pivot(UI *ui, const char *key, Pager *pg, Rect area, const char *app_title,
               const char *const *titles, int n, PivotPageFn fn, void *ud) {
	cairo_t *cr = ui->cr;
	double y = area.y + 8;
	if (app_title && *app_title)
		y += gfx_text(cr, i18n_upper(app_title), area.x + 18, y, 17, FW_SEMIBOLD, ui->th.fg, TA_LEFT, area.w - 36, 1) - 4;
	UiId id = ui_id(key);
	y = ui_pivot_headers(ui, id, pg, titles, n, area.x + 14, y, area.w - 14) + 6;
	Rect view = R(area.x, y, area.w, rect_bottom(area) - y);
	ui_pager_update(ui, id, pg, view, n);
	for (int p = 0; p < n; p++) {
		Rect pr;
		if (!ui_pager_page(pg, p, view, &pr)) continue;
		cairo_save(cr);
		cairo_translate(cr, pr.x - view.x, 0);
		ui_clip_push(ui, view);
		fn(ui, p, view, ud);
		ui_clip_pop(ui);
		cairo_restore(cr);
	}
	ui_pager_end(ui);
}

/* ------------------------------------------------------------- live tiles - */

void app_tile_name(UI *ui, Rect r, const char *name) {
	gfx_text(ui->cr, name, r.x + 9, rect_bottom(r) - 24, 15, FW_REGULAR, COL_WHITE, TA_LEFT, r.w - 18, 1);
}

void app_tile_faces(UI *ui, Rect r, const char *key, int nfaces, double period, TileFaceFn fn, void *ud) {
	if (nfaces <= 1 || period <= 0) { fn(ui, r, 0, ud); return; }
	UiId id = ui_id(key);
	double stagger = (id % 997) / 997.0 * period;
	int target = (int)fmod(floor((ui->now + stagger) / period), nfaces);
	double *st = ui_state(ui, id, -1);   /* [0] face shown, [1] time it changed */
	if (st[0] < 0 || st[0] >= nfaces) { st[0] = target; st[1] = -100; }
	if ((int)st[0] != target) {
		st[0] = target;
		st[1] = ui->now;
	}
	double p = (ui->now - st[1]) / 0.75;
	if (p >= 1 || !cfg.animations || cfg.battery_saver) { fn(ui, r, target, ud); return; }
	ui->animating = true;
	double e = ease(EASE_INOUT_CUBIC, p);
	int from = (target - 1 + nfaces) % nfaces;
	ui_clip_push(ui, r);
	cairo_save(ui->cr);
	cairo_translate(ui->cr, 0, -e * r.h);
	fn(ui, r, from, ud);
	cairo_restore(ui->cr);
	cairo_save(ui->cr);
	cairo_translate(ui->cr, 0, (1 - e) * r.h);
	fn(ui, r, target, ud);
	cairo_restore(ui->cr);
	ui_clip_pop(ui);
}

/* -------------------------------------------------------------- collation - */

static const char *TR_ALPHABET = "abcçdefgğhıijklmnoöprsştuüvyzqwx";

static uint32_t char_rank(uint32_t cp) {
	cp = g_unichar_tolower(cp);
	if (cp == 0x130) cp = 'i';
	if (i18n_lang() == LANG_TR) {
		int idx = 0;
		for (const char *p = TR_ALPHABET; *p; idx++) {
			uint32_t a;
			p += utf8_decode(p, &a);
			if (a == cp) return 0x100 + idx;
		}
	}
	char buf[8];
	utf8_encode(cp, buf);
	char *f = str_fold(buf);
	uint32_t r = (uint8_t)f[0];
	free(f);
	if (r >= 'a' && r <= 'z') return 0x100 + (r - 'a') * 2;
	if (r >= '0' && r <= '9') return 0x80 + (r - '0');
	return 0x200 + cp;
}

int app_collate(const char *a, const char *b) {
	if (!a) a = "";
	if (!b) b = "";
	while (*a && *b) {
		uint32_t ca, cb;
		a += utf8_decode(a, &ca);
		b += utf8_decode(b, &cb);
		uint32_t ra = char_rank(ca), rb = char_rank(cb);
		if (ra != rb) return ra < rb ? -1 : 1;
	}
	return *a ? 1 : *b ? -1 : 0;
}

void app_group_letter(const char *name, char out[8]) {
	uint32_t cp = 0;
	if (name) utf8_decode(name, &cp);
	if (!g_unichar_isalpha(cp)) { str_copy(out, 8, "#"); return; }
	char buf[8];
	utf8_encode(cp, buf);
	if (i18n_lang() != LANG_TR || !strstr(TR_ALPHABET, i18n_lower(buf))) {
		/* fold accents: "é" files under "e" */
		char *f = str_fold(buf);
		str_copy(buf, sizeof buf, f);
		free(f);
	}
	str_copy(out, 8, i18n_lower(buf));
}

/* --------------------------------------------------------------- jumplist - */

static void jump_letters(char (*letters)[8], int *n) {
	static const char *EN = "abcdefghijklmnopqrstuvwxyz";
	*n = 0;
	str_copy(letters[(*n)++], 8, "#");
	if (i18n_lang() == LANG_TR) {
		for (const char *p = "abcçdefgğhıijklmnoöprsştuüvyz"; *p && *n < 40;) {
			uint32_t cp;
			int k = utf8_decode(p, &cp);
			memcpy(letters[*n], p, (size_t)k);
			letters[(*n)++][k] = 0;
			p += k;
		}
	} else {
		for (const char *p = EN; *p; p++) { letters[*n][0] = *p; letters[(*n)++][1] = 0; }
	}
}

bool jumplist_back(JumpList *jl) {
	if (!jl->open) return false;
	jl->open = false;
	return true;
}

void jumplist_draw(UI *ui, JumpList *jl, const char *key, Rect view, const char (*letters)[8], int n,
                   double row_h, double top_h, JumpTopFn top, JumpRowFn row, void *ud) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	const double HDR = 60;
	double total = top_h;
	for (int i = 0; i < n; i++) total += ((i == 0 || strcmp(letters[i], letters[i - 1])) ? HDR : 0) + row_h;
	total += 24;
	char sk[96];
	snprintf(sk, sizeof sk, "%s-scroll", key);
	double off = ui_scroll_begin(ui, ui_id(sk), &jl->scroll, view, total);
	cairo_translate(cr, view.x, view.y);
	if (top) top(ui, R(0, 0, view.w, top_h), ud);
	double y = top_h;
	for (int i = 0; i < n; i++) {
		if (i == 0 || strcmp(letters[i], letters[i - 1])) {
			if (y + HDR > off && y < off + view.h) {
				Rect hr = R(24, y + 8, 46, 46);
				UiId hid = ui_idn(key, 100000 + i);
				ui_hit(ui, hid, hr);
				bool pr = ui_pressed(ui, hid);
				ui_letter_tile(ui, hr, letters[i], true, pr);
				if (ui_clicked(ui, hid)) {
					jl->open = true;
					anim_start(&jl->anim, 0, 1, ui->now, 0.22, EASE_OUT_CUBIC);
				}
			}
			y += HDR;
		}
		if (y + row_h > off && y < off + view.h) row(ui, i, R(0, y, view.w, row_h), ud);
		y += row_h;
	}
	ui_scroll_end(ui);

	if (!jl->open) return;
	/* jump grid over the whole list */
	double a = ui_anim(ui, &jl->anim);
	gfx_fill_rect(cr, view, col_alpha(th->bg, (float)(0.94 * a)));
	char bgk[96];
	snprintf(bgk, sizeof bgk, "%s-jumpbg", key);
	UiId bg = ui_id(bgk);
	ui_hit(ui, bg, view);
	if (ui_clicked(ui, bg)) jl->open = false;
	char all[48][8];
	int nl;
	jump_letters(all, &nl);
	int cols = 4;
	double gap = 10, cell = MIN(84, (view.w - 48 - (cols - 1) * gap) / cols);
	double gx = view.x + 24, gy = view.y + 24;
	for (int i = 0; i < nl; i++) {
		int r = i / cols, c = i % cols;
		Rect lr = R(gx + c * (cell + gap), gy + r * (cell + gap) + (1 - a) * 40, cell, cell);
		if (rect_bottom(lr) > rect_bottom(view)) break;
		double ty = top_h, found = -1;
		for (int k = 0; k < n; k++) {
			bool head = k == 0 || strcmp(letters[k], letters[k - 1]);
			if (head && !strcmp(letters[k], all[i])) { found = ty; break; }
			ty += (head ? HDR : 0) + row_h;
		}
		UiId lid = ui_idn(key, 200000 + i);
		if (found >= 0) ui_hit(ui, lid, lr);
		ui_letter_tile(ui, lr, all[i], found >= 0, found >= 0 && ui_pressed(ui, lid));
		if (found >= 0 && ui_clicked(ui, lid)) {
			ui_scroll_to(&jl->scroll, MIN(found, MAX(0, total - view.h)));
			jl->open = false;
		}
	}
}

/* ------------------------------------------------------------------ data -- */

char *app_data_file(const char *name) {
	char *dir = xdg_dir("XDG_DATA_HOME", ".local/share", "metrowl");
	char *p = path_join(dir, name);
	free(dir);
	return p;
}

cJSON *app_json_load(const char *name) {
	char *path = app_data_file(name);
	char *txt = file_read_all(path, NULL);
	free(path);
	if (!txt) return NULL;
	cJSON *j = cJSON_Parse(txt);
	free(txt);
	return j;
}

bool app_json_save(const char *name, const cJSON *root) {
	char *txt = cJSON_PrintUnformatted(root);
	if (!txt) return false;
	char *path = app_data_file(name);
	bool ok = file_write_all(path, txt, strlen(txt));
	if (!ok) LOGE("could not write %s", path);
	free(path);
	free(txt);
	return ok;
}

/* ----------------------------------------------------------------- sounds - */

static bool g_sound_loop;
static int g_sound_kind = -1;
static const char *g_player;

static char *sound_file(int kind) {
	static const char *names[] = {"alarm.wav", "chime.wav", "timer.wav"};
	char *dir = xdg_dir("XDG_CACHE_HOME", ".cache", "metrowl");
	char *path = path_join(dir, names[kind]);
	free(dir);
	if (file_exists(path)) return path;
	/* 16 bit mono PCM tones: a rising two-tone pattern for alarms, a soft chime otherwise */
	const int rate = 22050;
	static const struct { double f, start, len; } NOTES[3][6] = {
		{{880, 0.0, 0.16}, {1175, 0.2, 0.16}, {880, 0.4, 0.16}, {1175, 0.6, 0.16}, {0, 0, 0}},
		{{1046.5, 0.0, 0.5}, {1568, 0.14, 0.6}, {0, 0, 0}},
		{{988, 0.0, 0.12}, {988, 0.18, 0.12}, {988, 0.36, 0.12}, {0, 0, 0}},
	};
	double dur = kind == SND_ALARM ? 1.4 : kind == SND_CHIME ? 0.9 : 1.0;
	int ns = (int)(dur * rate);
	size_t bytes = 44 + (size_t)ns * 2;
	unsigned char *w = xcalloc(1, bytes);
	int16_t *pcm = (int16_t *)(w + 44);
	for (int k = 0; k < 6 && NOTES[kind][k].f > 0; k++) {
		int s0 = (int)(NOTES[kind][k].start * rate), sl = (int)(NOTES[kind][k].len * rate);
		for (int i = 0; i < sl && s0 + i < ns; i++) {
			double t = (double)i / rate, env = MIN(1.0, t / 0.01) * exp(-t * (kind == SND_CHIME ? 6 : 3));
			double v = sin(2 * M_PI * NOTES[kind][k].f * t) * 0.6 + sin(4 * M_PI * NOTES[kind][k].f * t) * 0.15;
			int s = pcm[s0 + i] + (int)(v * env * 9000);
			pcm[s0 + i] = (int16_t)CLAMP(s, -32000, 32000);
		}
	}
	uint32_t u32;
	uint16_t u16;
	memcpy(w, "RIFF", 4); u32 = (uint32_t)(bytes - 8); memcpy(w + 4, &u32, 4);
	memcpy(w + 8, "WAVEfmt ", 8); u32 = 16; memcpy(w + 16, &u32, 4);
	u16 = 1; memcpy(w + 20, &u16, 2); memcpy(w + 22, &u16, 2);
	u32 = rate; memcpy(w + 24, &u32, 4); u32 = rate * 2; memcpy(w + 28, &u32, 4);
	u16 = 2; memcpy(w + 32, &u16, 2); u16 = 16; memcpy(w + 34, &u16, 2);
	memcpy(w + 36, "data", 4); u32 = (uint32_t)ns * 2; memcpy(w + 40, &u32, 4);
	file_write_all(path, w, bytes);
	free(w);
	return path;
}

static void sound_done(int status, char *out, size_t len, void *ud);

static void sound_start(int kind) {
	if (!g_player) {
		static const char *players[] = {"pw-play", "paplay", "aplay"};
		for (size_t i = 0; i < ARRAY_LEN(players) && !g_player; i++)
			if (proc_exists(players[i])) g_player = players[i];
		if (!g_player) g_player = "";
	}
	if (!*g_player) return;
	char *path = sound_file(kind);
	char *argv[] = {(char *)g_player, str_eq(g_player, "aplay") ? "-q" : path, str_eq(g_player, "aplay") ? path : NULL, NULL};
	if (!proc_run(argv, sound_done, NULL)) g_sound_loop = false;
	free(path);
}

static void sound_done(int status, char *out, size_t len, void *ud) {
	if (g_sound_loop && status == 0) sound_start(g_sound_kind);
}

void app_sound(int kind, bool loop) {
	bool was_looping = g_sound_loop;
	g_sound_kind = kind;
	g_sound_loop = loop;
	if (!(was_looping && loop)) sound_start(kind);
}

void app_sound_stop(void) { g_sound_loop = false; }

/* ------------------------------------------------------------ image cache - */

typedef struct {
	char *path;
	int max_dim;
	Image *img;
	bool loading, failed;
	uint64_t used;
} CEntry;

struct ImgCache {
	CEntry *e;
	int n, max;
	uint64_t tick;
	uint32_t gen;
};

ImgCache *imgcache_new(int max_entries) {
	ImgCache *c = xcalloc(1, sizeof *c);
	c->max = max_entries;
	c->e = xcalloc((size_t)max_entries + 8, sizeof(CEntry));
	return c;
}

void imgcache_clear(ImgCache *c) {
	for (int i = 0; i < c->n; i++) { free(c->e[i].path); image_free(c->e[i].img); }
	c->n = 0;
	c->gen++;
}

typedef struct { ImgCache *c; char *path; int max_dim; Image *img; uint32_t gen; } LoadJob;

/* JPEG EXIF orientation (1..8) or 1 */
static int exif_orientation(const unsigned char *d, size_t n) {
	if (n < 12 || d[0] != 0xFF || d[1] != 0xD8) return 1;
	size_t i = 2;
	while (i + 4 < n && d[i] == 0xFF) {
		unsigned char m = d[i + 1];
		size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
		if (m == 0xE1 && i + 10 < n && !memcmp(d + i + 4, "Exif\0\0", 6)) {
			const unsigned char *t = d + i + 10;
			size_t tn = n - (i + 10);
			if (tn < 8) return 1;
			bool le = t[0] == 'I';
#define RD16(p) (le ? (uint16_t)(t[p] | (t[(p) + 1] << 8)) : (uint16_t)((t[p] << 8) | t[(p) + 1]))
#define RD32(p) (le ? (uint32_t)(t[p] | (t[(p)+1] << 8) | (t[(p)+2] << 16) | ((uint32_t)t[(p)+3] << 24)) : (uint32_t)(((uint32_t)t[p] << 24) | (t[(p)+1] << 16) | (t[(p)+2] << 8) | t[(p)+3]))
			uint32_t ifd = RD32(4);
			if (ifd + 2 > tn) return 1;
			uint16_t cnt = RD16(ifd);
			for (int k = 0; k < cnt; k++) {
				size_t e = ifd + 2 + (size_t)k * 12;
				if (e + 12 > tn) break;
				if (RD16(e) == 0x0112) return CLAMP((int)RD16(e + 8), 1, 8);
			}
			return 1;
		}
		if (m == 0xDA) break;
		i += 2 + len;
	}
	return 1;
}

static Image *apply_orientation(Image *img, int o) {
	if (!img || o <= 1) return img;
	int w = img->w, h = img->h;
	bool swap = o >= 5;
	int nw = swap ? h : w, nh = swap ? w : h;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
	cairo_t *cr = cairo_create(s);
	cairo_matrix_t m;
	switch (o) {
	case 2: cairo_matrix_init(&m, -1, 0, 0, 1, w, 0); break;
	case 3: cairo_matrix_init(&m, -1, 0, 0, -1, w, h); break;
	case 4: cairo_matrix_init(&m, 1, 0, 0, -1, 0, h); break;
	case 5: cairo_matrix_init(&m, 0, 1, 1, 0, 0, 0); break;
	case 6: cairo_matrix_init(&m, 0, 1, -1, 0, h, 0); break;
	case 7: cairo_matrix_init(&m, 0, -1, -1, 0, h, w); break;
	default: cairo_matrix_init(&m, 0, -1, 1, 0, 0, w); break;   /* 8 */
	}
	cairo_set_matrix(cr, &m);
	cairo_set_source_surface(cr, img->surf, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	image_free(img);
	return image_from_surface(s);
}

static void load_run(void *arg) {
	LoadJob *j = arg;
	size_t len = 0;
	unsigned char *data = (unsigned char *)file_read_all(j->path, &len);
	if (!data) return;
	if (str_ends(j->path, ".svg")) j->img = image_load_icon(j->path, j->max_dim);
	else {
		j->img = image_load_mem(data, len, j->max_dim);
		j->img = apply_orientation(j->img, exif_orientation(data, len));
	}
	free(data);
}

static void load_done(void *arg) {
	LoadJob *j = arg;
	ImgCache *c = j->c;
	if (j->gen == c->gen) {
		for (int i = 0; i < c->n; i++) {
			if (c->e[i].loading && str_eq(c->e[i].path, j->path) && c->e[i].max_dim == j->max_dim) {
				c->e[i].loading = false;
				c->e[i].img = j->img;
				c->e[i].failed = j->img == NULL;
				j->img = NULL;
				break;
			}
		}
	}
	image_free(j->img);
	free(j->path);
	free(j);
	shell_invalidate();
}

Image *imgcache_get(ImgCache *c, const char *path, int max_dim) {
	if (!path || !*path) return NULL;
	for (int i = 0; i < c->n; i++) {
		CEntry *e = &c->e[i];
		if (e->max_dim == max_dim && str_eq(e->path, path)) { e->used = ++c->tick; return e->img; }
	}
	if (c->n >= c->max) {
		int victim = -1;
		for (int i = 0; i < c->n; i++)
			if (!c->e[i].loading && (victim < 0 || c->e[i].used < c->e[victim].used)) victim = i;
		if (victim < 0) return NULL;
		free(c->e[victim].path);
		image_free(c->e[victim].img);
		c->e[victim] = c->e[--c->n];
	}
	CEntry *e = &c->e[c->n++];
	*e = (CEntry){.path = xstrdup(path), .max_dim = max_dim, .loading = true, .used = ++c->tick};
	LoadJob *j = xcalloc(1, sizeof *j);
	j->c = c;
	j->path = xstrdup(path);
	j->max_dim = max_dim;
	j->gen = c->gen;
	job_submit(load_run, load_done, j);
	return NULL;
}

/* -------------------------------------------------------- launcher "apps" - */

static bool launch_first(const char *const *ids, const char *what) {
	for (int i = 0; ids[i]; i++) {
		DesktopApp *a = desktop_find(ids[i]);
		if (a) {
			char key[200];
			snprintf(key, sizeof key, "d:%s", a->id);
			shell_launch(key);
			return true;
		}
	}
	shell_toast(what, _("No app installed for this. Get one from the Store."), NULL);
	return true;
}

static bool internet_launch(void) {
	DesktopApp *b = desktop_default_browser();
	if (b) {
		char key[200];
		snprintf(key, sizeof key, "d:%s", b->id);
		shell_launch(key);
	} else {
		shell_toast(_("Internet"), _("No web browser installed. Get one from the Store."), NULL);
	}
	return true;
}

static bool camera_launch(void) {
	static const char *ids[] = {"org.gnome.Snapshot", "org.gnome.Cheese", "cheese", "org.kde.kamoso", "megapixels",
		"org.postmarketos.Megapixels", "snapshot", "org.kde.kamera", NULL};
	return launch_first(ids, _("Camera"));
}

const AppDef app_internet = {
	.id = "internet", .name = N_("Internet"), .icon = ICON_GLOBE, .color = 0, .keywords = "web browser www",
	.launcher = internet_launch,
};

const AppDef app_camera = {
	.id = "camera", .name = N_("Camera"), .icon = ICON_CAMERA, .color = 0, .keywords = "photo video webcam",
	.launcher = camera_launch,
};

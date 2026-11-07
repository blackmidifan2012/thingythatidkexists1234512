/* People: the Windows Phone 8.1 People hub. Contacts with a jump list and search,
 * favourites, recently viewed, profile cards with call/text/mail/map actions, an editor,
 * vCard import/export and the photo mosaic live tile. */
#include "apps.h"
#include "../config.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ model - */

enum { F_FIRST, F_LAST, F_NICK, F_COMPANY, F_TITLE, F_WEB, F_BDAY, F_NOTES, F_PHOTO, NF };
static const char *const FKEY[NF] = {"first", "last", "nickname", "company", "title", "website", "birthday", "notes", "photo"};
enum { K_PHONE, K_EMAIL, K_ADDR, NK };
static const char *const KKEY[NK] = {"phones", "emails", "addresses"};
#define MAXI 8
#define NTYPES 4
static const char *const TKEY[NK][NTYPES] = {
	{"mobile", "home", "work", "other"},
	{"personal", "work", "other", NULL},
	{"home", "work", "other", NULL},
};
static const char *const TLABEL[NK][NTYPES] = {
	{N_("mobile"), N_("home"), N_("work"), N_("other")},
	{N_("personal"), N_("work"), N_("other"), NULL},
	{N_("home"), N_("work"), N_("other"), NULL},
};
static const char *const CALL_LABEL[NTYPES] = {N_("call mobile"), N_("call home"), N_("call work"), N_("call")};
static const char *const MAP_LABEL[3] = {N_("map home address"), N_("map work address"), N_("map address")};
static const char *const KIND_LABEL[NK] = {N_("phone"), N_("email"), N_("address")};
static const char *const ADD_LABEL[NK] = {N_("add phone"), N_("add email"), N_("add address")};
static int ntypes(int k) { return k == K_PHONE ? 4 : 3; }

typedef struct { int type; char *val; } Item;
typedef struct Contact {
	char *f[NF];
	Item it[NK][MAXI];
	int n[NK];
	bool fav;
	double viewed;
	unsigned id;
	/* derived from the name settings */
	char *disp, *sortkey;
	char letter[8], initials[16];
} Contact;

static VEC(Contact *) g_c;
static VEC(int) g_order;          /* all contacts, sorted */
static VEC(int) g_view;           /* filtered by the search box */
static char (*g_letters)[8];
static int g_letters_cap;
static bool g_loaded, g_open, g_dirty = true;
static unsigned g_next_id = 1;
static Lang g_names_lang = LANG_COUNT;
static bool g_sort_last, g_disp_last;
static char g_query[128], g_query_used[128];
static Timer *g_save_timer;
static ImgCache *g_img;

static const char *cf(const Contact *c, int f) { return c->f[f] ? c->f[f] : ""; }

static void cset(Contact *c, int f, const char *v) {
	free(c->f[f]);
	c->f[f] = NULL;
	if (!v) return;
	char *d = xstrdup(v);
	char *t = str_trim(d);
	if (*t) c->f[f] = xstrdup(t);
	free(d);
}

static void item_add(Contact *c, int k, int type, const char *v) {
	if (!v || c->n[k] >= MAXI) return;
	char *d = xstrdup(v);
	char *t = str_trim(d);
	if (*t) c->it[k][c->n[k]++] = (Item){CLAMP(type, 0, ntypes(k) - 1), xstrdup(t)};
	free(d);
}

static void items_clear(Contact *c) {
	for (int k = 0; k < NK; k++) {
		for (int i = 0; i < c->n[k]; i++) free(c->it[k][i].val);
		c->n[k] = 0;
	}
}

static void contact_free(Contact *c) {
	if (!c) return;
	for (int f = 0; f < NF; f++) free(c->f[f]);
	items_clear(c);
	free(c->disp);
	free(c->sortkey);
	free(c);
}

static Contact *contact_dup(const Contact *s) {
	Contact *c = xcalloc(1, sizeof *c);
	for (int f = 0; f < NF; f++) c->f[f] = s->f[f] ? xstrdup(s->f[f]) : NULL;
	for (int k = 0; k < NK; k++)
		for (int i = 0; i < s->n[k]; i++) item_add(c, k, s->it[k][i].type, s->it[k][i].val);
	c->fav = s->fav;
	c->id = s->id;
	return c;
}

static bool contact_empty(const Contact *c) {
	for (int f = 0; f < NF; f++) if (c->f[f] && f != F_PHOTO) return false;
	return !c->n[K_PHONE] && !c->n[K_EMAIL] && !c->n[K_ADDR];
}

static int type_from_key(int k, const char *s) {
	for (int t = 0; t < ntypes(k); t++) if (s && str_eq(TKEY[k][t], s)) return t;
	return k == K_PHONE ? 3 : 2;
}

/* first character of s (upper case) appended to out */
static void add_initial(char *out, size_t cap, const char *s) {
	if (!s || !*s) return;
	uint32_t cp;
	int n = utf8_decode(s, &cp);
	if (!g_unichar_isalnum(cp)) return;
	char one[8];
	memcpy(one, s, (size_t)n);
	one[n] = 0;
	size_t l = strlen(out);
	str_copy(out + l, cap - l, i18n_upper(one));
}

static void contact_names(Contact *c) {
	const char *fi = cf(c, F_FIRST), *la = cf(c, F_LAST);
	char *d;
	if (*fi && *la) d = g_disp_last ? xasprintf("%s, %s", la, fi) : xasprintf("%s %s", fi, la);
	else if (*fi || *la) d = xstrdup(*fi ? fi : la);
	else if (c->f[F_NICK]) d = xstrdup(c->f[F_NICK]);
	else if (c->f[F_COMPANY]) d = xstrdup(c->f[F_COMPANY]);
	else if (c->n[K_PHONE]) d = xstrdup(c->it[K_PHONE][0].val);
	else if (c->n[K_EMAIL]) d = xstrdup(c->it[K_EMAIL][0].val);
	else d = xstrdup(_("unnamed"));
	free(c->disp);
	c->disp = d;
	free(c->sortkey);
	if (*fi && *la) c->sortkey = g_sort_last ? xasprintf("%s %s", la, fi) : xasprintf("%s %s", fi, la);
	else c->sortkey = xstrdup(d);
	app_group_letter(c->sortkey, c->letter);
	c->initials[0] = 0;
	if (*fi || *la) {
		add_initial(c->initials, sizeof c->initials, fi);
		add_initial(c->initials, sizeof c->initials, la);
	}
	if (!c->initials[0]) add_initial(c->initials, sizeof c->initials, d);
	if (!c->initials[0]) str_copy(c->initials, sizeof c->initials, "#");
}

/* ---------------------------------------------------------------- storage - */

static void load_settings(void) {
	cJSON *s = config_section("people", false);
	g_sort_last = cJSON_IsTrue(cJSON_GetObjectItem(s, "sort_last"));
	g_disp_last = cJSON_IsTrue(cJSON_GetObjectItem(s, "display_last"));
}

static void save_settings(void) {
	cJSON *s = config_section("people", false);
	cJSON_DeleteItemFromObject(s, "sort_last");
	cJSON_DeleteItemFromObject(s, "display_last");
	cJSON_AddBoolToObject(s, "sort_last", g_sort_last);
	cJSON_AddBoolToObject(s, "display_last", g_disp_last);
	config_save_soon();
}

static const char *jstr(const cJSON *o, const char *k) {
	const cJSON *v = cJSON_GetObjectItem(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void load(void) {
	if (g_loaded) return;
	g_loaded = true;
	g_dirty = true;
	load_settings();
	if (!g_img) g_img = imgcache_new(48);
	cJSON *root = app_json_load("contacts.json");
	cJSON *arr = cJSON_GetObjectItem(root, "contacts");
	cJSON *o;
	cJSON_ArrayForEach(o, arr) {
		Contact *c = xcalloc(1, sizeof *c);
		for (int f = 0; f < NF; f++) cset(c, f, jstr(o, FKEY[f]));
		for (int k = 0; k < NK; k++) {
			cJSON *it;
			cJSON_ArrayForEach(it, cJSON_GetObjectItem(o, KKEY[k]))
				item_add(c, k, type_from_key(k, jstr(it, "type")), jstr(it, "value"));
		}
		c->fav = cJSON_IsTrue(cJSON_GetObjectItem(o, "favorite"));
		cJSON *v = cJSON_GetObjectItem(o, "viewed");
		c->viewed = cJSON_IsNumber(v) ? v->valuedouble : 0;
		v = cJSON_GetObjectItem(o, "id");
		c->id = cJSON_IsNumber(v) && v->valuedouble > 0 ? (unsigned)v->valuedouble : 0;
		if (contact_empty(c)) { contact_free(c); continue; }
		vec_push(&g_c, c);
	}
	cJSON_Delete(root);
	/* ids must be unique */
	for (int i = 0; i < g_c.len; i++) if (g_c.items[i]->id >= g_next_id) g_next_id = g_c.items[i]->id + 1;
	for (int i = 0; i < g_c.len; i++) {
		Contact *c = g_c.items[i];
		bool clash = !c->id;
		for (int j = 0; j < i && !clash; j++) clash = g_c.items[j]->id == c->id;
		if (clash) c->id = g_next_id++;
	}
}

static void tile_cache_update(void);

static void save_now(void) {
	if (!g_loaded) return;
	if (g_save_timer) timer_set(g_save_timer, 0);
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", 1);
	cJSON *arr = cJSON_AddArrayToObject(root, "contacts");
	for (int i = 0; i < g_c.len; i++) {
		Contact *c = g_c.items[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", c->id);
		for (int f = 0; f < NF; f++) if (c->f[f]) cJSON_AddStringToObject(o, FKEY[f], c->f[f]);
		for (int k = 0; k < NK; k++) {
			if (!c->n[k]) continue;
			cJSON *a = cJSON_AddArrayToObject(o, KKEY[k]);
			for (int j = 0; j < c->n[k]; j++) {
				cJSON *it = cJSON_CreateObject();
				cJSON_AddStringToObject(it, "type", TKEY[k][c->it[k][j].type]);
				cJSON_AddStringToObject(it, "value", c->it[k][j].val);
				cJSON_AddItemToArray(a, it);
			}
		}
		if (c->fav) cJSON_AddBoolToObject(o, "favorite", true);
		if (c->viewed > 0) cJSON_AddNumberToObject(o, "viewed", c->viewed);
		cJSON_AddItemToArray(arr, o);
	}
	app_json_save("contacts.json", root);
	cJSON_Delete(root);
	tile_cache_update();
}

static void save_timer_cb(void *ud) { save_now(); }

static void save_soon(void) {
	if (!g_save_timer) g_save_timer = timer_add(1500, false, save_timer_cb, NULL);
	else timer_set(g_save_timer, 1500);
}

static void changed(void) {
	g_dirty = true;
	shell_invalidate();
}

static void unload(void) {
	for (int i = 0; i < g_c.len; i++) contact_free(g_c.items[i]);
	vec_free(&g_c);
	vec_free(&g_order);
	vec_free(&g_view);
	free(g_letters);
	g_letters = NULL;
	g_letters_cap = 0;
	g_loaded = false;
	g_dirty = true;
	if (g_img) imgcache_clear(g_img);
}

/* ------------------------------------------------------- sorting / search - */

static int cmp_order(const void *a, const void *b) {
	const Contact *x = g_c.items[*(const int *)a], *y = g_c.items[*(const int *)b];
	bool hx = x->letter[0] == '#', hy = y->letter[0] == '#';
	if (hx != hy) return hx ? -1 : 1;
	int r = app_collate(x->sortkey, y->sortkey);
	if (!r) r = app_collate(x->disp, y->disp);
	return r ? r : (x->id < y->id ? -1 : 1);
}

static void digits_of(const char *s, char *out, size_t cap) {
	size_t n = 0;
	for (; *s && n + 1 < cap; s++) if (isdigit((unsigned char)*s)) out[n++] = *s;
	out[n] = 0;
}

static bool contact_matches(const Contact *c, const char *q) {
	if (str_contains_fold(c->disp, q)) return true;
	for (int f = F_FIRST; f <= F_TITLE; f++) if (c->f[f] && str_contains_fold(c->f[f], q)) return true;
	for (int i = 0; i < c->n[K_EMAIL]; i++) if (str_contains_fold(c->it[K_EMAIL][i].val, q)) return true;
	char qd[64], pd[64];
	digits_of(q, qd, sizeof qd);
	if (strlen(qd) >= 3) {
		for (int i = 0; i < c->n[K_PHONE]; i++) {
			digits_of(c->it[K_PHONE][i].val, pd, sizeof pd);
			if (strstr(pd, qd)) return true;
		}
	}
	return false;
}

static void rebuild(void) {
	if (g_names_lang != i18n_lang()) { g_names_lang = i18n_lang(); g_dirty = true; }
	if (!g_dirty && str_eq(g_query, g_query_used)) return;
	if (g_dirty) {
		for (int i = 0; i < g_c.len; i++) contact_names(g_c.items[i]);
		g_order.len = 0;
		for (int i = 0; i < g_c.len; i++) vec_push(&g_order, i);
		if (g_order.len) qsort(g_order.items, (size_t)g_order.len, sizeof(int), cmp_order);
		g_dirty = false;
	}
	str_copy(g_query_used, sizeof g_query_used, g_query);
	g_view.len = 0;
	char *q = str_trim(xstrdup(g_query)), *qa = q;
	for (int i = 0; i < g_order.len; i++)
		if (!*q || contact_matches(g_c.items[g_order.items[i]], q)) vec_push(&g_view, g_order.items[i]);
	free(qa);
	if (g_view.len > g_letters_cap) {
		g_letters_cap = g_view.len + 16;
		g_letters = xrealloc(g_letters, (size_t)g_letters_cap * sizeof *g_letters);
	}
	for (int i = 0; i < g_view.len; i++) memcpy(g_letters[i], g_c.items[g_view.items[i]]->letter, 8);
}

static int contact_index(const Contact *c) {
	for (int i = 0; i < g_c.len; i++) if (g_c.items[i] == c) return i;
	return -1;
}

/* ------------------------------------------------------------ live tile -- */

/* The Start tile only needs initials and photo paths, kept in the config so the
 * contact database is not loaded at startup. */
static void tile_cache_update(void) {
	rebuild();
	cJSON *sec = config_section("people", false);
	cJSON *arr = cJSON_CreateArray();
	int cnt = 0;
	bool *used = xcalloc((size_t)g_c.len + 1, 1);
	for (int pass = 0; pass < 4 && cnt < 18; pass++) {
		for (int k = 0; k < g_order.len && cnt < 18; k++) {
			int i = g_order.items[k];
			Contact *c = g_c.items[i];
			bool photo = c->f[F_PHOTO] != NULL;
			bool ok = pass == 0 ? c->fav && photo : pass == 1 ? c->fav : pass == 2 ? photo : true;
			if (used[i] || !ok) continue;
			used[i] = true;
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "i", c->initials);
			if (photo) cJSON_AddStringToObject(o, "p", c->f[F_PHOTO]);
			cJSON_AddItemToArray(arr, o);
			cnt++;
		}
	}
	free(used);
	cJSON *old = cJSON_GetObjectItem(sec, "tile");
	if (old && cJSON_Compare(old, arr, true)) { cJSON_Delete(arr); return; }
	cJSON_DeleteItemFromObject(sec, "tile");
	cJSON_AddItemToObject(sec, "tile", arr);
	config_save_soon();
}

static ImgCache *g_tile_img;
static bool g_tile_checked;

static uint32_t mix32(uint32_t h) {
	h ^= h >> 16; h *= 0x7feb352d; h ^= h >> 15; h *= 0x846ca68b; h ^= h >> 16;
	return h;
}

static int cell_hit(long s, int nc) { return (int)(mix32((uint32_t)s * 2654435761u + 17) % (uint32_t)nc); }

/* contact shown by cell c at flip step s */
static int cell_content(int c, long s, int nc, int n) {
	for (long k = s; k > s - 48; k--)
		if (cell_hit(k, nc) == c) return (int)(((k + nc) % n + n) % n);   /* walks through the list */
	return c % n;
}

static void tile_cell(UI *ui, Rect r, cJSON *arr, int idx, int cell) {
	cairo_t *cr = ui->cr;
	gfx_fill_rect(cr, r, col_alpha(COL_BLACK, 0.06f * (float)((cell * 7) % 4)));
	if (idx < 0) return;
	cJSON *o = cJSON_GetArrayItem(arr, idx);
	const char *p = jstr(o, "p"), *in = jstr(o, "i");
	Image *img = NULL;
	if (p) {
		if (!g_tile_img) g_tile_img = imgcache_new(20);
		img = imgcache_get(g_tile_img, p, 160);
	}
	if (img) { gfx_image_cover(cr, img, r, 1); return; }
	double fs = r.h * 0.36;
	gfx_text(cr, in ? in : "", rect_cx(r), rect_cy(r) - gfx_line_height(cr, fs, FW_SEMILIGHT) / 2, fs, FW_SEMILIGHT,
	         col_alpha(COL_WHITE, 0.9f), TA_CENTER, r.w, 1);
}

static void people_tile(UI *ui, Rect r, TileSize size, double now) {
	cairo_t *cr = ui->cr;
	cJSON *sec = config_section("people", false);
	cJSON *arr = cJSON_GetObjectItem(sec, "tile");
	if (!arr && !g_tile_checked) {
		/* first run with existing data: build the cache once */
		g_tile_checked = true;
		char *path = app_data_file("contacts.json");
		bool have = file_exists(path);
		free(path);
		if (have) {
			bool was = g_loaded;
			load();
			tile_cache_update();
			if (!was && !g_open) unload();
			arr = cJSON_GetObjectItem(sec, "tile");
		}
	}
	int n = cJSON_GetArraySize(arr);
	if (n == 0) {
		icon_draw(cr, ICON_PEOPLE, rect_cx(r), r.y + r.h * 0.46, MIN(r.h * 0.42, 70) * 1.1, COL_WHITE);
		app_tile_name(ui, r, _("People"));
		return;
	}
	int cols = size == TILE_WIDE ? 6 : 3, rows = 3, nc = cols * rows;
	double cw = r.w / cols, ch = r.h / rows;
	const double P = 2.6, FLIP = 0.7;
	long s = (long)floor(now / P);
	double t = now - (double)s * P;
	bool anim = n > nc && cfg.animations && !cfg.battery_saver;
	for (int c = 0; c < nc; c++) {
		double x0 = floor(r.x + (c % cols) * cw), y0 = floor(r.y + (c / cols) * ch);
		double x1 = floor(r.x + (c % cols + 1) * cw), y1 = floor(r.y + (c / cols + 1) * ch);
		if (c % cols == cols - 1) x1 = rect_right(r);
		if (c / cols == rows - 1) y1 = rect_bottom(r);
		Rect cr_ = R(x0, y0, x1 - x0, y1 - y0);
		int cur;
		if (n <= nc) cur = (c * 7) % nc < n ? (c * 7) % nc : -1;   /* scattered, rest blank */
		else cur = cell_content(c, s, nc, n);
		ui_clip_push(ui, cr_);
		if (anim && cell_hit(s, nc) == c && t < FLIP) {
			double e = ease(EASE_INOUT_CUBIC, t / FLIP);
			int show = e < 0.5 ? cell_content(c, s - 1, nc, n) : cur;
			double sy = MAX(0.001, fabs(cos(e * M_PI)));
			gfx_fill_rect(cr, cr_, col_alpha(COL_BLACK, 0.25f));
			cairo_save(cr);
			cairo_translate(cr, 0, rect_cy(cr_));
			cairo_scale(cr, 1, sy);
			cairo_translate(cr, 0, -rect_cy(cr_));
			tile_cell(ui, cr_, arr, show, c);
			cairo_restore(cr);
			ui->animating = true;
		} else {
			tile_cell(ui, cr_, arr, cur, c);
		}
		ui_clip_pop(ui);
	}
	/* label on a soft shade so it reads over photos */
	gfx_fill_rect(cr, R(r.x, rect_bottom(r) - 30, r.w, 30), col_alpha(COL_BLACK, 0.18f));
	app_tile_name(ui, r, _("People"));
}

/* ------------------------------------------------------------ navigation - */

enum { P_ROOT, P_CARD, P_EDIT, P_SETTINGS };
static Nav g_nav;
static Pager g_pivot;
static JumpList g_jl;
static Scroll g_fav_scroll, g_recent_scroll, g_card_scroll, g_set_scroll;
static AppBar g_bar_root, g_bar_card, g_bar_edit;
static Contact *g_cur;            /* contact shown on the card */
static double g_card_h = 800;
static Contact *g_menu_c;         /* long-pressed contact */
static bool g_focus_search;

/* the editor works on fixed buffers */
#define EVAL 256
static struct {
	Contact *target;              /* NULL = new contact */
	char f[NF][512];
	char notes[4096];
	struct { int type; char val[EVAL]; } it[NK][MAXI];
	int n[NK];
	bool dirty, focus_next;
	Scroll scroll;
	double h, focus_y, focus_h, last_view_h;
	UiId last_focus, focus_id;
} E;

static void nav_reset(void) {
	nav_init(&g_nav, P_ROOT);
	g_cur = NULL;
	g_bar_root = g_bar_card = g_bar_edit = (AppBar){0};
}

static void open_card(UI *ui, Contact *c) {
	g_cur = c;
	c->viewed = (double)time(NULL);
	g_card_scroll = (Scroll){0};
	g_bar_card = (AppBar){0};
	save_soon();
	if (nav_page(&g_nav) == P_CARD) return;
	nav_push(&g_nav, ui, P_CARD);
}

static void open_editor(UI *ui, Contact *c) {
	memset(&E, 0, sizeof E);
	E.target = c;
	if (c) {
		for (int f = 0; f < NF; f++) {
			if (f == F_NOTES) str_copy(E.notes, sizeof E.notes, cf(c, f));
			else str_copy(E.f[f], sizeof E.f[f], cf(c, f));
		}
		for (int k = 0; k < NK; k++) {
			E.n[k] = c->n[k];
			for (int i = 0; i < c->n[k]; i++) {
				E.it[k][i].type = c->it[k][i].type;
				str_copy(E.it[k][i].val, EVAL, c->it[k][i].val);
			}
		}
	}
	g_bar_edit = (AppBar){0};
	ui->focus = 0;
	if (!c) E.focus_id = ui_idn("pe-f", F_FIRST);
	nav_push(&g_nav, ui, P_EDIT);
}

/* replaces the current page (editor -> new contact's card) with a backward turnstile */
static void nav_replace(UI *ui, int page) {
	g_nav.from = nav_page(&g_nav);
	g_nav.stack[g_nav.depth] = page;
	g_nav.to = page;
	g_nav.dir = -1;
	anim_start(&g_nav.anim, 0, 1, ui->now, cfg.animations ? 0.36 : 0.001, EASE_LINEAR);
	shell_invalidate();
}

/* ------------------------------------------------------------- deleting -- */

static bool own_photo(const char *p) {
	if (!p) return false;
	char *dir = app_data_file("people-photos");
	bool r = str_starts(p, dir);
	free(dir);
	return r;
}

static void delete_contact(Contact *c) {
	int i = contact_index(c);
	if (i < 0) return;
	if (own_photo(c->f[F_PHOTO])) unlink(c->f[F_PHOTO]);
	vec_remove(&g_c, i);
	if (g_cur == c) g_cur = NULL;
	if (E.target == c) E.target = NULL;
	contact_free(c);
	changed();
	save_now();
}

static void delete_cb(int btn, void *ud) {
	Contact *c = ud;
	if (btn != 0 || contact_index(c) < 0) return;
	bool on_card = g_cur == c && nav_page(&g_nav) == P_CARD;
	delete_contact(c);
	if (on_card) nav_pop(&g_nav, &sh.ui);
}

static void confirm_delete(Contact *c) {
	char *t = xasprintf(_("%s will be deleted from your contacts."), c->disp);
	dialog_message(_("delete contact?"), t, _("delete"), _("cancel"), delete_cb, c);
	free(t);
}

static void toggle_fav(Contact *c) {
	c->fav = !c->fav;
	shell_toast(c->fav ? _("added to favorites") : _("removed from favorites"), c->disp, _("People"));
	save_now();
	shell_invalidate();
}

static void row_menu_cb(int idx, void *ud) {
	Contact *c = g_menu_c;
	intptr_t kind = (intptr_t)ud;   /* 1 = recent list */
	if (!c || contact_index(c) < 0) return;
	switch (idx) {
	case 0: open_editor(&sh.ui, c); break;
	case 1: confirm_delete(c); break;
	case 2: toggle_fav(c); break;
	case 3: if (kind == 1) { c->viewed = 0; save_soon(); shell_invalidate(); } break;
	default: break;
	}
}

static void row_menu(Contact *c, double x, double y, bool recent) {
	static const char *items[4];
	items[0] = _("edit");
	items[1] = _("delete");
	items[2] = c->fav ? _("remove from favorites") : _("add to favorites");
	items[3] = _("remove from recent");
	g_menu_c = c;
	dialog_menu(x, y, items, recent ? 4 : 3, row_menu_cb, (void *)(intptr_t)(recent ? 1 : 0));
}

/* ---------------------------------------------------------- vCard import - */

typedef struct {
	char *path, *photo_dir;
	VEC(Contact *) out;
	bool failed;
} ImportJob;

static void vc_unescape(char *s) {
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			*w++ = (*r == 'n' || *r == 'N') ? '\n' : *r;
		} else *w++ = *r;
	}
	*w = 0;
}

/* splits a structured value at unescaped ';' (in place); returns the count */
static int vc_split(char *s, char **parts, int max, char sep) {
	int n = 0;
	parts[n++] = s;
	for (char *p = s; *p && n < max; p++) {
		if (*p == '\\' && p[1]) { p++; continue; }
		if (*p == sep) { *p = 0; parts[n++] = p + 1; }
	}
	for (int i = 0; i < n; i++) vc_unescape(parts[i]);
	return n;
}

static void qp_decode(char *s) {
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '=' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
			char h[3] = {r[1], r[2], 0};
			*w++ = (char)strtol(h, NULL, 16);
			r += 2;
		} else *w++ = *r;
	}
	*w = 0;
}

static void normalize_bday(const char *v, char *out, size_t cap) {
	char d[16];
	int n = 0;
	out[0] = 0;
	bool noyear = str_starts(v, "--");
	for (const char *p = v; *p && *p != 'T' && n < 15; p++) if (isdigit((unsigned char)*p)) d[n++] = *p;
	d[n] = 0;
	int y = 0, m = 0, dd = 0;
	if (noyear && n == 4) sscanf(d, "%2d%2d", &m, &dd);
	else if (n == 8) sscanf(d, "%4d%2d%2d", &y, &m, &dd);
	else return;
	if (m < 1 || m > 12 || dd < 1 || dd > 31) return;
	if (y) snprintf(out, cap, "%04d-%02d-%02d", y, m, dd);
	else snprintf(out, cap, "--%02d-%02d", m, dd);
}

typedef struct { Contact *c; char *fn; bool v21; } VcState;

static void vc_photo(ImportJob *j, Contact *c, const char *types, const char *enc, char *val) {
	if (c->f[F_PHOTO] || !j->photo_dir) return;
	const char *ext = strstr(types, ",png,") ? "png" : "jpg";
	const char *b64 = NULL;
	if (str_starts(val, "data:")) {
		if (strstr(val, "image/png")) ext = "png";
		const char *comma = strchr(val, ',');
		if (comma && strstr(val, ";base64")) b64 = comma + 1;
	} else if (strstr(enc, ",b,") || strstr(enc, ",base64,")) {
		b64 = val;
	} else if (str_starts(val, "file://") && file_exists(val + 7)) {
		cset(c, F_PHOTO, val + 7);
		return;
	}
	if (!b64 || strlen(b64) > 8 * 1024 * 1024) return;
	gsize len = 0;
	guchar *data = g_base64_decode(b64, &len);
	if (data && len > 32) {
		char hex[65];
		sha256_hex((const char *)data, len, hex);
		hex[20] = 0;
		char *name = xasprintf("%s.%s", hex, ext);
		char *path = path_join(j->photo_dir, name);
		if (file_exists(path) || file_write_all(path, data, len)) cset(c, F_PHOTO, path);
		free(name);
		free(path);
	}
	g_free(data);
}

static void vc_line(ImportJob *j, VcState *st, char *line) {
	/* name;params:value - the colon may not be inside a quoted parameter */
	char *colon = NULL;
	bool q = false;
	for (char *p = line; *p; p++) {
		if (*p == '"') q = !q;
		else if (*p == ':' && !q) { colon = p; break; }
	}
	if (!colon) return;
	*colon = 0;
	char *val = colon + 1;
	char *params[24];
	int np = vc_split(line, params, 24, ';');
	char *name = params[0];
	char *dot = strrchr(name, '.');
	if (dot) name = dot + 1;
	for (char *p = name; *p; p++) *p = (char)toupper((unsigned char)*p);
	/* parameters -> ",type,type," and the encoding / charset */
	char types[160] = ",", enc[40] = ",", charset[40] = "";
	for (int i = 1; i < np; i++) {
		char *pv = params[i], *eq = strchr(pv, '=');
		char key[24] = "TYPE";
		if (eq) {
			*eq = 0;
			str_copy(key, sizeof key, pv);
			pv = eq + 1;
		}
		for (char *p = key; *p; p++) *p = (char)toupper((unsigned char)*p);
		for (char *p = pv; *p; p++) *p = (char)tolower((unsigned char)*p);
		char *dst = str_eq(key, "TYPE") ? types : str_eq(key, "ENCODING") ? enc : NULL;
		if (str_eq(key, "CHARSET")) { str_copy(charset, sizeof charset, pv); continue; }
		if (!dst) continue;
		size_t cap = dst == types ? sizeof types : sizeof enc;
		char *save = NULL;
		for (char *tok = strtok_r(pv, ",\"", &save); tok; tok = strtok_r(NULL, ",\"", &save))
			if (strlen(dst) + strlen(tok) + 2 < cap) { strcat(dst, tok); strcat(dst, ","); }
		/* vCard 2.1 bare parameters such as "CELL" or "QUOTED-PRINTABLE" */
		if (!eq && (strstr(pv, "quoted-printable") || str_eq(pv, "base64") || str_eq(pv, "b"))) {
			if (strlen(enc) + strlen(pv) + 2 < sizeof enc) { strcat(enc, pv); strcat(enc, ","); }
		}
	}
	if (str_eq(name, "BEGIN")) {
		if (strncasecmp(val, "VCARD", 5) == 0) {
			if (st->c) contact_free(st->c);
			free(st->fn);
			st->fn = NULL;
			st->c = xcalloc(1, sizeof(Contact));
		}
		return;
	}
	Contact *c = st->c;
	if (!c) return;
	if (str_eq(name, "END")) {
		if (!c->f[F_FIRST] && !c->f[F_LAST] && st->fn) cset(c, F_FIRST, st->fn);
		if (!contact_empty(c) && j->out.len < 5000) vec_push(&j->out, c);
		else contact_free(c);
		st->c = NULL;
		return;
	}
	if (str_eq(name, "PHOTO")) { vc_photo(j, c, types, enc, val); return; }
	if (strstr(enc, "quoted-printable")) qp_decode(val);
	/* legacy files are often not UTF-8 */
	char *conv = NULL;
	if (!g_utf8_validate(val, -1, NULL)) {
		conv = g_convert(val, -1, "UTF-8", charset[0] && strcasecmp(charset, "utf-8") ? charset : "WINDOWS-1252", NULL, NULL, NULL);
		if (!conv) return;
		val = conv;
	}
	char *parts[8];
	if (str_eq(name, "N")) {
		int n = vc_split(val, parts, 8, ';');
		cset(c, F_LAST, parts[0]);
		if (n > 1) {
			if (n > 2 && *str_trim(parts[2])) {
				char *fm = xasprintf("%s %s", str_trim(parts[1]), str_trim(parts[2]));
				cset(c, F_FIRST, fm);
				free(fm);
			} else cset(c, F_FIRST, parts[1]);
		}
	} else if (str_eq(name, "FN")) {
		vc_unescape(val);
		free(st->fn);
		st->fn = xstrdup(val);
	} else if (str_eq(name, "NICKNAME")) {
		vc_split(val, parts, 2, ',');
		cset(c, F_NICK, parts[0]);
	} else if (str_eq(name, "ORG")) {
		vc_split(val, parts, 2, ';');
		cset(c, F_COMPANY, parts[0]);
	} else if (str_eq(name, "TITLE") || str_eq(name, "NOTE") || str_eq(name, "URL")) {
		vc_unescape(val);
		int f = str_eq(name, "TITLE") ? F_TITLE : str_eq(name, "NOTE") ? F_NOTES : F_WEB;
		if (!c->f[f]) cset(c, f, val);
	} else if (str_eq(name, "BDAY")) {
		char b[16];
		normalize_bday(val, b, sizeof b);
		if (b[0]) cset(c, F_BDAY, b);
	} else if (str_eq(name, "TEL")) {
		vc_unescape(val);
		const char *v = str_starts(val, "tel:") ? val + 4 : val;
		int t = strstr(types, ",cell,") || strstr(types, ",mobile,") || strstr(types, ",iphone,") ? 0
		      : strstr(types, ",home,") ? 1 : strstr(types, ",work,") ? 2 : 3;
		item_add(c, K_PHONE, t, v);
	} else if (str_eq(name, "EMAIL")) {
		vc_unescape(val);
		const char *v = str_starts(val, "mailto:") ? val + 7 : val;
		int t = strstr(types, ",work,") ? 1 : strstr(types, ",other,") ? 2 : 0;
		item_add(c, K_EMAIL, t, v);
	} else if (str_eq(name, "ADR")) {
		int n = vc_split(val, parts, 8, ';');
		/* pobox; extended; street; city; region; postal code; country */
		static const int order[] = {2, 1, 0, 3, 4, 5, 6};
		char addr[512] = "";
		for (size_t i = 0; i < ARRAY_LEN(order); i++) {
			if (order[i] >= n) continue;
			char *p = str_trim(parts[order[i]]);
			for (char *x = p; *x; x++) if (*x == '\n') *x = ' ';
			if (!*p) continue;
			if (addr[0]) strncat(addr, ", ", sizeof addr - strlen(addr) - 1);
			strncat(addr, p, sizeof addr - strlen(addr) - 1);
		}
		int t = strstr(types, ",work,") ? 1 : strstr(types, ",home,") ? 0 : 2;
		item_add(c, K_ADDR, t, addr);
	}
	g_free(conv);
}

static bool line_is_qp(const char *s) {
	const char *colon = strchr(s, ':');
	if (!colon) return false;
	for (const char *p = s; p + 16 <= colon; p++)
		if (strncasecmp(p, "QUOTED-PRINTABLE", 16) == 0) return true;
	return false;
}

static void vcard_parse(ImportJob *j, const char *text) {
	GString *line = g_string_new(NULL);
	VcState st = {0};
	bool soft = false;
	const char *p = text;
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
	for (;;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		size_t l = len;
		if (l && p[l - 1] == '\r') l--;
		bool more = e || len;
		if (more && line->len && (soft || (l && (p[0] == ' ' || p[0] == '\t')))) {
			/* folded continuation (or a quoted-printable soft line break) */
			g_string_append_len(line, soft ? p : p + 1, (gssize)(soft ? l : l - 1));
		} else {
			if (line->len) vc_line(j, &st, line->str);
			g_string_truncate(line, 0);
			if (more) g_string_append_len(line, p, (gssize)l);
		}
		soft = false;
		if (line->len && line->str[line->len - 1] == '=' && line_is_qp(line->str)) {
			g_string_truncate(line, line->len - 1);
			soft = true;
		}
		if (!e) break;
		p = e + 1;
	}
	if (line->len) vc_line(j, &st, line->str);
	contact_free(st.c);
	free(st.fn);
	g_string_free(line, TRUE);
}

static void import_run(void *arg) {
	ImportJob *j = arg;
	struct stat sb;
	if (stat(j->path, &sb) != 0 || sb.st_size > 32 * 1024 * 1024) { j->failed = true; return; }
	char *txt = file_read_all(j->path, NULL);
	if (!txt) { j->failed = true; return; }
	vcard_parse(j, txt);
	free(txt);
}

static void fold_name(const Contact *c, char *out, size_t cap) {
	char *n = xasprintf("%s %s", cf(c, F_FIRST), cf(c, F_LAST));
	char *t = str_trim(n);
	char *f = str_fold(*t ? t : c->disp ? c->disp : "");
	str_copy(out, cap, f);
	free(f);
	free(n);
}

static bool same_person(const Contact *a, const Contact *b) {
	char na[256], nb[256];
	fold_name(a, na, sizeof na);
	fold_name(b, nb, sizeof nb);
	if (!str_eq(na, nb)) return false;
	bool ca = a->n[K_PHONE] || a->n[K_EMAIL], cb = b->n[K_PHONE] || b->n[K_EMAIL];
	if (!ca && !cb) return true;
	for (int i = 0; i < a->n[K_PHONE]; i++) {
		char da[64], db[64];
		digits_of(a->it[K_PHONE][i].val, da, sizeof da);
		size_t la = strlen(da);
		if (la < 3) continue;
		for (int k = 0; k < b->n[K_PHONE]; k++) {
			digits_of(b->it[K_PHONE][k].val, db, sizeof db);
			size_t lb = strlen(db), m = MIN(MIN(la, lb), 9);
			if (m >= 3 && !strcmp(da + la - m, db + lb - m)) return true;
		}
	}
	for (int i = 0; i < a->n[K_EMAIL]; i++)
		for (int k = 0; k < b->n[K_EMAIL]; k++)
			if (!strcasecmp(a->it[K_EMAIL][i].val, b->it[K_EMAIL][k].val)) return true;
	return false;
}

static void import_done(void *arg) {
	ImportJob *j = arg;
	load();
	int added = 0, dup = 0;
	for (int i = 0; i < j->out.len; i++) {
		Contact *c = j->out.items[i];
		contact_names(c);
		bool d = false;
		for (int k = 0; k < g_c.len && !d; k++) d = same_person(c, g_c.items[k]);
		if (d) {
			dup++;
			bool shared = false;
			for (int k = 0; k < g_c.len && !shared; k++) shared = str_eq(cf(g_c.items[k], F_PHOTO), cf(c, F_PHOTO));
			if (!shared && own_photo(c->f[F_PHOTO])) unlink(c->f[F_PHOTO]);
			contact_free(c);
			continue;
		}
		c->id = g_next_id++;
		vec_push(&g_c, c);
		added++;
	}
	if (j->failed) {
		shell_toast(_("Couldn't read the vCard file."), path_basename(j->path), _("People"));
	} else {
		char *body = dup ? xasprintf(_("%d imported, %d duplicates skipped"), added, dup)
		                 : xasprintf(_("%d contacts imported"), added);
		shell_toast(body, path_basename(j->path), _("People"));
		free(body);
	}
	if (added) { changed(); save_now(); }
	if (!g_open) unload();
	vec_free(&j->out);
	free(j->path);
	free(j->photo_dir);
	free(j);
	shell_invalidate();
}

static void import_file(const char *path) {
	ImportJob *j = xcalloc(1, sizeof *j);
	j->path = xstrdup(path);
	j->photo_dir = app_data_file("people-photos");
	mkdir_p(j->photo_dir);
	job_submit(import_run, import_done, j);
}

/* scan ~/Downloads, ~/Documents and ~/Desktop for .vcf files */
typedef struct { char *path; char *label; time_t mtime; } VcfFile;
typedef struct { VEC(VcfFile) files; } ScanJob;
static ScanJob *g_scan;

static void scan_dir(ScanJob *s, const char *dir, const char *label, int depth) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	int seen = 0;
	while ((e = readdir(d)) && seen++ < 4000 && s->files.len < 200) {
		if (e->d_name[0] == '.') continue;
		char *p = path_join(dir, e->d_name);
		struct stat sb;
		if (stat(p, &sb) == 0) {
			size_t l = strlen(e->d_name);
			if (S_ISREG(sb.st_mode) && l > 4 && !strcasecmp(e->d_name + l - 4, ".vcf")) {
				VcfFile f = {p, xasprintf("%s (%s)", e->d_name, label), sb.st_mtime};
				vec_push(&s->files, f);
				continue;
			}
			if (S_ISDIR(sb.st_mode) && depth > 0) scan_dir(s, p, label, depth - 1);
		}
		free(p);
	}
	closedir(d);
}

static int cmp_vcf(const void *a, const void *b) {
	const VcfFile *x = a, *y = b;
	return x->mtime > y->mtime ? -1 : x->mtime < y->mtime;
}

static void scan_run(void *arg) {
	ScanJob *s = arg;
	static const char *dirs[] = {"Downloads", "Documents", "Desktop"};
	for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
		char *p = path_join(home_dir(), dirs[i]);
		scan_dir(s, p, dirs[i], 1);
		free(p);
	}
	if (s->files.len) qsort(s->files.items, (size_t)s->files.len, sizeof(VcfFile), cmp_vcf);
}

static void scan_free(ScanJob *s) {
	if (!s) return;
	for (int i = 0; i < s->files.len; i++) { free(s->files.items[i].path); free(s->files.items[i].label); }
	vec_free(&s->files);
	free(s);
}

static void scan_pick_cb(int idx, void *ud) {
	ScanJob *s = g_scan;
	g_scan = NULL;
	if (s && idx >= 0 && idx < s->files.len) import_file(s->files.items[idx].path);
	scan_free(s);
}

static void scan_done(void *arg) {
	ScanJob *s = arg;
	if (!s->files.len) {
		scan_free(s);
		dialog_message(_("no vCard files"), _("Put a .vcf file in Downloads, Documents or Desktop, then try again."), NULL, NULL,
		               NULL, NULL);
		return;
	}
	scan_free(g_scan);
	g_scan = s;
	const char *items[24];
	int n = MIN(s->files.len, 24);
	for (int i = 0; i < n; i++) items[i] = s->files.items[i].label;
	dialog_list(_("import contacts from"), items, n, -1, scan_pick_cb, NULL);
}

static void start_import(void) {
	job_submit(scan_run, scan_done, xcalloc(1, sizeof(ScanJob)));
}

/* ---------------------------------------------------------- vCard export - */

static void vc_escape(GString *o, const char *s) {
	for (; s && *s; s++) {
		if (*s == '\\' || *s == ',' || *s == ';') { g_string_append_c(o, '\\'); g_string_append_c(o, *s); }
		else if (*s == '\n') g_string_append(o, "\\n");
		else if (*s != '\r') g_string_append_c(o, *s);
	}
}

/* writes one content line folded at 75 octets without splitting UTF-8 sequences */
static void vc_emit(GString *o, GString *line) {
	const char *s = line->str;
	size_t n = line->len, col = 0;
	for (size_t i = 0; i < n;) {
		size_t k = 1;
		while (i + k < n && ((unsigned char)s[i + k] & 0xC0) == 0x80) k++;
		if (col + k > 75) { g_string_append(o, "\r\n "); col = 1; }
		g_string_append_len(o, s + i, (gssize)k);
		col += k;
		i += k;
	}
	g_string_append(o, "\r\n");
	g_string_truncate(line, 0);
}

static void vcard_write(GString *o, const Contact *c) {
	GString *l = g_string_new(NULL);
	g_string_append(o, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
	g_string_append(l, "N:");
	vc_escape(l, cf(c, F_LAST));
	g_string_append_c(l, ';');
	vc_escape(l, cf(c, F_FIRST));
	g_string_append(l, ";;;");
	vc_emit(o, l);
	char *fn = xasprintf("%s %s", cf(c, F_FIRST), cf(c, F_LAST));
	char *t = str_trim(fn);
	if (!*t) t = (char *)(c->f[F_NICK] ? c->f[F_NICK] : c->f[F_COMPANY] ? c->f[F_COMPANY] : "");
	g_string_append(l, "FN:");
	vc_escape(l, t);
	vc_emit(o, l);
	free(fn);
	static const struct { int f; const char *name; } simple[] = {
		{F_NICK, "NICKNAME"}, {F_COMPANY, "ORG"}, {F_TITLE, "TITLE"}, {F_WEB, "URL"}, {F_NOTES, "NOTE"}};
	for (size_t i = 0; i < ARRAY_LEN(simple); i++) {
		if (!c->f[simple[i].f]) continue;
		g_string_append_printf(l, "%s:", simple[i].name);
		vc_escape(l, c->f[simple[i].f]);
		vc_emit(o, l);
	}
	static const char *tel[] = {"CELL", "HOME", "WORK", "VOICE"};
	static const char *mail[] = {"INTERNET,HOME", "INTERNET,WORK", "INTERNET"};
	static const char *adr[] = {"HOME", "WORK", "OTHER"};
	for (int i = 0; i < c->n[K_PHONE]; i++) {
		g_string_append_printf(l, "TEL;TYPE=%s:", tel[c->it[K_PHONE][i].type]);
		vc_escape(l, c->it[K_PHONE][i].val);
		vc_emit(o, l);
	}
	for (int i = 0; i < c->n[K_EMAIL]; i++) {
		g_string_append_printf(l, "EMAIL;TYPE=%s:", mail[c->it[K_EMAIL][i].type]);
		vc_escape(l, c->it[K_EMAIL][i].val);
		vc_emit(o, l);
	}
	for (int i = 0; i < c->n[K_ADDR]; i++) {
		g_string_append_printf(l, "ADR;TYPE=%s:;;", adr[c->it[K_ADDR][i].type]);
		vc_escape(l, c->it[K_ADDR][i].val);
		g_string_append(l, ";;;;");
		vc_emit(o, l);
	}
	if (c->f[F_BDAY]) { g_string_append_printf(l, "BDAY:%s", c->f[F_BDAY]); vc_emit(o, l); }
	if (c->f[F_PHOTO]) {
		size_t len = 0;
		struct stat sb;
		char *data = stat(c->f[F_PHOTO], &sb) == 0 && sb.st_size < 512 * 1024 ? file_read_all(c->f[F_PHOTO], &len) : NULL;
		if (data && len > 4) {
			bool png = (unsigned char)data[0] == 0x89 && data[1] == 'P';
			bool jpg = (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8;
			if (png || jpg) {
				gchar *b = g_base64_encode((const guchar *)data, len);
				g_string_append_printf(l, "PHOTO;ENCODING=b;TYPE=%s:%s", png ? "PNG" : "JPEG", b);
				vc_emit(o, l);
				g_free(b);
			}
		}
		free(data);
	}
	g_string_append(o, "END:VCARD\r\n");
	g_string_free(l, TRUE);
}

typedef struct { VEC(Contact *) cs; char *path; bool ok, single; } ExportJob;

static void export_run(void *arg) {
	ExportJob *j = arg;
	GString *o = g_string_new(NULL);
	for (int i = 0; i < j->cs.len; i++) vcard_write(o, j->cs.items[i]);
	char *dir = g_path_get_dirname(j->path);
	mkdir_p(dir);
	g_free(dir);
	j->ok = file_write_all(j->path, o->str, o->len);
	g_string_free(o, TRUE);
}

static void export_done(void *arg) {
	ExportJob *j = arg;
	const char *home = home_dir();
	char *shown = str_starts(j->path, home) ? xasprintf("~%s", j->path + strlen(home)) : xstrdup(j->path);
	if (!j->ok) shell_toast(_("People"), _("Couldn't write the vCard file."), _("People"));
	else if (j->single) shell_toast(_("contact saved"), shown, _("People"));
	else {
		char *t = xasprintf(_("%d contacts exported"), j->cs.len);
		shell_toast(t, shown, _("People"));
		free(t);
	}
	free(shown);
	for (int i = 0; i < j->cs.len; i++) contact_free(j->cs.items[i]);
	vec_free(&j->cs);
	free(j->path);
	free(j);
}

static void export_contacts(Contact *only) {
	ExportJob *j = xcalloc(1, sizeof *j);
	char *docs = path_join(home_dir(), "Documents");
	if (only) {
		char name[160];
		str_copy(name, sizeof name, only->disp);
		for (char *p = name; *p; p++) if (*p == '/' || *p == '\\' || (unsigned char)*p < 32 || *p == ':') *p = '_';
		char *fn = xasprintf("%s.vcf", name[0] && name[0] != '.' ? name : "contact");
		j->path = path_join(docs, fn);
		free(fn);
		vec_push(&j->cs, contact_dup(only));
		j->single = true;
	} else {
		j->path = path_join(docs, "contacts.vcf");
		for (int i = 0; i < g_order.len; i++) vec_push(&j->cs, contact_dup(g_c.items[g_order.items[i]]));
	}
	free(docs);
	if (!j->cs.len) {
		free(j->path);
		vec_free(&j->cs);
		free(j);
		dialog_message(_("nothing to export"), _("You don't have any contacts yet."), NULL, NULL, NULL, NULL);
		return;
	}
	job_submit(export_run, export_done, j);
}

/* -------------------------------------------------------------- drawing -- */

static void draw_avatar(UI *ui, const Contact *c, Rect r, int dim) {
	cairo_t *cr = ui->cr;
	Image *img = c->f[F_PHOTO] && g_img ? imgcache_get(g_img, c->f[F_PHOTO], dim) : NULL;
	if (img) { gfx_image_cover(cr, img, r, 1); return; }
	gfx_fill_rect(cr, r, ui->th.accent);
	double fs = r.h * 0.4;
	gfx_text(cr, c->initials, rect_cx(r), rect_cy(r) - gfx_line_height(cr, fs, FW_SEMILIGHT) / 2, fs,
	         r.h > 100 ? FW_LIGHT : FW_SEMILIGHT, COL_WHITE, TA_CENTER, r.w - 4, 1);
}

static void contact_row(UI *ui, Contact *c, Rect r, const char *key, const char *sub, bool recent) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	UiId id = ui_idn(key, (int)c->id);
	ui_hit(ui, id, r);
	if (ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.12f));
	double s = MIN(52, r.h - 12);
	Rect av = R(r.x + 24, r.y + (r.h - s) / 2, s, s);
	draw_avatar(ui, c, av, 128);
	double x = av.x + s + 16, w = rect_right(r) - x - 16;
	if (sub && *sub) {
		double h1 = gfx_line_height(cr, 24, FW_SEMILIGHT), h2 = gfx_line_height(cr, 15, FW_REGULAR);
		double y = r.y + (r.h - h1 - h2 + 2) / 2;
		gfx_text(cr, c->disp, x, y, 24, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		gfx_text(cr, sub, x, y + h1 - 2, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	} else {
		gfx_text(cr, c->disp, x, r.y + (r.h - gfx_line_height(cr, 24, FW_SEMILIGHT)) / 2, 24, FW_SEMILIGHT, th->fg, TA_LEFT,
		         w, 1);
	}
	if (ui_clicked(ui, id)) open_card(ui, c);
	if (ui_longpressed(ui, id)) {
		Rect sr = ui_to_screen(ui, r);
		row_menu(c, ui->p.x, sr.y + r.h / 2, recent);
	}
}

#define ROW_H 64.0
#define TOP_H 70.0

static void all_top(UI *ui, Rect r, void *ud) {
	UiId id = ui_id("pp-search");
	if (g_focus_search) { ui_focus(ui, id); ui_key_event(ui, UK_END); g_focus_search = false; }
	bool ch = false;
	ui_textbox(ui, id, R(r.x + 24, r.y + 8, r.w - 48, 46), g_query, sizeof g_query, _("search"), 0, &ch);
	if (ch) ui_request_frame(ui);   /* filtered on the next frame (the list is being drawn) */
}

static void all_row(UI *ui, int i, Rect r, void *ud) {
	if (i < g_view.len) contact_row(ui, g_c.items[g_view.items[i]], r, "pp-all", NULL, false);
}

static void page_all(UI *ui, Rect r) {
	if (!g_c.len) {
		app_empty(ui, R(r.x, r.y, r.w, r.h),
		          _("You don't have any contacts yet. Tap + to add one, or import a vCard file from the menu."));
		return;
	}
	jumplist_draw(ui, &g_jl, "pp-jl", r, (const char (*)[8])g_letters, g_view.len, ROW_H, TOP_H, all_top, all_row, NULL);
	if (!g_view.len) app_empty(ui, R(r.x, r.y + TOP_H, r.w, r.h - TOP_H), _("no results"));
}

static void page_favorites(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	int idx[512], n = 0;
	for (int k = 0; k < g_order.len && n < 512; k++) if (g_c.items[g_order.items[k]]->fav) idx[n++] = g_order.items[k];
	if (!n) {
		app_empty(ui, r, _("Your favorite people show up here. To add someone, open their contact card and tap the star."));
		return;
	}
	int cols = CLAMP((int)((r.w - 36) / 150), 3, 8);
	double gap = 12, tw = floor((r.w - 48 - (cols - 1) * gap) / cols), rh = tw + 40;
	int rows = (n + cols - 1) / cols;
	double off = ui_scroll_begin(ui, ui_id("pp-fav"), &g_fav_scroll, r, rows * rh + 24);
	cairo_translate(cr, r.x, r.y);
	for (int i = 0; i < n; i++) {
		double x = 24 + (i % cols) * (tw + gap), y = 8 + (i / cols) * rh;
		if (y + rh < off || y > off + r.h) continue;
		Contact *c = g_c.items[idx[i]];
		Rect tre = R(x, y, tw, tw);
		UiId id = ui_idn("pp-favt", (int)c->id);
		ui_hit(ui, id, R(x, y, tw, rh - 6));
		bool pr = ui_pressed(ui, id);
		cairo_save(cr);
		if (pr) {   /* tiles tilt/shrink slightly when pressed */
			cairo_translate(cr, rect_cx(tre), rect_cy(tre));
			cairo_scale(cr, 0.96, 0.96);
			cairo_translate(cr, -rect_cx(tre), -rect_cy(tre));
		}
		draw_avatar(ui, c, tre, 384);
		cairo_restore(cr);
		const char *fi = cf(c, F_FIRST);
		gfx_text(cr, *fi && !g_disp_last ? fi : c->disp, x, y + tw + 4, 17, FW_SEMILIGHT, th->fg, TA_LEFT, tw, 1);
		if (ui_clicked(ui, id)) open_card(ui, c);
		if (ui_longpressed(ui, id)) {
			Rect sr = ui_to_screen(ui, tre);
			row_menu(c, rect_cx(sr), rect_cy(sr), false);
		}
	}
	ui_scroll_end(ui);
}

static int cmp_recent(const void *a, const void *b) {
	double x = g_c.items[*(const int *)a]->viewed, y = g_c.items[*(const int *)b]->viewed;
	return x > y ? -1 : x < y;
}

static void page_recent(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	int idx[30], n = 0;
	VEC(int) all = {0};
	for (int i = 0; i < g_c.len; i++) if (g_c.items[i]->viewed > 0) vec_push(&all, i);
	if (all.len) qsort(all.items, (size_t)all.len, sizeof(int), cmp_recent);
	for (int i = 0; i < all.len && n < 30; i++) idx[n++] = all.items[i];
	vec_free(&all);
	if (!n) {
		app_empty(ui, r, _("People you look at recently show up here."));
		return;
	}
	double off = ui_scroll_begin(ui, ui_id("pp-recent"), &g_recent_scroll, r, n * ROW_H + 24);
	cairo_translate(cr, r.x, r.y);
	time_t now = time(NULL);
	for (int i = 0; i < n; i++) {
		double y = i * ROW_H;
		if (y + ROW_H < off || y > off + r.h) continue;
		Contact *c = g_c.items[idx[i]];
		char ago[64];
		i18n_fmt_ago(ago, sizeof ago, (time_t)c->viewed, now);
		contact_row(ui, c, R(0, y, r.w, ROW_H), "pp-rec", ago, true);
	}
	ui_scroll_end(ui);
}

static void pivot_page(UI *ui, int page, Rect r, void *ud) {
	if (page == 0) page_all(ui, r);
	else if (page == 1) page_favorites(ui, r);
	else page_recent(ui, r);
}

static void draw_root(UI *ui, Rect area) {
	Rect c = appbar_content(&g_bar_root, area);
	const char *titles[3] = {_("all"), _("favorites"), _("recent")};
	app_pivot(ui, "pp-pivot", &g_pivot, c, _("People"), titles, 3, pivot_page, NULL);
	AppBarBtn b[2] = {{ICON_ADD, _("new")}, {ICON_SEARCH, _("search")}};
	const char *menu[4] = {_("import contacts"), _("export contacts"), _("settings")};
	int a = appbar_draw(ui, &g_bar_root, area, b, 2, menu, 3);
	if (a == 0) open_editor(ui, NULL);
	else if (a == 1) {
		ui_pager_goto(&g_pivot, 0);
		ui_scroll_to(&g_jl.scroll, 0);
		g_jl.open = false;
		g_focus_search = true;
	} else if (a == APPBAR_MENU) start_import();
	else if (a == APPBAR_MENU + 1) export_contacts(NULL);
	else if (a == APPBAR_MENU + 2) { g_set_scroll = (Scroll){0}; nav_push(&g_nav, ui, P_SETTINGS); }
}

/* ------------------------------------------------------------ the card --- */

static char *url_encode(const char *s) {
	char *o = xmalloc(strlen(s) * 3 + 1), *w = o;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') *w++ = (char)*p;
		else if (*p == ' ') *w++ = '+';
		else w += sprintf(w, "%%%02X", *p);
	}
	*w = 0;
	return o;
}

static void open_scheme(const char *scheme, const char *v) {
	char clean[128];
	size_t n = 0;
	for (const char *p = v; *p && n + 1 < sizeof clean; p++)
		if (isdigit((unsigned char)*p) || *p == '+' || *p == '*' || *p == '#') clean[n++] = *p;
	clean[n] = 0;
	char *u = xasprintf("%s:%s", scheme, clean);
	open_uri(u);
	free(u);
}

static void format_bday(const char *b, char *out, size_t cap) {
	int y = 0, m = 0, d = 0;
	out[0] = 0;
	if (!b) return;
	if (str_starts(b, "--")) sscanf(b, "--%d-%d", &m, &d);
	else sscanf(b, "%d-%d-%d", &y, &m, &d);
	if (m < 1 || m > 12) return;
	const char *mon = i18n_month(m - 1, false);
	if (i18n_lang() == LANG_TR) {
		if (y) snprintf(out, cap, "%d %s %d", d, mon, y);
		else snprintf(out, cap, "%d %s", d, mon);
	} else {
		if (y) snprintf(out, cap, "%s %d, %d", mon, d, y);
		else snprintf(out, cap, "%s %d", mon, d);
	}
}

/* an action line on the profile: "call mobile" over the number */
static bool card_row(UI *ui, UiId id, double *y, double w, const char *title, const char *sub, int sub_lines, bool tap) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double th1 = gfx_line_height(cr, 24, FW_SEMILIGHT);
	double sh2 = sub && *sub ? gfx_text_size(cr, sub, 17, FW_REGULAR, w - 48, sub_lines).h : 0;
	Rect r = R(0, *y, w, 12 + th1 + sh2 + 12);
	bool clicked = false;
	if (tap) {
		ui_hit(ui, id, r);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.12f));
		clicked = ui_clicked(ui, id);
	}
	gfx_text(cr, title, 24, r.y + 10, 24, FW_SEMILIGHT, th->fg, TA_LEFT, w - 48, 1);
	if (sh2 > 0) gfx_text(cr, sub, 24, r.y + 10 + th1 - 2, 17, FW_REGULAR, th->sub, TA_LEFT, w - 48, sub_lines);
	*y += r.h;
	return clicked;
}

static void draw_card(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Contact *c = g_cur;
	if (!c) return;
	Rect pc = appbar_content(&g_bar_card, area);
	double w = pc.w;
	ui_scroll_begin(ui, ui_id("pc-scroll"), &g_card_scroll, pc, g_card_h);
	cairo_translate(cr, pc.x, pc.y);
	double y = 8;
	y += gfx_text(cr, i18n_upper(_("People")), 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, w - 36, 1) - 6;
	/* big lower-case name; shrinks for long names */
	const char *name = i18n_lower(c->disp);
	double fs = 56;
	while (fs > 36 && gfx_text_size(cr, name, fs, FW_LIGHT, 0, 1).w > w - 30) fs -= 4;
	y += gfx_text(cr, name, 14, y, fs, FW_LIGHT, th->fg, TA_LEFT, w - 28, 2) + 14;
	double ps = floor(MIN(200, w * 0.42));
	Rect ph = R(24, y, ps, ps);
	draw_avatar(ui, c, ph, 384);
	double ix = ph.x + ps + 16, iw = w - ix - 20, iy = y - 4;
	if (c->f[F_NICK]) iy += gfx_text(cr, c->f[F_NICK], ix, iy, 20, FW_SEMILIGHT, th->fg, TA_LEFT, iw, 2) + 2;
	if (c->f[F_TITLE]) iy += gfx_text(cr, c->f[F_TITLE], ix, iy, 17, FW_REGULAR, th->sub, TA_LEFT, iw, 2);
	if (c->f[F_COMPANY]) iy += gfx_text(cr, c->f[F_COMPANY], ix, iy, 17, FW_REGULAR, th->sub, TA_LEFT, iw, 2);
	if (c->fav) icon_draw(cr, ICON_STAR_FILLED, ix + 12, ph.y + ps - 14, 24, th->accent);
	y += ps + 18;

	char sub[300];
	for (int i = 0; i < c->n[K_PHONE]; i++) {
		Item *it = &c->it[K_PHONE][i];
		if (card_row(ui, ui_idn("pc-call", i), &y, w, _(CALL_LABEL[it->type]), it->val, 1, true)) open_scheme("tel", it->val);
	}
	/* text: every mobile number, or the first number when none is marked mobile */
	int texts = 0;
	for (int i = 0; i < c->n[K_PHONE]; i++) {
		Item *it = &c->it[K_PHONE][i];
		if (it->type != 0) continue;
		texts++;
		if (card_row(ui, ui_idn("pc-sms", i), &y, w, _("text"), it->val, 1, true)) open_scheme("sms", it->val);
	}
	if (!texts && c->n[K_PHONE] &&
	    card_row(ui, ui_idn("pc-sms", 0), &y, w, _("text"), c->it[K_PHONE][0].val, 1, true))
		open_scheme("sms", c->it[K_PHONE][0].val);
	for (int i = 0; i < c->n[K_EMAIL]; i++) {
		Item *it = &c->it[K_EMAIL][i];
		snprintf(sub, sizeof sub, "%s", it->val);
		if (card_row(ui, ui_idn("pc-mail", i), &y, w, _("send email"), sub, 1, true)) {
			char *u = xasprintf("mailto:%s", it->val);
			open_uri(u);
			free(u);
		}
	}
	for (int i = 0; i < c->n[K_ADDR]; i++) {
		Item *it = &c->it[K_ADDR][i];
		if (card_row(ui, ui_idn("pc-map", i), &y, w, _(MAP_LABEL[it->type]), it->val, 3, true)) {
			char *q = url_encode(it->val);
			char *u = xasprintf("https://www.openstreetmap.org/search?query=%s", q);
			open_uri(u);
			free(u);
			free(q);
		}
	}
	if (c->f[F_WEB] && card_row(ui, ui_id("pc-web"), &y, w, _("view website"), c->f[F_WEB], 1, true)) {
		const char *v = c->f[F_WEB];
		char *u = strstr(v, "://") ? xstrdup(v) : xasprintf("https://%s", v);
		open_uri(u);
		free(u);
	}
	if (c->f[F_BDAY]) {
		format_bday(c->f[F_BDAY], sub, sizeof sub);
		card_row(ui, 0, &y, w, _("birthday"), sub, 1, false);
	}
	if (c->f[F_NOTES]) card_row(ui, 0, &y, w, _("notes"), c->f[F_NOTES], 0, false);
	if (!c->n[K_PHONE] && !c->n[K_EMAIL] && !c->n[K_ADDR] && !c->f[F_WEB] && !c->f[F_BDAY] && !c->f[F_NOTES])
		y += gfx_text(cr, _("No contact info yet. Tap edit to add a phone number or email address."), 24, y, 20,
		              FW_SEMILIGHT, th->sub, TA_LEFT, w - 48, 0);
	g_card_h = y + 24;
	ui_scroll_end(ui);

	AppBarBtn b[3] = {{ICON_EDIT, _("edit")}, {c->fav ? ICON_STAR_FILLED : ICON_STAR, c->fav ? _("unfavorite") : _("favorite")},
	                  {ICON_DELETE, _("delete")}};
	const char *menu[1] = {_("share contact")};
	int a = appbar_draw(ui, &g_bar_card, area, b, 3, menu, 1);
	if (a == 0) open_editor(ui, c);
	else if (a == 1) toggle_fav(c);
	else if (a == 2) confirm_delete(c);
	else if (a == APPBAR_MENU) export_contacts(c);
}

/* ----------------------------------------------------------- the editor -- */

static char *ebuf(int f, size_t *cap) {
	if (f == F_NOTES) { *cap = sizeof E.notes; return E.notes; }
	*cap = f == F_BDAY ? 16 : sizeof E.f[f];
	return E.f[f];
}

static void edit_caption(UI *ui, double y, double w, const char *s) {
	gfx_text(ui->cr, s, 24, y, 15, FW_REGULAR, ui->th.sub, TA_LEFT, w - 48, 1);
}

static void track_focus(UI *ui, UiId id, double y, double h) {
	if ((E.focus_id == id && E.focus_id) || E.focus_next) {
		ui_focus(ui, id);
		ui_key_event(ui, UK_END);   /* caret at the end of the text */
		E.focus_id = 0;
		E.focus_next = false;
	}
	if (ui->focus == id) { E.focus_y = y; E.focus_h = h; }
}

static void edit_textbox(UI *ui, UiId id, Rect r, char *buf, size_t cap, int flags) {
	bool ch = false;
	track_focus(ui, id, r.y - 24, r.h + 30);
	bool enter = ui_textbox(ui, id, r, buf, cap, NULL, flags, &ch);
	if (ch) E.dirty = true;
	if (enter) E.focus_next = true;
}

static void edit_field(UI *ui, double *y, double w, int f, const char *caption, int flags) {
	edit_caption(ui, *y, w, caption);
	*y += 22;
	size_t cap;
	char *b = ebuf(f, &cap);
	edit_textbox(ui, ui_idn("pe-f", f), R(24, *y, w - 48, 48), b, cap, flags);
	*y += 48 + 14;
}

static void type_pick_cb(int idx, void *ud) {
	int k = (int)(intptr_t)ud / 100, i = (int)(intptr_t)ud % 100;
	if (idx < 0 || k >= NK || i >= E.n[k]) return;
	E.it[k][i].type = CLAMP(idx, 0, ntypes(k) - 1);
	E.dirty = true;
}

static void bday_cb(int btn, void *ud) {
	if (btn != 0) return;
	int y, m, d;
	dialog_date_get(&y, &m, &d);
	snprintf(E.f[F_BDAY], sizeof E.f[F_BDAY], "%04d-%02d-%02d", y, m + 1, d);
	E.dirty = true;
}

static void photo_picked(const char *path, void *ud) {
	if (!path || !*path) return;
	str_copy(E.f[F_PHOTO], sizeof E.f[F_PHOTO], path);
	E.dirty = true;
	shell_invalidate();
}

static void photo_menu_cb(int idx, void *ud) {
	if (idx == 0) photos_pick("people", photo_picked, NULL);
	else if (idx == 1) { E.f[F_PHOTO][0] = 0; E.dirty = true; }
}

static void edit_photo(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	UiId id = ui_id("pe-photo");
	ui_hit(ui, id, r);
	bool pr = ui_pressed(ui, id);
	Image *img = E.f[F_PHOTO][0] && g_img ? imgcache_get(g_img, E.f[F_PHOTO], 384) : NULL;
	if (img) gfx_image_cover(cr, img, r, pr ? 0.7 : 1);
	else {
		gfx_fill_rect(cr, r, pr ? th->accent : th->chrome2);
		icon_draw(cr, ICON_ADD, rect_cx(r), rect_cy(r) - 10, 34, pr ? COL_WHITE : th->fg);
		gfx_text(cr, _("add photo"), rect_cx(r), rect_bottom(r) - 30, 15, FW_REGULAR, pr ? COL_WHITE : th->fg, TA_CENTER,
		         r.w - 8, 1);
	}
	if (ui_clicked(ui, id)) {
		ui->focus = 0;
		if (!E.f[F_PHOTO][0]) photos_pick("people", photo_picked, NULL);
		else {
			static const char *items[2];
			items[0] = _("change photo");
			items[1] = _("remove photo");
			Rect sr = ui_to_screen(ui, r);
			dialog_menu(rect_cx(sr), rect_cy(sr), items, 2, photo_menu_cb, NULL);
		}
	}
}

static bool edit_valid(void) {
	for (int f = F_FIRST; f <= F_COMPANY; f++) {
		char tmp[512];
		str_copy(tmp, sizeof tmp, E.f[f]);
		if (*str_trim(tmp)) return true;
	}
	for (int k = K_PHONE; k <= K_EMAIL; k++)
		for (int i = 0; i < E.n[k]; i++) {
			char tmp[EVAL];
			str_copy(tmp, sizeof tmp, E.it[k][i].val);
			if (*str_trim(tmp)) return true;
		}
	return false;
}

static void edit_save(UI *ui) {
	if (!edit_valid()) {
		dialog_message(_("can't save"), _("Add a name, phone number or email address first."), NULL, NULL, NULL, NULL);
		return;
	}
	Contact *c = E.target;
	bool fresh = !c;
	if (fresh) {
		c = xcalloc(1, sizeof *c);
		c->id = g_next_id++;
		vec_push(&g_c, c);
	}
	if (!fresh && own_photo(c->f[F_PHOTO]) && !str_eq(c->f[F_PHOTO], E.f[F_PHOTO])) unlink(c->f[F_PHOTO]);
	for (int f = 0; f < NF; f++) {
		size_t cap;
		cset(c, f, ebuf(f, &cap));
	}
	items_clear(c);
	for (int k = 0; k < NK; k++)
		for (int i = 0; i < E.n[k]; i++) item_add(c, k, E.it[k][i].type, E.it[k][i].val);
	E.dirty = false;
	ui->focus = 0;
	changed();
	save_now();
	rebuild();
	if (fresh) {
		g_cur = c;
		c->viewed = (double)time(NULL);
		g_card_scroll = (Scroll){0};
		g_bar_card = (AppBar){0};
		nav_replace(ui, P_CARD);
	} else {
		nav_pop(&g_nav, ui);
	}
}

static void draw_edit(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect pc = appbar_content(&g_bar_edit, area);
	double w = pc.w;
	double off = ui_scroll_begin(ui, ui_id("pe-scroll"), &E.scroll, pc, E.h);
	cairo_translate(cr, pc.x, pc.y);
	E.focus_y = -1;
	double y = 8;
	y += gfx_text(cr, i18n_upper(E.target ? _("edit contact") : _("new contact")), 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT,
	              w - 36, 1) + 16;
	edit_photo(ui, R(24, y, 120, 120));
	y += 120 + 18;
	edit_field(ui, &y, w, F_FIRST, _("first name"), 0);
	edit_field(ui, &y, w, F_LAST, _("last name"), 0);
	edit_field(ui, &y, w, F_NICK, _("nickname"), 0);
	edit_field(ui, &y, w, F_COMPANY, _("company"), 0);
	edit_field(ui, &y, w, F_TITLE, _("job title"), 0);
	for (int k = 0; k < NK; k++) {
		for (int i = 0; i < E.n[k]; i++) {
			/* caption "phone" + tappable type ("mobile") */
			const char *kl = _(KIND_LABEL[k]);
			Size ks = gfx_text_size(cr, kl, 15, FW_REGULAR, 0, 1);
			edit_caption(ui, y, w, kl);
			const char *tl = _(TLABEL[k][E.it[k][i].type]);
			Size ts = gfx_text_size(cr, tl, 15, FW_REGULAR, 0, 1);
			Rect tre = R(24 + ks.w + 8, y - 6, ts.w + 30, 28);
			UiId tid = ui_idn("pe-ty", k * 100 + i);
			ui_hit(ui, tid, tre);
			Color tc = ui_pressed(ui, tid) ? th->sub : th->accent;
			gfx_text(cr, tl, tre.x, y, 15, FW_REGULAR, tc, TA_LEFT, 0, 1);
			icon_draw(cr, ICON_CHEVRON_D, tre.x + ts.w + 12, y + ks.h / 2, 12, tc);
			if (ui_clicked(ui, tid)) {
				const char *labels[NTYPES];
				for (int t = 0; t < ntypes(k); t++) labels[t] = _(TLABEL[k][t]);
				ui->focus = 0;
				dialog_list(_("choose type"), labels, ntypes(k), E.it[k][i].type, type_pick_cb, (void *)(intptr_t)(k * 100 + i));
			}
			y += 22;
			int fl = k == K_EMAIL ? TB_EMAIL : 0;   /* phone numbers keep spaces, dashes and parentheses */
			edit_textbox(ui, ui_idn("pe-it", k * 100 + i), R(24, y, w - 48 - 52, 48), E.it[k][i].val, EVAL, fl);
			UiId rid = ui_idn("pe-rm", k * 100 + i);
			if (ui_icon_button(ui, rid, R(w - 24 - 48, y, 48, 48), ICON_CLOSE, th->fg)) {
				memmove(&E.it[k][i], &E.it[k][i + 1], (size_t)(E.n[k] - i - 1) * sizeof E.it[k][0]);
				E.n[k]--;
				E.dirty = true;
				ui->focus = 0;
			}
			y += 48 + 14;
		}
		if (E.n[k] < MAXI) {
			Rect ar = R(0, y - 4, w, 52);
			UiId aid = ui_idn("pe-add", k);
			ui_hit(ui, aid, ar);
			bool pr = ui_pressed(ui, aid);
			icon_draw_circle(cr, ICON_ADD, 24 + 18, rect_cy(ar), 36, pr ? th->accent : th->fg, 2);
			gfx_text(cr, _(ADD_LABEL[k]), 24 + 48, rect_cy(ar) - gfx_line_height(cr, 22, FW_SEMILIGHT) / 2, 22, FW_SEMILIGHT,
			         pr ? th->accent : th->fg, TA_LEFT, w - 96, 1);
			if (ui_clicked(ui, aid)) {
				bool used[NTYPES] = {0};
				for (int i = 0; i < E.n[k]; i++) used[E.it[k][i].type] = true;
				int t = 0;
				while (t < ntypes(k) - 1 && used[t]) t++;
				E.it[k][E.n[k]].type = t;
				E.it[k][E.n[k]].val[0] = 0;
				E.focus_id = ui_idn("pe-it", k * 100 + E.n[k]);
				E.n[k]++;
				E.dirty = true;
			}
			y += 52 + 10;
		}
	}
	edit_field(ui, &y, w, F_WEB, _("website"), TB_URL);
	/* birthday: a picker field */
	edit_caption(ui, y, w, _("birthday"));
	y += 22;
	bool has = E.f[F_BDAY][0] != 0;
	Rect br = R(24, y, w - 48 - (has ? 52 : 0), 48);
	UiId bid = ui_id("pe-bday");
	ui_hit(ui, bid, br);
	gfx_fill_rect(cr, br, ui_pressed(ui, bid) ? th->accent : th->tb_fill);
	char bs[64];
	format_bday(E.f[F_BDAY], bs, sizeof bs);
	gfx_text(cr, has ? bs : _("add birthday"), br.x + 12, br.y + (48 - gfx_line_height(cr, 22, FW_REGULAR)) / 2, 22, FW_REGULAR,
	         has ? th->tb_fg : col_alpha(th->tb_fg, 0.5f), TA_LEFT, br.w - 24, 1);
	if (ui_clicked(ui, bid)) {
		int yy = 1990, m = 1, d = 1;
		if (has) {
			if (str_starts(E.f[F_BDAY], "--")) sscanf(E.f[F_BDAY], "--%d-%d", &m, &d);
			else sscanf(E.f[F_BDAY], "%d-%d-%d", &yy, &m, &d);
		}
		ui->focus = 0;
		dialog_date(_("birthday"), CLAMP(yy, 1900, 2199), CLAMP(m, 1, 12) - 1, CLAMP(d, 1, 31), bday_cb, NULL);
	}
	if (has && ui_icon_button(ui, ui_id("pe-bday-x"), R(w - 24 - 48, y, 48, 48), ICON_CLOSE, th->fg)) {
		E.f[F_BDAY][0] = 0;
		E.dirty = true;
	}
	y += 48 + 14;
	edit_caption(ui, y, w, _("notes"));
	y += 22;
	TextAreaInfo ti;
	UiId nid = ui_idn("pe-f", F_NOTES);
	if (E.focus_next) { E.focus_next = false; ui_focus(ui, nid); ui_key_event(ui, UK_END); }
	ui_textarea(ui, nid, R(24, y, w - 48, 96), E.notes, sizeof E.notes, NULL, 20, 0, &ti);
	if (ti.changed) E.dirty = true;
	if (ti.focused) { E.focus_y = ti.caret_y - 8; E.focus_h = ti.caret_h + 16; }
	y += ti.height + 30;
	E.h = y;
	ui_scroll_end(ui);
	/* keep the focused field (or the caret) visible above the keyboard */
	bool moved = ui->focus != E.last_focus || pc.h != E.last_view_h || (ti.focused && ti.changed);
	if (ui->focus && moved && E.focus_y >= 0) {
		if (E.focus_y < off) ui_scroll_to(&E.scroll, MAX(0, E.focus_y));
		else if (E.focus_y + E.focus_h > off + pc.h) ui_scroll_to(&E.scroll, E.focus_y + E.focus_h - pc.h);
	}
	E.last_focus = ui->focus;
	E.last_view_h = pc.h;

	AppBarBtn b[2] = {{ICON_SAVE, _("save")}, {ICON_CLOSE, _("cancel")}};
	int a = appbar_draw(ui, &g_bar_edit, area, b, 2, NULL, 0);
	if (a == 0) edit_save(ui);
	else if (a == 1) { ui->focus = 0; nav_pop(&g_nav, ui); }
}

static void discard_cb(int btn, void *ud) {
	if (btn == 0 && nav_page(&g_nav) == P_EDIT) { sh.ui.focus = 0; nav_pop(&g_nav, &sh.ui); }
}

/* ------------------------------------------------------------- settings -- */

static void draw_settings(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	ui_scroll_begin(ui, ui_id("ps-scroll"), &g_set_scroll, area, 560);
	cairo_translate(cr, area.x, area.y);
	double w = area.w;
	double y = ui_page_header(ui, 14, 6, _("People"), _("settings")) + 10;
	gfx_text(cr, _("sort list by"), 24, y, 20, FW_SEMILIGHT, th->sub, TA_LEFT, w - 48, 1);
	y += 32;
	bool s0 = ui_radio(ui, ui_id("ps-s0"), R(24, y, w - 48, 52), _("first name"), !g_sort_last);
	bool s1 = ui_radio(ui, ui_id("ps-s1"), R(24, y + 52, w - 48, 52), _("last name"), g_sort_last);
	y += 120;
	gfx_text(cr, _("display names by"), 24, y, 20, FW_SEMILIGHT, th->sub, TA_LEFT, w - 48, 1);
	y += 32;
	bool d0 = ui_radio(ui, ui_id("ps-d0"), R(24, y, w - 48, 52), _("first last"), !g_disp_last);
	bool d1 = ui_radio(ui, ui_id("ps-d1"), R(24, y + 52, w - 48, 52), _("last, first"), g_disp_last);
	y += 124;
	char t[96];
	snprintf(t, sizeof t, g_c.len == 1 ? _("%d contact") : _("%d contacts"), g_c.len);
	gfx_text(cr, t, 24, y, 17, FW_REGULAR, th->sub, TA_LEFT, w - 48, 1);
	ui_scroll_end(ui);
	if (s0 || s1 || d0 || d1) {
		if (s0 || s1) g_sort_last = s1;
		if (d0 || d1) g_disp_last = d1;
		save_settings();
		changed();
		tile_cache_update();
	}
}

/* ----------------------------------------------------------------- app --- */

static void page_draw(UI *ui, int page, Rect area, void *ud) {
	switch (page) {
	case P_CARD: if (g_cur) { draw_card(ui, area); break; } /* fallthrough */
	case P_ROOT: draw_root(ui, area); break;
	case P_EDIT: draw_edit(ui, area); break;
	case P_SETTINGS: draw_settings(ui, area); break;
	default: break;
	}
}

static void people_draw(UI *ui, Rect area) {
	load();
	rebuild();
	nav_draw(ui, &g_nav, area, page_draw, NULL);
}

static void people_open(const char *arg) {
	bool fresh = !g_loaded;
	load();
	g_open = true;
	if (fresh) nav_reset();
	g_jl.open = false;
	if (arg && *arg) {
		size_t l = strlen(arg);
		if (l > 4 && !strcasecmp(arg + l - 4, ".vcf")) import_file(arg);
		else if (str_eq(arg, "new") && nav_page(&g_nav) == P_ROOT) open_editor(&sh.ui, NULL);
	}
}

static bool people_back(void) {
	UI *ui = &sh.ui;
	switch (nav_page(&g_nav)) {
	case P_ROOT:
		if (appbar_back(&g_bar_root)) return true;
		if (jumplist_back(&g_jl)) return true;
		if (g_query[0]) { g_query[0] = 0; ui->focus = 0; return true; }
		return false;
	case P_CARD:
		if (appbar_back(&g_bar_card)) return true;
		return nav_pop(&g_nav, ui);
	case P_EDIT:
		if (appbar_back(&g_bar_edit)) return true;
		if (E.dirty) {
			dialog_message(_("discard changes?"), _("Your changes to this contact haven't been saved."), _("discard"),
			               _("keep editing"), discard_cb, NULL);
			return true;
		}
		ui->focus = 0;
		return nav_pop(&g_nav, ui);
	default:
		return nav_pop(&g_nav, ui);
	}
}

static void people_close(void) {
	save_now();
	unload();
	nav_reset();
	g_open = false;
	g_query[0] = g_query_used[0] = 0;
	g_pivot = (Pager){0};
	g_jl = (JumpList){0};
	memset(&E, 0, sizeof E);
	scan_free(g_scan);
	g_scan = NULL;
	if (g_img) imgcache_clear(g_img);
	if (g_tile_img) imgcache_clear(g_tile_img);
}

const AppDef app_people = {
	.id = "people", .name = N_("People"), .icon = ICON_PEOPLE, .color = 0,
	.default_size = TILE_MEDIUM, .keywords = "contacts address book phone vcard kişiler rehber",
	.open = people_open, .draw = people_draw, .back = people_back, .close = people_close,
	.tile = people_tile, .live = true,
};

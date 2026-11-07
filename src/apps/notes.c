/* Notes: a small OneNote-style notebook. A searchable list of notes (newest first) and a
 * full-page plain text editor that saves as you type. Stored in notes.json. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"

enum { P_LIST, P_EDIT };
#define NOTE_CAP 32768

typedef struct { char *text; time_t mtime; } Note;

static VEC(Note *) g_notes;
static bool g_loaded;
static Nav g_nav;
static AppBar g_bar, g_ebar;
static Scroll g_scroll, g_escroll;
static char g_q[128];
static Note *g_cur;                 /* note being edited (NULL = new, not stored yet) */
static char g_buf[NOTE_CAP];
static double g_last_caret = -1;
static Timer *g_save_timer;
static Note *g_menu_note;

/* ------------------------------------------------------------------ data -- */

static int cmp_mtime(const void *a, const void *b) {
	time_t x = (*(Note *const *)a)->mtime, y = (*(Note *const *)b)->mtime;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void ensure_loaded(void) {
	if (g_loaded) return;
	g_loaded = true;
	cJSON *root = app_json_load("notes.json");
	cJSON *arr = root ? cJSON_GetObjectItem(root, "notes") : NULL, *it;
	cJSON_ArrayForEach(it, arr) {
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
		if (!t || !*t) continue;
		Note *n = xcalloc(1, sizeof *n);
		n->text = xstrdup(t);
		n->mtime = (time_t)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "mtime"));
		vec_push(&g_notes, n);
	}
	cJSON_Delete(root);
	qsort(g_notes.items, g_notes.len, sizeof *g_notes.items, cmp_mtime);
}

static void save_now(void *ud) {
	cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "notes");
	for (int i = 0; i < g_notes.len; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "text", g_notes.items[i]->text);
		cJSON_AddNumberToObject(o, "mtime", (double)g_notes.items[i]->mtime);
		cJSON_AddItemToArray(arr, o);
	}
	app_json_save("notes.json", root);
	cJSON_Delete(root);
}

static void save_soon(void) {
	if (!g_save_timer) g_save_timer = timer_add(0, false, save_now, NULL);
	timer_set(g_save_timer, 1200);
}

static void flush_save(void) {
	if (g_save_timer) timer_set(g_save_timer, 0);
	save_now(NULL);
}

static bool blank(const char *s) {
	for (; *s; s++) if (*s != ' ' && *s != '\n' && *s != '\t') return false;
	return true;
}

static void note_remove(Note *n) {
	for (int i = 0; i < g_notes.len; i++) {
		if (g_notes.items[i] != n) continue;
		vec_remove(&g_notes, i);
		free(n->text);
		free(n);
		break;
	}
}

/* copies the editor buffer into the note, creating it on the first real character */
static void commit(bool final) {
	if (!g_cur && blank(g_buf)) return;
	if (!g_cur) {
		g_cur = xcalloc(1, sizeof *g_cur);
		g_cur->text = xstrdup("");
		vec_push(&g_notes, g_cur);
	}
	if (final && blank(g_buf)) {
		note_remove(g_cur);
		g_cur = NULL;
	} else if (!str_eq(g_cur->text, g_buf)) {
		free(g_cur->text);
		g_cur->text = xstrdup(g_buf);
		g_cur->mtime = time(NULL);
	}
	if (final) qsort(g_notes.items, g_notes.len, sizeof *g_notes.items, cmp_mtime);
	save_soon();
}

/* first non-empty line as the title, the following text (joined) as the snippet */
static void note_parts(const char *t, char *title, size_t tcap, char *snip, size_t scap) {
	while (*t == '\n' || *t == ' ') t++;
	size_t n = strcspn(t, "\n");
	snprintf(title, tcap, "%.*s", (int)MIN(n, tcap - 1), t);
	t += n;
	size_t k = 0;
	for (; *t && k + 1 < scap; t++) {
		char c = *t == '\n' || *t == '\t' ? ' ' : *t;
		if (c == ' ' && (k == 0 || snip[k - 1] == ' ')) continue;
		snip[k++] = c;
	}
	if (*t) {   /* don't cut a UTF-8 sequence in half */
		while (k > 0 && (snip[k - 1] & 0xC0) == 0x80) k--;
		if (k > 0 && (snip[k - 1] & 0x80)) k--;
	}
	snip[k] = 0;
}

static void fmt_when(char *buf, size_t n, time_t t) {
	struct tm tm, now;
	time_t tn = time(NULL);
	localtime_r(&t, &tm);
	localtime_r(&tn, &now);
	if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) i18n_fmt_time(buf, n, &tm, cfg.h24);
	else i18n_fmt_date_short(buf, n, &tm);
}

static void open_editor(Note *n) {
	g_cur = n;
	str_copy(g_buf, sizeof g_buf, n ? n->text : "");
	g_escroll = (Scroll){0};
	g_last_caret = -1;
	g_ebar = (AppBar){0};
	if (!n) ui_focus(&sh.ui, ui_id("notes-ed"));
	nav_push(&g_nav, &sh.ui, P_EDIT);
}

static void leave_editor(void) {
	sh.ui.focus = 0;
	commit(true);
	flush_save();
	nav_pop(&g_nav, &sh.ui);
}

/* ----------------------------------------------------------------- pages -- */

static void delete_cb(int b, void *ud) {
	Note *n = ud;
	if (b != 0) return;
	if (n == g_cur && nav_page(&g_nav) == P_EDIT) {
		sh.ui.focus = 0;
		if (g_cur) note_remove(g_cur);
		g_cur = NULL;
		nav_pop(&g_nav, &sh.ui);
	} else if (n) note_remove(n);
	flush_save();
	shell_invalidate();
}

static void confirm_delete(Note *n) {
	dialog_message(_("delete note?"), _("this note will be deleted."), _("delete"), _("cancel"), delete_cb, n);
}

static void menu_cb(int b, void *ud) {
	if (b == 0) confirm_delete(g_menu_note);
}

static void draw_list(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect a = appbar_content(&g_bar, area);
	double y = ui_page_header(ui, 14, a.y + 6, _("Notes"), _("all notes"));
	if (g_notes.len) {
		ui_textbox(ui, ui_id("notes-q"), R(a.x + 24, y + 4, a.w - 48, 44), g_q, sizeof g_q, _("search notes"), 0, NULL);
		y += 56;
	}
	Rect view = R(a.x, y, a.w, rect_bottom(a) - y);
	int idx[g_notes.len + 1], n = 0;
	for (int i = 0; i < g_notes.len; i++)
		if (!g_q[0] || str_contains_fold(g_notes.items[i]->text, g_q)) idx[n++] = i;
	if (!g_notes.len) app_empty(ui, R(a.x, y - 10, a.w, 100), _("no notes yet. tap + to add one."));
	else if (!n) app_empty(ui, R(a.x, y - 10, a.w, 100), _("no results"));
	const double RH = 86;
	double off = ui_scroll_begin(ui, ui_id("notes-list"), &g_scroll, view, n * RH + 20);
	cairo_translate(cr, view.x, view.y);
	for (int k = 0; k < n; k++) {
		double ry = k * RH;
		if (ry + RH < off || ry > off + view.h) continue;
		Note *note = g_notes.items[idx[k]];
		Rect rr = R(0, ry, view.w, RH);
		UiId id = ui_idp(note);
		ui_hit(ui, id, rr);
		if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.14f));
		char title[160], snip[200], when[64];
		note_parts(note->text, title, sizeof title, snip, sizeof snip);
		fmt_when(when, sizeof when, note->mtime);
		gfx_fill_rect(cr, R(24, ry + 12, 6, RH - 24), th->accent);
		double x = 44, w = view.w - x - 24;
		gfx_text(cr, title, x, ry + 8, 24, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
		gfx_text(cr, *snip ? snip : " ", x, ry + 40, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
		gfx_text(cr, when, x, ry + 59, 13, FW_REGULAR, th->accent, TA_LEFT, w, 1);
		if (ui_clicked(ui, id)) open_editor(note);
		if (ui_longpressed(ui, id)) {
			static const char *items[1];
			items[0] = _("delete");
			g_menu_note = note;
			dialog_menu(ui->p.x, ui->p.y, items, 1, menu_cb, NULL);
		}
	}
	ui_scroll_end(ui);
	AppBarBtn btns[] = {{ICON_ADD, _("new"), false}};
	if (appbar_draw(ui, &g_bar, area, btns, 1, NULL, 0) == 0) open_editor(NULL);
}

static void draw_editor(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect a = appbar_content(&g_ebar, area);
	double y = a.y + 8;
	char when[64] = "";
	if (g_cur && g_cur->mtime) fmt_when(when, sizeof when, g_cur->mtime);
	y += gfx_text(cr, i18n_upper(_("Notes")), a.x + 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, a.w - 36, 1);
	if (*when) gfx_text(cr, when, rect_right(a) - 24, a.y + 10, 15, FW_REGULAR, th->sub, TA_RIGHT, a.w / 2, 1);
	Rect view = R(a.x, y + 6, a.w, rect_bottom(a) - y - 6);
	static double content_h;
	double off = ui_scroll_begin(ui, ui_id("notes-escroll"), &g_escroll, view, MAX(content_h, view.h));
	cairo_translate(cr, view.x, view.y);
	TextAreaInfo ti;
	UiId tid = ui_id("notes-ed");
	ui_textarea(ui, tid, R(24, 8, view.w - 48, view.h - 16), g_buf, sizeof g_buf, _("type a note"), 22, TB_PLAIN, &ti);
	content_h = ti.height + 40;
	if (ti.focused && ti.caret_y != g_last_caret) {
		g_last_caret = ti.caret_y;
		if (ti.caret_y + ti.caret_h + 16 > off + view.h) ui_scroll_to(&g_escroll, ti.caret_y + ti.caret_h + 16 - view.h);
		else if (ti.caret_y - 8 < off) ui_scroll_to(&g_escroll, MAX(0, ti.caret_y - 8));
	}
	ui_scroll_end(ui);
	if (ti.changed) commit(false);
	AppBarBtn btns[] = {{ICON_DELETE, _("delete"), false}};
	if (appbar_draw(ui, &g_ebar, area, btns, 1, NULL, 0) == 0) {
		if (g_cur) confirm_delete(g_cur);
		else { g_buf[0] = 0; leave_editor(); }
	}
}

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	if (page == P_EDIT) draw_editor(ui, area);
	else draw_list(ui, area);
}

/* ---------------------------------------------------------------- AppDef -- */

static void notes_open(const char *arg) {
	ensure_loaded();
	if (arg && str_eq(arg, "new") && nav_page(&g_nav) == P_LIST) open_editor(NULL);
}

static void notes_draw(UI *ui, Rect area) {
	ensure_loaded();
	nav_draw(ui, &g_nav, area, page_fn, NULL);
}

static bool notes_back(void) {
	if (nav_page(&g_nav) == P_EDIT) {
		if (appbar_back(&g_ebar)) return true;
		leave_editor();
		return true;
	}
	if (appbar_back(&g_bar)) return true;
	if (g_q[0]) { g_q[0] = 0; sh.ui.focus = 0; return true; }
	return false;
}

static void notes_close(void) {
	if (nav_page(&g_nav) == P_EDIT) { commit(true); g_cur = NULL; }
	flush_save();
	nav_init(&g_nav, P_LIST);
	g_bar = g_ebar = (AppBar){0};
	g_scroll = (Scroll){0};
	g_q[0] = 0;
}

/* --------------------------------------------------------------- live tile */

static void notes_tile(UI *ui, Rect r, TileSize size, double now) {
	ensure_loaded();
	cairo_t *cr = ui->cr;
	if (!g_notes.len || size == TILE_SMALL) {
		icon_draw(cr, ICON_NOTE, rect_cx(r), rect_cy(r) - (size == TILE_SMALL ? 0 : 8), MIN(r.w, r.h) * 0.36, COL_WHITE);
		if (size != TILE_SMALL) app_tile_name(ui, r, tr(app_notes.name));
		return;
	}
	char title[160], snip[400];
	note_parts(g_notes.items[0]->text, title, sizeof title, snip, sizeof snip);
	double pad = 12, w = r.w - 2 * pad;
	double y = r.y + 8;
	y += gfx_text(cr, title, r.x + pad, y, 20, FW_SEMIBOLD, COL_WHITE, TA_LEFT, w, 1);
	int lines = (int)((r.h - 48 - (y - r.y)) / 19);
	if (*snip && lines > 0)
		gfx_text(cr, snip, r.x + pad, y, 15, FW_REGULAR, col_alpha(COL_WHITE, 0.85f), TA_LEFT, w, lines);
	app_tile_name(ui, r, tr(app_notes.name));
}

const AppDef app_notes = {
	.id = "notes", .name = N_("Notes"), .icon = ICON_NOTE, .color = 0,
	.keywords = "onenote memo text write",
	.open = notes_open, .draw = notes_draw, .back = notes_back, .close = notes_close,
	.tile = notes_tile,
};

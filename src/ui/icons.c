#include "icons.h"
#include "util.h"

#include <math.h>
#include <pango/pangocairo.h>

/* ---------------------------------------------------- Segoe MDL2 mapping --- */
/* Only glyphs whose code points are well known are mapped; every other icon
 * always uses the vector drawings below. */
static const uint16_t MDL2[ICON_COUNT] = {
	[ICON_BACK] = 0xE72B, [ICON_SEARCH] = 0xE721, [ICON_SETTINGS] = 0xE713, [ICON_PEOPLE] = 0xE716,
	[ICON_PERSON] = 0xE77B, [ICON_PHOTO] = 0xE8B9, [ICON_STORE] = 0xE719, [ICON_MAIL] = 0xE715,
	[ICON_CALENDAR] = 0xE787, [ICON_CLOCK] = 0xE121, [ICON_CALC] = 0xE8EF, [ICON_MUSIC] = 0xE8D6,
	[ICON_VIDEO] = 0xE714, [ICON_FOLDER] = 0xE8B7, [ICON_FILE] = 0xE7C3, [ICON_GLOBE] = 0xE774,
	[ICON_WIFI] = 0xE701, [ICON_BLUETOOTH] = 0xE702, [ICON_AIRPLANE] = 0xE709, [ICON_BRIGHTNESS] = 0xE706,
	[ICON_VOLUME] = 0xE767, [ICON_MUTE] = 0xE74F, [ICON_LOCK] = 0xE72E, [ICON_UNLOCK] = 0xE785,
	[ICON_KEYBOARD] = 0xE765, [ICON_BACKSPACE] = 0xE750, [ICON_CHECK] = 0xE73E, [ICON_CLOSE] = 0xE711,
	[ICON_ADD] = 0xE710, [ICON_DELETE] = 0xE74D, [ICON_EDIT] = 0xE70F, [ICON_PIN] = 0xE718,
	[ICON_UNPIN] = 0xE77A, [ICON_REFRESH] = 0xE72C, [ICON_DOWNLOAD] = 0xE896, [ICON_UPLOAD] = 0xE898,
	[ICON_SHARE] = 0xE72D, [ICON_STAR] = 0xE734, [ICON_STAR_FILLED] = 0xE735, [ICON_PLAY] = 0xE768,
	[ICON_PAUSE] = 0xE769, [ICON_NEXT] = 0xE893, [ICON_PREV] = 0xE892, [ICON_STOP] = 0xE71A,
	[ICON_INFO] = 0xE946, [ICON_LOCATION] = 0xE707, [ICON_PHONE] = 0xE717, [ICON_MESSAGE] = 0xE8BD,
	[ICON_MOON] = 0xE708, [ICON_POWER] = 0xE7E8, [ICON_CAMERA] = 0xE722, [ICON_CHEVRON_L] = 0xE76B,
	[ICON_CHEVRON_R] = 0xE76C, [ICON_CHEVRON_U] = 0xE70E, [ICON_CHEVRON_D] = 0xE70D, [ICON_MORE] = 0xE712,
	[ICON_LIST] = 0xE700, [ICON_MIC] = 0xE720, [ICON_COPY] = 0xE8C8, [ICON_UP] = 0xE74A, [ICON_DOWN] = 0xE74B,
	[ICON_CLOUD] = 0xE753, [ICON_SHUFFLE] = 0xE8B1, [ICON_REPEAT] = 0xE8EE, [ICON_REPEAT_ONE] = 0xE8ED,
	[ICON_STOPWATCH] = 0xE916, [ICON_WORLD] = 0xE909, [ICON_FLAG] = 0xE7C1, [ICON_SAVE] = 0xE74E,
	[ICON_SORT] = 0xE8CB, [ICON_SELECT] = 0xE762, [ICON_CUT] = 0xE8C6, [ICON_PASTE] = 0xE77F,
	[ICON_RENAME] = 0xE8AC, [ICON_NEW_FOLDER] = 0xE8F4, [ICON_TERMINAL] = 0xE756, [ICON_ZOOM_IN] = 0xE8A3,
	[ICON_ZOOM_OUT] = 0xE71F, [ICON_HISTORY] = 0xE81C, [ICON_GAME] = 0xE7FC, [ICON_FILTER] = 0xE71C,
	[ICON_HOME] = 0xE80F, [ICON_SLIDESHOW] = 0xE786, [ICON_CROP] = 0xE7A8, [ICON_LINK] = 0xE71B,
	[ICON_HELP] = 0xE897, [ICON_REMOVE] = 0xE738, [ICON_RECORD] = 0xE7C8, [ICON_EMAIL_SEND] = 0xE724,
	[ICON_TASKS] = 0xE7C4, [ICON_STORAGE] = 0xEDA2, [ICON_THEME] = 0xE790, [ICON_SMILEY] = 0xE76E,
	[ICON_BELL] = 0xEA8F, [ICON_EYE] = 0xE7B3, [ICON_ROTATE] = 0xE7AD, [ICON_HEART] = 0xEB51,
	[ICON_ALARM] = 0xE823, [ICON_TIMER] = 0xE916, [ICON_SOUND] = 0xE767, [ICON_DISPLAY] = 0xE7F4,
	[ICON_APPS] = 0xE8A9,
	[ICON_SUN] = 0xE706, [ICON_ENTER] = 0xE751, [ICON_SHIFT] = 0xE752, [ICON_FLASHLIGHT] = 0xE754,
	[ICON_NOTE] = 0xE8A5, [ICON_BATTERY] = 0xE83F, [ICON_USB] = 0xE88E, [ICON_TILES] = 0xECA5,
	[ICON_OPEN] = 0xE8E5, [ICON_LANGUAGE] = 0xF2B7,
};

static int g_mode = 0;          /* 0 auto, 1 font, 2 vector */
static const char *g_font;      /* detected family or NULL */
static bool g_detected;

void icons_set_mode(const char *mode) {
	g_mode = str_eq(mode, "font") ? 1 : str_eq(mode, "vector") ? 2 : 0;
}

static const char *icon_font(void) {
	if (!g_detected) {
		g_detected = true;
		if (gfx_has_font_family("Segoe MDL2 Assets")) g_font = "Segoe MDL2 Assets";
		else if (gfx_has_font_family("Segoe Fluent Icons")) g_font = "Segoe Fluent Icons";
	}
	return g_mode == 2 ? NULL : g_font;
}

bool icons_using_font(void) { return icon_font() != NULL; }

static bool draw_font_glyph(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c) {
	const char *fam = icon_font();
	if (!fam || !MDL2[ic]) return false;
	char buf[5];
	utf8_encode(MDL2[ic], buf);
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_new();
	pango_font_description_set_family(fd, fam);
	pango_font_description_set_absolute_size(fd, size * 0.92 * PANGO_SCALE);
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(l, buf, -1);
	PangoRectangle ink, logical;
	pango_layout_get_extents(l, &ink, &logical);
	double w = (double)ink.width / PANGO_SCALE, h = (double)ink.height / PANGO_SCALE;
	double ix = (double)ink.x / PANGO_SCALE, iy = (double)ink.y / PANGO_SCALE;
	cairo_save(cr);
	gfx_set_color(cr, c);
	cairo_move_to(cr, cx - w / 2 - ix, cy - h / 2 - iy);
	pango_cairo_show_layout(cr, l);
	cairo_restore(cr);
	g_object_unref(l);
	return true;
}

/* ------------------------------------------------------ vector drawings ---- */
/* All drawing happens in a 24x24 box centred on the origin, stroke width 2. */

#define MV(x, y) cairo_move_to(cr, x, y)
#define LN(x, y) cairo_line_to(cr, x, y)
#define CV(a, b, c, d, e, f) cairo_curve_to(cr, a, b, c, d, e, f)
#define CLOSE() cairo_close_path(cr)
#define STROKE() cairo_stroke(cr)
#define FILL() cairo_fill(cr)

static void rect(cairo_t *cr, double x, double y, double w, double h) { cairo_rectangle(cr, x, y, w, h); }
static void circle(cairo_t *cr, double x, double y, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, r, 0, 2 * M_PI);
}
static void arc(cairo_t *cr, double x, double y, double r, double a0, double a1) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, r, a0 * M_PI / 180, a1 * M_PI / 180);
}
static void poly(cairo_t *cr, const double *p, int n) {
	MV(p[0], p[1]);
	for (int i = 1; i < n; i++) LN(p[i * 2], p[i * 2 + 1]);
	CLOSE();
}

static void ic_back(cairo_t *cr) { MV(11, 0); LN(-10, 0); STROKE(); MV(-2, -8); LN(-10, 0); LN(-2, 8); STROKE(); }

static void ic_start(cairo_t *cr) {
	double a[] = {-11, -7, -1, -9, -1, -1, -11, -1}; poly(cr, a, 4); FILL();
	double b[] = {1, -9.5, 11, -11.5, 11, -1, 1, -1}; poly(cr, b, 4); FILL();
	double c[] = {-11, 1, -1, 1, -1, 9, -11, 7}; poly(cr, c, 4); FILL();
	double d[] = {1, 1, 11, 1, 11, 11.5, 1, 9.5}; poly(cr, d, 4); FILL();
}

static void ic_search(cairo_t *cr) {
	circle(cr, -2.5, -2.5, 7); STROKE();
	cairo_set_line_width(cr, 2.6); MV(2.8, 2.8); LN(10.5, 10.5); STROKE();
}

static void ic_gear(cairo_t *cr) {
	const int teeth = 8;
	for (int i = 0; i < teeth; i++) {
		double a = (i + 0.0) * 2 * M_PI / teeth, w = M_PI / teeth * 0.55;
		double ro = 11, ri = 8.2;
		double a0 = a - w * 1.6, a1 = a - w, a2 = a + w, a3 = a + w * 1.6;
		if (i == 0) MV(cos(a0) * ri, sin(a0) * ri); else LN(cos(a0) * ri, sin(a0) * ri);
		LN(cos(a1) * ro, sin(a1) * ro); LN(cos(a2) * ro, sin(a2) * ro); LN(cos(a3) * ri, sin(a3) * ri);
		double an = a + 2 * M_PI / teeth - w * 1.6;
		cairo_arc(cr, 0, 0, ri, a3, an);
	}
	CLOSE();
	circle(cr, 0, 0, 3.6);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	FILL();
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

static void ic_person(cairo_t *cr) {
	circle(cr, 0, -5.5, 4.5); STROKE();
	MV(-9, 11); CV(-9, 4, -5, 2, 0, 2); CV(5, 2, 9, 4, 9, 11); STROKE();
}

static void ic_people(cairo_t *cr) {
	circle(cr, -3.5, -5, 4); STROKE();
	MV(-11, 10); CV(-11, 4, -8, 2, -3.5, 2); CV(1, 2, 4, 4, 4, 10); STROKE();
	arc(cr, 5.5, -4, 3.2, -110, 90); STROKE();
	MV(8, 3); CV(10.5, 4, 11.5, 6, 11.5, 10); STROKE();
}

static void ic_photo(cairo_t *cr) {
	rect(cr, -10, -8, 20, 16); STROKE();
	circle(cr, -4.5, -3, 2); FILL();
	MV(-9, 7); LN(-3, 0.5); LN(1.5, 5); LN(5, 1.5); LN(9, 6); STROKE();
}

static void ic_store(cairo_t *cr) {
	rect(cr, -8, -5, 16, 15); STROKE();
	MV(-4, -5); LN(-4, -7); cairo_arc(cr, 0, -7, 4, M_PI, 2 * M_PI); LN(4, -5); STROKE();
}

static void ic_mail(cairo_t *cr) {
	rect(cr, -10, -7, 20, 14); STROKE();
	MV(-10, -7); LN(0, 2); LN(10, -7); STROKE();
}

static void ic_calendar(cairo_t *cr) {
	rect(cr, -9, -8, 18, 17); STROKE();
	rect(cr, -9, -8, 18, 4.5); FILL();
	MV(-4, -11); LN(-4, -6); MV(4, -11); LN(4, -6); STROKE();
	for (int r = 0; r < 2; r++) for (int c = 0; c < 3; c++) rect(cr, -6 + c * 4.6, -1 + r * 4.6, 3, 3);
	FILL();
}

static void ic_clock(cairo_t *cr) {
	circle(cr, 0, 0, 10); STROKE();
	MV(0, -6); LN(0, 0); LN(5, 3); STROKE();
}

static void ic_alarm(cairo_t *cr) {
	circle(cr, 0, 1.5, 8); STROKE();
	MV(0, -2.5); LN(0, 2); LN(3.5, 4); STROKE();
	MV(-10, -3); LN(-7, -8); MV(10, -3); LN(7, -8); STROKE();
}

static void ic_calc(cairo_t *cr) {
	rect(cr, -8, -11, 16, 22); STROKE();
	rect(cr, -5, -8, 10, 4); FILL();
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) rect(cr, -5 + c * 3.8, -1.5 + r * 3.8, 2.4, 2.4);
	FILL();
}

static void ic_music(cairo_t *cr) {
	cairo_save(cr); cairo_translate(cr, -3.5, 7); cairo_scale(cr, 1.15, 0.9); circle(cr, 0, 0, 3.6); cairo_restore(cr); FILL();
	MV(0.5, 7); LN(0.5, -9); LN(9, -6.5); LN(9, -3); LN(0.5, -5.5); STROKE();
}

static void ic_video(cairo_t *cr) {
	rect(cr, -10, -6, 14, 12); STROKE();
	double t[] = {6, -2, 10.5, -5, 10.5, 5, 6, 2}; poly(cr, t, 4); STROKE();
}

static void ic_folder(cairo_t *cr) {
	MV(-10, -7); LN(-3, -7); LN(-1, -4); LN(10, -4); LN(10, 8); LN(-10, 8); CLOSE(); STROKE();
}

static void ic_file(cairo_t *cr) {
	MV(-7, -10); LN(2, -10); LN(7, -5); LN(7, 10); LN(-7, 10); CLOSE(); STROKE();
	MV(2, -10); LN(2, -5); LN(7, -5); STROKE();
}

static void ic_globe(cairo_t *cr) {
	circle(cr, 0, 0, 10); STROKE();
	cairo_save(cr); cairo_scale(cr, 0.42, 1); circle(cr, 0, 0, 10); cairo_restore(cr); STROKE();
	MV(-10, 0); LN(10, 0); MV(-8.5, -5); LN(8.5, -5); MV(-8.5, 5); LN(8.5, 5); STROKE();
}

static void ic_wifi_arcs(cairo_t *cr, int level) {
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	double r[3] = {5.5, 9.5, 13.5};
	for (int i = 0; i < 3; i++) {
		if (level <= i && level >= 0) { cairo_save(cr); gfx_set_color(cr, (Color){0.5f, 0.5f, 0.5f, 0.45f}); }
		arc(cr, 0, 8, r[i], -135, -45); STROKE();
		if (level <= i && level >= 0) cairo_restore(cr);
	}
	circle(cr, 0, 8, 1.6); FILL();
}
static void ic_wifi(cairo_t *cr) { ic_wifi_arcs(cr, 3); }

static void ic_bluetooth(cairo_t *cr) {
	MV(-5.5, -5); LN(5.5, 5.5); LN(0, 10.5); LN(0, -10.5); LN(5.5, -5.5); LN(-5.5, 5); STROKE();
}

static void ic_airplane(cairo_t *cr) {
	double p[] = {0, -11, 1.8, -8, 1.8, -3, 11, 2, 11, 4, 1.8, 1.5, 1.8, 6.5, 4.5, 9, 4.5, 10.5,
	              0, 9.5, -4.5, 10.5, -4.5, 9, -1.8, 6.5, -1.8, 1.5, -11, 4, -11, 2, -1.8, -3, -1.8, -8};
	poly(cr, p, 18); FILL();
}

static void ic_brightness(cairo_t *cr) {
	circle(cr, 0, 0, 4.5); STROKE();
	for (int i = 0; i < 8; i++) {
		double a = i * M_PI / 4;
		MV(cos(a) * 7.5, sin(a) * 7.5); LN(cos(a) * 10.5, sin(a) * 10.5);
	}
	STROKE();
}

static void speaker(cairo_t *cr) {
	double s[] = {-10, -3.5, -6, -3.5, -1, -8, -1, 8, -6, 3.5, -10, 3.5}; poly(cr, s, 6); FILL();
}
static void ic_volume(cairo_t *cr) {
	speaker(cr);
	arc(cr, -1, 0, 5, -50, 50); STROKE();
	arc(cr, -1, 0, 9, -50, 50); STROKE();
}
static void ic_mute(cairo_t *cr) {
	speaker(cr);
	MV(3, -4); LN(10, 4); MV(10, -4); LN(3, 4); STROKE();
}

static void ic_lock(cairo_t *cr) {
	rect(cr, -7.5, -1, 15, 11); FILL();
	MV(-4.5, -1); LN(-4.5, -5); cairo_arc(cr, 0, -5, 4.5, M_PI, 2 * M_PI); LN(4.5, -1); STROKE();
}
static void ic_unlock(cairo_t *cr) {
	rect(cr, -7.5, -1, 15, 11); FILL();
	MV(-4.5, -1); LN(-4.5, -5); cairo_arc(cr, 0, -5, 4.5, M_PI, 2 * M_PI); LN(4.5, -3); STROKE();
}

static void ic_keyboard(cairo_t *cr) {
	rect(cr, -11, -7, 22, 14); STROKE();
	for (int r = 0; r < 2; r++) for (int c = 0; c < 6; c++) rect(cr, -8.5 + c * 3, -4.5 + r * 3, 1.6, 1.6);
	FILL();
	rect(cr, -5, 2.5, 10, 1.6); FILL();
}

static void ic_backspace(cairo_t *cr) {
	MV(-11, 0); LN(-5, -8); LN(10, -8); LN(10, 8); LN(-5, 8); CLOSE(); STROKE();
	MV(-1, -3.5); LN(5, 3.5); MV(5, -3.5); LN(-1, 3.5); STROKE();
}

static void ic_shift(cairo_t *cr) {
	double p[] = {0, -10, 9.5, 0, 4.5, 0, 4.5, 9, -4.5, 9, -4.5, 0, -9.5, 0}; poly(cr, p, 7); STROKE();
}
static void ic_caps(cairo_t *cr) {
	double p[] = {0, -11, 9.5, -1, 4.5, -1, 4.5, 6, -4.5, 6, -4.5, -1, -9.5, -1}; poly(cr, p, 7); FILL();
	rect(cr, -4.5, 8.5, 9, 2); FILL();
}

static void ic_enter(cairo_t *cr) {
	MV(10, -8); LN(10, 1); LN(-8, 1); STROKE();
	MV(-3, -4); LN(-8, 1); LN(-3, 6); STROKE();
}

static void ic_check(cairo_t *cr) { MV(-9, 1); LN(-3, 7.5); LN(10, -8); STROKE(); }
static void ic_close(cairo_t *cr) { MV(-8, -8); LN(8, 8); MV(8, -8); LN(-8, 8); STROKE(); }
static void ic_add(cairo_t *cr) { MV(-10, 0); LN(10, 0); MV(0, -10); LN(0, 10); STROKE(); }
static void ic_remove(cairo_t *cr) { MV(-10, 0); LN(10, 0); STROKE(); }

static void ic_delete(cairo_t *cr) {
	MV(-9, -6); LN(9, -6); STROKE();
	MV(-6.5, -6); LN(-5.5, 10); LN(5.5, 10); LN(6.5, -6); STROKE();
	MV(-3, -6); LN(-3, -9.5); LN(3, -9.5); LN(3, -6); STROKE();
	MV(-2, -2); LN(-2, 6); MV(2, -2); LN(2, 6); STROKE();
}

static void ic_edit(cairo_t *cr) {
	double p[] = {-9.5, 9.5, -8.5, 4, 4.5, -9, 9.5, -4, -3.5, 9}; poly(cr, p, 5); STROKE();
	MV(1, -6.5); LN(6.5, -1); STROKE();
}

static void ic_pin(cairo_t *cr) {
	double p[] = {-3.5, -10, 3.5, -10, 3.5, -3, 6.5, 1, -6.5, 1, -3.5, -3}; poly(cr, p, 6); FILL();
	MV(0, 1); LN(0, 10.5); STROKE();
}
static void ic_unpin(cairo_t *cr) { ic_pin(cr); }

static void ic_refresh(cairo_t *cr) {
	arc(cr, 0, 0, 8, -60, 220); STROKE();
	double p[] = {8.5, -10, 8.5, -2.5, 1, -2.5}; poly(cr, p, 3); FILL();
}

static void ic_download(cairo_t *cr) {
	MV(0, -10); LN(0, 4); STROKE(); MV(-6, -1.5); LN(0, 5); LN(6, -1.5); STROKE();
	MV(-9, 9.5); LN(9, 9.5); STROKE();
}
static void ic_upload(cairo_t *cr) {
	MV(0, 5); LN(0, -9); STROKE(); MV(-6, -3.5); LN(0, -10); LN(6, -3.5); STROKE();
	MV(-9, 9.5); LN(9, 9.5); STROKE();
}

static void ic_share(cairo_t *cr) {
	circle(cr, 6.5, -6.5, 3); STROKE(); circle(cr, -7, 0, 3); STROKE(); circle(cr, 6.5, 6.5, 3); STROKE();
	MV(-4.3, -1.4); LN(4, -5.2); MV(-4.3, 1.4); LN(4, 5.2); STROKE();
}

static void ic_heart(cairo_t *cr) {
	MV(0, 9.5); CV(-14, -1, -8, -10, 0, -4); CV(8, -10, 14, -1, 0, 9.5); CLOSE(); STROKE();
}

static void star_path(cairo_t *cr) {
	for (int i = 0; i < 10; i++) {
		double a = -M_PI / 2 + i * M_PI / 5, r = (i % 2) ? 4.6 : 11;
		if (i == 0) MV(cos(a) * r, sin(a) * r); else LN(cos(a) * r, sin(a) * r);
	}
	CLOSE();
}
static void ic_star(cairo_t *cr) { star_path(cr); STROKE(); }
static void ic_star_filled(cairo_t *cr) { star_path(cr); FILL(); }

static void ic_play(cairo_t *cr) { double p[] = {-6, -9, 9, 0, -6, 9}; poly(cr, p, 3); STROKE(); }
static void ic_pause(cairo_t *cr) { rect(cr, -7, -9, 4.5, 18); FILL(); rect(cr, 2.5, -9, 4.5, 18); FILL(); }
static void ic_next(cairo_t *cr) { double p[] = {-8, -8, 5, 0, -8, 8}; poly(cr, p, 3); FILL(); rect(cr, 6.5, -8, 2.5, 16); FILL(); }
static void ic_prev(cairo_t *cr) { double p[] = {8, -8, -5, 0, 8, 8}; poly(cr, p, 3); FILL(); rect(cr, -9, -8, 2.5, 16); FILL(); }
static void ic_stop(cairo_t *cr) { rect(cr, -8, -8, 16, 16); FILL(); }

static void ic_info(cairo_t *cr) {
	circle(cr, 0, 0, 10); STROKE();
	circle(cr, 0, -5, 1.4); FILL();
	MV(0, -1.5); LN(0, 6.5); STROKE();
}

static void ic_location(cairo_t *cr) {
	MV(0, 11); CV(-9, 1, -8, -2, -8, -4); cairo_arc(cr, 0, -4, 8, M_PI, 2 * M_PI); CV(8, -2, 9, 1, 0, 11); CLOSE(); STROKE();
	circle(cr, 0, -4, 2.8); STROKE();
}

static void ic_phone(cairo_t *cr) {
	MV(-8, -9); LN(-4, -9); LN(-2, -4); LN(-4.5, -2); CV(-3, 2, 0, 5, 4, 6.5); LN(6, 4); LN(11, 6); LN(11, 10); CV(-3, 11, -11, 2, -8, -9); CLOSE(); STROKE();
}

static void ic_message(cairo_t *cr) {
	MV(-10, -8); LN(10, -8); LN(10, 5); LN(-1, 5); LN(-6, 10); LN(-6, 5); LN(-10, 5); CLOSE(); STROKE();
}

static void ic_note(cairo_t *cr) {
	rect(cr, -8, -10, 16, 20); STROKE();
	MV(-4, -4); LN(4, -4); MV(-4, 0); LN(4, 0); MV(-4, 4); LN(1, 4); STROKE();
}

static void ic_flashlight(cairo_t *cr) {
	MV(-5, -10); LN(5, -10); LN(5, -6); LN(2.5, -3); LN(2.5, 10); LN(-2.5, 10); LN(-2.5, -3); LN(-5, -6); CLOSE(); STROKE();
	MV(0, -1); LN(0, 3); STROKE();
}

static void ic_rotate(cairo_t *cr) {
	rect(cr, -6, -4, 12, 8.5); STROKE();
	arc(cr, 0, 0, 10.5, 200, 330); STROKE();
	double p[] = {10.5, -3, 10.5, 3.5, 5, 0}; poly(cr, p, 3); FILL();
}

static void ic_moon(cairo_t *cr) {
	cairo_arc(cr, 0, 0, 10, 0.55 * M_PI, 1.9 * M_PI);
	cairo_arc_negative(cr, 3.5, -1, 8.2, 1.78 * M_PI, 0.62 * M_PI);
	CLOSE(); FILL();
}

static void ic_bell(cairo_t *cr) {
	MV(-8, 6); CV(-7, 4, -6, 3, -6, -2); CV(-6, -8, 6, -8, 6, -2); CV(6, 3, 7, 4, 8, 6); CLOSE(); STROKE();
	MV(-2.5, 9.5); LN(2.5, 9.5); STROKE();
	MV(0, -10); LN(0, -7); STROKE();
}

static void ic_power(cairo_t *cr) {
	arc(cr, 0, 1, 9, -60, 240); STROKE();
	MV(0, -11); LN(0, 1); STROKE();
}

static void ic_camera(cairo_t *cr) {
	MV(-10, -5); LN(-5, -5); LN(-3.5, -8); LN(3.5, -8); LN(5, -5); LN(10, -5); LN(10, 8); LN(-10, 8); CLOSE(); STROKE();
	circle(cr, 0, 1.5, 4); STROKE();
}

static void sun_rays(cairo_t *cr, double r0, double r1) {
	for (int i = 0; i < 8; i++) {
		double a = i * M_PI / 4;
		MV(cos(a) * r0, sin(a) * r0); LN(cos(a) * r1, sin(a) * r1);
	}
	STROKE();
}
static void ic_sun(cairo_t *cr) { circle(cr, 0, 0, 5); FILL(); sun_rays(cr, 7.5, 11); }

static void cloud(cairo_t *cr, double dy) {
	circle(cr, -4.5, 2 + dy, 4.5); circle(cr, 1, -1 + dy, 6); circle(cr, 6, 3 + dy, 4); rect(cr, -4.5, 2 + dy, 10.5, 5); FILL();
}
static void ic_cloud(cairo_t *cr) { cloud(cr, 0); }
static void ic_rain(cairo_t *cr) {
	cloud(cr, -3);
	for (int i = 0; i < 3; i++) { MV(-5 + i * 5, 5); LN(-6.5 + i * 5, 10); }
	STROKE();
}
static void ic_snow(cairo_t *cr) {
	cloud(cr, -3);
	for (int i = 0; i < 3; i++) { circle(cr, -5 + i * 5, 7.5 + (i % 2) * 2.5, 1.3); FILL(); }
}
static void ic_storm(cairo_t *cr) {
	cloud(cr, -3);
	double b[] = {1, 3.5, -3, 8.5, 0, 8.5, -1.5, 12, 4, 6.5, 1, 6.5, 3, 3.5}; poly(cr, b, 7); FILL();
}
static void ic_partly(cairo_t *cr) {
	cairo_save(cr); cairo_translate(cr, -3.5, -4); cairo_scale(cr, 0.7, 0.7); circle(cr, 0, 0, 5); FILL(); sun_rays(cr, 7.5, 11); cairo_restore(cr);
	cairo_save(cr); cairo_translate(cr, 2, 3); cairo_scale(cr, 0.85, 0.85); cloud(cr, 0); cairo_restore(cr);
}
static void ic_fog(cairo_t *cr) { cloud(cr, -4); MV(-9, 6); LN(9, 6); MV(-6, 10); LN(11, 10); STROKE(); }

static void ic_arrow_circle(cairo_t *cr) {
	circle(cr, 0, 0, 10.5); STROKE();
	MV(-5, 0); LN(5, 0); STROKE(); MV(1.5, -4.5); LN(6, 0); LN(1.5, 4.5); STROKE();
}

static void ic_chev_l(cairo_t *cr) { MV(4, -9); LN(-5, 0); LN(4, 9); STROKE(); }
static void ic_chev_r(cairo_t *cr) { MV(-4, -9); LN(5, 0); LN(-4, 9); STROKE(); }
static void ic_chev_u(cairo_t *cr) { MV(-9, 4); LN(0, -5); LN(9, 4); STROKE(); }
static void ic_chev_d(cairo_t *cr) { MV(-9, -4); LN(0, 5); LN(9, -4); STROKE(); }
static void ic_up(cairo_t *cr) { MV(0, 10); LN(0, -10); STROKE(); MV(-6, -4); LN(0, -10); LN(6, -4); STROKE(); }
static void ic_down(cairo_t *cr) { MV(0, -10); LN(0, 10); STROKE(); MV(-6, 4); LN(0, 10); LN(6, 4); STROKE(); }

static void ic_more(cairo_t *cr) { for (int i = -1; i <= 1; i++) { circle(cr, i * 7, 0, 1.8); FILL(); } }

static void ic_screenshot(cairo_t *cr) {
	MV(-10, -4); LN(-10, -10); LN(-4, -10); MV(4, -10); LN(10, -10); LN(10, -4);
	MV(10, 4); LN(10, 10); LN(4, 10); MV(-4, 10); LN(-10, 10); LN(-10, 4); STROKE();
	circle(cr, 0, 0, 3); FILL();
}

static void ic_theme(cairo_t *cr) {
	circle(cr, 0, 0, 10); STROKE();
	circle(cr, -4, -3, 1.8); FILL(); circle(cr, 1, -6, 1.8); FILL(); circle(cr, 5, -2, 1.8); FILL(); circle(cr, -3, 4, 1.8); FILL();
}

static void ic_storage(cairo_t *cr) {
	rect(cr, -10, -5, 20, 10); STROKE();
	circle(cr, 6, 0, 1.2); FILL();
	MV(-6, 0); LN(1, 0); STROKE();
}

static void ic_eye(cairo_t *cr) {
	MV(-11, 0); CV(-5, -8, 5, -8, 11, 0); CV(5, 8, -5, 8, -11, 0); CLOSE(); STROKE();
	circle(cr, 0, 0, 3.2); FILL();
}

static void ic_display(cairo_t *cr) {
	rect(cr, -6, -10, 12, 20); STROKE();
	MV(-2, 7); LN(2, 7); STROKE();
}

static void ic_list(cairo_t *cr) { MV(-10, -6); LN(10, -6); MV(-10, 0); LN(10, 0); MV(-10, 6); LN(10, 6); STROKE(); }

static void ic_mic(cairo_t *cr) {
	rect(cr, -3.5, -10, 7, 12); STROKE();
	MV(-7, -1); CV(-7, 8, 7, 8, 7, -1); STROKE();
	MV(0, 7); LN(0, 10.5); STROKE();
}

static void ic_copy(cairo_t *cr) {
	rect(cr, -3, -3, 12, 13); STROKE();
	MV(-6, 4); LN(-9, 4); LN(-9, -10); LN(3, -10); LN(3, -6.5); STROKE();
}

static void ic_open(cairo_t *cr) {
	MV(-3, -8); LN(-9, -8); LN(-9, 9); LN(8, 9); LN(8, 3); STROKE();
	MV(1, -10); LN(10, -10); LN(10, -1); MV(10, -10); LN(-1, 1); STROKE();
}

static void ic_apps(cairo_t *cr) {
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) rect(cr, -10 + c * 7.5, -10 + r * 7.5, 5, 5);
	FILL();
}

static void ic_tiles(cairo_t *cr) {
	rect(cr, -10, -10, 9, 9); FILL(); rect(cr, 1, -10, 9, 9); FILL();
	rect(cr, -10, 1, 9, 9); FILL(); rect(cr, 1, 1, 9, 9); FILL();
}

static void ic_language(cairo_t *cr) {
	MV(-10, -9); LN(4, -9); LN(4, 3); LN(-3, 3); LN(-6.5, 7); LN(-6.5, 3); LN(-10, 3); CLOSE(); STROKE();
	MV(-6.5, -4); LN(-1.5, -4); MV(-4, -6.5); LN(-4, -4); MV(-6, -1); CV(-3, -2, -2, -4, -2, -4); STROKE();
	MV(5, -2); LN(10, -2); LN(10, 9); LN(7, 9); LN(5, 11); LN(5, 9); STROKE();
}

static void ic_usb(cairo_t *cr) {
	MV(0, 10); LN(0, -9); STROKE(); circle(cr, 0, 10, 1.6); FILL();
	MV(0, 0); LN(-5, -3); LN(-5, -6); STROKE(); MV(0, 3); LN(5, 0); LN(5, -3); STROKE();
	double p[] = {-3, -9, 3, -9, 0, -12}; poly(cr, p, 3); FILL();
}

static void ic_smiley(cairo_t *cr) {
	circle(cr, 0, 0, 10); STROKE();
	circle(cr, -3.5, -3, 1.3); FILL(); circle(cr, 3.5, -3, 1.3); FILL();
	arc(cr, 0, 0.5, 6, 25, 155); STROKE();
}

static void ic_globe_search(cairo_t *cr) {
	cairo_save(cr); cairo_translate(cr, -2, -2); cairo_scale(cr, 0.75, 0.75); ic_globe(cr); cairo_restore(cr);
	cairo_set_line_width(cr, 2.2); MV(5, 5); LN(10.5, 10.5); STROKE();
}

static void ic_tasks(cairo_t *cr) {
	rect(cr, -10, -7, 11, 14); STROKE();
	MV(4, -7); LN(10, -7); LN(10, 7); LN(4, 7); STROKE();
}

static void ic_shuffle(cairo_t *cr) {
	MV(-10, -6); LN(-5, -6); CV(1, -6, -1, 6, 5, 6); LN(9, 6); STROKE();
	MV(-10, 6); LN(-5, 6); CV(-2.5, 6, -1.5, 3, -0.5, 1); MV(1.5, -3); CV(2.5, -5, 3, -6, 5, -6); LN(9, -6); STROKE();
	double a[] = {7, -9.5, 11, -6, 7, -2.5}; poly(cr, a, 3); FILL();
	double b[] = {7, 2.5, 11, 6, 7, 9.5}; poly(cr, b, 3); FILL();
}

static void repeat_path(cairo_t *cr) {
	MV(-9, 2); LN(-9, -5); LN(7, -5); STROKE();
	MV(9, -2); LN(9, 5); LN(-7, 5); STROKE();
	double a[] = {5, -8.5, 9.5, -5, 5, -1.5}; poly(cr, a, 3); FILL();
	double b[] = {-5, 1.5, -9.5, 5, -5, 8.5}; poly(cr, b, 3); FILL();
}
static void ic_repeat(cairo_t *cr) { repeat_path(cr); }
static void ic_repeat_one(cairo_t *cr) {
	repeat_path(cr);
	cairo_set_line_width(cr, 1.6); MV(-1, -1.5); LN(0.5, -2.5); LN(0.5, 2.5); STROKE();
}

static void ic_timer(cairo_t *cr) {
	MV(-7, -10); LN(7, -10); MV(-7, 10); LN(7, 10); STROKE();
	MV(-6, -10); CV(-6, -3, 0, -2, 0, 0); CV(0, 2, -6, 3, -6, 10); STROKE();
	MV(6, -10); CV(6, -3, 0, -2, 0, 0); CV(0, 2, 6, 3, 6, 10); STROKE();
	double s[] = {-4, 9, 4, 9, 0, 4}; poly(cr, s, 3); FILL();
}

static void ic_stopwatch(cairo_t *cr) {
	circle(cr, 0, 2, 8.5); STROKE();
	MV(-3, -10); LN(3, -10); MV(0, -10); LN(0, -6.5); STROKE();
	MV(0, 2); LN(3.5, -2.5); STROKE();
	MV(6.5, -6); LN(8.5, -8); STROKE();
}

static void ic_flag(cairo_t *cr) {
	MV(-7, 11); LN(-7, -10); STROKE();
	double p[] = {-7, -10, 8, -5.5, -7, -1}; poly(cr, p, 3); FILL();
}

static void ic_save(cairo_t *cr) {
	MV(-10, -10); LN(6, -10); LN(10, -6); LN(10, 10); LN(-10, 10); CLOSE(); STROKE();
	rect(cr, -5, -10, 9, 6); STROKE();
	rect(cr, -6, 2, 12, 8); STROKE();
}

static void ic_sort(cairo_t *cr) {
	MV(-5, 10); LN(-5, -9); STROKE(); MV(-9.5, -5); LN(-5, -10); LN(-0.5, -5); STROKE();
	MV(5, -10); LN(5, 9); STROKE(); MV(0.5, 5); LN(5, 10); LN(9.5, 5); STROKE();
}

static void ic_select(cairo_t *cr) {
	rect(cr, -10, -10, 8, 8); STROKE(); rect(cr, -10, 2, 8, 8); STROKE();
	MV(1, -6); LN(11, -6); MV(1, 6); LN(11, 6); STROKE();
	cairo_set_line_width(cr, 1.6); MV(-8.5, 6); LN(-6.5, 8); LN(-3.5, 4); STROKE();
}

static void ic_cut(cairo_t *cr) {
	circle(cr, -5, 7, 3.2); STROKE(); circle(cr, 5, 7, 3.2); STROKE();
	MV(-3, 4.5); LN(6, -10); MV(3, 4.5); LN(-6, -10); STROKE();
}

static void ic_paste(cairo_t *cr) {
	MV(-4, -8); LN(-9, -8); LN(-9, 10); LN(9, 10); LN(9, -8); LN(4, -8); STROKE();
	rect(cr, -4, -11, 8, 5); FILL();
	MV(-5, -1); LN(5, -1); MV(-5, 4); LN(5, 4); STROKE();
}

static void ic_rename(cairo_t *cr) {
	MV(-11, -5); LN(-11, 5); LN(3, 5); MV(8, 5); LN(11, 5); LN(11, -5); LN(8, -5); MV(3, -5); LN(-11, -5); STROKE();
	MV(5.5, -10); LN(5.5, 10); MV(3, -10); LN(8, -10); MV(3, 10); LN(8, 10); STROKE();
}

static void ic_new_folder(cairo_t *cr) {
	MV(1, 8); LN(-10, 8); LN(-10, -7); LN(-3, -7); LN(-1, -4); LN(10, -4); LN(10, -1); STROKE();
	MV(7, 2); LN(7, 11); MV(2.5, 6.5); LN(11.5, 6.5); STROKE();
}

static void ic_terminal(cairo_t *cr) {
	rect(cr, -11, -9, 22, 18); STROKE();
	MV(-7, -4); LN(-2, 0); LN(-7, 4); STROKE();
	MV(0, 5); LN(7, 5); STROKE();
}

static void ic_zoom_in(cairo_t *cr) {
	circle(cr, -2, -2, 7.5); STROKE();
	MV(3.5, 3.5); LN(10, 10); STROKE();
	MV(-6, -2); LN(2, -2); MV(-2, -6); LN(-2, 2); STROKE();
}
static void ic_zoom_out(cairo_t *cr) {
	circle(cr, -2, -2, 7.5); STROKE();
	MV(3.5, 3.5); LN(10, 10); STROKE();
	MV(-6, -2); LN(2, -2); STROKE();
}

static void ic_history(cairo_t *cr) {
	arc(cr, 1, 0, 9, -200, 140); STROKE();
	double p[] = {-11.5, -5, -5.5, -5, -8.5, 0}; poly(cr, p, 3); FILL();
	MV(1, -5); LN(1, 0); LN(5, 3); STROKE();
}

static void ic_game(cairo_t *cr) {
	MV(-6, -6); LN(6, -6); CV(10, -6, 11.5, 0, 11.5, 5); CV(11.5, 9, 8, 9.5, 6, 6); LN(4, 3); LN(-4, 3);
	LN(-6, 6); CV(-8, 9.5, -11.5, 9, -11.5, 5); CV(-11.5, 0, -10, -6, -6, -6); CLOSE(); STROKE();
	MV(-7, -2.5); LN(-7, 2.5); MV(-9.5, 0); LN(-4.5, 0); STROKE();
	circle(cr, 5.5, -2, 1.3); FILL(); circle(cr, 8, 1, 1.3); FILL();
}

static void ic_record(cairo_t *cr) { circle(cr, 0, 0, 10); STROKE(); circle(cr, 0, 0, 5.5); FILL(); }

static void ic_leaf(cairo_t *cr) {
	MV(-9, 10); CV(-9, -6, 0, -10, 10, -10); CV(10, 2, 4, 9, -5, 6); STROKE();
	MV(-9, 10); LN(4, -3); STROKE();
}

static void ic_mine(cairo_t *cr) {
	circle(cr, 0, 0, 6.5); FILL();
	for (int i = 0; i < 8; i++) {
		double a = i * M_PI / 4;
		MV(cos(a) * 5, sin(a) * 5); LN(cos(a) * (i % 2 ? 8.5 : 10.5), sin(a) * (i % 2 ? 8.5 : 10.5));
	}
	STROKE();
}

static void ic_filter(cairo_t *cr) {
	MV(-10, -9); LN(10, -9); LN(2.5, 0); LN(2.5, 9); LN(-2.5, 6); LN(-2.5, 0); CLOSE(); STROKE();
}

static void ic_disc(cairo_t *cr) { circle(cr, 0, 0, 10); STROKE(); circle(cr, 0, 0, 2.5); FILL(); arc(cr, 0, 0, 6.5, 200, 260); STROKE(); }

static void ic_home(cairo_t *cr) {
	MV(-11, 0); LN(0, -10); LN(11, 0); STROKE();
	MV(-7.5, -3); LN(-7.5, 10); LN(-2.5, 10); LN(-2.5, 3); LN(2.5, 3); LN(2.5, 10); LN(7.5, 10); LN(7.5, -3); STROKE();
}

static void ic_slideshow(cairo_t *cr) {
	rect(cr, -11, -8, 22, 16); STROKE();
	double p[] = {-3, -4.5, 5, 0, -3, 4.5}; poly(cr, p, 3); FILL();
}

static void ic_crop(cairo_t *cr) {
	MV(-6, -11); LN(-6, 6); LN(11, 6); MV(-11, -6); LN(6, -6); LN(6, 11); STROKE();
}

static void ic_snooze(cairo_t *cr) {
	MV(-10, -9); LN(-3, -9); LN(-10, -2); LN(-3, -2); STROKE();
	MV(1, -2); LN(10, -2); LN(1, 9); LN(10, 9); STROKE();
}

static void ic_email_send(cairo_t *cr) {
	double p[] = {-11, -9, 11, 0, -11, 9, -8, 0}; poly(cr, p, 4); STROKE();
	MV(-8, 0); LN(2, 0); STROKE();
}

static void ic_link(cairo_t *cr) {
	cairo_save(cr); cairo_rotate(cr, -M_PI / 4);
	cairo_new_sub_path(cr); cairo_arc(cr, -5, 0, 4.2, M_PI / 2, 3 * M_PI / 2); LN(-1, -4.2); MV(-1, 4.2); LN(-5, 4.2); STROKE();
	cairo_new_sub_path(cr); cairo_arc(cr, 5, 0, 4.2, -M_PI / 2, M_PI / 2); LN(1, 4.2); MV(1, -4.2); LN(5, -4.2); STROKE();
	MV(-4, 0); LN(4, 0); STROKE();
	cairo_restore(cr);
}

static void ic_help(cairo_t *cr) {
	circle(cr, 0, 0, 10); STROKE();
	MV(-3.5, -3); CV(-3.5, -7.5, 3.5, -7.5, 3.5, -3); CV(3.5, 0, 0, 0, 0, 3); STROKE();
	circle(cr, 0, 6.5, 1.3); FILL();
}

typedef void (*DrawFn)(cairo_t *);
static const DrawFn DRAW[ICON_COUNT] = {
	[ICON_BACK] = ic_back, [ICON_START] = ic_start, [ICON_SEARCH] = ic_search, [ICON_SETTINGS] = ic_gear,
	[ICON_PEOPLE] = ic_people, [ICON_PERSON] = ic_person, [ICON_PHOTO] = ic_photo, [ICON_STORE] = ic_store,
	[ICON_MAIL] = ic_mail, [ICON_CALENDAR] = ic_calendar, [ICON_CLOCK] = ic_clock, [ICON_ALARM] = ic_alarm,
	[ICON_CALC] = ic_calc, [ICON_MUSIC] = ic_music, [ICON_VIDEO] = ic_video, [ICON_FOLDER] = ic_folder,
	[ICON_FILE] = ic_file, [ICON_GLOBE] = ic_globe, [ICON_WIFI] = ic_wifi, [ICON_BLUETOOTH] = ic_bluetooth,
	[ICON_AIRPLANE] = ic_airplane, [ICON_BRIGHTNESS] = ic_brightness, [ICON_VOLUME] = ic_volume,
	[ICON_MUTE] = ic_mute, [ICON_LOCK] = ic_lock, [ICON_UNLOCK] = ic_unlock, [ICON_KEYBOARD] = ic_keyboard,
	[ICON_BACKSPACE] = ic_backspace, [ICON_SHIFT] = ic_shift, [ICON_CAPS] = ic_caps, [ICON_ENTER] = ic_enter,
	[ICON_CHECK] = ic_check, [ICON_CLOSE] = ic_close, [ICON_ADD] = ic_add, [ICON_REMOVE] = ic_remove,
	[ICON_DELETE] = ic_delete, [ICON_EDIT] = ic_edit, [ICON_PIN] = ic_pin, [ICON_UNPIN] = ic_unpin,
	[ICON_REFRESH] = ic_refresh, [ICON_DOWNLOAD] = ic_download, [ICON_UPLOAD] = ic_upload, [ICON_SHARE] = ic_share,
	[ICON_HEART] = ic_heart, [ICON_STAR] = ic_star, [ICON_STAR_FILLED] = ic_star_filled, [ICON_PLAY] = ic_play,
	[ICON_PAUSE] = ic_pause, [ICON_NEXT] = ic_next, [ICON_PREV] = ic_prev, [ICON_STOP] = ic_stop,
	[ICON_INFO] = ic_info, [ICON_LOCATION] = ic_location, [ICON_PHONE] = ic_phone, [ICON_MESSAGE] = ic_message,
	[ICON_NOTE] = ic_note, [ICON_FLASHLIGHT] = ic_flashlight, [ICON_ROTATE] = ic_rotate, [ICON_MOON] = ic_moon,
	[ICON_BELL] = ic_bell, [ICON_POWER] = ic_power, [ICON_CAMERA] = ic_camera, [ICON_SUN] = ic_sun,
	[ICON_CLOUD] = ic_cloud, [ICON_RAIN] = ic_rain, [ICON_SNOW] = ic_snow, [ICON_STORM] = ic_storm,
	[ICON_PARTLY] = ic_partly, [ICON_FOG] = ic_fog, [ICON_ARROW_CIRCLE] = ic_arrow_circle,
	[ICON_CHEVRON_L] = ic_chev_l, [ICON_CHEVRON_R] = ic_chev_r, [ICON_CHEVRON_U] = ic_chev_u,
	[ICON_CHEVRON_D] = ic_chev_d, [ICON_MORE] = ic_more, [ICON_SCREENSHOT] = ic_screenshot,
	[ICON_THEME] = ic_theme, [ICON_STORAGE] = ic_storage, [ICON_EYE] = ic_eye, [ICON_DISPLAY] = ic_display,
	[ICON_LIST] = ic_list, [ICON_MIC] = ic_mic, [ICON_COPY] = ic_copy, [ICON_OPEN] = ic_open, [ICON_UP] = ic_up,
	[ICON_DOWN] = ic_down, [ICON_APPS] = ic_apps, [ICON_TILES] = ic_tiles, [ICON_LANGUAGE] = ic_language,
	[ICON_SOUND] = ic_volume, [ICON_USB] = ic_usb, [ICON_SMILEY] = ic_smiley, [ICON_GLOBE_SEARCH] = ic_globe_search,
	[ICON_TASKS] = ic_tasks, [ICON_SHUFFLE] = ic_shuffle, [ICON_REPEAT] = ic_repeat, [ICON_REPEAT_ONE] = ic_repeat_one,
	[ICON_TIMER] = ic_timer, [ICON_STOPWATCH] = ic_stopwatch, [ICON_WORLD] = ic_globe, [ICON_FLAG] = ic_flag,
	[ICON_SAVE] = ic_save, [ICON_SORT] = ic_sort, [ICON_SELECT] = ic_select, [ICON_CUT] = ic_cut,
	[ICON_PASTE] = ic_paste, [ICON_RENAME] = ic_rename, [ICON_NEW_FOLDER] = ic_new_folder,
	[ICON_TERMINAL] = ic_terminal, [ICON_ZOOM_IN] = ic_zoom_in, [ICON_ZOOM_OUT] = ic_zoom_out,
	[ICON_HISTORY] = ic_history, [ICON_GAME] = ic_game, [ICON_RECORD] = ic_record, [ICON_LEAF] = ic_leaf,
	[ICON_MINE] = ic_mine, [ICON_FILTER] = ic_filter, [ICON_DISC] = ic_disc, [ICON_HOME] = ic_home,
	[ICON_SLIDESHOW] = ic_slideshow, [ICON_CROP] = ic_crop, [ICON_SNOOZE] = ic_snooze,
	[ICON_EMAIL_SEND] = ic_email_send, [ICON_LINK] = ic_link, [ICON_HELP] = ic_help,
};

static void begin(cairo_t *cr, double cx, double cy, double size, Color c) {
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, size / 24.0, size / 24.0);
	gfx_set_color(cr, c);
	cairo_set_line_width(cr, 2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
}

static void icon_raster(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c, int flags) {
	if (!draw_font_glyph(cr, ic, cx, cy, size, c)) {
		if (!DRAW[ic]) return;
		begin(cr, cx, cy, size, c);
		DRAW[ic](cr);
		cairo_restore(cr);
	}
	if (flags & ICON_FLAG_SLASH) {
		begin(cr, cx, cy, size, c);
		cairo_set_line_width(cr, 2.4);
		MV(-10, -10); LN(10, 10); STROKE();
		cairo_restore(cr);
	}
}

/* Rasterising the vector icons was the most expensive part of a Start frame on phone
 * CPUs: keep an alpha mask per (icon, device size) and only composite it. */
typedef struct {
	int ic, flags, dsz, half;
	cairo_surface_t *mask;
	uint64_t used;
} IconEnt;

#define ICACHE_N 256
static IconEnt g_icache[ICACHE_N];
static uint64_t g_itick;

void icon_draw_ex(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c, int flags) {
	if (ic <= ICON_NONE || ic >= ICON_COUNT) return;
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);
	if (m.xy != 0 || m.yx != 0 || m.xx <= 0 || fabs(m.xx - m.yy) > 1e-6 || size * m.xx > 400) {
		icon_raster(cr, ic, cx, cy, size, c, flags);
		return;
	}
	int dsz = (int)lround(size * m.xx * 4);   /* quarter-pixel steps */
	uint32_t h = ((uint32_t)ic * 2654435761u) ^ ((uint32_t)dsz * 40503u) ^ ((uint32_t)flags * 97u);
	IconEnt *e = NULL, *old = NULL;
	for (int i = 0; i < 8; i++) {
		IconEnt *p = &g_icache[(h + i) % ICACHE_N];
		if (p->mask && p->ic == ic && p->flags == flags && p->dsz == dsz) { e = p; break; }
		if (!old || p->used < old->used) old = p;
	}
	if (!e) {
		e = old;
		if (e->mask) cairo_surface_destroy(e->mask);
		double ds = dsz / 4.0;
		e->half = (int)ceil(ds * 0.75) + 2;
		e->mask = cairo_image_surface_create(CAIRO_FORMAT_A8, e->half * 2, e->half * 2);
		cairo_t *mc = cairo_create(e->mask);
		icon_raster(mc, ic, e->half, e->half, ds, COL_WHITE, flags);
		cairo_destroy(mc);
		e->ic = ic;
		e->flags = flags;
		e->dsz = dsz;
	}
	e->used = ++g_itick;
	double x = cx, y = cy;
	cairo_user_to_device(cr, &x, &y);
	cairo_save(cr);
	cairo_identity_matrix(cr);
	gfx_set_color(cr, c);
	cairo_mask_surface(cr, e->mask, round(x) - e->half, round(y) - e->half);
	cairo_restore(cr);
}

void icon_draw(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c) {
	icon_draw_ex(cr, ic, cx, cy, size, c, 0);
}

void icon_draw_battery(cairo_t *cr, double cx, double cy, double size, Color c, int pct, bool charging) {
	begin(cr, cx, cy, size, c);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	rect(cr, -11, -5.5, 19, 11); STROKE();
	rect(cr, 9, -2, 2.5, 4); FILL();
	double f = CLAMP(pct, 0, 100) / 100.0;
	if (f > 0) { rect(cr, -9, -3.5, 15 * f, 7); FILL(); }
	if (charging) {
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		double b[] = {1, -5, -4, 1, -0.5, 1, -2, 5.5, 4, -1, 0.5, -1}; poly(cr, b, 6); FILL();
		cairo_restore(cr);
	}
	cairo_restore(cr);
}

void icon_draw_signal(cairo_t *cr, double cx, double cy, double size, Color c, int bars) {
	begin(cr, cx, cy, size, c);
	for (int i = 0; i < 4; i++) {
		Color cc = i < bars ? c : col_alpha(c, 0.35f);
		gfx_set_color(cr, cc);
		rect(cr, -10 + i * 5.5, 9 - (i + 1) * 4.5, 3.5, (i + 1) * 4.5); FILL();
	}
	cairo_restore(cr);
}

void icon_draw_wifi_level(cairo_t *cr, double cx, double cy, double size, Color c, int level) {
	begin(cr, cx, cy, size, c);
	ic_wifi_arcs(cr, level);
	cairo_restore(cr);
}

void icon_draw_circle(cairo_t *cr, Icon ic, double cx, double cy, double dia, Color c, double ring) {
	gfx_stroke_circle(cr, cx, cy, dia / 2, ring, c);
	icon_draw(cr, ic, cx, cy, dia * 0.5, c);
}

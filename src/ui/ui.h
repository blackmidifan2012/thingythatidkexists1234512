#pragma once
/* Immediate-mode Metro UI toolkit.
 *
 * Widgets are drawn every frame and register "hit" rectangles; pointer events
 * are resolved against the hits of the *previous* frame and turn into
 * click / long-press / drag / fling events that widgets consume while drawing.
 * Everything is expressed in dp (480dp = width of a classic Windows Phone). */
#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "icons.h"

typedef uint32_t UiId;
UiId ui_id(const char *s);
UiId ui_idn(const char *s, int n);
UiId ui_idp(const void *p);

typedef struct Theme {
	bool dark;
	Color bg, fg, sub, dim, chrome, chrome2, accent, on_accent, tb_fill, tb_fg, scrim;
} Theme;

/* The 20 Windows Phone accent colours. */
#define ACCENT_COUNT 20
extern const uint32_t ACCENT_RGB[ACCENT_COUNT];
const char *accent_name(int i);   /* translated */
void theme_make(Theme *t, bool dark, Color accent);

enum { UL_BASE, UL_CHROME, UL_OSK, UL_PANEL, UL_DIALOG, UL_TOAST, UL_COUNT };
enum { UI_AXIS_X = 1, UI_AXIS_Y = 2 };

typedef enum {
	UK_NONE, UK_BACKSPACE, UK_DELETE, UK_ENTER, UK_LEFT, UK_RIGHT, UK_UP, UK_DOWN,
	UK_HOME, UK_END, UK_TAB, UK_ESC, UK_PGUP, UK_PGDN,
} UiKey;

#define UI_MAX_HITS 2048
#define UI_MAX_SCROLLERS 64

typedef struct { UiId id; Rect r; int layer; UiId scroller; bool captures; } UiHit;
typedef struct { UiId id; Rect r; int axes; UiId parent; } UiScroller;

typedef struct UI {
	cairo_t *cr;
	double w, h, scale, now, dt;
	Theme th;
	int layer;
	bool blocked;                 /* hit registration disabled (transitions) */

	UiHit hits[2][UI_MAX_HITS];
	int nhits[2];
	UiScroller scrollers[2][UI_MAX_SCROLLERS];
	int nscr[2];
	int hb;                       /* index being built (in a frame) or last completed (between frames) */
	bool in_frame;
	Rect clip, clip_stack[16];
	int nclip;
	UiId scr_stack[16];
	int nscr_stack;

	struct {
		bool down, moved, long_fired;
		double x, y, sx, sy, fx, fy, t0;
		UiId active, captured;
		int active_layer;
		UiId active_scroller;
		double hist[8][3];        /* t, x, y ring */
		int nhist;
		int source;               /* 0 touch, 1 mouse */
	} p;
	double mx, my;                /* hover position (mouse) */
	bool mouse_present;
	double wheel_x, wheel_y;

	UiId clicks[8];
	int nclicks;
	UiId longpress, released, fling_id;
	double fling_vx, fling_vy;
	UiId down_id;                 /* widget the finger just landed on */

	UiId focus;                   /* focused text field */
	char typed[256];
	UiKey keys[32];
	int nkeys;
	bool kbd_wanted;              /* set by a focused text field while drawing */
	int kbd_purpose;              /* OSK layout hint */
	double kbd_inset;             /* current OSK height (dp), set by the shell */

	bool animating;               /* some animation still running: draw another frame */
	bool redraw;                  /* state changed: draw another frame */
	uint32_t frame;
	double slop;
} UI;

/* ---- lifecycle (called by the shell) */
void ui_init(UI *ui);
void ui_begin(UI *ui, cairo_t *cr, double w, double h, double scale, double now);
void ui_end(UI *ui);

/* ---- raw input (called from the compositor input layer, coordinates in dp) */
void ui_pointer_down(UI *ui, double x, double y, double now, int source);
void ui_pointer_move(UI *ui, double x, double y, double now);
void ui_pointer_up(UI *ui, double x, double y, double now);
void ui_pointer_hover(UI *ui, double x, double y);
void ui_wheel(UI *ui, double dx, double dy);
void ui_longpress_fire(UI *ui);
void ui_cancel_pointer(UI *ui);
void ui_text_event(UI *ui, const char *utf8);
void ui_key_event(UI *ui, UiKey k);
/* Keyboard shortcuts for pages without a focused text field (calculator keys, game
 * controls). Both consume the event. Escape/BackSpace are not delivered: they mean Back. */
bool ui_key(UI *ui, UiKey k);
bool ui_char(UI *ui, uint32_t *cp);
/* Would the pointer at (x,y) land on any widget at or above `min_layer`? */
bool ui_hit_test(const UI *ui, double x, double y, int min_layer);
UiId ui_hit_id_at(const UI *ui, double x, double y);

/* ---- hit registration / queries (widgets) */
void ui_hit(UI *ui, UiId id, Rect r);                 /* r in current user space */
void ui_hit_drag(UI *ui, UiId id, Rect r);            /* captures the drag immediately (sliders...) */
Rect ui_to_screen(UI *ui, Rect r);
bool ui_clicked(UI *ui, UiId id);
bool ui_longpressed(UI *ui, UiId id);
bool ui_pressed(const UI *ui, UiId id);
bool ui_dragging(const UI *ui, UiId id);
bool ui_down_on(UI *ui, UiId id);                     /* finger just landed (one frame) */
bool ui_released(UI *ui, UiId id);
bool ui_drag_delta(UI *ui, UiId id, double *dx, double *dy);
bool ui_fling(UI *ui, UiId id, double *vx, double *vy);
/* Pointer position in the current user space. */
void ui_pointer_local(const UI *ui, double *x, double *y);
bool ui_pointer_in(const UI *ui, Rect r);             /* finger/pointer inside r (user space) */
bool ui_hover(UI *ui, Rect r);                        /* mouse hovering r (user space) */
bool ui_wheel_for(UI *ui, Rect r, double *dx, double *dy);

/* Registers a nested scroller (e.g. a swipeable list row) for gesture arbitration. */
void ui_scroller_push(UI *ui, UiId id, Rect view, int axes);
void ui_scroller_pop(UI *ui);
/* Makes `id` the owner of the current drag (long-press then drag). */
void ui_capture(UI *ui, UiId id);
/* Per-widget persistent scratch values (two doubles), garbage collected when unused. */
double *ui_state(UI *ui, UiId id, double init);

void ui_clip_push(UI *ui, Rect r);
void ui_clip_pop(UI *ui);
void ui_request_frame(UI *ui);

/* ---- animation helpers */
typedef enum {
	EASE_LINEAR, EASE_OUT_CUBIC, EASE_IN_CUBIC, EASE_INOUT_CUBIC, EASE_OUT_QUINT,
	EASE_OUT_EXPO, EASE_OUT_BACK, EASE_IN_QUAD, EASE_OUT_QUAD,
} Ease;
double ease(Ease e, double t);

typedef struct { double from, to, start, dur; Ease ease; bool active; } Anim;
void anim_start(Anim *a, double from, double to, double now, double dur, Ease e);
void anim_set(Anim *a, double v);
double anim_value(const Anim *a, double now);
bool anim_running(const Anim *a, double now);
/* Samples the animation and keeps the frame loop alive while it runs. */
double ui_anim(UI *ui, Anim *a);
/* Exponential smoothing towards a target; keeps frames alive until converged. */
double ui_smooth(UI *ui, double cur, double target, double rate);

/* ---- scrolling */
typedef struct { double off, vel; bool flinging; double target; bool has_target; } Scroll;
/* Pushes clip + scroller + translation. Returns the (clamped) offset. */
double ui_scroll_begin(UI *ui, UiId id, Scroll *s, Rect view, double content_h);
void ui_scroll_end(UI *ui);
void ui_scroll_to(Scroll *s, double off);
/* Horizontal pager (pages laid out side by side, snapping). */
typedef struct { double pos; int target; double vel; bool dragging; } Pager;
/* Registers the pager as the parent scroller of everything drawn until ui_pager_end(). */
void ui_pager_update(UI *ui, UiId id, Pager *pg, Rect view, int npages);
void ui_pager_end(UI *ui);
void ui_pager_goto(Pager *pg, int page);
bool ui_pager_page(const Pager *pg, int index, Rect view, Rect *out);

/* ---- layout helper */
typedef struct { double x, y, w; } Flow;
static inline Flow flow_make(double x, double y, double w) { return (Flow){x, y, w}; }
static inline Rect flow_row(Flow *f, double h) { Rect r = R(f->x, f->y, f->w, h); f->y += h; return r; }
static inline void flow_gap(Flow *f, double h) { f->y += h; }

/* ---- widgets */
typedef enum { BTN_OUTLINE, BTN_FILLED, BTN_ACCENT } ButtonStyle;
bool ui_button(UI *ui, UiId id, Rect r, const char *label, ButtonStyle st);
bool ui_button_enabled(UI *ui, UiId id, Rect r, const char *label, ButtonStyle st, bool enabled);
/* App-bar style circular icon button, optional label below. */
bool ui_circle_button(UI *ui, UiId id, double cx, double cy, double dia, Icon ic, const char *label);
bool ui_icon_button(UI *ui, UiId id, Rect r, Icon ic, Color c);
/* Standard two-line list row. Returns true when tapped. */
bool ui_row(UI *ui, UiId id, Rect r, const char *title, const char *subtitle, Icon ic);
bool ui_row_ex(UI *ui, UiId id, Rect r, const char *title, const char *subtitle, Icon ic,
               const char *trailing, bool chevron);
bool ui_toggle(UI *ui, UiId id, Rect r, const char *label, bool *v);
bool ui_slider(UI *ui, UiId id, Rect r, double *v, double lo, double hi);
bool ui_checkbox(UI *ui, UiId id, Rect r, const char *label, bool *v);
bool ui_radio(UI *ui, UiId id, Rect r, const char *label, bool selected);
enum { TB_PASSWORD = 1, TB_NUMBER = 2, TB_MULTILINE = 4, TB_URL = 8, TB_NOKBD = 16, TB_EMAIL = 32, TB_PLAIN = 64 };
/* Returns true when Enter was pressed. `*changed` is set when the text changed. */
bool ui_textbox(UI *ui, UiId id, Rect r, char *buf, size_t cap, const char *hint, int flags, bool *changed);
/* Multi-line, word-wrapped text editor. The box grows with the text: r.h is the minimum
 * height and info->height the height actually used. caret_y/caret_h (user space) let the
 * caller scroll the caret into view. TB_PLAIN draws the text on the page without a box
 * (note pages). */
typedef struct { double height, caret_y, caret_h; bool changed, focused; } TextAreaInfo;
void ui_textarea(UI *ui, UiId id, Rect r, char *buf, size_t cap, const char *hint, double size, int flags,
                 TextAreaInfo *info);
void ui_focus(UI *ui, UiId id);
void ui_progress_dots(UI *ui, Rect r, Color c);
void ui_progress_bar(UI *ui, Rect r, double v /*0..1, <0 = indeterminate*/);
/* "APP TITLE" + big lowercase page title. Returns y below the header. */
double ui_page_header(UI *ui, double x, double y, const char *app_title, const char *title);
/* Section heading in accent colour. */
double ui_section(UI *ui, Rect r, const char *label);
/* Letter tile used in jump lists. */
void ui_letter_tile(UI *ui, Rect r, const char *letter, bool enabled, bool pressed);
/* Metro pivot header row. Returns the y below it. */
double ui_pivot_headers(UI *ui, UiId id, Pager *pg, const char *const *titles, int n, double x, double y, double w);
/* ---- Windows Phone 8.1 application bar
 * A bar along the bottom of the page with up to four round icon buttons and a "..."
 * that expands it to show button labels and the menu items. Draw it after the page
 * content (so it is on top) and size the page with appbar_content(). */
#define APPBAR_H 56.0
#define APPBAR_MENU 100        /* appbar_draw() returns APPBAR_MENU + i for menu item i */
typedef struct { Icon icon; const char *label; bool disabled; } AppBarBtn;
typedef struct { bool open; double pos; bool minimized; } AppBar;
Rect appbar_content(const AppBar *ab, Rect area);   /* area minus the collapsed bar */
/* Returns the clicked button index, APPBAR_MENU + menu index, or -1. */
int appbar_draw(UI *ui, AppBar *ab, Rect area, const AppBarBtn *btns, int nbtn, const char *const *menu, int nmenu);
bool appbar_back(AppBar *ab);                        /* collapses an open bar */

/* Draws the tile-style "turnstile" 3D page flip of a snapshot. angle in degrees (0 = flat). */
void ui_draw_turnstile(cairo_t *cr, cairo_surface_t *snap, double sw, double sh, Rect region,
                       double angle_deg, double pivot_x, double alpha);

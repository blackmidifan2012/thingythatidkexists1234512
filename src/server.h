#pragma once
/* Compositor-wide state. A single instance (srv) exists for the process. */
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo.h>

#include "build_config.h"
#include "ui/gfx.h"

typedef struct DesktopApp DesktopApp;

/* A pixel layer drawn with cairo and shown through a wlr_scene_buffer. */
typedef struct UiLayer UiLayer;

typedef struct View {
	struct wl_list link;               /* srv.views, most recently used first */
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;
	char *app_id;
	char *title;
	DesktopApp *app;
	bool mapped;
	bool fullscreen;                   /* client asked for fullscreen */
	bool is_dialog;                    /* has a parent toplevel */
	struct View *parent;
	uint32_t pid;
	double mapped_at;
	int last_w, last_h;                /* last size we configured */
	struct wlr_xdg_toplevel_decoration_v1 *deco;   /* pending server-side decoration request */
	struct wl_listener map, unmap, commit, destroy, request_fullscreen, request_maximize,
		set_title, set_app_id, request_move, request_resize, set_parent;
} View;

typedef struct Output {
	struct wlr_output *wlr;
	struct wlr_scene_output *scene_output;
	struct wl_listener frame, request_state, destroy;
	int pw, ph;                        /* physical pixels */
	double scale;                      /* wlroots output scale: Linux apps' pixels per layout px */
	double ui_scale;                   /* the shell's pixels per dp, see output_apply_scale() */
	int lw, lh;                        /* logical (layout) size = pw/scale */
	bool blanked;
} Output;

typedef struct Server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *layout;
	struct wlr_scene_tree *t_base, *t_windows, *t_overlay, *t_cursor;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor;
	struct wlr_text_input_manager_v3 *text_input_mgr;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_xdg_activation_v1 *activation;
	const char *socket;

	struct wl_list outputs;            /* Output.link not used: single primary */
	Output *out;                       /* primary (phone) output */

	struct wl_list views;
	struct wl_list keyboards;
	struct wlr_keyboard vkb;           /* virtual keyboard used by the OSK and as fallback */
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *keymap;
	bool vkb_ready;

	/* text input relay */
	struct wlr_text_input_v3 *ti;      /* active (enabled) text input */
	bool ti_enabled;
	uint32_t ti_purpose, ti_hint;

	/* command line options */
	int opt_w, opt_h;
	double opt_scale;
	bool opt_demo;
	const char *opt_ipc_path;
	const char *opt_startup;
	bool mouse_visible;
	bool opt_nolock;
	bool idle_inhibited;

	struct wl_listener new_output, new_input, new_toplevel, new_popup, request_cursor,
		request_set_selection, cursor_motion, cursor_motion_abs, cursor_button, cursor_axis,
		cursor_frame, touch_down, touch_up, touch_motion, touch_cancel, touch_frame,
		new_text_input, new_decoration, activation_request, request_set_primary_selection,
		backend_destroy;
} Server;

extern Server srv;

/* ---- server.c */
void srv_quit(void);
void srv_schedule_frame(void);
void srv_set_keymap(const char *layout, const char *variant);
bool srv_capture_screen(cairo_surface_t **out);   /* composited output, async-free; see output.c */

/* ---- output.c */
UiLayer *uilayer_create(struct wlr_scene_tree *parent);
void uilayer_resize(UiLayer *l, int pw, int ph, double scale);
cairo_t *uilayer_begin(UiLayer *l);               /* cleared, transparent ARGB32 surface */
cairo_surface_t *uilayer_surface(UiLayer *l);
void uilayer_commit(UiLayer *l, bool opaque);
void uilayer_set_visible(UiLayer *l, bool v);
bool uilayer_visible(const UiLayer *l);
void output_init_primary(struct wlr_output *wlr);
void output_set_blanked(bool blank);
void output_reapply_scale(void);
double output_layout_per_dp(void);
void output_set_orientation(int orient);         /* ORIENT_*, see sys.h */                /* layout px (apps, cursor) per shell dp */
void output_request_capture(void (*cb)(cairo_surface_t *surf, void *ud), void *ud);
extern UiLayer *g_layer_base, *g_layer_overlay;

/* ---- input.c */
void input_init(void);
void input_set_keymap(struct xkb_keymap *km);
void input_inject_key(uint32_t keycode_evdev, bool pressed, uint32_t mods_depressed);
bool input_inject_keysym(xkb_keysym_t sym, uint32_t extra_mods);   /* press+release */
void input_inject_text(const char *utf8);                          /* via text-input or keys */
void input_sim_pointer(int type /*0 down 1 move 2 up*/, double x, double y);
void input_notify_activity(void);
double input_idle_seconds(void);
void input_focus_view(View *v);
void input_clear_focus(void);
void input_show_cursor(bool show);

/* ---- views.c */
void views_init(void);
View *view_from_surface(struct wlr_surface *s);
void view_activate(View *v);                       /* bring to front, focus, configure */
void view_close(View *v);
void views_relayout(void);                         /* resize all views to the app area */
View *views_foreground(void);
View *view_find_by_app(const DesktopApp *a);
int views_count_mapped(void);
View *view_at_point(double lx, double ly, struct wlr_surface **surf, double *sx, double *sy);
void views_set_visible_only(View *fg);             /* hide all but fg (or all when NULL) */

/* ---- textinput.c */
void textinput_init(void);
void textinput_focus_changed(struct wlr_surface *s);
bool textinput_active(void);
int textinput_purpose_flags(void);
bool textinput_commit(const char *utf8);

/* ---- ipc.c */
void ipc_init(const char *path);
void ipc_shutdown(void);

/* ---- notifyd.c */
void notifyd_init(void);
void notifyd_close(uint32_t id, uint32_t reason);
void notifyd_action(uint32_t id, const char *key);

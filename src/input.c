#include "server.h"
#include "config.h"
#include "i18n.h"
#include "proc.h"
#include "shell/shell.h"
#include "sys.h"
#include "util.h"

#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <math.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <libinput.h>

typedef struct Keyboard {
	struct wl_list link;
	struct wlr_keyboard *kb;
	struct wl_listener modifiers, key, destroy;
} Keyboard;

static double g_last_activity;
static bool g_super_down, g_super_used, g_alt_down;
static int g_ntouch_devices;
static struct wlr_cursor_shape_manager_v1 *g_shape_mgr;
static struct wl_listener g_shape_listener;

/* where each active touch / the mouse button press is routed */
enum { T_NONE, T_SHELL, T_CLIENT };
static int g_touch_target[16];
static int g_shell_touch_id = -1;
static int g_mouse_target = T_NONE;
static struct wlr_surface *g_pointer_focus_surface;

/* key repeat for keys consumed by the shell */
static Timer *g_repeat_timer;
static struct { xkb_keysym_t sym; char utf8[16]; uint32_t mods; bool active; } g_rep;

/* ---------------------------------------------------------------- activity */

void input_notify_activity(void) {
	g_last_activity = now_sec();
	if (srv.idle_notifier) wlr_idle_notifier_v1_notify_activity(srv.idle_notifier, srv.seat);
	shell_activity();
}

double input_idle_seconds(void) { return now_sec() - g_last_activity; }

/* --------------------------------------------------------------- keymaps -- */

static struct xkb_keymap *make_keymap(const char *layout, const char *variant) {
	struct xkb_rule_names names = {0};
	names.layout = layout && *layout ? layout : "us";
	names.variant = variant && *variant ? variant : NULL;
	names.options = getenv("XKB_DEFAULT_OPTIONS");
	struct xkb_keymap *km = xkb_keymap_new_from_names(srv.xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		names.layout = "us";
		names.variant = NULL;
		km = xkb_keymap_new_from_names(srv.xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	return km;
}

void input_set_keymap(struct xkb_keymap *km) {
	if (!km) return;
	if (srv.keymap) xkb_keymap_unref(srv.keymap);
	srv.keymap = xkb_keymap_ref(km);
	if (srv.vkb_ready) wlr_keyboard_set_keymap(&srv.vkb, km);
	Keyboard *k;
	wl_list_for_each(k, &srv.keyboards, link) wlr_keyboard_set_keymap(k->kb, km);
}

void srv_set_keymap(const char *layout, const char *variant) {
	struct xkb_keymap *km = make_keymap(layout, variant);
	if (!km) return;
	input_set_keymap(km);
	xkb_keymap_unref(km);
}

/* ------------------------------------------------------ key injection ---- */

static void send_mods(uint32_t depressed) {
	struct wlr_keyboard_modifiers m = {.depressed = depressed};
	wlr_seat_keyboard_notify_modifiers(srv.seat, &m);
}

void input_inject_key(uint32_t keycode, bool pressed, uint32_t mods_depressed) {
	if (!srv.seat->keyboard_state.focused_surface) return;
	if (!wlr_seat_get_keyboard(srv.seat)) wlr_seat_set_keyboard(srv.seat, &srv.vkb);
	send_mods(mods_depressed);
	wlr_seat_keyboard_notify_key(srv.seat, now_msec(), keycode,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

bool input_inject_keysym(xkb_keysym_t sym, uint32_t extra_mods) {
	struct xkb_keymap *km = srv.keymap;
	if (!km || !srv.seat->keyboard_state.focused_surface) return false;
	xkb_keycode_t lo = xkb_keymap_min_keycode(km), hi = xkb_keymap_max_keycode(km);
	for (xkb_keycode_t kc = lo; kc <= hi; kc++) {
		xkb_layout_index_t nl = xkb_keymap_num_layouts_for_key(km, kc);
		for (xkb_layout_index_t lay = 0; lay < nl && lay < 1; lay++) {
			xkb_level_index_t nlev = xkb_keymap_num_levels_for_key(km, kc, lay);
			for (xkb_level_index_t lev = 0; lev < nlev; lev++) {
				const xkb_keysym_t *syms;
				int ns = xkb_keymap_key_get_syms_by_level(km, kc, lay, lev, &syms);
				if (ns != 1 || syms[0] != sym) continue;
				xkb_mod_mask_t masks[4];
				size_t nm = xkb_keymap_key_get_mods_for_level(km, kc, lay, lev, masks, 4);
				uint32_t mods = nm ? masks[0] : 0;
				mods |= extra_mods;
				input_inject_key(kc - 8, true, mods);
				input_inject_key(kc - 8, false, mods);
				send_mods(0);
				return true;
			}
		}
	}
	return false;
}

void input_inject_text(const char *utf8) {
	if (textinput_active() && textinput_commit(utf8)) return;
	for (const char *p = utf8; *p;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if (cp == '\n') { input_inject_keysym(XKB_KEY_Return, 0); continue; }
		xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
		if (sym != XKB_KEY_NoSymbol) input_inject_keysym(sym, 0);
	}
}

/* -------------------------------------------------------------- focusing -- */

void input_focus_view(View *v) {
	struct wlr_seat *seat = srv.seat;
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;
	struct wlr_surface *surf = v ? v->xdg->base->surface : NULL;
	if (prev == surf) return;
	if (prev) {
		struct wlr_xdg_toplevel *pt = wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (pt) wlr_xdg_toplevel_set_activated(pt, false);
	}
	if (!v) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		textinput_focus_changed(NULL);
		return;
	}
	wlr_xdg_toplevel_set_activated(v->xdg, true);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (!kb) kb = &srv.vkb;
	wlr_seat_keyboard_notify_enter(seat, surf, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	textinput_focus_changed(surf);
}

void input_clear_focus(void) { input_focus_view(NULL); }

void input_show_cursor(bool show) {
	if (show == srv.mouse_visible) return;
	srv.mouse_visible = show;
	if (show) wlr_cursor_set_xcursor(srv.cursor, srv.xcursor, "default");
	else wlr_cursor_unset_image(srv.cursor);
}

/* -------------------------------------------------------------- keyboard -- */

static void repeat_fire(void *ud) {
	if (!g_rep.active) return;
	shell_key(g_rep.sym, g_rep.utf8, true, g_rep.mods);
	timer_set(g_repeat_timer, 35);
}

static bool is_modifier_sym(xkb_keysym_t s) {
	return s == XKB_KEY_Shift_L || s == XKB_KEY_Shift_R || s == XKB_KEY_Control_L || s == XKB_KEY_Control_R ||
	       s == XKB_KEY_Alt_L || s == XKB_KEY_Alt_R || s == XKB_KEY_Super_L || s == XKB_KEY_Super_R ||
	       s == XKB_KEY_Meta_L || s == XKB_KEY_Meta_R || s == XKB_KEY_Caps_Lock || s == XKB_KEY_ISO_Level3_Shift;
}

static bool shell_takes_keys(void) {
	if (sh.screen != SCR_EXT) return true;
	if (sh.ui.focus) return true;
	if (dialog_active()) return true;
	return false;
}

/* Returns true if the key was consumed by a compositor binding. */
static bool handle_hotkeys(xkb_keysym_t sym, bool pressed, uint32_t mods) {
	bool ctrl = mods & WLR_MODIFIER_CTRL, alt = mods & WLR_MODIFIER_ALT, shift = mods & WLR_MODIFIER_SHIFT;

	if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R || sym == XKB_KEY_Meta_L) {
		if (pressed) { g_super_down = true; g_super_used = false; }
		else {
			g_super_down = false;
			if (!g_super_used) shell_go_home();
		}
		return true;
	}
	if (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R) g_alt_down = pressed;
	if (!pressed) {
		/* swallow the release of keys whose press we consumed */
		return false;
	}

	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (srv.session) wlr_session_change_vt(srv.session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
		return true;
	}
	if (ctrl && alt && sym == XKB_KEY_BackSpace) { srv_quit(); return true; }

	if (g_super_down) {
		g_super_used = true;
		switch (sym) {
		case XKB_KEY_l: case XKB_KEY_L: shell_lock(false); return true;
		case XKB_KEY_a: case XKB_KEY_A: case XKB_KEY_n: case XKB_KEY_N: shell_toggle_action_center(!sh.ac_target_open); return true;
		case XKB_KEY_Tab: shell_open_switcher(); return true;
		case XKB_KEY_s: case XKB_KEY_S: case XKB_KEY_slash: case XKB_KEY_space: shell_open_search(); return true;
		case XKB_KEY_b: case XKB_KEY_B: case XKB_KEY_BackSpace: shell_back(); return true;
		case XKB_KEY_q: case XKB_KEY_Q: {
			if (sh.screen == SCR_EXT && sh.cur_view) view_close(sh.cur_view);
			else if (sh.screen == SCR_APP && sh.cur_app) shell_close_app(sh.cur_app);
			return true;
		}
		case XKB_KEY_Print: shell_screenshot(); return true;
		case XKB_KEY_k: case XKB_KEY_K: shell_show_keyboard(!osk_wanted()); return true;
		case XKB_KEY_p: case XKB_KEY_P: shell_power_menu(); return true;
		default: break;
		}
	}
	if (alt && sym == XKB_KEY_Tab) { shell_open_switcher(); return true; }
	if (sym == XKB_KEY_Print) { shell_screenshot(); return true; }

	switch (sym) {
	case XKB_KEY_XF86AudioRaiseVolume: sys_set_volume(sys_volume() + 5); sys_set_mute(false); sh.volume_hud_t = now_sec(); shell_invalidate(); return true;
	case XKB_KEY_XF86AudioLowerVolume: sys_set_volume(sys_volume() - 5); sh.volume_hud_t = now_sec(); shell_invalidate(); return true;
	case XKB_KEY_XF86AudioMute: sys_set_mute(!sys_muted()); sh.volume_hud_t = now_sec(); shell_invalidate(); return true;
	case XKB_KEY_XF86MonBrightnessUp: sys_set_brightness(sys_brightness() + 10); sh.volume_hud_t = -now_sec(); shell_invalidate(); return true;
	case XKB_KEY_XF86MonBrightnessDown: sys_set_brightness(sys_brightness() - 10); sh.volume_hud_t = -now_sec(); shell_invalidate(); return true;
	case XKB_KEY_XF86PowerOff: case XKB_KEY_XF86Sleep:
		if (sh.screen_off) shell_wake(); else shell_lock(true);
		return true;
	case XKB_KEY_XF86Back: shell_back(); return true;
	case XKB_KEY_XF86HomePage: case XKB_KEY_XF86Go: shell_go_home(); return true;
	case XKB_KEY_XF86Search: shell_open_search(); return true;
	default: break;
	}
	(void)shift;
	return false;
}

static void keyboard_modifiers(struct wl_listener *l, void *data) {
	Keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(srv.seat, k->kb);
	if (!shell_takes_keys()) wlr_seat_keyboard_notify_modifiers(srv.seat, &k->kb->modifiers);
}

static void keyboard_key(struct wl_listener *l, void *data) {
	Keyboard *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *ev = data;
	uint32_t keycode = ev->keycode + 8;
	const xkb_keysym_t *syms;
	int ns = xkb_state_key_get_syms(k->kb->xkb_state, keycode, &syms);
	bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	uint32_t mods = wlr_keyboard_get_modifiers(k->kb);
	input_notify_activity();
	bool was_off = sh.screen_off;
	if (sh.screen_off && pressed) { shell_wake(); }   /* wake */
	/* the power key press that woke the screen must not also blank it again */
	if (was_off && pressed && ns > 0 && (syms[0] == XKB_KEY_XF86PowerOff || syms[0] == XKB_KEY_XF86Sleep)) return;

	bool handled = false;
	for (int i = 0; i < ns && !handled; i++) handled = handle_hotkeys(syms[i], pressed, mods);
	if (handled) return;
	xkb_keysym_t sym = ns > 0 ? syms[0] : XKB_KEY_NoSymbol;

	if (shell_takes_keys() || (g_rep.active && !pressed)) {
		char utf8[16] = "";
		if (pressed) xkb_state_key_get_utf8(k->kb->xkb_state, keycode, utf8, sizeof utf8);
		shell_key(sym, utf8, pressed, mods);
		if (pressed && !is_modifier_sym(sym)) {
			g_rep.sym = sym;
			str_copy(g_rep.utf8, sizeof g_rep.utf8, utf8);
			g_rep.mods = mods;
			g_rep.active = true;
			if (!g_repeat_timer) g_repeat_timer = timer_add(0, false, repeat_fire, NULL);
			timer_set(g_repeat_timer, 420);
		} else if (!pressed) {
			g_rep.active = false;
			if (g_repeat_timer) timer_set(g_repeat_timer, 0);
		}
		return;
	}
	g_rep.active = false;
	wlr_seat_set_keyboard(srv.seat, k->kb);
	wlr_seat_keyboard_notify_key(srv.seat, ev->time_msec, ev->keycode, ev->state);
}

static void keyboard_destroy(struct wl_listener *l, void *data) {
	Keyboard *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

static void new_keyboard(struct wlr_input_device *dev) {
	struct wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
	Keyboard *k = xcalloc(1, sizeof *k);
	k->kb = kb;
	if (srv.keymap) wlr_keyboard_set_keymap(kb, srv.keymap);
	wlr_keyboard_set_repeat_info(kb, 25, 600);
	k->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&kb->events.modifiers, &k->modifiers);
	k->key.notify = keyboard_key;
	wl_signal_add(&kb->events.key, &k->key);
	k->destroy.notify = keyboard_destroy;
	wl_signal_add(&dev->events.destroy, &k->destroy);
	wlr_seat_set_keyboard(srv.seat, kb);
	wl_list_insert(&srv.keyboards, &k->link);
}

/* --------------------------------------------------------- pointer/touch -- */

static void update_caps(void) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
	if (g_ntouch_devices > 0) caps |= WL_SEAT_CAPABILITY_TOUCH;
	wlr_seat_set_capabilities(srv.seat, caps);
}

static void configure_libinput(struct wlr_input_device *dev) {
	if (!wlr_input_device_is_libinput(dev)) return;
	struct libinput_device *ld = wlr_libinput_get_device_handle(dev);
	if (!ld) return;
	if (libinput_device_config_tap_get_finger_count(ld) > 0) {
		libinput_device_config_tap_set_enabled(ld, LIBINPUT_CONFIG_TAP_ENABLED);
		libinput_device_config_tap_set_drag_enabled(ld, LIBINPUT_CONFIG_DRAG_ENABLED);
	}
	if (libinput_device_config_scroll_has_natural_scroll(ld))
		libinput_device_config_scroll_set_natural_scroll_enabled(ld, 1);
}

static void new_input(struct wl_listener *l, void *data) {
	struct wlr_input_device *dev = data;
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(dev);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(srv.cursor, dev);
		configure_libinput(dev);
		input_show_cursor(true);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		wlr_cursor_attach_input_device(srv.cursor, dev);
		g_ntouch_devices++;
		break;
	default:
		break;
	}
	update_caps();
}

static void request_cursor(struct wl_listener *l, void *data) {
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	struct wlr_seat_client *focused = srv.seat->pointer_state.focused_client;
	if (focused == ev->seat_client && srv.mouse_visible)
		wlr_cursor_set_surface(srv.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void request_set_selection(struct wl_listener *l, void *data) {
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(srv.seat, ev->source, ev->serial);
}

static void request_set_primary_selection(struct wl_listener *l, void *data) {
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(srv.seat, ev->source, ev->serial);
}

static void shape_request(struct wl_listener *l, void *data) {
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (srv.seat->pointer_state.focused_client != ev->seat_client->seat->pointer_state.focused_client && 0) return;
	if (srv.seat->pointer_state.focused_client == NULL) return;
	if (ev->seat_client != srv.seat->pointer_state.focused_client) return;
	if (srv.mouse_visible)
		wlr_cursor_set_xcursor(srv.cursor, srv.xcursor, wlr_cursor_shape_v1_name(ev->shape));
}

/* Finds the layout (dp) coordinates of the cursor. */
static void cursor_pos(double *x, double *y) { *x = srv.cursor->x; *y = srv.cursor->y; }

static void pointer_to_client(uint32_t time) {
	double sx, sy;
	struct wlr_surface *surf = NULL;
	View *v = view_at_point(srv.cursor->x, srv.cursor->y, &surf, &sx, &sy);
	if (!v || !surf) {
		wlr_seat_pointer_notify_clear_focus(srv.seat);
		g_pointer_focus_surface = NULL;
		return;
	}
	if (surf != g_pointer_focus_surface) {
		wlr_seat_pointer_notify_enter(srv.seat, surf, sx, sy);
		g_pointer_focus_surface = surf;
	}
	wlr_seat_pointer_notify_motion(srv.seat, time, sx, sy);
}

static void cursor_motion_common(uint32_t time) {
	input_notify_activity();
	input_show_cursor(true);
	double x, y;
	cursor_pos(&x, &y);
	if (g_mouse_target == T_SHELL) {
		shell_pointer(1, x, y, 1);
	} else if (g_mouse_target == T_CLIENT) {
		pointer_to_client(time);
	} else if (shell_wants_pointer(x, y)) {
		wlr_seat_pointer_notify_clear_focus(srv.seat);
		g_pointer_focus_surface = NULL;
		shell_hover(x, y);
	} else {
		pointer_to_client(time);
	}
}

static void on_cursor_motion(struct wl_listener *l, void *data) {
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(srv.cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	cursor_motion_common(ev->time_msec);
}

static void on_cursor_motion_abs(struct wl_listener *l, void *data) {
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(srv.cursor, &ev->pointer->base, ev->x, ev->y);
	cursor_motion_common(ev->time_msec);
}

static void on_cursor_button(struct wl_listener *l, void *data) {
	struct wlr_pointer_button_event *ev = data;
	input_notify_activity();
	input_show_cursor(true);
	double x, y;
	cursor_pos(&x, &y);
	bool pressed = ev->state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (pressed) {
		if (shell_wants_pointer(x, y)) {
			g_mouse_target = T_SHELL;
			if (ev->button == BTN_RIGHT) { shell_pointer(0, x, y, 1); shell_pointer(3, x, y, 1); }
			else if (ev->button == BTN_MIDDLE) { shell_go_home(); }
			else shell_pointer(0, x, y, 1);
			return;
		}
		g_mouse_target = T_CLIENT;
		pointer_to_client(ev->time_msec);
		wlr_seat_pointer_notify_button(srv.seat, ev->time_msec, ev->button, ev->state);
	} else {
		if (g_mouse_target == T_SHELL) {
			shell_pointer(2, x, y, 1);
		} else if (g_mouse_target == T_CLIENT) {
			wlr_seat_pointer_notify_button(srv.seat, ev->time_msec, ev->button, ev->state);
		}
		if (srv.cursor) g_mouse_target = T_NONE;
	}
}

static void on_cursor_axis(struct wl_listener *l, void *data) {
	struct wlr_pointer_axis_event *ev = data;
	input_notify_activity();
	double x, y;
	cursor_pos(&x, &y);
	if (shell_wants_pointer(x, y)) {
		if (ev->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) shell_scroll(0, ev->delta * 4.0);
		else shell_scroll(ev->delta * 4.0, 0);
		return;
	}
	wlr_seat_pointer_notify_axis(srv.seat, ev->time_msec, ev->orientation, ev->delta,
		ev->delta_discrete, ev->source, ev->relative_direction);
}

static void on_cursor_frame(struct wl_listener *l, void *data) {
	wlr_seat_pointer_notify_frame(srv.seat);
}

/* ---- touch */

static double g_touch_x[16], g_touch_y[16];      /* last position of each touch (layout dp) */
static double g_touch_ox[16], g_touch_oy[16];    /* layout origin of the surface a client touch started on */

static void touch_coords(struct wlr_touch *touch, double nx, double ny, double *lx, double *ly) {
	wlr_cursor_absolute_to_layout_coords(srv.cursor, &touch->base, nx, ny, lx, ly);
}

static void on_touch_down(struct wl_listener *l, void *data) {
	struct wlr_touch_down_event *ev = data;
	input_notify_activity();
	input_show_cursor(false);
	double lx, ly;
	touch_coords(ev->touch, ev->x, ev->y, &lx, &ly);
	int id = ev->touch_id & 15;
	g_touch_x[id] = lx;
	g_touch_y[id] = ly;
	if (sh.screen_off) { shell_wake(); g_touch_target[id] = T_NONE; return; }
	if (shell_wants_pointer(lx, ly)) {
		if (g_shell_touch_id < 0) {
			g_shell_touch_id = id;
			g_touch_target[id] = T_SHELL;
			shell_pointer(0, lx, ly, 0);
		} else {
			g_touch_target[id] = T_NONE;
		}
		return;
	}
	double sx, sy;
	struct wlr_surface *surf = NULL;
	View *v = view_at_point(lx, ly, &surf, &sx, &sy);
	if (v && surf) {
		g_touch_target[id] = T_CLIENT;
		g_touch_ox[id] = lx - sx;
		g_touch_oy[id] = ly - sy;
		wlr_seat_touch_notify_down(srv.seat, surf, ev->time_msec, ev->touch_id, sx, sy);
	} else {
		g_touch_target[id] = T_NONE;
	}
}

static void on_touch_motion(struct wl_listener *l, void *data) {
	struct wlr_touch_motion_event *ev = data;
	input_notify_activity();
	double lx, ly;
	touch_coords(ev->touch, ev->x, ev->y, &lx, &ly);
	int id = ev->touch_id & 15;
	g_touch_x[id] = lx;
	g_touch_y[id] = ly;
	if (g_touch_target[id] == T_SHELL)
		shell_pointer(1, lx, ly, 0);
	else if (g_touch_target[id] == T_CLIENT)
		wlr_seat_touch_notify_motion(srv.seat, ev->time_msec, ev->touch_id, lx - g_touch_ox[id], ly - g_touch_oy[id]);
}

static void on_touch_up(struct wl_listener *l, void *data) {
	struct wlr_touch_up_event *ev = data;
	input_notify_activity();
	int id = ev->touch_id & 15;
	if (g_touch_target[id] == T_SHELL) {
		shell_pointer(2, g_touch_x[id], g_touch_y[id], 0);
		g_shell_touch_id = -1;
	} else if (g_touch_target[id] == T_CLIENT) {
		wlr_seat_touch_notify_up(srv.seat, ev->time_msec, ev->touch_id);
	}
	g_touch_target[id] = T_NONE;
}

static void on_touch_cancel(struct wl_listener *l, void *data) {
	struct wlr_touch_cancel_event *ev = data;
	int id = ev->touch_id & 15;
	if (g_touch_target[id] == T_SHELL) {
		shell_pointer(2, g_touch_x[id], g_touch_y[id], 0);
		g_shell_touch_id = -1;
	}
	g_touch_target[id] = T_NONE;
}

static void on_touch_frame(struct wl_listener *l, void *data) {
	wlr_seat_touch_notify_frame(srv.seat);
}

/* Programmatic pointer events (IPC / tests): behaves like a touchscreen. */
void input_sim_pointer(int type, double x, double y) {
	input_notify_activity();
	if (type == 0) {
		if (sh.screen_off) { shell_wake(); return; }
		if (shell_wants_pointer(x, y)) {
			g_mouse_target = T_SHELL;
			shell_pointer(0, x, y, 0);
		} else {
			g_mouse_target = T_CLIENT;
			wlr_cursor_warp_closest(srv.cursor, NULL, x, y);
			pointer_to_client(now_msec());
			wlr_seat_pointer_notify_button(srv.seat, now_msec(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			wlr_seat_pointer_notify_frame(srv.seat);
		}
	} else if (type == 1) {
		if (g_mouse_target == T_SHELL) shell_pointer(1, x, y, 0);
		else if (g_mouse_target == T_CLIENT) {
			wlr_cursor_warp_closest(srv.cursor, NULL, x, y);
			pointer_to_client(now_msec());
			wlr_seat_pointer_notify_frame(srv.seat);
		}
	} else {
		if (g_mouse_target == T_SHELL) shell_pointer(2, x, y, 0);
		else if (g_mouse_target == T_CLIENT) {
			wlr_seat_pointer_notify_button(srv.seat, now_msec(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
			wlr_seat_pointer_notify_frame(srv.seat);
		}
		g_mouse_target = T_NONE;
	}
}

/* ------------------------------------------------------------------ init -- */

static void vkb_led_update(struct wlr_keyboard *kb, uint32_t leds) {}
static const struct wlr_keyboard_impl vkb_impl = {.name = "metrowl-vkb", .led_update = vkb_led_update};

void input_init(void) {
	srv.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	const char *layout = cfg.kb_layout[0] ? cfg.kb_layout : (i18n_lang() == LANG_TR ? "tr" : "us");
	srv.keymap = make_keymap(layout, NULL);
	wl_list_init(&srv.keyboards);

	wlr_keyboard_init(&srv.vkb, &vkb_impl, "metrowl-vkb");
	wlr_keyboard_set_keymap(&srv.vkb, srv.keymap);
	srv.vkb_ready = true;

	srv.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(srv.cursor, srv.layout);
	srv.xcursor = wlr_xcursor_manager_create(NULL, 24);

	srv.cursor_motion.notify = on_cursor_motion;
	wl_signal_add(&srv.cursor->events.motion, &srv.cursor_motion);
	srv.cursor_motion_abs.notify = on_cursor_motion_abs;
	wl_signal_add(&srv.cursor->events.motion_absolute, &srv.cursor_motion_abs);
	srv.cursor_button.notify = on_cursor_button;
	wl_signal_add(&srv.cursor->events.button, &srv.cursor_button);
	srv.cursor_axis.notify = on_cursor_axis;
	wl_signal_add(&srv.cursor->events.axis, &srv.cursor_axis);
	srv.cursor_frame.notify = on_cursor_frame;
	wl_signal_add(&srv.cursor->events.frame, &srv.cursor_frame);
	srv.touch_down.notify = on_touch_down;
	wl_signal_add(&srv.cursor->events.touch_down, &srv.touch_down);
	srv.touch_up.notify = on_touch_up;
	wl_signal_add(&srv.cursor->events.touch_up, &srv.touch_up);
	srv.touch_motion.notify = on_touch_motion;
	wl_signal_add(&srv.cursor->events.touch_motion, &srv.touch_motion);
	srv.touch_cancel.notify = on_touch_cancel;
	wl_signal_add(&srv.cursor->events.touch_cancel, &srv.touch_cancel);
	srv.touch_frame.notify = on_touch_frame;
	wl_signal_add(&srv.cursor->events.touch_frame, &srv.touch_frame);

	srv.seat = wlr_seat_create(srv.display, "seat0");
	srv.new_input.notify = new_input;
	wl_signal_add(&srv.backend->events.new_input, &srv.new_input);
	srv.request_cursor.notify = request_cursor;
	wl_signal_add(&srv.seat->events.request_set_cursor, &srv.request_cursor);
	srv.request_set_selection.notify = request_set_selection;
	wl_signal_add(&srv.seat->events.request_set_selection, &srv.request_set_selection);
	srv.request_set_primary_selection.notify = request_set_primary_selection;
	wl_signal_add(&srv.seat->events.request_set_primary_selection, &srv.request_set_primary_selection);

	g_shape_mgr = wlr_cursor_shape_manager_v1_create(srv.display, 1);
	g_shape_listener.notify = shape_request;
	wl_signal_add(&g_shape_mgr->events.request_set_shape, &g_shape_listener);

	wlr_seat_set_keyboard(srv.seat, &srv.vkb);
	update_caps();
	g_last_activity = now_sec();
	srv.mouse_visible = false;
}

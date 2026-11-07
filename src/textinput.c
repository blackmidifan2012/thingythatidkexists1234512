#include "server.h"
#include "shell/shell.h"
#include "util.h"

#include <wlr/types/wlr_text_input_v3.h>

/* The compositor itself is the input method: a client text field that enables
 * text-input-v3 makes the on-screen keyboard appear, and typed text is sent
 * back with commit_string (which makes Turkish letters work in any client). */

typedef struct TI {
	struct wl_list link;
	struct wlr_text_input_v3 *ti;
	struct wl_listener enable, commit, disable, destroy;
} TI;

static struct wl_list g_tis;
static TI *g_active;

static bool same_client(struct wlr_text_input_v3 *ti, struct wlr_surface *s) {
	return s && wl_resource_get_client(ti->resource) == wl_resource_get_client(s->resource);
}

bool textinput_active(void) {
	return g_active && g_active->ti->current_enabled && g_active->ti->focused_surface;
}

int textinput_purpose_flags(void) {
	if (!textinput_active()) return 0;
	switch (g_active->ti->current.content_type.purpose) {
	case 2: case 3: case 4: case 9: return TB_NUMBER;
	case 5: return TB_URL;
	case 6: return TB_EMAIL;
	case 8: return TB_PASSWORD;
	default: return 0;
	}
}

bool textinput_commit(const char *utf8) {
	if (!textinput_active()) return false;
	wlr_text_input_v3_send_commit_string(g_active->ti, utf8);
	wlr_text_input_v3_send_done(g_active->ti);
	return true;
}

static void on_enable(struct wl_listener *l, void *data) {
	TI *t = wl_container_of(l, t, enable);
	g_active = t;
	shell_invalidate();
}

static void on_commit(struct wl_listener *l, void *data) {
	TI *t = wl_container_of(l, t, commit);
	if (t->ti->current_enabled) g_active = t;
	shell_invalidate();
}

static void on_disable(struct wl_listener *l, void *data) {
	TI *t = wl_container_of(l, t, disable);
	if (g_active == t) g_active = NULL;
	shell_invalidate();
}

static void on_destroy(struct wl_listener *l, void *data) {
	TI *t = wl_container_of(l, t, destroy);
	if (g_active == t) g_active = NULL;
	wl_list_remove(&t->enable.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->disable.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
	shell_invalidate();
}

static void new_text_input(struct wl_listener *l, void *data) {
	struct wlr_text_input_v3 *ti = data;
	TI *t = xcalloc(1, sizeof *t);
	t->ti = ti;
	t->enable.notify = on_enable;
	wl_signal_add(&ti->events.enable, &t->enable);
	t->commit.notify = on_commit;
	wl_signal_add(&ti->events.commit, &t->commit);
	t->disable.notify = on_disable;
	wl_signal_add(&ti->events.disable, &t->disable);
	t->destroy.notify = on_destroy;
	wl_signal_add(&ti->events.destroy, &t->destroy);
	wl_list_insert(&g_tis, &t->link);
	/* if the client already has keyboard focus, tell it right away */
	struct wlr_surface *focus = srv.seat->keyboard_state.focused_surface;
	if (same_client(ti, focus)) wlr_text_input_v3_send_enter(ti, focus);
}

void textinput_focus_changed(struct wlr_surface *s) {
	TI *t;
	wl_list_for_each(t, &g_tis, link) {
		if (t->ti->focused_surface && t->ti->focused_surface != s) {
			wlr_text_input_v3_send_leave(t->ti);
			if (g_active == t) g_active = NULL;
		}
		if (s && same_client(t->ti, s) && t->ti->focused_surface != s)
			wlr_text_input_v3_send_enter(t->ti, s);
	}
	shell_invalidate();
}

void textinput_init(void) {
	wl_list_init(&g_tis);
	srv.text_input_mgr = wlr_text_input_manager_v3_create(srv.display);
	srv.new_text_input.notify = new_text_input;
	wl_signal_add(&srv.text_input_mgr->events.text_input, &srv.new_text_input);
}

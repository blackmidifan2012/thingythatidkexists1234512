#include "server.h"
#include "desktop.h"
#include "shell/shell.h"
#include "util.h"

#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>

static View *g_fg;
static struct wlr_xdg_decoration_manager_v1 *g_deco_mgr;

typedef struct Popup {
	struct wlr_xdg_popup *xdg;
	struct wl_listener commit, destroy;
} Popup;

typedef struct Deco {
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wl_listener request_mode, destroy;
} Deco;

/* ---------------------------------------------------------------- lookup -- */

View *view_from_surface(struct wlr_surface *s) {
	if (!s) return NULL;
	struct wlr_surface *root = wlr_surface_get_root_surface(s);
	struct wlr_xdg_toplevel *tl = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (tl && tl->base->data) {
		struct wlr_scene_tree *tree = tl->base->data;
		return tree->node.data;
	}
	/* popups: climb to the parent toplevel */
	struct wlr_xdg_popup *pop = wlr_xdg_popup_try_from_wlr_surface(root);
	while (pop && pop->parent) {
		struct wlr_surface *pr = wlr_surface_get_root_surface(pop->parent);
		struct wlr_xdg_toplevel *ptl = wlr_xdg_toplevel_try_from_wlr_surface(pr);
		if (ptl && ptl->base->data) return ((struct wlr_scene_tree *)ptl->base->data)->node.data;
		pop = wlr_xdg_popup_try_from_wlr_surface(pr);
	}
	return NULL;
}

View *view_at_point(double lx, double ly, struct wlr_surface **surf, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&srv.t_windows->node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
	struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	if (!ss) return NULL;
	*surf = ss->surface;
	struct wlr_scene_tree *t = node->parent;
	while (t && !t->node.data) t = t->node.parent;
	return t ? t->node.data : NULL;
}

View *views_foreground(void) { return g_fg; }

View *view_find_by_app(const DesktopApp *a) {
	View *v;
	wl_list_for_each(v, &srv.views, link) {
		if (v->mapped && !v->is_dialog && v->app == a) return v;
	}
	return NULL;
}

int views_count_mapped(void) {
	int n = 0;
	View *v;
	wl_list_for_each(v, &srv.views, link) if (v->mapped && !v->is_dialog) n++;
	return n;
}

/* ---------------------------------------------------------------- layout -- */

static bool view_is_descendant_of(const View *v, const View *root) {
	for (int guard = 0; v && guard < 8; guard++, v = v->parent) if (v == root) return true;
	return false;
}

/* The shell's area for apps, converted from dp to layout pixels (apps use a bigger scale
 * than the shell, see output.c). */
static struct wlr_box ext_box(bool fullscreen) {
	Rect a = shell_ext_rect(fullscreen);
	double k = output_layout_per_dp();
	int x0 = (int)lround(a.x * k), y0 = (int)lround(a.y * k);
	int x1 = (int)lround((a.x + a.w) * k), y1 = (int)lround((a.y + a.h) * k);
	if (srv.out) { x1 = MIN(x1, srv.out->lw); y1 = MIN(y1, srv.out->lh); }
	return (struct wlr_box){x0, y0, x1 - x0, y1 - y0};
}

static void view_place(View *v) {
	if (!v->mapped && !v->xdg->base->initialized) return;
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
	bool fs = v->fullscreen && v == g_fg;
	struct wlr_box a = ext_box(fs);
	if (v->is_dialog && v->parent) {
		int x = a.x + (a.width - geo.width) / 2, y = a.y + (a.height - geo.height) / 2;
		wlr_scene_node_set_position(&v->tree->node, x - geo.x, y - geo.y);
		return;
	}
	wlr_scene_node_set_position(&v->tree->node, a.x - geo.x, a.y - geo.y);
}

static void view_configure(View *v) {
	if (v->is_dialog || !v->xdg->base->initialized) return;
	bool fs = v->fullscreen && v == g_fg;
	struct wlr_box a = ext_box(fs);
	int w = a.width, h = a.height;
	if (w != v->last_w || h != v->last_h || fs != (bool)v->xdg->current.fullscreen) {
		wlr_xdg_toplevel_set_size(v->xdg, w, h);
		wlr_xdg_toplevel_set_maximized(v->xdg, !fs);
		wlr_xdg_toplevel_set_fullscreen(v->xdg, fs);
		v->last_w = w;
		v->last_h = h;
	}
	view_place(v);
}

void views_relayout(void) {
	View *v;
	wl_list_for_each(v, &srv.views, link) {
		view_configure(v);
		view_place(v);
	}
}

void views_set_visible_only(View *fg) {
	View *v;
	wl_list_for_each(v, &srv.views, link) {
		bool show = fg && v->mapped && view_is_descendant_of(v, fg);
		wlr_scene_node_set_enabled(&v->tree->node, show);
	}
	if (fg) {
		/* dialogs above their parent */
		wlr_scene_node_raise_to_top(&fg->tree->node);
		wl_list_for_each(v, &srv.views, link)
			if (v != fg && v->is_dialog && view_is_descendant_of(v, fg)) wlr_scene_node_raise_to_top(&v->tree->node);
	}
}

void view_activate(View *v) {
	if (!v) {
		g_fg = NULL;
		views_set_visible_only(NULL);
		input_focus_view(NULL);
		return;
	}
	/* focus the topmost dialog of the view, if any */
	View *target = v;
	View *d;
	wl_list_for_each(d, &srv.views, link)
		if (d->mapped && d->is_dialog && d->parent && view_is_descendant_of(d, v)) { target = d; break; }
	g_fg = v;
	wl_list_remove(&v->link);
	wl_list_insert(&srv.views, &v->link);
	views_set_visible_only(v);
	view_configure(v);
	views_relayout();
	input_focus_view(target);
}

void view_close(View *v) {
	if (v) wlr_xdg_toplevel_send_close(v->xdg);
}

/* --------------------------------------------------------- xdg toplevels -- */

static void update_pid(View *v) {
	struct wl_client *c = wl_resource_get_client(v->xdg->base->resource);
	pid_t pid = 0;
	wl_client_get_credentials(c, &pid, NULL, NULL);
	v->pid = (uint32_t)pid;
}

static void resolve_app(View *v) {
	v->app = desktop_find_by_app_id(v->app_id);
}

/* Dialogs coming and going re-activate their app only while it is the one on screen, not
 * over Start or another app. */
static bool fg_on_screen(void) { return g_fg && sh.screen == SCR_EXT && sh.cur_view == g_fg; }

static void view_map(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, map);
	v->mapped = true;
	v->mapped_at = now_sec();
	v->is_dialog = v->xdg->parent != NULL;
	if (v->xdg->parent && v->xdg->parent->base->data)
		v->parent = ((struct wlr_scene_tree *)v->xdg->parent->base->data)->node.data;
	free(v->app_id);
	v->app_id = xstrdup(v->xdg->app_id);
	free(v->title);
	v->title = xstrdup(v->xdg->title);
	resolve_app(v);
	update_pid(v);
	v->fullscreen = v->xdg->requested.fullscreen;
	LOGI("view mapped: app_id=%s title=%s%s", v->app_id ? v->app_id : "(none)", v->title ? v->title : "",
	     v->is_dialog ? " [dialog]" : "");
	if (v->is_dialog && v->parent) {
		wlr_scene_node_set_enabled(&v->tree->node, view_is_descendant_of(v, g_fg));
		view_place(v);
		if (fg_on_screen() && view_is_descendant_of(v, g_fg)) view_activate(g_fg);
		return;
	}
	shell_view_mapped(v);
}

static void view_unmap(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, unmap);
	v->mapped = false;
	if (srv.seat->pointer_state.focused_surface &&
	    view_from_surface(srv.seat->pointer_state.focused_surface) == v)
		wlr_seat_pointer_notify_clear_focus(srv.seat);
	bool was_fg = (g_fg == v);
	if (was_fg) g_fg = NULL;
	if (srv.seat->keyboard_state.focused_surface &&
	    view_from_surface(srv.seat->keyboard_state.focused_surface) == v)
		input_focus_view(NULL);
	if (v->is_dialog && v->parent) { if (fg_on_screen()) view_activate(g_fg); return; }
	shell_view_unmapped(v);
}

static void view_commit(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, commit);
	if (v->xdg->base->initial_commit) {
		if (v->deco) wlr_xdg_toplevel_decoration_v1_set_mode(v->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		struct wlr_box a = ext_box(false);
		if (!v->is_dialog && v->xdg->parent == NULL) {
			wlr_xdg_toplevel_set_size(v->xdg, a.width, a.height);
			wlr_xdg_toplevel_set_maximized(v->xdg, true);
			wlr_xdg_toplevel_set_tiled(v->xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
			wlr_xdg_toplevel_set_bounds(v->xdg, a.width, a.height);
			v->last_w = a.width;
			v->last_h = a.height;
		} else {
			wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
		}
		return;
	}
	if (v->mapped) view_place(v);
}

static void view_destroy(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, destroy);
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->set_parent.link);
	wl_list_remove(&v->link);
	View *o;
	wl_list_for_each(o, &srv.views, link) if (o->parent == v) o->parent = NULL;
	if (g_fg == v) g_fg = NULL;
	/* the decoration (and popups) are destroyed after the toplevel and look the view up
	 * through this pointer: it must not lead to the freed view */
	v->xdg->base->data = NULL;
	shell_view_unmapped(v);   /* in case unmap never arrived */
	free(v->app_id);
	free(v->title);
	free(v);
}

static void view_request_fullscreen(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, request_fullscreen);
	v->fullscreen = v->xdg->requested.fullscreen;
	if (v->xdg->base->initialized) {
		if (v == g_fg) { shell_view_updated(v); view_configure(v); }
		wlr_xdg_surface_schedule_configure(v->xdg->base);
	}
}

static void view_request_maximize(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, request_maximize);
	if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_request_noop(struct wl_listener *l, void *data) {}

static void view_set_title(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, set_title);
	free(v->title);
	v->title = xstrdup(v->xdg->title);
	shell_view_updated(v);
}

static void view_set_app_id(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, set_app_id);
	free(v->app_id);
	v->app_id = xstrdup(v->xdg->app_id);
	resolve_app(v);
	shell_view_updated(v);
}

static void view_set_parent(struct wl_listener *l, void *data) {
	View *v = wl_container_of(l, v, set_parent);
	v->is_dialog = v->xdg->parent != NULL;
	v->parent = (v->xdg->parent && v->xdg->parent->base->data)
		? ((struct wlr_scene_tree *)v->xdg->parent->base->data)->node.data : NULL;
}

static void new_toplevel(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel *xdg = data;
	View *v = xcalloc(1, sizeof *v);
	v->xdg = xdg;
	v->tree = wlr_scene_xdg_surface_create(srv.t_windows, xdg->base);
	v->tree->node.data = v;
	xdg->base->data = v->tree;
	wlr_scene_node_set_enabled(&v->tree->node, false);
	wl_list_insert(srv.views.prev, &v->link);   /* new views go to the back until activated */

	v->map.notify = view_map;
	wl_signal_add(&xdg->base->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
	v->destroy.notify = view_destroy;
	wl_signal_add(&xdg->events.destroy, &v->destroy);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
	v->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
	v->request_move.notify = view_request_noop;
	wl_signal_add(&xdg->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_noop;
	wl_signal_add(&xdg->events.request_resize, &v->request_resize);
	v->set_title.notify = view_set_title;
	wl_signal_add(&xdg->events.set_title, &v->set_title);
	v->set_app_id.notify = view_set_app_id;
	wl_signal_add(&xdg->events.set_app_id, &v->set_app_id);
	v->set_parent.notify = view_set_parent;
	wl_signal_add(&xdg->events.set_parent, &v->set_parent);
}

/* ---------------------------------------------------------------- popups -- */

static void popup_commit(struct wl_listener *l, void *data) {
	Popup *p = wl_container_of(l, p, commit);
	if (!p->xdg->base->initial_commit) return;
	/* keep popups on screen */
	View *root = view_from_surface(p->xdg->base->surface);
	int ox = 0, oy = 0;
	if (root) wlr_scene_node_coords(&root->tree->node, &ox, &oy);
	if (srv.out) {
		struct wlr_box box = {-ox, -oy, srv.out->lw, srv.out->lh};
		wlr_xdg_popup_unconstrain_from_box(p->xdg, &box);
	}
	wlr_xdg_surface_schedule_configure(p->xdg->base);
}

static void popup_destroy(struct wl_listener *l, void *data) {
	Popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void new_popup(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *xp = data;
	struct wlr_xdg_surface *parent = xp->parent ? wlr_xdg_surface_try_from_wlr_surface(xp->parent) : NULL;
	if (!parent || !parent->data) return;
	Popup *p = xcalloc(1, sizeof *p);
	p->xdg = xp;
	xp->base->data = wlr_scene_xdg_surface_create(parent->data, xp->base);
	p->commit.notify = popup_commit;
	wl_signal_add(&xp->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&xp->events.destroy, &p->destroy);
}

/* ------------------------------------------------------------ decoration -- */

/* The mode can only be set once the xdg_surface has been initialised by its first commit;
 * until then the request is remembered and applied from view_commit(). */
static void deco_apply(struct wlr_xdg_toplevel_decoration_v1 *deco) {
	if (deco->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_request_mode(struct wl_listener *l, void *data) {
	Deco *d = wl_container_of(l, d, request_mode);
	deco_apply(d->deco);
}

static void deco_destroy(struct wl_listener *l, void *data) {
	Deco *d = wl_container_of(l, d, destroy);
	View *v = view_from_surface(d->deco->toplevel->base->surface);
	if (v && v->deco == d->deco) v->deco = NULL;
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

static void new_decoration(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Deco *d = xcalloc(1, sizeof *d);
	d->deco = deco;
	d->request_mode.notify = deco_request_mode;
	wl_signal_add(&deco->events.request_mode, &d->request_mode);
	d->destroy.notify = deco_destroy;
	wl_signal_add(&deco->events.destroy, &d->destroy);
	struct wlr_scene_tree *tree = deco->toplevel->base->data;
	View *v = tree ? tree->node.data : NULL;
	if (v) v->deco = deco;
	deco_apply(deco);
}

static void activation_request(struct wl_listener *l, void *data) {
	const struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	View *v = view_from_surface(ev->surface);
	if (!v || !v->mapped) return;
	while (v->parent) v = v->parent;
	shell_view_mapped(v);   /* treat like a fresh request to show this app */
}

void views_init(void) {
	wl_list_init(&srv.views);
	srv.xdg_shell = wlr_xdg_shell_create(srv.display, 6);
	srv.new_toplevel.notify = new_toplevel;
	wl_signal_add(&srv.xdg_shell->events.new_toplevel, &srv.new_toplevel);
	srv.new_popup.notify = new_popup;
	wl_signal_add(&srv.xdg_shell->events.new_popup, &srv.new_popup);
	g_deco_mgr = wlr_xdg_decoration_manager_v1_create(srv.display);
	srv.new_decoration.notify = new_decoration;
	wl_signal_add(&g_deco_mgr->events.new_toplevel_decoration, &srv.new_decoration);
	srv.activation = wlr_xdg_activation_v1_create(srv.display);
	srv.activation_request.notify = activation_request;
	wl_signal_add(&srv.activation->events.request_activate, &srv.activation_request);
}

#include "server.h"
#include "config.h"
#include "shell/shell.h"
#include "util.h"

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <math.h>
#include <pixman.h>
#include <wlr/backend/headless.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_fractional_scale_v1.h>

UiLayer *g_layer_base, *g_layer_overlay;

/* ------------------------------------------------------------- ui buffers */

struct ui_buf {
	struct wlr_buffer base;
	cairo_surface_t *surf;
};

static void ui_buf_destroy(struct wlr_buffer *b) {
	struct ui_buf *ub = wl_container_of(b, ub, base);
	cairo_surface_destroy(ub->surf);
	free(ub);
}

static bool ui_buf_begin_access(struct wlr_buffer *b, uint32_t flags, void **data,
                                uint32_t *format, size_t *stride) {
	struct ui_buf *ub = wl_container_of(b, ub, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = cairo_image_surface_get_data(ub->surf);
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)cairo_image_surface_get_stride(ub->surf);
	return true;
}

static void ui_buf_end_access(struct wlr_buffer *b) {}

static const struct wlr_buffer_impl ui_buf_impl = {
	.destroy = ui_buf_destroy,
	.begin_data_ptr_access = ui_buf_begin_access,
	.end_data_ptr_access = ui_buf_end_access,
};

struct UiLayer {
	struct wlr_scene_buffer *node;
	int pw, ph;
	double scale;
	struct ui_buf *bufs[4];
	int nbufs;
	struct ui_buf *cur;
	cairo_t *cr;
	bool visible;
	int lw, lh;
};

UiLayer *uilayer_create(struct wlr_scene_tree *parent) {
	UiLayer *l = xcalloc(1, sizeof *l);
	l->node = wlr_scene_buffer_create(parent, NULL);
	wlr_scene_buffer_set_filter_mode(l->node, WLR_SCALE_FILTER_NEAREST);
	l->visible = true;
	return l;
}

static void layer_drop_buffers(UiLayer *l) {
	for (int i = 0; i < l->nbufs; i++) wlr_buffer_drop(&l->bufs[i]->base);
	l->nbufs = 0;
	l->cur = NULL;
}

void uilayer_resize(UiLayer *l, int pw, int ph, double scale) {
	if (l->pw == pw && l->ph == ph && l->scale == scale) return;
	if (l->cr) { cairo_destroy(l->cr); l->cr = NULL; }
	layer_drop_buffers(l);
	l->pw = pw;
	l->ph = ph;
	l->scale = scale;
	l->lw = srv.out ? srv.out->lw : (int)lround(pw / scale);
	l->lh = srv.out ? srv.out->lh : (int)lround(ph / scale);
}

cairo_t *uilayer_begin(UiLayer *l) {
	if (l->cr) { cairo_destroy(l->cr); l->cr = NULL; }
	struct ui_buf *pick = NULL;
	for (int i = 0; i < l->nbufs; i++) {
		if (l->bufs[i]->base.n_locks == 0) { pick = l->bufs[i]; break; }
	}
	if (!pick) {
		if (l->nbufs >= 4) {   /* consumers hold everything: recycle the oldest anyway */
			pick = l->bufs[0];
		} else {
			pick = xcalloc(1, sizeof *pick);
			wlr_buffer_init(&pick->base, &ui_buf_impl, l->pw, l->ph);
			pick->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, l->pw, l->ph);
			l->bufs[l->nbufs++] = pick;
		}
	}
	l->cur = pick;
	l->cr = cairo_create(pick->surf);
	cairo_set_operator(l->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(l->cr, 0, 0, 0, 0);
	cairo_paint(l->cr);
	cairo_set_operator(l->cr, CAIRO_OPERATOR_OVER);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
	cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
	cairo_set_font_options(l->cr, fo);
	cairo_font_options_destroy(fo);
	return l->cr;
}

cairo_surface_t *uilayer_surface(UiLayer *l) { return l->cur ? l->cur->surf : NULL; }

void uilayer_commit(UiLayer *l, bool opaque) {
	if (!l->cur) return;
	if (l->cr) { cairo_destroy(l->cr); l->cr = NULL; }
	cairo_surface_flush(l->cur->surf);
	cairo_surface_mark_dirty(l->cur->surf);
	wlr_scene_buffer_set_buffer(l->node, &l->cur->base);
	wlr_scene_buffer_set_dest_size(l->node, l->lw, l->lh);
	pixman_region32_t region;
	pixman_region32_init(&region);
	if (opaque) pixman_region32_union_rect(&region, &region, 0, 0, l->pw, l->ph);
	wlr_scene_buffer_set_opaque_region(l->node, &region);
	pixman_region32_fini(&region);
	l->cur = NULL;
}

void uilayer_set_visible(UiLayer *l, bool v) {
	l->visible = v;
	wlr_scene_node_set_enabled(&l->node->node, v);
}
bool uilayer_visible(const UiLayer *l) { return l->visible; }

/* ---------------------------------------------------------------- outputs */

/* Picks a fractional scale near `want` for which the UI buffer maps exactly onto
 * physical pixels given wlroots' truncating/rounding arithmetic. */
static float pick_scale(int pw, int ph, double want) {
	float best = (float)want;
	double best_err = 1e9;
	for (int k = -16; k <= 16; k++) {
		float s = (float)(want * (1.0 + k * 0.01));
		if (s < 0.75f) continue;
		int lw = (int)(pw / s), lh = (int)(ph / s);
		if (lroundf(lw * s) == pw && lroundf(lh * s) == ph) {
			double err = fabs(s - want);
			if (err < best_err) { best_err = err; best = s; }
		}
	}
	return best;
}

/* Windows Phone sizes its UI so the 480 dp wide page is ~62 mm on the glass (7.7 dp/mm).
 * With the panel's physical size known (DRM) that gives the right scale on any phone
 * (Galaxy Grand Prime 540x960 -> 1.125, 1080p phones -> ~2.5) and on tablets, which then
 * get a wider page with more tile columns instead of a giant phone layout. */
static double auto_scale(struct wlr_output *w, int pw, int ph) {
	double s;
	int mm_w = w ? w->phys_width : 0, mm_h = w ? w->phys_height : 0;
	if (mm_w > 20 && mm_h > 20 && mm_w < 2000 && mm_h < 2000) {
		double ppmm = hypot(pw, ph) / hypot(mm_w, mm_h);
		s = ppmm / 7.7;
	} else {
		/* unknown size: phones are tall (>= 15:9), tablets/monitors are squarer */
		double aspect = (double)MAX(pw, ph) / MIN(pw, ph);
		s = MIN(pw, ph) / (aspect >= 1.65 ? 480.0 : 768.0);
	}
	s = round(s * 8) / 8;
	return CLAMP(s, 1, 4);
}

static void output_update(Output *o) {
	struct wlr_output *w = o->wlr;
	int tw, th;
	wlr_output_transformed_resolution(w, &tw, &th);
	o->pw = tw;
	o->ph = th;
	o->scale = w->scale;
	wlr_output_effective_resolution(w, &o->lw, &o->lh);
	if (g_layer_base) {
		uilayer_resize(g_layer_base, o->pw, o->ph, o->scale);
		uilayer_resize(g_layer_overlay, o->pw, o->ph, o->scale);
	}
	shell_set_output(o->pw, o->ph, o->scale);
	views_relayout();
}

static void output_apply_scale(Output *o, struct wlr_output_state *st, int pw, int ph) {
	double want = srv.opt_scale > 0 ? srv.opt_scale : cfg.ui_scale > 0 ? cfg.ui_scale : auto_scale(o->wlr, pw, ph);
	float s = pick_scale(pw, ph, want);
	wlr_output_state_set_scale(st, s);
}

static void output_frame_cb(struct wl_listener *l, void *data);

typedef struct { void (*cb)(cairo_surface_t *, void *); void *ud; } CaptureReq;
static CaptureReq g_capture;

void output_request_capture(void (*cb)(cairo_surface_t *surf, void *ud), void *ud) {
	g_capture = (CaptureReq){cb, ud};
	if (srv.out) {
		struct wlr_scene_output *so = srv.out->scene_output;
		wlr_damage_ring_add_whole(&so->damage_ring);
		/* wlroots >= 0.18 only renders a buffer for pending commit damage; without it an
		 * idle screen produces no buffer and the capture fails. */
		pixman_region32_union_rect(&so->pending_commit_damage, &so->pending_commit_damage, 0, 0,
		                           srv.out->wlr->width, srv.out->wlr->height);
		wlr_output_schedule_frame(srv.out->wlr);
	}
}

static cairo_surface_t *read_buffer(struct wlr_buffer *buf) {
	int w = buf->width, h = buf->height;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *dst = cairo_image_surface_get_data(surf);
	int dstride = cairo_image_surface_get_stride(surf);
	void *data = NULL;
	uint32_t fmt = 0;
	size_t stride = 0;
	struct wlr_texture *tex = NULL;
	unsigned char *tmp = NULL;
	if (wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
		/* direct access */
	} else {
		tex = wlr_texture_from_buffer(srv.renderer, buf);
		if (!tex) { cairo_surface_destroy(surf); return NULL; }
		fmt = DRM_FORMAT_ARGB8888;
		stride = (size_t)w * 4;
		tmp = xmalloc(stride * (size_t)h);
		struct wlr_texture_read_pixels_options opts = {.data = tmp, .format = fmt, .stride = (uint32_t)stride};
		if (!wlr_texture_read_pixels(tex, &opts)) {
			free(tmp); wlr_texture_destroy(tex); cairo_surface_destroy(surf);
			return NULL;
		}
		data = tmp;
	}
	bool swap = (fmt == DRM_FORMAT_ABGR8888 || fmt == DRM_FORMAT_XBGR8888);
	bool opaque = (fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_XBGR8888);
	for (int y = 0; y < h; y++) {
		const uint32_t *s = (const uint32_t *)((const char *)data + (size_t)y * stride);
		uint32_t *d = (uint32_t *)(dst + (size_t)y * dstride);
		for (int x = 0; x < w; x++) {
			uint32_t p = s[x];
			if (swap) p = (p & 0xFF00FF00) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
			if (opaque) p |= 0xFF000000;
			d[x] = p;
		}
	}
	if (tmp) { free(tmp); wlr_texture_destroy(tex); }
	else wlr_buffer_end_data_ptr_access(buf);
	cairo_surface_mark_dirty(surf);
	return surf;
}

static void output_frame_cb(struct wl_listener *l, void *data) {
	Output *o = wl_container_of(l, o, frame);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double now = ts.tv_sec + ts.tv_nsec / 1e9;

	bool more = false;
	if (!o->blanked) more = shell_render(now);

	if (g_capture.cb && !o->blanked) {
		CaptureReq req = g_capture;
		g_capture.cb = NULL;
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		cairo_surface_t *img = NULL;
		if (wlr_scene_output_build_state(o->scene_output, &st, NULL)) {
			if (st.buffer) img = read_buffer(st.buffer);
			wlr_output_commit_state(o->wlr, &st);
		}
		wlr_output_state_finish(&st);
		req.cb(img, req.ud);
		if (img) cairo_surface_destroy(img);
	} else if (!o->blanked) {
		wlr_scene_output_commit(o->scene_output, NULL);
	}
	wlr_scene_output_send_frame_done(o->scene_output, &ts);
	if (more) wlr_output_schedule_frame(o->wlr);
}

static void output_request_state_cb(struct wl_listener *l, void *data) {
	Output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr, ev->state);
	if (ev->state->committed & WLR_OUTPUT_STATE_MODE) {
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		output_apply_scale(o, &st, o->wlr->width, o->wlr->height);
		wlr_output_commit_state(o->wlr, &st);
		wlr_output_state_finish(&st);
	}
	output_update(o);
	srv_schedule_frame();
}

static void output_destroy_cb(struct wl_listener *l, void *data) {
	Output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	if (srv.out == o) srv.out = NULL;
	free(o);
}

void output_init_primary(struct wlr_output *wlr) {
	wlr_output_init_render(wlr, srv.allocator, srv.renderer);
	Output *o = xcalloc(1, sizeof *o);
	o->wlr = wlr;

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	int pw = wlr->width, ph = wlr->height;
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
	if (srv.opt_w > 0 && srv.opt_h > 0) {
		wlr_output_state_set_custom_mode(&st, srv.opt_w, srv.opt_h, 0);
		pw = srv.opt_w;
		ph = srv.opt_h;
	} else if (mode) {
		wlr_output_state_set_mode(&st, mode);
		pw = mode->width;
		ph = mode->height;
	}
	output_apply_scale(o, &st, pw, ph);
	if (!wlr_output_commit_state(wlr, &st)) {
		/* custom modes are not supported everywhere (DRM): retry with the preferred mode */
		wlr_output_state_finish(&st);
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, true);
		if (mode) wlr_output_state_set_mode(&st, mode);
		pw = mode ? mode->width : wlr->width;
		ph = mode ? mode->height : wlr->height;
		output_apply_scale(o, &st, pw, ph);
		wlr_output_commit_state(wlr, &st);
	}
	wlr_output_state_finish(&st);

	o->frame.notify = output_frame_cb;
	wl_signal_add(&wlr->events.frame, &o->frame);
	o->request_state.notify = output_request_state_cb;
	wl_signal_add(&wlr->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy_cb;
	wl_signal_add(&wlr->events.destroy, &o->destroy);

	struct wlr_output_layout_output *lo = wlr_output_layout_add(srv.layout, wlr, 0, 0);
	o->scene_output = wlr_scene_output_create(srv.scene, wlr);
	wlr_scene_output_layout_add_output(srv.scene_layout, lo, o->scene_output);
	srv.out = o;

	g_layer_base = uilayer_create(srv.t_base);
	g_layer_overlay = uilayer_create(srv.t_overlay);
	output_update(o);
	LOGI("output %s: %dx%d px, scale %.3f, logical %dx%d dp", wlr->name, o->pw, o->ph, o->scale, o->lw, o->lh);
	wlr_output_schedule_frame(wlr);
}

void output_reapply_scale(void) {
	Output *o = srv.out;
	if (!o) return;
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	output_apply_scale(o, &st, o->wlr->width, o->wlr->height);
	wlr_output_commit_state(o->wlr, &st);
	wlr_output_state_finish(&st);
	output_update(o);
}

void output_set_blanked(bool blank) {
	Output *o = srv.out;
	if (!o || o->blanked == blank) return;
	o->blanked = blank;
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, !blank);
	wlr_output_commit_state(o->wlr, &st);
	wlr_output_state_finish(&st);
	if (!blank) {
		wlr_damage_ring_add_whole(&o->scene_output->damage_ring);
		wlr_output_schedule_frame(o->wlr);
	}
}

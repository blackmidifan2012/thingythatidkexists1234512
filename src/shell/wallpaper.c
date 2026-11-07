#include "shell.h"
#include "../apps/apps.h"
#include "../config.h"
#include "../util.h"

#include <math.h>

/* Wallpapers: user pictures (loaded asynchronously) or a generated Metro-style
 * abstract composition in the accent colour. */

static ImgCache *g_cache;

static ImgCache *cache(void) {
	if (!g_cache) g_cache = imgcache_new(4);
	return g_cache;
}

/* decode just large enough for the output (plus room for parallax), not at full size */
static int decode_dim(void) { return CLAMP((int)(MAX(sh.pw, sh.ph) * 1.35), 640, 2560); }

Image *wallpaper_start(void) {
	if (!cfg.wallpaper[0]) return NULL;
	return imgcache_get(cache(), cfg.wallpaper, decode_dim());
}

Image *wallpaper_lock(double w, double h) {
	if (!cfg.lock_wallpaper[0]) return NULL;
	return imgcache_get(cache(), cfg.lock_wallpaper, decode_dim());
}

void wallpaper_changed(void) {
	if (g_cache) imgcache_clear(g_cache);
	shell_invalidate();
}

static unsigned rnd(unsigned *s) {
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

void wallpaper_draw_generated(cairo_t *cr, Rect r, Color accent, bool dark, unsigned seed) {
	Color dk = col_mix(accent, COL_BLACK, 0.7f);
	Color md = col_mix(accent, COL_BLACK, 0.35f);
	cairo_save(cr);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_clip(cr);
	cairo_pattern_t *g = cairo_pattern_create_linear(r.x, r.y, r.x + r.w * 0.4, r.y + r.h);
	cairo_pattern_add_color_stop_rgb(g, 0, md.r, md.g, md.b);
	cairo_pattern_add_color_stop_rgb(g, 1, dk.r, dk.g, dk.b);
	cairo_set_source(cr, g);
	cairo_paint(cr);
	cairo_pattern_destroy(g);
	unsigned s = seed * 2654435761u + 12345;
	/* big flat translucent shapes */
	for (int i = 0; i < 5; i++) {
		double cx = r.x + (rnd(&s) % 1000) / 1000.0 * r.w;
		double cy = r.y + (rnd(&s) % 1000) / 1000.0 * r.h;
		double sz = (0.35 + (rnd(&s) % 100) / 160.0) * MAX(r.w, r.h) * 0.5;
		Color c = col_mix(accent, COL_WHITE, (float)((rnd(&s) % 40) / 100.0));
		cairo_save(cr);
		cairo_translate(cr, cx, cy);
		cairo_rotate(cr, (double)(rnd(&s) % 90) * M_PI / 180);
		gfx_set_color(cr, col_alpha(c, 0.12f + (float)(rnd(&s) % 10) / 100.f));
		if (i % 2 == 0) cairo_rectangle(cr, -sz / 2, -sz / 2, sz, sz);
		else { cairo_new_sub_path(cr); cairo_arc(cr, 0, 0, sz / 2, 0, 2 * M_PI); }
		cairo_fill(cr);
		cairo_restore(cr);
	}
	/* diagonal band */
	gfx_set_color(cr, col_alpha(COL_WHITE, 0.05f));
	cairo_move_to(cr, r.x, r.y + r.h * 0.55);
	cairo_line_to(cr, r.x + r.w, r.y + r.h * 0.25);
	cairo_line_to(cr, r.x + r.w, r.y + r.h * 0.45);
	cairo_line_to(cr, r.x, r.y + r.h * 0.78);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_restore(cr);
	(void)dark;
}

#include "gfx.h"
#include "util.h"
#include "build_config.h"

#include <fontconfig/fontconfig.h>
#include <math.h>
#include <pango/pangocairo.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#define STBI_NO_TGA
#include "stb_image.h"

#if HAVE_RSVG
#include <librsvg/rsvg.h>
#endif

/* ------------------------------------------------------------------ colour */

Color col_mix(Color a, Color b, float t) {
	return (Color){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
	               a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
Color col_from_u32(uint32_t v) { return RGBX(v); }
uint32_t col_to_u32(Color c) {
	return ((uint32_t)lroundf(c.r * 255) << 16) | ((uint32_t)lroundf(c.g * 255) << 8) | (uint32_t)lroundf(c.b * 255);
}
float col_luma(Color c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
Color col_contrast(Color bg) { return col_luma(bg) > 0.62f ? COL_BLACK : COL_WHITE; }

Rect rect_inset(Rect r, double d) { return R(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }
Rect rect_intersect(Rect a, Rect b) {
	double x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
	double x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
	return R(x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0));
}

/* ------------------------------------------------------------------- setup */

static double g_text_scale = 1.0;
void gfx_set_text_scale(double s) { g_text_scale = CLAMP(s, 0.7, 1.6); }
double gfx_text_scale(void) { return g_text_scale; }

static const char *FAMILIES[] = {
	[FW_LIGHT]     = "Segoe UI Light,Segoe WP Light,Selawik Light,Open Sans Light,Noto Sans Light,Sans",
	[FW_SEMILIGHT] = "Segoe UI Semilight,Segoe WP Semilight,Selawik Semilight,Open Sans,Noto Sans,Sans",
	[FW_REGULAR]   = "Segoe UI,Segoe WP,Selawik,Open Sans,Noto Sans,Sans",
	[FW_SEMIBOLD]  = "Segoe UI Semibold,Segoe WP Semibold,Selawik Semibold,Open Sans Semibold,Noto Sans,Sans",
	[FW_BOLD]      = "Segoe UI,Segoe WP,Selawik,Open Sans,Noto Sans,Sans",
};

void gfx_init(void) {
	char *dir = data_path("fonts");
	if (dir) {
		FcConfigAppFontAddDir(NULL, (const FcChar8 *)dir);
		LOGI("registered bundled fonts from %s", dir);
		free(dir);
	} else {
		LOGE("bundled fonts not found (set METROWL_DATADIR)");
	}
	LOGI("fonts: Segoe UI %s, Segoe MDL2 Assets %s, Selawik %s",
		gfx_has_font_family("Segoe UI") ? "yes" : "no",
		gfx_has_font_family("Segoe MDL2 Assets") ? "yes" : "no",
		gfx_has_font_family("Selawik") ? "yes" : "no");
}

bool gfx_has_font_family(const char *family) {
	PangoFontMap *map = pango_cairo_font_map_get_default();
	PangoFontFamily **fams;
	int n;
	pango_font_map_list_families(map, &fams, &n);
	bool found = false;
	for (int i = 0; i < n && !found; i++)
		found = g_ascii_strcasecmp(pango_font_family_get_name(fams[i]), family) == 0;
	g_free(fams);
	return found;
}

/* ------------------------------------------------------------------- fills */

void gfx_set_color(cairo_t *cr, Color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void gfx_fill_rect(cairo_t *cr, Rect r, Color c) {
	if (c.a <= 0 || r.w <= 0 || r.h <= 0) return;
	gfx_set_color(cr, c);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_fill(cr);
}

void gfx_stroke_rect(cairo_t *cr, Rect r, double w, Color c) {
	if (c.a <= 0) return;
	gfx_set_color(cr, c);
	cairo_set_line_width(cr, w);
	cairo_rectangle(cr, r.x + w / 2, r.y + w / 2, r.w - w, r.h - w);
	cairo_stroke(cr);
}

void gfx_fill_circle(cairo_t *cr, double cx, double cy, double rad, Color c) {
	gfx_set_color(cr, c);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
	cairo_fill(cr);
}

void gfx_stroke_circle(cairo_t *cr, double cx, double cy, double rad, double w, Color c) {
	gfx_set_color(cr, c);
	cairo_set_line_width(cr, w);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, rad - w / 2, 0, 2 * M_PI);
	cairo_stroke(cr);
}

/* -------------------------------------------------------------------- text */

typedef struct {
	uint32_t hash;
	char *text;
	float size;
	int weight, align, lines;
	float max_w;
	PangoLayout *layout;
	uint64_t used;
	cairo_surface_t *mask;       /* rendered text at device scale mask_scale, see gfx_text() */
	double mask_scale;
	int mask_x, mask_y;          /* mask origin relative to the text origin, device pixels */
	cairo_matrix_t m;            /* transform the layout was last updated for */
} CacheEnt;

#define CACHE_N 512
static CacheEnt g_cache[CACHE_N];
static uint64_t g_tick;

static uint32_t text_hash(const char *s, double size, int weight, int align, int lines, double max_w) {
	uint32_t h = 2166136261u;
	for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
	h ^= (uint32_t)(size * 100) * 2654435761u;
	h ^= (uint32_t)(max_w * 10) * 40503u;
	h ^= (uint32_t)(weight * 7 + align * 13 + lines * 31);
	return h ? h : 1;
}

static CacheEnt *get_ent(cairo_t *cr, const char *s, double size, int weight, int align,
                         double max_w, int lines) {
	size *= g_text_scale;
	uint32_t h = text_hash(s, size, weight, align, lines, max_w);
	CacheEnt *slot = NULL, *oldest = &g_cache[0];
	for (int i = 0; i < CACHE_N; i++) {
		CacheEnt *e = &g_cache[(h + i) % CACHE_N];
		if (e->layout && e->hash == h && e->weight == weight && e->align == align && e->lines == lines
		    && e->size == (float)size && e->max_w == (float)max_w && strcmp(e->text, s) == 0) {
			e->used = ++g_tick;
			/* updating bumps the context serial and forces a full relayout: only do it
			 * when the transform really changed */
			cairo_matrix_t m;
			cairo_get_matrix(cr, &m);
			if (m.xx != e->m.xx || m.yy != e->m.yy || m.xy != e->m.xy || m.yx != e->m.yx) {
				pango_cairo_update_layout(cr, e->layout);
				e->m = m;
			}
			return e;
		}
		if (!e->layout) { slot = e; break; }
		if (e->used < oldest->used) oldest = e;
		if (i > 8) break;   /* bounded probing, evict the oldest of the probed ones */
	}
	if (!slot) slot = oldest;
	if (slot->layout) { g_object_unref(slot->layout); free(slot->text); }
	if (slot->mask) cairo_surface_destroy(slot->mask);

	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_new();
	pango_font_description_set_family(fd, FAMILIES[weight]);
	pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
	pango_font_description_set_weight(fd, weight == FW_BOLD ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(l, s, -1);
	pango_layout_set_alignment(l, align == TA_CENTER ? PANGO_ALIGN_CENTER
	                            : align == TA_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
	if (max_w > 0) {
		pango_layout_set_width(l, (int)(max_w * PANGO_SCALE));
		pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
		if (lines > 0) {
			pango_layout_set_height(l, -lines);
			pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
		}
	}
	pango_cairo_update_layout(cr, l);
	*slot = (CacheEnt){h, xstrdup(s), (float)size, weight, align, lines, (float)max_w, l, ++g_tick};
	cairo_get_matrix(cr, &slot->m);
	return slot;
}

static PangoLayout *get_layout(cairo_t *cr, const char *s, double size, int weight, int align,
                               double max_w, int lines) {
	return get_ent(cr, s, size, weight, align, max_w, lines)->layout;
}

/* Emoji are drawn in colour: they cannot go through the single-colour text masks.
 * U+1F000.. (F0 9F ..) and the U+2600..27BF / U+2B50 symbol blocks. */
static bool has_color_glyphs(const char *s) {
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (p[0] == 0xF0 && p[1] == 0x9F) return true;
		if (p[0] == 0xE2 && ((p[1] >= 0x98 && p[1] <= 0x9E) || p[1] == 0xAD)) return true;
	}
	return false;
}

/* Laying out and rasterising glyphs every frame is a large part of a frame on phone
 * CPUs: render each cached layout once into an alpha mask and stamp that instead. */
static cairo_surface_t *text_mask(cairo_t *cr, CacheEnt *e, double scale) {
	if (e->mask && e->mask_scale == scale) return e->mask;
	if (e->mask) cairo_surface_destroy(e->mask);
	e->mask = NULL;
	PangoRectangle ink, log;
	pango_layout_get_extents(e->layout, &ink, &log);
	double x0 = MIN(ink.x, log.x), y0 = MIN(ink.y, log.y);
	double x1 = MAX(ink.x + ink.width, log.x + log.width), y1 = MAX(ink.y + ink.height, log.y + log.height);
	int mx = (int)floor(x0 / PANGO_SCALE * scale) - 1, my = (int)floor(y0 / PANGO_SCALE * scale) - 1;
	int w = (int)ceil(x1 / PANGO_SCALE * scale) + 1 - mx, h = (int)ceil(y1 / PANGO_SCALE * scale) + 1 - my;
	if (w <= 0 || h <= 0 || (double)w * h > 4e6) return NULL;
	e->mask = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
	cairo_t *mc = cairo_create(e->mask);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_get_font_options(cr, fo);
	cairo_set_font_options(mc, fo);
	cairo_font_options_destroy(fo);
	cairo_translate(mc, -mx, -my);
	cairo_scale(mc, scale, scale);
	pango_cairo_update_layout(mc, e->layout);
	pango_cairo_show_layout(mc, e->layout);
	cairo_destroy(mc);
	e->mask_scale = scale;
	e->mask_x = mx;
	e->mask_y = my;
	return e->mask;
}

Size gfx_text_size(cairo_t *cr, const char *s, double size, int weight, double max_w, int max_lines) {
	if (!s || !*s) return (Size){0, gfx_line_height(cr, size, weight)};
	PangoLayout *l = get_layout(cr, s, size, weight, TA_LEFT, max_w, max_lines);
	PangoRectangle r;
	pango_layout_get_extents(l, NULL, &r);
	return (Size){(double)r.width / PANGO_SCALE, (double)r.height / PANGO_SCALE};
}

double gfx_line_height(cairo_t *cr, double size, int weight) {
	PangoLayout *l = get_layout(cr, "Ag", size, weight, TA_LEFT, 0, 0);
	PangoRectangle r;
	pango_layout_get_extents(l, NULL, &r);
	return (double)r.height / PANGO_SCALE;
}

double gfx_text(cairo_t *cr, const char *s, double x, double y, double size, int weight,
                Color c, int align, double max_w, int max_lines) {
	if (!s || !*s || c.a <= 0) return gfx_line_height(cr, size, weight);
	CacheEnt *e = get_ent(cr, s, size, weight, align, max_w, max_lines);
	PangoLayout *l = e->layout;
	PangoRectangle r;
	pango_layout_get_extents(l, NULL, &r);
	double bw = max_w > 0 ? max_w : (double)r.width / PANGO_SCALE;
	double left = x - (align == TA_CENTER ? bw / 2 : align == TA_RIGHT ? bw : 0);
	/* snap to the device pixel grid for crisp glyphs */
	double dx = left, dy = y;
	cairo_user_to_device(cr, &dx, &dy);
	dx = round(dx);
	dy = round(dy);
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);
	cairo_surface_t *mask = m.xy == 0 && m.yx == 0 && m.xx > 0 && m.xx == m.yy && !has_color_glyphs(s)
	                        ? text_mask(cr, e, m.xx) : NULL;
	if (mask) {
		cairo_save(cr);
		cairo_identity_matrix(cr);
		gfx_set_color(cr, c);
		cairo_mask_surface(cr, mask, dx + e->mask_x, dy + e->mask_y);
		cairo_restore(cr);
		return (double)r.height / PANGO_SCALE;
	}
	cairo_device_to_user(cr, &dx, &dy);
	cairo_save(cr);
	cairo_move_to(cr, dx, dy);
	gfx_set_color(cr, c);
	pango_cairo_show_layout(cr, l);
	cairo_restore(cr);
	return (double)r.height / PANGO_SCALE;
}

PangoLayout *gfx_layout_new(cairo_t *cr, const char *s, double size, int weight, double max_w) {
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_new();
	pango_font_description_set_family(fd, FAMILIES[weight]);
	pango_font_description_set_absolute_size(fd, size * g_text_scale * PANGO_SCALE);
	pango_font_description_set_weight(fd, weight == FW_BOLD ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(l, s, -1);
	if (max_w > 0) {
		pango_layout_set_width(l, (int)(max_w * PANGO_SCALE));
		pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	}
	return l;
}

void gfx_text_in(cairo_t *cr, const char *s, Rect r, double size, int weight, Color c,
                 int halign, bool vcenter) {
	Size sz = gfx_text_size(cr, s, size, weight, r.w, 1);
	double x = halign == TA_CENTER ? rect_cx(r) : halign == TA_RIGHT ? rect_right(r) : r.x;
	double y = vcenter ? r.y + (r.h - sz.h) / 2 : r.y;
	gfx_text(cr, s, x, y, size, weight, c, halign, r.w, 1);
}

/* ------------------------------------------------------------------ images */

Image *image_from_surface(cairo_surface_t *s) {
	if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return NULL;
	Image *img = xcalloc(1, sizeof *img);
	img->surf = s;
	img->w = cairo_image_surface_get_width(s);
	img->h = cairo_image_surface_get_height(s);
	return img;
}

void image_free(Image *img) {
	if (!img) return;
	cairo_surface_destroy(img->surf);
	if (img->scaled) cairo_surface_destroy(img->scaled);
	free(img);
}

static cairo_surface_t *surface_from_rgba(const unsigned char *rgba, int w, int h) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return s;
	unsigned char *dst = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		uint32_t *d = (uint32_t *)(dst + (size_t)y * stride);
		const unsigned char *p = rgba + (size_t)y * w * 4;
		for (int x = 0; x < w; x++, p += 4) {
			uint32_t a = p[3];
			uint32_t r = (p[0] * a + 127) / 255, g = (p[1] * a + 127) / 255, b = (p[2] * a + 127) / 255;
			d[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

/* Halves the image with a 2x2 box filter until it is within 2x of the target. */
static cairo_surface_t *shrink_surface(cairo_surface_t *src, int max_dim) {
	cairo_surface_reference(src);
	for (;;) {
		int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
		if (MAX(w, h) / 2 < max_dim || w < 2 || h < 2) break;
		int nw = w / 2, nh = h / 2;
		cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
		unsigned char *sp = cairo_image_surface_get_data(src), *dp = cairo_image_surface_get_data(dst);
		int ss = cairo_image_surface_get_stride(src), ds = cairo_image_surface_get_stride(dst);
		for (int y = 0; y < nh; y++) {
			const uint32_t *r0 = (const uint32_t *)(sp + (size_t)(2 * y) * ss);
			const uint32_t *r1 = (const uint32_t *)(sp + (size_t)(2 * y + 1) * ss);
			uint32_t *o = (uint32_t *)(dp + (size_t)y * ds);
			for (int x = 0; x < nw; x++) {
				uint32_t p[4] = {r0[2 * x], r0[2 * x + 1], r1[2 * x], r1[2 * x + 1]};
				uint32_t a = 0, r = 0, g = 0, b = 0;
				for (int i = 0; i < 4; i++) {
					a += p[i] >> 24; r += (p[i] >> 16) & 255; g += (p[i] >> 8) & 255; b += p[i] & 255;
				}
				o[x] = ((a / 4) << 24) | ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
			}
		}
		cairo_surface_mark_dirty(dst);
		cairo_surface_destroy(src);
		src = dst;
	}
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	if (MAX(w, h) <= max_dim) return src;
	double s = (double)max_dim / MAX(w, h);
	int nw = MAX(1, (int)lround(w * s)), nh = MAX(1, (int)lround(h * s));
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
	cairo_t *cr = cairo_create(dst);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(src);
	return dst;
}

static Image *finish_decode(unsigned char *px, int w, int h, int max_dim) {
	if (!px) return NULL;
	cairo_surface_t *full = surface_from_rgba(px, w, h);
	stbi_image_free(px);
	if (cairo_surface_status(full) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(full); return NULL; }
	if (max_dim > 0 && MAX(w, h) > max_dim) {
		cairo_surface_t *small = shrink_surface(full, max_dim);
		cairo_surface_destroy(full);
		full = small;
	}
	return image_from_surface(full);
}

Image *image_load_mem(const unsigned char *data, size_t len, int max_dim) {
	int w, h, n;
	unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
	return finish_decode(px, w, h, max_dim);
}

Image *image_load(const char *path, int max_dim) {
	int w, h, n;
	unsigned char *px = stbi_load(path, &w, &h, &n, 4);
	return finish_decode(px, w, h, max_dim);
}

Image *image_load_icon(const char *path, int px) {
#if HAVE_RSVG
	if (str_ends(path, ".svg") || str_ends(path, ".svgz")) {
		GError *err = NULL;
		RsvgHandle *h = rsvg_handle_new_from_file(path, &err);
		if (!h) { if (err) g_error_free(err); return NULL; }
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
		cairo_t *cr = cairo_create(s);
		RsvgRectangle vp = {0, 0, px, px};
		bool ok = rsvg_handle_render_document(h, cr, &vp, &err);
		if (err) g_error_free(err);
		cairo_destroy(cr);
		g_object_unref(h);
		if (!ok) { cairo_surface_destroy(s); return NULL; }
		return image_from_surface(s);
	}
#endif
	if (str_ends(path, ".svg") || str_ends(path, ".svgz") || str_ends(path, ".xpm")) return NULL;
	return image_load(path, px);
}

/* Scaling a picture with CAIRO_FILTER_GOOD on every frame is far too slow for phone
 * CPUs: keep a copy pre-scaled to the device size it is drawn at and blit that 1:1.
 * Moves the origin to (x, y) and the image's scale s; returns a pattern to paint. */
static cairo_pattern_t *image_pattern(cairo_t *cr, const Image *cimg, double x, double y, double s) {
	Image *img = (Image *)cimg;
	cairo_surface_t *src = img->surf;
	cairo_translate(cr, x, y);
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);
	if (m.xy == 0 && m.yx == 0 && m.xx > 0 && m.yy > 0) {
		int dw = MAX(1, (int)lround(img->w * s * m.xx)), dh = MAX(1, (int)lround(img->h * s * m.yy));
		if (dw != img->w || dh != img->h) {
			if (!img->scaled || img->sw != dw || img->sh != dh) {
				if (img->scaled) cairo_surface_destroy(img->scaled);
				img->scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dw, dh);
				cairo_t *c = cairo_create(img->scaled);
				cairo_scale(c, (double)dw / img->w, (double)dh / img->h);
				cairo_set_source_surface(c, img->surf, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
				cairo_paint(c);
				cairo_destroy(c);
				img->sw = dw;
				img->sh = dh;
			}
			src = img->scaled;
		}
		cairo_identity_matrix(cr);
		cairo_translate(cr, round(m.x0), round(m.y0));
	} else {
		cairo_scale(cr, s, s);
	}
	cairo_pattern_t *p = cairo_pattern_create_for_surface(src);
	cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);
	return p;
}

static void image_paint(cairo_t *cr, const Image *img, double x, double y, double s, double alpha) {
	cairo_pattern_t *p = image_pattern(cr, img, x, y, s);
	cairo_set_source(cr, p);
	cairo_paint_with_alpha(cr, alpha);
	cairo_pattern_destroy(p);
}

void gfx_image_cover(cairo_t *cr, const Image *img, Rect r, double alpha) {
	if (!img || r.w <= 0 || r.h <= 0) return;
	double s = MAX(r.w / img->w, r.h / img->h);
	cairo_save(cr);
	cairo_rectangle(cr, r.x, r.y, r.w, r.h);
	cairo_clip(cr);
	image_paint(cr, img, r.x + (r.w - img->w * s) / 2, r.y + (r.h - img->h * s) / 2, s, alpha);
	cairo_restore(cr);
}

void gfx_image_contain(cairo_t *cr, const Image *img, Rect r, double alpha) {
	if (!img || r.w <= 0 || r.h <= 0) return;
	double s = MIN(r.w / img->w, r.h / img->h);
	cairo_save(cr);
	image_paint(cr, img, r.x + (r.w - img->w * s) / 2, r.y + (r.h - img->h * s) / 2, s, alpha);
	cairo_restore(cr);
}

void gfx_image_silhouette(cairo_t *cr, const Image *img, Rect r, Color c) {
	if (!img) return;
	double s = MIN(r.w / img->w, r.h / img->h);
	cairo_save(cr);
	cairo_pattern_t *p = image_pattern(cr, img, r.x + (r.w - img->w * s) / 2, r.y + (r.h - img->h * s) / 2, s);
	gfx_set_color(cr, c);
	cairo_mask(cr, p);
	cairo_pattern_destroy(p);
	cairo_restore(cr);
}

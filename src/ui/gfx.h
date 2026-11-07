#pragma once
/* Drawing primitives: colours, rectangles, text (Pango) and images. */
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { float r, g, b, a; } Color;

#define RGBX(hex) ((Color){(float)(((hex) >> 16) & 255) / 255.f, (float)(((hex) >> 8) & 255) / 255.f, \
                           (float)((hex) & 255) / 255.f, 1.f})
#define RGBA(hex, al) ((Color){(float)(((hex) >> 16) & 255) / 255.f, (float)(((hex) >> 8) & 255) / 255.f, \
                               (float)((hex) & 255) / 255.f, (al)})
#define COL_WHITE ((Color){1, 1, 1, 1})
#define COL_BLACK ((Color){0, 0, 0, 1})
#define COL_CLEAR ((Color){0, 0, 0, 0})

static inline Color col_alpha(Color c, float a) { c.a *= a; return c; }
Color col_mix(Color a, Color b, float t);
Color col_from_u32(uint32_t rgb);
uint32_t col_to_u32(Color c);
/* Picks black or white, whichever reads better on `bg`. */
Color col_contrast(Color bg);
float col_luma(Color c);

typedef struct { double x, y, w, h; } Rect;
static inline Rect R(double x, double y, double w, double h) { return (Rect){x, y, w, h}; }
static inline bool rect_has(Rect r, double px, double py) {
	return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}
Rect rect_inset(Rect r, double d);
Rect rect_intersect(Rect a, Rect b);
static inline double rect_right(Rect r) { return r.x + r.w; }
static inline double rect_bottom(Rect r) { return r.y + r.h; }
static inline double rect_cx(Rect r) { return r.x + r.w / 2; }
static inline double rect_cy(Rect r) { return r.y + r.h / 2; }

typedef struct { double w, h; } Size;

enum { FW_LIGHT, FW_SEMILIGHT, FW_REGULAR, FW_SEMIBOLD, FW_BOLD };
enum { TA_LEFT, TA_CENTER, TA_RIGHT };

/* ---- setup */
void gfx_init(void);
bool gfx_has_font_family(const char *family);
/* Global multiplier for all text sizes (Settings > Ease of access). */
void gfx_set_text_scale(double s);
double gfx_text_scale(void);

/* ---- fills */
void gfx_set_color(cairo_t *cr, Color c);
void gfx_fill_rect(cairo_t *cr, Rect r, Color c);
void gfx_stroke_rect(cairo_t *cr, Rect r, double w, Color c);   /* border drawn inside r */
void gfx_fill_circle(cairo_t *cr, double cx, double cy, double rad, Color c);
void gfx_stroke_circle(cairo_t *cr, double cx, double cy, double rad, double w, Color c);

/* ---- text. (x,y) is the top-left corner, or top-centre/top-right for TA_CENTER/TA_RIGHT.
 * max_w > 0 limits the width (ellipsised when max_lines == 1, wrapped otherwise). */
Size gfx_text_size(cairo_t *cr, const char *s, double size, int weight, double max_w, int max_lines);
double gfx_text(cairo_t *cr, const char *s, double x, double y, double size, int weight,
                Color c, int align, double max_w, int max_lines);
/* Draws text centred inside r, honouring halign. */
void gfx_text_in(cairo_t *cr, const char *s, Rect r, double size, int weight, Color c,
                 int halign, bool vcenter);
/* Height of one line of text with this style. */
double gfx_line_height(cairo_t *cr, double size, int weight);
/* A fresh (uncached) word-wrapping Pango layout in the UI font for editors that need
 * cursor geometry. The caller releases it with g_object_unref(). */
struct _PangoLayout *gfx_layout_new(cairo_t *cr, const char *s, double size, int weight, double max_w);

/* ---- images */
typedef struct Image {
	cairo_surface_t *surf;   /* ARGB32 premultiplied */
	int w, h;
	cairo_surface_t *scaled; /* copy at the device size it was last drawn at */
	int sw, sh;
} Image;

Image *image_load(const char *path, int max_dim);                  /* PNG/JPG/BMP/GIF/SVG/... */
Image *image_load_mem(const unsigned char *data, size_t len, int max_dim);
Image *image_from_surface(cairo_surface_t *s);                     /* takes ownership */
void image_free(Image *img);
/* cover = scale to fill r (cropping), contain = scale to fit. */
void gfx_image_cover(cairo_t *cr, const Image *img, Rect r, double alpha);
void gfx_image_contain(cairo_t *cr, const Image *img, Rect r, double alpha);
/* Draws the image as a flat single-colour silhouette using its alpha channel
 * (the way Windows Phone renders third party tile icons). */
void gfx_image_silhouette(cairo_t *cr, const Image *img, Rect r, Color c);
/* Renders an SVG (or raster) icon file to an Image of the given square size. */
Image *image_load_icon(const char *path, int px);

#pragma once
/* Small utility helpers shared by the whole code base. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/util/log.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define LOGI(...) wlr_log(WLR_INFO, __VA_ARGS__)
#define LOGD(...) wlr_log(WLR_DEBUG, __VA_ARGS__)
#define LOGE(...) wlr_log(WLR_ERROR, __VA_ARGS__)

/* Monotonic clock in seconds. */
double now_sec(void);
uint32_t now_msec(void);

/* Memory / strings (abort on OOM). */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void str_copy(char *dst, size_t cap, const char *src);
bool str_eq(const char *a, const char *b);
bool str_starts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
char *str_trim(char *s);

/* Growable arrays: struct { T *items; int len, cap; } */
#define VEC(T) struct { T *items; int len, cap; }
#define vec_push(v, x) do { \
		if ((v)->len >= (v)->cap) { \
			(v)->cap = (v)->cap ? (v)->cap * 2 : 8; \
			(v)->items = xrealloc((v)->items, (size_t)(v)->cap * sizeof(*(v)->items)); \
		} \
		(v)->items[(v)->len++] = (x); \
	} while (0)
#define vec_free(v) do { free((v)->items); (v)->items = NULL; (v)->len = (v)->cap = 0; } while (0)
#define vec_remove(v, i) do { \
		memmove(&(v)->items[i], &(v)->items[(i) + 1], \
			(size_t)((v)->len - (i) - 1) * sizeof(*(v)->items)); \
		(v)->len--; \
	} while (0)

/* UTF-8 */
int utf8_decode(const char *s, uint32_t *cp);   /* returns bytes consumed (>=1), cp=0xFFFD on error */
int utf8_encode(uint32_t cp, char out[5]);      /* returns bytes written, NUL-terminated */
int utf8_prev_len(const char *s, int pos);      /* byte length of the char ending at pos */
int utf8_strlen(const char *s);

/* Lower-cases and strips diacritics (including Turkish dotted/dotless i, s-cedilla,
 * g-breve, ...) so searches like "sarki" match "Şarkı". Result is malloc'd. */
char *str_fold(const char *s);
bool str_contains_fold(const char *hay, const char *needle);

/* Files */
char *file_read_all(const char *path, size_t *len);        /* malloc'd, NUL-terminated */
bool file_write_all(const char *path, const void *data, size_t len); /* atomic (tmp+rename) */
bool mkdir_p(const char *path);
bool file_exists(const char *path);
bool is_dir(const char *path);
char *path_join(const char *a, const char *b);
const char *path_basename(const char *p);
const char *home_dir(void);
char *xdg_dir(const char *env, const char *fallback_rel, const char *sub); /* malloc'd, created */
char *human_size(double bytes, char *buf, size_t cap);

/* Locates a bundled data file (fonts, store catalog...). malloc'd or NULL. */
char *data_path(const char *rel);

/* Hex colour parse: "#rrggbb" -> 0xrrggbb, returns false on error. */
bool parse_hex_color(const char *s, uint32_t *out);

/* SHA-256 (hex) used for the lock-screen PIN. */
void sha256_hex(const char *data, size_t len, char out[65]);

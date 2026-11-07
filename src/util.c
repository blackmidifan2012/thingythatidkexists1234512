#include "util.h"
#include "build_config.h"

#include <errno.h>
#include <glib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

uint32_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}
void *xcalloc(size_t n, size_t sz) {
	void *p = calloc(n ? n : 1, sz ? sz : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}
void *xrealloc(void *p, size_t n) {
	p = realloc(p, n ? n : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}
char *xstrdup(const char *s) {
	return s ? strcpy(xmalloc(strlen(s) + 1), s) : NULL;
}
char *xasprintf(const char *fmt, ...) {
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	char *s = xmalloc((size_t)n + 1);
	vsnprintf(s, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return s;
}

void str_copy(char *dst, size_t cap, const char *src) {
	if (!cap) return;
	if (!src) src = "";
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
		/* do not cut a UTF-8 sequence in half */
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

bool str_eq(const char *a, const char *b) {
	return a && b && strcmp(a, b) == 0;
}
bool str_starts(const char *s, const char *p) {
	return s && p && strncmp(s, p, strlen(p)) == 0;
}
bool str_ends(const char *s, const char *suf) {
	if (!s || !suf) return false;
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && strcmp(s + a - b, suf) == 0;
}
char *str_trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

/* ------------------------------------------------------------------ UTF-8 */

int utf8_decode(const char *s, uint32_t *cp) {
	const unsigned char *u = (const unsigned char *)s;
	if (u[0] < 0x80) { *cp = u[0]; return u[0] ? 1 : 1; }
	int n; uint32_t c;
	if ((u[0] & 0xE0) == 0xC0) { n = 2; c = u[0] & 0x1F; }
	else if ((u[0] & 0xF0) == 0xE0) { n = 3; c = u[0] & 0x0F; }
	else if ((u[0] & 0xF8) == 0xF0) { n = 4; c = u[0] & 0x07; }
	else { *cp = 0xFFFD; return 1; }
	for (int i = 1; i < n; i++) {
		if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
		c = (c << 6) | (u[i] & 0x3F);
	}
	*cp = c;
	return n;
}

int utf8_encode(uint32_t cp, char out[5]) {
	int n;
	if (cp < 0x80) { out[0] = (char)cp; n = 1; }
	else if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); n = 2; }
	else if (cp < 0x10000) {
		out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F);
		out[2] = 0x80 | (cp & 0x3F); n = 3;
	} else {
		out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
		out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); n = 4;
	}
	out[n] = 0;
	return n;
}

int utf8_prev_len(const char *s, int pos) {
	int n = 0;
	while (pos - n > 0) {
		n++;
		if (((unsigned char)s[pos - n] & 0xC0) != 0x80) break;
	}
	return n;
}

int utf8_strlen(const char *s) {
	int n = 0;
	while (*s) {
		uint32_t cp;
		s += utf8_decode(s, &cp);
		n++;
	}
	return n;
}

char *str_fold(const char *s) {
	char *norm = g_utf8_normalize(s ? s : "", -1, G_NORMALIZE_ALL);
	if (!norm) return xstrdup("");
	char *out = xmalloc(strlen(norm) * 4 + 1), *o = out;
	for (const char *p = norm; *p;) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		if (cp >= 0x300 && cp <= 0x36F) continue;      /* combining marks */
		if (cp == 0x131 || cp == 0x130) cp = 'i';      /* dotless / dotted capital i */
		else if (cp == 0xDF) { *o++ = 's'; cp = 's'; } /* sharp s */
		else cp = g_unichar_tolower(cp);
		o += utf8_encode(cp, o);
	}
	*o = 0;
	g_free(norm);
	return out;
}

bool str_contains_fold(const char *hay, const char *needle) {
	if (!needle || !*needle) return true;
	char *h = str_fold(hay), *n = str_fold(needle);
	bool r = strstr(h, n) != NULL;
	free(h);
	free(n);
	return r;
}

/* ------------------------------------------------------------------ files */

char *file_read_all(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 4096, n = 0;
	char *buf = xmalloc(cap);
	for (;;) {
		size_t r = fread(buf + n, 1, cap - n - 1, f);
		n += r;
		if (n < cap - 1) break;
		cap *= 2;
		buf = xrealloc(buf, cap);
	}
	fclose(f);
	buf[n] = 0;
	if (len) *len = n;
	return buf;
}

bool file_write_all(const char *path, const void *data, size_t len) {
	char *tmp = xasprintf("%s.tmp%d", path, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) { free(tmp); return false; }
	bool ok = fwrite(data, 1, len, f) == len;
	ok = (fclose(f) == 0) && ok;
	if (ok) ok = rename(tmp, path) == 0;
	if (!ok) unlink(tmp);
	free(tmp);
	return ok;
}

bool mkdir_p(const char *path) {
	char *p = xstrdup(path);
	bool ok = true;
	for (char *c = p + 1; *c; c++) {
		if (*c != '/') continue;
		*c = 0;
		if (mkdir(p, 0755) != 0 && errno != EEXIST) ok = false;
		*c = '/';
	}
	if (mkdir(p, 0755) != 0 && errno != EEXIST) ok = false;
	free(p);
	return ok;
}

bool file_exists(const char *path) {
	return path && access(path, F_OK) == 0;
}

bool is_dir(const char *path) {
	struct stat st;
	return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char *path_join(const char *a, const char *b) {
	size_t n = strlen(a);
	return xasprintf("%s%s%s", a, (n && a[n - 1] == '/') ? "" : "/", b);
}

const char *path_basename(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

const char *home_dir(void) {
	const char *h = getenv("HOME");
	return h && *h ? h : "/tmp";
}

char *xdg_dir(const char *env, const char *fallback_rel, const char *sub) {
	const char *base = getenv(env);
	char *b = (base && *base) ? xstrdup(base) : path_join(home_dir(), fallback_rel);
	char *d = sub ? path_join(b, sub) : b;
	if (sub) free(b);
	mkdir_p(d);
	return d;
}

char *human_size(double bytes, char *buf, size_t cap) {
	static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
	int i = 0;
	while (bytes >= 1024 && i < 4) { bytes /= 1024; i++; }
	snprintf(buf, cap, i == 0 ? "%.0f %s" : "%.1f %s", bytes, u[i]);
	return buf;
}

char *data_path(const char *rel) {
	char *cands[5] = {0};
	const char *env = getenv("METROWL_DATADIR");
	if (env && *env) cands[0] = path_join(env, rel);
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n > 0) {
		exe[n] = 0;
		char *slash = strrchr(exe, '/');
		if (slash) {
			*slash = 0;
			char *share = path_join(exe, "../share/metrowl");
			cands[1] = path_join(share, rel);
			free(share);
		}
	}
	cands[2] = path_join(METROWL_SOURCE_DATADIR, rel);
	cands[3] = path_join(METROWL_DATADIR, rel);
	char *found = NULL;
	for (int i = 0; i < 4; i++) {
		if (!found && cands[i] && file_exists(cands[i])) found = cands[i];
		else free(cands[i]);
	}
	return found;
}

bool parse_hex_color(const char *s, uint32_t *out) {
	if (!s) return false;
	if (*s == '#') s++;
	if (strlen(s) != 6) return false;
	char *end;
	unsigned long v = strtoul(s, &end, 16);
	if (*end) return false;
	*out = (uint32_t)v;
	return true;
}

/* ----------------------------------------------------------------- SHA-256 */

typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t len; size_t fill; } Sha256;

static const uint32_t K256[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *s, const uint8_t *p) {
	uint32_t w[64], a, b, c, d, e, f, g, h;
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i*4] << 24 | p[i*4+1] << 16 | p[i*4+2] << 8 | p[i*4+3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K256[i] + w[i];
		uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_hex(const char *data, size_t len, char out[65]) {
	Sha256 s = {.h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
	const uint8_t *p = (const uint8_t *)data;
	size_t n = len;
	while (n >= 64) { sha256_block(&s, p); p += 64; n -= 64; }
	uint8_t tail[128] = {0};
	memcpy(tail, p, n);
	tail[n] = 0x80;
	size_t tl = (n < 56) ? 64 : 128;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
	sha256_block(&s, tail);
	if (tl == 128) sha256_block(&s, tail + 64);
	for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", s.h[i]);
}

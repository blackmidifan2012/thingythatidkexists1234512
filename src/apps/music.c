/* Music: the Windows Phone 8.1 music hub.
 *
 * Library: ~/Music is scanned on a worker thread. Tags are parsed here (ID3v1/v2, FLAC and
 * Ogg Vorbis/Opus comments, MP4 atoms, RIFF INFO) with hard size limits; the index is
 * cached in ~/.cache/metrowl/music.json keyed by path + mtime + size, so reopening only
 * stats the files. Album art comes from folder images or is extracted from the tags into
 * ~/.cache/metrowl/art. Playback goes through the shared player engine (player.c). */
#include "apps.h"
#include "player.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <dirent.h>
#include <glib.h>
#include <math.h>
#include <sys/stat.h>

#define MAX_FILES 20000
#define MAX_DIRS 6000
#define MAX_DEPTH 10
#define TEXT_CAP (256u << 10)      /* largest tag text block we read */
#define PIC_CAP (8u << 20)         /* largest embedded picture we extract */

/* ================================================================ tags === */

typedef struct {
	char *title, *artist, *album, *albumartist;
	int track, disc, year;
	double dur, dur_hint;
	unsigned char *pic;
	size_t piclen;
	int picscore;
	bool want_pic;
} Tags;

static void tags_free(Tags *t) {
	free(t->title); free(t->artist); free(t->album); free(t->albumartist); free(t->pic);
	memset(t, 0, sizeof *t);
}

static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const unsigned char *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint32_t be24(const unsigned char *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t syncsafe(const unsigned char *p) { return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) | ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static bool read_at(FILE *f, long off, void *dst, size_t n) {
	if (off < 0 || fseek(f, off, SEEK_SET) != 0) return false;
	return fread(dst, 1, n, f) == n;
}

static unsigned char *read_alloc(FILE *f, long off, size_t n) {
	unsigned char *b = malloc(n + 1);
	if (!b) return NULL;
	if (!read_at(f, off, b, n)) { free(b); return NULL; }
	b[n] = 0;
	return b;
}

/* Trims, rejects empty strings, and makes sure the result is valid UTF-8. */
static char *clean_str(char *s) {
	if (!s) return NULL;
	if (!g_utf8_validate(s, -1, NULL)) {
		char *fixed = g_utf8_make_valid(s, -1);
		free(s);
		s = xstrdup(fixed);
		g_free(fixed);
	}
	char *t = str_trim(s);
	if (!*t) { free(s); return NULL; }
	if (t != s) memmove(s, t, strlen(t) + 1);
	return s;
}

/* Latin-1 to UTF-8. Turkish users mostly have Windows-1254 tags, which differ from
 * Latin-1 in exactly six letters; map those when the UI is in Turkish. */
static char *latin1_to_utf8(const unsigned char *d, size_t n) {
	static const uint16_t TR1254[][2] = {{0xD0, 0x11E}, {0xDD, 0x130}, {0xDE, 0x15E}, {0xF0, 0x11F}, {0xFD, 0x131}, {0xFE, 0x15F}};
	bool tr = i18n_lang() == LANG_TR;
	char *out = xmalloc(n * 3 + 1), *o = out;
	for (size_t i = 0; i < n && d[i]; i++) {
		uint32_t cp = d[i];
		if (tr && cp >= 0xD0) for (size_t k = 0; k < ARRAY_LEN(TR1254); k++) if (TR1254[k][0] == cp) cp = TR1254[k][1];
		o += utf8_encode(cp, o);
	}
	*o = 0;
	return out;
}

static char *utf16_to_utf8(const unsigned char *d, size_t n, bool be) {
	if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { be = false; d += 2; n -= 2; }
	else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { be = true; d += 2; n -= 2; }
	size_t cnt = n / 2;
	gunichar2 *u = xmalloc((cnt + 1) * sizeof *u);
	size_t k = 0;
	for (size_t i = 0; i < cnt; i++) {
		gunichar2 c = be ? (gunichar2)((d[2 * i] << 8) | d[2 * i + 1]) : (gunichar2)(d[2 * i] | (d[2 * i + 1] << 8));
		if (!c) break;
		u[k++] = c;
	}
	char *g = g_utf16_to_utf8(u, (glong)k, NULL, NULL, NULL);
	free(u);
	char *r = xstrdup(g ? g : "");
	g_free(g);
	return r;
}

/* ID3 text with an encoding byte */
static char *id3_text(int enc, const unsigned char *d, size_t n) {
	switch (enc) {
	case 1: return clean_str(utf16_to_utf8(d, n, false));
	case 2: return clean_str(utf16_to_utf8(d, n, true));
	case 3: {
		size_t l = strnlen((const char *)d, n);
		char *s = xmalloc(l + 1);
		memcpy(s, d, l);
		s[l] = 0;
		if (!g_utf8_validate(s, -1, NULL)) { free(s); return clean_str(latin1_to_utf8(d, l)); }
		return clean_str(s);
	}
	default: return clean_str(latin1_to_utf8(d, n));
	}
}

/* Length of an encoding-terminated string (including the terminator). */
static size_t id3_strlen(int enc, const unsigned char *d, size_t n) {
	if (enc == 1 || enc == 2) {
		for (size_t i = 0; i + 1 < n; i += 2) if (!d[i] && !d[i + 1]) return i + 2;
		return n;
	}
	size_t l = strnlen((const char *)d, n);
	return l < n ? l + 1 : n;
}

static void set_str(char **dst, char *s) {
	if (!s) return;
	if (*dst) { free(s); return; }
	*dst = s;
}

static int leading_int(const char *s) {
	if (!s) return 0;
	while (*s == ' ' || *s == '(') s++;
	int v = 0, k = 0;
	while (*s >= '0' && *s <= '9' && k < 9) { v = v * 10 + (*s++ - '0'); k++; }
	return v;
}

static void set_year(Tags *t, const char *s) {
	if (t->year || !s) return;
	int y = leading_int(s);
	if (y >= 1000 && y <= 2999) t->year = y;
}

static void take_pic(Tags *t, int type, const unsigned char *d, size_t n) {
	if (!t->want_pic || n < 64 || n > PIC_CAP) return;
	bool jpg = d[0] == 0xFF && d[1] == 0xD8, png = !memcmp(d, "\x89PNG", 4);
	if (!jpg && !png) return;
	int score = type == 3 ? 3 : type == 0 ? 2 : 1;   /* front cover > other > the rest */
	if (score <= t->picscore) return;
	free(t->pic);
	t->pic = xmalloc(n);
	memcpy(t->pic, d, n);
	t->piclen = n;
	t->picscore = score;
}

/* ---- ID3v2 */

/* Reads an ID3v2 tag at the start of the file. Returns the total tag size (0 = none). */
static long id3v2_parse(FILE *f, Tags *t) {
	unsigned char h[10];
	if (!read_at(f, 0, h, 10) || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4 || (h[6] | h[7] | h[8] | h[9]) & 0x80)
		return 0;
	int ver = h[3], flags = h[5];
	size_t size = syncsafe(h + 6);
	long total = (long)size + 10 + ((flags & 0x10) ? 10 : 0);
	bool unsync = (flags & 0x80) && ver < 4;
	/* frames are read one by one, except for whole-tag unsynchronisation (rare, small) */
	unsigned char *mem = NULL;
	if (unsync) {
		size_t n = MIN(size, (size_t)(2u << 20));
		mem = read_alloc(f, 10, n);
		if (!mem) return total;
		size_t o = 0;
		for (size_t i = 0; i < n; i++) { mem[o++] = mem[i]; if (mem[i] == 0xFF && i + 1 < n && mem[i + 1] == 0) i++; }
		size = o;
	}
#define TREAD(off, dst, n) (mem ? ((off) + (n) <= size ? (memcpy(dst, mem + (off), n), true) : false) : read_at(f, (long)(10 + (off)), dst, n))
	size_t pos = 0;
	if ((flags & 0x40) && ver >= 3) {   /* extended header */
		unsigned char e[4];
		if (!TREAD(0, e, 4)) goto out;
		pos = ver == 4 ? syncsafe(e) : be32(e) + 4;
	}
	int hdr = ver == 2 ? 6 : 10;
	for (int guard = 0; guard < 400 && pos + (size_t)hdr <= size; guard++) {
		unsigned char fh[10];
		if (!TREAD(pos, fh, (size_t)hdr)) break;
		if (!fh[0]) break;   /* padding */
		bool ok = true;
		for (int i = 0; i < (ver == 2 ? 3 : 4); i++) if (!((fh[i] >= 'A' && fh[i] <= 'Z') || (fh[i] >= '0' && fh[i] <= '9'))) ok = false;
		if (!ok) break;
		char id[5] = {0};
		memcpy(id, fh, ver == 2 ? 3 : 4);
		size_t fs = ver == 2 ? be24(fh + 3) : ver == 3 ? be32(fh + 4) : syncsafe(fh + 4);
		if (ver == 4 && fs >= 0x80) {
			/* some writers store plain sizes in v2.4: prefer whichever lands on a frame */
			size_t plain = be32(fh + 4);
			unsigned char nx[4];
			bool sync_ok = pos + hdr + fs + 4 <= size && TREAD(pos + hdr + fs, nx, 4) && ((nx[0] >= 'A' && nx[0] <= 'Z') || !nx[0]);
			if (!sync_ok && plain != fs) {
				bool plain_ok = pos + hdr + plain == size || (pos + hdr + plain + 4 <= size && TREAD(pos + hdr + plain, nx, 4) &&
				                                              ((nx[0] >= 'A' && nx[0] <= 'Z') || !nx[0]));
				if (plain_ok) fs = plain;
			}
		}
		if (fs > size - pos - hdr) break;
		size_t body = pos + hdr;
		pos = body + fs;
		int fflags = ver == 2 ? 0 : (fh[8] << 8) | fh[9];
		if (ver == 3 && (fflags & 0xC0)) continue;      /* compressed / encrypted */
		if (ver == 4 && (fflags & 0x0C)) continue;
		bool is_pic = !strcmp(id, "APIC") || !strcmp(id, "PIC");
		bool is_text = id[0] == 'T';
		if (!is_pic && !is_text) continue;
		if (is_pic && (!t->want_pic || fs > PIC_CAP)) continue;
		if (is_text && fs > 64 * 1024) continue;
		unsigned char *d = malloc(fs + 2);
		if (!d) continue;
		if (!TREAD(body, d, fs)) { free(d); break; }
		d[fs] = d[fs + 1] = 0;
		size_t n = fs;
		unsigned char *p = d;
		if (ver == 4 && (fflags & 0x01) && n >= 4) { p += 4; n -= 4; }   /* data length indicator */
		if (ver == 4 && (fflags & 0x02)) {
			size_t o = 0;
			for (size_t i = 0; i < n; i++) { p[o++] = p[i]; if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0) i++; }
			n = o;
		}
		if (n < 2) { free(d); continue; }
		int enc = p[0] <= 3 ? p[0] : 0;
		if (is_text) {
			char *s = id3_text(enc, p + 1, n - 1);
			if (!s) { free(d); continue; }
			if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) set_str(&t->title, s);
			else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) set_str(&t->artist, s);
			else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) set_str(&t->album, s);
			else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) set_str(&t->albumartist, s);
			else {
				if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) { if (!t->track) t->track = leading_int(s); }
				else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) { if (!t->disc) t->disc = leading_int(s); }
				else if (!strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TDRC") || !strcmp(id, "TDOR") || !strcmp(id, "TORY")) set_year(t, s);
				else if (!strcmp(id, "TLEN") || !strcmp(id, "TLE")) { double ms = atof(s); if (ms > 1000 && ms < 4e8) t->dur_hint = ms / 1000; }
				free(s);
			}
		} else {
			size_t o = 1;
			int type = 0;
			if (ver == 2) {
				o += 3;
			} else {
				size_t ml = strnlen((const char *)p + o, n - o);
				o += ml + 1;
			}
			if (o < n) { type = p[o]; o++; }
			if (o < n) o += id3_strlen(enc, p + o, n - o);
			if (o < n) take_pic(t, type, p + o, n - o);
		}
		free(d);
	}
out:
#undef TREAD
	free(mem);
	return total;
}

static void id3v1_parse(FILE *f, long fsize, Tags *t) {
	unsigned char b[128];
	if (fsize < 128 || !read_at(f, fsize - 128, b, 128) || memcmp(b, "TAG", 3)) return;
	char tmp[31];
	char **dst[3] = {&t->title, &t->artist, &t->album};
	for (int i = 0; i < 3; i++) {
		memcpy(tmp, b + 3 + i * 30, 30);
		tmp[30] = 0;
		if (!*dst[i]) set_str(dst[i], clean_str(latin1_to_utf8((unsigned char *)tmp, 30)));
	}
	memcpy(tmp, b + 93, 4);
	tmp[4] = 0;
	set_year(t, tmp);
	if (!t->track && b[125] == 0 && b[126]) t->track = b[126];
}

/* ---- MPEG audio duration */

static double mp3_duration(FILE *f, long start, long end) {
	static const short BR[2][3][15] = {
		{{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
		 {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
		 {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
		{{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
		 {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
		 {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
	};
	static const int SR[3][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}};
	static const int SR1[3] = {44100, 48000, 32000};
	enum { BUF = 64 * 1024 };
	unsigned char *b = malloc(BUF);
	if (!b) return 0;
	size_t n = 0;
	if (fseek(f, start, SEEK_SET) == 0) n = fread(b, 1, BUF, f);
	double dur = 0;
	for (size_t i = 0; i + 4 <= n; i++) {
		if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
		int ver = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, bri = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3;
		if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;
		bool v1 = ver == 3;
		int L = 3 - layer;   /* 0 = layer I, 1 = II, 2 = III */
		int br = BR[v1][L][bri] * 1000, sr = v1 ? SR1[sri] : SR[ver][sri];
		int pad = (b[i + 2] >> 1) & 1, mono = (b[i + 3] >> 6) == 3;
		if (!sr || !br) continue;
		int spf = L == 0 ? 384 : (L == 2 && !v1) ? 576 : 1152;
		long flen = L == 0 ? (12L * br / sr + pad) * 4 : (long)spf / 8 * br / sr + pad;
		if (flen < 21) continue;
		/* require a second frame header right behind this one when it is in the buffer */
		if (i + (size_t)flen + 2 <= n && (b[i + flen] != 0xFF || (b[i + flen + 1] & 0xE0) != 0xE0)) continue;
		size_t xo = i + 4 + (v1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
		if (xo + 12 <= n && (!memcmp(b + xo, "Xing", 4) || !memcmp(b + xo, "Info", 4))) {
			uint32_t fl = be32(b + xo + 4);
			if (fl & 1) { uint32_t frames = be32(b + xo + 8); if (frames) dur = (double)frames * spf / sr; }
		}
		if (!dur && i + 4 + 32 + 18 <= n && !memcmp(b + i + 36, "VBRI", 4)) {
			uint32_t frames = be32(b + i + 36 + 14);
			if (frames) dur = (double)frames * spf / sr;
		}
		if (!dur) dur = (double)(end - (start + (long)i)) * 8.0 / br;
		break;
	}
	free(b);
	return dur;
}

/* ---- Vorbis comments (FLAC, Ogg Vorbis, Opus) */

static void flac_picture(Tags *t, const unsigned char *d, size_t n) {
	if (n < 32) return;
	uint32_t type = be32(d);
	size_t o = 4;
	uint32_t ml = be32(d + o); o += 4;
	if (ml > n - o) return;
	o += ml;
	if (o + 4 > n) return;
	uint32_t dl = be32(d + o); o += 4;
	if (dl > n - o) return;
	o += dl + 16;
	if (o + 4 > n) return;
	uint32_t len = be32(d + o); o += 4;
	if (len > n - o) return;
	take_pic(t, (int)type, d + o, len);
}

static void vorbis_comments(Tags *t, const unsigned char *d, size_t n) {
	if (n < 8) return;
	uint32_t vl = le32(d);
	if (vl > n - 8) return;
	size_t o = 4 + vl;
	uint32_t cnt = le32(d + o);
	o += 4;
	for (uint32_t i = 0; i < cnt && i < 2000 && o + 4 <= n; i++) {
		uint32_t l = le32(d + o);
		o += 4;
		if (l > n - o) break;
		const char *c = (const char *)d + o;
		const char *eq = memchr(c, '=', l);
		size_t o_next = o + l;
		if (eq) {
			size_t kl = (size_t)(eq - c), vlen = l - kl - 1;
			char key[32];
			if (kl < sizeof key) {
				for (size_t k = 0; k < kl; k++) key[k] = (char)g_ascii_toupper(c[k]);
				key[kl] = 0;
				const unsigned char *v = (const unsigned char *)eq + 1;
				if (!strcmp(key, "METADATA_BLOCK_PICTURE") || !strcmp(key, "COVERART")) {
					if (t->want_pic && vlen < PIC_CAP * 4 / 3 + 8) {
						char *b64 = xmalloc(vlen + 1);
						memcpy(b64, v, vlen);
						b64[vlen] = 0;
						gsize dl = 0;
						guchar *raw = g_base64_decode(b64, &dl);
						if (raw && key[0] == 'M') flac_picture(t, raw, dl);
						else if (raw) take_pic(t, 3, raw, dl);
						g_free(raw);
						free(b64);
					}
				} else if (vlen < 4096) {
					char *s = xmalloc(vlen + 1);
					memcpy(s, v, vlen);
					s[vlen] = 0;
					s = clean_str(s);
					if (!s) {}
					else if (!strcmp(key, "TITLE")) set_str(&t->title, s);
					else if (!strcmp(key, "ARTIST")) set_str(&t->artist, s);
					else if (!strcmp(key, "ALBUM")) set_str(&t->album, s);
					else if (!strcmp(key, "ALBUMARTIST") || !strcmp(key, "ALBUM ARTIST")) set_str(&t->albumartist, s);
					else {
						if (!strcmp(key, "TRACKNUMBER")) { if (!t->track) t->track = leading_int(s); }
						else if (!strcmp(key, "DISCNUMBER")) { if (!t->disc) t->disc = leading_int(s); }
						else if (!strcmp(key, "DATE") || !strcmp(key, "YEAR") || !strcmp(key, "ORIGINALDATE")) set_year(t, s);
						free(s);
					}
				}
			}
		}
		o = o_next;
	}
}

static void flac_parse(FILE *f, Tags *t) {
	unsigned char h[4];
	long pos = 4;
	if (!read_at(f, 0, h, 4)) return;
	if (!memcmp(h, "ID3", 3)) {   /* tolerated in front of fLaC */
		unsigned char ih[10];
		if (!read_at(f, 0, ih, 10)) return;
		pos = 10 + (long)syncsafe(ih + 6);
		if (!read_at(f, pos, h, 4)) return;
		pos += 4;
	}
	if (memcmp(h, "fLaC", 4)) return;
	for (int i = 0; i < 64; i++) {
		unsigned char bh[4];
		if (!read_at(f, pos, bh, 4)) return;
		int type = bh[0] & 0x7f;
		uint32_t len = be24(bh + 1);
		long body = pos + 4;
		if (type == 0 && len >= 18) {
			unsigned char si[18];
			if (read_at(f, body, si, 18)) {
				uint32_t sr = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (si[12] >> 4);
				uint64_t total = ((uint64_t)(si[13] & 0x0f) << 32) | be32(si + 14);
				if (sr) t->dur = (double)total / sr;
			}
		} else if (type == 4 && len <= TEXT_CAP * 4) {
			unsigned char *d = read_alloc(f, body, len);
			if (d) { vorbis_comments(t, d, len); free(d); }
		} else if (type == 6 && t->want_pic && len <= PIC_CAP + 4096) {
			unsigned char *d = read_alloc(f, body, len);
			if (d) { flac_picture(t, d, len); free(d); }
		}
		pos = body + (long)len;
		if (bh[0] & 0x80) break;
	}
}

/* ---- Ogg (Vorbis / Opus) */

static void ogg_parse(FILE *f, long fsize, Tags *t) {
	size_t cap = t->want_pic ? PIC_CAP * 2 : TEXT_CAP * 4;
	unsigned char *pk = NULL;
	size_t pklen = 0;
	int npk = 0;
	long pos = 0;
	uint32_t serial = 0;
	int rate = 0, preskip = 0;
	bool opus = false, done = false;
	for (int pages = 0; pages < 3000 && !done; pages++) {
		unsigned char ph[27], seg[255];
		if (!read_at(f, pos, ph, 27) || memcmp(ph, "OggS", 4)) break;
		int ns = ph[26];
		if (!read_at(f, pos + 27, seg, (size_t)ns)) break;
		if (pages == 0) serial = le32(ph + 14);
		long data = pos + 27 + ns, plen = 0;
		for (int i = 0; i < ns; i++) plen += seg[i];
		if (le32(ph + 14) != serial) { pos = data + plen; continue; }
		long o = data;
		for (int i = 0; i < ns && !done;) {
			/* one packet fragment: lacing values up to the first < 255 */
			size_t fl = 0;
			bool end = false;
			while (i < ns) { fl += seg[i]; if (seg[i++] < 255) { end = true; break; } }
			if (pklen + fl > cap) { done = true; break; }
			pk = xrealloc(pk, pklen + fl + 1);
			if (fl && !read_at(f, o, pk + pklen, fl)) { done = true; break; }
			pklen += fl;
			o += (long)fl;
			if (!end) break;
			/* complete packet */
			if (npk == 0) {
				if (pklen >= 16 && !memcmp(pk, "\x01vorbis", 7)) rate = (int)le32(pk + 12);
				else if (pklen >= 12 && !memcmp(pk, "OpusHead", 8)) { opus = true; rate = 48000; preskip = pk[10] | (pk[11] << 8); }
				else done = true;
			} else {
				if (!opus && pklen > 7 && !memcmp(pk, "\x03vorbis", 7)) vorbis_comments(t, pk + 7, pklen - 7);
				else if (opus && pklen > 8 && !memcmp(pk, "OpusTags", 8)) vorbis_comments(t, pk + 8, pklen - 8);
				done = true;
			}
			npk++;
			pklen = 0;
		}
		pos = data + plen;
	}
	free(pk);
	if (!rate) return;
	/* duration from the granule position of the last page */
	long tail = MIN(fsize, 65536L);
	unsigned char *b = read_alloc(f, fsize - tail, (size_t)tail);
	if (!b) return;
	for (long i = tail - 27; i >= 0; i--) {
		if (b[i] == 'O' && !memcmp(b + i, "OggS", 4) && le32(b + i + 14) == serial) {
			int64_t g = (int64_t)le64(b + i + 6);
			if (g > 0) t->dur = (double)(g - preskip) / rate;
			break;
		}
	}
	free(b);
}

/* ---- MP4 / M4A */

static bool mp4_find(const unsigned char *d, size_t n, const char *type, size_t *off, size_t *len) {
	size_t o = 0;
	while (o + 8 <= n) {
		uint64_t sz = be32(d + o);
		size_t h = 8;
		if (sz == 1) { if (o + 16 > n) return false; sz = ((uint64_t)be32(d + o + 8) << 32) | be32(d + o + 12); h = 16; }
		else if (sz == 0) sz = n - o;
		if (sz < h || sz > n - o) return false;
		if (!memcmp(d + o + 4, type, 4)) { *off = o + h; *len = (size_t)sz - h; return true; }
		o += (size_t)sz;
	}
	return false;
}

static void mp4_ilst(Tags *t, const unsigned char *d, size_t n) {
	size_t o = 0;
	while (o + 8 <= n) {
		size_t sz = be32(d + o);
		if (sz < 8 || sz > n - o) break;
		const unsigned char *it = d + o + 8;
		size_t itn = sz - 8, doff, dlen;
		if (mp4_find(it, itn, "data", &doff, &dlen) && dlen >= 8) {
			const unsigned char *v = it + doff + 8;
			size_t vl = dlen - 8;
			const unsigned char *k = d + o + 4;
			if (!memcmp(k, "covr", 4)) take_pic(t, 3, v, vl);
			else if (!memcmp(k, "trkn", 4) && vl >= 4) { if (!t->track) t->track = (v[2] << 8) | v[3]; }
			else if (!memcmp(k, "disk", 4) && vl >= 4) { if (!t->disc) t->disc = (v[2] << 8) | v[3]; }
			else if (vl < 4096) {
				char *s = xmalloc(vl + 1);
				memcpy(s, v, vl);
				s[vl] = 0;
				s = clean_str(s);
				if (!s) {}
				else if (!memcmp(k, "\xa9nam", 4)) set_str(&t->title, s);
				else if (!memcmp(k, "\xa9" "ART", 4)) set_str(&t->artist, s);
				else if (!memcmp(k, "\xa9" "alb", 4)) set_str(&t->album, s);
				else if (!memcmp(k, "aART", 4)) set_str(&t->albumartist, s);
				else { if (!memcmp(k, "\xa9" "day", 4)) set_year(t, s); free(s); }
			}
		}
		o += sz;
	}
}

static void mp4_parse(FILE *f, long fsize, Tags *t) {
	long pos = 0;
	for (int i = 0; i < 64 && pos + 8 <= fsize; i++) {
		unsigned char h[16];
		if (!read_at(f, pos, h, 8)) return;
		uint64_t sz = be32(h);
		long hl = 8;
		if (sz == 1) { if (!read_at(f, pos, h, 16)) return; sz = ((uint64_t)be32(h + 8) << 32) | be32(h + 12); hl = 16; }
		else if (sz == 0) sz = (uint64_t)(fsize - pos);
		if (sz < (uint64_t)hl) return;
		if (!memcmp(h + 4, "moov", 4)) {
			size_t n = (size_t)sz - (size_t)hl;
			if (n > (t->want_pic ? 24u << 20 : 12u << 20)) return;
			unsigned char *m = read_alloc(f, pos + hl, n);
			if (!m) return;
			size_t o, l;
			if (mp4_find(m, n, "mvhd", &o, &l) && l >= 32) {
				const unsigned char *v = m + o;
				if (v[0] == 1 && l >= 32) {
					uint32_t ts = be32(v + 20);
					uint64_t du = ((uint64_t)be32(v + 24) << 32) | be32(v + 28);
					if (ts) t->dur = (double)du / ts;
				} else {
					uint32_t ts = be32(v + 12), du = be32(v + 16);
					if (ts) t->dur = (double)du / ts;
				}
			}
			size_t uo, ul, mo, ml, io, il;
			const unsigned char *base = m;
			size_t bn = n;
			if (mp4_find(m, n, "udta", &uo, &ul)) { base = m + uo; bn = ul; }
			if (mp4_find(base, bn, "meta", &mo, &ml) && ml > 4 && mp4_find(base + mo + 4, ml - 4, "ilst", &io, &il))
				mp4_ilst(t, base + mo + 4 + io, il);
			free(m);
			return;
		}
		pos += (long)sz;
	}
}

/* ---- RIFF WAVE */

static void wav_parse(FILE *f, long fsize, Tags *t) {
	unsigned char h[12];
	if (!read_at(f, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
	long pos = 12;
	uint32_t byterate = 0;
	for (int i = 0; i < 64 && pos + 8 <= fsize; i++) {
		unsigned char c[8];
		if (!read_at(f, pos, c, 8)) break;
		uint32_t len = le32(c + 4);
		long body = pos + 8;
		if (!memcmp(c, "fmt ", 4) && len >= 16) {
			unsigned char fm[16];
			if (read_at(f, body, fm, 16)) byterate = le32(fm + 8);
		} else if (!memcmp(c, "data", 4)) {
			long avail = fsize - body;
			long dl = (len == 0 || len == 0xFFFFFFFFu || (long)len > avail) ? avail : (long)len;
			if (byterate) t->dur = (double)dl / byterate;
			if ((long)len > avail || len == 0) break;
		} else if (!memcmp(c, "LIST", 4) && len >= 4 && len < 65536) {
			unsigned char *d = read_alloc(f, body, len);
			if (d && !memcmp(d, "INFO", 4)) {
				for (size_t o = 4; o + 8 <= len;) {
					uint32_t sl = le32(d + o + 4);
					if (sl > len - o - 8) break;
					char *s = clean_str(latin1_to_utf8(d + o + 8, sl));
					if (!memcmp(d + o, "INAM", 4)) set_str(&t->title, s);
					else if (!memcmp(d + o, "IART", 4)) set_str(&t->artist, s);
					else if (!memcmp(d + o, "IPRD", 4)) set_str(&t->album, s);
					else { if (!memcmp(d + o, "ICRD", 4)) set_year(t, s); else if (!memcmp(d + o, "ITRK", 4) && !t->track) t->track = leading_int(s); free(s); }
					o += 8 + sl + (sl & 1);
				}
			}
			free(d);
		}
		pos = body + (long)len + (len & 1);
	}
}

static const char *ext_lower(const char *path, char out[8]) {
	const char *b = path_basename(path), *d = strrchr(b, '.');
	out[0] = 0;
	if (!d || strlen(d + 1) >= 8) return out;
	for (int i = 0; d[1 + i]; i++) out[i] = (char)g_ascii_tolower(d[1 + i]), out[i + 1] = 0;
	return out;
}

static bool is_audio(const char *name) {
	char e[8];
	ext_lower(name, e);
	return !strcmp(e, "mp3") || !strcmp(e, "flac") || !strcmp(e, "ogg") || !strcmp(e, "oga") || !strcmp(e, "opus") ||
	       !strcmp(e, "m4a") || !strcmp(e, "wav") || !strcmp(e, "aac") || !strcmp(e, "mp4");
}

static void tags_read(const char *path, long fsize, Tags *t) {
	FILE *f = fopen(path, "rb");
	if (!f) return;
	char e[8];
	ext_lower(path, e);
	unsigned char magic[12] = {0};
	read_at(f, 0, magic, 12);
	if (!memcmp(magic, "fLaC", 4) || !strcmp(e, "flac")) flac_parse(f, t);
	else if (!memcmp(magic, "OggS", 4)) ogg_parse(f, fsize, t);
	else if (!memcmp(magic, "RIFF", 4)) wav_parse(f, fsize, t);
	else if (!memcmp(magic + 4, "ftyp", 4)) mp4_parse(f, fsize, t);
	else {
		long tag = id3v2_parse(f, t);
		unsigned char v1[3];
		long end = fsize;
		if (fsize >= 128 && read_at(f, fsize - 128, v1, 3) && !memcmp(v1, "TAG", 3)) end -= 128;
		id3v1_parse(f, fsize, t);
		if (!strcmp(e, "mp3") || !strcmp(e, "mp2")) t->dur = mp3_duration(f, tag, end);
		if (t->dur_hint > 0 && (t->dur <= 0 || fabs(t->dur - t->dur_hint) > 30)) t->dur = t->dur_hint;
	}
	fclose(f);
}

/* ============================================================= library === */

typedef struct {
	char *path, *title, *artist, *album, *albumartist;
	char *art;                  /* embedded picture extracted to the cache */
	char *cover;                /* folder image (not cached: re-checked every scan) */
	int track, disc, year;
	double dur;
	int64_t mtime, size;
	int alb, art_i;             /* album / artist index */
	char letter[8];
} Song;

typedef struct {
	char *name, *artist;        /* artist = album artist or the common track artist ("" = various) */
	char *key;
	bool various;
	int year;
	int *songs, n;              /* sorted by disc / track */
	double dur;
	int orig;
	char letter[8];
} Album;

typedef struct {
	char *name;
	int *albums, nalb;
	int *songs, n;              /* sorted by title */
	int orig;
	char letter[8];
} Artist;

typedef struct { char *name, *path; char **items; int n; } M3u;

typedef struct {
	Song *songs; int nsongs;    /* sorted by title */
	Album *albums; int nalbums; /* sorted by name */
	Artist *artists; int nartists;
	M3u *m3u; int nm3u;
	char (*song_letters)[8];
	char (*artist_letters)[8];
	GHashTable *by_path;        /* path -> index + 1 */
	int lang;
	bool truncated;
} Lib;

static const char *song_art(const Song *s) { return s->cover ? s->cover : s->art; }

static void lib_free(Lib *l) {
	if (!l) return;
	for (int i = 0; i < l->nsongs; i++) {
		Song *s = &l->songs[i];
		free(s->path); free(s->title); free(s->artist); free(s->album); free(s->albumartist); free(s->art); free(s->cover);
	}
	for (int i = 0; i < l->nalbums; i++) { free(l->albums[i].name); free(l->albums[i].artist); free(l->albums[i].key); free(l->albums[i].songs); }
	for (int i = 0; i < l->nartists; i++) { free(l->artists[i].name); free(l->artists[i].albums); free(l->artists[i].songs); }
	for (int i = 0; i < l->nm3u; i++) {
		free(l->m3u[i].name); free(l->m3u[i].path);
		for (int k = 0; k < l->m3u[i].n; k++) free(l->m3u[i].items[k]);
		free(l->m3u[i].items);
	}
	free(l->songs); free(l->albums); free(l->artists); free(l->m3u);
	free(l->song_letters); free(l->artist_letters);
	if (l->by_path) g_hash_table_destroy(l->by_path);
	free(l);
}

static char *music_dir(void) { return path_join(home_dir(), "Music"); }
static char *cache_file(void) {
	char *d = xdg_dir("XDG_CACHE_HOME", ".cache", "metrowl");
	char *p = path_join(d, "music.json");
	free(d);
	return p;
}

static uint64_t fnv64(const char *s) {
	uint64_t h = 1469598103934665603ull;
	while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; }
	return h;
}

/* "03 - Song name" -> "Song name", "Artist - Song" -> artist + title */
static void names_from_file(Song *s, const char *root) {
	const char *base = path_basename(s->path);
	char name[512];
	str_copy(name, sizeof name, base);
	char *dot = strrchr(name, '.');
	if (dot && dot != name) *dot = 0;
	char *p = name;
	int num = 0, k = 0;
	while (*p >= '0' && *p <= '9' && k < 3) { num = num * 10 + (*p++ - '0'); k++; }
	if (k && (*p == ' ' || *p == '.' || *p == '-' || *p == '_')) {
		while (*p == ' ' || *p == '.' || *p == '-' || *p == '_') p++;
		if (!s->track) s->track = num;
	} else p = name;
	if (!*p) p = name;
	char *sep = strstr(p, " - ");
	if (sep && !s->artist && !s->title) {
		*sep = 0;
		s->artist = clean_str(xstrdup(p));
		p = sep + 3;
	}
	if (!s->title) s->title = clean_str(xstrdup(p));
	if (!s->title) s->title = xstrdup(base);
	/* Music/Artist/Album/track.ext */
	char *dir = g_path_get_dirname(s->path);
	if (!s->album && strcmp(dir, root) && str_starts(dir, root)) {
		s->album = clean_str(xstrdup(path_basename(dir)));
		char *up = g_path_get_dirname(dir);
		if (!s->artist && strcmp(up, root) && str_starts(up, root)) s->artist = clean_str(xstrdup(path_basename(up)));
		g_free(up);
	}
	g_free(dir);
}

typedef struct {
	char *root, *cache, *artdir;
	bool cache_only;
	int lang;
	Lib *out;
	bool changed;
} ScanJob;

typedef struct { char *path; int64_t mtime, size; char *cover; } FileRef;
typedef VEC(FileRef) FileVec;
typedef VEC(char *) StrVec;

static const char *const COVER_NAMES[] = {"cover.jpg", "folder.jpg", "front.jpg", "cover.jpeg", "folder.jpeg", "front.jpeg",
	"cover.png", "folder.png", "front.png", "albumart.jpg", "albumartsmall.jpg", "album.jpg", NULL};

static int cover_rank(const char *name) {
	for (int i = 0; COVER_NAMES[i]; i++) if (!g_ascii_strcasecmp(name, COVER_NAMES[i])) return i;
	return -1;
}

static void walk(const char *root, FileVec *files, StrVec *m3u, bool *truncated) {
	VEC(char *) stack = {0};
	VEC(int) depth = {0};
	vec_push(&stack, xstrdup(root));
	vec_push(&depth, 0);
	int ndirs = 0;
	while (stack.len) {
		char *dir = stack.items[--stack.len];
		int d = depth.items[--depth.len];
		DIR *dh = opendir(dir);
		if (!dh) { free(dir); continue; }
		ndirs++;
		char *cover = NULL;
		int cover_r = 1000;
		int first_file = files->len;
		struct dirent *e;
		while ((e = readdir(dh))) {
			if (e->d_name[0] == '.') continue;
			char *p = path_join(dir, e->d_name);
			struct stat st;
			if (stat(p, &st) != 0) { free(p); continue; }
			if (S_ISDIR(st.st_mode)) {
				if (e->d_type == DT_LNK || d + 1 > MAX_DEPTH || ndirs + stack.len >= MAX_DIRS) { free(p); *truncated |= d + 1 > MAX_DEPTH; continue; }
				vec_push(&stack, p);
				vec_push(&depth, d + 1);
			} else if (S_ISREG(st.st_mode)) {
				int cr = cover_rank(e->d_name);
				char e8[8];
				ext_lower(e->d_name, e8);
				if (cr >= 0 && cr < cover_r) { free(cover); cover = p; cover_r = cr; }
				else if (is_audio(e->d_name) && st.st_size > 0) {
					if (files->len >= MAX_FILES) { *truncated = true; free(p); continue; }
					FileRef fr = {p, (int64_t)st.st_mtime, (int64_t)st.st_size, NULL};
					vec_push(files, fr);
				} else if ((!strcmp(e8, "m3u") || !strcmp(e8, "m3u8")) && m3u->len < 200) vec_push(m3u, p);
				else free(p);
			} else free(p);
		}
		closedir(dh);
		if (cover) for (int i = first_file; i < files->len; i++) files->items[i].cover = xstrdup(cover);
		free(cover);
		free(dir);
	}
	vec_free(&stack);
	vec_free(&depth);
}

static char *jstr(cJSON *o, const char *k) {
	cJSON *v = cJSON_GetObjectItem(o, k);
	return cJSON_IsString(v) && v->valuestring[0] ? xstrdup(v->valuestring) : NULL;
}
static double jnum(cJSON *o, const char *k) {
	cJSON *v = cJSON_GetObjectItem(o, k);
	return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static Song song_from_json(cJSON *o) {
	Song s = {0};
	s.path = jstr(o, "p");
	s.title = jstr(o, "t");
	s.artist = jstr(o, "a");
	s.album = jstr(o, "al");
	s.albumartist = jstr(o, "aa");
	s.art = jstr(o, "art");
	s.cover = jstr(o, "cv");
	s.track = (int)jnum(o, "n");
	s.disc = (int)jnum(o, "dn");
	s.year = (int)jnum(o, "y");
	s.dur = jnum(o, "d");
	s.mtime = (int64_t)jnum(o, "m");
	s.size = (int64_t)jnum(o, "s");
	return s;
}

static cJSON *song_to_json(const Song *s) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "p", s->path);
	cJSON_AddNumberToObject(o, "m", (double)s->mtime);
	cJSON_AddNumberToObject(o, "s", (double)s->size);
	if (s->title) cJSON_AddStringToObject(o, "t", s->title);
	if (s->artist) cJSON_AddStringToObject(o, "a", s->artist);
	if (s->album) cJSON_AddStringToObject(o, "al", s->album);
	if (s->albumartist) cJSON_AddStringToObject(o, "aa", s->albumartist);
	if (s->art) cJSON_AddStringToObject(o, "art", s->art);
	if (s->cover) cJSON_AddStringToObject(o, "cv", s->cover);
	if (s->track) cJSON_AddNumberToObject(o, "n", s->track);
	if (s->disc) cJSON_AddNumberToObject(o, "dn", s->disc);
	if (s->year) cJSON_AddNumberToObject(o, "y", s->year);
	if (s->dur > 0) cJSON_AddNumberToObject(o, "d", round(s->dur * 10) / 10);
	return o;
}

/* Parses a file's tags; embedded art is written to the art cache when the folder has none. */
static Song song_scan(const FileRef *fr, const char *root, const char *artdir) {
	Song s = {0};
	s.path = xstrdup(fr->path);
	s.mtime = fr->mtime;
	s.size = fr->size;
	s.cover = fr->cover ? xstrdup(fr->cover) : NULL;
	Tags t = {0};
	t.want_pic = !fr->cover;
	tags_read(fr->path, (long)fr->size, &t);
	s.title = t.title; s.artist = t.artist; s.album = t.album; s.albumartist = t.albumartist;
	t.title = t.artist = t.album = t.albumartist = NULL;
	s.track = t.track; s.disc = t.disc; s.year = t.year; s.dur = t.dur;
	if (t.pic) {
		/* one picture per album (directory + album name) */
		char *dir = g_path_get_dirname(fr->path);
		char *key = xasprintf("%s|%s", dir, s.album ? s.album : "");
		bool png = !memcmp(t.pic, "\x89PNG", 4);
		char name[48];
		snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)fnv64(key), png ? "png" : "jpg");
		char *ap = path_join(artdir, name);
		if (file_exists(ap) || file_write_all(ap, t.pic, t.piclen)) s.art = ap;
		else free(ap);
		free(key);
		g_free(dir);
	}
	tags_free(&t);
	names_from_file(&s, root);
	return s;
}

static Lib *g_sort_lib;

static int cmp_song(const void *a, const void *b) {
	const Song *x = a, *y = b;
	int c = app_collate(x->title, y->title);
	if (c) return c;
	c = app_collate(x->artist ? x->artist : "", y->artist ? y->artist : "");
	return c ? c : strcmp(x->path, y->path);
}

static int cmp_album(const void *a, const void *b) {
	const Album *x = a, *y = b;
	int c = app_collate(x->name, y->name);
	return c ? c : app_collate(x->artist, y->artist);
}

static int cmp_artist(const void *a, const void *b) { return app_collate(((const Artist *)a)->name, ((const Artist *)b)->name); }

static int cmp_track(const void *a, const void *b) {
	const Song *x = &g_sort_lib->songs[*(const int *)a], *y = &g_sort_lib->songs[*(const int *)b];
	if (x->disc != y->disc) return x->disc - y->disc;
	if (x->track != y->track) return (x->track ? x->track : 9999) - (y->track ? y->track : 9999);
	return app_collate(x->title, y->title);
}

static int cmp_album_year(const void *a, const void *b) {
	const Album *x = &g_sort_lib->albums[*(const int *)a], *y = &g_sort_lib->albums[*(const int *)b];
	if (x->year != y->year) return (x->year ? x->year : 9999) - (y->year ? y->year : 9999);
	return app_collate(x->name, y->name);
}

static void m3u_load(Lib *l, const char *path) {
	size_t len = 0;
	char *txt = file_read_all(path, &len);
	if (!txt || len > (1u << 20)) { free(txt); return; }
	char *dir = g_path_get_dirname(path);
	VEC(char *) items = {0};
	char *save = NULL;
	for (char *line = strtok_r(txt, "\r\n", &save); line && items.len < 5000; line = strtok_r(NULL, "\r\n", &save)) {
		line = str_trim(line);
		if (!*line || *line == '#') continue;
		if (str_starts(line, "file://")) line += 7;
		for (char *c = line; *c; c++) if (*c == '\\') *c = '/';
		char *p = line[0] == '/' ? xstrdup(line) : path_join(dir, line);
		char *canon = g_canonicalize_filename(p, NULL);
		free(p);
		vec_push(&items, xstrdup(canon));
		g_free(canon);
	}
	g_free(dir);
	free(txt);
	if (!items.len) { vec_free(&items); return; }
	l->m3u = xrealloc(l->m3u, (size_t)(l->nm3u + 1) * sizeof(M3u));
	char name[256];
	str_copy(name, sizeof name, path_basename(path));
	char *dot = strrchr(name, '.');
	if (dot) *dot = 0;
	l->m3u[l->nm3u++] = (M3u){xstrdup(name), xstrdup(path), items.items, items.len};
}

/* Groups songs into albums and artists and sorts everything (worker thread). */
static void lib_build(Lib *l) {
	g_sort_lib = l;
	qsort(l->songs, (size_t)l->nsongs, sizeof(Song), cmp_song);
	GHashTable *albums = g_hash_table_new(g_str_hash, g_str_equal);
	GHashTable *artists = g_hash_table_new(g_str_hash, g_str_equal);
	VEC(Album) al = {0};
	VEC(Artist) ar = {0};
	/* album key: album artist when tagged, otherwise album name + folder (compilations) */
	char **keys = xcalloc((size_t)l->nsongs + 1, sizeof(char *));
	for (int i = 0; i < l->nsongs; i++) {
		Song *s = &l->songs[i];
		char *dir = g_path_get_dirname(s->path);
		char *fold = str_fold(s->album ? s->album : "");
		if (s->albumartist) { char *aa = str_fold(s->albumartist); keys[i] = xasprintf("%s\x1f%s", fold, aa); free(aa); }
		else keys[i] = xasprintf("%s\x1f@%s", fold, dir);
		free(fold);
		g_free(dir);
		gpointer v;
		int ai;
		if (g_hash_table_lookup_extended(albums, keys[i], NULL, &v)) ai = GPOINTER_TO_INT(v) - 1;
		else {
			Album a = {.name = xstrdup(s->album ? s->album : ""), .key = xstrdup(keys[i])};
			a.artist = xstrdup(s->albumartist ? s->albumartist : s->artist ? s->artist : "");
			vec_push(&al, a);
			ai = al.len - 1;
			g_hash_table_insert(albums, al.items[ai].key, GINT_TO_POINTER(ai + 1));
		}
		Album *a = &al.items[ai];
		if (!s->albumartist && strcmp(a->artist, s->artist ? s->artist : "")) a->various = true;
		if (!a->year && s->year) a->year = s->year;
		a->songs = xrealloc(a->songs, (size_t)(a->n + 1) * sizeof(int));
		a->songs[a->n++] = i;
		a->dur += s->dur;
		s->alb = ai;
		/* artist */
		const char *an = s->artist ? s->artist : "";
		char *af = str_fold(an);
		int ri;
		if (g_hash_table_lookup_extended(artists, af, NULL, &v)) { ri = GPOINTER_TO_INT(v) - 1; free(af); }
		else {
			Artist r = {.name = xstrdup(an)};
			vec_push(&ar, r);
			ri = ar.len - 1;
			g_hash_table_insert(artists, af, GINT_TO_POINTER(ri + 1));
		}
		Artist *r = &ar.items[ri];
		r->songs = xrealloc(r->songs, (size_t)(r->n + 1) * sizeof(int));
		r->songs[r->n++] = i;
		s->art_i = ri;
	}
	for (int i = 0; i < al.len; i++) if (al.items[i].various) { free(al.items[i].artist); al.items[i].artist = xstrdup(""); }
	/* sort albums and artists, then remap the indices stored in songs */
	int *amap = xmalloc((size_t)(al.len + 1) * sizeof(int)), *rmap = xmalloc((size_t)(ar.len + 1) * sizeof(int));
	for (int i = 0; i < al.len; i++) al.items[i].orig = i;
	qsort(al.items, (size_t)al.len, sizeof(Album), cmp_album);
	for (int i = 0; i < al.len; i++) amap[al.items[i].orig] = i;
	for (int i = 0; i < ar.len; i++) ar.items[i].orig = i;
	qsort(ar.items, (size_t)ar.len, sizeof(Artist), cmp_artist);
	for (int i = 0; i < ar.len; i++) rmap[ar.items[i].orig] = i;
	for (int i = 0; i < l->nsongs; i++) { l->songs[i].alb = amap[l->songs[i].alb]; l->songs[i].art_i = rmap[l->songs[i].art_i]; }
	l->albums = al.items; l->nalbums = al.len;
	l->artists = ar.items; l->nartists = ar.len;
	for (int i = 0; i < l->nalbums; i++) {
		Album *a = &l->albums[i];
		qsort(a->songs, (size_t)a->n, sizeof(int), cmp_track);
		app_group_letter(a->name, a->letter);
	}
	for (int i = 0; i < l->nartists; i++) {
		Artist *r = &l->artists[i];
		for (int k = 0; k < r->n; k++) {
			int alb = l->songs[r->songs[k]].alb;
			bool have = false;
			for (int j = 0; j < r->nalb; j++) if (r->albums[j] == alb) { have = true; break; }
			if (!have) { r->albums = xrealloc(r->albums, (size_t)(r->nalb + 1) * sizeof(int)); r->albums[r->nalb++] = alb; }
		}
		qsort(r->albums, (size_t)r->nalb, sizeof(int), cmp_album_year);
		app_group_letter(r->name, r->letter);
	}
	l->song_letters = xcalloc((size_t)l->nsongs + 1, 8);
	for (int i = 0; i < l->nsongs; i++) { app_group_letter(l->songs[i].title, l->songs[i].letter); memcpy(l->song_letters[i], l->songs[i].letter, 8); }
	l->artist_letters = xcalloc((size_t)l->nartists + 1, 8);
	for (int i = 0; i < l->nartists; i++) memcpy(l->artist_letters[i], l->artists[i].letter, 8);
	for (int i = 0; i < l->nsongs; i++) free(keys[i]);
	free(keys);
	free(amap);
	free(rmap);
	g_hash_table_destroy(albums);
	g_hash_table_destroy(artists);
}

static void scan_run(void *arg) {
	ScanJob *j = arg;
	Lib *l = xcalloc(1, sizeof *l);
	l->lang = j->lang;
	GHashTable *cache = g_hash_table_new(g_str_hash, g_str_equal);
	cJSON *root = NULL;
	char *txt = file_read_all(j->cache, NULL);
	if (txt) { root = cJSON_Parse(txt); free(txt); }
	cJSON *arr = root ? cJSON_GetObjectItem(root, "songs") : NULL;
	cJSON *it;
	cJSON_ArrayForEach(it, arr) {
		cJSON *p = cJSON_GetObjectItem(it, "p");
		if (cJSON_IsString(p)) g_hash_table_insert(cache, p->valuestring, it);
	}
	VEC(Song) songs = {0};
	if (j->cache_only) {
		cJSON_ArrayForEach(it, arr) {
			Song s = song_from_json(it);
			if (!s.path || !s.title) { free(s.path); free(s.title); free(s.artist); free(s.album); free(s.albumartist); free(s.art); free(s.cover); continue; }
			vec_push(&songs, s);
			if (songs.len >= MAX_FILES) break;
		}
		cJSON *m3 = root ? cJSON_GetObjectItem(root, "m3u") : NULL;
		cJSON_ArrayForEach(it, m3) if (cJSON_IsString(it)) m3u_load(l, it->valuestring);
	} else {
		FileVec files = {0};
		StrVec m3u = {0};
		walk(j->root, &files, &m3u, &l->truncated);
		int reused = 0;
		for (int i = 0; i < files.len; i++) {
			FileRef *fr = &files.items[i];
			cJSON *c = g_hash_table_lookup(cache, fr->path);
			if (c && (int64_t)jnum(c, "m") == fr->mtime && (int64_t)jnum(c, "s") == fr->size) {
				Song s = song_from_json(c);
				bool had_cover = s.cover != NULL;
				if (!str_eq(s.cover ? s.cover : "", fr->cover ? fr->cover : "")) j->changed = true;
				free(s.cover);
				s.cover = fr->cover ? xstrdup(fr->cover) : NULL;
				if (s.art && fr->cover) { free(s.art); s.art = NULL; }   /* a folder image appeared */
				if (!fr->cover && ((s.art && !file_exists(s.art)) || (!s.art && had_cover))) {
					/* the picture went away: read the embedded one again */
					free(s.path); free(s.title); free(s.artist); free(s.album); free(s.albumartist); free(s.art);
					s = song_scan(fr, j->root, j->artdir);
				}
				if (!s.title) names_from_file(&s, j->root);
				vec_push(&songs, s);
				reused++;
			} else {
				vec_push(&songs, song_scan(fr, j->root, j->artdir));
				j->changed = true;
			}
		}
		if (reused != cJSON_GetArraySize(arr)) j->changed = true;
		/* m3u list changes */
		cJSON *m3 = root ? cJSON_GetObjectItem(root, "m3u") : NULL;
		if (cJSON_GetArraySize(m3) != m3u.len) j->changed = true;
		for (int i = 0; i < m3u.len; i++) { m3u_load(l, m3u.items[i]); }
		/* write the cache */
		cJSON *out = cJSON_CreateObject();
		cJSON_AddNumberToObject(out, "v", 1);
		cJSON *sa = cJSON_AddArrayToObject(out, "songs");
		for (int i = 0; i < songs.len; i++) cJSON_AddItemToArray(sa, song_to_json(&songs.items[i]));
		cJSON *ma = cJSON_AddArrayToObject(out, "m3u");
		for (int i = 0; i < m3u.len; i++) { cJSON_AddItemToArray(ma, cJSON_CreateString(m3u.items[i])); free(m3u.items[i]); }
		if (j->changed || !root) {
			char *s = cJSON_PrintUnformatted(out);
			if (s) { file_write_all(j->cache, s, strlen(s)); free(s); }
		}
		cJSON_Delete(out);
		for (int i = 0; i < files.len; i++) { free(files.items[i].path); free(files.items[i].cover); }
		vec_free(&files);
		vec_free(&m3u);
	}
	g_hash_table_destroy(cache);
	cJSON_Delete(root);
	l->songs = songs.items;
	l->nsongs = songs.len;
	lib_build(l);
	l->by_path = g_hash_table_new(g_str_hash, g_str_equal);
	for (int i = 0; i < l->nsongs; i++) g_hash_table_insert(l->by_path, l->songs[i].path, GINT_TO_POINTER(i + 1));
	j->out = l;
}

/* ============================================================ app state === */

typedef struct { char *path, *title, *artist, *album, *art; double dur; } QItem;
typedef struct { char *name; VEC(char *) songs; } Playlist;

enum { REP_OFF, REP_ALL, REP_ONE };
#define QUEUE_MAX 5000

static struct {
	QItem *it;
	int n;
	int *order;                 /* play order -> item index */
	int pos;                    /* index into order */
	bool shuffle;
	int repeat;
} Q;

static Lib *g_lib;
static VEC(Playlist) g_pl;
static bool g_scanning, g_state_loaded, g_rescan_after;
static double g_last_scan = -1000;
static ImgCache *g_thumbs, *g_big;
static Timer *g_save_timer;
static bool g_save_dirty, g_atexit_set;
static double g_resume_pos = -1;   /* saved position for the restored track */

static void save_state_now(void);

static QItem *q_cur(void) { return Q.n && Q.pos >= 0 && Q.pos < Q.n ? &Q.it[Q.order[Q.pos]] : NULL; }

static void qitem_free(QItem *q) { free(q->path); free(q->title); free(q->artist); free(q->album); free(q->art); }

static int lib_find(const char *path) {
	if (!g_lib || !path) return -1;
	gpointer v = g_hash_table_lookup(g_lib->by_path, path);
	return v ? GPOINTER_TO_INT(v) - 1 : -1;
}

static void qitem_fill(QItem *q) {
	int i = lib_find(q->path);
	if (i < 0) return;
	const Song *s = &g_lib->songs[i];
	free(q->title); free(q->artist); free(q->album); free(q->art);
	q->title = xstrdup(s->title);
	q->artist = s->artist ? xstrdup(s->artist) : NULL;
	q->album = s->album ? xstrdup(s->album) : NULL;
	q->art = song_art(s) ? xstrdup(song_art(s)) : NULL;
	q->dur = s->dur;
}

/* Builds a queue item for a path; files outside the library get their tags read here. */
static QItem qitem_for(const char *path) {
	QItem q = {.path = xstrdup(path)};
	if (lib_find(path) >= 0) { qitem_fill(&q); return q; }
	struct stat st;
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < (2LL << 30)) {
		Tags t = {0};
		tags_read(path, (long)st.st_size, &t);
		q.title = t.title; q.artist = t.artist; q.album = t.album; q.dur = t.dur;
		t.title = t.artist = t.album = NULL;
		tags_free(&t);
		char *dir = g_path_get_dirname(path);
		DIR *dh = opendir(dir);
		if (dh) {
			struct dirent *e;
			int best = 1000;
			while ((e = readdir(dh))) {
				int r = cover_rank(e->d_name);
				if (r >= 0 && r < best) { best = r; free(q.art); q.art = path_join(dir, e->d_name); }
			}
			closedir(dh);
		}
		g_free(dir);
	}
	if (!q.title) {
		char name[256];
		str_copy(name, sizeof name, path_basename(path));
		char *dot = strrchr(name, '.');
		if (dot && dot != name) *dot = 0;
		q.title = xstrdup(name);
	}
	return q;
}

static void q_clear(void) {
	for (int i = 0; i < Q.n; i++) qitem_free(&Q.it[i]);
	free(Q.it);
	free(Q.order);
	Q.it = NULL;
	Q.order = NULL;
	Q.n = Q.pos = 0;
}

static void shuffle_ints(int *a, int n) {
	for (int i = n - 1; i > 0; i--) {
		int j = g_random_int_range(0, i + 1), t = a[i];
		a[i] = a[j];
		a[j] = t;
	}
}

/* Shuffles the play order keeping `first` (item index) in front. */
static void q_shuffle_order(int first) {
	for (int i = 0; i < Q.n; i++) Q.order[i] = i;
	shuffle_ints(Q.order, Q.n);
	if (first >= 0)
		for (int i = 0; i < Q.n; i++) if (Q.order[i] == first) { Q.order[i] = Q.order[0]; Q.order[0] = first; break; }
	Q.pos = 0;
}

static void q_set_paths(char **paths, int n, int start, bool shuffle) {
	q_clear();
	n = MIN(n, QUEUE_MAX);
	if (n <= 0) return;
	Q.it = xcalloc((size_t)n, sizeof(QItem));
	Q.order = xmalloc((size_t)n * sizeof(int));
	for (int i = 0; i < n; i++) { Q.it[i] = qitem_for(paths[i]); Q.order[i] = i; }
	Q.n = n;
	Q.shuffle = shuffle;
	if (shuffle) q_shuffle_order(start >= 0 && start < n ? start : g_random_int_range(0, n));
	else Q.pos = CLAMP(start, 0, n - 1);
}

static void q_append(const char *path) {
	if (Q.n >= QUEUE_MAX) return;
	Q.it = xrealloc(Q.it, (size_t)(Q.n + 1) * sizeof(QItem));
	Q.order = xrealloc(Q.order, (size_t)(Q.n + 1) * sizeof(int));
	Q.it[Q.n] = qitem_for(path);
	Q.order[Q.n] = Q.n;
	Q.n++;
}

/* Removes the entry at play position p. */
static void q_remove_at(int p) {
	if (p < 0 || p >= Q.n) return;
	int item = Q.order[p];
	qitem_free(&Q.it[item]);
	memmove(&Q.it[item], &Q.it[item + 1], (size_t)(Q.n - item - 1) * sizeof(QItem));
	memmove(&Q.order[p], &Q.order[p + 1], (size_t)(Q.n - p - 1) * sizeof(int));
	Q.n--;
	for (int i = 0; i < Q.n; i++) if (Q.order[i] > item) Q.order[i]--;
	if (p < Q.pos) Q.pos--;
	if (Q.pos >= Q.n) Q.pos = MAX(0, Q.n - 1);
}

static void save_soon(void);

static bool music_owns(void) {
	QItem *c = q_cur();
	return c && str_eq(player_owner(), "music") && player_path() && str_eq(player_path(), c->path);
}

static void music_next(void);
static void music_prev(void);
static void on_finished(void *ud);

static void claim(QItem *c) {
	player_set_owner("music");
	player_set_meta(c->title, c->artist);
	player_set_duration(c->dur);
	player_on_finished(on_finished, NULL);
	player_set_queue_hooks(music_next, music_prev);
}

static bool check_backend(void) {
	if (player_available()) return true;
	shell_toast(_("No audio player found. Install mpv."), NULL, tr("Music"));
	return false;
}

/* Starts (or cues, when !play) the entry at play position p. */
static void play_pos(int p, bool play, double at) {
	if (!Q.n) return;
	Q.pos = CLAMP(p, 0, Q.n - 1);
	QItem *c = q_cur();
	g_resume_pos = play ? -1 : at;
	if (play && check_backend()) {
		if (!player_can_play(c->path)) {
			shell_toast(_("Can't play this file"), c->title, tr("Music"));
			player_cue(c->path, 0);
		} else if (player_play(c->path)) {
			if (at > 1) player_seek(at);
		}
	} else {
		player_cue(c->path, at);
	}
	claim(c);
	save_soon();
	shell_invalidate();
}

static void on_finished(void *ud) {
	if (!Q.n) return;
	if (Q.repeat == REP_ONE) { play_pos(Q.pos, true, 0); return; }
	if (Q.pos + 1 < Q.n) { play_pos(Q.pos + 1, true, 0); return; }
	if (Q.repeat == REP_ALL) {
		if (Q.shuffle) q_shuffle_order(-1);
		play_pos(0, true, 0);
		return;
	}
	play_pos(0, false, 0);   /* end of the queue: back to the first song, paused */
}

static void music_next(void) {
	if (!Q.n) return;
	bool playing = music_owns() && player_state() == PL_PLAYING;
	play_pos((Q.pos + 1) % Q.n, playing, 0);
}

static void music_prev(void) {
	if (!Q.n) return;
	bool owns = music_owns(), playing = owns && player_state() == PL_PLAYING;
	if (owns && player_position() > 3) { player_seek(0); return; }
	play_pos((Q.pos - 1 + Q.n) % Q.n, playing, 0);
}

static void music_toggle(void) {
	QItem *c = q_cur();
	if (!c) return;
	if (music_owns() && player_state() != PL_STOPPED) {
		if (player_state() == PL_PAUSED && !check_backend()) return;
		player_toggle();
		save_soon();
	} else {
		play_pos(Q.pos, true, 0);
	}
}

static void set_shuffle(bool on) {
	if (on == Q.shuffle || !Q.n) { Q.shuffle = on; return; }
	int cur = Q.order[Q.pos];
	Q.shuffle = on;
	if (on) q_shuffle_order(cur);
	else { for (int i = 0; i < Q.n; i++) Q.order[i] = i; Q.pos = cur; }
	save_soon();
}

static void play_paths(char **paths, int n, int start, bool shuffle) {
	if (n <= 0) return;
	q_set_paths(paths, n, start, shuffle);
	play_pos(Q.pos, true, 0);
}

/* ------------------------------------------------------------ playlists */

static Playlist *pl_new(const char *name) {
	Playlist p = {.name = xstrdup(name)};
	vec_push(&g_pl, p);
	return &g_pl.items[g_pl.len - 1];
}

static void pl_add(Playlist *p, const char *path) {
	if (p->songs.len < QUEUE_MAX) vec_push(&p->songs, xstrdup(path));
}

static void pl_delete(int i) {
	if (i < 0 || i >= g_pl.len) return;
	Playlist *p = &g_pl.items[i];
	for (int k = 0; k < p->songs.len; k++) free(p->songs.items[k]);
	vec_free(&p->songs);
	free(p->name);
	vec_remove(&g_pl, i);
}

/* ---------------------------------------------------------- persistence */

static void save_state_now(void) {
	if (!g_state_loaded) return;
	g_save_dirty = false;
	cJSON *root = cJSON_CreateObject();
	cJSON *pa = cJSON_AddArrayToObject(root, "playlists");
	for (int i = 0; i < g_pl.len; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", g_pl.items[i].name);
		cJSON *s = cJSON_AddArrayToObject(o, "songs");
		for (int k = 0; k < g_pl.items[i].songs.len; k++) cJSON_AddItemToArray(s, cJSON_CreateString(g_pl.items[i].songs.items[k]));
		cJSON_AddItemToArray(pa, o);
	}
	cJSON *qa = cJSON_AddArrayToObject(root, "queue");
	cJSON *oa = cJSON_AddArrayToObject(root, "order");
	for (int i = 0; i < Q.n; i++) {
		cJSON_AddItemToArray(qa, cJSON_CreateString(Q.it[i].path));
		cJSON_AddItemToArray(oa, cJSON_CreateNumber(Q.order[i]));
	}
	cJSON_AddNumberToObject(root, "pos", Q.pos);
	double t = music_owns() ? player_position() : g_resume_pos > 0 ? g_resume_pos : 0;
	cJSON_AddNumberToObject(root, "time", round(t));
	cJSON_AddBoolToObject(root, "shuffle", Q.shuffle);
	cJSON_AddNumberToObject(root, "repeat", Q.repeat);
	app_json_save("music.json", root);
	cJSON_Delete(root);
}

static void save_timer_cb(void *ud) { if (g_save_dirty) save_state_now(); }

static void at_exit_save(void) { if (g_save_dirty || music_owns()) save_state_now(); }

static void save_soon(void) {
	g_save_dirty = true;
	if (!g_save_timer) g_save_timer = timer_add(1500, false, save_timer_cb, NULL);
	else timer_set(g_save_timer, 1500);
	if (!g_atexit_set) { atexit(at_exit_save); g_atexit_set = true; }
}

static void load_state(void) {
	if (g_state_loaded) return;
	g_state_loaded = true;
	cJSON *root = app_json_load("music.json");
	if (!root) return;
	cJSON *it;
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "playlists")) {
		char *name = jstr(it, "name");
		if (!name) continue;
		Playlist *p = pl_new(name);
		free(name);
		cJSON *s;
		cJSON_ArrayForEach(s, cJSON_GetObjectItem(it, "songs")) if (cJSON_IsString(s)) pl_add(p, s->valuestring);
	}
	cJSON *qa = cJSON_GetObjectItem(root, "queue"), *oa = cJSON_GetObjectItem(root, "order");
	int n = MIN(cJSON_GetArraySize(qa), QUEUE_MAX);
	if (n > 0 && !player_active()) {
		Q.it = xcalloc((size_t)n, sizeof(QItem));
		Q.order = xmalloc((size_t)n * sizeof(int));
		int k = 0;
		cJSON_ArrayForEach(it, qa) {
			if (k >= n) break;
			Q.it[k] = (QItem){.path = xstrdup(cJSON_IsString(it) ? it->valuestring : "")};
			char name[256];
			str_copy(name, sizeof name, path_basename(Q.it[k].path));
			char *dot = strrchr(name, '.');
			if (dot && dot != name) *dot = 0;
			Q.it[k].title = xstrdup(name);
			Q.order[k] = k;
			k++;
		}
		Q.n = k;
		/* the saved order must be a permutation, otherwise fall back to the natural order */
		bool ok = cJSON_GetArraySize(oa) == Q.n;
		bool *seen = xcalloc((size_t)Q.n + 1, 1);
		for (int i = 0; ok && i < Q.n; i++) {
			int v = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(oa, i));
			if (v < 0 || v >= Q.n || seen[v]) ok = false;
			else { seen[v] = true; Q.order[i] = v; }
		}
		free(seen);
		if (!ok) for (int i = 0; i < Q.n; i++) Q.order[i] = i;
		Q.pos = CLAMP((int)jnum(root, "pos"), 0, Q.n - 1);
		Q.shuffle = cJSON_IsTrue(cJSON_GetObjectItem(root, "shuffle"));
		Q.repeat = CLAMP((int)jnum(root, "repeat"), 0, 2);
		g_resume_pos = jnum(root, "time");
		/* resume paused where we left off, without starting a player process */
		QItem *c = q_cur();
		if (c && file_exists(c->path)) {
			player_cue(c->path, g_resume_pos);
			claim(c);
		}
	}
	cJSON_Delete(root);
}

/* ------------------------------------------------------------- scanning */

static void nav_remap(Lib *old, Lib *nw);
static bool g_open;

static void scan_done(void *arg) {
	ScanJob *j = arg;
	g_scanning = false;
	bool full = !j->cache_only;
	if (!g_open) { lib_free(j->out); j->out = NULL; full = true; g_rescan_after = false; }
	if (j->out && (!g_lib || j->changed || j->cache_only || j->out->lang != g_lib->lang)) {
		Lib *old = g_lib;
		g_lib = j->out;
		j->out = NULL;
		nav_remap(old, g_lib);
		lib_free(old);
		for (int i = 0; i < Q.n; i++) qitem_fill(&Q.it[i]);
		QItem *c = q_cur();
		if (c && music_owns()) { player_set_meta(c->title, c->artist); player_set_duration(c->dur); }
	}
	lib_free(j->out);
	if (full) g_last_scan = now_sec();
	free(j->root); free(j->cache); free(j->artdir);
	free(j);
	shell_invalidate();
	if (!full || g_rescan_after) {
		g_rescan_after = false;
		ScanJob *n = xcalloc(1, sizeof *n);
		n->root = music_dir();
		n->cache = cache_file();
		char *d = xdg_dir("XDG_CACHE_HOME", ".cache", "metrowl/art");
		n->artdir = d;
		n->lang = (int)i18n_lang();
		g_scanning = true;
		job_submit(scan_run, scan_done, n);
	}
}

static void scan_start(bool cache_only) {
	if (g_scanning) { if (!cache_only) g_rescan_after = true; return; }
	ScanJob *j = xcalloc(1, sizeof *j);
	j->root = music_dir();
	j->cache = cache_file();
	j->artdir = xdg_dir("XDG_CACHE_HOME", ".cache", "metrowl/art");
	j->cache_only = cache_only;
	j->lang = (int)i18n_lang();
	g_scanning = true;
	job_submit(scan_run, scan_done, j);
}

/* ================================================================== UI === */

enum { PG_ROOT, PG_ARTIST, PG_ALBUM, PG_PLAYLIST, PG_M3U, PG_NOW, PG_QUEUE, PG_SEARCH };
#define PAGE(k, c) ((k) | ((c) << 4))
#define PKIND(p) ((p) & 15)
#define PCTX(p) ((p) >> 4)

static Nav g_nav;
static Pager g_pivot;
static AppBar g_bar;
static JumpList g_jl_artists, g_jl_songs;
static Scroll g_sc_albums, g_sc_pls, g_sc_artist, g_sc_album, g_sc_pl, g_sc_queue, g_sc_search, g_sc_now;
static bool g_nav_ready;
static bool g_seeking;
static double g_seek_frac;
static char g_query[128];
static VEC(int) g_sr_artists, g_sr_albums, g_sr_songs;

static void push(int kind, int ctx) {
	switch (kind) {
	case PG_ARTIST: g_sc_artist = (Scroll){0}; break;
	case PG_ALBUM: g_sc_album = (Scroll){0}; break;
	case PG_PLAYLIST: case PG_M3U: g_sc_pl = (Scroll){0}; break;
	case PG_QUEUE: g_sc_queue = (Scroll){0}; break;
	default: break;
	}
	g_bar.open = false;
	/* jumping to a page that is already on the stack pops back to it */
	for (int i = 0; i <= g_nav.depth; i++) {
		if (g_nav.stack[i] == PAGE(kind, ctx) && i < g_nav.depth) {
			g_nav.depth = i + 1;
			nav_pop(&g_nav, &sh.ui);
			return;
		}
	}
	nav_push(&g_nav, &sh.ui, PAGE(kind, ctx));
}

/* After a rescan, indices change: find the same artist / album again, or drop the page. */
static void nav_remap(Lib *old, Lib *nw) {
	if (!old) return;
	for (int i = 1; i <= g_nav.depth; i++) {
		int p = g_nav.stack[i], k = PKIND(p), c = PCTX(p), found = -1;
		if (k == PG_ARTIST && c < old->nartists) {
			for (int j = 0; j < nw->nartists && found < 0; j++) if (str_eq(nw->artists[j].name, old->artists[c].name)) found = j;
		} else if (k == PG_ALBUM && c < old->nalbums) {
			for (int j = 0; j < nw->nalbums && found < 0; j++) if (str_eq(nw->albums[j].key, old->albums[c].key)) found = j;
		} else if (k == PG_M3U && c < old->nm3u) {
			for (int j = 0; j < nw->nm3u && found < 0; j++) if (str_eq(nw->m3u[j].path, old->m3u[c].path)) found = j;
		} else continue;
		if (found < 0) { g_nav.depth = i - 1; break; }
		g_nav.stack[i] = PAGE(k, found);
	}
	g_nav.from = g_nav.to = g_nav.stack[g_nav.depth];
	g_sr_artists.len = g_sr_albums.len = g_sr_songs.len = 0;
	g_query[0] = 0;
}

/* ------------------------------------------------------------- helpers */

static const char *fmt_dur(double s, char *b, size_t n) {
	if (s < 0 || !isfinite(s)) s = 0;
	long t = lround(s);
	if (t >= 3600) snprintf(b, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
	else snprintf(b, n, "%ld:%02ld", t / 60, t % 60);
	return b;
}

static const char *count_str(char *b, size_t n, int c, const char *one, const char *many) {
	snprintf(b, n, c == 1 ? tr(one) : tr(many), c);
	return b;
}

static const char *artist_name(const char *a) { return a && *a ? a : _("unknown artist"); }
static const char *album_name(const char *a) { return a && *a ? a : _("unknown album"); }

static int thumb_px(UI *ui) { return (int)MIN(400, 180 * ui->scale); }

static Image *art_image(UI *ui, const char *path, bool big) {
	if (!path) return NULL;
	if (!g_thumbs) g_thumbs = imgcache_new(64);
	if (big) {
		if (!g_big) g_big = imgcache_new(3);
		Image *img = imgcache_get(g_big, path, (int)MIN(1024, 480 * ui->scale));
		if (img) return img;
	}
	return imgcache_get(g_thumbs, path, thumb_px(ui));
}

static void art_draw(UI *ui, const char *path, Rect r, bool big) {
	Image *img = art_image(ui, path, big);
	if (img) { gfx_image_cover(ui->cr, img, r, 1.0); return; }
	gfx_fill_rect(ui->cr, r, ui->th.chrome2);
	icon_draw(ui->cr, ICON_MUSIC, rect_cx(r), rect_cy(r), MIN(r.w, r.h) * 0.42, ui->th.dim);
}

static const char *album_art(const Album *a) {
	for (int i = 0; i < a->n; i++) { const char *p = song_art(&g_lib->songs[a->songs[i]]); if (p) return p; }
	return NULL;
}

static bool is_current(const char *path) {
	QItem *c = q_cur();
	return c && path && str_eq(c->path, path);
}

/* paths of a set of song indices (borrowed pointers) */
static char **song_paths(const int *idx, int n) {
	char **p = xmalloc((size_t)(n + 1) * sizeof(char *));
	for (int i = 0; i < n; i++) p[i] = g_lib->songs[idx[i]].path;
	return p;
}

static void play_songs(const int *idx, int n, int start, bool shuffle) {
	if (!n) return;
	char **p = song_paths(idx, n);
	play_paths(p, n, start, shuffle);
	free(p);
}

static void play_all(bool shuffle) {
	if (!g_lib || !g_lib->nsongs) return;
	int n = MIN(g_lib->nsongs, QUEUE_MAX);
	int *idx = xmalloc((size_t)n * sizeof(int));
	if (shuffle && g_lib->nsongs > QUEUE_MAX) {
		int *all = xmalloc((size_t)g_lib->nsongs * sizeof(int));
		for (int i = 0; i < g_lib->nsongs; i++) all[i] = i;
		shuffle_ints(all, g_lib->nsongs);
		memcpy(idx, all, (size_t)n * sizeof(int));
		free(all);
	} else {
		for (int i = 0; i < n; i++) idx[i] = i;
	}
	play_songs(idx, n, shuffle ? -1 : 0, shuffle);
	free(idx);
}

static void enqueue(char **paths, int n) {
	bool empty = Q.n == 0;
	for (int i = 0; i < n; i++) q_append(paths[i]);
	if (empty && Q.n) play_pos(0, false, 0);
	save_soon();
	char b[64];
	shell_toast(_("Added to now playing"), count_str(b, sizeof b, n, "%d song", "%d songs"), tr("Music"));
}

static void list_highlight(UI *ui, UiId id, Rect r) {
	if (ui_pressed(ui, id)) gfx_fill_rect(ui->cr, r, col_alpha(ui->th.fg, 0.12f));
}

static Rect screen_point_rect(UI *ui) { return R(ui->p.x, ui->p.y, 0, 0); }

/* ---------------------------------------------------- context menus */

enum { T_SONG, T_ALBUM, T_ARTIST, T_PLAYLIST, T_M3U, T_PLENTRY, T_QUEUE };
enum { A_PLAY, A_ENQUEUE, A_ADD_PL, A_GO_ALBUM, A_GO_ARTIST, A_PL_REMOVE, A_PL_UP, A_PL_DOWN, A_PL_RENAME, A_PL_DELETE, A_Q_REMOVE, A_SHUFFLE };

static struct { int kind, idx, ctx; int act[10]; const char *label[10]; int n; } M;
static VEC(char *) g_pending;   /* paths waiting for "add to playlist" */

static void pending_clear(void) {
	for (int i = 0; i < g_pending.len; i++) free(g_pending.items[i]);
	g_pending.len = 0;
}

static void pending_set(char **paths, int n) {
	pending_clear();
	for (int i = 0; i < n; i++) vec_push(&g_pending, xstrdup(paths[i]));
}

/* paths for a menu target (borrowed; free the array only) */
static char **target_paths(int kind, int idx, int ctx, int *n) {
	*n = 0;
	switch (kind) {
	case T_SONG: if (!g_lib || idx >= g_lib->nsongs) return NULL; *n = 1; return song_paths(&idx, 1);
	case T_ALBUM: if (!g_lib || idx >= g_lib->nalbums) return NULL; *n = g_lib->albums[idx].n; return song_paths(g_lib->albums[idx].songs, *n);
	case T_ARTIST: if (!g_lib || idx >= g_lib->nartists) return NULL; *n = g_lib->artists[idx].n; return song_paths(g_lib->artists[idx].songs, *n);
	case T_PLAYLIST: {
		if (idx >= g_pl.len) return NULL;
		*n = g_pl.items[idx].songs.len;
		char **p = xmalloc((size_t)(*n + 1) * sizeof(char *));
		memcpy(p, g_pl.items[idx].songs.items, (size_t)*n * sizeof(char *));
		return p;
	}
	case T_M3U: {
		if (!g_lib || idx >= g_lib->nm3u) return NULL;
		*n = g_lib->m3u[idx].n;
		char **p = xmalloc((size_t)(*n + 1) * sizeof(char *));
		memcpy(p, g_lib->m3u[idx].items, (size_t)*n * sizeof(char *));
		return p;
	}
	case T_PLENTRY:
		if (ctx >= g_pl.len || idx >= g_pl.items[ctx].songs.len) return NULL;
		*n = 1;
		{ char **p = xmalloc(2 * sizeof(char *)); p[0] = g_pl.items[ctx].songs.items[idx]; return p; }
	case T_QUEUE:
		if (idx >= Q.n) return NULL;
		*n = 1;
		{ char **p = xmalloc(2 * sizeof(char *)); p[0] = Q.it[Q.order[idx]].path; return p; }
	}
	return NULL;
}

static void newpl_cb(int b, void *ud) {
	if (b != 0) { pending_clear(); return; }
	char name[128];
	str_copy(name, sizeof name, dialog_input_text());
	char *t = str_trim(name);
	if (!*t) { pending_clear(); return; }
	Playlist *p = pl_new(t);
	for (int i = 0; i < g_pending.len; i++) pl_add(p, g_pending.items[i]);
	if (g_pending.len) shell_toast(_("Added to playlist"), p->name, tr("Music"));
	pending_clear();
	save_soon();
}

static void addpl_cb(int b, void *ud) {
	if (b < 0) { pending_clear(); return; }
	if (b == 0) { dialog_input(_("new playlist"), _("playlist name"), "", 0, newpl_cb, NULL); return; }
	if (b - 1 < g_pl.len) {
		Playlist *p = &g_pl.items[b - 1];
		for (int i = 0; i < g_pending.len; i++) pl_add(p, g_pending.items[i]);
		shell_toast(_("Added to playlist"), p->name, tr("Music"));
		save_soon();
	}
	pending_clear();
}

static void ask_playlist(char **paths, int n) {
	pending_set(paths, n);
	const char *items[24];
	int k = 0;
	items[k++] = _("new playlist");
	for (int i = 0; i < g_pl.len && k < 24; i++) items[k++] = g_pl.items[i].name;
	dialog_list(_("add to playlist"), items, k, -1, addpl_cb, NULL);
}

static int g_rename_idx = -1;

static void rename_cb(int b, void *ud) {
	if (b != 0 || g_rename_idx < 0 || g_rename_idx >= g_pl.len) return;
	char name[128];
	str_copy(name, sizeof name, dialog_input_text());
	char *t = str_trim(name);
	if (!*t) return;
	free(g_pl.items[g_rename_idx].name);
	g_pl.items[g_rename_idx].name = xstrdup(t);
	save_soon();
}

static int g_delete_idx = -1;

static void delete_cb(int b, void *ud) {
	if (b != 0 || g_delete_idx < 0 || g_delete_idx >= g_pl.len) return;
	/* leave the playlist's page first, then fix the indices of the remaining pages */
	for (int i = 1; i <= g_nav.depth; i++) {
		int p = g_nav.stack[i];
		if (PKIND(p) != PG_PLAYLIST) continue;
		if (PCTX(p) == g_delete_idx) { g_nav.depth = i - 1; g_nav.from = g_nav.to = g_nav.stack[g_nav.depth]; break; }
		if (PCTX(p) > g_delete_idx) g_nav.stack[i] = PAGE(PG_PLAYLIST, PCTX(p) - 1);
	}
	pl_delete(g_delete_idx);
	save_soon();
}

static void do_action(int act, int kind, int idx, int ctx) {
	int n = 0;
	char **paths = target_paths(kind, idx, ctx, &n);
	switch (act) {
	case A_PLAY:
		if (kind == T_QUEUE) play_pos(idx, true, 0);
		else if (n) play_paths(paths, n, 0, false);
		break;
	case A_SHUFFLE: if (n) play_paths(paths, n, -1, true); break;
	case A_ENQUEUE: if (n) enqueue(paths, n); break;
	case A_ADD_PL: if (n) ask_playlist(paths, n); break;
	case A_GO_ALBUM:
		if (kind == T_SONG && g_lib) push(PG_ALBUM, g_lib->songs[idx].alb);
		break;
	case A_GO_ARTIST:
		if (kind == T_SONG && g_lib) push(PG_ARTIST, g_lib->songs[idx].art_i);
		else if (kind == T_ALBUM && g_lib && g_lib->albums[idx].n) push(PG_ARTIST, g_lib->songs[g_lib->albums[idx].songs[0]].art_i);
		break;
	case A_PL_REMOVE:
		if (ctx < g_pl.len && idx < g_pl.items[ctx].songs.len) {
			free(g_pl.items[ctx].songs.items[idx]);
			vec_remove(&g_pl.items[ctx].songs, idx);
			save_soon();
		}
		break;
	case A_PL_UP: case A_PL_DOWN:
		if (ctx < g_pl.len) {
			Playlist *p = &g_pl.items[ctx];
			int j = act == A_PL_UP ? idx - 1 : idx + 1;
			if (j >= 0 && j < p->songs.len && idx < p->songs.len) {
				char *t = p->songs.items[idx];
				p->songs.items[idx] = p->songs.items[j];
				p->songs.items[j] = t;
				save_soon();
			}
		}
		break;
	case A_PL_RENAME:
		if (idx < g_pl.len) { g_rename_idx = idx; dialog_input(_("rename playlist"), _("playlist name"), g_pl.items[idx].name, 0, rename_cb, NULL); }
		break;
	case A_PL_DELETE:
		if (idx < g_pl.len) {
			g_delete_idx = idx;
			char *t = xasprintf(_("\"%s\" will be deleted. The songs stay in your collection."), g_pl.items[idx].name);
			dialog_message(_("delete playlist?"), t, _("delete"), _("cancel"), delete_cb, NULL);
			free(t);
		}
		break;
	case A_Q_REMOVE: {
		bool cur = idx == Q.pos;
		bool playing = music_owns() && player_state() == PL_PLAYING;
		q_remove_at(idx);
		if (!Q.n) { if (music_owns()) player_stop(); }
		else if (cur) play_pos(Q.pos, playing, 0);
		save_soon();
		break;
	}
	}
	free(paths);
	shell_invalidate();
}

static void menu_cb(int b, void *ud) {
	if (b < 0 || b >= M.n) return;
	do_action(M.act[b], M.kind, M.idx, M.ctx);
}

static void menu_add(int act, const char *label) {
	if (M.n < 10) { M.act[M.n] = act; M.label[M.n++] = label; }
}

static void open_menu(UI *ui, int kind, int idx, int ctx) {
	M.kind = kind; M.idx = idx; M.ctx = ctx; M.n = 0;
	switch (kind) {
	case T_SONG:
		menu_add(A_PLAY, _("play"));
		menu_add(A_ENQUEUE, _("add to now playing"));
		menu_add(A_ADD_PL, _("add to playlist"));
		menu_add(A_GO_ALBUM, _("go to album"));
		menu_add(A_GO_ARTIST, _("go to artist"));
		break;
	case T_ALBUM: case T_ARTIST: case T_M3U:
		menu_add(A_PLAY, _("play"));
		menu_add(A_SHUFFLE, _("shuffle"));
		menu_add(A_ENQUEUE, _("add to now playing"));
		menu_add(A_ADD_PL, _("add to playlist"));
		if (kind == T_ALBUM) menu_add(A_GO_ARTIST, _("go to artist"));
		break;
	case T_PLAYLIST:
		menu_add(A_PLAY, _("play"));
		menu_add(A_SHUFFLE, _("shuffle"));
		menu_add(A_ENQUEUE, _("add to now playing"));
		menu_add(A_PL_RENAME, _("rename"));
		menu_add(A_PL_DELETE, _("delete"));
		break;
	case T_PLENTRY:
		menu_add(A_PLAY, _("play"));
		menu_add(A_ENQUEUE, _("add to now playing"));
		menu_add(A_PL_REMOVE, _("remove from playlist"));
		if (idx > 0) menu_add(A_PL_UP, _("move up"));
		if (ctx < g_pl.len && idx + 1 < g_pl.items[ctx].songs.len) menu_add(A_PL_DOWN, _("move down"));
		break;
	case T_QUEUE:
		menu_add(A_PLAY, _("play"));
		menu_add(A_ADD_PL, _("add to playlist"));
		menu_add(A_Q_REMOVE, _("remove from now playing"));
		break;
	}
	Rect p = screen_point_rect(ui);
	dialog_menu(p.x, p.y, M.label, M.n, menu_cb, NULL);
}

/* ------------------------------------------------------------ rows */

/* Song row: title + subtitle, optional track number column and duration. */
static void song_row(UI *ui, UiId id, Rect r, const char *num, const char *title, const char *sub, double dur, bool current) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	ui_hit(ui, id, r);
	list_highlight(ui, id, r);
	double x = r.x + 24, right = rect_right(r) - 24;
	if (num) {
		gfx_text(cr, num, x, r.y + (r.h - gfx_line_height(cr, 22, FW_LIGHT)) / 2, 22, FW_LIGHT, current ? th->accent : th->sub, TA_LEFT, 0, 1);
		x += 40;
	}
	if (dur > 0) {
		char b[24];
		fmt_dur(dur, b, sizeof b);
		Size s = gfx_text_size(cr, b, 17, FW_SEMILIGHT, 0, 1);
		gfx_text(cr, b, right, r.y + (r.h - s.h) / 2, 17, FW_SEMILIGHT, th->sub, TA_RIGHT, 0, 1);
		right -= s.w + 14;
	}
	Color tc = current ? th->accent : th->fg;
	if (sub && *sub) {
		double lh = gfx_line_height(cr, 23, FW_SEMILIGHT), sl = gfx_line_height(cr, 15, FW_REGULAR);
		double y = r.y + (r.h - lh - sl + 2) / 2;
		gfx_text(cr, title, x, y, 23, FW_SEMILIGHT, tc, TA_LEFT, right - x, 1);
		gfx_text(cr, sub, x, y + lh - 2, 15, FW_REGULAR, th->sub, TA_LEFT, right - x, 1);
	} else {
		gfx_text(cr, title, x, r.y + (r.h - gfx_line_height(cr, 23, FW_SEMILIGHT)) / 2, 23, FW_SEMILIGHT, tc, TA_LEFT, right - x, 1);
	}
}

static void lib_status(UI *ui, Rect r) {
	if (!g_lib || (g_scanning && !g_lib->nsongs)) {
		ui_progress_bar(ui, R(r.x + 24, r.y + 10, r.w - 48, 8), -1);
		gfx_text(ui->cr, _("looking for music..."), r.x + 24, r.y + 28, 20, FW_SEMILIGHT, ui->th.sub, TA_LEFT, r.w - 48, 0);
	} else {
		app_empty(ui, R(r.x, r.y, r.w, r.h), _("no music yet. copy songs into the Music folder and they will show up here."));
	}
}

static void artist_row_fn(UI *ui, int i, Rect r, void *ud) {
	Artist *a = &g_lib->artists[i];
	UiId id = ui_idn("mus-ar", i);
	char b1[48], b2[48], sub[100];
	snprintf(sub, sizeof sub, "%s, %s", count_str(b1, sizeof b1, a->nalb, "%d album", "%d albums"),
	         count_str(b2, sizeof b2, a->n, "%d song", "%d songs"));
	song_row(ui, id, r, NULL, artist_name(a->name), sub, 0, false);
	if (ui_clicked(ui, id)) push(PG_ARTIST, i);
	if (ui_longpressed(ui, id)) open_menu(ui, T_ARTIST, i, 0);
}

static void song_row_fn(UI *ui, int i, Rect r, void *ud) {
	Song *s = &g_lib->songs[i];
	UiId id = ui_idn("mus-so", i);
	song_row(ui, id, r, NULL, s->title, artist_name(s->artist), 0, is_current(s->path));
	if (ui_clicked(ui, id)) {
		int n = MIN(g_lib->nsongs, QUEUE_MAX), start = i;
		int *idx = xmalloc((size_t)n * sizeof(int));
		int first = MAX(0, MIN(i - n / 2, g_lib->nsongs - n));
		for (int k = 0; k < n; k++) idx[k] = first + k;
		start = i - first;
		play_songs(idx, n, start, false);
		free(idx);
	}
	if (ui_longpressed(ui, id)) open_menu(ui, T_SONG, i, 0);
}

static void songs_top_fn(UI *ui, Rect r, void *ud) {
	UiId id = ui_id("mus-shuffle-all");
	Rect hr = R(r.x, r.y + 4, r.w, r.h - 8);
	ui_hit(ui, id, hr);
	list_highlight(ui, id, hr);
	icon_draw_circle(ui->cr, ICON_SHUFFLE, r.x + 24 + 18, rect_cy(hr), 36, ui_pressed(ui, id) ? ui->th.accent : ui->th.fg, 2);
	gfx_text(ui->cr, _("shuffle all"), r.x + 24 + 50, rect_cy(hr) - gfx_line_height(ui->cr, 22, FW_SEMILIGHT) / 2, 22, FW_SEMILIGHT,
	         ui->th.fg, TA_LEFT, r.w - 100, 1);
	if (ui_clicked(ui, id)) play_all(true);
}

/* ------------------------------------------------------- pivot pages */

static void albums_grid(UI *ui, Rect view, const int *list, int n, Scroll *sc, const char *key, double top_pad) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	int cols = MAX(2, (int)((view.w - 24) / 210));
	double gap = 12, cw = (view.w - 48 - gap * (cols - 1)) / cols, rowh = cw + 62;
	int rows = (n + cols - 1) / cols;
	double total = top_pad + rows * rowh + 24;
	double off = ui_scroll_begin(ui, ui_id(key), sc, view, total);
	cairo_translate(cr, view.x, view.y);
	for (int k = 0; k < n; k++) {
		int r = k / cols, c = k % cols;
		double y = top_pad + r * rowh;
		if (y + rowh < off || y > off + view.h) continue;
		int ai = list ? list[k] : k;
		Album *a = &g_lib->albums[ai];
		Rect tr_ = R(24 + c * (cw + gap), y, cw, cw);
		UiId id = ui_idn(key, ai);
		ui_hit(ui, id, R(tr_.x, tr_.y, cw, rowh - 8));
		bool pr = ui_pressed(ui, id);
		Rect ar = pr ? rect_inset(tr_, 4) : tr_;
		art_draw(ui, album_art(a), ar, false);
		gfx_text(cr, album_name(a->name), tr_.x, y + cw + 6, 19, FW_SEMILIGHT, th->fg, TA_LEFT, cw, 1);
		gfx_text(cr, a->various ? _("various artists") : artist_name(a->artist), tr_.x, y + cw + 32, 15, FW_REGULAR, th->sub, TA_LEFT, cw, 1);
		if (ui_clicked(ui, id)) push(PG_ALBUM, ai);
		if (ui_longpressed(ui, id)) open_menu(ui, T_ALBUM, ai, 0);
	}
	ui_scroll_end(ui);
}

static void playlists_page(UI *ui, Rect view) {
	cairo_t *cr = ui->cr;
	int nm = g_lib ? g_lib->nm3u : 0, n = g_pl.len + nm;
	const double ROW = 70;
	if (!n) { app_empty(ui, view, _("no playlists yet. tap + to create one, or long-press a song and add it to a playlist.")); return; }
	double off = ui_scroll_begin(ui, ui_id("mus-pls-scroll"), &g_sc_pls, view, n * ROW + 24);
	cairo_translate(cr, view.x, view.y);
	for (int i = 0; i < n; i++) {
		double y = i * ROW;
		if (y + ROW < off || y > off + view.h) continue;
		bool m3u = i >= g_pl.len;
		int k = m3u ? i - g_pl.len : i;
		const char *name = m3u ? g_lib->m3u[k].name : g_pl.items[k].name;
		int cnt = m3u ? g_lib->m3u[k].n : g_pl.items[k].songs.len;
		char b[48], sub[96];
		count_str(b, sizeof b, cnt, "%d song", "%d songs");
		if (m3u) snprintf(sub, sizeof sub, "%s \xc2\xb7 m3u", b);
		else str_copy(sub, sizeof sub, b);
		UiId id = ui_idn(m3u ? "mus-m3u" : "mus-pl", k);
		song_row(ui, id, R(0, y, view.w, ROW), NULL, name, sub, 0, false);
		if (ui_clicked(ui, id)) push(m3u ? PG_M3U : PG_PLAYLIST, k);
		if (ui_longpressed(ui, id)) open_menu(ui, m3u ? T_M3U : T_PLAYLIST, k, 0);
	}
	ui_scroll_end(ui);
}

static void pivot_page(UI *ui, int page, Rect view, void *ud) {
	if (page == 3) { playlists_page(ui, view); return; }
	if (!g_lib || !g_lib->nsongs) { lib_status(ui, view); return; }
	if (page == 0)
		jumplist_draw(ui, &g_jl_artists, "mus-artists", view, (const char (*)[8])g_lib->artist_letters, g_lib->nartists, 68, 0, NULL,
		              artist_row_fn, NULL);
	else if (page == 1)
		albums_grid(ui, view, NULL, g_lib->nalbums, &g_sc_albums, "mus-albums", 4);
	else
		jumplist_draw(ui, &g_jl_songs, "mus-songs", view, (const char (*)[8])g_lib->song_letters, g_lib->nsongs, 66, 56, songs_top_fn,
		              song_row_fn, NULL);
}

/* "now playing" strip under the library pivot */
static void draw_strip(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	QItem *c = q_cur();
	if (!c) return;
	bool owns = music_owns(), playing = owns && player_state() == PL_PLAYING;
	gfx_fill_rect(cr, r, th->chrome);
	UiId id = ui_id("mus-strip");
	ui_hit(ui, id, r);
	if (ui_pressed(ui, id)) gfx_fill_rect(cr, r, col_alpha(th->fg, 0.08f));
	double dur = owns && player_duration() > 0 ? player_duration() : c->dur;
	if (owns && dur > 0) {
		if (playing) player_watch();
		gfx_fill_rect(cr, R(r.x, r.y, r.w * CLAMP(player_position() / dur, 0, 1), 3), th->accent);
	}
	double as = r.h - 18;
	art_draw(ui, c->art, R(r.x + 14, r.y + 10, as, as), false);
	double x = r.x + 14 + as + 14, w = r.w - (x - r.x) - 80;
	gfx_text(cr, c->title, x, r.y + 10, 20, FW_SEMILIGHT, th->fg, TA_LEFT, w, 1);
	gfx_text(cr, artist_name(c->artist), x, r.y + 36, 15, FW_REGULAR, th->sub, TA_LEFT, w, 1);
	if (ui_circle_button(ui, ui_id("mus-strip-pp"), rect_right(r) - 42, rect_cy(r), 44, playing ? ICON_PAUSE : ICON_PLAY, NULL))
		music_toggle();
	else if (ui_clicked(ui, id)) push(PG_NOW, 0);
}

static void draw_root(UI *ui, Rect area) {
	Rect c = appbar_content(&g_bar, area);
	bool strip = q_cur() != NULL;
	const double STRIP = 68;
	Rect pv = c;
	if (strip) pv.h -= STRIP;
	const char *titles[4] = {_("artists"), _("albums"), _("songs"), _("playlists")};
	app_pivot(ui, "mus-pivot", &g_pivot, pv, tr("Music"), titles, 4, pivot_page, NULL);
	if (strip) draw_strip(ui, R(c.x, rect_bottom(pv), c.w, STRIP));
	bool pls = g_pivot.target == 3;
	AppBarBtn btns[3];
	int nb = 0;
	if (pls) btns[nb++] = (AppBarBtn){ICON_ADD, _("new playlist"), false};
	btns[nb++] = (AppBarBtn){ICON_SHUFFLE, _("shuffle all"), !g_lib || !g_lib->nsongs};
	btns[nb++] = (AppBarBtn){ICON_SEARCH, _("search"), !g_lib || !g_lib->nsongs};
	const char *menu[] = {_("refresh library")};
	int r = appbar_draw(ui, &g_bar, area, btns, nb, menu, 1);
	if (r >= 0 && r < nb) {
		Icon ic = btns[r].icon;
		if (ic == ICON_ADD) { pending_clear(); dialog_input(_("new playlist"), _("playlist name"), "", 0, newpl_cb, NULL); }
		else if (ic == ICON_SHUFFLE) play_all(true);
		else if (ic == ICON_SEARCH) { g_query[0] = 0; g_sr_artists.len = g_sr_albums.len = g_sr_songs.len = 0; g_sc_search = (Scroll){0}; push(PG_SEARCH, 0); }
	} else if (r == APPBAR_MENU) {
		scan_start(false);
		shell_toast(_("Refreshing your music"), NULL, tr("Music"));
	}
}

/* ------------------------------------------------------- inner pages */

/* Small app title + a big title that keeps its case (names). Returns y below it. */
static double page_title(UI *ui, Rect area, const char *title) {
	cairo_t *cr = ui->cr;
	double y = area.y + 8;
	y += gfx_text(cr, i18n_upper(tr("Music")), area.x + 18, y, 17, FW_SEMIBOLD, ui->th.fg, TA_LEFT, area.w - 36, 1) - 6;
	y += gfx_text(cr, title, area.x + 14, y, 50, FW_LIGHT, ui->th.fg, TA_LEFT, area.w - 24, 1);
	return y + 6;
}

static double section_label(UI *ui, double x, double y, double w, const char *label) {
	return y + gfx_text(ui->cr, label, x, y, 22, FW_SEMILIGHT, ui->th.sub, TA_LEFT, w, 1) + 4;
}

static void inner_appbar(UI *ui, Rect area, int kind, int idx, bool can_edit_extra) {
	AppBarBtn btns[3] = {{ICON_PLAY, _("play"), false}, {ICON_SHUFFLE, _("shuffle"), false}, {ICON_ADD, _("add to playlist"), false}};
	const char *menu[4];
	int act[4], nm = 0;
	menu[nm] = _("add to now playing"); act[nm++] = A_ENQUEUE;
	if (kind == T_ALBUM) { menu[nm] = _("go to artist"); act[nm++] = A_GO_ARTIST; }
	if (kind == T_PLAYLIST) { menu[nm] = _("rename"); act[nm++] = A_PL_RENAME; menu[nm] = _("delete"); act[nm++] = A_PL_DELETE; }
	int r = appbar_draw(ui, &g_bar, area, btns, kind == T_M3U ? 2 : 3, menu, nm);
	if (r == 0) do_action(A_PLAY, kind, idx, 0);
	else if (r == 1) do_action(A_SHUFFLE, kind, idx, 0);
	else if (r == 2) do_action(A_ADD_PL, kind, idx, 0);
	else if (r >= APPBAR_MENU && r - APPBAR_MENU < nm) do_action(act[r - APPBAR_MENU], kind, idx, 0);
	(void)can_edit_extra;
}

static void draw_artist(UI *ui, Rect area, int ai) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (!g_lib || ai >= g_lib->nartists) return;
	Artist *a = &g_lib->artists[ai];
	Rect c = appbar_content(&g_bar, area);
	double top = page_title(ui, c, artist_name(a->name));
	Rect view = R(c.x, top, c.w, rect_bottom(c) - top);
	const double AROW = 96, SROW = 64;
	double total = 36 + a->nalb * AROW + 20 + 36 + a->n * SROW + 24;
	double off = ui_scroll_begin(ui, ui_id("mus-artist-scroll"), &g_sc_artist, view, total);
	cairo_translate(cr, view.x, view.y);
	double y = section_label(ui, 24, 0, view.w - 48, _("albums"));
	for (int k = 0; k < a->nalb; k++, y += AROW) {
		if (y + AROW < off || y > off + view.h) continue;
		Album *al = &g_lib->albums[a->albums[k]];
		Rect r = R(0, y, view.w, AROW);
		UiId id = ui_idn("mus-ar-alb", a->albums[k]);
		ui_hit(ui, id, r);
		list_highlight(ui, id, r);
		art_draw(ui, album_art(al), R(24, y + 8, AROW - 16, AROW - 16), false);
		double x = 24 + AROW;
		gfx_text(cr, album_name(al->name), x, y + 14, 23, FW_SEMILIGHT, th->fg, TA_LEFT, view.w - x - 24, 1);
		char b[48], sub[96];
		count_str(b, sizeof b, al->n, "%d song", "%d songs");
		if (al->year) snprintf(sub, sizeof sub, "%d \xc2\xb7 %s", al->year, b);
		else str_copy(sub, sizeof sub, b);
		gfx_text(cr, sub, x, y + 46, 15, FW_REGULAR, th->sub, TA_LEFT, view.w - x - 24, 1);
		if (ui_clicked(ui, id)) push(PG_ALBUM, a->albums[k]);
		if (ui_longpressed(ui, id)) open_menu(ui, T_ALBUM, a->albums[k], 0);
	}
	y += 20;
	y = section_label(ui, 24, y, view.w - 48, _("songs"));
	for (int k = 0; k < a->n; k++, y += SROW) {
		if (y + SROW < off || y > off + view.h) continue;
		Song *s = &g_lib->songs[a->songs[k]];
		UiId id = ui_idn("mus-ar-song", a->songs[k]);
		song_row(ui, id, R(0, y, view.w, SROW), NULL, s->title, album_name(s->album), s->dur, is_current(s->path));
		if (ui_clicked(ui, id)) play_songs(a->songs, a->n, k, false);
		if (ui_longpressed(ui, id)) open_menu(ui, T_SONG, a->songs[k], 0);
	}
	ui_scroll_end(ui);
	inner_appbar(ui, area, T_ARTIST, ai, false);
}

static void draw_album(UI *ui, Rect area, int ai) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	if (!g_lib || ai >= g_lib->nalbums) return;
	Album *a = &g_lib->albums[ai];
	Rect c = appbar_content(&g_bar, area);
	double y0 = c.y + 8;
	y0 += gfx_text(cr, i18n_upper(tr("Music")), c.x + 18, y0, 17, FW_SEMIBOLD, th->fg, TA_LEFT, c.w - 36, 1) + 10;
	Rect view = R(c.x, y0, c.w, rect_bottom(c) - y0);
	const double ROW = a->various ? 64 : 58;
	double art = MIN(160, view.w * 0.36);
	double head = art + 24;
	double off = ui_scroll_begin(ui, ui_id("mus-album-scroll"), &g_sc_album, view, head + a->n * ROW + 24);
	cairo_translate(cr, view.x, view.y);
	art_draw(ui, album_art(a), R(24, 0, art, art), true);
	double x = 24 + art + 16, w = view.w - x - 16;
	double ty = gfx_text(cr, album_name(a->name), x, -6, 30, FW_LIGHT, th->fg, TA_LEFT, w, 3);
	UiId aid = ui_id("mus-album-artist");
	const char *an = a->various ? _("various artists") : artist_name(a->artist);
	Size as = gfx_text_size(cr, an, 18, FW_SEMILIGHT, w, 1);
	ui_hit(ui, aid, R(x - 4, ty - 2, as.w + 8, as.h + 4));
	gfx_text(cr, an, x, ty, 18, FW_SEMILIGHT, ui_pressed(ui, aid) ? th->accent : th->fg, TA_LEFT, w, 1);
	if (ui_clicked(ui, aid) && a->n && !a->various) push(PG_ARTIST, g_lib->songs[a->songs[0]].art_i);
	char b[48], info[128], mins[32];
	count_str(b, sizeof b, a->n, "%d song", "%d songs");
	snprintf(mins, sizeof mins, tr("%d min"), (int)lround(a->dur / 60));
	if (a->year) snprintf(info, sizeof info, "%d \xc2\xb7 %s \xc2\xb7 %s", a->year, b, mins);
	else snprintf(info, sizeof info, "%s \xc2\xb7 %s", b, mins);
	gfx_text(cr, info, x, ty + as.h + 4, 15, FW_REGULAR, th->sub, TA_LEFT, w, 2);
	double y = head;
	for (int k = 0; k < a->n; k++, y += ROW) {
		if (y + ROW < off || y > off + view.h) continue;
		Song *s = &g_lib->songs[a->songs[k]];
		char num[12];
		snprintf(num, sizeof num, "%d", s->track ? s->track : k + 1);
		UiId id = ui_idn("mus-al-song", a->songs[k]);
		song_row(ui, id, R(0, y, view.w, ROW), num, s->title, a->various ? artist_name(s->artist) : NULL, s->dur, is_current(s->path));
		if (ui_clicked(ui, id)) play_songs(a->songs, a->n, k, false);
		if (ui_longpressed(ui, id)) open_menu(ui, T_SONG, a->songs[k], 0);
	}
	ui_scroll_end(ui);
	inner_appbar(ui, area, T_ALBUM, ai, false);
}

static void draw_playlist(UI *ui, Rect area, int pi, bool m3u) {
	cairo_t *cr = ui->cr;
	if (m3u ? (!g_lib || pi >= g_lib->nm3u) : pi >= g_pl.len) return;
	const char *name = m3u ? g_lib->m3u[pi].name : g_pl.items[pi].name;
	char **items = m3u ? g_lib->m3u[pi].items : g_pl.items[pi].songs.items;
	int n = m3u ? g_lib->m3u[pi].n : g_pl.items[pi].songs.len;
	Rect c = appbar_content(&g_bar, area);
	double top = page_title(ui, c, name);
	Rect view = R(c.x, top, c.w, rect_bottom(c) - top);
	if (!n) {
		app_empty(ui, view, _("this playlist is empty. long-press a song, album or artist and choose \"add to playlist\"."));
	} else {
		const double ROW = 64;
		double off = ui_scroll_begin(ui, ui_id("mus-pl-scroll"), &g_sc_pl, view, n * ROW + 24);
		cairo_translate(cr, view.x, view.y);
		for (int k = 0; k < n; k++) {
			double y = k * ROW;
			if (y + ROW < off || y > off + view.h) continue;
			int si = lib_find(items[k]);
			Song *s = si >= 0 ? &g_lib->songs[si] : NULL;
			char name2[256];
			if (!s) {
				str_copy(name2, sizeof name2, path_basename(items[k]));
				char *dot = strrchr(name2, '.');
				if (dot && dot != name2) *dot = 0;
			}
			UiId id = ui_idn(m3u ? "mus-m3u-e" : "mus-pl-e", k);
			song_row(ui, id, R(0, y, view.w, ROW), NULL, s ? s->title : name2,
			         s ? artist_name(s->artist) : (file_exists(items[k]) ? "" : _("file not found")), s ? s->dur : 0, is_current(items[k]));
			if (ui_clicked(ui, id)) play_paths(items, n, k, false);
			if (ui_longpressed(ui, id)) {
				if (m3u) { if (s) open_menu(ui, T_SONG, si, 0); }
				else open_menu(ui, T_PLENTRY, k, pi);
			}
		}
		ui_scroll_end(ui);
	}
	inner_appbar(ui, area, m3u ? T_M3U : T_PLAYLIST, pi, true);
}

/* ------------------------------------------------------- now playing */

static void seek_bar(UI *ui, Rect r, double pos, double dur, bool can_seek) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	UiId id = ui_id("mus-seek");
	if (can_seek && dur > 0) ui_hit_drag(ui, id, R(r.x - 10, r.y - 16, r.w + 20, 40));
	double frac = dur > 0 ? CLAMP(pos / dur, 0, 1) : 0;
	if (can_seek && dur > 0 && (ui_dragging(ui, id) || ui_pressed(ui, id))) {
		double lx, ly;
		ui_pointer_local(ui, &lx, &ly);
		g_seeking = true;
		g_seek_frac = CLAMP((lx - r.x) / r.w, 0, 1);
	}
	if (g_seeking && !ui_dragging(ui, id) && !ui_pressed(ui, id)) {
		g_seeking = false;
		player_seek(g_seek_frac * dur);
		save_soon();
		frac = g_seek_frac;
	}
	if (g_seeking) frac = g_seek_frac;
	gfx_fill_rect(cr, R(r.x, r.y + 2, r.w, 6), th->chrome2);
	gfx_fill_rect(cr, R(r.x, r.y + 2, r.w * frac, 6), th->accent);
	if (g_seeking) gfx_fill_rect(cr, R(r.x + r.w * frac - 5, r.y - 8, 10, 26), th->fg);
	char a[24], b[24], rb[28];
	fmt_dur(frac * dur, a, sizeof a);
	fmt_dur(dur - frac * dur, b, sizeof b);
	snprintf(rb, sizeof rb, "-%s", b);
	gfx_text(cr, a, r.x, r.y + 14, 15, FW_REGULAR, th->sub, TA_LEFT, 0, 1);
	if (dur > 0) gfx_text(cr, rb, rect_right(r), r.y + 14, 15, FW_REGULAR, th->sub, TA_RIGHT, 0, 1);
}

static bool big_button(UI *ui, const char *key, double cx, double cy, double dia, Icon ic) {
	UiId id = ui_id(key);
	ui_hit(ui, id, R(cx - dia / 2 - 6, cy - dia / 2 - 6, dia + 12, dia + 12));
	bool pr = ui_pressed(ui, id);
	const Theme *th = &ui->th;
	if (pr) gfx_fill_circle(ui->cr, cx, cy, dia / 2, th->fg);
	else gfx_stroke_circle(ui->cr, cx, cy, dia / 2 - 1, 2.5, th->fg);
	icon_draw(ui->cr, ic, cx, cy, dia * 0.42, pr ? th->bg : th->fg);
	return ui_clicked(ui, id);
}

static void draw_now(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	QItem *c = q_cur();
	Rect cnt = appbar_content(&g_bar, area);
	double y = cnt.y + 8;
	y += gfx_text(cr, i18n_upper(_("now playing")), cnt.x + 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, cnt.w - 36, 1) + 10;
	if (!c) {
		app_empty(ui, R(cnt.x, y, cnt.w, cnt.h), _("nothing is playing. pick some music from your collection."));
		appbar_draw(ui, &g_bar, area, NULL, 0, NULL, 0);
		return;
	}
	bool owns = music_owns();
	PlayerState st = owns ? player_state() : PL_STOPPED;
	if (st == PL_PLAYING) player_watch();
	double dur = owns && player_duration() > 0 ? player_duration() : c->dur;
	double pos = owns ? player_position() : 0;
	bool wide = cnt.w > cnt.h * 1.15;
	double avail_h = rect_bottom(cnt) - y;
	/* layout: info block needs ~ 250dp under the art in portrait */
	double art, ix, iy, iw;
	if (wide) {
		art = MIN(avail_h - 16, cnt.w * 0.42);
		ix = cnt.x + 24 + art + 28;
		iy = y + MAX(0, (avail_h - 300) / 2);
		iw = rect_right(cnt) - ix - 24;
	} else {
		art = MIN(cnt.w - 48, avail_h - 262);
		art = MAX(art, 80);
		ix = cnt.x + 24;
		iy = y + art + 16;
		iw = cnt.w - 48;
	}
	/* the page scrolls only when the screen is very short */
	double need = wide ? 300 : art + 270;
	ui_scroll_begin(ui, ui_id("mus-now-scroll"), &g_sc_now, R(cnt.x, y, cnt.w, avail_h), need);
	Rect ar = R(cnt.x + 24, y, art, art);
	UiId aid = ui_id("mus-now-art");
	ui_hit(ui, aid, ar);
	art_draw(ui, c->art, ar, true);
	double ty = iy;
	ty += gfx_text(cr, c->title, ix, ty, wide ? 34 : 32, FW_LIGHT, th->fg, TA_LEFT, iw, wide ? 2 : 1);
	ty += gfx_text(cr, artist_name(c->artist), ix, ty, 20, FW_SEMILIGHT, th->fg, TA_LEFT, iw, 1);
	ty += gfx_text(cr, album_name(c->album), ix, ty, 16, FW_REGULAR, th->sub, TA_LEFT, iw, 1) + 18;
	seek_bar(ui, R(ix, ty, iw, 10), pos, dur, owns ? player_can_seek() : false);
	ty += 48;
	double cy = ty + 38, cx = ix + iw / 2;
	if (big_button(ui, "mus-prev", cx - 100, cy, 58, ICON_PREV)) music_prev();
	if (big_button(ui, "mus-pp", cx, cy, 78, st == PL_PLAYING ? ICON_PAUSE : ICON_PLAY)) music_toggle();
	if (big_button(ui, "mus-next", cx + 100, cy, 58, ICON_NEXT)) music_next();
	/* shuffle / repeat state */
	double sy = cy + 54;
	icon_draw(cr, ICON_SHUFFLE, cx - 22, sy, 20, Q.shuffle ? th->accent : th->dim);
	icon_draw(cr, Q.repeat == REP_ONE ? ICON_REPEAT_ONE : ICON_REPEAT, cx + 22, sy, 20, Q.repeat ? th->accent : th->dim);
	char qb[48];
	snprintf(qb, sizeof qb, tr("%d of %d"), Q.pos + 1, Q.n);
	gfx_text(cr, qb, ix + iw, sy - 10, 15, FW_REGULAR, th->sub, TA_RIGHT, 0, 1);
	ui_scroll_end(ui);

	AppBarBtn btns[3] = {{ICON_SHUFFLE, Q.shuffle ? _("shuffle on") : _("shuffle off"), false},
	                     {Q.repeat == REP_ONE ? ICON_REPEAT_ONE : ICON_REPEAT, Q.repeat == REP_OFF ? _("repeat off") : Q.repeat == REP_ALL ? _("repeat all") : _("repeat one"), false},
	                     {ICON_LIST, _("queue"), false}};
	const char *menu[] = {_("go to artist"), _("go to album"), _("add to playlist")};
	int r = appbar_draw(ui, &g_bar, area, btns, 3, menu, 3);
	if (r == 0) set_shuffle(!Q.shuffle);
	else if (r == 1) { Q.repeat = (Q.repeat + 1) % 3; save_soon(); }
	else if (r == 2) {
		g_sc_queue = (Scroll){0};
		ui_scroll_to(&g_sc_queue, MAX(0, (Q.pos - 2) * 64.0));
		push(PG_QUEUE, 0);
	} else if (r >= APPBAR_MENU) {
		int si = lib_find(c->path);
		if (r == APPBAR_MENU + 2) { char *p = c->path; ask_playlist(&p, 1); }
		else if (si < 0) shell_toast(_("This song isn't in your collection"), c->title, tr("Music"));
		else do_action(r == APPBAR_MENU ? A_GO_ARTIST : A_GO_ALBUM, T_SONG, si, 0);
	}
}

static void queue_save_cb(int b, void *ud) {
	if (b != 0) return;
	char name[128];
	str_copy(name, sizeof name, dialog_input_text());
	char *t = str_trim(name);
	if (!*t) return;
	Playlist *p = pl_new(t);
	for (int i = 0; i < Q.n; i++) pl_add(p, Q.it[Q.order[i]].path);
	shell_toast(_("Playlist saved"), p->name, tr("Music"));
	save_soon();
}

static void draw_queue(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	Rect c = appbar_content(&g_bar, area);
	double top = ui_page_header(ui, c.x + 14, c.y + 6, tr("Music"), _("queue"));
	Rect view = R(c.x, top, c.w, rect_bottom(c) - top);
	const double ROW = 64;
	if (!Q.n) app_empty(ui, view, _("nothing is playing. pick some music from your collection."));
	else {
		double off = ui_scroll_begin(ui, ui_id("mus-queue-scroll"), &g_sc_queue, view, Q.n * ROW + 24);
		cairo_translate(cr, view.x, view.y);
		for (int k = 0; k < Q.n; k++) {
			double y = k * ROW;
			if (y + ROW < off || y > off + view.h) continue;
			QItem *q = &Q.it[Q.order[k]];
			UiId id = ui_idn("mus-q", k);
			song_row(ui, id, R(0, y, view.w, ROW), NULL, q->title, artist_name(q->artist), q->dur, k == Q.pos);
			if (ui_clicked(ui, id)) play_pos(k, true, 0);
			if (ui_longpressed(ui, id)) open_menu(ui, T_QUEUE, k, 0);
		}
		ui_scroll_end(ui);
	}
	const char *menu[] = {_("save as playlist"), _("clear")};
	int r = appbar_draw(ui, &g_bar, area, NULL, 0, menu, Q.n ? 2 : 0);
	if (r == APPBAR_MENU) dialog_input(_("save as playlist"), _("playlist name"), "", 0, queue_save_cb, NULL);
	else if (r == APPBAR_MENU + 1) {
		if (music_owns()) player_stop();
		q_clear();
		save_soon();
		g_nav.depth = 0;
		g_nav.from = g_nav.to = g_nav.stack[0];
	}
}

/* ------------------------------------------------------------ search */

static void run_search(void) {
	g_sr_artists.len = g_sr_albums.len = g_sr_songs.len = 0;
	if (!g_lib || !g_query[0]) return;
	for (int i = 0; i < g_lib->nartists && g_sr_artists.len < 8; i++)
		if (g_lib->artists[i].name[0] && str_contains_fold(g_lib->artists[i].name, g_query)) vec_push(&g_sr_artists, i);
	for (int i = 0; i < g_lib->nalbums && g_sr_albums.len < 8; i++)
		if (g_lib->albums[i].name[0] && str_contains_fold(g_lib->albums[i].name, g_query)) vec_push(&g_sr_albums, i);
	for (int i = 0; i < g_lib->nsongs && g_sr_songs.len < 100; i++)
		if (str_contains_fold(g_lib->songs[i].title, g_query)) vec_push(&g_sr_songs, i);
}

static void draw_search(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect c = area;
	double y = c.y + 8;
	y += gfx_text(cr, i18n_upper(tr("Music")), c.x + 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, c.w - 36, 1) + 8;
	bool changed = false;
	UiId tid = ui_id("mus-search-box");
	if (!g_query[0] && ui->focus != tid && nav_page(&g_nav) == PAGE(PG_SEARCH, 0) && !nav_busy(&g_nav, ui)) ui_focus(ui, tid);
	ui_textbox(ui, tid, R(c.x + 24, y, c.w - 48, 44), g_query, sizeof g_query, _("search music"), 0, &changed);
	if (changed) { run_search(); g_sc_search = (Scroll){0}; }
	y += 56;
	Rect view = R(c.x, y, c.w, rect_bottom(c) - y);
	int na = g_sr_artists.len, nb = g_sr_albums.len, ns = g_sr_songs.len;
	if (!g_query[0]) return;
	if (!na && !nb && !ns) { app_empty(ui, view, _("no results")); return; }
	const double ROW = 64;
	double total = (na ? 36 + na * ROW : 0) + (nb ? 36 + nb * ROW : 0) + (ns ? 36 + ns * ROW : 0) + 24;
	double off = ui_scroll_begin(ui, ui_id("mus-search-scroll"), &g_sc_search, view, total);
	cairo_translate(cr, view.x, view.y);
	double yy = 0;
	if (na) {
		yy = section_label(ui, 24, yy, view.w - 48, _("artists"));
		for (int k = 0; k < na; k++, yy += ROW) {
			if (yy + ROW < off || yy > off + view.h) continue;
			Artist *a = &g_lib->artists[g_sr_artists.items[k]];
			UiId id = ui_idn("mus-sr-ar", k);
			char b[48];
			song_row(ui, id, R(0, yy, view.w, ROW), NULL, a->name, count_str(b, sizeof b, a->n, "%d song", "%d songs"), 0, false);
			if (ui_clicked(ui, id)) push(PG_ARTIST, g_sr_artists.items[k]);
			if (ui_longpressed(ui, id)) open_menu(ui, T_ARTIST, g_sr_artists.items[k], 0);
		}
	}
	if (nb) {
		yy = section_label(ui, 24, yy, view.w - 48, _("albums"));
		for (int k = 0; k < nb; k++, yy += ROW) {
			if (yy + ROW < off || yy > off + view.h) continue;
			Album *a = &g_lib->albums[g_sr_albums.items[k]];
			UiId id = ui_idn("mus-sr-al", k);
			song_row(ui, id, R(0, yy, view.w, ROW), NULL, a->name, a->various ? _("various artists") : artist_name(a->artist), 0, false);
			if (ui_clicked(ui, id)) push(PG_ALBUM, g_sr_albums.items[k]);
			if (ui_longpressed(ui, id)) open_menu(ui, T_ALBUM, g_sr_albums.items[k], 0);
		}
	}
	if (ns) {
		yy = section_label(ui, 24, yy, view.w - 48, _("songs"));
		for (int k = 0; k < ns; k++, yy += ROW) {
			if (yy + ROW < off || yy > off + view.h) continue;
			Song *s = &g_lib->songs[g_sr_songs.items[k]];
			UiId id = ui_idn("mus-sr-so", k);
			song_row(ui, id, R(0, yy, view.w, ROW), NULL, s->title, artist_name(s->artist), s->dur, is_current(s->path));
			if (ui_clicked(ui, id)) play_songs(g_sr_songs.items, ns, k, false);
			if (ui_longpressed(ui, id)) open_menu(ui, T_SONG, g_sr_songs.items[k], 0);
		}
	}
	ui_scroll_end(ui);
}

/* ------------------------------------------------------------- app */

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	switch (PKIND(page)) {
	case PG_ROOT: draw_root(ui, area); break;
	case PG_ARTIST: draw_artist(ui, area, PCTX(page)); break;
	case PG_ALBUM: draw_album(ui, area, PCTX(page)); break;
	case PG_PLAYLIST: draw_playlist(ui, area, PCTX(page), false); break;
	case PG_M3U: draw_playlist(ui, area, PCTX(page), true); break;
	case PG_NOW: draw_now(ui, area); break;
	case PG_QUEUE: draw_queue(ui, area); break;
	case PG_SEARCH: draw_search(ui, area); break;
	}
}

static void music_draw(UI *ui, Rect area) {
	if (!g_nav_ready) { nav_init(&g_nav, PAGE(PG_ROOT, 0)); g_nav_ready = true; }
	if (g_lib && g_lib->lang != (int)i18n_lang() && !g_scanning) scan_start(true);
	nav_draw(ui, &g_nav, area, page_fn, NULL);
}

static bool music_back(void) {
	if (appbar_back(&g_bar)) return true;
	int k = PKIND(nav_page(&g_nav));
	if (k == PG_ROOT) return jumplist_back(&g_jl_artists) || jumplist_back(&g_jl_songs);
	if (k == PG_SEARCH) sh.ui.focus = 0;
	return nav_pop(&g_nav, &sh.ui);
}

/* Plays a file handed over by another app: the songs of its folder become the queue. */
static void open_file(const char *path) {
	int si = lib_find(path);
	if (si >= 0) {
		Album *a = &g_lib->albums[g_lib->songs[si].alb];
		for (int k = 0; k < a->n; k++) if (a->songs[k] == si) { play_songs(a->songs, a->n, k, false); return; }
	}
	char *p = (char *)path;
	play_paths(&p, 1, 0, false);
}

static void music_open(const char *arg) {
	if (!g_nav_ready) { nav_init(&g_nav, PAGE(PG_ROOT, 0)); g_nav_ready = true; }
	g_open = true;
	load_state();
	if (!g_lib && !g_scanning) scan_start(true);
	else if (now_sec() - g_last_scan > 30) scan_start(false);
	if (!arg || !*arg) return;
	if (str_eq(arg, "nowplaying")) {
		if (PKIND(nav_page(&g_nav)) != PG_NOW) push(PG_NOW, 0);
	} else if (arg[0] == '/' || str_starts(arg, "file://")) {
		char *p = str_starts(arg, "file://") ? g_filename_from_uri(arg, NULL, NULL) : g_strdup(arg);
		if (p && file_exists(p) && is_audio(p)) {
			open_file(p);
			g_nav.depth = 0;
			push(PG_NOW, 0);
		}
		g_free(p);
	}
}

static void music_close(void) {
	g_open = false;
	if (g_save_dirty) save_state_now();
	lib_free(g_lib);
	g_lib = NULL;
	if (g_thumbs) imgcache_clear(g_thumbs);
	if (g_big) imgcache_clear(g_big);
	nav_init(&g_nav, PAGE(PG_ROOT, 0));
	g_pivot = (Pager){0};
	g_bar = (AppBar){0};
	g_jl_artists = g_jl_songs = (JumpList){0};
	g_sc_albums = g_sc_pls = (Scroll){0};
	g_sr_artists.len = g_sr_albums.len = g_sr_songs.len = 0;
	g_query[0] = 0;
	g_last_scan = -1000;
}

/* ------------------------------------------------------------ live tile */

static void tile_icon(UI *ui, Rect r) {
	double s = MIN(r.w, r.h);
	icon_draw(ui->cr, ICON_MUSIC, rect_cx(r), r.y + r.h * 0.46, MIN(s * 0.42, 70) * 1.1, COL_WHITE);
	app_tile_name(ui, r, tr("Music"));
}

static void music_tile(UI *ui, Rect r, TileSize size, double now) {
	cairo_t *cr = ui->cr;
	QItem *c = q_cur();
	if (!c || !music_owns() || player_state() == PL_STOPPED) { tile_icon(ui, r); return; }
	Image *img = c->art ? art_image(ui, c->art, false) : NULL;
	if (size == TILE_WIDE) {
		Rect ar = R(r.x, r.y, r.h, r.h);
		if (img) gfx_image_cover(cr, img, ar, 1.0);
		else icon_draw(cr, ICON_MUSIC, rect_cx(ar), rect_cy(ar), r.h * 0.42, COL_WHITE);
		double x = r.x + r.h + 12, w = r.w - r.h - 22;
		double y = r.y + 10;
		y += gfx_text(cr, c->title, x, y, 20, FW_SEMILIGHT, COL_WHITE, TA_LEFT, w, 2);
		gfx_text(cr, artist_name(c->artist), x, y, 15, FW_REGULAR, RGBA(0xffffff, 0.85f), TA_LEFT, w, 1);
		gfx_text(cr, tr("Music"), x, rect_bottom(r) - 24, 15, FW_REGULAR, COL_WHITE, TA_LEFT, w, 1);
	} else {
		if (img) gfx_image_cover(cr, img, r, 1.0);
		else icon_draw(cr, ICON_MUSIC, rect_cx(r), r.y + r.h * 0.4, MIN(r.w, r.h) * 0.4, COL_WHITE);
		gfx_fill_rect(cr, R(r.x, rect_bottom(r) - 32, r.w, 32), RGBA(0x000000, 0.5f));
		gfx_text(cr, c->title, r.x + 9, rect_bottom(r) - 26, 15, FW_REGULAR, COL_WHITE, TA_LEFT, r.w - 18, 1);
	}
}

const AppDef app_music = {
	.id = "music", .name = N_("Music"), .icon = ICON_MUSIC, .color = 0, .keywords = "mp3 songs audio player albums artists",
	.open = music_open, .draw = music_draw, .back = music_back, .close = music_close, .tile = music_tile,
};

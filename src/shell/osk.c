#include "shell.h"
#include "../config.h"
#include "../i18n.h"
#include "../util.h"

#include <glib.h>
#include <math.h>
#include <xkbcommon/xkbcommon-keysyms.h>

/* On-screen keyboard in the Windows Phone 8.1 style. It types into built-in text
 * fields directly, and into Wayland clients through text-input-v3 (which makes
 * ç ş ğ ı ö ü work everywhere) or synthetic key events. */

enum { K_CHAR, K_SHIFT, K_BKSP, K_ENTER, K_SPACE, K_SYM, K_SYM2, K_ABC, K_LANG, K_TEXT };

typedef struct {
	char label[16];
	char out[16];
	int type;
	float w;
	char alts[40];
} Key;

static int g_shift;            /* 0 off, 1 once, 2 caps lock */
static int g_page;             /* 0 letters, 1 symbols, 2 more symbols */
static int g_layout;           /* index into enabled layouts */
static bool g_ui_wanted;       /* a built-in text field wants the keyboard */
static int g_ui_flags;
static char g_word[48];
static bool g_sentence_start = true;
static double g_press_t0;
static UiId g_repeat_id;
static double g_last_repeat;
static struct { bool open; Rect anchor; char alts[40]; int n; int sel; UiId key; } g_alt;
static double g_last_shift_tap;

/* ------------------------------------------------------------- dictionary - */

static const char *WORDS_EN =
	"the be to of and a in that have I it for not on with he as you do at this but his by from they we say her she or an "
	"will my one all would there their what so up out if about who get which go me when make can like time no just him "
	"know take people into year your good some could them see other than then now look only come its over think also "
	"back after use two how our work first well way even new want because any these give day most us hello hi thanks "
	"thank please sorry okay yes tomorrow today tonight morning night love friend meeting call message phone home where "
	"why going doing really great nice awesome definitely probably already again always never maybe later soon sure "
	"where when what please tell today tomorrow yesterday weekend birthday congratulations welcome goodbye little "
	"should would could might must still every little very much many right where here there while before between "
	"another house family money water food coffee lunch dinner breakfast office school music movie photo picture";

static const char *WORDS_TR =
	"ve bir bu da için ile ne çok daha var olarak gibi ama değil en kadar sonra ben sen o biz siz onlar evet hayır "
	"merhaba teşekkürler teşekkür lütfen tamam nasılsın nasılsınız iyi günaydın geceler görüşürüz bugün yarın şimdi "
	"nerede neden nasıl kim zaman hangi her şey bütün benim senin bizim yok mu mı mi mü çünkü ancak fakat veya yani "
	"artık hala hiç bile belki herhalde tabii güzel arkadaş aile ev iş okul telefon mesaj arama toplantı akşam sabah "
	"gece öğle gün hafta ay yıl saat dakika para su ekmek çay kahve yemek gelmek gitmek yapmak olmak demek bilmek "
	"istemek görmek vermek almak geliyorum gidiyorum bakalım tamamdır sağol rica ederim afiyet olsun hoş geldiniz "
	"hoşça kal kendine iyi bak anladım biliyorum bilmiyorum düşünüyorum bekliyorum geldim gittim yaptım ettim "
	"yarın görüşürüz akşam yemeği öğle yemeği kahvaltı doğum günü kutlu olsun mutlu yıllar seviyorum özledim "
	"nerdesin ne yapıyorsun neredesin ararım yazarım haber ver müsait misin buradayım geç kaldım yoldayım";

static VEC(char *) g_learned;
static bool g_dict_loaded;

static void load_dict(void) {
	if (g_dict_loaded) return;
	g_dict_loaded = true;
	cJSON *a = config_section("osk_words", true);
	cJSON *it;
	cJSON_ArrayForEach(it, a) if (cJSON_IsString(it)) vec_push(&g_learned, xstrdup(it->valuestring));
}

static void learn_word(const char *w) {
	if (strlen(w) < 3 || strlen(w) > 24 || !cfg.osk_suggest) return;
	load_dict();
	for (int i = 0; i < g_learned.len; i++) if (str_eq(g_learned.items[i], w)) return;
	if (g_learned.len > 400) { free(g_learned.items[0]); vec_remove(&g_learned, 0); }
	vec_push(&g_learned, xstrdup(w));
	cJSON *a = config_section("osk_words", true);
	cJSON_AddItemToArray(a, cJSON_CreateString(w));
	while (cJSON_GetArraySize(a) > 400) cJSON_DeleteItemFromArray(a, 0);
	config_save_soon();
}

static int suggest(const char *prefix, char out[3][48]) {
	int n = 0;
	if (!prefix[0]) return 0;
	char *pf = str_fold(prefix);
	size_t pl = strlen(prefix);
	load_dict();
	/* learned words first (most recent first), then the built-in lists */
	for (int i = g_learned.len - 1; i >= 0 && n < 3; i--) {
		const char *w = g_learned.items[i];
		if (strncmp(w, prefix, pl) == 0 && strlen(w) > pl) str_copy(out[n++], 48, w);
	}
	const char *lists[2] = {WORDS_TR, WORDS_EN};
	if (!(i18n_lang() == LANG_TR)) { lists[0] = WORDS_EN; lists[1] = WORDS_TR; }
	for (int l = 0; l < 2 && n < 3; l++) {
		const char *p = lists[l];
		while (*p && n < 3) {
			while (*p == ' ') p++;
			const char *e = p;
			while (*e && *e != ' ') e++;
			size_t len = (size_t)(e - p);
			if (len > pl && len < 40) {
				char w[48];
				memcpy(w, p, len);
				w[len] = 0;
				bool dup = false;
				for (int i = 0; i < n; i++) if (str_eq(out[i], w)) dup = true;
				if (!dup && (strncmp(w, prefix, pl) == 0)) str_copy(out[n++], 48, w);
			}
			p = e;
		}
	}
	free(pf);
	return n;
}

/* ---------------------------------------------------------------- layouts - */

static int enabled_layouts(const char **names, int max) {
	int n = 0;
	char *copy = xstrdup(cfg.osk_layouts[0] ? cfg.osk_layouts : "en");
	static char store[4][8];
	char *save = NULL;
	for (char *t = strtok_r(copy, ",", &save); t && n < max && n < 4; t = strtok_r(NULL, ",", &save)) {
		str_copy(store[n], 8, t);
		names[n] = store[n];
		n++;
	}
	free(copy);
	if (!n) { str_copy(store[0], 8, "en"); names[0] = store[0]; n = 1; }
	return n;
}

static const char *cur_layout(void) {
	const char *names[4];
	int n = enabled_layouts(names, 4);
	return names[CLAMP(g_layout, 0, n - 1)];
}

static const char *alts_for(const char *layout, const char *ch) {
	bool tr = str_eq(layout, "tr");
	switch (ch[0]) {
	case 'a': return tr ? "âáàä" : "àáâäãåā";
	case 'c': return tr ? "ç" : "çćč";
	case 'e': return "éèêëē";
	case 'g': return tr ? "ğ" : "ğ";
	case 'i': return tr ? "ıîíì" : "ìíîïı";
	case 'o': return tr ? "öôòó" : "òóôöõø";
	case 's': return tr ? "şß" : "šśß";
	case 'u': return tr ? "üûùú" : "ùúûü";
	case 'n': return "ñń";
	case 'y': return "ýÿ";
	case 'z': return "žźż";
	case '.': return ",?!:;'\"…";
	case ',': return ";:-_";
	case '-': return "–—_";
	case '$': return "€£¥₺";
	default: return "";
	}
}

static void upper_char(const char *layout, const char *in, char *out, size_t n) {
	bool tr = str_eq(layout, "tr");
	uint32_t cp;
	utf8_decode(in, &cp);
	if (tr && cp == 'i') cp = 0x130;
	else if (tr && cp == 0x131) cp = 'I';
	else cp = g_unichar_toupper(cp);
	char b[5];
	utf8_encode(cp, b);
	str_copy(out, n, b);
}

static int utf8_nth(const char *s, int i, char out[8]) {
	const char *p = s;
	for (int k = 0; k < i && *p; k++) { uint32_t cp; p += utf8_decode(p, &cp); }
	if (!*p) return 0;
	uint32_t cp;
	int n = utf8_decode(p, &cp);
	memcpy(out, p, (size_t)n);
	out[n] = 0;
	return n;
}

/* Builds the key rows for the current layout / page. Returns the row count. */
static int build_rows(Key rows[5][14], int ncols[5]) {
	const char *lay = cur_layout();
	bool tr = str_eq(lay, "tr");
	const char *names[4];
	int nlay = enabled_layouts(names, 4);
	int purpose = g_ui_flags;
	extern int textinput_purpose_flags(void);
	if (textinput_active()) purpose = textinput_purpose_flags();
	memset(ncols, 0, 5 * sizeof(int));
	int nr = 0;
#define ADD(r, ...) do { rows[r][ncols[r]++] = (Key){__VA_ARGS__}; } while (0)

	if (purpose & TB_NUMBER) {
		const char *g[4][4] = {{"1", "2", "3", "-"}, {"4", "5", "6", "."}, {"7", "8", "9", ","}, {"+", "0", "", ""}};
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 4; c++) ADD(r, .label = "", .out = "", .type = K_CHAR, .w = 1);
			for (int c = 0; c < 4; c++) { str_copy(rows[r][c].label, 16, g[r][c]); str_copy(rows[r][c].out, 16, g[r][c]); }
		}
		ADD(3, .label = "+", .out = "+", .type = K_CHAR, .w = 1);
		ADD(3, .label = "0", .out = "0", .type = K_CHAR, .w = 1);
		ADD(3, .label = "", .out = "", .type = K_BKSP, .w = 1);
		ADD(3, .label = "", .out = "\n", .type = K_ENTER, .w = 1);
		return 4;
	}

	if (g_page == 0) {
		const char *r1 = tr ? "qwertyuıopğü" : "qwertyuiop";
		const char *r2 = tr ? "asdfghjklşi" : "asdfghjkl";
		const char *r3 = tr ? "zxcvbnmöç" : "zxcvbnm";
		const char *rr[3] = {r1, r2, r3};
		for (int r = 0; r < 3; r++) {
			if (r == 1) ADD(r, .label = "", .out = "", .type = K_TEXT, .w = 0.5f);
			if (r == 2) ADD(r, .label = "", .out = "", .type = g_shift == 2 ? K_SHIFT : K_SHIFT, .w = 1.5f);
			int n = utf8_strlen(rr[r]);
			for (int i = 0; i < n; i++) {
				char ch[8];
				utf8_nth(rr[r], i, ch);
				Key k = {.type = K_CHAR, .w = 1};
				char out[16], lab[16];
				if (g_shift) { upper_char(lay, ch, out, sizeof out); str_copy(lab, sizeof lab, out); }
				else { str_copy(out, sizeof out, ch); str_copy(lab, sizeof lab, ch); }
				str_copy(k.label, sizeof k.label, lab);
				str_copy(k.out, sizeof k.out, out);
				const char *alts = alts_for(lay, ch);
				char altbuf[40] = "";
				if (r == 0 && i < 10) { altbuf[0] = (char)('0' + (i + 1) % 10); altbuf[1] = 0; }
				strncat(altbuf, alts, sizeof altbuf - strlen(altbuf) - 1);
				if (g_shift) {
					/* upper-case the accented alternates too */
					char up[48] = "";
					for (const char *p = altbuf; *p;) {
						uint32_t cp; int m = utf8_decode(p, &cp);
						char one[8]; memcpy(one, p, (size_t)m); one[m] = 0;
						char u[8]; upper_char(lay, one, u, sizeof u);
						strncat(up, u, sizeof up - strlen(up) - 1);
						p += m;
					}
					str_copy(k.alts, sizeof k.alts, up);
				} else str_copy(k.alts, sizeof k.alts, altbuf);
				rows[r][ncols[r]++] = k;
			}
			if (r == 1) ADD(r, .label = "", .out = "", .type = K_TEXT, .w = 0.5f);
			if (r == 2) ADD(r, .label = "", .out = "", .type = K_BKSP, .w = 1.5f);
		}
		nr = 3;
	} else {
		const char *p1 = g_page == 1 ? "1234567890" : "~`|\\^{}[]<>";
		const char *p2 = g_page == 1 ? "@#$%&*-+()" : "=_/<>€£¥₺§";
		const char *p3 = g_page == 1 ? "!\"':;?/" : "©®™°•…¿";
		const char *rr[3] = {p1, p2, p3};
		for (int r = 0; r < 3; r++) {
			if (r == 2) {
				if (g_page == 1) ADD(r, .label = "1/2", .out = "", .type = K_SYM2, .w = 1.5f);
				else ADD(r, .label = "2/2", .out = "", .type = K_SYM, .w = 1.5f);
			}
			int n = utf8_strlen(rr[r]);
			for (int i = 0; i < n; i++) {
				char ch[8];
				utf8_nth(rr[r], i, ch);
				Key k = {.type = K_CHAR, .w = 1};
				str_copy(k.label, sizeof k.label, ch);
				str_copy(k.out, sizeof k.out, ch);
				str_copy(k.alts, sizeof k.alts, alts_for(lay, ch));
				rows[r][ncols[r]++] = k;
			}
			if (r == 2) ADD(r, .label = "", .out = "", .type = K_BKSP, .w = 1.5f);
		}
		nr = 3;
	}
	/* bottom row */
	if (g_page == 0) ADD(3, .label = "&123", .out = "", .type = K_SYM, .w = 1.5f);
	else ADD(3, .label = "abc", .out = "", .type = K_ABC, .w = 1.5f);
	if (nlay > 1) ADD(3, .label = "", .out = "", .type = K_LANG, .w = 1.1f);
	if (purpose & TB_EMAIL) ADD(3, .label = "@", .out = "@", .type = K_CHAR, .w = 1.1f);
	else if (purpose & TB_URL) ADD(3, .label = "/", .out = "/", .type = K_CHAR, .w = 1.1f);
	else ADD(3, .label = ",", .out = ",", .type = K_CHAR, .w = 1.1f);
	ADD(3, .label = "", .out = " ", .type = K_SPACE, .w = nlay > 1 ? 3.4f : 4.4f);
	ADD(3, .label = ".", .out = ".", .type = K_CHAR, .w = 1.1f);
	if (purpose & TB_URL) ADD(3, .label = ".com", .out = ".com", .type = K_CHAR, .w = 1.5f);
	ADD(3, .label = "", .out = "\n", .type = K_ENTER, .w = 1.5f);
	(void)nr;
	return 4;
#undef ADD
}

/* --------------------------------------------------------------- sending -- */

static bool target_is_ui(void) { return sh.ui.focus != 0; }

static void send_text(const char *utf8) {
	if (target_is_ui()) { ui_text_event(&sh.ui, utf8); }
	else input_inject_text(utf8);
}

static void send_key(UiKey k, xkb_keysym_t sym) {
	if (target_is_ui()) ui_key_event(&sh.ui, k);
	else input_inject_keysym(sym, 0);
}

static void word_append(const char *s) {
	size_t n = strlen(g_word);
	if (n + strlen(s) < sizeof g_word) strcat(g_word, s);
}

static void end_word(void) {
	if (g_word[0]) learn_word(g_word);
	g_word[0] = 0;
}

static void type_string(const char *s) {
	uint32_t cp;
	utf8_decode(s, &cp);
	send_text(s);
	if (g_unichar_isalpha(cp) || cp == '\'') word_append(s);
	else end_word();
	if (g_shift == 1) g_shift = 0;
	g_sentence_start = (cp == '.' || cp == '!' || cp == '?');
	if (g_sentence_start) { /* wait for the space */ g_sentence_start = false; }
}

static void apply_autocap(void) {
	if (cfg.osk_autocap && g_page == 0 && g_shift == 0 && g_sentence_start) g_shift = 1;
}

void osk_reset(void) {
	g_shift = 0;
	g_page = 0;
	g_word[0] = 0;
	g_sentence_start = true;
	g_alt.open = false;
	g_layout = CLAMP(g_layout, 0, 3);
	/* start with the layout matching the UI language when it is enabled */
	const char *names[4];
	int n = enabled_layouts(names, 4);
	if (g_layout >= n) g_layout = 0;
}

void osk_init(void) { osk_reset(); }

void osk_set_context(int flags) { g_ui_flags = flags; }

void osk_note_wanted(bool wanted, int flags) {
	bool was = g_ui_wanted;
	g_ui_wanted = wanted;
	g_ui_flags = flags;
	if (wanted && !was) { g_sentence_start = true; g_word[0] = 0; g_page = 0; if (cfg.osk_autocap) g_shift = 1; }
	if (!wanted && was) { g_word[0] = 0; g_page = 0; g_shift = 0; }
}

bool osk_wanted(void) { return g_ui_wanted || textinput_active() || sh.kbd_forced; }

/* ------------------------------------------------------------------ layout - */

static double key_height(void) { return CLAMP(MIN(sh.w, sh.h) * 0.125, 42, 62); }
#define SUGG_H 38.0

double osk_height(void) {
	int purpose = g_ui_flags;
	if (textinput_active()) purpose = textinput_purpose_flags();
	int rows = (purpose & TB_NUMBER) ? 4 : 4;
	double kh = key_height();
	bool sugg = cfg.osk_suggest && !(purpose & (TB_NUMBER | TB_PASSWORD));
	return (sugg ? SUGG_H : 6) + rows * (kh + 6) + 6;
}

/* -------------------------------------------------------------------- draw - */

static Color key_bg(const Theme *th, bool pressed, bool special) {
	if (pressed) return th->dark ? COL_WHITE : COL_BLACK;
	if (th->dark) return special ? RGBX(0x2b2b2b) : RGBX(0x3d3d3d);
	return special ? RGBX(0xc2c2c2) : RGBX(0xf5f5f5);
}

void osk_draw(UI *ui, double bottom) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	int purpose = g_ui_flags;
	if (textinput_active()) purpose = textinput_purpose_flags();
	double full_h = osk_height();
	double kh = key_height();
	double width = MIN(sh.w, 700);
	double x0 = (sh.w - width) / 2;
	double y0 = bottom - sh.kbd_pos;
	Rect panel = R(0, y0, sh.w, sh.kbd_pos + 1);
	gfx_fill_rect(cr, panel, th->dark ? RGBX(0x1a1a1a) : RGBX(0xd7d7d7));
	ui_hit(ui, ui_id("osk-panel"), panel);

	/* the contents are laid out for the full height and clipped while sliding */
	ui_clip_push(ui, panel);
	double top = bottom - full_h + (full_h - sh.kbd_pos) * 0 ;
	top = y0;
	bool sugg_on = cfg.osk_suggest && !(purpose & (TB_NUMBER | TB_PASSWORD));
	double y = top;
	if (sugg_on) {
		char sg[3][48];
		int n = suggest(g_word, sg);
		double sw = width / 3;
		for (int i = 0; i < 3; i++) {
			Rect sr = R(x0 + i * sw, y, sw, SUGG_H);
			int slot = i == 1 ? 0 : i == 0 ? 1 : 2;     /* best suggestion in the middle */
			if (slot < n) {
				UiId id = ui_idn("osk-sugg", i);
				ui_hit(ui, id, sr);
				bool pr = ui_pressed(ui, id);
				if (pr) gfx_fill_rect(cr, sr, col_alpha(th->fg, 0.15f));
				gfx_text_in(cr, sg[slot], sr, 20, i == 1 ? FW_SEMIBOLD : FW_SEMILIGHT, th->fg, TA_CENTER, true);
				if (ui_clicked(ui, id)) {
					/* replace the typed prefix with the suggestion */
					int chars = utf8_strlen(g_word);
					for (int b = 0; b < chars; b++) send_key(UK_BACKSPACE, XKB_KEY_BackSpace);
					send_text(sg[slot]);
					send_text(" ");
					learn_word(sg[slot]);
					g_word[0] = 0;
					g_sentence_start = false;
					if (g_shift == 1) g_shift = 0;
				}
			}
			if (i > 0) gfx_fill_rect(cr, R(sr.x, sr.y + 8, 1, SUGG_H - 16), col_alpha(th->fg, 0.2f));
		}
		y += SUGG_H;
	} else {
		y += 6;
	}

	Key rows[5][14];
	int ncols[5];
	int nrows = build_rows(rows, ncols);
	apply_autocap();
	double gap = 6;
	const char *layname[2] = {"English", "Türkçe"};
	for (int r = 0; r < nrows; r++) {
		double total_w = 0;
		for (int c = 0; c < ncols[r]; c++) total_w += rows[r][c].w;
		double unit = (width - 12 - gap * (ncols[r] - 1)) / total_w;
		double x = x0 + 6;
		for (int c = 0; c < ncols[r]; c++) {
			Key *k = &rows[r][c];
			double w = unit * k->w;
			Rect kr = R(x, y + 3, w, kh);
			x += w + gap;
			if (k->type == K_TEXT) continue;
			UiId id = ui_idn("osk-key", r * 16 + c + (g_page * 100) + (g_shift * 1000) + g_layout * 7000);
			ui_hit(ui, id, R(kr.x - gap / 2, kr.y - 3, kr.w + gap, kr.h + 6));
			bool pr = ui_pressed(ui, id);
			bool special = k->type != K_CHAR && k->type != K_SPACE && k->type != K_TEXT;
			Color bg = key_bg(th, pr, special);
			if (k->type == K_ENTER) bg = pr ? (th->dark ? COL_WHITE : COL_BLACK) : th->accent;
			if (k->type == K_SHIFT && g_shift) bg = pr ? bg : (g_shift == 2 ? th->accent : th->accent);
			gfx_fill_rect(cr, kr, bg);
			Color fg = pr ? (th->dark ? COL_BLACK : COL_WHITE) : th->fg;
			if (k->type == K_ENTER && !pr) fg = th->on_accent;
			if (k->type == K_SHIFT && g_shift && !pr) fg = th->on_accent;
			switch (k->type) {
			case K_CHAR:
				gfx_text_in(cr, k->label, kr, strlen(k->label) > 3 ? 18 : 24, FW_SEMILIGHT, fg, TA_CENTER, true);
				if (k->alts[0] && g_page == 0 && r == 0) {
					char dg[8] = "";
					dg[0] = k->alts[0] >= '0' && k->alts[0] <= '9' ? k->alts[0] : 0;
					if (dg[0]) { dg[1] = 0; gfx_text(cr, dg, rect_right(kr) - 4, kr.y + 1, 10, FW_REGULAR, col_alpha(fg, 0.5f), TA_RIGHT, 0, 1); }
				}
				break;
			case K_SHIFT: icon_draw(cr, g_shift == 2 ? ICON_CAPS : ICON_SHIFT, rect_cx(kr), rect_cy(kr), 20, fg); break;
			case K_BKSP: icon_draw(cr, ICON_BACKSPACE, rect_cx(kr), rect_cy(kr), 22, fg); break;
			case K_ENTER: icon_draw(cr, ICON_ENTER, rect_cx(kr), rect_cy(kr), 21, fg); break;
			case K_SPACE: {
				gfx_text_in(cr, layname[str_eq(cur_layout(), "tr") ? 1 : 0], kr, 14, FW_REGULAR, col_alpha(fg, 0.55f), TA_CENTER, true);
				break;
			}
			case K_LANG: icon_draw(cr, ICON_GLOBE, rect_cx(kr), rect_cy(kr), 20, fg); break;
			default: gfx_text_in(cr, k->label, kr, 17, FW_SEMILIGHT, fg, TA_CENTER, true); break;
			}

			/* ---- behaviour */
			bool repeat_fire = false;
			if (k->type == K_BKSP && pr) {
				ui->animating = true;
				if (g_repeat_id != id) { g_repeat_id = id; g_press_t0 = ui->now; g_last_repeat = ui->now; }
				else if (ui->now - g_press_t0 > 0.45 && ui->now - g_last_repeat > 0.055) { repeat_fire = true; g_last_repeat = ui->now; }
			} else if (k->type == K_BKSP && g_repeat_id == id) g_repeat_id = 0;

			bool clicked = ui_clicked(ui, id);
			if (k->type == K_CHAR && k->alts[0] && ui_longpressed(ui, id)) {
				g_alt.open = true;
				g_alt.anchor = kr;
				str_copy(g_alt.alts, sizeof g_alt.alts, k->alts);
				g_alt.n = utf8_strlen(k->alts);
				g_alt.sel = 0;
				g_alt.key = id;
			}
			if (clicked || repeat_fire) {
				switch (k->type) {
				case K_CHAR: type_string(k->out); break;
				case K_SPACE:
					if (g_word[0]) end_word();
					send_text(" ");
					g_sentence_start = false;
					break;
				case K_BKSP:
					send_key(UK_BACKSPACE, XKB_KEY_BackSpace);
					if (g_word[0]) { int n = utf8_prev_len(g_word, (int)strlen(g_word)); g_word[strlen(g_word) - (size_t)n] = 0; }
					break;
				case K_ENTER:
					end_word();
					if (target_is_ui()) ui_key_event(&sh.ui, UK_ENTER); else input_inject_keysym(XKB_KEY_Return, 0);
					g_sentence_start = true;
					break;
				case K_SHIFT:
					if (ui->now - g_last_shift_tap < 0.4) g_shift = 2;
					else g_shift = g_shift ? 0 : 1;
					g_last_shift_tap = ui->now;
					g_sentence_start = false;       /* the user decided: no more auto-capitalisation */
					break;
				case K_SYM: g_page = g_page == 0 ? 1 : 1; break;
				case K_SYM2: g_page = 2; break;
				case K_ABC: g_page = 0; break;
				case K_LANG: {
					const char *names[4];
					int n = enabled_layouts(names, 4);
					g_layout = (g_layout + 1) % n;
					g_page = 0;
					break;
				}
				default: break;
				}
				ui->redraw = true;
			}
			/* alternates popup selection follows the finger */
			if (g_alt.open && g_alt.key == id) {
				double aw = 40, total = g_alt.n * aw;
				double ax = CLAMP(kr.x + kr.w / 2 - total / 2, 4, sh.w - total - 4);
				double fx, fy;
				ui_pointer_local(ui, &fx, &fy);
				if (ui->p.down) {
					int s = (int)floor((fx - ax) / aw);
					g_alt.sel = CLAMP(s, 0, g_alt.n - 1);
				}
				/* committed on release */
				if (ui_released(ui, id) && !ui->p.down) {
					char one[8];
					if (utf8_nth(g_alt.alts, g_alt.sel, one)) type_string(one);
					g_alt.open = false;
				}
			}
		}
		y += kh + gap;
	}
	ui_clip_pop(ui);

	/* alternates popup (drawn above the panel) */
	if (g_alt.open) {
		double aw = 40, ah = 52, total = g_alt.n * aw;
		double ax = CLAMP(g_alt.anchor.x + g_alt.anchor.w / 2 - total / 2, 4, sh.w - total - 4);
		double ay = g_alt.anchor.y - ah - 6;
		Rect pr = R(ax - 2, ay - 2, total + 4, ah + 4);
		gfx_fill_rect(cr, pr, th->dark ? RGBX(0x555555) : RGBX(0xffffff));
		gfx_stroke_rect(cr, pr, 2, th->accent);
		for (int i = 0; i < g_alt.n; i++) {
			char one[8];
			utf8_nth(g_alt.alts, i, one);
			Rect ir = R(ax + i * aw, ay, aw, ah);
			if (i == g_alt.sel) gfx_fill_rect(cr, ir, th->accent);
			gfx_text_in(cr, one, ir, 26, FW_SEMILIGHT, i == g_alt.sel ? th->on_accent : th->fg, TA_CENTER, true);
		}
		if (!ui->p.down) g_alt.open = false;
	}
}

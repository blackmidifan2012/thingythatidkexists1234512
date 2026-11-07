/* Calculator: Windows Phone style standard (immediate execution) and scientific
 * (operator precedence, parentheses, functions) modes, memory and history. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <math.h>

/* ------------------------------------------------------------ number text */

/* Canonical number text: '.' decimal, no grouping, <= 15 significant digits,
 * exponent notation for huge / tiny values. */
static void canon_num(double v, char *out, size_t cap) {
	if (v == 0 || fabs(v) < 1e-300) { str_copy(out, cap, "0"); return; }
	double a = fabs(v);
	char buf[64];
	if (a >= 1e16 || a < 1e-9) {
		snprintf(buf, sizeof buf, "%.14e", v);
		char *e = strchr(buf, 'e');
		char exp[16];
		str_copy(exp, sizeof exp, e);
		*e = 0;
		char *dot = strchr(buf, '.');
		if (dot) {
			char *end = buf + strlen(buf) - 1;
			while (end > dot && *end == '0') *end-- = 0;
			if (end == dot) *end = 0;
		}
		/* "e+020" -> "e+20" */
		int ev = atoi(exp + 1);
		snprintf(out, cap, "%se%c%d", buf, ev < 0 ? '-' : '+', abs(ev));
		return;
	}
	int mag = (int)floor(log10(a));
	int dec = CLAMP(14 - mag, 0, 20);
	snprintf(buf, sizeof buf, "%.*f", dec, v);
	char *dot = strchr(buf, '.');
	if (dot) {
		char *end = buf + strlen(buf) - 1;
		while (end > dot && *end == '0') *end-- = 0;
		if (end == dot) *end = 0;
	}
	if (str_eq(buf, "-0")) str_copy(buf, sizeof buf, "0");
	str_copy(out, cap, buf);
}

/* Converts canonical text (which may contain several numbers inside an expression)
 * to the display form of the current language: digit grouping and decimal mark. */
static void localize(const char *in, char *out, size_t cap) {
	bool tr_lang = i18n_lang() == LANG_TR;
	char gsep = tr_lang ? '.' : ',', dsep = tr_lang ? ',' : '.';
	size_t o = 0;
	const char *p = in;
#define PUT(c) do { if (o + 1 < cap) out[o++] = (c); } while (0)
	while (*p) {
		bool numstart = (*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9');
		bool prev_alpha = p > in && ((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z'));
		if (!numstart || prev_alpha) {
			/* copy one byte (UTF-8 continuation bytes pass through) */
			PUT(*p);
			p++;
			continue;
		}
		const char *s = p;
		while (*p >= '0' && *p <= '9') p++;
		int ilen = (int)(p - s);
		for (int i = 0; i < ilen; i++) {
			if (i > 0 && (ilen - i) % 3 == 0) PUT(gsep);
			PUT(s[i]);
		}
		if (*p == '.') {
			PUT(dsep);
			p++;
			while (*p >= '0' && *p <= '9') PUT(*p++);
		}
		if (*p == 'e' && (p[1] == '+' || p[1] == '-')) {
			PUT('e'); PUT(p[1]);
			p += 2;
			while (*p >= '0' && *p <= '9') PUT(*p++);
		}
	}
	out[o] = 0;
#undef PUT
}

/* ----------------------------------------------------------- calculator --- */

enum { T_NUM, T_OP, T_LP };
enum { OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_ROOT, OP_MOD };
enum { E_OK, E_DIV0, E_UNDEF, E_INVALID, E_OVERFLOW };

typedef struct { int kind, op; double v; char disp[120]; } Tok;

#define MAX_TOK 64
#define MAX_HIST 50

typedef struct { char *expr; double result; } HistItem;

static struct {
	Tok t[MAX_TOK];
	int n;
	char in[48];            /* digits being typed (canonical) */
	bool typing;
	bool have_val;          /* an operand that was not typed: result, constant, function... */
	double val;
	char vdisp[120];
	bool vgroup;            /* vdisp is a parenthesised group */
	double shown;           /* value shown when there is no operand */
	int err;
	char line[600];         /* canonical expression line */
	bool just_eval;
	int last_op;
	double last_b;
	double mem;
	bool has_mem;
	bool deg, second, sci_portrait;
	VEC(HistItem) hist;
	bool loaded;
	int flash_key;
	double flash_t;
} C = {.deg = true, .flash_key = -1};

static Nav g_nav;
static AppBar g_bar, g_hbar;
static Scroll g_hscroll;
static Timer *g_save_timer;
static bool g_sci;              /* current layout is scientific */

static const char *op_text(int op) {
	static const char *s[] = {"", "+", "–", "×", "÷", "^", "yroot", "mod"};
	return s[CLAMP(op, 0, 7)];
}

static int op_prec(int op, bool sci) {
	if (!sci) return 1;
	switch (op) {
	case OP_ADD: case OP_SUB: return 1;
	case OP_MUL: case OP_DIV: case OP_MOD: return 2;
	default: return 3;
	}
}

static bool op_right(int op) { return op == OP_POW || op == OP_ROOT; }

static int check(double r) {
	if (isnan(r)) return E_INVALID;
	if (isinf(r)) return E_OVERFLOW;
	return E_OK;
}

static int apply_op(int op, double a, double b, double *r) {
	switch (op) {
	case OP_ADD: *r = a + b; break;
	case OP_SUB: *r = a - b; break;
	case OP_MUL: *r = a * b; break;
	case OP_DIV:
		if (b == 0) return a == 0 ? E_UNDEF : E_DIV0;
		*r = a / b;
		break;
	case OP_MOD:
		if (b == 0) return a == 0 ? E_UNDEF : E_DIV0;
		*r = fmod(a, b);
		if (*r != 0 && ((*r < 0) != (b < 0))) *r += b;   /* sign follows the divisor */
		break;
	case OP_POW:
		if (a == 0 && b < 0) return E_DIV0;
		if (a < 0 && b != floor(b)) {
			/* real odd roots of negative numbers: (-8)^(1/3) = -2 */
			double inv = 1 / b, ri = round(inv);
			if (fabs(inv - ri) < 1e-9 && fmod(fabs(ri), 2) == 1) { *r = -pow(-a, b); break; }
			return E_INVALID;
		}
		*r = pow(a, b);
		break;
	case OP_ROOT:
		if (b == 0) return E_INVALID;
		if (a < 0) {
			if (b == floor(b) && fmod(fabs(b), 2) == 1) { *r = -pow(-a, 1 / b); break; }
			return E_INVALID;
		}
		*r = pow(a, 1 / b);
		break;
	default: *r = b; break;
	}
	if (fabs(*r) < 1e-300) *r = 0;
	return check(*r);
}

/* Shunting-yard over tokens [0,n). Reduces pending operators whose precedence is
 * >= final_prec (0 = everything above the innermost open parenthesis). */
static int sy_eval(const Tok *t, int n, bool sci, int final_prec, double *top) {
	double vs[MAX_TOK + 2];
	int os[MAX_TOK + 2];
	int nv = 0, no = 0, e = E_OK;
#define REDUCE() do { \
		int op_ = os[--no]; \
		double b_ = nv ? vs[--nv] : 0, a_ = nv ? vs[--nv] : 0, r_ = 0; \
		e = apply_op(op_, a_, b_, &r_); \
		if (e) return e; \
		vs[nv++] = r_; \
	} while (0)
	for (int i = 0; i < n; i++) {
		if (t[i].kind == T_NUM) vs[nv++] = t[i].v;
		else if (t[i].kind == T_LP) os[no++] = -1;
		else {
			int p = op_prec(t[i].op, sci);
			while (no && os[no - 1] != -1) {
				int q = op_prec(os[no - 1], sci);
				if (q > p || (q == p && !op_right(t[i].op))) REDUCE();
				else break;
			}
			os[no++] = t[i].op;
		}
	}
	while (no && os[no - 1] != -1 && op_prec(os[no - 1], sci) >= final_prec) REDUCE();
#undef REDUCE
	*top = nv ? vs[nv - 1] : 0;
	return E_OK;
}

static const char *err_text(int e) {
	switch (e) {
	case E_DIV0: return _("cannot divide by zero");
	case E_UNDEF: return _("result is undefined");
	case E_OVERFLOW: return _("overflow");
	default: return _("invalid input");
	}
}

static void rebuild_line(void) {
	C.line[0] = 0;
	for (int i = 0; i < C.n; i++) {
		const char *s = C.t[i].kind == T_NUM ? C.t[i].disp : C.t[i].kind == T_LP ? "(" : op_text(C.t[i].op);
		size_t l = strlen(C.line);
		snprintf(C.line + l, sizeof C.line - l, "%s%s", (l && C.t[i - 1].kind != T_LP) ? " " : "", s);
	}
}

static int open_parens(void) {
	int k = 0;
	for (int i = 0; i < C.n; i++) if (C.t[i].kind == T_LP) k++;
	return k;
}

static void set_error(int e) {
	C.err = e;
	C.typing = false;
	C.have_val = false;
}

static void clear_all(void) {
	C.n = 0;
	C.typing = false;
	C.have_val = false;
	C.vgroup = false;
	C.shown = 0;
	C.err = E_OK;
	C.line[0] = 0;
	C.just_eval = false;
	C.last_op = OP_NONE;
}

/* After "=" or an error, a new number starts a fresh calculation. */
static void fresh_if_done(void) {
	if (C.err) clear_all();
	if (C.just_eval) {
		C.n = 0;
		C.line[0] = 0;
		C.just_eval = false;
		C.last_op = OP_NONE;
	}
}

static bool has_operand(void) { return C.typing || C.have_val; }

static double operand_value(void) {
	if (C.typing) return strtod(C.in, NULL);
	if (C.have_val) return C.val;
	return C.shown;
}

static void operand_disp(char *out, size_t cap) {
	if (C.typing) {
		/* "12." -> "12" */
		str_copy(out, cap, C.in);
		size_t l = strlen(out);
		if (l && out[l - 1] == '.') out[l - 1] = 0;
		if (!strcmp(out, "-0")) str_copy(out, cap, "0");
	} else if (C.have_val) str_copy(out, cap, C.vdisp);
	else canon_num(C.shown, out, cap);
}

static void set_val(double v, const char *disp) {
	C.have_val = true;
	C.typing = false;
	C.val = v;
	C.vgroup = false;
	if (disp) str_copy(C.vdisp, sizeof C.vdisp, disp);
	else canon_num(v, C.vdisp, sizeof C.vdisp);
}

static bool push_tok(Tok tk) {
	if (C.n >= MAX_TOK - 1) return false;
	C.t[C.n++] = tk;
	return true;
}

/* Commits the current operand (or the shown value) as a number token. */
static bool commit_operand(void) {
	double v = operand_value();
	int e = check(v);
	if (e) { set_error(e); return false; }
	Tok tk = {.kind = T_NUM, .v = v};
	operand_disp(tk.disp, sizeof tk.disp);
	C.typing = C.have_val = false;
	return push_tok(tk);
}

static void history_add(const char *expr, double r);
static void save_soon(void);

static void do_digit(int d) {
	fresh_if_done();
	if (!C.typing) {
		C.typing = true;
		C.have_val = false;
		str_copy(C.in, sizeof C.in, "0");
	}
	char *e = strchr(C.in, 'e');
	if (e) {
		/* exponent digits */
		char *ed = e + 2;
		if (strlen(ed) >= 4) return;
		if (str_eq(ed, "0")) *ed = 0;
	} else {
		int digits = 0;
		for (char *p = C.in; *p; p++) if (*p >= '0' && *p <= '9') digits++;
		bool neg = C.in[0] == '-';
		if (str_eq(C.in + neg, "0")) { C.in[neg] = 0; digits = 0; }
		if (digits >= 16) return;
	}
	size_t l = strlen(C.in);
	if (l + 2 < sizeof C.in) { C.in[l] = (char)('0' + d); C.in[l + 1] = 0; }
}

static void do_dot(void) {
	fresh_if_done();
	if (!C.typing) {
		C.typing = true;
		C.have_val = false;
		str_copy(C.in, sizeof C.in, "0.");
		return;
	}
	if (strchr(C.in, '.') || strchr(C.in, 'e')) return;
	size_t l = strlen(C.in);
	if (l + 2 < sizeof C.in) { C.in[l] = '.'; C.in[l + 1] = 0; }
}

static void do_exp(void) {
	if (C.err) return;
	if (!C.typing) {
		double v = operand_value();
		fresh_if_done();
		char b[48];
		canon_num(v, b, sizeof b);
		if (strchr(b, 'e')) return;
		C.typing = true;
		C.have_val = false;
		str_copy(C.in, sizeof C.in, b);
	}
	if (strchr(C.in, 'e')) return;
	size_t l = strlen(C.in);
	if (l && C.in[l - 1] == '.') C.in[--l] = 0;
	if (l + 4 < sizeof C.in) strcat(C.in, "e+0");
}

static void do_backspace(void) {
	if (C.err) { clear_all(); return; }
	if (C.just_eval) { C.line[0] = 0; return; }
	if (!C.typing) return;
	size_t l = strlen(C.in);
	char *e = strchr(C.in, 'e');
	if (e && C.in + l - 1 == e + 2 && str_eq(e + 2, "0")) { *e = 0; return; }   /* "e+0" */
	if (e && C.in + l - 1 == e + 2) { e[2] = '0'; e[3] = 0; return; }
	if (l) C.in[--l] = 0;
	if (!l || str_eq(C.in, "-")) str_copy(C.in, sizeof C.in, "0");
}

static void do_clear_entry(void) {
	if (C.err || C.just_eval) { clear_all(); return; }
	C.typing = true;
	C.have_val = false;
	str_copy(C.in, sizeof C.in, "0");
}

static void do_negate(void) {
	if (C.err) return;
	if (C.typing) {
		char *e = strchr(C.in, 'e');
		if (e) { e[1] = e[1] == '+' ? '-' : '+'; return; }
		if (C.in[0] == '-') memmove(C.in, C.in + 1, strlen(C.in));
		else if (!str_eq(C.in, "0")) {
			size_t l = strlen(C.in);
			if (l + 2 < sizeof C.in) { memmove(C.in + 1, C.in, l + 1); C.in[0] = '-'; }
		}
		return;
	}
	double v = operand_value();
	char d[120], o[140];
	operand_disp(d, sizeof d);
	if (C.just_eval) { fresh_if_done(); canon_num(v, d, sizeof d); }
	snprintf(o, sizeof o, "negate(%s)", d);
	set_val(-v, o);
}

static void do_binop(int op) {
	if (C.err) return;
	if (C.just_eval) {
		double v = C.val;
		fresh_if_done();
		set_val(v, NULL);
	}
	if (!has_operand() && C.n && C.t[C.n - 1].kind == T_OP) {
		C.t[C.n - 1].op = op;       /* change the pending operator */
		rebuild_line();
		return;
	}
	if (!has_operand() && C.n && C.t[C.n - 1].kind == T_LP) {
		set_val(0, NULL);
	}
	if (!commit_operand()) return;
	double top;
	int e = sy_eval(C.t, C.n, g_sci, op_right(op) ? op_prec(op, g_sci) + 1 : op_prec(op, g_sci), &top);
	if (e) { rebuild_line(); set_error(e); return; }
	C.shown = top;
	push_tok((Tok){.kind = T_OP, .op = op});
	rebuild_line();
}

static void do_percent(void) {
	if (C.err) return;
	double b = operand_value();
	if (C.just_eval) { fresh_if_done(); }
	double r = b / 100;
	if (C.n && C.t[C.n - 1].kind == T_OP) {
		int op = C.t[C.n - 1].op;
		if (op == OP_ADD || op == OP_SUB) {
			double a;
			if (sy_eval(C.t, C.n - 1, g_sci, op_prec(op, g_sci), &a) == E_OK) r = a * b / 100;
		}
	}
	set_val(r, NULL);
}

static void do_lparen(void) {
	if (C.err) clear_all();
	fresh_if_done();
	if (has_operand()) {
		/* "2(" means "2 × (" */
		if (!commit_operand()) return;
		push_tok((Tok){.kind = T_OP, .op = OP_MUL});
	}
	if (open_parens() >= 25) return;
	push_tok((Tok){.kind = T_LP});
	C.shown = 0;
	rebuild_line();
}

/* Evaluates the innermost open group and turns it into the current operand. */
static bool close_group(void) {
	int k = -1;
	for (int i = C.n - 1; i >= 0; i--) if (C.t[i].kind == T_LP) { k = i; break; }
	if (k < 0) return false;
	if (has_operand() || (C.n > k + 1 && C.t[C.n - 1].kind == T_OP)) {
		if (!commit_operand()) return false;
	}
	if (C.n == k + 1) {
		/* "()" */
		C.n = k;
		set_val(0, NULL);
		rebuild_line();
		return true;
	}
	double r;
	int e = sy_eval(C.t + k + 1, C.n - k - 1, g_sci, 0, &r);
	if (e) { rebuild_line(); set_error(e); return false; }
	char inner[600] = "";
	for (int i = k + 1; i < C.n; i++) {
		const char *s = C.t[i].kind == T_NUM ? C.t[i].disp : C.t[i].kind == T_LP ? "(" : op_text(C.t[i].op);
		size_t l = strlen(inner);
		snprintf(inner + l, sizeof inner - l, "%s%s", (l && C.t[i - 1].kind != T_LP) ? " " : "", s);
	}
	char d[640];
	snprintf(d, sizeof d, "(%s)", inner);
	if (strlen(d) >= sizeof C.vdisp) canon_num(r, d, sizeof d);
	C.n = k;
	set_val(r, d);
	C.vgroup = true;
	rebuild_line();
	return true;
}

static void do_rparen(void) {
	if (C.err || !open_parens()) return;
	close_group();
}

static void do_equals(void) {
	if (C.err) { clear_all(); return; }
	if (C.just_eval) {
		if (C.last_op == OP_NONE) return;
		double a = C.val, r;
		char da[64], db[64];
		canon_num(a, da, sizeof da);
		canon_num(C.last_b, db, sizeof db);
		snprintf(C.line, sizeof C.line, "%s %s %s =", da, op_text(C.last_op), db);
		int e = apply_op(C.last_op, a, C.last_b, &r);
		if (e) { set_error(e); return; }
		char expr[200];
		snprintf(expr, sizeof expr, "%s %s %s", da, op_text(C.last_op), db);
		history_add(expr, r);
		set_val(r, NULL);
		return;
	}
	if (!C.n && !has_operand()) return;
	if (has_operand() || !C.n || C.t[C.n - 1].kind == T_OP) {
		if (!commit_operand()) return;
	}
	while (open_parens()) {
		if (!close_group()) return;
		if (!commit_operand()) return;
	}
	/* remember "op b" for repeated "=" */
	C.last_op = OP_NONE;
	if (C.n >= 3 && C.t[C.n - 2].kind == T_OP && C.t[C.n - 1].kind == T_NUM) {
		C.last_op = C.t[C.n - 2].op;
		C.last_b = C.t[C.n - 1].v;
		/* with precedence, repeating only makes sense for a plain "a op b" */
		if (g_sci && C.n != 3) C.last_op = OP_NONE;
	}
	double r;
	int e = sy_eval(C.t, C.n, g_sci, 0, &r);
	rebuild_line();
	char expr[600];
	str_copy(expr, sizeof expr, C.line);
	size_t l = strlen(C.line);
	snprintf(C.line + l, sizeof C.line - l, " =");
	if (e) { set_error(e); C.n = 0; return; }
	if (C.n > 1) history_add(expr, r);
	C.n = 0;
	set_val(r, NULL);
	C.just_eval = true;
	C.shown = r;
}

enum {
	F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_LN, F_EXP, F_LOG, F_POW10, F_SQR, F_CUBE,
	F_SQRT, F_CBRT, F_INV, F_FACT, F_ABS,
};

static double snap_tiny(double r) { return fabs(r) < 1e-15 ? 0 : r; }

static int fn_apply(int f, double x, double *r) {
	double rad = C.deg ? x * M_PI / 180 : x;
	switch (f) {
	case F_SIN: case F_COS: case F_TAN:
		if (C.deg) {
			double m = fmod(x, 360);
			if (m < 0) m += 360;
			/* exact values at multiples of 90 degrees */
			if (fmod(m, 90) == 0) {
				int q = (int)(m / 90);
				static const double S[] = {0, 1, 0, -1}, Co[] = {1, 0, -1, 0};
				if (f == F_SIN) { *r = S[q]; return E_OK; }
				if (f == F_COS) { *r = Co[q]; return E_OK; }
				if (q % 2) return E_INVALID;
				*r = 0;
				return E_OK;
			}
			rad = m * M_PI / 180;
		}
		if (fabs(x) > 1e15) return E_INVALID;
		*r = snap_tiny(f == F_SIN ? sin(rad) : f == F_COS ? cos(rad) : tan(rad));
		if (f == F_TAN && fabs(*r) > 1e15) return E_INVALID;
		break;
	case F_ASIN: case F_ACOS:
		if (x < -1 || x > 1) return E_INVALID;
		*r = f == F_ASIN ? asin(x) : acos(x);
		if (C.deg) *r = *r * 180 / M_PI;
		break;
	case F_ATAN:
		*r = atan(x);
		if (C.deg) *r = *r * 180 / M_PI;
		break;
	case F_LN: if (x <= 0) return E_INVALID; *r = log(x); break;
	case F_LOG: if (x <= 0) return E_INVALID; *r = log10(x); break;
	case F_EXP: *r = exp(x); break;
	case F_POW10: *r = pow(10, x); break;
	case F_SQR: *r = x * x; break;
	case F_CUBE: *r = x * x * x; break;
	case F_SQRT: if (x < 0) return E_INVALID; *r = sqrt(x); break;
	case F_CBRT: *r = cbrt(x); break;
	case F_INV: if (x == 0) return E_DIV0; *r = 1 / x; break;
	case F_FACT:
		if (x < 0 && x == floor(x)) return E_INVALID;
		if (x > 170.6) return E_OVERFLOW;
		if (x == floor(x)) { double p = 1; for (int i = 2; i <= (int)x; i++) p *= i; *r = p; }
		else *r = tgamma(x + 1);
		break;
	case F_ABS: *r = fabs(x); break;
	default: *r = x; break;
	}
	return check(*r);
}

static void do_func(int f) {
	if (C.err) return;
	static const char *names[] = {"sin", "cos", "tan", "asin", "acos", "atan", "ln", "e^", "log", "10^",
		"sqr", "cube", "√", "∛", "1/", "fact", "abs"};
	double x = operand_value();
	char d[120], o[160];
	if (C.just_eval) { fresh_if_done(); canon_num(x, d, sizeof d); }
	else operand_disp(d, sizeof d);
	double r = 0;
	int e = fn_apply(f, x, &r);
	if (C.vgroup && C.have_val) snprintf(o, sizeof o, "%s%s", names[f], d);
	else snprintf(o, sizeof o, "%s(%s)", names[f], d);
	if (strlen(o) >= sizeof C.vdisp) canon_num(r, o, sizeof o);
	if (e) {
		/* show what failed in the expression line */
		size_t l = strlen(C.line);
		if (C.n) snprintf(C.line + l, sizeof C.line - l, " %s", o);
		else str_copy(C.line, sizeof C.line, o);
		set_error(e);
		return;
	}
	set_val(r, o);
}

static void do_const(double v, const char *name) {
	if (C.err) clear_all();
	fresh_if_done();
	set_val(v, name);
}

static void do_mem(int which) {
	if (which == 0) { C.mem = 0; C.has_mem = false; save_soon(); return; }
	if (which == 1) {
		if (!C.has_mem) return;
		if (C.err) clear_all();
		fresh_if_done();
		set_val(C.mem, NULL);
		return;
	}
	if (C.err) return;
	double v = operand_value();
	C.mem += which == 2 ? v : -v;
	if (fabs(C.mem) < 1e-300) C.mem = 0;
	C.has_mem = true;
	if (C.typing) set_val(v, NULL);
	save_soon();
}

/* ------------------------------------------------------------ persistence */

static void save_now(void *ud) {
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "memory", C.mem);
	cJSON_AddBoolToObject(root, "has_memory", C.has_mem);
	cJSON_AddBoolToObject(root, "degrees", C.deg);
	cJSON_AddBoolToObject(root, "scientific", C.sci_portrait);
	cJSON *h = cJSON_AddArrayToObject(root, "history");
	for (int i = 0; i < C.hist.len; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "expr", C.hist.items[i].expr);
		cJSON_AddNumberToObject(o, "result", C.hist.items[i].result);
		cJSON_AddItemToArray(h, o);
	}
	app_json_save("calculator.json", root);
	cJSON_Delete(root);
}

static void save_soon(void) {
	if (!g_save_timer) g_save_timer = timer_add(0, false, save_now, NULL);
	timer_set(g_save_timer, 1500);
}

static void load(void) {
	if (C.loaded) return;
	C.loaded = true;
	cJSON *root = app_json_load("calculator.json");
	if (!root) return;
	cJSON *j;
	if (cJSON_IsNumber(j = cJSON_GetObjectItem(root, "memory"))) C.mem = j->valuedouble;
	C.has_mem = cJSON_IsTrue(cJSON_GetObjectItem(root, "has_memory"));
	if ((j = cJSON_GetObjectItem(root, "degrees"))) C.deg = cJSON_IsTrue(j);
	C.sci_portrait = cJSON_IsTrue(cJSON_GetObjectItem(root, "scientific"));
	cJSON *h = cJSON_GetObjectItem(root, "history"), *it;
	cJSON_ArrayForEach(it, h) {
		cJSON *e = cJSON_GetObjectItem(it, "expr"), *r = cJSON_GetObjectItem(it, "result");
		if (!cJSON_IsString(e) || !cJSON_IsNumber(r) || C.hist.len >= MAX_HIST) continue;
		vec_push(&C.hist, ((HistItem){xstrdup(e->valuestring), r->valuedouble}));
	}
	cJSON_Delete(root);
}

static void history_add(const char *expr, double r) {
	if (C.hist.len >= MAX_HIST) {
		free(C.hist.items[C.hist.len - 1].expr);
		C.hist.len--;
	}
	vec_push(&C.hist, ((HistItem){NULL, 0}));
	memmove(&C.hist.items[1], &C.hist.items[0], (size_t)(C.hist.len - 1) * sizeof(HistItem));
	C.hist.items[0] = (HistItem){xstrdup(expr), r};
	save_soon();
}

static void history_clear(void) {
	for (int i = 0; i < C.hist.len; i++) free(C.hist.items[i].expr);
	C.hist.len = 0;
	save_soon();
}

/* ------------------------------------------------------------------ keys -- */

enum {
	K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_DOT, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ,
	K_C, K_CE, K_BS, K_NEG, K_PCT, K_MC, K_MR, K_MP, K_MM, K_2ND, K_DEG, K_LP, K_RP,
	K_SIN, K_COS, K_TAN, K_LN, K_LOG, K_SQR, K_POW, K_SQRT, K_ROOT, K_INV, K_FACT, K_PI, K_E,
	K_EXP, K_MOD, K_ABS, K_COUNT
};

enum { KS_DIGIT, KS_OP, KS_FN, KS_EQ, KS_MEM };

static void press(int k) {
	C.flash_key = k;
	C.flash_t = now_sec();
	if (k <= K_9) { do_digit(k); return; }
	switch (k) {
	case K_DOT: do_dot(); break;
	case K_ADD: do_binop(OP_ADD); break;
	case K_SUB: do_binop(OP_SUB); break;
	case K_MUL: do_binop(OP_MUL); break;
	case K_DIV: do_binop(OP_DIV); break;
	case K_POW: do_binop(OP_POW); break;
	case K_ROOT: do_binop(OP_ROOT); break;
	case K_MOD: do_binop(OP_MOD); break;
	case K_EQ: do_equals(); break;
	case K_C: clear_all(); break;
	case K_CE: do_clear_entry(); break;
	case K_BS: do_backspace(); break;
	case K_NEG: do_negate(); break;
	case K_PCT: do_percent(); break;
	case K_MC: do_mem(0); break;
	case K_MR: do_mem(1); break;
	case K_MP: do_mem(2); break;
	case K_MM: do_mem(3); break;
	case K_2ND: C.second = !C.second; break;
	case K_DEG: C.deg = !C.deg; save_soon(); break;
	case K_LP: do_lparen(); break;
	case K_RP: do_rparen(); break;
	case K_SIN: do_func(C.second ? F_ASIN : F_SIN); break;
	case K_COS: do_func(C.second ? F_ACOS : F_COS); break;
	case K_TAN: do_func(C.second ? F_ATAN : F_TAN); break;
	case K_LN: do_func(C.second ? F_EXP : F_LN); break;
	case K_LOG: do_func(C.second ? F_POW10 : F_LOG); break;
	case K_SQR: do_func(C.second ? F_CUBE : F_SQR); break;
	case K_SQRT: do_func(C.second ? F_CBRT : F_SQRT); break;
	case K_INV: do_func(F_INV); break;
	case K_FACT: do_func(F_FACT); break;
	case K_ABS: do_func(F_ABS); break;
	case K_PI: do_const(M_PI, "π"); break;
	case K_E: do_const(M_E, "e"); break;
	case K_EXP: do_exp(); break;
	default: break;
	}
}

/* Key label: base text with an optional superscript after (sup) or before (pre). */
typedef struct { const char *base, *sup, *pre; } Label;

static Label key_label(int k) {
	bool s = C.second;
	switch (k) {
	case K_DOT: return (Label){i18n_lang() == LANG_TR ? "," : ".", NULL, NULL};
	case K_ADD: return (Label){"+", NULL, NULL};
	case K_SUB: return (Label){"–", NULL, NULL};
	case K_MUL: return (Label){"×", NULL, NULL};
	case K_DIV: return (Label){"÷", NULL, NULL};
	case K_EQ: return (Label){"=", NULL, NULL};
	case K_C: return (Label){"C", NULL, NULL};
	case K_CE: return (Label){"CE", NULL, NULL};
	case K_BS: return (Label){"", NULL, NULL};
	case K_NEG: return (Label){"±", NULL, NULL};
	case K_PCT: return (Label){"%", NULL, NULL};
	case K_MC: return (Label){"MC", NULL, NULL};
	case K_MR: return (Label){"MR", NULL, NULL};
	case K_MP: return (Label){"M+", NULL, NULL};
	case K_MM: return (Label){"M–", NULL, NULL};
	case K_2ND: return (Label){"2", "nd", NULL};
	case K_DEG: return (Label){C.deg ? "deg" : "rad", NULL, NULL};
	case K_LP: return (Label){"(", NULL, NULL};
	case K_RP: return (Label){")", NULL, NULL};
	case K_SIN: return (Label){"sin", s ? "−1" : NULL, NULL};
	case K_COS: return (Label){"cos", s ? "−1" : NULL, NULL};
	case K_TAN: return (Label){"tan", s ? "−1" : NULL, NULL};
	case K_LN: return s ? (Label){"e", "x", NULL} : (Label){"ln", NULL, NULL};
	case K_LOG: return s ? (Label){"10", "x", NULL} : (Label){"log", NULL, NULL};
	case K_SQR: return (Label){"x", s ? "3" : "2", NULL};
	case K_POW: return (Label){"x", "y", NULL};
	case K_SQRT: return (Label){"√", NULL, s ? "3" : NULL};
	case K_ROOT: return (Label){"√x", NULL, "y"};
	case K_INV: return (Label){"1/x", NULL, NULL};
	case K_FACT: return (Label){"n!", NULL, NULL};
	case K_PI: return (Label){"π", NULL, NULL};
	case K_E: return (Label){"e", NULL, NULL};
	case K_EXP: return (Label){"exp", NULL, NULL};
	case K_MOD: return (Label){"mod", NULL, NULL};
	case K_ABS: return (Label){"|x|", NULL, NULL};
	default: break;
	}
	static char d[2];
	d[0] = (char)('0' + k);
	d[1] = 0;
	return (Label){d, NULL, NULL};
}

static int key_style(int k) {
	if (k <= K_9 || k == K_DOT || k == K_NEG) return KS_DIGIT;
	if (k == K_EQ) return KS_EQ;
	if (k == K_ADD || k == K_SUB || k == K_MUL || k == K_DIV || k == K_C || k == K_BS || k == K_PCT) return KS_OP;
	if (k >= K_MC && k <= K_MM) return KS_MEM;
	return KS_FN;
}

static bool key_enabled(int k) {
	if ((k == K_MR || k == K_MC) && !C.has_mem) return false;
	if (C.err) {
		/* after an error only entry keys work */
		return k <= K_9 || k == K_DOT || k == K_C || k == K_CE || k == K_BS || k == K_EQ || k == K_PI ||
		       k == K_E || k == K_MC || k == K_MR || k == K_DEG || k == K_2ND || k == K_LP;
	}
	if (k == K_RP && !open_parens()) return false;
	return true;
}

static void draw_label(cairo_t *cr, Label l, double cx, double cy, double size, Color c) {
	double ss = size * 0.6;
	Size bs = gfx_text_size(cr, l.base, size, FW_SEMILIGHT, 0, 1);
	Size ps = l.sup ? gfx_text_size(cr, l.sup, ss, FW_REGULAR, 0, 1) : (Size){0, 0};
	Size qs = l.pre ? gfx_text_size(cr, l.pre, ss, FW_REGULAR, 0, 1) : (Size){0, 0};
	double total = bs.w + ps.w + qs.w;
	double x = cx - total / 2, y = cy - bs.h / 2;
	if (l.pre) gfx_text(cr, l.pre, x + 2, y - size * 0.12, ss, FW_REGULAR, c, TA_LEFT, 0, 1);
	x += qs.w;
	gfx_text(cr, l.base, x, y, size, FW_SEMILIGHT, c, TA_LEFT, 0, 1);
	x += bs.w;
	if (l.sup) gfx_text(cr, l.sup, x, y - size * 0.12, ss, FW_REGULAR, c, TA_LEFT, 0, 1);
}

static void draw_key(UI *ui, int k, Rect r, double fsize) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	UiId id = ui_idn("calc-key", k);
	bool en = key_enabled(k);
	if (en) ui_hit(ui, id, r);
	bool pr = en && ui_pressed(ui, id);
	double ft = ui->now - C.flash_t;
	bool flash = C.flash_key == k && ft >= 0 && ft < 0.14;
	if (flash) ui->animating = true;
	int st = key_style(k);
	Color bg;
	switch (st) {
	case KS_DIGIT: bg = col_mix(th->bg, th->fg, th->dark ? 0.2f : 0.12f); break;
	case KS_OP: bg = col_mix(th->bg, th->fg, th->dark ? 0.13f : 0.21f); break;
	case KS_EQ: bg = th->accent; break;
	default: bg = col_mix(th->bg, th->fg, th->dark ? 0.09f : 0.06f); break;
	}
	if (k == K_2ND && C.second) bg = col_mix(bg, th->accent, 0.7f);
	Color fg = st == KS_EQ ? th->on_accent : (k == K_2ND && C.second) ? th->on_accent : th->fg;
	if (pr || flash) {
		bg = st == KS_EQ ? th->fg : th->accent;
		fg = st == KS_EQ ? th->bg : th->on_accent;
	}
	if (!en) fg = col_alpha(fg, 0.35f);
	gfx_fill_rect(cr, r, bg);
	if (k == K_BS) icon_draw(cr, ICON_BACKSPACE, rect_cx(r), rect_cy(r), fsize * 1.05, fg);
	else draw_label(cr, key_label(k), rect_cx(r), rect_cy(r), fsize, fg);
	if (en && ui_clicked(ui, id)) {
		press(k);
		C.flash_t = -1;     /* touch already showed the pressed state */
		shell_invalidate();
	}
}

static void draw_grid(UI *ui, const int *keys, int cols, int rows, Rect r, double gap, double fsize) {
	double kw = (r.w - gap * (cols - 1)) / cols, kh = (r.h - gap * (rows - 1)) / rows;
	for (int i = 0; i < cols * rows; i++) {
		if (keys[i] < 0) continue;
		int row = i / cols, col = i % cols;
		Rect kr = R(r.x + col * (kw + gap), r.y + row * (kh + gap), kw, kh);
		/* snap to whole dp so the gaps look even */
		double x0 = round(kr.x), y0 = round(kr.y), x1 = round(kr.x + kr.w), y1 = round(kr.y + kr.h);
		draw_key(ui, keys[i], R(x0, y0, x1 - x0, y1 - y0), fsize);
	}
}

static const int STD_KEYS[] = {
	K_C, K_BS, K_PCT, K_DIV,
	K_7, K_8, K_9, K_MUL,
	K_4, K_5, K_6, K_SUB,
	K_1, K_2, K_3, K_ADD,
	K_NEG, K_0, K_DOT, K_EQ,
};
static const int MEM_KEYS[] = {K_MC, K_MR, K_MP, K_MM};
static const int SCI_KEYS[] = {
	K_2ND, K_DEG, K_LP, K_RP, K_MC,
	K_SIN, K_COS, K_TAN, K_PI, K_MR,
	K_LN, K_LOG, K_E, K_EXP, K_MP,
	K_SQR, K_POW, K_SQRT, K_ROOT, K_MM,
	K_INV, K_FACT, K_MOD, K_ABS, K_CE,
};

static void keyboard_input(UI *ui) {
	uint32_t cp;
	while (ui_char(ui, &cp)) {
		int k = -1;
		if (cp >= '0' && cp <= '9') k = K_0 + (int)(cp - '0');
		else switch (cp) {
		case '.': case ',': k = K_DOT; break;
		case '+': k = K_ADD; break;
		case '-': k = K_SUB; break;
		case '*': case 'x': case 'X': k = K_MUL; break;
		case '/': k = K_DIV; break;
		case '=': k = K_EQ; break;
		case '%': k = K_PCT; break;
		case '^': k = g_sci ? K_POW : -1; break;
		case '(': k = g_sci ? K_LP : -1; break;
		case ')': k = g_sci ? K_RP : -1; break;
		case '!': k = g_sci ? K_FACT : -1; break;
		case 'r': k = g_sci ? K_INV : -1; break;
		case '@': k = g_sci ? K_SQRT : -1; break;
		case 'q': k = g_sci ? K_SQR : -1; break;
		case 'p': k = g_sci ? K_PI : -1; break;
		case 's': k = g_sci ? K_SIN : -1; break;
		case 'o': k = g_sci ? K_COS : -1; break;
		case 't': k = g_sci ? K_TAN : -1; break;
		case 'n': k = g_sci ? K_LN : -1; break;
		case 'l': k = g_sci ? K_LOG : -1; break;
		case 'e': k = g_sci ? K_EXP : -1; break;
		case 'm': k = g_sci ? K_MOD : -1; break;
		default: break;
		}
		if (k >= 0 && key_enabled(k)) press(k);
	}
	if (ui_key(ui, UK_ENTER)) press(K_EQ);
	if (ui_key(ui, UK_DELETE)) press(K_C);
}

/* Draws `s` right aligned at (right, y); long text keeps its end visible. */
static void text_tail(cairo_t *cr, const char *s, double right, double y, double size, Color c, double max_w) {
	const char *p = s;
	char buf[700];
	while (*p && gfx_text_size(cr, p, size, FW_REGULAR, 0, 1).w > max_w - 14) {
		uint32_t cp;
		p += utf8_decode(p, &cp);
		while (*p == ' ') p++;
	}
	if (p != s) snprintf(buf, sizeof buf, "…%s", p);
	else str_copy(buf, sizeof buf, s);
	gfx_text(cr, buf, right, y, size, FW_REGULAR, c, TA_RIGHT, 0, 1);
}

static void draw_display(UI *ui, Rect r) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double pad = 18;
	char loc[700];
	/* expression line, including a pending function / group operand like "sin(30)" */
	char line[800];
	str_copy(line, sizeof line, C.line);
	if (C.have_val && !C.just_eval && !C.err) {
		char cn[64];
		canon_num(C.val, cn, sizeof cn);
		if (strcmp(cn, C.vdisp)) {
			size_t l = strlen(line);
			snprintf(line + l, sizeof line - l, "%s%s", l && line[l - 1] != '(' ? " " : "", C.vdisp);
		}
	}
	localize(line, loc, sizeof loc);
	double ly = r.y + 6;
	text_tail(cr, loc, rect_right(r) - pad, ly, 20, th->sub, r.w - 2 * pad - 30);
	/* indicators */
	double ix = r.x + pad;
	if (C.has_mem) {
		gfx_text(cr, "M", ix, ly, 17, FW_SEMIBOLD, th->sub, TA_LEFT, 0, 1);
		ix += 22;
	}
	int op = open_parens();
	if (op && g_sci) {
		char b[16];
		snprintf(b, sizeof b, "(%d", op);
		gfx_text(cr, b, ix, ly, 17, FW_SEMIBOLD, th->sub, TA_LEFT, 0, 1);
		ix += 30;
	}
	if (g_sci) gfx_text(cr, C.deg ? "DEG" : "RAD", ix, ly, 13, FW_SEMIBOLD, th->dim, TA_LEFT, 0, 1);

	/* main number */
	double avail = r.w - 2 * pad;
	double top = ly + 30, h = rect_bottom(r) - top;
	if (C.err) {
		double sz = MIN(40, h * 0.5);
		Size ts = gfx_text_size(cr, err_text(C.err), sz, FW_LIGHT, avail, 2);
		gfx_text(cr, err_text(C.err), rect_right(r) - pad, rect_bottom(r) - ts.h - 6, sz, FW_LIGHT, th->fg, TA_RIGHT, avail, 2);
		return;
	}
	char canon[120];
	if (C.typing) str_copy(canon, sizeof canon, C.in);
	else canon_num(C.have_val ? C.val : C.shown, canon, sizeof canon);
	localize(canon, loc, sizeof loc);
	double sz = MIN(84, h * 0.82);
	Size ts = gfx_text_size(cr, loc, sz, FW_LIGHT, 0, 1);
	if (ts.w > avail) { sz = MAX(20, sz * avail / ts.w); ts = gfx_text_size(cr, loc, sz, FW_LIGHT, 0, 1); }
	gfx_text(cr, loc, rect_right(r) - pad + sz * 0.04, rect_bottom(r) - ts.h, sz, FW_LIGHT, th->fg, TA_RIGHT, avail, 1);
}

static void calc_page(UI *ui, Rect area) {
	bool landscape = area.w > area.h;
	g_sci = landscape || C.sci_portrait;
	g_bar.minimized = true;
	Rect a = appbar_content(&g_bar, area);
	double gap = 3;
	Rect pad = R(a.x + gap, a.y, a.w - 2 * gap, a.h - gap);
	if (!dialog_active() && !nav_busy(&g_nav, ui)) keyboard_input(ui);

	if (landscape) {
		int rows = 5;
		double kh = MIN(82, (pad.h - 100) / rows);
		double kb_h = kh * rows + gap * (rows - 1);
		Rect disp = R(a.x, a.y, a.w, pad.h - kb_h - gap);
		draw_display(ui, disp);
		double sci_w = (pad.w - gap) * 5 / 9;
		Rect left = R(pad.x, rect_bottom(pad) - kb_h, sci_w, kb_h);
		Rect right = R(pad.x + sci_w + gap, left.y, pad.w - sci_w - gap, kb_h);
		draw_grid(ui, SCI_KEYS, 5, 5, left, gap, MIN(22, kh * 0.38));
		draw_grid(ui, STD_KEYS, 4, 5, right, gap, MIN(30, kh * 0.5));
	} else if (g_sci) {
		double kh = MIN(70, (pad.h - 150 - 9 * gap) / 10);
		double sci_h = kh * 5 * 0.82 + gap * 4, std_h = kh * 5 * 1.18 + gap * 4;
		Rect disp = R(a.x, a.y, a.w, pad.h - sci_h - std_h - gap * 2);
		draw_display(ui, disp);
		Rect sr = R(pad.x, rect_bottom(disp) + gap, pad.w, sci_h);
		Rect kr = R(pad.x, rect_bottom(sr) + gap, pad.w, std_h);
		draw_grid(ui, SCI_KEYS, 5, 5, sr, gap, MIN(20, kh * 0.82 * 0.42));
		draw_grid(ui, STD_KEYS, 4, 5, kr, gap, MIN(30, kh * 1.18 * 0.42));
	} else {
		double kw = (pad.w - 3 * gap) / 4;
		double kh = MIN(kw * 0.86, (pad.h - 170 - 6 * gap) / 5.55);
		double mem_h = kh * 0.55;
		double kb_h = kh * 5 + gap * 4;
		Rect disp = R(a.x, a.y, a.w, pad.h - kb_h - mem_h - gap * 2);
		draw_display(ui, disp);
		Rect mr = R(pad.x, rect_bottom(disp) + gap, pad.w, mem_h);
		draw_grid(ui, MEM_KEYS, 4, 1, mr, gap, 17);
		Rect kr = R(pad.x, rect_bottom(mr) + gap, pad.w, kb_h);
		draw_grid(ui, STD_KEYS, 4, 5, kr, gap, MIN(32, kh * 0.4));
	}

	AppBarBtn btns[] = {
		{ICON_HISTORY, _("history"), false},
		{ICON_CALC, C.sci_portrait ? _("standard") : _("scientific"), landscape},
	};
	const char *menu[] = {_("clear history"), _("clear memory")};
	int r = appbar_draw(ui, &g_bar, area, btns, 2, menu, 2);
	if (r == 0) { g_hscroll = (Scroll){0}; nav_push(&g_nav, ui, 1); }
	else if (r == 1) {
		C.sci_portrait = !C.sci_portrait;
		C.second = false;
		save_soon();
	} else if (r == APPBAR_MENU) {
		history_clear();
	} else if (r == APPBAR_MENU + 1) {
		do_mem(0);
	}
}

/* --------------------------------------------------------------- history -- */

static int g_hist_menu_i = -1;

static void hist_menu_cb(int b, void *ud) {
	int i = g_hist_menu_i;
	if (i < 0 || i >= C.hist.len) return;
	if (b == 0) {
		fresh_if_done();
		set_val(C.hist.items[i].result, NULL);
		nav_pop(&g_nav, &sh.ui);
	} else if (b == 1) {
		free(C.hist.items[i].expr);
		vec_remove(&C.hist, i);
		save_soon();
	}
	shell_invalidate();
}

static void clear_cb(int b, void *ud) {
	if (b == 0) history_clear();
	shell_invalidate();
}

static void history_page(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	Rect a = appbar_content(&g_hbar, area);
	double y = ui_page_header(ui, 14, a.y + 6, _("Calculator"), _("history"));
	Rect view = R(a.x, y + 4, a.w, rect_bottom(a) - y - 4);
	if (!C.hist.len) {
		app_empty(ui, R(a.x, view.y - 14, a.w, 100), _("there's no history yet. your calculations show up here."));
	} else {
		const double RH = 92;
		double off = ui_scroll_begin(ui, ui_id("calc-hist"), &g_hscroll, view, C.hist.len * RH + 20);
		cairo_translate(cr, view.x, view.y);
		for (int i = 0; i < C.hist.len; i++) {
			double ry = i * RH;
			if (ry + RH < off || ry > off + view.h) continue;
			Rect rr = R(0, ry, view.w, RH);
			UiId id = ui_idn("calc-hrow", i);
			ui_hit(ui, id, rr);
			if (ui_pressed(ui, id)) gfx_fill_rect(cr, rr, col_alpha(th->fg, 0.14f));
			char loc[700], canon[64];
			localize(C.hist.items[i].expr, loc, sizeof loc);
			strncat(loc, " =", sizeof loc - strlen(loc) - 1);
			gfx_text(cr, loc, 24, ry + 10, 17, FW_REGULAR, th->sub, TA_LEFT, view.w - 48, 1);
			canon_num(C.hist.items[i].result, canon, sizeof canon);
			localize(canon, loc, sizeof loc);
			gfx_text(cr, loc, 24, ry + 32, 40, FW_LIGHT, th->fg, TA_LEFT, view.w - 48, 1);
			if (ui_clicked(ui, id)) {
				fresh_if_done();
				set_val(C.hist.items[i].result, NULL);
				nav_pop(&g_nav, ui);
			}
			if (ui_longpressed(ui, id)) {
				static const char *items[2];
				items[0] = _("use result");
				items[1] = _("delete");
				g_hist_menu_i = i;
				dialog_menu(ui->p.x, ui->p.y, items, 2, hist_menu_cb, NULL);
			}
		}
		ui_scroll_end(ui);
	}
	AppBarBtn btns[] = {{ICON_DELETE, _("clear"), C.hist.len == 0}};
	if (appbar_draw(ui, &g_hbar, area, btns, 1, NULL, 0) == 0)
		dialog_message(_("clear history?"), _("all your previous calculations will be deleted."), _("clear"),
		               _("cancel"), clear_cb, NULL);
}

static void page_fn(UI *ui, int page, Rect area, void *ud) {
	if (page == 1) history_page(ui, area);
	else calc_page(ui, area);
}

/* ---------------------------------------------------------------- AppDef -- */

static void calculator_open(const char *arg) {
	load();
}

static void calculator_draw(UI *ui, Rect area) {
	nav_draw(ui, &g_nav, area, page_fn, NULL);
}

static bool calculator_back(void) {
	if (appbar_back(nav_page(&g_nav) == 1 ? &g_hbar : &g_bar)) return true;
	if (nav_pop(&g_nav, &sh.ui)) return true;
	return false;
}

static void calculator_close(void) {
	if (g_save_timer) { timer_set(g_save_timer, 0); save_now(NULL); }
	clear_all();
	C.second = false;
	nav_init(&g_nav, 0);
	g_bar = (AppBar){0};
	g_hbar = (AppBar){0};
}

const AppDef app_calculator = {
	.id = "calculator", .name = N_("Calculator"), .icon = ICON_CALC, .color = 0,
	.keywords = "calc math scientific",
	.open = calculator_open, .draw = calculator_draw, .back = calculator_back, .close = calculator_close,
};

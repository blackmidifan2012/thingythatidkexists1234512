/* Phone: a dialer for ModemManager modems, driven through mmcli. Places outgoing
 * calls, shows the call state and duration, and sends touch tones during a call.
 * Incoming calls are not handled yet. */
#include "apps.h"
#include "../config.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_num[40];
static int g_modem = -2;          /* -2 not checked yet, -1 none, else the modem index */
static int g_call = -1;           /* ModemManager call index while a call is up */
static char g_state[24];          /* mmcli call state: dialing, ringing-out, active... */
static char g_status[96];         /* message under the number when not in a call */
static double g_active_t0;        /* when the call was answered */
static bool g_polling;
static Timer *g_poll;

static void set_status(const char *s) {
	str_copy(g_status, sizeof g_status, s);
	shell_invalidate();
}

static void modem_done(int status, char *out, size_t len, void *ud) {
	(void)len; (void)ud;
	const char *m = status == 0 && out ? strstr(out, "/Modem/") : NULL;
	g_modem = m ? atoi(m + 7) : -1;
	set_status(g_modem >= 0 ? "" : _("No modem found. Calls need a working modem and ModemManager."));
}

static void modem_check(void) {
	if (!proc_exists("mmcli")) {
		g_modem = -1;
		set_status(_("ModemManager (mmcli) is not installed, so calls can't be made."));
		return;
	}
	char *argv[] = {"mmcli", "-L", NULL};
	proc_run(argv, modem_done, NULL);
}

static void call_finished(void) {
	if (g_call < 0) return;
	char m[16], del[48];
	snprintf(m, sizeof m, "%d", g_modem);
	snprintf(del, sizeof del, "--voice-delete-call=%d", g_call);
	char *argv[] = {"mmcli", "-m", m, del, NULL};
	proc_run(argv, NULL, NULL);
	g_call = -1;
	g_state[0] = 0;
	if (g_poll) timer_set(g_poll, 0);
	set_status(_("call ended"));
}

static void poll_done(int status, char *out, size_t len, void *ud) {
	(void)len; (void)ud;
	g_polling = false;
	if (g_call < 0) return;
	const char *s = status == 0 && out ? strstr(out, "state:") : NULL;
	if (!s) { call_finished(); return; }
	s += 6;
	while (*s == ' ') s++;
	size_t n = 0;
	while (s[n] && s[n] != ' ' && s[n] != '\n' && n + 1 < sizeof g_state) n++;
	memcpy(g_state, s, n);
	g_state[n] = 0;
	if (str_eq(g_state, "terminated")) { call_finished(); return; }
	if (str_eq(g_state, "active") && g_active_t0 == 0) g_active_t0 = now_sec();
	shell_invalidate();
}

static void poll_tick(void *ud) {
	(void)ud;
	if (g_call < 0 || g_polling) return;
	char o[16];
	snprintf(o, sizeof o, "%d", g_call);
	char *argv[] = {"mmcli", "-o", o, NULL};
	g_polling = proc_run(argv, poll_done, NULL);
	shell_invalidate();   /* also ticks the call duration */
}

static void created_done(int status, char *out, size_t len, void *ud) {
	(void)len; (void)ud;
	const char *c = status == 0 && out ? strstr(out, "/Call/") : NULL;
	if (!c) { set_status(_("The call could not be started.")); return; }
	g_call = atoi(c + 6);
	g_active_t0 = 0;
	str_copy(g_state, sizeof g_state, "dialing");
	char o[16];
	snprintf(o, sizeof o, "%d", g_call);
	char *argv[] = {"mmcli", "-o", o, "--start", NULL};
	proc_run(argv, NULL, NULL);
	if (!g_poll) g_poll = timer_add(0, true, poll_tick, NULL);
	timer_set(g_poll, 1000);
	shell_invalidate();
}

static void call_start(void) {
	if (!g_num[0] || g_call >= 0) return;
	if (g_modem < 0) {
		shell_toast(_("Phone"), g_status[0] ? g_status : _("No modem found."), _("Phone"));
		return;
	}
	char m[16], arg[80];
	snprintf(m, sizeof m, "%d", g_modem);
	snprintf(arg, sizeof arg, "--voice-create-call=number=%s", g_num);   /* g_num holds only 0-9 * # + */
	char *argv[] = {"mmcli", "-m", m, arg, NULL};
	set_status(_("calling…"));
	if (!proc_run(argv, created_done, NULL)) set_status(_("The call could not be started."));
}

static void call_hangup(void) {
	if (g_call < 0) return;
	char o[16];
	snprintf(o, sizeof o, "%d", g_call);
	char *argv[] = {"mmcli", "-o", o, "--hangup", NULL};
	proc_run(argv, NULL, NULL);
	str_copy(g_state, sizeof g_state, "ending");
	shell_invalidate();
}

static void key_pressed(const char *k) {
	app_play_event(EV_KEY);
	if (g_call >= 0) {
		/* touch tones for menus ("press 1 for...") */
		char o[16], arg[24];
		snprintf(o, sizeof o, "%d", g_call);
		snprintf(arg, sizeof arg, "--send-dtmf=%s", k);
		char *argv[] = {"mmcli", "-o", o, arg, NULL};
		proc_run(argv, NULL, NULL);
	}
	size_t n = strlen(g_num);
	if (n + 1 < sizeof g_num) { g_num[n] = k[0]; g_num[n + 1] = 0; }
	if (g_modem >= 0) g_status[0] = 0;   /* keep "no modem" explanations visible */
}

static const char *state_label(void) {
	if (str_eq(g_state, "active")) return _("on call");
	if (str_eq(g_state, "ringing-out")) return _("ringing…");
	if (str_eq(g_state, "held")) return _("on hold");
	if (str_eq(g_state, "ending")) return _("ending call…");
	return _("calling…");
}

static void phone_open(const char *arg) {
	if (arg && *arg && g_call < 0) {
		if (str_starts(arg, "tel:")) arg += 4;
		size_t n = 0;
		for (const char *p = arg; *p && n + 1 < sizeof g_num; p++)
			if ((*p >= '0' && *p <= '9') || *p == '+' || *p == '*' || *p == '#') g_num[n++] = *p;
		g_num[n] = 0;
	}
	if (g_modem == -2) modem_check();
}

static void phone_draw(UI *ui, Rect area) {
	cairo_t *cr = ui->cr;
	const Theme *th = &ui->th;
	double y = area.y + 8;
	y += gfx_text(cr, i18n_upper(_("Phone")), area.x + 18, y, 17, FW_SEMIBOLD, th->fg, TA_LEFT, area.w - 36, 1) + 10;

	/* number, with backspace while dialling */
	double nh = 64;
	Rect nr = R(area.x + 12, y, area.w - 24, nh);
	double size = strlen(g_num) > 13 ? 30 : 42;
	gfx_text_in(cr, g_num[0] ? g_num : "", R(nr.x + 44, nr.y, nr.w - 88, nr.h), size, FW_LIGHT, th->fg, TA_CENTER, true);
	if (g_num[0] && g_call < 0) {
		Rect br = R(rect_right(nr) - 44, nr.y, 44, nh);
		UiId bid = ui_id("ph-bksp");
		ui_hit(ui, bid, br);
		icon_draw(cr, ICON_BACKSPACE, rect_cx(br), rect_cy(br), 24, ui_pressed(ui, bid) ? th->accent : th->sub);
		if (ui_longpressed(ui, bid)) g_num[0] = 0;
		else if (ui_clicked(ui, bid)) g_num[strlen(g_num) - 1] = 0;
	}
	y += nh;

	/* status line: call progress and duration, or why calls are unavailable */
	char line[128] = "";
	if (g_call >= 0) {
		if (g_active_t0 > 0) {
			int s = (int)(now_sec() - g_active_t0);
			snprintf(line, sizeof line, "%s  %d:%02d", state_label(), s / 60, s % 60);
		} else str_copy(line, sizeof line, state_label());
	} else str_copy(line, sizeof line, g_status);
	y += gfx_text(cr, line, area.x + 18, y, 15, FW_REGULAR, g_call >= 0 ? th->accent : th->sub, TA_CENTER, area.w - 36, 2)
	     + 12;

	/* keypad */
	static const char *K[12][2] = {
		{"1", ""}, {"2", "ABC"}, {"3", "DEF"}, {"4", "GHI"}, {"5", "JKL"}, {"6", "MNO"},
		{"7", "PQRS"}, {"8", "TUV"}, {"9", "WXYZ"}, {"*", ""}, {"0", "+"}, {"#", ""},
	};
	double gap = 6, call_h = 56;
	double kw = (area.w - 24 - 2 * gap) / 3;
	double kh = MIN(76, (rect_bottom(area) - y - call_h - 18 - 3 * gap) / 4);
	Color kbg = th->dark ? RGBX(0x2b2b2b) : RGBX(0xe6e6e6);
	for (int i = 0; i < 12; i++) {
		Rect kr = R(area.x + 12 + (i % 3) * (kw + gap), y + (i / 3) * (kh + gap), kw, kh);
		UiId id = ui_idn("ph-key", i);
		ui_hit(ui, id, kr);
		bool pr = ui_pressed(ui, id);
		gfx_fill_rect(cr, kr, pr ? th->accent : kbg);
		Color fg = pr ? th->on_accent : th->fg;
		double cy = K[i][1][0] ? rect_cy(kr) - 8 : rect_cy(kr);
		gfx_text_in(cr, K[i][0], R(kr.x, cy - 20, kr.w, 40), 32, FW_LIGHT, fg, TA_CENTER, true);
		if (K[i][1][0]) gfx_text_in(cr, K[i][1], R(kr.x, cy + 14, kr.w, 18), 12, FW_REGULAR, pr ? fg : th->sub, TA_CENTER, true);
		/* hold 0 for "+" (international prefix) */
		if (i == 10 && g_call < 0 && ui_longpressed(ui, id)) key_pressed("+");
		else if (ui_clicked(ui, id)) key_pressed(K[i][0]);
	}
	y += 4 * kh + 3 * gap + 12;

	/* call / end call */
	Rect cbt = R(area.x + 12, y, area.w - 24, call_h);
	UiId cid = ui_id("ph-call");
	ui_hit(ui, cid, cbt);
	bool in_call = g_call >= 0;
	Color cbg = in_call ? RGBX(0xE51400) : th->accent;
	gfx_fill_rect(cr, cbt, ui_pressed(ui, cid) ? col_mix(cbg, COL_BLACK, 0.3f) : cbg);
	icon_draw(cr, ICON_PHONE, cbt.x + cbt.w / 2 - 50, rect_cy(cbt), 22, COL_WHITE);
	gfx_text_in(cr, in_call ? _("end call") : _("call"), R(cbt.x + cbt.w / 2 - 30, cbt.y, cbt.w / 2, cbt.h), 22,
	            FW_SEMILIGHT, COL_WHITE, TA_LEFT, true);
	if (ui_clicked(ui, cid)) { if (in_call) call_hangup(); else call_start(); }
}

static bool phone_back(void) { return false; }

const AppDef app_phone = {
	.id = "phone", .name = N_("Phone"), .icon = ICON_PHONE, .color = 0,
	.keywords = "dialer call dial number telephone arama telefon numara",
	.open = phone_open, .draw = phone_draw, .back = phone_back,
};

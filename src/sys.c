#include "sys.h"
#include "proc.h"
#include "server.h"
#include "shell/shell.h"
#include "util.h"

#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <wlr/version.h>

static bool g_demo;
static Battery g_bat;
static WifiInfo g_wifi;
static int g_volume = 50, g_brightness = 70;
static bool g_muted, g_airplane, g_bt_on, g_bt_avail;
static bool g_have_audio, g_audio_probed;
static enum { AUD_NONE, AUD_WPCTL, AUD_PACTL, AUD_AMIXER } g_aud;
static char g_backlight[256];       /* sysfs dir */
static int g_backlight_max;
static Timer *g_poll_timer;
static WifiNet g_nets[48];
static int g_nnets;
static bool g_scanning;
static char g_wifi_msg[128];
static bool g_online;

void sys_init_demo(void);

/* ---------------------------------------------------------------- helpers */

static char *read_sys_file(const char *path) {
	char *s = file_read_all(path, NULL);
	if (s) str_trim(s);
	return s;
}

static int read_sys_int(const char *path, int def) {
	char *s = read_sys_file(path);
	if (!s) return def;
	int v = atoi(s);
	free(s);
	return v;
}

/* ---------------------------------------------------------------- battery */

static void poll_battery(void) {
	if (g_demo) return;
	g_bat = (Battery){0};
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char *base = path_join("/sys/class/power_supply", e->d_name);
		char *tp = path_join(base, "type");
		char *type = read_sys_file(tp);
		free(tp);
		if (type && str_eq(type, "Battery")) {
			char *cp = path_join(base, "capacity"), *sp = path_join(base, "status");
			char *st = read_sys_file(sp);
			g_bat.present = true;
			g_bat.pct = CLAMP(read_sys_int(cp, 0), 0, 100);
			g_bat.charging = st && str_eq(st, "Charging");
			g_bat.full = st && str_eq(st, "Full");
			free(cp); free(sp); free(st);
		}
		free(type);
		free(base);
	}
	closedir(d);
}

Battery sys_battery(void) { return g_bat; }

/* -------------------------------------------------------------- backlight */

static void probe_backlight(void) {
	DIR *d = opendir("/sys/class/backlight");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char *base = path_join("/sys/class/backlight", e->d_name);
		char *mp = path_join(base, "max_brightness");
		int mx = read_sys_int(mp, 0);
		free(mp);
		if (mx > 0) {
			str_copy(g_backlight, sizeof g_backlight, base);
			g_backlight_max = mx;
			free(base);
			break;
		}
		free(base);
	}
	closedir(d);
}

bool sys_has_backlight(void) { return g_demo || g_backlight[0]; }

int sys_brightness(void) {
	if (g_demo || !g_backlight[0]) return g_brightness;
	char *p = path_join(g_backlight, "brightness");
	int v = read_sys_int(p, 0);
	free(p);
	g_brightness = CLAMP(v * 100 / MAX(1, g_backlight_max), 0, 100);
	return g_brightness;
}

void sys_set_brightness(int pct) {
	pct = CLAMP(pct, 1, 100);
	g_brightness = pct;
	if (g_demo || !g_backlight[0]) return;
	char *p = path_join(g_backlight, "brightness");
	char val[16];
	snprintf(val, sizeof val, "%d", MAX(1, pct * g_backlight_max / 100));
	if (!file_write_all(p, val, strlen(val))) {
		if (proc_exists("brightnessctl")) {
			char arg[16];
			snprintf(arg, sizeof arg, "%d%%", pct);
			char *argv[] = {"brightnessctl", "-q", "set", arg, NULL};
			proc_run(argv, NULL, NULL);
		}
	}
	free(p);
}

/* ------------------------------------------------------------------ audio */

static void parse_volume(int status, char *out, size_t len, void *ud) {
	if (g_aud == AUD_WPCTL) {              /* "Volume: 0.45 [MUTED]" */
		char *v = strstr(out, "Volume:");
		if (v) g_volume = CLAMP((int)(atof(v + 7) * 100 + 0.5), 0, 150);
		g_muted = strstr(out, "[MUTED]") != NULL;
	} else if (g_aud == AUD_PACTL) {        /* "Volume: front-left: 29491 /  45% / ..." */
		char *pc = strchr(out, '%');
		if (pc) {
			char *s = pc;
			while (s > out && s[-1] >= '0' && s[-1] <= '9') s--;
			g_volume = CLAMP(atoi(s), 0, 150);
		}
		g_muted = strstr(out, "yes") != NULL;
	} else if (g_aud == AUD_AMIXER) {       /* "[45%] [on]" */
		char *b = strchr(out, '[');
		if (b) g_volume = CLAMP(atoi(b + 1), 0, 100);
		g_muted = strstr(out, "[off]") != NULL;
	}
}

static void poll_volume(void) {
	if (g_demo) return;
	if (!g_audio_probed) {
		g_audio_probed = true;
		g_aud = proc_exists("wpctl") ? AUD_WPCTL : proc_exists("pactl") ? AUD_PACTL
		      : proc_exists("amixer") ? AUD_AMIXER : AUD_NONE;
		g_have_audio = g_aud != AUD_NONE;
	}
	switch (g_aud) {
	case AUD_WPCTL: { char *a[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL}; proc_run(a, parse_volume, NULL); break; }
	case AUD_PACTL: { char *a[] = {"sh", "-c", "pactl get-sink-volume @DEFAULT_SINK@; pactl get-sink-mute @DEFAULT_SINK@", NULL}; proc_run(a, parse_volume, NULL); break; }
	case AUD_AMIXER: { char *a[] = {"amixer", "sget", "Master", NULL}; proc_run(a, parse_volume, NULL); break; }
	default: break;
	}
}

bool sys_has_audio(void) { return g_demo || g_have_audio; }
int sys_volume(void) { return g_volume; }
bool sys_muted(void) { return g_muted; }

void sys_set_volume(int pct) {
	pct = CLAMP(pct, 0, 100);
	g_volume = pct;
	if (g_demo) return;
	char arg[16];
	switch (g_aud) {
	case AUD_WPCTL: snprintf(arg, sizeof arg, "%d%%", pct);
		{ char *a[] = {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", arg, NULL}; proc_run(a, NULL, NULL); } break;
	case AUD_PACTL: snprintf(arg, sizeof arg, "%d%%", pct);
		{ char *a[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", arg, NULL}; proc_run(a, NULL, NULL); } break;
	case AUD_AMIXER: snprintf(arg, sizeof arg, "%d%%", pct);
		{ char *a[] = {"amixer", "-q", "sset", "Master", arg, NULL}; proc_run(a, NULL, NULL); } break;
	default: break;
	}
}

void sys_set_mute(bool m) {
	g_muted = m;
	if (g_demo) return;
	switch (g_aud) {
	case AUD_WPCTL: { char *a[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", m ? "1" : "0", NULL}; proc_run(a, NULL, NULL); } break;
	case AUD_PACTL: { char *a[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", m ? "1" : "0", NULL}; proc_run(a, NULL, NULL); } break;
	case AUD_AMIXER: { char *a[] = {"amixer", "-q", "sset", "Master", m ? "mute" : "unmute", NULL}; proc_run(a, NULL, NULL); } break;
	default: break;
	}
}

/* ------------------------------------------------------------------- wifi */

/* Splits an nmcli terse (-t) line on unescaped ':' into fields. */
static int split_terse(char *line, char **f, int max) {
	int n = 0;
	char *w = line, *start = line;
	f[n++] = w;
	for (char *r = line; *r && n <= max; r++) {
		if (*r == '\\' && r[1]) { *w++ = *++r; continue; }
		if (*r == ':') { *w++ = 0; if (n < max) f[n++] = w; continue; }
		*w++ = *r;
	}
	*w = 0;
	(void)start;
	return n;
}

static void parse_wifi_status(int status, char *out, size_t len, void *ud) {
	/* nmcli -t -f IN-USE,SSID,SIGNAL dev wifi list  (active row has '*') */
	g_wifi.connected = false;
	g_wifi.ssid[0] = 0;
	for (char *save = NULL, *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[4];
		int n = split_terse(line, f, 4);
		if (n >= 3 && str_eq(f[0], "*")) {
			g_wifi.connected = true;
			str_copy(g_wifi.ssid, sizeof g_wifi.ssid, f[1]);
			g_wifi.signal = atoi(f[2]);
		}
	}
	shell_invalidate();
}

static void parse_radio(int status, char *out, size_t len, void *ud) {
	g_wifi.available = status == 0 && *out;
	g_wifi.enabled = strstr(out, "enabled") != NULL;
	if (g_wifi.available && g_wifi.enabled) {
		char *a[] = {"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "no", NULL};
		proc_run(a, parse_wifi_status, NULL);
	} else {
		g_wifi.connected = false;
	}
}

static void poll_wifi(void) {
	if (g_demo) return;
	if (!proc_exists("nmcli")) { g_wifi.available = false; return; }
	char *a[] = {"nmcli", "radio", "wifi", NULL};
	proc_run(a, parse_radio, NULL);
}

WifiInfo sys_wifi(void) { return g_wifi; }

void sys_wifi_set_enabled(bool on) {
	g_wifi.enabled = on;
	if (!on) g_wifi.connected = false;
	if (g_demo) return;
	char *a[] = {"nmcli", "radio", "wifi", on ? "on" : "off", NULL};
	proc_run(a, NULL, NULL);
}

static void parse_scan(int status, char *out, size_t len, void *ud) {
	g_scanning = false;
	g_nnets = 0;
	for (char *save = NULL, *line = strtok_r(out, "\n", &save); line && g_nnets < 48; line = strtok_r(NULL, "\n", &save)) {
		char *f[6];
		int n = split_terse(line, f, 6);
		if (n < 4 || !f[1][0]) continue;
		bool dup = false;
		for (int i = 0; i < g_nnets; i++) if (str_eq(g_nets[i].ssid, f[1])) dup = true;
		if (dup) continue;
		WifiNet *w = &g_nets[g_nnets++];
		w->active = str_eq(f[0], "*");
		str_copy(w->ssid, sizeof w->ssid, f[1]);
		w->signal = atoi(f[2]);
		w->secured = f[3][0] && !str_eq(f[3], "--");
	}
	shell_invalidate();
}

void sys_wifi_scan(void) {
	if (g_scanning) return;
	if (g_demo) {
		g_nnets = 0;
		const struct { const char *s; int sig; bool sec; bool act; } d[] = {
			{"MetroNet", 82, true, true}, {"Lumia-Hotspot", 64, true, false}, {"Cafe Guest", 47, false, false},
			{"Ev-Wifi-5G", 38, true, false}, {"TTNET_B3F2", 22, true, false}};
		for (size_t i = 0; i < ARRAY_LEN(d); i++) {
			WifiNet *w = &g_nets[g_nnets++];
			str_copy(w->ssid, sizeof w->ssid, d[i].s);
			w->signal = d[i].sig; w->secured = d[i].sec; w->active = d[i].act;
		}
		return;
	}
	if (!g_wifi.available) return;
	g_scanning = true;
	char *a[] = {"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", "yes", NULL};
	if (!proc_run(a, parse_scan, NULL)) g_scanning = false;
}

const WifiNet *sys_wifi_list(int *n) { *n = g_nnets; return g_nets; }
bool sys_wifi_scanning(void) { return g_scanning; }
const char *sys_wifi_status_msg(void) { return g_wifi_msg; }

static void connect_done(int status, char *out, size_t len, void *ud) {
	if (status == 0) {
		g_wifi_msg[0] = 0;
		poll_wifi();
	} else {
		str_copy(g_wifi_msg, sizeof g_wifi_msg, str_trim(out)[0] ? str_trim(out) : "Could not connect");
	}
	shell_invalidate();
}

void sys_wifi_connect(const char *ssid, const char *password) {
	g_wifi_msg[0] = 0;
	if (g_demo) {
		for (int i = 0; i < g_nnets; i++) g_nets[i].active = str_eq(g_nets[i].ssid, ssid);
		g_wifi.connected = true;
		str_copy(g_wifi.ssid, sizeof g_wifi.ssid, ssid);
		return;
	}
	if (password && *password) {
		char *a[] = {"nmcli", "dev", "wifi", "connect", (char *)ssid, "password", (char *)password, NULL};
		proc_run(a, connect_done, NULL);
	} else {
		char *a[] = {"nmcli", "dev", "wifi", "connect", (char *)ssid, NULL};
		proc_run(a, connect_done, NULL);
	}
}

/* --------------------------------------------------------------- radios --- */

static void parse_rfkill(int status, char *out, size_t len, void *ud) {
	g_bt_avail = strstr(out, "Bluetooth") != NULL;
	bool bt_blocked = false, all_blocked = true, any = false;
	char *cur = NULL;
	for (char *save = NULL, *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] >= '0' && line[0] <= '9') cur = line;
		else if (strstr(line, "Soft blocked:") && cur) {
			bool blocked = strstr(line, "yes") != NULL;
			any = true;
			if (strstr(cur, "Bluetooth")) bt_blocked = blocked;
			if (!blocked) all_blocked = false;
		}
	}
	g_bt_on = g_bt_avail && !bt_blocked;
	g_airplane = any && all_blocked;
}

static void poll_radios(void) {
	if (g_demo || !proc_exists("rfkill")) return;
	char *a[] = {"rfkill", "list", NULL};
	proc_run(a, parse_rfkill, NULL);
}

bool sys_bluetooth_available(void) { return g_demo || g_bt_avail; }
bool sys_bluetooth_enabled(void) { return g_bt_on; }

void sys_bluetooth_set(bool on) {
	g_bt_on = on;
	if (g_demo || !proc_exists("rfkill")) return;
	char *a[] = {"rfkill", on ? "unblock" : "block", "bluetooth", NULL};
	proc_run(a, NULL, NULL);
}

bool sys_airplane(void) { return g_airplane; }

void sys_set_airplane(bool on) {
	g_airplane = on;
	if (on) { g_wifi.enabled = false; g_wifi.connected = false; g_bt_on = false; }
	if (g_demo) return;
	if (proc_exists("rfkill")) {
		char *a[] = {"rfkill", on ? "block" : "unblock", "all", NULL};
		proc_run(a, NULL, NULL);
	} else if (proc_exists("nmcli")) {
		char *a[] = {"nmcli", "radio", "all", on ? "off" : "on", NULL};
		proc_run(a, NULL, NULL);
	}
}

/* ------------------------------------------------------------------ power */

static void power_cmd(const char *verb) {
	if (g_demo) { LOGI("demo mode: ignoring %s", verb); return; }
	char *a[] = {"systemctl", (char *)verb, NULL};
	if (proc_exists("systemctl")) proc_spawn(a);
	else if (proc_exists("loginctl")) { char *b[] = {"loginctl", (char *)verb, NULL}; proc_spawn(b); }
}
void sys_poweroff(void) { power_cmd("poweroff"); }
void sys_reboot(void) { power_cmd("reboot"); }
void sys_suspend(void) { power_cmd("suspend"); }

/* ------------------------------------------------------------------- info */

void sys_info(SysInfo *o) {
	memset(o, 0, sizeof *o);
	str_copy(o->os, sizeof o->os, "Linux");
	char *rel = file_read_all("/etc/os-release", NULL);
	if (rel) {
		for (char *save = NULL, *l = strtok_r(rel, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (str_starts(l, "PRETTY_NAME=")) {
				char *v = l + 12;
				if (*v == '"') { v++; char *q = strchr(v, '"'); if (q) *q = 0; }
				str_copy(o->os, sizeof o->os, v);
			}
		}
		free(rel);
	}
	struct utsname u;
	if (uname(&u) == 0) {
		str_copy(o->kernel, sizeof o->kernel, u.release);
		str_copy(o->hostname, sizeof o->hostname, u.nodename);
		str_copy(o->arch, sizeof o->arch, u.machine);
	}
	str_copy(o->wlroots, sizeof o->wlroots, WLR_VERSION_STR);
	char *ci = file_read_all("/proc/cpuinfo", NULL);
	if (ci) {
		char *m = strstr(ci, "model name");
		if (!m) m = strstr(ci, "Hardware");
		if (!m) m = strstr(ci, "Model");
		if (m) {
			char *c = strchr(m, ':');
			if (c) { char *e = strchr(c, '\n'); if (e) *e = 0; str_copy(o->cpu, sizeof o->cpu, str_trim(c + 1)); }
		}
		free(ci);
	}
	if (!o->cpu[0]) {
		/* many ARM boards (PinePhone, Raspberry Pi...) only name the device in the device tree */
		char *dt = file_read_all("/proc/device-tree/model", NULL);
		if (dt) { str_copy(o->cpu, sizeof o->cpu, str_trim(dt)); free(dt); }
	}
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		o->mem_total = (double)si.totalram * si.mem_unit;
		o->uptime = (double)si.uptime;
	}
	char *mi = file_read_all("/proc/meminfo", NULL);
	if (mi) {
		char *a = strstr(mi, "MemAvailable:");
		if (a) o->mem_avail = atof(a + 13) * 1024;
		free(mi);
	}
	struct statvfs sv;
	if (statvfs(home_dir(), &sv) == 0) {
		o->disk_total = (double)sv.f_blocks * sv.f_frsize;
		o->disk_free = (double)sv.f_bavail * sv.f_frsize;
	}
}

bool sys_online(void) { return g_demo || g_online || g_wifi.connected; }

static void parse_route(int status, char *out, size_t len, void *ud) { g_online = strstr(out, "default") != NULL; }

/* ---------------------------------------------------------------- polling */

static void poll_all(void *ud) {
	poll_battery();
	poll_volume();
	poll_wifi();
	poll_radios();
	if (!g_demo && proc_exists("ip")) { char *a[] = {"ip", "route", NULL}; proc_run(a, parse_route, NULL); }
	shell_invalidate();
}

bool sys_demo(void) { return g_demo; }

void sys_init(bool demo) {
	g_demo = demo;
	probe_backlight();
	if (demo) {
		g_bat = (Battery){true, false, false, 78};
		g_wifi = (WifiInfo){true, true, true, "MetroNet", 82};
		g_volume = 40;
		g_bt_avail = true;
		g_bt_on = true;
		g_have_audio = true;
		g_brightness = 70;
		g_online = true;
	}
	poll_all(NULL);
	g_poll_timer = timer_add(15000, true, poll_all, NULL);
}

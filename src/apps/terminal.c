/* Terminal: a launcher tile that starts the user's terminal emulator. */
#include "apps.h"
#include "terminal.h"
#include "../desktop.h"
#include "../i18n.h"
#include "../proc.h"
#include "../util.h"

/* Known terminals: desktop ids, executable and how to pass a working directory
 * ("--flag=" glued to the path, or "--flag" as a separate argument). */
static const struct {
	const char *ids[3];
	const char *bin;
	const char *wd;
} TERMS[] = {
	{{"org.gnome.Console", "kgx"}, "kgx", "--working-directory="},
	{{"org.gnome.Terminal", "gnome-terminal"}, "gnome-terminal", "--working-directory="},
	{{"org.kde.konsole", "konsole"}, "konsole", "--workdir"},
	{{"org.codeberg.dnkl.foot", "foot"}, "foot", "--working-directory="},
	{{"Alacritty", "alacritty"}, "alacritty", "--working-directory"},
	{{"kitty"}, "kitty", "--directory"},
	{{"org.wezfurlong.wezterm"}, "wezterm", NULL},
	{{"xfce4-terminal", "org.xfce.terminal"}, "xfce4-terminal", "--working-directory="},
	{{"terminator"}, "terminator", "--working-directory="},
	{{"com.gexperts.Tilix"}, "tilix", "--working-directory="},
	{{"mate-terminal"}, "mate-terminal", "--working-directory="},
	{{"lxterminal"}, "lxterminal", "--working-directory="},
	{{"qterminal"}, "qterminal", "--workdir"},
	{{"xterm", "debian-xterm"}, "xterm", NULL},
	{{"urxvt", "rxvt-unicode"}, "urxvt", NULL},
};

/* $TERMINAL when it names an installed program */
static const char *env_terminal(void) {
	const char *t = getenv("TERMINAL");
	if (!t || !*t) return NULL;
	char prog[256];
	str_copy(prog, sizeof prog, t);
	char *sp = strchr(prog, ' ');
	if (sp) *sp = 0;
	return proc_exists(prog) ? t : NULL;
}

static DesktopApp *term_desktop(int i) {
	for (int k = 0; k < 3 && TERMS[i].ids[k]; k++) {
		DesktopApp *a = desktop_find(TERMS[i].ids[k]);
		if (a) return a;
	}
	return NULL;
}

bool terminal_available(void) {
	static double checked = -100;
	static bool avail;
	double now = now_sec();
	if (now - checked < 10) return avail;
	checked = now;
	avail = env_terminal() != NULL;
	for (size_t i = 0; i < ARRAY_LEN(TERMS) && !avail; i++) avail = term_desktop((int)i) || proc_exists(TERMS[i].bin);
	return avail;
}

static void launch_desktop(DesktopApp *a) {
	char key[200];
	snprintf(key, sizeof key, "d:%s", a->id);
	shell_launch(key);
}

bool terminal_open(const char *dir) {
	if (dir && !is_dir(dir)) dir = NULL;
	const char *env = env_terminal();
	if (env) {
		if (dir) {
			char *argv[] = {"sh", "-c", "cd -- \"$1\" && exec $TERMINAL", "sh", (char *)dir, NULL};
			return proc_spawn(argv);
		}
		char *argv[] = {"sh", "-c", "exec $TERMINAL", NULL};
		return proc_spawn(argv);
	}
	for (size_t i = 0; i < ARRAY_LEN(TERMS); i++) {
		DesktopApp *a = term_desktop((int)i);
		bool bin = proc_exists(TERMS[i].bin);
		if (!a && !bin) continue;
		if (!dir || !bin) {
			/* through the desktop entry when possible: Start shows the launch splash */
			if (a) launch_desktop(a);
			else {
				char *argv[] = {(char *)TERMS[i].bin, NULL};
				proc_spawn(argv);
			}
			return true;
		}
		/* start inside dir (cd) and also pass the terminal's own option: server based
		 * terminals (gnome-terminal) ignore the working directory of the client */
		char *flag = NULL;
		char *argv[8] = {"sh", "-c", "cd -- \"$1\" && shift && exec \"$@\"", "sh", (char *)dir, (char *)TERMS[i].bin};
		int n = 6;
		const char *wd = TERMS[i].wd;
		if (wd && str_ends(wd, "=")) argv[n++] = flag = xasprintf("%s%s", wd, dir);
		else if (wd) { argv[n++] = (char *)wd; argv[n++] = (char *)dir; }
		argv[n] = NULL;
		bool ok = proc_spawn(argv);
		free(flag);
		return ok;
	}
	shell_toast(_("Terminal"), _("No terminal installed. Get one from the Store."), NULL);
	return false;
}

static bool terminal_launch(void) {
	terminal_open(NULL);
	return true;
}

const AppDef app_terminal = {
	.id = "terminal", .name = N_("Terminal"), .icon = ICON_TERMINAL, .color = 0x2D2D30,
	.keywords = "console shell command line bash konsole",
	.launcher = terminal_launch,
};

#include "server.h"
#include "util.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int server_main(void);

/* A fresh phone user has no Documents/Downloads/...: create them, and register them
 * in user-dirs.dirs (unless the user already has one) so apps save in the same place. */
static void ensure_user_dirs(void) {
	const char *home = getenv("HOME");
	if (!home || !*home) return;
	static const char *D[][2] = {
		{"DESKTOP", "Desktop"}, {"DOCUMENTS", "Documents"}, {"DOWNLOAD", "Downloads"},
		{"MUSIC", "Music"}, {"PICTURES", "Pictures"}, {"VIDEOS", "Videos"},
		{"TEMPLATES", "Templates"}, {"PUBLICSHARE", "Public"},
	};
	char path[4096];
	for (size_t i = 0; i < sizeof D / sizeof *D; i++) {
		snprintf(path, sizeof path, "%s/%s", home, D[i][1]);
		mkdir_p(path);
	}
	snprintf(path, sizeof path, "%s/Pictures/Screenshots", home);
	mkdir_p(path);
	const char *xdg = getenv("XDG_CONFIG_HOME");
	char cfgdir[4000];
	if (xdg && *xdg) snprintf(cfgdir, sizeof cfgdir, "%s", xdg);
	else snprintf(cfgdir, sizeof cfgdir, "%s/.config", home);
	mkdir_p(cfgdir);
	snprintf(path, sizeof path, "%s/user-dirs.dirs", cfgdir);
	if (access(path, F_OK) == 0) return;
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "# written by metrowl\n");
	for (size_t i = 0; i < sizeof D / sizeof *D; i++) fprintf(f, "XDG_%s_DIR=\"$HOME/%s\"\n", D[i][0], D[i][1]);
	fclose(f);
}

static void usage(const char *prog) {
	printf(
		"metrowl %s - a Windows Phone Metro style Wayland compositor (wlroots)\n\n"
		"Usage: %s [options]\n"
		"  -s, --startup CMD   run CMD after the compositor started\n"
		"      --size WxH      force the output size (nested/headless backends)\n"
		"      --scale S       UI scale factor (default: automatic, ~ width/480)\n"
		"      --no-lock       start unlocked on the Start screen\n"
		"      --demo          fake battery/Wi-Fi/volume so the shell looks complete on a desktop\n"
		"      --ipc PATH      control socket path (default $XDG_RUNTIME_DIR/metrowl-ipc)\n"
		"  -d, --debug         verbose logging\n"
		"  -v, --version       print version\n"
		"  -h, --help          this help\n\n"
		"Environment: WLR_BACKENDS=wayland|headless|drm, WLR_RENDERER=pixman|gles2,\n"
		"             METROWL_DATADIR=<dir with fonts/ and store/>\n",
		METROWL_VERSION, prog);
}

int main(int argc, char **argv) {
	enum wlr_log_importance level = WLR_INFO;
	static const struct option opts[] = {
		{"startup", required_argument, 0, 's'}, {"size", required_argument, 0, 'z'},
		{"scale", required_argument, 0, 'S'}, {"demo", no_argument, 0, 'D'},
		{"ipc", required_argument, 0, 'i'}, {"no-lock", no_argument, 0, 'N'}, {"debug", no_argument, 0, 'd'},
		{"version", no_argument, 0, 'v'}, {"help", no_argument, 0, 'h'}, {0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "s:dvh", opts, NULL)) != -1) {
		switch (c) {
		case 's': srv.opt_startup = optarg; break;
		case 'z': {
			int w, h;
			if (sscanf(optarg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { srv.opt_w = w; srv.opt_h = h; }
			else { fprintf(stderr, "bad --size, expected WxH\n"); return 2; }
			break;
		}
		case 'S': srv.opt_scale = atof(optarg); break;
		case 'D': srv.opt_demo = true; break;
		case 'N': srv.opt_nolock = true; break;
		case 'i': srv.opt_ipc_path = optarg; break;
		case 'd': level = WLR_DEBUG; break;
		case 'v': printf("metrowl %s\n", METROWL_VERSION); return 0;
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}
	wlr_log_init(level, NULL);
	ensure_user_dirs();
	return server_main();
}

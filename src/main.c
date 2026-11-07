#include "server.h"
#include "util.h"

#include <getopt.h>
#include <unistd.h>

int server_main(void);

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
	return server_main();
}

/* metrowlctl - talk to a running metrowl compositor.
 *   metrowlctl screenshot /tmp/shot.png
 *   metrowlctl launch b:settings
 *   metrowlctl -            (read commands from stdin, one per line) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int send_command(const char *path, const char *line) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 2; }
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		fprintf(stderr, "metrowlctl: cannot connect to %s: %s\n", path, strerror(errno));
		return 2;
	}
	size_t n = strlen(line);
	if (write(fd, line, n) < 0 || write(fd, "\n", 1) < 0) { perror("write"); return 2; }
	char buf[4096];
	ssize_t r;
	int status = 0;
	char first[16] = "";
	int got_first = 0;
	while ((r = read(fd, buf, sizeof buf)) > 0) {
		fwrite(buf, 1, (size_t)r, stdout);
		if (!got_first) { snprintf(first, sizeof first, "%.3s", buf); got_first = 1; }
		if (r >= 3 && strstr(buf, "err ")) status = 1;
	}
	close(fd);
	(void)first;
	return status;
}

int main(int argc, char **argv) {
	const char *path = getenv("METROWL_IPC");
	char def[256];
	if (!path) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (rt && *rt) snprintf(def, sizeof def, "%s/metrowl-ipc", rt);
		else snprintf(def, sizeof def, "/tmp/metrowl-ipc-%d", (int)getuid());
		path = def;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: metrowlctl <command> [args...]   (try: metrowlctl help)\n");
		return 2;
	}
	if (strcmp(argv[1], "-") == 0) {
		char line[1024];
		int rc = 0;
		while (fgets(line, sizeof line, stdin)) {
			line[strcspn(line, "\n")] = 0;
			if (!*line || *line == '#') continue;
			int r = send_command(path, line);
			if (r) rc = r;
		}
		return rc;
	}
	char line[2048] = "";
	for (int i = 1; i < argc; i++) {
		if (i > 1) strncat(line, " ", sizeof line - strlen(line) - 1);
		strncat(line, argv[i], sizeof line - strlen(line) - 1);
	}
	return send_command(path, line);
}

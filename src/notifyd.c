#include "server.h"
#include "shell/shell.h"
#include "util.h"

/* org.freedesktop.Notifications server: lets notify-send and every desktop app
 * post toasts into the Metro Action Center. Optional (needs sd-bus). */

#if HAVE_SDBUS

#if __has_include(<systemd/sd-bus.h>)
#include <systemd/sd-bus.h>
#elif __has_include(<elogind/sd-bus.h>)
#include <elogind/sd-bus.h>
#else
#include <basu/sd-bus.h>
#endif

static sd_bus *g_bus;
static struct wl_event_source *g_bus_src;

static const char *OBJ = "/org/freedesktop/Notifications";
static const char *IFACE = "org.freedesktop.Notifications";

/* Removes pango-ish markup and decodes the common entities. */
static char *strip_markup(const char *s) {
	char *out = xmalloc(strlen(s) + 1), *w = out;
	for (const char *p = s; *p;) {
		if (*p == '<') {
			const char *e = strchr(p, '>');
			if (e) {
				if (!strncmp(p, "<br", 3)) *w++ = '\n';
				p = e + 1;
				continue;
			}
		}
		if (*p == '&') {
			struct { const char *ent; char ch; } map[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
				{"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}};
			bool done = false;
			for (size_t i = 0; i < ARRAY_LEN(map) && !done; i++) {
				size_t n = strlen(map[i].ent);
				if (!strncmp(p, map[i].ent, n)) { *w++ = map[i].ch; p += n; done = true; }
			}
			if (done) continue;
		}
		*w++ = *p++;
	}
	*w = 0;
	return out;
}

static int method_caps(sd_bus_message *m, void *ud, sd_bus_error *err) {
	sd_bus_message *r = NULL;
	sd_bus_message_new_method_return(m, &r);
	sd_bus_message_append_strv(r, (char *[]){"body", "actions", "icon-static", "persistence", NULL});
	int rc = sd_bus_send(NULL, r, NULL);
	sd_bus_message_unref(r);
	return rc;
}

static int method_info(sd_bus_message *m, void *ud, sd_bus_error *err) {
	return sd_bus_reply_method_return(m, "ssss", "metrowl", "metrowl", METROWL_VERSION, "1.2");
}

static int method_notify(sd_bus_message *m, void *ud, sd_bus_error *err) {
	const char *app = "", *icon = "", *summary = "", *body = "";
	uint32_t replaces = 0;
	int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
	if (r < 0) return r;
	char **actions = NULL;
	r = sd_bus_message_read_strv(m, &actions);
	if (r < 0) return r;
	char *default_action = NULL;
	for (int i = 0; actions && actions[i] && actions[i + 1]; i += 2)
		if (str_eq(actions[i], "default")) default_action = xstrdup(actions[i]);
	int urgency = 1;
	char *desktop_entry = NULL;
	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		sd_bus_message_read(m, "s", &key);
		char type;
		const char *contents;
		sd_bus_message_peek_type(m, &type, &contents);
		if (str_eq(key, "urgency") && contents && contents[0] == 'y') {
			sd_bus_message_enter_container(m, 'v', "y");
			uint8_t u = 1;
			sd_bus_message_read(m, "y", &u);
			sd_bus_message_exit_container(m);
			urgency = u;
		} else if (str_eq(key, "desktop-entry") && contents && contents[0] == 's') {
			sd_bus_message_enter_container(m, 'v', "s");
			const char *de = NULL;
			sd_bus_message_read(m, "s", &de);
			sd_bus_message_exit_container(m);
			free(desktop_entry);
			desktop_entry = xstrdup(de);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	int32_t timeout = -1;
	sd_bus_message_read(m, "i", &timeout);

	char *plain = strip_markup(body);
	uint32_t id = shell_notify(app, summary, plain, icon, desktop_entry, urgency, replaces, false);
	free(plain);
	free(desktop_entry);
	free(default_action);
	if (actions) { for (int i = 0; actions[i]; i++) free(actions[i]); free(actions); }
	return sd_bus_reply_method_return(m, "u", id);
}

static int method_close(sd_bus_message *m, void *ud, sd_bus_error *err) {
	uint32_t id;
	int r = sd_bus_message_read(m, "u", &id);
	if (r < 0) return r;
	shell_notify_remove(id);
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("GetCapabilities", "", "as", method_caps, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", method_info, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END
};

static int bus_readable(int fd, uint32_t mask, void *data) {
	int r;
	do { r = sd_bus_process(g_bus, NULL); } while (r > 0);
	sd_bus_flush(g_bus);
	return 0;
}

void notifyd_close(uint32_t id, uint32_t reason) {
	if (g_bus) sd_bus_emit_signal(g_bus, OBJ, IFACE, "NotificationClosed", "uu", id, reason);
}

void notifyd_action(uint32_t id, const char *key) {
	if (g_bus) sd_bus_emit_signal(g_bus, OBJ, IFACE, "ActionInvoked", "us", id, key);
}

void notifyd_init(void) {
	int r = sd_bus_open_user(&g_bus);
	if (r < 0) {
		LOGI("notifications: no D-Bus session bus (%s); notify-send will not reach the shell", strerror(-r));
		g_bus = NULL;
		return;
	}
	r = sd_bus_add_object_vtable(g_bus, NULL, OBJ, IFACE, vtable, NULL);
	if (r >= 0) r = sd_bus_request_name(g_bus, IFACE, 0);
	if (r < 0) {
		LOGI("notifications: cannot own %s (%s) - another daemon is running?", IFACE, strerror(-r));
		sd_bus_unref(g_bus);
		g_bus = NULL;
		return;
	}
	g_bus_src = wl_event_loop_add_fd(srv.loop, sd_bus_get_fd(g_bus), WL_EVENT_READABLE, bus_readable, NULL);
	bus_readable(0, 0, NULL);
	LOGI("notifications: serving %s on the session bus", IFACE);
}

#else

void notifyd_close(uint32_t id, uint32_t reason) {}
void notifyd_action(uint32_t id, const char *key) {}
void notifyd_init(void) { LOGI("notifications: built without sd-bus, D-Bus notifications disabled"); }

#endif

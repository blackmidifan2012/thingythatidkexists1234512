#include "server.h"
#include "config.h"
#include "i18n.h"
#include "proc.h"
#include "shell/shell.h"
#include "sys.h"
#include "ui/gfx.h"
#include "ui/icons.h"
#include "desktop.h"
#include "util.h"

#include <signal.h>
#include <unistd.h>
#include <wlr/backend/session.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

Server srv;

void srv_quit(void) { wl_display_terminate(srv.display); }

void srv_schedule_frame(void) {
	if (srv.out && !srv.out->blanked) wlr_output_schedule_frame(srv.out->wlr);
}

static void on_new_output(struct wl_listener *l, void *data) {
	struct wlr_output *wlr = data;
	if (srv.out) {
		LOGI("extra output %s ignored (single-screen phone shell)", wlr->name);
		return;
	}
	output_init_primary(wlr);
}

static int on_signal(int sig, void *data) {
	LOGI("signal %d: shutting down", sig);
	srv_quit();
	return 0;
}

static void on_new_virtual_keyboard(struct wl_listener *l, void *data) {
	/* external on-screen keyboards (wvkbd, squeekboard) could attach here; the
	 * built-in keyboard is preferred, so we only accept the device. */
	struct wlr_virtual_keyboard_v1 *vk = data;
	wlr_seat_set_keyboard(srv.seat, &vk->keyboard);
}

static bool server_init(void) {
	srv.display = wl_display_create();
	srv.loop = wl_display_get_event_loop(srv.display);

	srv.backend = wlr_backend_autocreate(srv.loop, &srv.session);
	if (!srv.backend) { LOGE("failed to create wlr_backend"); return false; }
	srv.renderer = wlr_renderer_autocreate(srv.backend);
	if (!srv.renderer) { LOGE("failed to create wlr_renderer"); return false; }
	wlr_renderer_init_wl_display(srv.renderer, srv.display);
	srv.allocator = wlr_allocator_autocreate(srv.backend, srv.renderer);
	if (!srv.allocator) { LOGE("failed to create wlr_allocator"); return false; }

	srv.compositor = wlr_compositor_create(srv.display, 6, srv.renderer);
	wlr_subcompositor_create(srv.display);
	wlr_data_device_manager_create(srv.display);
	wlr_primary_selection_v1_device_manager_create(srv.display);
	wlr_data_control_manager_v1_create(srv.display);
	wlr_viewporter_create(srv.display);
	wlr_single_pixel_buffer_manager_v1_create(srv.display);
	wlr_fractional_scale_manager_v1_create(srv.display, 1);
	wlr_screencopy_manager_v1_create(srv.display);
	wlr_presentation_create(srv.display, srv.backend);
	srv.idle_notifier = wlr_idle_notifier_v1_create(srv.display);
	if (wlr_renderer_get_texture_formats(srv.renderer, WLR_BUFFER_CAP_DMABUF))
		wlr_linux_dmabuf_v1_create_with_renderer(srv.display, 4, srv.renderer);

	srv.layout = wlr_output_layout_create(srv.display);
	wlr_xdg_output_manager_v1_create(srv.display, srv.layout);
	srv.scene = wlr_scene_create();
	srv.scene_layout = wlr_scene_attach_output_layout(srv.scene, srv.layout);
	srv.t_base = wlr_scene_tree_create(&srv.scene->tree);
	srv.t_windows = wlr_scene_tree_create(&srv.scene->tree);
	srv.t_overlay = wlr_scene_tree_create(&srv.scene->tree);

	wl_list_init(&srv.outputs);
	srv.new_output.notify = on_new_output;
	wl_signal_add(&srv.backend->events.new_output, &srv.new_output);

	views_init();
	input_init();
	textinput_init();

	struct wlr_virtual_keyboard_manager_v1 *vkm = wlr_virtual_keyboard_manager_v1_create(srv.display);
	static struct wl_listener vk_listener;
	vk_listener.notify = on_new_virtual_keyboard;
	wl_signal_add(&vkm->events.new_virtual_keyboard, &vk_listener);

	wl_event_loop_add_signal(srv.loop, SIGINT, on_signal, NULL);
	wl_event_loop_add_signal(srv.loop, SIGTERM, on_signal, NULL);
	wl_event_loop_add_signal(srv.loop, SIGHUP, on_signal, NULL);
	return true;
}

static void apps_changed(void) {
	applist_apps_changed();
	shell_invalidate();
}

int server_main(void) {
	config_init();
	i18n_init(cfg.lang);
	gfx_init();
	gfx_set_text_scale(cfg.text_scale);
	icons_set_mode(cfg.icon_mode);

	if (!server_init()) return 1;
	jobs_init();
	sys_init(srv.opt_demo);
	desktop_scan();
	shell_init();
	desktop_watch(apps_changed, shell_invalidate);

	srv.socket = wl_display_add_socket_auto(srv.display);
	if (!srv.socket) { LOGE("unable to create the wayland socket"); return 1; }
	if (!wlr_backend_start(srv.backend)) { LOGE("failed to start backend"); return 1; }
	setenv("WAYLAND_DISPLAY", srv.socket, 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", "metrowl", 1);
	LOGI("metrowl %s running on WAYLAND_DISPLAY=%s", METROWL_VERSION, srv.socket);
	/* Apps started by D-Bus or systemd (notifications, xdg-open, D-Bus activated GNOME and
	 * KDE apps) need this display and the phone layout variables too. Only when metrowl is
	 * the session on the screen: nested in another desktop it would send its apps here. */
	if (srv.session && proc_exists("dbus-update-activation-environment")) {
		char *argv[] = {"dbus-update-activation-environment", "--systemd", "WAYLAND_DISPLAY",
			"XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "MOZ_ENABLE_WAYLAND", "QT_QUICK_CONTROLS_MOBILE",
			"PLASMA_PLATFORM", "KDE_KIRIGAMI_TABLET_MODE", "ELECTRON_OZONE_PLATFORM_HINT", "QT_WAYLAND_DISABLE_WINDOWDECORATION", "GTK_CSD", "QT_QPA_PLATFORM",
			"SDL_VIDEODRIVER", "_JAVA_AWT_WM_NONREPARENTING", NULL};
		proc_spawn(argv);   /* the variables are set in the child, see child_env() */
	}

	ipc_init(srv.opt_ipc_path);
	notifyd_init();
	if (srv.opt_startup) proc_spawn_sh(srv.opt_startup);

	wl_display_run(srv.display);

	LOGI("shutting down");
	config_save_now();
	ipc_shutdown();
	jobs_shutdown();
	shell_shutdown();
	wl_display_destroy_clients(srv.display);
	wlr_scene_node_destroy(&srv.scene->tree.node);
	wlr_backend_destroy(srv.backend);
	wl_display_destroy(srv.display);
	return 0;
}

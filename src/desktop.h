#pragma once
/* XDG desktop entries: the list of installed Linux apps shown in "all apps". */
#include <stdbool.h>
#include "ui/gfx.h"

typedef struct DesktopApp {
	char *id;            /* desktop file id without ".desktop" */
	char *name;          /* localised name */
	char *comment;
	char *exec;
	char *icon_key;
	char *wm_class;
	char *categories;
	char *file;
	bool terminal;
	bool is_flatpak;
	char *fold_name;     /* for sorting/searching */
	Image *icon;         /* lazily loaded */
	bool icon_tried;
} DesktopApp;

void desktop_scan(void);
/* Rescans whenever an applications folder changes, then calls on_change. */
void desktop_watch(void (*on_change)(void));
int desktop_count(void);
DesktopApp *desktop_get(int i);
DesktopApp *desktop_find(const char *id);
/* Match a Wayland app_id against ids / StartupWMClass / executable names. */
DesktopApp *desktop_find_by_app_id(const char *app_id);
bool desktop_launch(const DesktopApp *a);
bool desktop_launch_uri(const DesktopApp *a, const char *uri_or_path);
/* The app registered for http links (a web browser), or NULL. */
DesktopApp *desktop_default_browser(void);
/* Loads (once) and returns the icon image at ~px size, or NULL. */
Image *desktop_icon(DesktopApp *a, int px);
/* Icon by theme name or absolute path (cached), e.g. from a notification. */
Image *desktop_icon_by_name(const char *name, int px);
/* Opens a file/URL with xdg-open (or the browser). */
void open_uri(const char *uri);

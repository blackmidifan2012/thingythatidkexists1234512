#pragma once
/* Icon set. If the "Segoe MDL2 Assets" font is installed the real Segoe glyphs are
 * used; otherwise each icon is drawn from built-in vector outlines in the same
 * flat Metro style (no font required). */
#include <cairo.h>
#include "gfx.h"

typedef enum {
	ICON_NONE = 0,
	ICON_BACK, ICON_START, ICON_SEARCH, ICON_SETTINGS, ICON_PEOPLE, ICON_PERSON, ICON_PHOTO,
	ICON_STORE, ICON_MAIL, ICON_CALENDAR, ICON_CLOCK, ICON_ALARM, ICON_CALC, ICON_MUSIC,
	ICON_VIDEO, ICON_FOLDER, ICON_FILE, ICON_GLOBE, ICON_WIFI, ICON_BLUETOOTH, ICON_AIRPLANE,
	ICON_BRIGHTNESS, ICON_VOLUME, ICON_MUTE, ICON_LOCK, ICON_UNLOCK, ICON_KEYBOARD,
	ICON_BACKSPACE, ICON_SHIFT, ICON_CAPS, ICON_ENTER, ICON_CHECK, ICON_CLOSE, ICON_ADD,
	ICON_REMOVE, ICON_DELETE, ICON_EDIT, ICON_PIN, ICON_UNPIN, ICON_REFRESH, ICON_DOWNLOAD,
	ICON_UPLOAD, ICON_SHARE, ICON_HEART, ICON_STAR, ICON_STAR_FILLED, ICON_PLAY, ICON_PAUSE,
	ICON_NEXT, ICON_PREV, ICON_STOP, ICON_INFO, ICON_LOCATION, ICON_PHONE, ICON_MESSAGE,
	ICON_NOTE, ICON_FLASHLIGHT, ICON_ROTATE, ICON_MOON, ICON_BELL, ICON_POWER, ICON_CAMERA,
	ICON_SUN, ICON_CLOUD, ICON_RAIN, ICON_SNOW, ICON_STORM, ICON_PARTLY, ICON_FOG,
	ICON_ARROW_CIRCLE, ICON_CHEVRON_L, ICON_CHEVRON_R, ICON_CHEVRON_U, ICON_CHEVRON_D,
	ICON_MORE, ICON_SCREENSHOT, ICON_THEME, ICON_STORAGE, ICON_EYE, ICON_DISPLAY, ICON_LIST,
	ICON_MIC, ICON_COPY, ICON_OPEN, ICON_UP, ICON_DOWN, ICON_BATTERY, ICON_SIGNAL, ICON_APPS,
	ICON_TILES, ICON_LANGUAGE, ICON_SOUND, ICON_USB, ICON_SMILEY, ICON_GLOBE_SEARCH, ICON_TASKS,
	ICON_SHUFFLE, ICON_REPEAT, ICON_REPEAT_ONE, ICON_TIMER, ICON_STOPWATCH, ICON_WORLD, ICON_FLAG,
	ICON_SAVE, ICON_SORT, ICON_SELECT, ICON_CUT, ICON_PASTE, ICON_RENAME, ICON_NEW_FOLDER, ICON_TERMINAL,
	ICON_ZOOM_IN, ICON_ZOOM_OUT, ICON_HISTORY, ICON_GAME, ICON_RECORD, ICON_LEAF, ICON_MINE, ICON_FILTER,
	ICON_DISC, ICON_HOME, ICON_SLIDESHOW, ICON_CROP, ICON_SNOOZE, ICON_EMAIL_SEND, ICON_LINK, ICON_HELP,
	ICON_COUNT
} Icon;

enum { ICON_FLAG_SLASH = 1 };   /* draws a diagonal "off" slash over the icon */

/* Draws icon centred at (cx,cy) inside a size x size box. */
void icon_draw(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c);
void icon_draw_ex(cairo_t *cr, Icon ic, double cx, double cy, double size, Color c, int flags);
/* Parameterised icons. */
void icon_draw_battery(cairo_t *cr, double cx, double cy, double size, Color c, int pct, bool charging);
void icon_draw_signal(cairo_t *cr, double cx, double cy, double size, Color c, int bars /*0..4*/);
void icon_draw_wifi_level(cairo_t *cr, double cx, double cy, double size, Color c, int level /*0..3*/);
/* Metro circle button: ring + glyph. */
void icon_draw_circle(cairo_t *cr, Icon ic, double cx, double cy, double dia, Color c, double ring);

/* "auto" (default), "font" or "vector". */
void icons_set_mode(const char *mode);
bool icons_using_font(void);

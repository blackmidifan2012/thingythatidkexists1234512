#pragma once
/* Built-in apps (the Metro "first party" apps) share this small interface. */
#include "../shell/shell.h"
#include "../ui/ui.h"
#include "cJSON.h"

typedef struct AppDef {
	const char *id;             /* stable identifier, e.g. "settings" */
	const char *name;           /* English name; translated when shown */
	Icon icon;
	uint32_t color;             /* tile colour, 0 = accent colour */
	bool hidden;                /* not listed in "all apps" */
	bool default_pinned;        /* pinned on first run */
	TileSize default_size;
	const char *keywords;       /* extra search words */
	void (*init)(void);                /* called once at startup (register timers, load data) */
	void (*open)(const char *arg);     /* (re)opened, arg may carry a deep link */
	void (*draw)(UI *ui, Rect area);   /* area = content rect between status bar and nav bar */
	bool (*back)(void);                /* Back pressed: return true when consumed (inner navigation) */
	void (*close)(void);               /* app closed from the task switcher */
	/* Optional live tile content. Drawn over the tile colour; r is the tile rectangle. */
	void (*tile)(UI *ui, Rect r, TileSize size, double now);
	bool live;                  /* tile animates (needs frames) */
	/* Launcher-style apps start an external program instead of showing a page.
	 * Return true when handled. */
	bool (*launcher)(void);
	/* Optional count shown on the Start tile (unread items, ringing alarms...). */
	int (*badge)(void);
} AppDef;

/* Settings pages that the global search can jump to */
typedef struct { const char *title; const char *link; const char *keywords; } SettingsPageInfo;
int settings_page_list(const SettingsPageInfo **out);
void photos_rescan(void);
void photos_set_wallpaper(const char *path, bool lock);   /* lock screen or Start background */
/* Opens Photos as a picture chooser; cb runs with the chosen path (NULL = cancelled) and
 * the caller's app is reopened afterwards. */
void photos_pick(const char *return_app, void (*cb)(const char *path, void *ud), void *ud);

const AppDef *app_find(const char *id);
int app_count(void);
const AppDef *app_get(int i);

extern const AppDef app_settings, app_store, app_photos, app_calculator, app_clock, app_calendar,
	app_weather, app_people, app_notes, app_files, app_music, app_internet, app_camera,
	app_video, app_battery, app_storage, app_terminal, app_minesweeper, app_recorder, app_phone;

/* Full-screen overlay above everything, including the lock screen, used by Alarms &
 * Clock for a ringing alarm or timer. clock_overlay_active() decides whether it is shown;
 * Back is offered to clock_overlay_back() first. */
bool clock_overlay_active(void);
/* Incoming call page above everything, the lock screen included (phone.c). */
bool phone_overlay_active(void);
void phone_overlay_draw(UI *ui, Rect r);
void clock_overlay_draw(UI *ui, Rect full);
bool clock_overlay_back(void);

/* ---- page navigation with Metro "turnstile" transitions between pages */
typedef struct {
	int stack[8];
	int depth;                  /* stack[depth] is the current page */
	int from, to;
	Anim anim;
	int dir;
} Nav;
typedef void (*PageDrawFn)(UI *ui, int page, Rect area, void *ud);
void nav_init(Nav *n, int root);
void nav_push(Nav *n, UI *ui, int page);
bool nav_pop(Nav *n, UI *ui);           /* false when already at the root */
int nav_page(const Nav *n);
bool nav_busy(const Nav *n, UI *ui);
void nav_draw(UI *ui, Nav *n, Rect area, PageDrawFn fn, void *ud);

/* Small shared helpers for apps */
Rect app_page_rect(UI *ui, Rect area);          /* inner rect with the standard 12dp margins */
/* Grey explanatory text for empty pages ("no alarms"...), left aligned like on the phone. */
void app_empty(UI *ui, Rect r, const char *text);

/* ---- pivot: small upper-case app title, swipeable lower-case headers and pages.
 * `fn` draws page `page` into r (r is already positioned and clipped). */
typedef void (*PivotPageFn)(UI *ui, int page, Rect r, void *ud);
void app_pivot(UI *ui, const char *key, Pager *pg, Rect area, const char *app_title,
               const char *const *titles, int n, PivotPageFn fn, void *ud);

/* ---- live tiles
 * Cycles through `nfaces` faces every `period` seconds with the Windows Phone "peek"
 * slide (new face pushes the old one up). `key` identifies the tile (staggers timing). */
typedef void (*TileFaceFn)(UI *ui, Rect r, int face, void *ud);
void app_tile_faces(UI *ui, Rect r, const char *key, int nfaces, double period, TileFaceFn fn, void *ud);
/* App name in the bottom-left corner of a medium/wide tile. */
void app_tile_name(UI *ui, Rect r, const char *name);

/* ---- alphabetic list with letter group tiles and the Windows Phone jump grid
 * (tap a letter tile to see all letters, tap one to jump). Items must be sorted
 * with app_collate(); letters[i] comes from app_group_letter(). `top_h` is extra space
 * above the first row that `top` (may be NULL) draws, e.g. a search box. */
typedef struct { Scroll scroll; bool open; Anim anim; } JumpList;
typedef void (*JumpRowFn)(UI *ui, int i, Rect r, void *ud);
typedef void (*JumpTopFn)(UI *ui, Rect r, void *ud);
void jumplist_draw(UI *ui, JumpList *jl, const char *key, Rect view, const char (*letters)[8], int n,
                   double row_h, double top_h, JumpTopFn top, JumpRowFn row, void *ud);
bool jumplist_back(JumpList *jl);
int app_collate(const char *a, const char *b);   /* language aware (Turkish alphabet) */
void app_group_letter(const char *name, char out[8]);   /* "a".."z", Turkish letters or "#" */

/* ---- persistent app data in ~/.local/share/metrowl/ */
char *app_data_file(const char *name);           /* malloc'd path, directory created */
cJSON *app_json_load(const char *name);          /* NULL when missing or invalid */
bool app_json_save(const char *name, const cJSON *root);

/* ---- sounds (generated tones played with pw-play / paplay / aplay when available) */
enum { SND_ALARM, SND_CHIME, SND_TIMER, SND_KEY, SND_WARN, SND_RING };   /* SND_RING: cfg.ringtone */
/* Short event sounds, silent when muted. The notification and warning sounds are the
 * user's choice (cfg.notif_sound / cfg.warn_sound). */
enum { EV_KEY, EV_NOTIF, EV_WARN };
void app_play_event(int ev);
/* Sound files from installed (open source) sound themes and ~/.local/share/sounds,
 * sorted by name. The array stays valid until the next call. */
int app_sound_list(char ***paths);
void app_sound(int kind, bool loop);
void app_sound_stop(void);

/* Async thumbnail / image cache used by Photos, Store and wallpapers. */
typedef struct ImgCache ImgCache;
ImgCache *imgcache_new(int max_entries);
/* Returns the image if ready, else NULL and queues a background decode. */
Image *imgcache_get(ImgCache *c, const char *path, int max_dim);
void imgcache_clear(ImgCache *c);

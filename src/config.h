#pragma once
/* Persistent user settings (~/.config/metrowl/config.json). */
#include <stdbool.h>
#include "cJSON.h"

typedef struct Config {
	char lang[8];              /* "" = follow $LANG, else "en" / "tr" */
	bool dark;
	int accent;                /* index into ACCENT_RGB */
	bool more_tiles;           /* 3 instead of 2 tile columns on phone sized screens */
	bool start_wallpaper;      /* show a picture behind the tiles */
	bool parallax;
	bool live_tiles;
	bool mono_icons;           /* draw third party app icons as flat white silhouettes */
	char wallpaper[512];       /* Start background picture */
	char lock_wallpaper[512];  /* lock screen picture ("" = generated) */
	char pin_hash[65];         /* SHA-256 of the PIN, "" = no PIN */
	bool h24;
	int screen_timeout;        /* seconds, 0 = never */
	double text_scale;
	double ui_scale;           /* 0 = automatic */
	char kb_layout[16];        /* physical keyboard xkb layout ("" = by language) */
	char osk_layouts[32];      /* comma separated on-screen keyboard layouts: "en,tr" */
	bool osk_suggest;
	bool osk_autocap;
	bool toasts;
	bool quiet_hours;
	bool battery_saver;
	bool animations;
	char city[64];
	double lat, lon;
	bool celsius;
	char icon_mode[8];         /* "auto" | "font" | "vector" */
	char home_tz[64];
	char region[8];            /* regional format: "" = follow language, "en" / "tr" */
	bool nav_autohide;         /* hide the navigation bar over Linux apps (swipe up to show) */
	bool phone_apps;           /* Linux apps at phone size (360 px wide pages), see output.c */
	bool key_sound;            /* click when on-screen keys are pressed */
	char notif_sound[256];     /* sound file, "" = built-in chime, "none" = silent */
	char warn_sound[256];      /* same for warnings such as low battery */
} Config;

extern Config cfg;

void config_init(void);              /* defaults + load */
void config_save_soon(void);         /* debounced write */
void config_save_now(void);
cJSON *config_root(void);            /* free-form extra state of other modules */
cJSON *config_section(const char *name, bool array);   /* get-or-create a child */

#include "config.h"
#include "proc.h"
#include "util.h"

Config cfg;
static cJSON *g_root;
static char *g_path;
static Timer *g_save_timer;

static void set_defaults(void) {
	memset(&cfg, 0, sizeof cfg);
	cfg.dark = true;
	cfg.accent = 5;               /* cobalt, the default WP8 blue */
	cfg.more_tiles = true;
	cfg.start_wallpaper = false;
	cfg.parallax = true;
	cfg.live_tiles = true;
	cfg.mono_icons = false;    /* third-party apps keep their own icons */
	cfg.h24 = false;
	cfg.screen_timeout = 60;
	cfg.text_scale = 1.0;
	cfg.ui_scale = 0;
	str_copy(cfg.osk_layouts, sizeof cfg.osk_layouts, "en,tr");
	cfg.osk_suggest = true;
	cfg.osk_autocap = true;
	cfg.toasts = true;
	cfg.animations = true;
	cfg.celsius = true;
	cfg.nav_autohide = true;
	str_copy(cfg.icon_mode, sizeof cfg.icon_mode, "auto");
}

#define GET_STR(key, field) do { \
		cJSON *j = cJSON_GetObjectItemCaseSensitive(g_root, key); \
		if (cJSON_IsString(j)) str_copy(cfg.field, sizeof cfg.field, j->valuestring); } while (0)
#define GET_BOOL(key, field) do { \
		cJSON *j = cJSON_GetObjectItemCaseSensitive(g_root, key); \
		if (cJSON_IsBool(j)) cfg.field = cJSON_IsTrue(j); } while (0)
#define GET_NUM(key, field) do { \
		cJSON *j = cJSON_GetObjectItemCaseSensitive(g_root, key); \
		if (cJSON_IsNumber(j)) cfg.field = j->valuedouble; } while (0)

static void load_fields(void) {
	GET_STR("lang", lang);
	GET_BOOL("dark", dark);
	GET_NUM("accent", accent);
	cfg.accent = CLAMP(cfg.accent, 0, 19);
	GET_BOOL("more_tiles", more_tiles);
	GET_BOOL("start_wallpaper", start_wallpaper);
	GET_BOOL("parallax", parallax);
	GET_BOOL("live_tiles", live_tiles);
	GET_BOOL("mono_icons", mono_icons);
	GET_STR("wallpaper", wallpaper);
	GET_STR("lock_wallpaper", lock_wallpaper);
	GET_STR("pin_hash", pin_hash);
	GET_BOOL("h24", h24);
	GET_NUM("screen_timeout", screen_timeout);
	GET_NUM("text_scale", text_scale);
	GET_NUM("ui_scale", ui_scale);
	GET_STR("kb_layout", kb_layout);
	GET_STR("osk_layouts", osk_layouts);
	GET_BOOL("osk_suggest", osk_suggest);
	GET_BOOL("osk_autocap", osk_autocap);
	GET_BOOL("toasts", toasts);
	GET_BOOL("quiet_hours", quiet_hours);
	GET_BOOL("battery_saver", battery_saver);
	GET_BOOL("animations", animations);
	GET_STR("city", city);
	GET_NUM("lat", lat);
	GET_NUM("lon", lon);
	GET_BOOL("celsius", celsius);
	GET_STR("icon_mode", icon_mode);
	GET_STR("home_tz", home_tz);
	GET_STR("region", region);
	GET_BOOL("nav_autohide", nav_autohide);
	/* one-time switch to full-colour third-party icons (older configs saved monochrome) */
	if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g_root, "colour_icons_v1"))) {
		cfg.mono_icons = false;
		cJSON_DeleteItemFromObjectCaseSensitive(g_root, "colour_icons_v1");
		cJSON_AddBoolToObject(g_root, "colour_icons_v1", true);
	}
}

#define PUT_STR(key, field) cJSON_ReplaceItemInObjectCaseSensitive(g_root, key, cJSON_CreateString(cfg.field)) \
	? (void)0 : (void)cJSON_AddStringToObject(g_root, key, cfg.field)
#define PUT_BOOL(key, field) do { cJSON_DeleteItemFromObjectCaseSensitive(g_root, key); cJSON_AddBoolToObject(g_root, key, cfg.field); } while (0)
#define PUT_NUM(key, field) do { cJSON_DeleteItemFromObjectCaseSensitive(g_root, key); cJSON_AddNumberToObject(g_root, key, cfg.field); } while (0)
#define PUT_S(key, field) do { cJSON_DeleteItemFromObjectCaseSensitive(g_root, key); cJSON_AddStringToObject(g_root, key, cfg.field); } while (0)

static void store_fields(void) {
	PUT_S("lang", lang);
	PUT_BOOL("dark", dark);
	PUT_NUM("accent", accent);
	PUT_BOOL("more_tiles", more_tiles);
	PUT_BOOL("start_wallpaper", start_wallpaper);
	PUT_BOOL("parallax", parallax);
	PUT_BOOL("live_tiles", live_tiles);
	PUT_BOOL("mono_icons", mono_icons);
	PUT_S("wallpaper", wallpaper);
	PUT_S("lock_wallpaper", lock_wallpaper);
	PUT_S("pin_hash", pin_hash);
	PUT_BOOL("h24", h24);
	PUT_NUM("screen_timeout", screen_timeout);
	PUT_NUM("text_scale", text_scale);
	PUT_NUM("ui_scale", ui_scale);
	PUT_S("kb_layout", kb_layout);
	PUT_S("osk_layouts", osk_layouts);
	PUT_BOOL("osk_suggest", osk_suggest);
	PUT_BOOL("osk_autocap", osk_autocap);
	PUT_BOOL("toasts", toasts);
	PUT_BOOL("quiet_hours", quiet_hours);
	PUT_BOOL("battery_saver", battery_saver);
	PUT_BOOL("animations", animations);
	PUT_S("city", city);
	PUT_NUM("lat", lat);
	PUT_NUM("lon", lon);
	PUT_BOOL("celsius", celsius);
	PUT_S("icon_mode", icon_mode);
	PUT_S("home_tz", home_tz);
	PUT_S("region", region);
	PUT_BOOL("nav_autohide", nav_autohide);
}

cJSON *config_root(void) { return g_root; }

cJSON *config_section(const char *name, bool array) {
	cJSON *j = cJSON_GetObjectItemCaseSensitive(g_root, name);
	if (j && (array ? cJSON_IsArray(j) : cJSON_IsObject(j))) return j;
	if (j) cJSON_DeleteItemFromObjectCaseSensitive(g_root, name);
	j = array ? cJSON_CreateArray() : cJSON_CreateObject();
	cJSON_AddItemToObject(g_root, name, j);
	return j;
}

void config_init(void) {
	set_defaults();
	char *dir = xdg_dir("XDG_CONFIG_HOME", ".config", "metrowl");
	g_path = path_join(dir, "config.json");
	free(dir);
	char *txt = file_read_all(g_path, NULL);
	if (txt) {
		g_root = cJSON_Parse(txt);
		free(txt);
	}
	if (!g_root || !cJSON_IsObject(g_root)) {
		cJSON_Delete(g_root);
		g_root = cJSON_CreateObject();
	}
	load_fields();
}

void config_save_now(void) {
	if (!g_root) return;
	store_fields();
	char *txt = cJSON_Print(g_root);
	if (txt) {
		if (!file_write_all(g_path, txt, strlen(txt))) LOGE("could not write %s", g_path);
		free(txt);
	}
}

static void save_cb(void *ud) { config_save_now(); }

void config_save_soon(void) {
	if (!g_save_timer) g_save_timer = timer_add(0, false, save_cb, NULL);
	timer_set(g_save_timer, 400);
}

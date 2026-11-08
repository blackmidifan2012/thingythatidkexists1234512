#pragma once
/* System services: battery, brightness, volume, radios, power, device info.
 * Everything degrades gracefully when the hardware / helper tool is absent. */
#include <stdbool.h>
#include <stddef.h>

typedef struct { bool present, charging, full; int pct; } Battery;
typedef struct { bool available, enabled, connected; char ssid[64]; int signal; /*0..100*/ } WifiInfo;
typedef struct { char ssid[64]; int signal; bool secured, active, known; } WifiNet;
typedef struct {
	char os[96], kernel[64], hostname[64], cpu[96], arch[16], wlroots[32];
	double mem_total, mem_avail, disk_total, disk_free, uptime;
} SysInfo;

void sys_init(bool demo);
bool sys_demo(void);

Battery sys_battery(void);

/* Accelerometer (IIO), for rotating apps: which edge of the phone points up. */
enum { ORIENT_NORMAL, ORIENT_LEFT_UP, ORIENT_RIGHT_UP, ORIENT_BOTTOM_UP };
bool sys_has_accel(void);
int sys_orientation(int prev);
void sys_force_orientation(int o);              /* tests: -1 = use the sensor */

bool sys_has_backlight(void);
int sys_brightness(void);                 /* 0..100 */
void sys_set_brightness(int pct);

bool sys_has_audio(void);
int sys_volume(void);                     /* 0..100 */
bool sys_muted(void);
void sys_set_volume(int pct);
void sys_set_mute(bool m);

WifiInfo sys_wifi(void);
void sys_wifi_set_enabled(bool on);
void sys_wifi_scan(void);                 /* async; poll sys_wifi_list */
const WifiNet *sys_wifi_list(int *n);
bool sys_wifi_scanning(void);
void sys_wifi_connect(const char *ssid, const char *password);
const char *sys_wifi_status_msg(void);    /* last connect result, "" if none */

bool sys_bluetooth_available(void);
bool sys_bluetooth_enabled(void);
void sys_bluetooth_set(bool on);
bool sys_airplane(void);
void sys_set_airplane(bool on);

void sys_poweroff(void);
void sys_reboot(void);
void sys_suspend(void);

void sys_info(SysInfo *out);
/* Online connectivity guess (default route present). */
bool sys_online(void);

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/** Verbose USB Serial traces (ticker UI + LAN IP in sketch). Override with `-DBITVAVO_MONITOR_SERIAL_DEBUG=1`. */
#ifndef BITVAVO_MONITOR_SERIAL_DEBUG
#define BITVAVO_MONITOR_SERIAL_DEBUG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

void app_settings_begin(void);

/** Erase WiFi, time/NTP, market, favorites, and other keys in the app NVS namespace. */
void app_settings_clear_all(void);

/** POSIX TZ string for setenv("TZ") — from timezone index or legacy NVS key "tz". */
void app_settings_get_tz(char *buf, size_t cap);
void app_settings_set_tz(const char *tz);

/** 24 standard zones: index 0 = UTC-12 … 23 = UTC+11; default 12 = UTC+0 when unset. */
uint8_t app_settings_get_timezone_index(void);
void app_settings_set_timezone_index(uint8_t idx);
/** Build POSIX TZ for dropdown index (ESP32: "GMT±N" fixed offsets); idx 0..23. */
void app_settings_tz_from_index(uint8_t idx, char *buf, size_t cap);

/** Primary + optional secondary NTP hostnames (pass nullptr or "" to clear secondary). */
void app_settings_get_ntp(char *primary, size_t pcap, char *secondary, size_t scap);
void app_settings_set_ntp(const char *primary, const char *secondary);

/** Stored STA credentials; returns false if SSID not saved. */
bool app_settings_get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
void app_settings_set_wifi(const char *ssid, const char *pass);

/** Bitvavo market id e.g. BTC-EUR */
void app_settings_get_market(char *buf, size_t cap);
void app_settings_set_market(const char *market);

bool app_settings_fav_contains(const char *market);
void app_settings_fav_add(const char *market);
void app_settings_fav_remove(const char *market);

/** Restart lwIP SNTP using stored TZ + NTP hosts (Arduino configTzTime; same stack as configTime). */
void app_ntp_apply_from_settings(void);

/** Reapply TZ + restart SNTP using stored NTP hosts (call after saving config). */
void app_settings_apply_time_and_ntp(void);

/** Wall clock from SNTP: false until time() looks valid, then fills *tm via localtime_r. */
bool app_time_local_tm(struct tm *tm);

#ifdef __cplusplus
}

#include <Arduino.h>

/** One NVS read of comma-separated favorites (avoid per-row fav_contains during UI builds). */
void app_settings_fav_get_blob(String *out);
#endif

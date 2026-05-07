#include "app_settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static Preferences prefs;
static bool s_open;

#ifndef APP_DEFAULT_TZ
#define APP_DEFAULT_TZ "CET-1CEST,M3.5.0/2,M10.5.0/3"
#endif
#ifndef APP_DEFAULT_NTP0
#define APP_DEFAULT_NTP0 "europe.pool.ntp.org"
#endif
#ifndef APP_DEFAULT_NTP1
#define APP_DEFAULT_NTP1 "pool.ntp.org"
#endif
#ifndef APP_DEFAULT_MARKET
#define APP_DEFAULT_MARKET "BTC-EUR"
#endif

void app_settings_begin(void)
{
  if (s_open) {
    return;
  }
  prefs.begin("btcmon", false);
  s_open = true;
}

static void ensure_open(void)
{
  if (!s_open) {
    app_settings_begin();
  }
}

void app_settings_clear_all(void)
{
  ensure_open();
  prefs.clear();
}

static void tz_index_to_posix(uint8_t idx, char *buf, size_t cap)
{
  if (idx > 23) {
    idx = 12;
  }
  int off = static_cast<int>(idx) - 12;
  if (off == 0) {
    snprintf(buf, cap, "GMT0");
    return;
  }
  /*
   * ESP32 newlib (Arduino configTzTime/setenv): use GMT± form, not "Etc/GMT" —
   * otherwise TZ is often ignored and local time stays at UTC.
   * Convention matches ESP examples (e.g. UTC+8 → "GMT-8", "CST-8").
   */
  snprintf(buf, cap, "GMT%+d", -off);
}

void app_settings_tz_from_index(uint8_t idx, char *buf, size_t cap)
{
  if (!buf || cap == 0) {
    return;
  }
  tz_index_to_posix(idx, buf, cap);
}

uint8_t app_settings_get_timezone_index(void)
{
  ensure_open();
  if (!prefs.isKey("tz_i")) {
    return 12;
  }
  uint8_t v = prefs.getUChar("tz_i", 12);
  if (v > 23) {
    return 12;
  }
  return v;
}

void app_settings_set_timezone_index(uint8_t idx)
{
  ensure_open();
  if (idx > 23) {
    idx = 12;
  }
  prefs.putUChar("tz_i", idx);
  prefs.remove("tz");
}

void app_settings_get_tz(char *buf, size_t cap)
{
  ensure_open();
  if (!buf || cap == 0) {
    return;
  }
  if (prefs.isKey("tz_i")) {
    uint8_t ix = prefs.getUChar("tz_i", 12);
    if (ix > 23) {
      ix = 12;
    }
    tz_index_to_posix(ix, buf, cap);
    return;
  }
  String s = prefs.getString("tz", APP_DEFAULT_TZ);
  strncpy(buf, s.c_str(), cap - 1);
  buf[cap - 1] = '\0';
}

void app_settings_set_tz(const char *tz)
{
  ensure_open();
  if (tz && tz[0]) {
    prefs.putString("tz", tz);
    prefs.remove("tz_i");
  }
}

void app_settings_get_ntp(char *primary, size_t pcap, char *secondary, size_t scap)
{
  ensure_open();
  if (primary && pcap) {
    String s = prefs.getString("ntp0", APP_DEFAULT_NTP0);
    strncpy(primary, s.c_str(), pcap - 1);
    primary[pcap - 1] = '\0';
  }
  if (secondary && scap) {
    String s = prefs.getString("ntp1", "");
    if (s.length() == 0 && !prefs.isKey("ntp1")) {
      s = String(APP_DEFAULT_NTP1);
    }
    strncpy(secondary, s.c_str(), scap - 1);
    secondary[scap - 1] = '\0';
  }
}

void app_settings_set_ntp(const char *primary, const char *secondary)
{
  ensure_open();
  if (primary && primary[0]) {
    prefs.putString("ntp0", primary);
  }
  if (!secondary || !secondary[0]) {
    prefs.putString("ntp1", "");
  } else {
    prefs.putString("ntp1", secondary);
  }
}

bool app_settings_get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
  ensure_open();
  if (!ssid || ssid_cap == 0) {
    return false;
  }
  String s = prefs.getString("wifi_ssid", "");
  if (s.length() == 0) {
    ssid[0] = '\0';
    if (pass && pass_cap) {
      pass[0] = '\0';
    }
    return false;
  }
  strncpy(ssid, s.c_str(), ssid_cap - 1);
  ssid[ssid_cap - 1] = '\0';
  if (pass && pass_cap) {
    String p = prefs.getString("wifi_pass", "");
    strncpy(pass, p.c_str(), pass_cap - 1);
    pass[pass_cap - 1] = '\0';
  }
  return true;
}

void app_settings_set_wifi(const char *ssid, const char *pass)
{
  ensure_open();
  if (ssid) {
    prefs.putString("wifi_ssid", ssid);
  }
  if (pass) {
    prefs.putString("wifi_pass", pass);
  }
}

void app_settings_get_market(char *buf, size_t cap)
{
  ensure_open();
  if (!buf || cap == 0) {
    return;
  }
  String s = prefs.getString("mkt", APP_DEFAULT_MARKET);
  strncpy(buf, s.c_str(), cap - 1);
  buf[cap - 1] = '\0';
}

void app_settings_set_market(const char *market)
{
  ensure_open();
  if (market && market[0]) {
    prefs.putString("mkt", market);
  }
}

static String favorites_blob(void)
{
  return prefs.getString("fav", "");
}

void app_settings_fav_get_blob(String *out)
{
  ensure_open();
  if (out) {
    *out = favorites_blob();
  }
}

bool app_settings_fav_contains(const char *market)
{
  ensure_open();
  if (!market || !market[0]) {
    return false;
  }
  String blob = favorites_blob();
  String needle = String(market);
  int pos = 0;
  while (pos < blob.length()) {
    int c = blob.indexOf(',', pos);
    if (c < 0) {
      c = blob.length();
    }
    String tok = blob.substring(pos, c);
    tok.trim();
    if (tok == needle) {
      return true;
    }
    pos = c + 1;
  }
  return false;
}

void app_settings_fav_add(const char *market)
{
  ensure_open();
  if (!market || !market[0]) {
    return;
  }
  if (app_settings_fav_contains(market)) {
    return;
  }
  String blob = favorites_blob();
  if (blob.length() == 0) {
    blob = market;
  } else {
    blob += ",";
    blob += market;
  }
  prefs.putString("fav", blob);
}

void app_settings_fav_remove(const char *market)
{
  ensure_open();
  if (!market || !market[0]) {
    return;
  }
  String blob = favorites_blob();
  String out;
  int pos = 0;
  while (pos < blob.length()) {
    int c = blob.indexOf(',', pos);
    if (c < 0) {
      c = blob.length();
    }
    String tok = blob.substring(pos, c);
    tok.trim();
    if (tok.length() && tok != String(market)) {
      if (out.length()) {
        out += ",";
      }
      out += tok;
    }
    pos = c + 1;
  }
  prefs.putString("fav", out);
}

void app_settings_apply_time_and_ntp(void)
{
  app_ntp_apply_from_settings();
}

void app_ntp_apply_from_settings(void)
{
  ensure_open();
  char tz[48];
  char n0[64], n1[64];
  app_settings_get_tz(tz, sizeof tz);
  app_settings_get_ntp(n0, sizeof n0, n1, sizeof n1);
  if (!n0[0]) {
    strncpy(n0, APP_DEFAULT_NTP0, sizeof n0 - 1);
    n0[sizeof n0 - 1] = '\0';
  }
  const char *s2 = (n1[0] != '\0') ? n1 : APP_DEFAULT_NTP1;
  configTzTime(tz, n0, s2, "time.google.com");
}

bool app_time_local_tm(struct tm *tm)
{
  if (!tm) {
    return false;
  }
  time_t now = time(nullptr);
  localtime_r(&now, tm);
  return tm->tm_year > (2016 - 1900);
}

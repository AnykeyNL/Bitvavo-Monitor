#include "settings_ui.h"

#include "app_settings.h"
#include "bitvavo_monitor_ui.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <vector>
#include <lvgl.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#ifndef INDICATOR_BTN_GPIO
#define INDICATOR_BTN_GPIO 38
#endif

#if BITVAVO_MONITOR_SERIAL_DEBUG
#define MKT_SER_PRINTF(...) Serial.printf(__VA_ARGS__)
#define MKT_SER_PRINTLN(s) Serial.println(s)
#define MKT_SER_FLUSH() Serial.flush()
/** Serial trace: millis + heap + LVGL pool (for ticker list / markets fetch issues). */
static void mkt_ui_snap(const char *label)
{
  Serial.printf("[mkt-ui] %lu ms | %s", (unsigned long)millis(), label);
#if defined(ESP32)
  Serial.printf(" | heap=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  Serial.printf(" psram_free=%u", (unsigned)psram_free);
#endif
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  Serial.printf(" lv_free=%u lv_used=%u lv_frag=%u%% lv_max=%u",
                (unsigned)mon.free_size, (unsigned)mon.total_size - (unsigned)mon.free_size,
                (unsigned)mon.frag_pct, (unsigned)mon.max_used);
  Serial.println();
  Serial.flush();
}
#else
#define MKT_SER_PRINTF(...) ((void)0)
#define MKT_SER_PRINTLN(s) ((void)0)
#define MKT_SER_FLUSH() ((void)0)
static void mkt_ui_snap(const char *) {}
#endif

static bool s_settings_active;
static lv_obj_t *s_kb;

static lv_obj_t *s_menu;
static lv_obj_t *s_wifi;
static lv_obj_t *s_wifi_pw;
static lv_obj_t *s_config;
static lv_obj_t *s_ticker;
static lv_obj_t *s_info;
static lv_obj_t *s_reset_confirm;

/** Filled by settings_ui_init() before build_screens() — Info menu text. */
static const char *s_app_version;
static const char *s_app_version_date;

static lv_obj_t *s_wifi_scroll;
static lv_obj_t *s_lbl_wifi_stat;
static lv_obj_t *s_lbl_wifi_detail;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_lbl_pw_stat;
static lv_obj_t *s_dd_tz;
static lv_obj_t *s_ta_ntp0;
static lv_obj_t *s_lbl_cfg_clock;
static lv_obj_t *s_lbl_cfg_ntp;
static lv_obj_t *s_ta_filter;
static lv_obj_t *s_tick_scroll;
static lv_obj_t *s_tick_list;
static lv_obj_t *s_lbl_tick_stat;

/** Incremental ticker list build (full rebuild in one LVGL tick hangs the device). */
static lv_timer_t *s_mkt_row_timer = nullptr;
static lv_timer_t *s_filter_debounce = nullptr;
/** Non-blocking STA connect from password screen (LVGL stays responsive). */
static lv_timer_t *s_wifi_connect_timer = nullptr;
static uint32_t s_wifi_connect_started_ms;
static char s_wifi_connect_pass[65];
/** When non-zero: run market_rebuild_rows() once millis() reaches this (main loop — not inside lv_task_handler). */
static uint32_t s_ticker_rebuild_due_ms;
static size_t s_mkt_b_mi;
static uint16_t s_mkt_b_shown;
static String s_mkt_b_needle;
static String s_mkt_b_fav_blob;

/** Ignore dropdown events while syncing from NVS (opening screen). */
static bool s_dd_tz_programmatic;
static char s_sel_ssid[33];
static std::vector<String> s_markets;
static bool s_markets_loaded;

static const char *k_zone_dropdown_opts =
    "UTC-12:00\n"
    "UTC-11:00\n"
    "UTC-10:00\n"
    "UTC-09:00\n"
    "UTC-08:00\n"
    "UTC-07:00\n"
    "UTC-06:00\n"
    "UTC-05:00\n"
    "UTC-04:00\n"
    "UTC-03:00\n"
    "UTC-02:00\n"
    "UTC-01:00\n"
    "UTC+00:00\n"
    "UTC+01:00\n"
    "UTC+02:00\n"
    "UTC+03:00\n"
    "UTC+04:00\n"
    "UTC+05:00\n"
    "UTC+06:00\n"
    "UTC+07:00\n"
    "UTC+08:00\n"
    "UTC+09:00\n"
    "UTC+10:00\n"
    "UTC+11:00";

static void go_main_chart(void);
static void kb_hide(void);
static void kb_show_for(lv_obj_t *ta);
static void market_cancel_row_build_timer(void);
static void filter_debounce_cancel(void);

static void restore_tz_from_nvs(void)
{
  char tz[48];
  app_settings_get_tz(tz, sizeof tz);
  setenv("TZ", tz, 1);
  tzset();
}

static void dd_tz_preview_evt(lv_event_t *e)
{
  if (s_dd_tz_programmatic) {
    return;
  }
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
    return;
  }
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  uint16_t sel = lv_dropdown_get_selected(dd);
  if (sel > 23) {
    return;
  }
  char tzbuf[48];
  app_settings_tz_from_index(static_cast<uint8_t>(sel), tzbuf, sizeof tzbuf);
  setenv("TZ", tzbuf, 1);
  tzset();
}

static void ta_kb_event(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
  if (code == LV_EVENT_FOCUSED) {
    kb_show_for(ta);
  } else if (code == LV_EVENT_DEFOCUSED) {
    kb_hide();
  }
}

static void kb_hide(void)
{
  if (s_kb) {
    lv_keyboard_set_textarea(s_kb, nullptr);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

static void kb_show_for(lv_obj_t *ta)
{
  if (!s_kb || !ta) {
    return;
  }
  lv_keyboard_set_textarea(s_kb, ta);
  lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void go_main_chart(void)
{
  kb_hide();
  market_cancel_row_build_timer();
  filter_debounce_cancel();
  restore_tz_from_nvs();
  s_settings_active = false;
  char mk[24];
  app_settings_get_market(mk, sizeof mk);
  bitvavo_monitor_ui_set_market_heading(mk);
  lv_screen_load(bitvavo_monitor_ui_get_screen());
  char ssid[33], pass[65];
  if (!app_settings_get_wifi(ssid, sizeof ssid, pass, sizeof pass)) {
    bitvavo_monitor_ui_show_network_setup_hint();
  }
}

static void go_menu_root(void)
{
  kb_hide();
  market_cancel_row_build_timer();
  filter_debounce_cancel();
  restore_tz_from_nvs();
  s_settings_active = true;
  lv_screen_load(s_menu);
}

static void style_screen(lv_obj_t *scr)
{
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, 480, 480);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1419), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

static void add_back_to_menu(lv_obj_t *scr, int y)
{
  lv_obj_t *b = lv_button_create(scr);
  lv_obj_set_size(b, 120, 36);
  lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, y);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, "< Menu");
  lv_obj_center(l);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        LV_UNUSED(e);
        go_menu_root();
      },
      LV_EVENT_CLICKED, nullptr);
}

static bool fetch_markets_blocking(void)
{
  s_markets.clear();
  if (WiFi.status() != WL_CONNECTED) {
    MKT_SER_PRINTF("[markets] fail: WiFi status=%d (not WL_CONNECTED=%d)\n", (int)WiFi.status(),
                   (int)WL_CONNECTED);
    return false;
  }
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(120000);

  HTTPClient http;
  http.setTimeout(120000);
  /* Helps ESP32 + TLS: stream-based JSON parse often gets IncompleteInput without this. */
  http.useHTTP10(true);

  if (!http.begin(cli, "https://api.bitvavo.com/v2/markets")) {
    MKT_SER_PRINTLN("[markets] fail: http.begin() returned false");
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    MKT_SER_PRINTF("[markets] fail: HTTP GET code=%d (%s)\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  /*
   * ~190 KB response. Avoid String(getString()) — double allocation + fragmentation.
   * Avoid deserializeJson(http.getStream()) — often fails on ESP32 (IncompleteInput / timeouts).
   * Read body into one buffer (PSRAM if present), then parse with a filter (only "market").
   */
  JsonDocument filter;
  filter[0]["market"] = true;
  JsonDocument doc;

  int contentLen = http.getSize();
  WiFiClient &stream = http.getStream();
  DeserializationError err = DeserializationError::Ok;

  MKT_SER_PRINTF("[markets] GET ok, Content-Length(raw)=%d stream_available=%d\n", contentLen,
                 (int)stream.available());

  if (contentLen > 0 && contentLen < 600 * 1024) {
    size_t sz = (size_t)contentLen + 1u;
    uint8_t *body = nullptr;
    const char *alloc_src = "heap";
#if defined(ESP32)
    if (ESP.getPsramSize() > 0) {
      body = static_cast<uint8_t *>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (body) {
        alloc_src = "psram";
      }
    }
#endif
    if (!body) {
      body = static_cast<uint8_t *>(malloc(sz));
      alloc_src = "heap";
    }
    if (!body) {
      MKT_SER_PRINTF("[markets] fail: malloc(%u) failed (%s)\n", (unsigned)sz, alloc_src);
      http.end();
      return false;
    }
    MKT_SER_PRINTF("[markets] buffer %u bytes via %s\n", (unsigned)sz, alloc_src);

    /*
     * ESP32 WiFiClient/Secure: read(buf, len) with len > available() often returns -1
     * when the stack is waiting for more TCP/TLS data (see arduino-esp32 #4390 / #4435).
     * Only request min(available(), remaining). If nothing available yet, delay — never
     * call read with no bytes ready.
     */
    size_t nread = 0;
    const uint32_t k_stall_ms = 120000;
    uint32_t last_prog = millis();
    while (nread < (size_t)contentLen) {
      int avail = stream.available();
      if (avail <= 0) {
        if (!stream.connected()) {
          break;
        }
        if ((millis() - last_prog) > k_stall_ms) {
          MKT_SER_PRINTF("[markets] fail: no progress for %lu ms at offset %u/%d\n",
                         (unsigned long)k_stall_ms, (unsigned)nread, contentLen);
          break;
        }
        delay(2);
#if defined(ESP32)
        yield();
#endif
        continue;
      }

      size_t need = (size_t)contentLen - nread;
      size_t chunk = (size_t)avail;
      if (chunk > need) {
        chunk = need;
      }
      int n = stream.read(body + nread, chunk);
      if (n > 0) {
        nread += (size_t)n;
        last_prog = millis();
        continue;
      }
      /* avail>0 but read stalled: treat like "wait" (also handles stray -1). */
      if (!stream.connected() && stream.available() <= 0) {
        break;
      }
      if ((millis() - last_prog) > k_stall_ms) {
        MKT_SER_PRINTF("[markets] fail: read stall at offset %u (last read=%d)\n", (unsigned)nread, n);
        break;
      }
      delay(2);
#if defined(ESP32)
      yield();
#endif
    }

    if (nread != (size_t)contentLen) {
      MKT_SER_PRINTF("[markets] fail: body read got %u expected %u (avail=%d connected=%d)\n",
                     (unsigned)nread, (unsigned)contentLen, (int)stream.available(),
                     (int)stream.connected());
      free(body);
      http.end();
      return false;
    }
    body[contentLen] = '\0';
    http.end();

    err = deserializeJson(doc, reinterpret_cast<const char *>(body),
                        DeserializationOption::Filter(filter));
    if (err) {
      int head = contentLen < 160 ? contentLen : 160;
      char peek[164];
      memcpy(peek, body, (size_t)head);
      peek[head] = '\0';
      MKT_SER_PRINTF("[markets] fail: deserializeJson err=%s code=%d overflowed=%d head=\"%s\"\n",
                     err.c_str(), (int)err.code(), (int)doc.overflowed(), peek);
    }
    free(body);
  } else {
    MKT_SER_PRINTLN("[markets] path: stream parse (no Content-Length or size out of range)");
    err = deserializeJson(doc, stream, DeserializationOption::Filter(filter));
    http.end();
    if (err) {
      MKT_SER_PRINTF("[markets] fail: stream deserializeJson err=%s code=%d overflowed=%d\n",
                     err.c_str(), (int)err.code(), (int)doc.overflowed());
    }
  }

  if (err) {
    return false;
  }
  JsonArray ar = doc.as<JsonArray>();
  if (ar.isNull()) {
    MKT_SER_PRINTLN("[markets] fail: root is not a JSON array (as<JsonArray>().isNull())");
    return false;
  }
  MKT_SER_PRINTF("[markets] JSON array size=%u\n", (unsigned)ar.size());
  s_markets.reserve(ar.size());
  for (JsonObject o : ar) {
    const char *m = o["market"];
    if (m && m[0]) {
      s_markets.push_back(String(m));
    }
  }
  std::sort(s_markets.begin(), s_markets.end());
  if (s_markets.empty()) {
    MKT_SER_PRINTLN("[markets] fail: no \"market\" strings extracted (empty vector after parse)");
    return false;
  }
  MKT_SER_PRINTF("[markets] ok: %u symbols\n", (unsigned)s_markets.size());
  return true;
}

static const char *wifi_status_label(int st)
{
  switch (st) {
    case WL_CONNECTED:
      return "Connected";
    case WL_CONNECT_FAILED:
      return "Connect failed";
    case WL_CONNECTION_LOST:
      return "Lost";
    case WL_DISCONNECTED:
      return "Disconnected";
    case WL_IDLE_STATUS:
      return "Idle";
    case WL_NO_SSID_AVAIL:
      return "No SSID";
    case WL_SCAN_COMPLETED:
      return "Scan completed";
    default:
      return "Unknown";
  }
}

static void wifi_update_connection_detail(void)
{
  if (!s_lbl_wifi_detail) {
    return;
  }
  int st = WiFi.status();
  char buf[320];
  if (st == WL_CONNECTED) {
    IPAddress dns = WiFi.dnsIP();
    IPAddress dns2 = WiFi.dnsIP(1);
    char dns2line[48];
    if (dns2 != IPAddress(0u, 0u, 0u, 0u)) {
      snprintf(dns2line, sizeof dns2line, ", %s", dns2.toString().c_str());
    } else {
      dns2line[0] = '\0';
    }
    snprintf(buf, sizeof buf,
             "Connection: %s\n"
             "SSID: %s\n"
             "IP: %s\n"
             "Gateway: %s\n"
             "DNS: %s%s",
             wifi_status_label(st), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
             WiFi.gatewayIP().toString().c_str(), dns.toString().c_str(), dns2line);
  } else {
    snprintf(buf, sizeof buf,
             "Connection: %s\n"
             "SSID: —\n"
             "IP: —\n"
             "Gateway: —\n"
             "DNS: —",
             wifi_status_label(st));
  }
  lv_label_set_text(s_lbl_wifi_detail, buf);
}

static void wifi_start_scanning_ui(void)
{
  while (lv_obj_get_child_cnt(s_wifi_scroll) > 0) {
    lv_obj_del(lv_obj_get_child(s_wifi_scroll, 0));
  }
  lv_label_set_text(s_lbl_wifi_stat, "Scanning networks…");
  wifi_update_connection_detail();
  if (s_wifi) {
    lv_obj_invalidate(s_wifi);
  }
}

static void wifi_run_scan_fill(void)
{
  wifi_update_connection_detail();

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    lv_label_set_text(s_lbl_wifi_stat, n == 0 ? "No networks" : "Scan failed");
    wifi_update_connection_detail();
    return;
  }

  lv_label_set_text(s_lbl_wifi_stat, "Tap a network");

  std::vector<int> idx;
  idx.reserve((size_t)n);
  for (int i = 0; i < n; i++) {
    idx.push_back(i);
  }
  std::sort(idx.begin(), idx.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

  for (int k : idx) {
    String ssid = WiFi.SSID(k);

    lv_obj_t *row = lv_button_create(s_wifi_scroll);
    lv_obj_set_width(row, lv_pct(96));
    lv_obj_set_height(row, 42);
    lv_obj_t *lab = lv_label_create(row);
    char line[80];
    snprintf(line, sizeof line, "%s  (%d dBm)%s", ssid.c_str(), WiFi.RSSI(k),
             WiFi.encryptionType(k) == WIFI_AUTH_OPEN ? "  open" : "");
    lv_label_set_text(lab, line);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(lab, 400);

    lv_obj_add_event_cb(
        row,
        [](lv_event_t *e) {
          int kk = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
          String ss = WiFi.SSID(kk);
          strncpy(s_sel_ssid, ss.c_str(), sizeof s_sel_ssid - 1);
          s_sel_ssid[sizeof s_sel_ssid - 1] = '\0';
          lv_textarea_set_text(s_ta_pass, "");
          lv_label_set_text(s_lbl_pw_stat, s_sel_ssid);
          kb_hide();
          lv_screen_load(s_wifi_pw);
        },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(k)));
  }

  wifi_update_connection_detail();
}

static void wifi_scan_timer_cb(lv_timer_t *t)
{
  lv_timer_delete(t);
  wifi_run_scan_fill();
}

/** Show Wi-Fi screen, then scan after LVGL can render "Scanning…". */
static void wifi_schedule_scan(void)
{
  wifi_start_scanning_ui();
  lv_timer_create(wifi_scan_timer_cb, 15, nullptr);
}

static void wifi_connect_timer_cb(lv_timer_t *t)
{
  if (!s_wifi_connect_timer || t != s_wifi_connect_timer) {
    return;
  }
  if (lv_screen_active() != s_wifi_pw) {
    lv_timer_delete(s_wifi_connect_timer);
    s_wifi_connect_timer = nullptr;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false);
    }
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    lv_timer_delete(s_wifi_connect_timer);
    s_wifi_connect_timer = nullptr;
    app_settings_set_wifi(s_sel_ssid, s_wifi_connect_pass);
    lv_textarea_set_text(s_ta_pass, "");
    lv_label_set_text(s_lbl_pw_stat, s_sel_ssid);
    lv_screen_load(s_wifi);
    wifi_update_connection_detail();
    wifi_schedule_scan();
    return;
  }
  if ((uint32_t)(millis() - s_wifi_connect_started_ms) >= 30000UL) {
    lv_timer_delete(s_wifi_connect_timer);
    s_wifi_connect_timer = nullptr;
    WiFi.disconnect(false);
    lv_textarea_set_text(s_ta_pass, "");
    lv_label_set_text(s_lbl_pw_stat, s_sel_ssid);
    lv_screen_load(s_wifi);
    wifi_update_connection_detail();
    wifi_schedule_scan();
  }
}

/** Match market id against one NVS blob read (same rules as app_settings_fav_contains). */
static bool market_fav_blob_has(const String &blob, const char *market)
{
  if (!market || !market[0]) {
    return false;
  }
  String needle(market);
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

static void market_rebuild_rows(void);

static void filter_debounce_cb(lv_timer_t *t)
{
  mkt_ui_snap("debounce_cb: enter");
  lv_timer_delete(t);
  s_filter_debounce = nullptr;
  mkt_ui_snap("debounce_cb: before market_rebuild_rows");
  market_rebuild_rows();
  mkt_ui_snap("debounce_cb: after market_rebuild_rows");
}

static void filter_debounce_cancel(void)
{
  if (s_filter_debounce) {
    mkt_ui_snap("filter_debounce_cancel: had pending");
    lv_timer_delete(s_filter_debounce);
    s_filter_debounce = nullptr;
  }
}

static void market_cancel_row_build_timer(void)
{
  if (s_mkt_row_timer) {
    mkt_ui_snap("row_build_timer: cancel/delete");
    lv_timer_delete(s_mkt_row_timer);
    s_mkt_row_timer = nullptr;
  }
}

static void market_rows_batch_cb(lv_timer_t *t)
{
  LV_UNUSED(t);
  if (!s_tick_scroll || !s_tick_list || !s_ta_filter) {
    mkt_ui_snap("batch_cb: bad pointers -> cancel");
    market_cancel_row_build_timer();
    return;
  }

  MKT_SER_PRINTF("[mkt-ui] batch_cb enter mi=%u shown=%u markets=%u sel_scr=%p\n",
                 (unsigned)s_mkt_b_mi, (unsigned)s_mkt_b_shown, (unsigned)s_markets.size(),
                 (void *)lv_screen_active());
  MKT_SER_FLUSH();

  /* Manual Y layout: avoid a 120-row flex column (heavy layout + nested scrollables). */
  constexpr int k_row_pitch = 48;

  const int k_batch = 1;
  int added = 0;
  while (s_mkt_b_mi < s_markets.size() && s_mkt_b_shown < 120) {
    const String &m = s_markets[s_mkt_b_mi];
    if (s_mkt_b_needle.length()) {
      String u = m;
      u.toUpperCase();
      if (u.indexOf(s_mkt_b_needle) < 0) {
        s_mkt_b_mi++;
        continue;
      }
    }
    if (added >= k_batch) {
      break;
    }

    size_t mi = s_mkt_b_mi;
    s_mkt_b_mi++;

    const int32_t y = (int32_t)lv_obj_get_child_cnt(s_tick_list) * k_row_pitch;
    lv_obj_t *row = lv_obj_create(s_tick_list);
    if (!row) {
      mkt_ui_snap("batch_cb: FATAL lv_obj_create(row) returned NULL");
      market_cancel_row_build_timer();
      return;
    }
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, 440, 44);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1a2332), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);

    lv_obj_t *nm = lv_label_create(row);
    if (!nm) {
      mkt_ui_snap("batch_cb: FATAL lv_label_create(name) NULL");
      market_cancel_row_build_timer();
      return;
    }
    lv_label_set_text(nm, m.c_str());
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(nm, 200);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xc8d6e5), LV_PART_MAIN);

    bool is_fav = market_fav_blob_has(s_mkt_b_fav_blob, m.c_str());
    lv_obj_t *bf = lv_button_create(row);
    if (!bf) {
      mkt_ui_snap("batch_cb: FATAL lv_button_create(fav) NULL");
      market_cancel_row_build_timer();
      return;
    }
    lv_obj_remove_flag(bf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bf, 72, 32);
    lv_obj_set_style_bg_color(bf, lv_color_hex(0x2d3d52), LV_PART_MAIN);
    lv_obj_set_style_text_color(bf, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_t *lf = lv_label_create(bf);
    if (!lf) {
      mkt_ui_snap("batch_cb: FATAL lv_label_create(fav) NULL");
      market_cancel_row_build_timer();
      return;
    }
    lv_label_set_text(lf, is_fav ? "Unfav" : "Fav");
    lv_obj_set_style_text_color(lf, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(lf);

    lv_obj_add_event_cb(
        bf,
        [](lv_event_t *e) {
          size_t mix = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
          if (mix >= s_markets.size()) {
            return;
          }
          const char *mm = s_markets[mix].c_str();
          if (app_settings_fav_contains(mm)) {
            app_settings_fav_remove(mm);
          } else {
            app_settings_fav_add(mm);
          }
          market_rebuild_rows();
        },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(mi)));

    lv_obj_t *bu = lv_button_create(row);
    if (!bu) {
      mkt_ui_snap("batch_cb: FATAL lv_button_create(use) NULL");
      market_cancel_row_build_timer();
      return;
    }
    lv_obj_remove_flag(bu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bu, 72, 32);
    lv_obj_set_style_bg_color(bu, lv_color_hex(0x2d3d52), LV_PART_MAIN);
    lv_obj_set_style_text_color(bu, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_t *lu = lv_label_create(bu);
    if (!lu) {
      mkt_ui_snap("batch_cb: FATAL lv_label_create(use) NULL");
      market_cancel_row_build_timer();
      return;
    }
    lv_label_set_text(lu, "Use");
    lv_obj_set_style_text_color(lu, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(lu);

    lv_obj_add_event_cb(
        bu,
        [](lv_event_t *e) {
          size_t mix = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
          if (mix >= s_markets.size()) {
            return;
          }
          const char *mm = s_markets[mix].c_str();
          app_settings_set_market(mm);
          bitvavo_monitor_ui_set_market_heading(mm);
          bitvavo_monitor_wake_poll();
          go_main_chart();
        },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(mi)));

    s_mkt_b_shown++;
    added++;
  }

  MKT_SER_PRINTF("[mkt-ui] batch_cb after loop added=%d nkids=%u\n", added,
                 (unsigned)lv_obj_get_child_cnt(s_tick_list));
  MKT_SER_FLUSH();

  const uint32_t nkids = lv_obj_get_child_cnt(s_tick_list);
  if (nkids > 0) {
    /* List height only; avoid lv_obj_update_layout every tick — that relayouts all rows (~O(n²), freezes). */
    lv_obj_set_size(s_tick_list, 448, (lv_coord_t)(nkids * k_row_pitch + 8));
    lv_obj_invalidate(s_tick_list);
    lv_obj_invalidate(s_tick_scroll);
  }

  const bool done = (s_mkt_b_mi >= s_markets.size() || s_mkt_b_shown >= 120);
  if (done) {
    mkt_ui_snap("batch_cb done=1");
    if (s_mkt_b_shown == 0) {
      mkt_ui_snap("batch_cb empty state label");
      lv_obj_t *empty = lv_label_create(s_tick_list);
      lv_label_set_text(empty, s_mkt_b_needle.length() ? "No matches" : "No data");
      lv_obj_set_style_text_color(empty, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
      lv_obj_set_pos(empty, 8, 8);
      lv_obj_set_size(s_tick_list, 448, 40);
      mkt_ui_snap("batch_cb before final update_layout (empty)");
      lv_obj_update_layout(s_tick_scroll);
      mkt_ui_snap("batch_cb after final update_layout (empty)");
    } else {
      mkt_ui_snap("batch_cb before final update_layout (full list)");
      lv_obj_update_layout(s_tick_scroll);
      mkt_ui_snap("batch_cb after final update_layout (full list)");
    }
    mkt_ui_snap("batch_cb before row timer cancel");
    market_cancel_row_build_timer();
    mkt_ui_snap("LIST BUILD COMPLETE (timer deleted)");
  }

#if defined(ESP32)
  yield();
#endif
  mkt_ui_snap("batch_cb return");
}

static void market_rebuild_rows(void)
{
  s_ticker_rebuild_due_ms = 0;
  if (!s_tick_scroll || !s_tick_list || !s_ta_filter) {
    mkt_ui_snap("rebuild_rows: skip (null ui)");
    return;
  }
  mkt_ui_snap("rebuild_rows: start");
  filter_debounce_cancel();
  market_cancel_row_build_timer();

  mkt_ui_snap("rebuild_rows: deleting old children");
  while (lv_obj_get_child_cnt(s_tick_list) > 0) {
    lv_obj_del(lv_obj_get_child(s_tick_list, 0));
  }
  mkt_ui_snap("rebuild_rows: children cleared");

  const char *filt = lv_textarea_get_text(s_ta_filter);
  if (!filt) {
    filt = "";
  }
  s_mkt_b_needle = String(filt);
  s_mkt_b_needle.trim();
  s_mkt_b_needle.toUpperCase();

  app_settings_fav_get_blob(&s_mkt_b_fav_blob);
  s_mkt_b_mi = 0;
  s_mkt_b_shown = 0;

  s_mkt_row_timer = lv_timer_create(market_rows_batch_cb, 35, nullptr);
  mkt_ui_snap("rebuild_rows: lv_timer_create(batch) done");
}

static void filter_evt(lv_event_t *e)
{
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
    return;
  }
  MKT_SER_PRINTLN("[mkt-ui] filter_evt VALUE_CHANGED -> debounce 280ms");
  MKT_SER_FLUSH();
  if (s_filter_debounce) {
    lv_timer_delete(s_filter_debounce);
    s_filter_debounce = nullptr;
  }
  s_filter_debounce = lv_timer_create(filter_debounce_cb, 280, nullptr);
}

/** Run after opening Ticker screen so LVGL finishes the tap; avoids heap spike + heavy work on event stack. */
static void ticker_load_timer_cb(lv_timer_t *t)
{
  mkt_ui_snap("ticker_load_timer_cb: enter");
  lv_timer_delete(t);
  mkt_ui_snap("ticker_load_timer_cb: before fetch_markets_blocking");
  if (fetch_markets_blocking()) {
    mkt_ui_snap("ticker_load_timer_cb: fetch OK");
    s_markets_loaded = true;
    if (s_lbl_tick_stat) {
      lv_label_set_text(s_lbl_tick_stat, "Fav = favorite, Use = chart");
    }
  } else {
    mkt_ui_snap("ticker_load_timer_cb: fetch FAIL");
    if (s_lbl_tick_stat) {
      lv_label_set_text(s_lbl_tick_stat, "Load failed (WiFi?)");
    }
  }
  mkt_ui_snap("ticker_load_timer_cb: before invalidate");
  if (s_ticker) {
    lv_obj_invalidate(s_ticker);
  }
  mkt_ui_snap("ticker_load_timer_cb: before lv_refr_now");
#if LVGL_VERSION_MAJOR >= 9
  lv_display_t *disp = lv_display_get_default();
  if (disp) {
    lv_refr_now(disp);
  }
#elif LVGL_VERSION_MAJOR >= 8
  lv_disp_t *disp = lv_disp_get_default();
  if (disp) {
    lv_refr_now(disp);
  }
#endif
  /* Do not lv_timer_create from this callback — nested timer work while lv_task_handler runs the timer
   * list can hang. Defer rebuild to settings_ui_tick() (main loop, outside LVGL). */
  s_ticker_rebuild_due_ms = millis() + 5;
  mkt_ui_snap("ticker_load_timer_cb: defer rebuild to main loop (+5ms)");
  mkt_ui_snap("ticker_load_timer_cb: return");
}

static void build_screens(void)
{
  /* --- Main menu --- */
  s_menu = lv_obj_create(nullptr);
  style_screen(s_menu);

  lv_obj_t *tmenu = lv_label_create(s_menu);
  lv_label_set_text(tmenu, "Settings");
  lv_obj_set_style_text_color(tmenu, lv_color_hex(0xf0b90b), LV_PART_MAIN);
  lv_obj_set_style_text_font(tmenu, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(tmenu, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *sub = lv_label_create(s_menu);
  lv_label_set_text(sub, "Button: exit to chart");
  lv_obj_set_style_text_color(sub, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 38);

  lv_obj_t *col = lv_obj_create(s_menu);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, 480, 380);
  lv_obj_align(col, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_layout(col, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 10, LV_PART_MAIN);

  const char *names[] = {"WiFi Setup", "Configuration", "Ticker Setup", "Information", "Clear all settings"};
  for (unsigned i = 0; i < 5; i++) {
    lv_obj_t *btn = lv_button_create(col);
    lv_obj_set_size(btn, 400, 52);
    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, names[i]);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lb);
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t *e) {
          intptr_t ix = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
          kb_hide();
          if (ix == 0) {
            lv_screen_load(s_wifi);
            wifi_schedule_scan();
          } else if (ix == 1) {
            restore_tz_from_nvs();
            s_dd_tz_programmatic = true;
            lv_dropdown_set_selected(s_dd_tz, app_settings_get_timezone_index());
            s_dd_tz_programmatic = false;
            char n0[64], n1[64];
            app_settings_get_ntp(n0, sizeof n0, n1, sizeof n1);
            lv_textarea_set_text(s_ta_ntp0, n0);
            lv_screen_load(s_config);
          } else if (ix == 2) {
            lv_screen_load(s_ticker);
            lv_obj_remove_state(s_ta_filter, LV_STATE_FOCUSED);
            if (!s_markets_loaded) {
              lv_label_set_text(s_lbl_tick_stat, "Loading...");
              lv_timer_create(ticker_load_timer_cb, 20, nullptr);
            } else {
              lv_label_set_text(s_lbl_tick_stat, "Fav = favorite, Use = chart");
              market_rebuild_rows();
            }
          } else if (ix == 3) {
            lv_screen_load(s_info);
          } else {
            lv_screen_load(s_reset_confirm);
          }
        },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
  }

  /* --- WiFi list --- */
  s_wifi = lv_obj_create(nullptr);
  style_screen(s_wifi);
  add_back_to_menu(s_wifi, 8);

  s_lbl_wifi_detail = lv_label_create(s_wifi);
  lv_label_set_text(s_lbl_wifi_detail, "Connection: —");
  lv_obj_set_style_text_font(s_lbl_wifi_detail, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_wifi_detail, lv_color_hex(0xc8d6e5), LV_PART_MAIN);
  lv_obj_set_style_text_align(s_lbl_wifi_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(s_lbl_wifi_detail, 440);
  lv_label_set_long_mode(s_lbl_wifi_detail, LV_LABEL_LONG_MODE_WRAP);
  /* Below < Menu (8+36) and Scan (8+36) — both end ~y=44 */
  lv_obj_align(s_lbl_wifi_detail, LV_ALIGN_TOP_MID, 0, 50);

  lv_obj_t *scan = lv_button_create(s_wifi);
  lv_obj_set_size(scan, 100, 36);
  lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -8, 8);
  lv_obj_t *sl = lv_label_create(scan);
  lv_label_set_text(sl, "Scan");
  lv_obj_center(sl);
  lv_obj_add_event_cb(
      scan,
      [](lv_event_t *e) {
        LV_UNUSED(e);
        wifi_schedule_scan();
      },
      LV_EVENT_CLICKED, nullptr);

  s_wifi_scroll = lv_obj_create(s_wifi);
  lv_obj_set_size(s_wifi_scroll, 460, 302);
  lv_obj_align(s_wifi_scroll, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_opa(s_wifi_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_layout(s_wifi_scroll, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_wifi_scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_wifi_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(s_wifi_scroll, 4, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_wifi_scroll, LV_DIR_VER);
  lv_obj_set_scroll_snap_y(s_wifi_scroll, LV_SCROLL_SNAP_NONE);

  s_lbl_wifi_stat = lv_label_create(s_wifi);
  lv_label_set_text(s_lbl_wifi_stat, "Scanning networks…");
  lv_obj_set_style_text_font(s_lbl_wifi_stat, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(s_lbl_wifi_stat, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_wifi_stat, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_width(s_lbl_wifi_stat, 440);
  lv_label_set_long_mode(s_lbl_wifi_stat, LV_LABEL_LONG_MODE_WRAP);
  /* Directly above the network list (not under connection details — avoids overlap). */
  lv_obj_align_to(s_lbl_wifi_stat, s_wifi_scroll, LV_ALIGN_OUT_TOP_MID, 0, -10);

  /* --- WiFi password --- */
  s_wifi_pw = lv_obj_create(nullptr);
  style_screen(s_wifi_pw);
  add_back_to_menu(s_wifi_pw, 8);

  s_lbl_pw_stat = lv_label_create(s_wifi_pw);
  lv_label_set_text(s_lbl_pw_stat, "SSID");
  lv_obj_set_style_text_font(s_lbl_pw_stat, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(s_lbl_pw_stat, LV_ALIGN_TOP_MID, 0, 50);

  lv_obj_t *lbl_pw_field = lv_label_create(s_wifi_pw);
  lv_label_set_text(lbl_pw_field, "Password");
  lv_obj_set_style_text_font(lbl_pw_field, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl_pw_field, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
  lv_obj_align_to(lbl_pw_field, s_lbl_pw_stat, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  s_ta_pass = lv_textarea_create(s_wifi_pw);
  lv_obj_set_size(s_ta_pass, 420, 44);
  lv_obj_align_to(s_ta_pass, lbl_pw_field, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
  lv_textarea_set_one_line(s_ta_pass, true);
  /* Show passphrase in plain text (not obscured). */
  lv_textarea_set_password_mode(s_ta_pass, false);
  lv_obj_set_style_text_font(s_ta_pass, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_add_event_cb(s_ta_pass, ta_kb_event, LV_EVENT_ALL, nullptr);

  lv_obj_t *bconn = lv_button_create(s_wifi_pw);
  lv_obj_set_size(bconn, 160, 44);
  lv_obj_align(bconn, LV_ALIGN_BOTTOM_MID, 0, -80);
  lv_obj_t *lc = lv_label_create(bconn);
  lv_label_set_text(lc, "Connect");
  lv_obj_center(lc);
  lv_obj_add_event_cb(
      bconn,
      [](lv_event_t *e) {
        LV_UNUSED(e);
        kb_hide();
        if (s_wifi_connect_timer) {
          lv_timer_delete(s_wifi_connect_timer);
          s_wifi_connect_timer = nullptr;
        }
        const char *pw = lv_textarea_get_text(s_ta_pass);
        strncpy(s_wifi_connect_pass, pw ? pw : "", sizeof(s_wifi_connect_pass) - 1);
        s_wifi_connect_pass[sizeof(s_wifi_connect_pass) - 1] = '\0';
        lv_label_set_text(s_lbl_pw_stat, "Connecting...");
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false);
        WiFi.begin(s_sel_ssid, s_wifi_connect_pass[0] ? s_wifi_connect_pass : nullptr);
        s_wifi_connect_started_ms = millis();
        s_wifi_connect_timer = lv_timer_create(wifi_connect_timer_cb, 300, nullptr);
      },
      LV_EVENT_CLICKED, nullptr);

  /* --- Config --- */
  s_config = lv_obj_create(nullptr);
  style_screen(s_config);
  add_back_to_menu(s_config, 4);

  lv_obj_t *lz = lv_label_create(s_config);
  lv_label_set_text(lz, "Time zone");
  lv_obj_set_style_text_font(lz, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(lz, LV_ALIGN_TOP_LEFT, 12, 46);

  s_dd_tz = lv_dropdown_create(s_config);
  lv_obj_set_width(s_dd_tz, 440);
  lv_obj_align(s_dd_tz, LV_ALIGN_TOP_MID, 0, 68);
  lv_dropdown_set_options(s_dd_tz, k_zone_dropdown_opts);
  lv_obj_set_style_text_font(s_dd_tz, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_add_event_cb(s_dd_tz, dd_tz_preview_evt, LV_EVENT_VALUE_CHANGED, nullptr);
  s_dd_tz_programmatic = true;
  lv_dropdown_set_selected(s_dd_tz, 12);
  s_dd_tz_programmatic = false;

  lv_obj_t *ln0 = lv_label_create(s_config);
  lv_label_set_text(ln0, "NTP server address");
  lv_obj_set_style_text_font(ln0, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(ln0, LV_ALIGN_TOP_LEFT, 12, 118);

  s_ta_ntp0 = lv_textarea_create(s_config);
  lv_obj_set_size(s_ta_ntp0, 450, 40);
  lv_obj_align(s_ta_ntp0, LV_ALIGN_TOP_MID, 0, 142);
  lv_textarea_set_one_line(s_ta_ntp0, true);
  lv_obj_set_style_text_font(s_ta_ntp0, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_add_event_cb(s_ta_ntp0, ta_kb_event, LV_EVENT_ALL, nullptr);

  lv_obj_t *lclk_t = lv_label_create(s_config);
  lv_label_set_text(lclk_t, "Local date & time");
  lv_obj_set_style_text_font(lclk_t, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(lclk_t, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
  lv_obj_align(lclk_t, LV_ALIGN_TOP_LEFT, 12, 190);

  s_lbl_cfg_clock = lv_label_create(s_config);
  lv_label_set_text(s_lbl_cfg_clock, "—");
  lv_obj_set_style_text_font(s_lbl_cfg_clock, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_cfg_clock, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_align(s_lbl_cfg_clock, LV_ALIGN_TOP_MID, 0, 212);
  lv_label_set_long_mode(s_lbl_cfg_clock, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_set_width(s_lbl_cfg_clock, 440);

  s_lbl_cfg_ntp = lv_label_create(s_config);
  lv_label_set_text(s_lbl_cfg_ntp, "NTP: …");
  lv_obj_set_style_text_font(s_lbl_cfg_ntp, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_cfg_ntp, lv_color_hex(0xc8d6e5), LV_PART_MAIN);
  lv_obj_align(s_lbl_cfg_ntp, LV_ALIGN_TOP_MID, 0, 248);
  lv_label_set_long_mode(s_lbl_cfg_ntp, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(s_lbl_cfg_ntp, 440);

  lv_obj_t *bsave = lv_button_create(s_config);
  lv_obj_set_size(bsave, 160, 40);
  lv_obj_align(bsave, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_t *ls = lv_label_create(bsave);
  lv_label_set_text(ls, "Save");
  lv_obj_center(ls);
  lv_obj_add_event_cb(
      bsave,
      [](lv_event_t *e) {
        LV_UNUSED(e);
        kb_hide();
        uint16_t sel = lv_dropdown_get_selected(s_dd_tz);
        if (sel <= 23) {
          app_settings_set_timezone_index(static_cast<uint8_t>(sel));
        }
        const char *n0 = lv_textarea_get_text(s_ta_ntp0);
        app_settings_set_ntp(n0 && n0[0] ? n0 : "europe.pool.ntp.org", nullptr);
        app_settings_apply_time_and_ntp();
      },
      LV_EVENT_CLICKED, nullptr);

  /* --- Ticker / markets --- */
  s_ticker = lv_obj_create(nullptr);
  style_screen(s_ticker);
  add_back_to_menu(s_ticker, 4);

  lv_obj_t *lfilt = lv_label_create(s_ticker);
  lv_label_set_text(lfilt, "Search filter");
  lv_obj_set_style_text_font(lfilt, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lfilt, lv_color_hex(0xc8d6e5), LV_PART_MAIN);
  lv_obj_align(lfilt, LV_ALIGN_TOP_LEFT, 12, 46);

  s_ta_filter = lv_textarea_create(s_ticker);
  lv_obj_set_size(s_ta_filter, 450, 36);
  lv_obj_align(s_ta_filter, LV_ALIGN_TOP_MID, 0, 68);
  lv_textarea_set_one_line(s_ta_filter, true);
  lv_textarea_set_placeholder_text(s_ta_filter, "e.g. BTC");
  lv_obj_set_style_text_font(s_ta_filter, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_ta_filter, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_ta_filter, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_ta_filter, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_text_color(s_ta_filter, lv_color_hex(0x6b7c8f), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_add_event_cb(s_ta_filter, ta_kb_event, LV_EVENT_ALL, nullptr);
  lv_obj_add_event_cb(s_ta_filter, filter_evt, LV_EVENT_VALUE_CHANGED, nullptr);

  s_lbl_tick_stat = lv_label_create(s_ticker);
  lv_label_set_text(s_lbl_tick_stat, "—");
  lv_obj_set_style_text_font(s_lbl_tick_stat, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_tick_stat, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
  lv_obj_align(s_lbl_tick_stat, LV_ALIGN_TOP_MID, 0, 108);
  lv_label_set_long_mode(s_lbl_tick_stat, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
  lv_obj_set_width(s_lbl_tick_stat, 440);

  s_tick_scroll = lv_obj_create(s_ticker);
  lv_obj_set_size(s_tick_scroll, 460, 330);
  lv_obj_align(s_tick_scroll, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_opa(s_tick_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_tick_scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_tick_scroll, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(s_tick_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  s_tick_list = lv_obj_create(s_tick_scroll);
  lv_obj_remove_style_all(s_tick_list);
  lv_obj_remove_flag(s_tick_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_tick_list, 448, 48);
  lv_obj_set_style_bg_opa(s_tick_list, LV_OPA_TRANSP, LV_PART_MAIN);

  /* --- Info --- */
  s_info = lv_obj_create(nullptr);
  style_screen(s_info);
  add_back_to_menu(s_info, 8);

  lv_obj_t *inf = lv_label_create(s_info);
  char inf_buf[768];
  snprintf(inf_buf, sizeof inf_buf,
           "Bitvavo Market Monitor\n\n"
           "Version: %s\n"
           "Date: %s\n\n"
           "This app is independently developed and is not affiliated with, endorsed by, sponsored by, or "
           "approved by Bitvavo B.V. \"Bitvavo\" is a trademark of Bitvavo B.V. and is used only to "
           "describe compatibility with the Bitvavo platform.",
           s_app_version && s_app_version[0] ? s_app_version : "—",
           s_app_version_date && s_app_version_date[0] ? s_app_version_date : "—");
  lv_label_set_text(inf, inf_buf);
  lv_obj_set_style_text_color(inf, lv_color_hex(0xe0e0e0), LV_PART_MAIN);
  lv_obj_set_style_text_font(inf, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(inf, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(inf, 420);
  lv_label_set_long_mode(inf, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(inf, LV_ALIGN_CENTER, 0, 0);

  /* --- Clear all settings (confirmation) --- */
  s_reset_confirm = lv_obj_create(nullptr);
  style_screen(s_reset_confirm);
  add_back_to_menu(s_reset_confirm, 8);

  lv_obj_t *rt = lv_label_create(s_reset_confirm);
  lv_label_set_text(rt, "Clear all settings?");
  lv_obj_set_style_text_color(rt, lv_color_hex(0xf0b90b), LV_PART_MAIN);
  lv_obj_set_style_text_font(rt, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(rt, LV_ALIGN_TOP_MID, 0, 44);

  lv_obj_t *rbody = lv_label_create(s_reset_confirm);
  lv_label_set_text(
      rbody,
      "This will erase saved WiFi credentials, time zone, NTP servers, the active market, and all "
      "ticker favorites.\n\nContinue?");
  lv_obj_set_style_text_color(rbody, lv_color_hex(0xc8d6e5), LV_PART_MAIN);
  lv_obj_set_style_text_font(rbody, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(rbody, 440);
  lv_label_set_long_mode(rbody, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(rbody, LV_ALIGN_TOP_MID, 0, 84);

  lv_obj_t *rrow = lv_obj_create(s_reset_confirm);
  lv_obj_remove_style_all(rrow);
  lv_obj_set_size(rrow, 440, 52);
  lv_obj_align(rrow, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_layout(rrow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(rrow, 16, LV_PART_MAIN);

  lv_obj_t *bcancel = lv_button_create(rrow);
  lv_obj_set_size(bcancel, 168, 44);
  lv_obj_t *lcancel = lv_label_create(bcancel);
  lv_label_set_text(lcancel, "Cancel");
  lv_obj_set_style_text_font(lcancel, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(lcancel);
  lv_obj_add_event_cb(
      bcancel,
      [](lv_event_t *e) {
        LV_UNUSED(e);
        go_menu_root();
      },
      LV_EVENT_CLICKED, nullptr);

  lv_obj_t *bclear = lv_button_create(rrow);
  lv_obj_set_size(bclear, 168, 44);
  lv_obj_set_style_bg_color(bclear, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
  lv_obj_t *lclear = lv_label_create(bclear);
  lv_label_set_text(lclear, "Clear all");
  lv_obj_set_style_text_font(lclear, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lclear, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_center(lclear);
  lv_obj_add_event_cb(
      bclear,
      [](lv_event_t *e) {
        LV_UNUSED(e);
        kb_hide();
        market_cancel_row_build_timer();
        filter_debounce_cancel();
        app_settings_clear_all();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        app_settings_apply_time_and_ntp();
        restore_tz_from_nvs();
        char mk[24];
        app_settings_get_market(mk, sizeof mk);
        bitvavo_monitor_ui_set_market_heading(mk);
        bitvavo_monitor_wake_poll();
        s_markets_loaded = false;
        lv_screen_load(s_menu);
      },
      LV_EVENT_CLICKED, nullptr);
}

void settings_ui_init(const char *app_version, const char *app_version_date)
{
  s_app_version = (app_version && app_version[0]) ? app_version : "—";
  s_app_version_date = (app_version_date && app_version_date[0]) ? app_version_date : "—";

  pinMode(INDICATOR_BTN_GPIO, INPUT_PULLUP);

  s_kb = lv_keyboard_create(lv_layer_top());
  lv_obj_set_size(s_kb, 480, 170);
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      s_kb,
      [](lv_event_t *e) {
        lv_event_code_t c = lv_event_get_code(e);
        if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
          kb_hide();
        }
      },
      LV_EVENT_ALL, nullptr);

  build_screens();
  s_settings_active = false;
}

void settings_ui_poll(void)
{
  static int last_rd = HIGH;
  static uint32_t db_t = 0;
  static int stable = HIGH;

  int rd = digitalRead(INDICATOR_BTN_GPIO);
  uint32_t now = millis();

  if (rd != last_rd) {
    db_t = now;
    last_rd = rd;
  }
  if ((now - db_t) < 45) {
    return;
  }
  if (rd == stable) {
    return;
  }
  stable = rd;

  /* Active low: pressed = LOW */
  if (stable != LOW) {
    return;
  }

  if (s_settings_active) {
    go_main_chart();
  } else {
    kb_hide();
    s_settings_active = true;
    lv_screen_load(s_menu);
  }
}

void settings_ui_tick(void)
{
  uint32_t now_ms = millis();

  if (s_ticker_rebuild_due_ms != 0u && (int32_t)(now_ms - s_ticker_rebuild_due_ms) >= 0) {
    s_ticker_rebuild_due_ms = 0;
    mkt_ui_snap("deferred ticker rebuild: start");
    market_rebuild_rows();
    mkt_ui_snap("deferred ticker rebuild: done");
  }

  if (s_wifi && s_lbl_wifi_detail && lv_screen_active() == s_wifi) {
    static uint32_t last_wifi_detail_ms;
    if (last_wifi_detail_ms == 0 || (now_ms - last_wifi_detail_ms) >= 2000) {
      last_wifi_detail_ms = now_ms;
      wifi_update_connection_detail();
    }
  }

  if (!s_config || !s_lbl_cfg_clock || !s_lbl_cfg_ntp) {
    return;
  }
  if (lv_screen_active() != s_config) {
    return;
  }

  static uint32_t last_ms;
  if ((uint32_t)(now_ms - last_ms) < 500) {
    return;
  }
  last_ms = now_ms;

  struct tm tm_loc;
  if (app_time_local_tm(&tm_loc)) {
    char b[48];
    snprintf(b, sizeof b, "%02d-%02d-%04d   %02d:%02d:%02d", tm_loc.tm_mday, tm_loc.tm_mon + 1,
             tm_loc.tm_year + 1900, tm_loc.tm_hour, tm_loc.tm_min, tm_loc.tm_sec);
    lv_label_set_text(s_lbl_cfg_clock, b);
    lv_label_set_text(s_lbl_cfg_ntp, "NTP: OK (time synced)");
  } else {
    lv_label_set_text(s_lbl_cfg_clock, "Time not set (NTP?)");
    char line[96];
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(line, sizeof line, "NTP: offline (no Wi-Fi)");
    } else {
      snprintf(line, sizeof line, "NTP: waiting for time sync…");
    }
    lv_label_set_text(s_lbl_cfg_ntp, line);
  }
}

bool settings_ui_is_active(void)
{
  return s_settings_active;
}

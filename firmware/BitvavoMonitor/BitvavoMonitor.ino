/**
 * SenseCAP Indicator D1S — BitvavoMonitor: Bitvavo market price + zoomable candle chart.
 *
 * Setup (see https://wiki.seeedstudio.com/SenseCAP_Indicator_ESP32_Arduino/):
 * - Board: ESP32S3 Dev Module, OPI PSRAM enabled, partition scheme with app space for HTTPS + LVGL.
 * - Libraries: lvgl 9.2.x, GFX Library for Arduino, TouchLib (ZIP), PacketSerial not required here,
 *   Anitracks PCA95x5, ArduinoJson (v7).
 *   If Arduino reports duplicate PCA95x5 libraries, remove/rename `libraries/PCA95x5` so only one copy is used.
 * - Copy `lv_conf.h` from this sketch folder into `Arduino/libraries/lvgl/` as `lv_conf.h`
 *   (or next to the lvgl library per LVGL Arduino docs). This project expects `LV_USE_CANVAS 1` and `LV_USE_CHART 0`.
 * - Footer clock uses JetBrains Mono (SIL OFL 1.1); `lv_font_jetbrains_mono_24.c` is generated from `fonts/JetBrainsMono-Regular.ttf` via `lv_font_conv`.
 *   After regenerating, keep `#include <lvgl.h>` at the top of that file (Arduino has no `lvgl/lvgl.h`).
 * - Optional: define `BITVAVO_MONITOR_DISPLAY_TZ_POSIX` before compile for NTP wall-clock (default EU CET/CEST).
 * - Optional: `-DBITVAVO_MONITOR_SERIAL_DEBUG=1` for verbose USB Serial (markets/LVGL/LAN IP).
 * - If you see "Sketch too big" / text section exceeds space: Arduino IDE → Tools → Partition Scheme
 *   → choose a layout with a larger app partition (e.g. **Huge APP**, **No OTA**, or **3MB APP** on 16MB flash).
 *   See https://support.arduino.cc/hc/en-us/articles/360013825179
 *
 * - **First run**: configure Wi‑Fi under Settings → WiFi Setup (no credentials are compiled into the sketch).
 * - Chart zoom: in the **bottom 70%**, **press and release** — **left half** zooms in, **right half** zooms out
 *   (uses **release** + a short minimum hold so I²C noise can’t change the preset). Zoom ignored while the footer shows **Updating..** (until the candle refresh finishes).
 *
 * API reference (base URL `https://api.bitvavo.com/v2`): endpoints this firmware calls and why.
 * - GET `/ticker/price?market=<market>` — latest trade **price** as a display string (headline when present).
 *   https://docs.bitvavo.com/docs/rest-api/get-ticker-prices/
 * - GET `/ticker/24h?market=<market>` — rolling **24h** stats (`open`, `high`, `low`, `last`, etc.) for the 24h O/H/L line and % change vs open.
 *   https://docs.bitvavo.com/docs/rest-api/get-candlestick-data-24-h/
 * - GET `/<market>/candles?interval=<interval>&limit=<n>` — historical **OHLC candles** for the on-screen chart (interval/limit from zoom presets in this sketch).
 *   https://docs.bitvavo.com/docs/rest-api/get-candlestick-data/
 * - GET `/markets` — full market list (Settings → Ticker Setup / favorites; requested from `settings_ui.cpp`, not from this `.ino`).
 *   https://docs.bitvavo.com/docs/rest-api/get-markets/
 */

#include <Arduino.h>
/* ESP32 Arduino: include FreeRTOS before task.h (path/layout varies by core; quotes match core examples). */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include <lvgl.h>
#include <Arduino_GFX_Library.h>

#include "Indicator_Extender.h"
#include "Indicator_SWSPI.h"
#include "src/Display/Indicator_RGB_Display.h"
#include "src/Display/lvgldriver.h"
#include "touch.h"
#include "bitvavo_monitor_ui.h"
#include "app_settings.h"
#include "settings_ui.h"

/** Shown in Settings → Information (bump date when you cut a release). */
static const char kAppVersion[] = "1.0";
static const char kAppVersionDate[] = "7 May 2026";

/** Bitvavo poll intervals (milliseconds). */
static constexpr uint32_t kRefreshTickerPriceMs = 60UL * 1000UL;   /**< GET /ticker/price — headline trade price. */
static constexpr uint32_t kRefreshTicker24hMs = 60UL * 1000UL;    /**< GET /ticker/24h — rolling 24h O/H/L + % change. */
static constexpr uint32_t kRefreshCandlesMs = 60UL * 60UL * 1000UL; /**< GET /…/candles — chart. */

#define HOR_RES 480
#define VER_RES 480

#define GFX_BL 45
 
/** SenseCAP Indicator D1 built-in user button (INPUT_PULLUP, active low). */
#ifndef INDICATOR_BTN_GPIO
#define INDICATOR_BTN_GPIO 38
#endif

static constexpr char BITVAVO_API[] = "https://api.bitvavo.com/v2";

/** Chart presets for Bitvavo candles (GET /{market}/candles; interval strings per Bitvavo docs). */
enum { BITVAVO_MONITOR_NUM_CHART_PRESETS = 5 };
static const char *const kChartInterval[BITVAVO_MONITOR_NUM_CHART_PRESETS] = {"1h", "8h", "1d", "1W", "1M"};
static const uint16_t kChartLimit[BITVAVO_MONITOR_NUM_CHART_PRESETS] = {24, 28, 28, 26, 24};
static const char *const kChartCaption[BITVAVO_MONITOR_NUM_CHART_PRESETS] = {
    "24x 1h (~1 day)",
    "28x 8h (~1 week)",
    "28x 1d (~4 weeks)",
    "26x 1W (~6 months)",
    "24x 1M (~2 years)",
};

/** Bottom 70% chart zoom: left half = zoom in, right half = zoom out on **release** (see poll_chart_zoom_touch). */
static portMUX_TYPE g_chart_preset_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t g_chart_preset_idx = 1;
static volatile bool g_chart_candles_force = false;
static volatile bool g_zoom_refresh_busy = false;
static volatile bool g_zoom_footer_clear_req = false;
/** Poll task sleeps until the next Bitvavo refresh deadline; wake sooner via notify (zoom / market). */
static TaskHandle_t g_bitvavo_task_handle;
/** NVS market we last aligned `merged` with (when this differs, ticker/24h/candles refetch immediately). */
static char s_bitvavo_poll_market[24];

static void bitvavo_poll_wake(void)
{
  if (g_bitvavo_task_handle != nullptr) {
    xTaskNotifyGive(g_bitvavo_task_handle);
  }
}

void bitvavo_monitor_wake_poll(void)
{
  /* Wake the bitvavo task AND mark candles as needing a refresh; otherwise the next
   * cycle only fetches the ticker (top values) and the chart keeps showing stale candles
   * until the hourly interval expires. Used after market changes (favorites/settings). */
  portENTER_CRITICAL(&g_chart_preset_mux);
  g_chart_candles_force = true;
  portEXIT_CRITICAL(&g_chart_preset_mux);
  bitvavo_poll_wake();
}

static void chart_zoom_caption_copy(uint8_t idx, bitvavo_monitor_snapshot_t *snap)
{
  if (idx >= BITVAVO_MONITOR_NUM_CHART_PRESETS) {
    idx = 0;
  }
  snprintf(snap->chart_zoom_caption, sizeof(snap->chart_zoom_caption), "Chart: %s", kChartCaption[idx]);
}

static void chart_preset_peek(uint8_t *idx_out, bool *force_candles_out)
{
  portENTER_CRITICAL(&g_chart_preset_mux);
  if (idx_out) {
    *idx_out = g_chart_preset_idx;
  }
  if (force_candles_out) {
    *force_candles_out = g_chart_candles_force;
  }
  portEXIT_CRITICAL(&g_chart_preset_mux);
}

static void chart_preset_clear_candle_force(void)
{
  portENTER_CRITICAL(&g_chart_preset_mux);
  g_chart_candles_force = false;
  portEXIT_CRITICAL(&g_chart_preset_mux);
}

static void poll_chart_zoom_touch(void)
{
  static bool was_down;
  static uint32_t debounce_ms;
  /* Top-band taps fire on RELEASE (not press) so the still-active touch can't bleed
   * a click through onto a button on the freshly-loaded overlay. */
  static bool top_band_armed;
  /** Chart zoom (bottom 70%) uses release + min hold so brief I²C/touch noise can't change preset. */
  static bool chart_zoom_armed;
  static uint32_t chart_zoom_arm_ms;
  static int chart_zoom_arm_x;

  const bool down = touch_touched();
  const uint32_t now = millis();

  if ((uint32_t)(now - debounce_ms) < 450) {
    was_down = down;
    return;
  }

  constexpr int band_top = VER_RES * 30 / 100;
  /** Ignore press–release shorter than this (filters spurious `touch.read()` glitches). */
  constexpr uint32_t k_chart_zoom_min_ms = 55;

  if (down && !was_down) {
    const int x = touch_last_x;
    const int y = touch_last_y;
    if (y < band_top) {
      /* Press only arms the gesture; the screen swap waits until the user lifts. */
      top_band_armed = true;
      chart_zoom_armed = false;
    } else if (!g_zoom_refresh_busy) {
      top_band_armed = false;
      chart_zoom_armed = true;
      chart_zoom_arm_ms = now;
      chart_zoom_arm_x = x;
    } else {
      top_band_armed = false;
      chart_zoom_armed = false;
    }
  } else if (!down && was_down) {
    if (top_band_armed) {
      top_band_armed = false;
      bitvavo_monitor_ui_show_favorites_overlay();
      debounce_ms = now;
    } else if (chart_zoom_armed) {
      chart_zoom_armed = false;
      const uint32_t dur = (uint32_t)(now - chart_zoom_arm_ms);
      if (dur >= k_chart_zoom_min_ms && touch_last_y >= band_top) {
        bool changed = false;
        portENTER_CRITICAL(&g_chart_preset_mux);
        if (chart_zoom_arm_x < HOR_RES / 2 && g_chart_preset_idx > 0) {
          g_chart_preset_idx--;
          g_chart_candles_force = true;
          changed = true;
        } else if (chart_zoom_arm_x >= HOR_RES / 2 && g_chart_preset_idx + 1 < BITVAVO_MONITOR_NUM_CHART_PRESETS) {
          g_chart_preset_idx++;
          g_chart_candles_force = true;
          changed = true;
        }
        portEXIT_CRITICAL(&g_chart_preset_mux);
        if (changed) {
          g_zoom_refresh_busy = true;
          bitvavo_monitor_ui_set_footer_updating(true);
          debounce_ms = now;
          bitvavo_poll_wake();
        }
      }
    }
  }
  was_down = down;
}

/** POSIX TZ string for local HH:MM after NTP (default: EU CET/CEST). Override before build if needed. */
#ifndef BITVAVO_MONITOR_DISPLAY_TZ_POSIX
#define BITVAVO_MONITOR_DISPLAY_TZ_POSIX "CET-1CEST,M3.5.0/2,M10.5.0/3"
#endif

Arduino_DataBus *bus = new Indicator_SWSPI(GFX_NOT_DEFINED, EXPANDER_IO_LCD_CS, SPI_SCLK, SPI_MOSI,
                                           GFX_NOT_DEFINED);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    18, 17, 16, 21, 4, 3, 2, 1, 0, 10, 9, 8, 7, 6, 5, 15, 14, 13, 12, 11, 1, 10, 8, 50, 1, 10, 8, 20);

Arduino_RGB_Display *gfx =
    new Arduino_RGB_Display(HOR_RES, VER_RES, rgbpanel, 0, false, bus, GFX_NOT_DEFINED,
                          st7701_indicator_init_operations, sizeof(st7701_indicator_init_operations));

static portMUX_TYPE g_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static bitvavo_monitor_snapshot_t g_pending_snap;
static volatile bool g_have_snap = false;
static bool g_ntp_started = false;

static void ensure_ntp_started(void)
{
  if (g_ntp_started) {
    return;
  }
  app_ntp_apply_from_settings();
  g_ntp_started = true;
}

#if LV_USE_LOG != 0 && BITVAVO_MONITOR_SERIAL_DEBUG
static void lv_print_cb(lv_log_level_t level, const char *buf)
{
  LV_UNUSED(level);
  Serial.println(buf);
}
#endif

static uint32_t millis_cb_lvgl(void)
{
  return millis();
}

static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  LV_UNUSED(indev);
  if (touch_has_signal() && touch_touched()) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static bool parsePriceField(JsonVariantConst v, double *out)
{
  if (v.isNull()) {
    return false;
  }
  if (v.is<double>()) {
    *out = v.as<double>();
    return true;
  }
  const char *s = v.as<const char *>();
  if (!s || !s[0]) {
    return false;
  }
  char *end = nullptr;
  *out = strtod(s, &end);
  return end != s;
}

static bool fetch_bitvavo_ticker_24h(bitvavo_monitor_snapshot_t *snap, const char *market)
{
  char url[168];
  snprintf(url, sizeof url, "%s/ticker/24h?market=%s", BITVAVO_API, market);
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient httpTicker;
  if (!httpTicker.begin(client, url)) {
    strncpy(snap->error_line, "HTTP ticker begin failed", sizeof(snap->error_line) - 1);
    return false;
  }
  int tickerCode = httpTicker.GET();
  String tickerBody = httpTicker.getString();
  httpTicker.end();
  if (tickerCode != 200) {
    snprintf(snap->error_line, sizeof(snap->error_line), "Ticker HTTP %d", tickerCode);
    return false;
  }

  JsonDocument docTicker;
  DeserializationError terr = deserializeJson(docTicker, tickerBody);
  if (terr) {
    snprintf(snap->error_line, sizeof(snap->error_line), "Ticker JSON %s", terr.c_str());
    return false;
  }

  if (!parsePriceField(docTicker["last"], &snap->last_eur) ||
      !parsePriceField(docTicker["open"], &snap->open_eur) ||
      !parsePriceField(docTicker["high"], &snap->high_eur) ||
      !parsePriceField(docTicker["low"], &snap->low_eur)) {
    strncpy(snap->error_line, "Ticker missing OHLC fields", sizeof(snap->error_line) - 1);
    return false;
  }

  return true;
}

/** Latest trade display string from GET /ticker/price (see Bitvavo docs). */
static bool fetch_bitvavo_ticker_price(bitvavo_monitor_snapshot_t *snap, const char *market)
{
  snap->last_price_display[0] = '\0';
  char url[168];
  snprintf(url, sizeof url, "%s/ticker/price?market=%s", BITVAVO_API, market);
  WiFiClientSecure clientPx;
  clientPx.setInsecure();
  HTTPClient httpPx;
  if (!httpPx.begin(clientPx, url)) {
    return false;
  }
  int pxCode = httpPx.GET();
  String pxBody = httpPx.getString();
  httpPx.end();
  if (pxCode != 200) {
    return false;
  }
  JsonDocument docPx;
  if (deserializeJson(docPx, pxBody)) {
    return false;
  }
  const char *ps = nullptr;
  if (docPx.is<JsonArray>()) {
    JsonArray rowsPx = docPx.as<JsonArray>();
    if (!rowsPx.isNull() && rowsPx.size() >= 1) {
      ps = rowsPx[0]["price"].as<const char *>();
    }
  } else {
    ps = docPx["price"].as<const char *>();
  }
  if (!ps || !ps[0]) {
    return false;
  }
  strncpy(snap->last_price_display, ps, sizeof(snap->last_price_display) - 1);
  snap->last_price_display[sizeof(snap->last_price_display) - 1] = '\0';
  return true;
}

/** Fills candle arrays only (caller merges with latest ticker snapshot). */
static bool fetch_bitvavo_candles(bitvavo_monitor_snapshot_t *snap, uint8_t preset_idx, const char *market)
{
  if (preset_idx >= BITVAVO_MONITOR_NUM_CHART_PRESETS) {
    preset_idx = 0;
  }

  char url[176];
  snprintf(url, sizeof url, "%s/%s/candles?interval=%s&limit=%u", BITVAVO_API, market,
           kChartInterval[preset_idx], (unsigned)kChartLimit[preset_idx]);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient httpCandles;
  if (!httpCandles.begin(client, url)) {
    strncpy(snap->error_line, "HTTP candles begin failed", sizeof(snap->error_line) - 1);
    return false;
  }
  int candleCode = httpCandles.GET();
  String candleBody = httpCandles.getString();
  httpCandles.end();
  if (candleCode != 200) {
    snprintf(snap->error_line, sizeof(snap->error_line), "Candles HTTP %d", candleCode);
    return false;
  }

  JsonDocument docCandles;
  DeserializationError cerr = deserializeJson(docCandles, candleBody);
  if (cerr) {
    snprintf(snap->error_line, sizeof(snap->error_line), "Candles JSON %s", cerr.c_str());
    return false;
  }

  JsonArray rows = docCandles.as<JsonArray>();
  size_t rawCount = rows.size();
  uint16_t idx = 0;
  for (int i = (int)rawCount - 1; i >= 0 && idx < BITVAVO_MONITOR_UI_MAX_CANDLES; i--) {
    JsonArray row = rows[i].as<JsonArray>();
    if (row.size() < 5) {
      continue;
    }
    double po = 0, ph = 0, pl = 0, pc = 0;
    if (!parsePriceField(row[1], &po) || !parsePriceField(row[2], &ph) || !parsePriceField(row[3], &pl) ||
        !parsePriceField(row[4], &pc)) {
      continue;
    }
    snap->cdl_o[idx] = po;
    snap->cdl_h[idx] = ph;
    snap->cdl_l[idx] = pl;
    snap->cdl_c[idx] = pc;
    idx++;
  }
  snap->chart_count = idx;
  snap->error_line[0] = '\0';
  return idx > 0;
}

static void bitvavoPollTask(void *param)
{
  LV_UNUSED(param);
  vTaskDelay(pdMS_TO_TICKS(2500));

  static bitvavo_monitor_snapshot_t merged;
  static TickType_t next_price_deadline = 0;
  static TickType_t next_24h_deadline = 0;
  static TickType_t next_candle_deadline = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (g_zoom_refresh_busy) {
        g_zoom_refresh_busy = false;
        g_zoom_footer_clear_req = true;
      }
      bitvavo_monitor_snapshot_t snap;
      memset(&snap, 0, sizeof(snap));
      char ssid_chk[33], pass_chk[65];
      if (!app_settings_get_wifi(ssid_chk, sizeof ssid_chk, pass_chk, sizeof pass_chk)) {
        strncpy(snap.error_line, BITVAVO_MONITOR_MSG_SETUP_DEVICE, sizeof(snap.error_line) - 1);
      } else {
        strncpy(snap.error_line, "WiFi disconnected", sizeof(snap.error_line) - 1);
      }
      portENTER_CRITICAL(&g_snap_mux);
      g_pending_snap = snap;
      g_have_snap = true;
      portEXIT_CRITICAL(&g_snap_mux);
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    ensure_ntp_started();

    uint8_t preset_idx = 0;
    bool force_candles = false;
    chart_preset_peek(&preset_idx, &force_candles);

    char market[24];
    app_settings_get_market(market, sizeof market);
    if (strcmp(market, s_bitvavo_poll_market) != 0) {
      strncpy(s_bitvavo_poll_market, market, sizeof(s_bitvavo_poll_market) - 1);
      s_bitvavo_poll_market[sizeof(s_bitvavo_poll_market) - 1] = '\0';
      next_24h_deadline = 0;
      next_price_deadline = 0;
      next_candle_deadline = 0;
      portENTER_CRITICAL(&g_chart_preset_mux);
      g_chart_candles_force = true;
      portEXIT_CRITICAL(&g_chart_preset_mux);
      /* Drop cached snapshot so we never mix ticker/24h/candles from the previous market. */
      memset(&merged, 0, sizeof(merged));
    }

    bitvavo_monitor_snapshot_t snap;
    memcpy(&snap, &merged, sizeof(snap));
    snap.error_line[0] = '\0';
    chart_zoom_caption_copy(preset_idx, &snap);

    const TickType_t now = xTaskGetTickCount();
    const bool need_24h =
        (next_24h_deadline == 0) || ((int32_t)(now - next_24h_deadline) >= 0);
    const bool need_price =
        (next_price_deadline == 0) || ((int32_t)(now - next_price_deadline) >= 0);

    if (need_24h) {
      if (!fetch_bitvavo_ticker_24h(&snap, market)) {
        if (g_zoom_refresh_busy) {
          g_zoom_refresh_busy = false;
          g_zoom_footer_clear_req = true;
        }
        portENTER_CRITICAL(&g_snap_mux);
        g_pending_snap = snap;
        g_have_snap = true;
        portEXIT_CRITICAL(&g_snap_mux);
        next_24h_deadline = now + pdMS_TO_TICKS(kRefreshTicker24hMs);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kRefreshTicker24hMs));
        continue;
      }
      next_24h_deadline = now + pdMS_TO_TICKS(kRefreshTicker24hMs);
    }

    if (need_price) {
      (void)fetch_bitvavo_ticker_price(&snap, market);
      next_price_deadline = now + pdMS_TO_TICKS(kRefreshTickerPriceMs);
    }

    const bool want_candles =
        force_candles || next_candle_deadline == 0 ||
        ((int32_t)(now - next_candle_deadline) >= 0);
    if (want_candles) {
      const bool zoom_footer_cycle = g_zoom_refresh_busy;
      if (fetch_bitvavo_candles(&snap, preset_idx, market)) {
        next_candle_deadline = now + pdMS_TO_TICKS(kRefreshCandlesMs);
        chart_preset_clear_candle_force();
      } else {
        next_candle_deadline = now + pdMS_TO_TICKS(60UL * 1000UL);
      }
      snap.error_line[0] = '\0';
      if (zoom_footer_cycle) {
        g_zoom_refresh_busy = false;
        g_zoom_footer_clear_req = true;
      }
    }

    snap.ok = true;
    memcpy(&merged, &snap, sizeof(merged));

    portENTER_CRITICAL(&g_snap_mux);
    g_pending_snap = snap;
    g_have_snap = true;
    portEXIT_CRITICAL(&g_snap_mux);

    TickType_t nw = next_price_deadline;
    if ((int32_t)(next_24h_deadline - nw) < 0) {
      nw = next_24h_deadline;
    }
    if ((int32_t)(next_candle_deadline - nw) < 0) {
      nw = next_candle_deadline;
    }
    const TickType_t now2 = xTaskGetTickCount();
    int32_t delta = (int32_t)(nw - now2);
    uint32_t wait_ticks;
    if (delta <= 0) {
      wait_ticks = pdMS_TO_TICKS(200);
    } else if (delta > (int32_t)pdMS_TO_TICKS(120000)) {
      wait_ticks = pdMS_TO_TICKS(120000);
    } else {
      wait_ticks = (uint32_t)delta;
    }
    ulTaskNotifyTake(pdTRUE, wait_ticks);
  }
}

void setup()
{
  Serial.begin(115200);

  extender_init();

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  if (!gfx->begin(12000000UL)) {
    Serial.println("gfx->begin failed");
  }
  gfx->fillScreen(RGB565_BLACK);

  lv_init();
  lv_tick_set_cb(millis_cb_lvgl);
#if LV_USE_LOG != 0 && BITVAVO_MONITOR_SERIAL_DEBUG
  lv_log_register_print_cb(lv_print_cb);
#endif

  lv_screen_init(gfx, HOR_RES, VER_RES);

  touch_init(HOR_RES, VER_RES, 0);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchpad_read_cb);

  bitvavo_monitor_ui_init();

  app_settings_begin();
  settings_ui_init(kAppVersion, kAppVersionDate);

  char active_mkt[24];
  app_settings_get_market(active_mkt, sizeof active_mkt);
  bitvavo_monitor_ui_set_market_heading(active_mkt);

  char wifi_ssid[33], wifi_pass[65];
  const bool have_saved_wifi =
      app_settings_get_wifi(wifi_ssid, sizeof wifi_ssid, wifi_pass, sizeof wifi_pass);

  WiFi.mode(WIFI_STA);
  if (have_saved_wifi) {
    WiFi.begin(wifi_ssid, wifi_pass);
  }

  if (!have_saved_wifi) {
    bitvavo_monitor_ui_show_network_setup_hint();
  }

  xTaskCreatePinnedToCore(bitvavoPollTask, "bitvavo", 12288, nullptr, 1, &g_bitvavo_task_handle, 0);

#if BITVAVO_MONITOR_SERIAL_DEBUG
  Serial.println("Setup done — waiting for WiFi / Bitvavo");
#endif
}

void loop()
{
  settings_ui_poll();

  if (g_zoom_footer_clear_req) {
    g_zoom_footer_clear_req = false;
    bitvavo_monitor_ui_set_footer_updating(false);
  }

  if (!settings_ui_is_active() && !bitvavo_monitor_ui_favorites_overlay_active()) {
    poll_chart_zoom_touch();
  }

  if (g_have_snap) {
    bitvavo_monitor_snapshot_t local;
    portENTER_CRITICAL(&g_snap_mux);
    local = g_pending_snap;
    g_have_snap = false;
    portEXIT_CRITICAL(&g_snap_mux);
    bitvavo_monitor_ui_apply(&local);
  }

  {
    static uint32_t next_clock_ms;
    uint32_t now_ms = millis();
    if (next_clock_ms == 0 || (int32_t)(now_ms - next_clock_ms) >= 0) {
      next_clock_ms = now_ms + 1000;
      bitvavo_monitor_ui_tick_clock();
    }
  }

  settings_ui_tick();

  lv_task_handler();

  static TickType_t next = 0;
  if (xTaskGetTickCount() >= next) {
    if (WiFi.status() == WL_CONNECTED) {
      static bool printed;
      if (!printed) {
#if BITVAVO_MONITOR_SERIAL_DEBUG
        Serial.print("WiFi OK, IP ");
        Serial.println(WiFi.localIP());
#endif
        printed = true;
      }
    }
    next = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
  }

  delay(5);
}

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

/** Shown in the footer when no Wi‑Fi credentials are stored (must match poll-task message). */
#define BITVAVO_MONITOR_MSG_SETUP_DEVICE "Press button to setup this device"

/** Max candles returned for a single Bitvavo GET (used for chart buffer). */
#define BITVAVO_MONITOR_UI_MAX_CANDLES 28

typedef struct {
  double last_eur;
  /** Latest trade price string from GET /ticker/price (verbatim); empty → UI formats `last_eur`. */
  char last_price_display[64];
  double open_eur;
  double high_eur;
  double low_eur;
  /** OHLC per candle in EUR (parsed `double` from API), chronological oldest → newest for drawing. */
  double cdl_o[BITVAVO_MONITOR_UI_MAX_CANDLES];
  double cdl_h[BITVAVO_MONITOR_UI_MAX_CANDLES];
  double cdl_l[BITVAVO_MONITOR_UI_MAX_CANDLES];
  double cdl_c[BITVAVO_MONITOR_UI_MAX_CANDLES];
  uint16_t chart_count;
  char error_line[96];
  /** Second line under 24h OHLC: current chart zoom preset. */
  char chart_zoom_caption[56];
  bool ok;
} bitvavo_monitor_snapshot_t;

void bitvavo_monitor_ui_init(void);
/** Call after UI init if there are no saved Wi‑Fi credentials (footer hint until connected). */
void bitvavo_monitor_ui_show_network_setup_hint(void);
/** Main chart screen (for returning from settings menu). */
lv_obj_t *bitvavo_monitor_ui_get_screen(void);
/** Modal-style screen listing favorites; tap a row to switch active market. No-op if already shown. */
void bitvavo_monitor_ui_show_favorites_overlay(void);
/** True while the favorites picker overlay is the active screen. */
bool bitvavo_monitor_ui_favorites_overlay_active(void);
/** Set top title from Bitvavo market id (e.g. BTC-EUR → "BTC / EUR"). */
void bitvavo_monitor_ui_set_market_heading(const char *market_id);
void bitvavo_monitor_ui_apply(const bitvavo_monitor_snapshot_t *snap);
/** Wake Bitvavo poll task (e.g. after changing market in settings). */
void bitvavo_monitor_wake_poll(void);
/** Updates footer clock when Wi-Fi/data UI is OK (call ~once per second). */
void bitvavo_monitor_ui_tick_clock(void);
/** Footer "Updating.." during chart zoom refresh (call from UI thread only). */
void bitvavo_monitor_ui_set_footer_updating(bool updating);

#include "bitvavo_monitor_ui.h"
#include "app_settings.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

static lv_obj_t *lbl_title;
static lv_obj_t *row_price;
static lv_obj_t *lbl_price;
static lv_obj_t *lbl_pct_change;
static lv_obj_t *lbl_ohlc;
static lv_obj_t *lbl_footer;
static lv_obj_t *lbl_y_axis_max;
static lv_obj_t *lbl_y_axis_min;
static lv_obj_t *candle_canvas;
static void *canvas_buf;
static lv_obj_t *s_main_scr;
/** When true, footer shows chart refresh hint instead of the clock (zoom touch). */
static bool s_footer_zoom_updating;
/** When true, footer shows wall-clock time (hidden on error overlay). */
static bool s_footer_live_clock;

#define H_RES 480
#define V_RES 480
/** Full-width canvas for candles + grid (labels sit above/below, not beside). */
#define CBUF_W (H_RES - 24)
#define CBUF_H (V_RES - 200)
#define CHART_PAD_L 2
#define CHART_PAD_R 4

static lv_color_t col_wick;
static lv_color_t col_body_up;
static lv_color_t col_body_dn;
static lv_color_t col_grid;

static void format_eur_int_dots(char *out, size_t cap, double value);
static void format_eur_price_display(char *out, size_t cap, double value);
static void write_grouped_digit_str(char *out, size_t cap, const char *digits);
static void format_decimal_european_body(char *out, size_t cap, double mag, int dec);
static void format_api_price_string_european(char *out, size_t cap, const char *api_price);
static int frac_digits_from_price_display(const char *s);
static void format_eur_axis_like_price(char *out, size_t cap, double value, int frac_from_headline,
                                       double headline_ref_eur);

static void clamp_rect(int *x0, int *y0, int *x1, int *y1)
{
  if (*x0 < 0) {
    *x0 = 0;
  }
  if (*y0 < 0) {
    *y0 = 0;
  }
  if (*x1 >= CBUF_W) {
    *x1 = CBUF_W - 1;
  }
  if (*y1 >= CBUF_H) {
    *y1 = CBUF_H - 1;
  }
  if (*x0 > *x1 || *y0 > *y1) {
    return;
  }
}

static void canvas_set_px_clip(lv_obj_t *cv, int x, int y, lv_color_t c)
{
  if (x < 0 || y < 0 || x >= CBUF_W || y >= CBUF_H) {
    return;
  }
  lv_canvas_set_px(cv, x, y, c, LV_OPA_COVER);
}

static void canvas_fill_rect(lv_obj_t *cv, int x0, int y0, int x1, int y1, lv_color_t c)
{
  if (x0 > x1) {
    int t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  clamp_rect(&x0, &y0, &x1, &y1);
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      lv_canvas_set_px(cv, x, y, c, LV_OPA_COVER);
    }
  }
}

static void canvas_vline_thick(lv_obj_t *cv, int xc, int y0, int y1, int half_w, lv_color_t c)
{
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  for (int dx = -half_w; dx <= half_w; dx++) {
    int x = xc + dx;
    for (int y = y0; y <= y1; y++) {
      canvas_set_px_clip(cv, x, y, c);
    }
  }
}

/** Map price (EUR) to canvas Y (top = higher price). */
static int price_to_y(double price, double ymin, double ymax)
{
  if (!(ymax > ymin)) {
    ymax = nextafter(ymin, INFINITY);
  }
  double span = ymax - ymin;
  double t = (price - ymin) * (double)(CBUF_H - 1) / span;
  int y = (int)((double)(CBUF_H - 1) - t);
  if (y < 0) {
    y = 0;
  }
  if (y >= CBUF_H) {
    y = CBUF_H - 1;
  }
  return y;
}

/** Horizontal grid step from visible price span (ymax − ymin): coarser grid when span is large. */
static double grid_step_eur_for_span(double span)
{
  if (!(span > 0.0) || !isfinite(span)) {
    return 1e-12;
  }
  if (span > 10000.0) {
    return 10000.0;
  }
  if (span > 1000.0) {
    return 1000.0;
  }
  if (span > 100.0) {
    return 100.0;
  }
  if (span > 10.0) {
    return 10.0;
  }
  if (span > 1.0) {
    return 1.0;
  }
  if (span > 0.1) {
    return 0.1;
  }
  if (span > 0.01) {
    return 0.01;
  }
  if (span > 0.001) {
    return 0.001;
  }
  if (span > 0.0001) {
    return 0.0001;
  }
  /* Very small span: one decade below the range scale */
  double exp10 = floor(log10(span));
  double s = pow(10.0, exp10);
  if (!(s > 0.0) || !isfinite(s)) {
    return 1e-12;
  }
  return s;
}

static void draw_price_grid(lv_obj_t *cv, double ymin, double ymax)
{
  double span = ymax - ymin;
  double step = grid_step_eur_for_span(span);
  if (!(step > 0.0) || !isfinite(step)) {
    step = 1e-12;
  }

  double gc_start = floor(ymin / step) * step;
  if (gc_start < ymin) {
    gc_start += step;
  }

  int guard = 0;
  for (double gc = gc_start; gc <= ymax + step * 1e-9 && guard < 256; gc += step, guard++) {
    int gy = price_to_y(gc, ymin, ymax);
    canvas_fill_rect(cv, CHART_PAD_L, gy, CBUF_W - 1 - CHART_PAD_R, gy, col_grid);
  }

  /* Vertical axis at plot left */
  canvas_vline_thick(cv, CHART_PAD_L, 0, CBUF_H - 1, 0, col_grid);
}

static void draw_candles(lv_obj_t *cv, const bitvavo_monitor_snapshot_t *snap)
{
  lv_canvas_fill_bg(cv, lv_color_hex(0x1a2332), LV_OPA_COVER);

  uint16_t n = snap->chart_count;
  if (n == 0 || n > BITVAVO_MONITOR_UI_MAX_CANDLES) {
    lv_label_set_text(lbl_y_axis_max, "");
    lv_label_set_text(lbl_y_axis_min, "");
    lv_obj_invalidate(cv);
    return;
  }

  double ymin = snap->cdl_o[0];
  double ymax = snap->cdl_o[0];
  for (uint16_t i = 0; i < n; i++) {
    double v[] = {snap->cdl_o[i], snap->cdl_h[i], snap->cdl_l[i], snap->cdl_c[i]};
    for (unsigned k = 0; k < 4; k++) {
      if (v[k] < ymin) {
        ymin = v[k];
      }
      if (v[k] > ymax) {
        ymax = v[k];
      }
    }
  }
  double range = ymax - ymin;
  double pad = range / 80.0;
  if (!(pad > 0.0)) {
    double m = fabs(ymin);
    pad = (m > 0.0) ? m * 1e-9 : 1e-9;
  }
  ymin -= pad;
  ymax += pad;

  draw_price_grid(cv, ymin, ymax);

  const int axis_frac = frac_digits_from_price_display(snap->last_price_display);
  char ax[56];
  format_eur_axis_like_price(ax, sizeof(ax), ymax, axis_frac, snap->last_eur);
  lv_label_set_text(lbl_y_axis_max, ax);
  format_eur_axis_like_price(ax, sizeof(ax), ymin, axis_frac, snap->last_eur);
  lv_label_set_text(lbl_y_axis_min, ax);

  const int plot_w = CBUF_W - CHART_PAD_L - CHART_PAD_R;
  const int slot_w_num = plot_w;
  const int slot_w_den = (int)n;

  for (uint16_t i = 0; i < n; i++) {
    int cx = CHART_PAD_L + ((int)i * slot_w_num + slot_w_num / 2) / slot_w_den;
    int slot_px =
        (CHART_PAD_L + ((int)(i + 1) * slot_w_num / slot_w_den)) -
        (CHART_PAD_L + ((int)i * slot_w_num / slot_w_den));
    int half_body = slot_px / 2 - 1;
    if (half_body < 1) {
      half_body = 1;
    }
    if (half_body > 5) {
      half_body = 5;
    }

    int yo = price_to_y(snap->cdl_o[i], ymin, ymax);
    int yh = price_to_y(snap->cdl_h[i], ymin, ymax);
    int yl = price_to_y(snap->cdl_l[i], ymin, ymax);
    int yc = price_to_y(snap->cdl_c[i], ymin, ymax);

    int wick_top = yh;
    int wick_bot = yl;
    if (wick_top > wick_bot) {
      int t = wick_top;
      wick_top = wick_bot;
      wick_bot = t;
    }

    canvas_vline_thick(cv, cx, wick_top, wick_bot, 1, col_wick);

    int bt = yo < yc ? yo : yc;
    int bb = yo > yc ? yo : yc;
    if (bt < wick_top) {
      bt = wick_top;
    }
    if (bb > wick_bot) {
      bb = wick_bot;
    }
    if (bb - bt < 2) {
      int mid = (bt + bb) / 2;
      bt = mid - 1;
      bb = mid + 1;
      if (bt < wick_top) {
        bt = wick_top;
      }
      if (bb > wick_bot) {
        bb = wick_bot;
      }
      if (bt > bb) {
        bb = bt;
      }
    }

    bool up = snap->cdl_c[i] >= snap->cdl_o[i];
    lv_color_t bc = up ? col_body_up : col_body_dn;

    canvas_fill_rect(cv, cx - half_body, bt, cx + half_body, bb, bc);
  }

  lv_obj_invalidate(cv);
}

/** Digit string only → thousands grouped with '.' (e.g. `1234567` → `1.234.567`). */
static void write_grouped_digit_str(char *out, size_t cap, const char *digits)
{
  if (!out || cap < 2) {
    return;
  }
  if (!digits || !digits[0]) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  size_t len = strlen(digits);
  size_t gi = 0;
  for (size_t i = 0; i < len && gi + 2 < cap; i++) {
    if (i > 0 && (len - i) % 3 == 0) {
      out[gi++] = '.';
    }
    out[gi++] = digits[i];
  }
  out[gi] = '\0';
}

/**
 * Non-negative magnitude, fixed decimal places.
 * Writes body only (no €): `12.345,67` (`.` thousands, `,` decimal).
 */
static void format_decimal_european_body(char *out, size_t cap, double mag, int dec)
{
  if (!out || cap < 4) {
    return;
  }
  if (dec <= 0) {
    char raw[24];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)llround(mag));
    write_grouped_digit_str(out, cap, raw);
    return;
  }

  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%.*f", dec, mag);
  char *dot = strchr(tmp, '.');
  if (!dot) {
    char raw[24];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)llround(mag));
    write_grouped_digit_str(out, cap, raw);
    return;
  }
  *dot = '\0';
  const char *ip = tmp;
  while (ip[0] == '0' && ip[1] != '\0') {
    ip++;
  }
  if (ip[0] == '\0') {
    ip = "0";
  }
  char grouped_int[32];
  write_grouped_digit_str(grouped_int, sizeof(grouped_int), ip);
  snprintf(out, cap, "%s,%s", grouped_int, dot + 1);
}

/** Bitvavo /ticker/price string (ASCII `.` decimal) → € + European grouping. */
static void format_api_price_string_european(char *out, size_t cap, const char *api_price)
{
  if (!out || cap < 4) {
    return;
  }
  if (!api_price || !api_price[0]) {
    out[0] = '\0';
    return;
  }
  if (api_price[0] == '-') {
    char tmp[96];
    format_api_price_string_european(tmp, sizeof(tmp), api_price + 1);
    if (tmp[0] == '\0') {
      out[0] = '\0';
      return;
    }
    snprintf(out, cap, "-%s", tmp);
    return;
  }
  const char *last_dot = strrchr(api_price, '.');
  char int_work[32];
  char frac_work[32];

  if (!last_dot) {
    const char *p = api_price;
    while (*p == '0' && p[1] != '\0') {
      p++;
    }
    write_grouped_digit_str(int_work, sizeof(int_work), p);
    snprintf(out, cap, "\xE2\x82\xAC%s", int_work);
    return;
  }

  size_t ilen = (size_t)(last_dot - api_price);
  if (ilen >= sizeof(int_work)) {
    ilen = sizeof(int_work) - 1;
  }
  memcpy(int_work, api_price, ilen);
  int_work[ilen] = '\0';
  char *ip = int_work;
  while (*ip == '0' && ip[1] != '\0') {
    ip++;
  }
  if (*ip == '\0') {
    ip = int_work;
    int_work[0] = '0';
    int_work[1] = '\0';
    ip = int_work;
  }

  const char *fr_src = last_dot + 1;
  size_t fi = 0;
  while (fr_src[fi] && fi + 1 < sizeof(frac_work) && fr_src[fi] >= '0' && fr_src[fi] <= '9') {
    frac_work[fi] = fr_src[fi];
    fi++;
  }
  frac_work[fi] = '\0';

  char grouped_int[40];
  write_grouped_digit_str(grouped_int, sizeof(grouped_int), ip);
  if (frac_work[0] != '\0') {
    snprintf(out, cap, "\xE2\x82\xAC%s,%s", grouped_int, frac_work);
  } else {
    snprintf(out, cap, "\xE2\x82\xAC%s", grouped_int);
  }
}

/** Whole euros with '.' thousands, prefixed with € (no decimals). */
static void format_eur_int_dots(char *out, size_t cap, double value)
{
  char body[40];
  unsigned long long n = (unsigned long long)llround(fabs(value));
  char raw[24];
  snprintf(raw, sizeof(raw), "%llu", n);
  write_grouped_digit_str(body, sizeof(body), raw);
  if (value < 0) {
    snprintf(out, cap, "-\xE2\x82\xAC%s", body);
  } else {
    snprintf(out, cap, "\xE2\x82\xAC%s", body);
  }
}

/** € display: >= €100 whole euros + thousands dots; €10..€99 two decimals; < €10 three decimals. */
static void format_eur_price_display(char *out, size_t cap, double value)
{
  const char *euro = "\xE2\x82\xAC";
  double mag = fabs(value);
  if (mag >= 100.0) {
    format_eur_int_dots(out, cap, value);
    return;
  }
  int dec = (mag < 10.0) ? 3 : 2;
  char body[48];
  format_decimal_european_body(body, sizeof(body), mag, dec);
  if (value < 0) {
    snprintf(out, cap, "-%s%s", euro, body);
  } else {
    snprintf(out, cap, "%s%s", euro, body);
  }
}

/**
 * Fractional digits after '.' in ticker price string (Bitvavo JSON uses ASCII '.').
 * -1 = no display string → use format_eur_price_display rules per value (same as numeric headline).
 */
static int frac_digits_from_price_display(const char *s)
{
  if (!s || !s[0]) {
    return -1;
  }
  const char *dot = strrchr(s, '.');
  if (!dot) {
    return 0;
  }
  int n = 0;
  for (const char *p = dot + 1; *p >= '0' && *p <= '9'; ++p) {
    n++;
  }
  if (n > 16) {
    n = 16;
  }
  return n;
}

/** Y-axis min/max: same digit policy as headline (API fraction count or same rules as `format_eur_price_display(last_eur)`). */
static void format_eur_axis_like_price(char *out, size_t cap, double value, int frac_from_headline,
                                       double headline_ref_eur)
{
  const char *euro = "\xE2\x82\xAC";
  double mag = fabs(value);
  if (frac_from_headline < 0) {
    double ref = fabs(headline_ref_eur);
    if (ref >= 100.0) {
      format_eur_price_display(out, cap, value);
      return;
    }
    int dec = (ref < 10.0) ? 3 : 2;
    if (mag >= 100.0) {
      format_eur_int_dots(out, cap, value);
      return;
    }
    char body[48];
    format_decimal_european_body(body, sizeof(body), mag, dec);
    if (value < 0.0) {
      snprintf(out, cap, "-%s%s", euro, body);
    } else {
      snprintf(out, cap, "%s%s", euro, body);
    }
    return;
  }
  if (frac_from_headline == 0 && mag >= 100.0) {
    format_eur_int_dots(out, cap, value);
    return;
  }
  char body[48];
  format_decimal_european_body(body, sizeof(body), mag, frac_from_headline);
  if (value < 0.0) {
    snprintf(out, cap, "-%s%s", euro, body);
  } else {
    snprintf(out, cap, "%s%s", euro, body);
  }
}

void bitvavo_monitor_ui_init(void)
{
  col_wick = lv_color_hex(0xb8c5d6);
  col_body_up = lv_color_hex(0x26a69a);
  col_body_dn = lv_color_hex(0xef5350);
  col_grid = lv_color_hex(0x34495e);

  lv_obj_t *scr = lv_obj_create(nullptr);
  s_main_scr = scr;
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, H_RES, V_RES);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1419), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  lbl_title = lv_label_create(scr);
  lv_label_set_text(lbl_title, "Bitvavo");
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xf0b90b), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 12);

  row_price = lv_obj_create(scr);
  lv_obj_remove_style_all(row_price);
  lv_obj_set_width(row_price, H_RES - 16);
  lv_obj_set_height(row_price, LV_SIZE_CONTENT);
  lv_obj_set_layout(row_price, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(row_price, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_price, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row_price, 12, LV_PART_MAIN);
  lv_obj_align(row_price, LV_ALIGN_TOP_MID, 0, 38);

  lbl_price = lv_label_create(row_price);
  lv_label_set_text(lbl_price, "—");
  lv_obj_set_style_text_color(lbl_price, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_price, &lv_font_montserrat_36, LV_PART_MAIN);

  lbl_pct_change = lv_label_create(row_price);
  lv_label_set_text(lbl_pct_change, "");
  lv_obj_set_style_text_font(lbl_pct_change, &lv_font_montserrat_36, LV_PART_MAIN);

  lbl_ohlc = lv_label_create(scr);
  lv_label_set_text(lbl_ohlc, "24h rolling · Bitvavo");
  lv_obj_set_style_text_color(lbl_ohlc, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_ohlc, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl_ohlc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(lbl_ohlc, H_RES - 24);
  lv_label_set_long_mode(lbl_ohlc, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(lbl_ohlc, LV_ALIGN_TOP_MID, 0, 94);

  size_t buf_sz = (size_t)CBUF_W * (size_t)CBUF_H * sizeof(lv_color_t);
  canvas_buf =
      heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!canvas_buf) {
    canvas_buf = heap_caps_malloc(buf_sz, MALLOC_CAP_8BIT);
  }

  candle_canvas = lv_canvas_create(scr);
  lv_obj_set_size(candle_canvas, CBUF_W, CBUF_H);
  lv_obj_align(candle_canvas, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_set_style_border_color(candle_canvas, lv_color_hex(0x2b3d51), LV_PART_MAIN);
  lv_obj_set_style_border_width(candle_canvas, 1, LV_PART_MAIN);

  lbl_y_axis_max = lv_label_create(scr);
  lv_label_set_text(lbl_y_axis_max, "");
  lv_obj_set_style_text_color(lbl_y_axis_max, lv_color_hex(0xc8d6e5), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_y_axis_max, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl_y_axis_max, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_width(lbl_y_axis_max, CBUF_W);
  lv_label_set_long_mode(lbl_y_axis_max, LV_LABEL_LONG_CLIP);
  lv_obj_align_to(lbl_y_axis_max, candle_canvas, LV_ALIGN_OUT_TOP_LEFT, 0, -6);

  lbl_y_axis_min = lv_label_create(scr);
  lv_label_set_text(lbl_y_axis_min, "");
  lv_obj_set_style_text_color(lbl_y_axis_min, lv_color_hex(0xc8d6e5), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_y_axis_min, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl_y_axis_min, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_width(lbl_y_axis_min, CBUF_W);
  lv_label_set_long_mode(lbl_y_axis_min, LV_LABEL_LONG_CLIP);
  lv_obj_align_to(lbl_y_axis_min, candle_canvas, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

  if (canvas_buf) {
    lv_canvas_set_buffer(candle_canvas, canvas_buf, CBUF_W, CBUF_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(candle_canvas, lv_color_hex(0x1a2332), LV_OPA_COVER);
  } else {
    lv_obj_add_flag(candle_canvas, LV_OBJ_FLAG_HIDDEN);
  }

  lbl_footer = lv_label_create(scr);
  lv_label_set_text(lbl_footer, "--:--:--");
  lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_footer, &lv_font_jetbrains_mono_24, LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl_footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(lbl_footer, H_RES - 16);
  lv_label_set_long_mode(lbl_footer, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -8);

  /* Keep scale labels above the canvas bitmap (avoids looking "under" the chart). */
  lv_obj_move_foreground(lbl_y_axis_max);
  lv_obj_move_foreground(lbl_y_axis_min);
  lv_obj_move_foreground(lbl_footer);

  lv_screen_load(scr);

  s_footer_live_clock = false;
}

void bitvavo_monitor_ui_show_network_setup_hint(void)
{
  if (!lbl_footer) {
    return;
  }
  s_footer_live_clock = false;
  s_footer_zoom_updating = false;
  lv_label_set_text(lbl_footer, BITVAVO_MONITOR_MSG_SETUP_DEVICE);
  lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_footer, &lv_font_jetbrains_mono_24, LV_PART_MAIN);
  if (lbl_pct_change) {
    lv_label_set_text(lbl_pct_change, "");
  }
  if (lbl_y_axis_max) {
    lv_label_set_text(lbl_y_axis_max, "");
  }
  if (lbl_y_axis_min) {
    lv_label_set_text(lbl_y_axis_min, "");
  }
  if (lbl_price) {
    lv_label_set_text(lbl_price, "—");
  }
  if (lbl_ohlc) {
    lv_label_set_text(lbl_ohlc, "Wi‑Fi not configured — use Settings (button)");
  }

  if (canvas_buf && candle_canvas) {
    lv_canvas_fill_bg(candle_canvas, lv_color_hex(0x1a2332), LV_OPA_COVER);
    lv_obj_invalidate(candle_canvas);
  }
}

lv_obj_t *bitvavo_monitor_ui_get_screen(void)
{
  return s_main_scr;
}

void bitvavo_monitor_ui_set_market_heading(const char *market_id)
{
  if (!lbl_title || !market_id || !market_id[0]) {
    return;
  }
  const char *dash = strchr(market_id, '-');
  char buf[48];
  if (dash) {
    snprintf(buf, sizeof buf, "%.*s / %s", (int)(dash - market_id), market_id, dash + 1);
  } else {
    strncpy(buf, market_id, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
  }
  lv_label_set_text(lbl_title, buf);
}

void bitvavo_monitor_ui_tick_clock(void)
{
  if (!lbl_footer || !s_footer_live_clock) {
    return;
  }
  if (s_footer_zoom_updating) {
    lv_label_set_text(lbl_footer, "Updating..");
    lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
    return;
  }
  struct tm ti;
  if (!app_time_local_tm(&ti)) {
    lv_label_set_text(lbl_footer, "--:--:--");
    return;
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
  lv_label_set_text(lbl_footer, buf);
}

void bitvavo_monitor_ui_apply(const bitvavo_monitor_snapshot_t *snap)
{
  if (!snap) {
    return;
  }

  if (!snap->ok && snap->error_line[0]) {
    s_footer_live_clock = false;
    s_footer_zoom_updating = false;
    lv_label_set_text(lbl_footer, snap->error_line);
    const bool setup_hint = (strcmp(snap->error_line, BITVAVO_MONITOR_MSG_SETUP_DEVICE) == 0);
    lv_obj_set_style_text_color(lbl_footer,
                                setup_hint ? lv_color_hex(0x6b7c8f) : lv_palette_main(LV_PALETTE_RED),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_footer, &lv_font_jetbrains_mono_24, LV_PART_MAIN);
    lv_label_set_text(lbl_pct_change, "");
    lv_label_set_text(lbl_y_axis_max, "");
    lv_label_set_text(lbl_y_axis_min, "");
    if (setup_hint) {
      lv_label_set_text(lbl_price, "—");
      lv_label_set_text(lbl_ohlc, "Wi‑Fi not configured — use Settings (button)");
      if (canvas_buf && candle_canvas) {
        lv_canvas_fill_bg(candle_canvas, lv_color_hex(0x1a2332), LV_OPA_COVER);
        lv_obj_invalidate(candle_canvas);
      }
    }
    return;
  }

  char line[128];
  if (snap->last_price_display[0]) {
    format_api_price_string_european(line, sizeof(line), snap->last_price_display);
    lv_label_set_text(lbl_price, line);
  } else {
    format_eur_price_display(line, sizeof(line), snap->last_eur);
    lv_label_set_text(lbl_price, line);
  }

  double pct = 0;
  if (fabs(snap->open_eur) > 1e-12) {
    pct = (snap->last_eur - snap->open_eur) / snap->open_eur * 100.0;
  }
  const int pct_tenths = (int)lrint(pct * 10.0);
  if (pct_tenths == 0) {
    lv_label_set_text(lbl_pct_change, "+0,0%");
    lv_obj_set_style_text_color(lbl_pct_change, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
  } else {
    snprintf(line, sizeof(line), "%+.1f%%", pct_tenths / 10.0);
    for (char *p = line; *p; ++p) {
      if (*p == '.') {
        *p = ',';
        break;
      }
    }
    lv_label_set_text(lbl_pct_change, line);
    lv_obj_set_style_text_color(lbl_pct_change,
                                pct_tenths > 0 ? lv_color_hex(0x66bb6a) : lv_color_hex(0xe57373),
                                LV_PART_MAIN);
  }

  char o[48];
  char h[48];
  char l[48];
  format_eur_price_display(o, sizeof(o), snap->open_eur);
  format_eur_price_display(h, sizeof(h), snap->high_eur);
  format_eur_price_display(l, sizeof(l), snap->low_eur);
  snprintf(line, sizeof(line), "24h  O %s  H %s  L %s\n%s", o, h, l,
           snap->chart_zoom_caption[0] ? snap->chart_zoom_caption : "Chart: —");
  lv_label_set_text(lbl_ohlc, line);

  lv_obj_set_style_text_font(lbl_footer, &lv_font_jetbrains_mono_24, LV_PART_MAIN);
  s_footer_live_clock = true;
  if (s_footer_zoom_updating) {
    lv_label_set_text(lbl_footer, "Updating..");
    lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
  } else {
    lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
    bitvavo_monitor_ui_tick_clock();
  }

  if (!canvas_buf || candle_canvas == nullptr) {
    return;
  }

  if (snap->chart_count == 0) {
    lv_canvas_fill_bg(candle_canvas, lv_color_hex(0x1a2332), LV_OPA_COVER);
    lv_label_set_text(lbl_y_axis_max, "");
    lv_label_set_text(lbl_y_axis_min, "");
    lv_obj_invalidate(candle_canvas);
    return;
  }

  draw_candles(candle_canvas, snap);
}

void bitvavo_monitor_ui_set_footer_updating(bool updating)
{
  s_footer_zoom_updating = updating;
  if (!lbl_footer) {
    return;
  }
  if (updating && s_footer_live_clock) {
    lv_label_set_text(lbl_footer, "Updating..");
    lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x6b7c8f), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_footer, &lv_font_jetbrains_mono_24, LV_PART_MAIN);
  } else if (!updating && s_footer_live_clock) {
    bitvavo_monitor_ui_tick_clock();
  }
}

/* ----------------------------------------------------------------------------
 * Favorites picker overlay (top-band tap on main chart)
 * --------------------------------------------------------------------------*/
static lv_obj_t *s_fav_scr;
/** Row strings live as long as the overlay screen is up — we hand
 * indices into this vector to per-row event callbacks. */
static std::vector<String> s_fav_items;

static void favorites_overlay_dismiss(void)
{
  lv_obj_t *to_del = s_fav_scr;
  s_fav_scr = nullptr;
  if (s_main_scr) {
    lv_screen_load(s_main_scr);
  }
  if (to_del) {
    lv_obj_del_async(to_del);
  }
  s_fav_items.clear();
}

static void fav_close_evt_cb(lv_event_t *e)
{
  LV_UNUSED(e);
  favorites_overlay_dismiss();
}

static void fav_row_evt_cb(lv_event_t *e)
{
  size_t idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (idx >= s_fav_items.size()) {
    favorites_overlay_dismiss();
    return;
  }
  String pick = s_fav_items[idx];
  if (pick.length() == 0) {
    favorites_overlay_dismiss();
    return;
  }
  app_settings_set_market(pick.c_str());
  bitvavo_monitor_ui_set_market_heading(pick.c_str());
  bitvavo_monitor_wake_poll();
  favorites_overlay_dismiss();
}

bool bitvavo_monitor_ui_favorites_overlay_active(void)
{
  return s_fav_scr != nullptr && lv_screen_active() == s_fav_scr;
}

void bitvavo_monitor_ui_show_favorites_overlay(void)
{
  if (s_fav_scr) {
    return;
  }

  /* Parse comma-separated favorites blob into stable rows. */
  s_fav_items.clear();
  String blob;
  app_settings_fav_get_blob(&blob);
  for (int pos = 0; pos < (int)blob.length();) {
    int c = blob.indexOf(',', pos);
    if (c < 0) {
      c = blob.length();
    }
    String tok = blob.substring(pos, c);
    tok.trim();
    pos = c + 1;
    if (tok.length()) {
      s_fav_items.push_back(tok);
    }
  }
  std::sort(s_fav_items.begin(), s_fav_items.end(),
            [](const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) < 0; });

  lv_obj_t *scr = lv_obj_create(nullptr);
  if (!scr) {
    return;
  }
  s_fav_scr = scr;
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, H_RES, V_RES);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1419), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Favorites");
  lv_obj_set_style_text_color(title, lv_color_hex(0xf0b90b), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

  lv_obj_t *close_btn = lv_button_create(scr);
  lv_obj_set_size(close_btn, 88, 38);
  lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2d3d52), LV_PART_MAIN);
  lv_obj_remove_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *cl = lv_label_create(close_btn);
  lv_label_set_text(cl, "Close");
  lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(cl);
  lv_obj_add_event_cb(close_btn, fav_close_evt_cb, LV_EVENT_CLICKED, nullptr);

  /* Scrollable list area below header. */
  lv_obj_t *list = lv_obj_create(scr);
  lv_obj_set_size(list, H_RES - 16, V_RES - 70);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
  lv_obj_set_layout(list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  if (s_fav_items.empty()) {
    lv_obj_t *empty = lv_label_create(list);
    lv_label_set_text(empty, "No favorites yet.\nOpen Settings → Ticker to add some.");
    lv_obj_set_style_text_color(empty, lv_color_hex(0xa7b6c4), LV_PART_MAIN);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, LV_PART_MAIN);
  } else {
    char curr[40];
    app_settings_get_market(curr, sizeof curr);
    for (size_t i = 0; i < s_fav_items.size(); ++i) {
      lv_obj_t *row = lv_button_create(list);
      if (!row) {
        break;
      }
      lv_obj_set_size(row, H_RES - 32, 56);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      const bool is_active = strncmp(curr, s_fav_items[i].c_str(), sizeof curr) == 0;
      lv_obj_set_style_bg_color(row,
                                is_active ? lv_color_hex(0x35506b) : lv_color_hex(0x1a2332),
                                LV_PART_MAIN);

      lv_obj_t *t = lv_label_create(row);
      lv_label_set_text(t, s_fav_items[i].c_str());
      lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), LV_PART_MAIN);
      lv_obj_set_style_text_font(t, &lv_font_montserrat_18, LV_PART_MAIN);
      lv_obj_center(t);

      lv_obj_add_event_cb(row, fav_row_evt_cb, LV_EVENT_CLICKED,
                          reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
  }

  lv_screen_load(scr);
}

# Bitvavo Monitor based on Seeedstudio Indicator D1

Created by Richard Garsthagen - the.anykey@gmail.com

Version 1.0 - May 7th 2026 - Initial release

Hardware is based on: Seeed Studio — SenseCAP Indicator D1 - https://www.seeedstudio.com/SenseCAP-Indicator-D1-p-5643.html

## Demo

https://youtube.com/shorts/rs2AxUEtQ34?si=kvXED0Hs_FpiZ3fH

## What it does

<img src=images/example2.jpg width=200><img src=images/example3.jpg width=200><img src=images/example5.jpg width=200><img src=images/example4.jpg width=200><img src=images/example6.jpg width=200><img src=images/example7.jpg width=200>


- **Live price** and **rolling 24h** open / high / low plus **% change** for a chosen EUR pair (e.g. BTC-EUR).
- **Candlestick chart** with several zoom levels (time span presets); **press and release** in the lower **~70%** of the screen — **left half** zooms in, **right half** zooms out.
- **Favorites** (top area gesture) and **Settings** (device button): Wi‑Fi setup, ticker search, time zone / NTP, app info, and optional **clear all saved settings**.


Public API base used by the app: `https://api.bitvavo.com/v2` ([REST overview](https://docs.bitvavo.com/docs/rest-api/)).

## Hardware

| Item | Link |
|------|------|
| SenseCAP Indicator D1 | [Seeed Studio — SenseCAP Indicator D1](https://www.seeedstudio.com/SenseCAP-Indicator-D1-p-5643.html) |

For ESP32-S3 Arduino bring-up, display, and toolchain hints, Seeed’s wiki is the primary reference:

- **[SenseCAP Indicator ESP32 Arduino](https://wiki.seeedstudio.com/SenseCAP_Indicator_ESP32_Arduino/)**

Install the correct **USB driver** for serial upload (see the wiki / your OS).

## Software: Arduino IDE

The sketch lives under **`firmware/BitvavoMonitor/`**. Open **`BitvavoMonitor.ino`** in the Arduino IDE (File → Open… and select that file).

### 1. Board and core

- Install the **ESP32** board support package (Espressif) in the Boards Manager.
- Select a board matching the Indicator’s **ESP32-S3** configuration. The sketch comments use **“ESP32S3 Dev Module”** as a baseline; align with the **SenseCAP Indicator ESP32 Arduino** wiki for the exact menu choices on your core version.
- Enable **OPI PSRAM** if your board menu exposes it (the UI uses PSRAM for the chart canvas when available).
- Choose a **partition scheme** with enough **app** space for HTTPS + LVGL (e.g. **Huge APP**, **No OTA**, or **3MB APP** on **16 MB** flash if you see “sketch too big”). See [Arduino partition guidance](https://support.arduino.cc/hc/en-us/articles/360013825179).

<img src=images/Arduino-setup.png width=200>

### 2. Libraries (Library Manager or ZIP)

Install the versions the sketch expects (see comments at the top of `BitvavoMonitor.ino`):

General seeedstudio documentation: https://wiki.seeedstudio.com/SenseCAP_Indicator_ESP32_Arduino/

| Library | Notes |
|---------|--------|
| **lvgl** | 9.2.x |
| **GFX Library for Arduino** | Arduino_GFX |
| **TouchLib** | Often installed as a **ZIP** (https://github.com/mmMicky/TouchLib) |
| **Anitracks PCA95x5** | I/O expander; remove duplicate copies if the IDE reports conflicts |
| **ArduinoJson** | v7 |

### 3. LVGL configuration

Copy **`firmware/BitvavoMonitor/lv_conf.h`** into your LVGL library folder as **`lv_conf.h`** (next to the `lvgl` library), per [LVGL Arduino configuration](https://docs.lvgl.io/). This project needs **`LV_USE_CANVAS 1`** and **`LV_USE_CHART 0`**.

### 4. Build flags (optional)

| Define | Purpose |
|--------|--------|
| `BITVAVO_MONITOR_DISPLAY_TZ_POSIX` | Override POSIX `TZ` for the footer clock (default in sketch is EU CET/CEST-style) |
| `BITVAVO_MONITOR_SERIAL_DEBUG=1` | Extra USB Serial logs (markets UI, optional LVGL log, LAN IP). Default is **off** in `app_settings.h`. |

Set global defines in `platformio.ini` if you use Platform IO, or use the Arduino IDE’s **compiler flags** / build extras if your workflow supports them.

### 5. Upload

1. Connect the Indicator over **USB** (data-capable cable) and select the correct **COM** port.
2. **Compile and upload** `BitvavoMonitor.ino`.
3. On first boot without saved Wi‑Fi, follow on-screen prompts: **Settings** (physical button) → **WiFi Setup**, then **Ticker Setup** to pick markets and favorites.

## Repository layout

```text
firmware/BitvavoMonitor/   # Main sketch (.ino), UI, settings, LVGL config, display/touch helpers
```

## Disclaimer

The on-device **Information** screen states that this app is **not affiliated with Bitvavo B.V.**; “Bitvavo” is used only to describe compatibility with the Bitvavo platform.

## License / third-party

- UI font assets follow their respective licenses (e.g. JetBrains Mono — SIL OFL; see font files and vendor headers in the sketch folder).

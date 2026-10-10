# MiliTV / IPSTube — open firmware (clean-room)

Open firmware for the **IPSTube MiliTV** desk gadget (ESP32-S3 + ST7789 IPS
panel + speaker + battery). Written from scratch: no stock code, strings,
images or audio are reused. The only things taken from the factory firmware are
**hardware facts** — GPIO numbers, the panel's register init table, peripheral
base addresses and one BLE UUID — each recorded with its evidence in
`include/pins.h` and the RE artifacts.

This is a personal-project-turned-community base. The goal is a readable,
hackable firmware you can build apps on, not a drop-in clone of the stock
firmware.

---

## Apps

Three apps, tap **−** / **+** to swipe (hold them for backlight). The last app
is remembered across reboots.

| App | What it shows | Free data source |
|-----|---------------|------------------|
| **CRYPTO** | live price + 24 h change + a scrolling **24-hour** chart (5 m candles), any Binance USDT pair | Binance public WebSocket + REST (no key) |
| **CLOCK** | NTP time, date, city, weather condition + temp + high/low — text only, laid out in a fixed inner frame | NTP pools + Open-Meteo (no key) |
| **STOCK** | live quote, % change, market-open flag, ~1 month daily sparkline | Yahoo Finance chart API (no key) |

## New in this version: the rest of the hardware

The board has more than a screen. Previous versions ignored most of it.

- **Audio (I2S speaker)** — `src/audio.cpp`. A small, dependency-free tone
  engine (no MP3 decoder) with alerts, key clicks and a boot chime, and the
  console verb `beep <hz> <ms>`. Non-blocking: it feeds the I2S DMA in slices
  from `loop()`, so a tone never stalls the UI. Pins are **proven** by RE
  (BCK=12, WS=11, DOUT=13) plus the amplifier enable pair (40 then 39).
- **Bluetooth LE** — `src/ble.cpp`. A live console over the **Nordic UART
  Service** — the same UUIDs the stock firmware used, so any generic BLE-UART
  app (nRF Connect, Serial Bluetooth Terminal) works with no custom client.
  WiFi setup stays on the captive portal; BLE also works out of WiFi range.
- **Battery monitor** — `src/battery.cpp`. Samples the cell on ADC1 and can
  warn low. See the honesty note below — the channel is inferred, not named.
- **11 MB asset partition (LittleFS)** — `src/assets.cpp`. The factory filled
  it with album art; the open firmware mounts it so you can drop in fonts,
  icons and sounds. `pio run -t uploadfs` pushes the `data/` folder.
- **A real 16 MB partition table** — `partitions.csv` now mirrors the factory
  layout instead of PlatformIO's `huge_app.csv`, which had declared a 3 MB app0
  and a bogus 896 KB `spiffs` overlapping the real OTA slot. You get a proper
  **second app slot (app1) for OTA** and the full **11 MB** filesystem.

## Controls

| Key | Tap | Hold |
|-----|-----|------|
| PLUS (+) | next app | backlight up (repeats) |
| MINUS (−) | previous app | backlight down (repeats) |
| POWER | screen sleep / wake | at boot: setup portal |

A **command console** runs over both USB serial and BLE (same verbs):

```
help                 list commands
app crypto|clock|stock   switch app
next | prev          cycle apps
ticker <PAIR>        switch the crypto pair at runtime (e.g. ticker ETHUSDT)
px                   print the chart's axis + live price (what it draws)
fb [x0 y0 x1 y1]     ASCII-dump the framebuffer region (default full screen):
                     '#' lit, '.' black — check a layout without reading the glass
status               one-line telemetry (app, WS, WiFi, battery)
batt                 battery voltage / percent
batt scan            sample every ADC1 channel (battery calibration aid)
fs                   asset-partition usage
beep <hz> <ms>       play a tone
quiet                stop audio
bright <0-100>       backlight percent
portal | quit        open / close the WiFi setup portal
```

On USB serial the classic single-key shortcuts still work when typed on their
own line: `+`/`-` backlight, `n`/`p` app, `w`/`x` portal.

## WiFi setup

Normal boot auto-joins saved credentials in ~3 s. To configure, hold **POWER
for 3 s** (at power-on *or* while running — the device has a battery, so there
is no power cycle needed). Join the `MILITV-SETUP` AP from a phone/laptop and
open `http://192.168.4.1`. One page configures WiFi **and** every app setting
(crypto ticker, timezone, weather lat/lon/city, stock id).

**Saving never wipes WiFi** — credentials live in WiFiManager's NVS keys, app
settings in a separate `militv` namespace. Leave the WiFi fields blank to keep
the current network. A save reboots once to apply cleanly.

---

## Build & flash

```bat
cd openfw
pio run                 ; build
pio run -t upload       ; flash app0 (needs the port set, see platformio.ini)
pio run -t uploadfs     ; optional: upload the data/ asset folder
pio device monitor
```

Requires PlatformIO (`espressif32`). Libraries are fetched automatically:
`tzapu/WiFiManager`, `links2004/WebSockets`, `bblanchon/ArduinoJson`,
`h2zero/NimBLE-Arduino`.

### Feature switches

Every added subsystem can be compiled out in `platformio.ini`:

```ini
build_flags =
    -DMILITV_BLE=0        ; drop the Bluetooth console
    -DMILITV_AUDIO=0      ; drop the I2S tone engine
    -DMILITV_BATTERY=0    ; drop the ADC battery gauge
```

A lean build with all three off fits comfortably and leaves the original
serial-only behavior.

> **WiFi + BLE coexistence**: the ESP32-S3 has *one* radio. When `MILITV_BLE=1`
> the firmware sets `WiFi.setSleep(true)` — the WiFi driver hard-aborts if
> modem sleep is disabled while Bluetooth is enabled. The reconnect watchdog,
> not modem sleep, is what keeps the link alive.

---

## Hardware map (and how confident we are)

The rule in this repo: a pin is "proven" only with a probe or a quoted RE
artifact. Everything else is marked.

| Net | Pin | Confidence | Evidence |
|-----|-----|-----------|----------|
| TFT SCLK / MOSI | 17 / 14 | **proven** | stock `SPIClass::begin(obj,17,-1,14,-1)`; RED-fill probe |
| TFT CS / DC / RST | 19 / 18 / 20 | **proven** | RED fill order; init pulse |
| Backlight | 21 (LEDC ch1, 20 kHz, 8 bit) | **proven** | `ledcSetup(1,20000,8)` + `ledcAttachPin(21,1)` |
| POWER / PLUS / MINUS | 4 / 5 / 6 | **proven** | 3 debounced inputs pinned by live GPIO probe |
| Panel | ST7789 320×240 rot3, BGR, INVON | **proven** | full init table; MADCTL BGR (else red→blue) |
| Audio I2S BCK / WS / DOUT | 12 / 11 / 13 | **proven** | `FUN_42011ca0(obj,0xc,0xb,0xd,-1)` + `i2s_pin_config_t` field-name strings |
| Amplifier enable | 40 → 39 (pair, rest LOW) | strong | stock raises 40, waits 100 ms, then 39; lowers in reverse |
| Battery sense | ADC1 ch0 = GPIO1, 11 dB, 12-bit | **uncertain** | only ADC1 atten call in the image — **see note** |
| Console | USB-UART (UART0 defaults) @115200 | **proven** | probe + this firmware's own log |
| stock UART2 | RX 38 / TX 45 | strong | left untouched (routing Serial there silences the log) |

### Honesty note: the battery pin

RE found exactly one ADC1 channel configuration in the stock image and
inferred a battery behind it, on **GPIO1**. On this unit **GPIO1 reads a flat
0 mV**, while `batt scan` shows the three button pull-ups (channels 3/4/5,
i.e. GPIO4/5/6) reading a healthy ~3.03 V — so the ADC path itself is fine and
the inference, not the code, is what needs checking. Possibilities: the divider
is on a different pin, the sense net needs the stock's GPIO8/9/27 outputs
driven first, or this revision simply lacks it.

So the firmware treats a ~0 V reading as **"unknown"**, never "empty" —
`batteryStatus()` prints `BATT n/a` and the low-battery beep stays silent.
If you find the right channel on your unit, run `batt scan`, then set
`BATT_ADC_CHANNEL` (and `BATT_DIVIDER`, default 2.0) in `include/pins.h` /
`src/battery.cpp`. PRs with a measured divider ratio are very welcome — that
is the one number this repo cannot recover by static analysis alone.

---

## Project layout

```
openfw/
  platformio.ini          build config + feature switches
  partitions.csv          factory-matching 16 MB table (app0/app1/11 MB fs)
  data/                   asset partition contents (uploadfs target)
  include/
    pins.h                board profile: every pin + evidence + confidence
    tft.h                 ST7789 driver: PSRAM framebuffer + dirty-rect flush
    chart.h  clockapp.h  stockapp.h  cfg.h  netcfg.h
    audio.h  ble.h  battery.h  assets.h
  src/
    main.cpp              app switcher, crypto WS, keys, command console
    chart.cpp  clockapp.cpp  stockapp.cpp  cfg.cpp  netcfg.cpp
    audio.cpp  ble.cpp  battery.cpp  assets.cpp
```

### Rendering notes (for anyone adding UI)

The display has a **320×240 RGB565 framebuffer in PSRAM**; primitives draw into
it and `flush()` pushes only the dirty rectangles. Two rules if you add
drawing:

1. **Drawing must be flushed.** Anything drawn inside `loop()` is flushed at
   the end of the pass; code in `setup()` or a one-off screen must call
   `tft.flush()` itself.
2. **Never draw from a callback on another task** (the WebSocket library runs
   its own task). Post a flag and drain it in `loop()` — see `cryptoDrainWs()`.
   The BLE and audio modules follow the same rule.
3. **Keep text inside x40..299 and erase the whole slot.** The bezel covers
   x<40 and the glued frame covers x>300, and all three apps put their content
   inside a shared inner frame (`FX`/`FR` in `clockapp.cpp`, `CH_X0/CH_X1` in
   `chart.h`). Truncate every string to its slot width before drawing, and give
   every field an erase box that spans its entire slot — a partial-redraw field
   that erases less than it draws leaves fragments behind when a value shrinks
   (a stray half-erased "NG" from the clock's loading message is what users saw
   as a phantom "IG"). `fb` on the console dumps the framebuffer as ASCII if
   you need to check what is really on the glass.

### Changing the crypto chart window

The chart's timeframe lives in `include/chart.h` and nowhere else:

```c
#define CH_INTERVAL "5m"   // candle size
#define CH_N 288           // 288 x 5 m = 24 h
```

The REST history URL and the WebSocket stream name are both built from these
two, so they cannot drift apart. Keep them describing the same span: Binance
caps one klines request at 1000 candles, so `1h x 24` or `15m x 96` also give a
day, and `5m x 288` was chosen because the plot is 260 px wide — 288 points is
~1.1 per pixel, the panel's own resolution. The header's 24 h percentage always
comes from Binance's separate rolling-24 h ticker, so it stays correct no matter
which window you plot.

Text is a 5×7 bitmap font (A–Z, digits, symbols; uppercase-only). A real
antialiased font is a wanted contribution — the framebuffer and the 11 MB
asset partition are both there for it.

---

## Restoring stock

Keep your own full-flash backup. Flashing the open firmware only writes the
app0 slot; the partition table, NVS (your WiFi creds) and the factory
filesystem are left alone.

```bat
esptool --port COM8 write_flash 0x10000 your_stock_app0.bin
```

## Credits & license

Clean-room reverse engineering, firmware and UI by the project author.
Add a license of your choice before publishing (e.g. MIT for the code).

`speedtest`, datasheet-derived register tables and the ST7789 init sequence are
hardware facts, not copied code.

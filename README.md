# MiliTV / IPSTube — open firmware

Clean-room open firmware for the **IPSTube MiliTV** desk gadget: an ESP32-S3
with a 320×240 IPS panel, a speaker, a battery and Bluetooth LE.

No stock code, strings, images or audio are reused. Everything in `openfw/` was
written from scratch; the only things taken from the factory firmware are
**hardware facts** — GPIO numbers, the panel's register init table, one BLE UUID
— each recorded with its evidence in `openfw/include/pins.h`.

## Features

| | |
|---|---|
| **CRYPTO** | live price, 24 h change and a scrolling 24-hour chart, any Binance USDT pair |
| **CLOCK** | NTP time + weather with drawn condition icons |
| **STOCK** | live quote, % change, market flag and a 1-month sparkline |
| **Audio** | I2S tone engine for alerts, key clicks and a boot chime |
| **Bluetooth LE** | live console over the Nordic UART Service (works with any BLE-UART app) |
| **Battery** | ADC cell gauge with low warning |
| **11 MB flash store** | LittleFS partition for community fonts, icons and sounds |
| **Captive portal** | configure WiFi *and* every app setting from a phone |

Buttons: tap **−** / **+** to switch apps, hold them for backlight, **POWER** to
sleep/wake (hold 3 s at boot for the setup portal).

All data sources are free and key-less, and every subsystem can be compiled out
with a feature switch. See **[openfw/README.md](openfw/README.md)** for the full
guide: build, flash, the hardware map with confidence levels, and the console
commands.

## Quick start

```bat
cd openfw
pio run -t upload       ; flash
pio device monitor      ; logs at 115200
```

Then hold **POWER for 3 s** at boot, join the `MILITV-SETUP` WiFi AP and open
`http://192.168.4.1` to configure WiFi and the apps. Full instructions and the
first-boot walkthrough are in [openfw/README.md](openfw/README.md).

## Hardware

ESP32-S3 (16 MB flash, 2 MB PSRAM) · ST7789 320×240 IPS · 3 tactile keys ·
I2S speaker · Li-ion cell · CH340 USB-UART.

The display driver keeps a framebuffer in PSRAM and pushes only dirty regions,
which is what makes the UI update smoothly instead of flickering. Notes for
anyone adding UI are at the end of [openfw/README.md](openfw/README.md).

## Repository layout

```
openfw/            the firmware (PlatformIO project)
  src/  include/   app + driver sources, one module per subsystem
  partitions.csv   factory-matching 16 MB partition table
  data/            asset-partition contents (pio run -t uploadfs)
ANALYSIS.md        reverse-engineering notes: chip, flash layout, endpoints
LICENSE            MIT
```

## Reverse engineering

`ANALYSIS.md` documents how the hardware was recovered: the full flash dump,
the partition layout, the Ghidra workflow for the Xtensa image, and the
recovered cloud/BLE surface. The dump itself is not published — it contains the
device owner's NVS, including WiFi credentials.

## Contributing

Pull requests are welcome, and a few things are explicitly wanted:

- **A real font.** Text is a 5×7 bitmap font (uppercase only). The PSRAM
  framebuffer and the 11 MB asset partition are both already there for an
  antialiased font.
- **A working MP3/AAC decoder** on top of the I2S bring-up, for alarm sounds.
- **The battery divider ratio.** One number this project cannot recover by
  static analysis — see the honesty note in [openfw/README.md](openfw/README.md).

## License

MIT (see [LICENSE](LICENSE)). Unaffiliated with the device vendor; flashing
custom firmware may void your warranty.

# MiliTV / IPSTube — Device Reverse-Engineering Baseline

Analysis date: 2026-10-08. All artifacts in `flash_dump/`.

## Deliverables so far

| File | What it is |
|---|---|
| `flash_dump/full_flash_16MB.bin` | Full SPI flash dump, 16 MiB. sha256 `f48d0e48…fb15461` (see `sha256.txt`) |
| `flash_dump/app0.bin` / `app0.elf` | Factory app partition and ELF-ified image (7 loadable segments mapped at runtime addresses) |
| `flash_dump/spiffs.bin` | Data partition (actually **LittleFS**, not SPIFFS) |
| `flash_dump/fs_extracted/` | All 46 recovered asset files incl. web UI |
| `flash_dump/core_only.elf` | Crash dump as standard ELF core (18 tasks) |
| `flash_dump/strings_all.txt` | Every printable string in the app image |
| `funcs.txt`, `xrefs.txt`, `key_fns.c`, `ble_setup.c`, `crash_window.txt` | Ghidra headless analysis output (6,473 functions) |

## Hardware / chip identity

- MCU: **ESP32-S3** rev 0.2, QFN56, dual-core 240 MHz, 2 MB embedded PSRAM (AP_3v3)
- Flash: 16 MB quad SPI (mfr 0x5e), 80 MHz DIO
- USB-UART: CH340 (VID 1A86 PID 7523) on **COM8**, 3V3 logic
- MAC `98:A3:16:xx:xx:xx` (redacted in this public copy); BLE advertised name **IPSTV_I_xxxxxx**
- Security eFuses all **factory defaults**: no secure boot, no flash encryption, no download-mode lock, JTAG not disabled. The "closed" firmware is only closed by NDA, not by silicon.

## Flash layout (16 MB)

| Partition | Offset | Size | Notes |
|---|---|---|---|
| bootloader | 0x0000 | 8 KB | stock ESP-IDF v4.4.6 stage 2 |
| nvs | 0x9000 | 20 KB | WiFi creds, BLE name, ticker settings |
| otadata | 0xE000 | 8 KB | OTA boot selector |
| app0 (ota_0) | 0x10000 | 2560 KB | **active factory app** |
| app1 (ota_1) | 0x290000 | 2560 KB | empty (never OTA-updated) |
| data (littlefs) | 0x510000 | 11136 KB | 2784 × 4 KB blocks, LFS 2.1, mostly full |
| coredump | 0xFF0000 | 64 KB | one panic captured |

## Factory firmware profile

- Built with **PlatformIO**, env name `ipsesp32s3`, Arduino framework
  arduino-esp32 **2.0.14** / ESP-IDF **v4.4.6**; compile stamp Oct 4 2023.
- Libraries identified: **TFT_eSPI** (S3 processor file present), **NimBLE-Arduino**,
  **ESP32-audioI2S** (MP3/AAC/FLAC decoders), **esp_littlefs**, ArduinoJson-style
  HTTP clients, esp32-webserver (`/update`, `/file`).
- Tasks recovered from core dump: `loopTask`, `WeatherTask`, `TickerTask`,
  `NTPTask`, `audioplay`, `arduino_events`, `nimble_host`, `btController`,
  `wifi`, `sys_evt`, `async_tcp`, `mdns`, `esp_timer`, `Tmr Svc`, `ipc0/1`, `IDLE`.
- Feature surface: JPEG/GIF slideshow albums, boot/sleep animations, alarms
  (mp3/aac/m4a/wav), NTP clock faces (`/time/%d.jpg`), weather + crypto ticker
  from `service.ipstube.com` (`/weather/getWeather?location=`, `/ticker/getTicker?ticker=`),
  HLS (`m3u8`) stream fallback handling, BLE serial (NimBLE Uart service UUID
  `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`), HTTP firmware-OTA + file upload web UI.

## Network endpoints

- Cloud: `service.ipstube.com` (weather, ticker), NTP pool `ntp.aliyun.com`,
  `ntp.tencent.com`, `time.apple.com`, `time.windows.com`.
- Local HTTP server: `GET /www/index.html`, `POST /update` (firmware `.bin`),
  `POST /file` (multipart file upload to any LittleFS path), plus BLE console.
- NVS settings seen: STA ssid/pswd (redacted dump), BLE name `IPSTV_I_xxxxxx`,
  `BTC/USD` + `Binance` ticker config, misc `eeprom` blob.

## Crash forensics

One captured panic (partition not erased afterwards): crashed in **loopTask**
(TCB 0x3fcec14c), exception frame references code address **0x4208f9ad**
(disassembled in `crash_window.txt` — near audio/format dispatch code).
Task list and stacks preserved in `core_only.elf` — loadable in GDB/IDA for
full backtrace if needed.

## Ghidra workflow established (repeatable)

1. Ghidra 12.1.4 at `C:\ghidra\ghidra_12.1.4_PUBLIC`, JDK 21 (Temurin, both set via `setx`).
2. Project `C:\ghidra\projects\militv` contains `app0.elf`.
3. Import: `analyzeHeadless.bat C:\ghidra\projects militv -import flash_dump/app0.elf -processor "Xtensa:LE:32:default"`.
4. Scripts in `scripts/` (`DumpAll.java`, `DecompileFns.java`, `DumpCrash.java`) run via `-process app0.elf -scriptPath scripts -postScript …`.

Note on REA MCP: it installed fine and the bridge works, but its artifact gate
only accepts x86 / ARM / ARM64 ELFs — Xtensa (machine 94) is rejected, so Ghidra
is driven headless directly (same engine REA would use). Everything REA exposes
for ELF analysis is reproducible with the scripts above.

## Open-firmware roadmap

1. **Scaffold** a PlatformIO project `env:ipsesp32s3_open` matching the factory
   image geometry (keep partition table, LittleFS layout, `/www`, `/boot`,
   `/backup`, `/config`, `/album`, `/animated`, `/time` directories) so the
   device's existing web updater can flash it and user assets survive.
2. **Pin map recovery** (next RE pass): single-step `TFT_eSPI_ESP32_S3` init and
   audio/I2S setup in Ghidra; log which GPIOs are written (`iowr` to GPIO_OUT_REG).
   Alternative faster route: run factory firmware with UART log at boot and read
   pin assignments from `[TFT]`-style driver logs, or buzz out the custom board.
3. **Replacement firmware** candidates: ESP-IDF + LVGL or TFT_eSPI + littlefs,
   port the feature set already visible in strings (clock faces, album art,
   BLE config, HTTP upload, audio alarms). Start with a minimal "hello screen"
   to validate pins/display panel (likely ST7789/ILI9341-class SPI panel),
   then grow features.
4. **Install path** (no disassembly needed): `POST /update` with our `.bin`
   goes to **app1** OTA slot — factory app stays recoverable; otadata can be
   reverted by re-flashing the saved dump.

## Known gaps / next steps

- Display panel driver + exact TFT pin map not yet extracted (needs the
  targeted Ghidra pass on TFT_eSPI init, or boot-log sniffing).
- BLE command protocol: NimBLE Uart RX characteristic handler not yet
  decompiled (`FUN_42009a74`, `FUN_42009d40` are leads).
- Web handler dispatch table around `0x42000440–0x42000880` maps `/update`,
  `/file` routes; deeper cleanup could yield the exact multipart format.
- `esp32s3 ROM` symbols not loaded, so callx into ROM shows as raw addresses.

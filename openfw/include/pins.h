// MiliTV (IPSTube) board profile — hardware facts recovered by clean-room
// reverse engineering of the stock firmware (pins, panel, buses only;
// no stock code or assets are reused by the open firmware).
//
// Each block below carries its evidence and a confidence note. The rule for
// this project: never promote a pin to "proven" without a probe or a quoted
// RE artifact. Unproven pins are marked and kept overridable.
#pragma once

// Feature switches live in platformio.ini build_flags; default them on so a
// bare `pio run` (or a checkout that edits the ini) still builds a full device.
#ifndef MILITV_BLE
#define MILITV_BLE 1
#endif
#ifndef MILITV_AUDIO
#define MILITV_AUDIO 1
#endif
#ifndef MILITV_BATTERY
#define MILITV_BATTERY 1
#endif

// ---- Display: ST7789 240x320 IPS on software-driven SPI ----
// Proven by RE + on-screen probe (combo 20 showed solid RED):
//   SPIClass::begin(obj, 17, -1, 14, -1) => SCLK=17 MOSI=14, no MISO
//   CS=19 / DC=18 (RED filled with this order; swapped order = black)
//   RST=20 (HIGH-5ms-LOW-20ms-HIGH-150ms pulse at init)
//   BL=21 active-high, driven by LEDC channel 1, 20 kHz, 8-bit
#define TFT_SCLK 17
#define TFT_MOSI 14
#define TFT_MISO -1
#define TFT_CS   19
#define TFT_DC   18
#define TFT_RST  20
#define TFT_BL   21
#define TFT_BL_CH 1
#define TFT_BL_FREQ 20000
#define TFT_BL_BITS 8
#define TFT_W 320
#define TFT_H 240
// Panel tolerates well above 20 MHz, but 20 MHz is proven stable on this
// board's wiring. Treat raising it as an experiment requiring eyes-on glass.
#define TFT_SPI_HZ 20000000

// ---- Console UART (host debug) ----
// The open firmware prints on UART0's default pins (the CH340 bridge).
// GPIO38/45 are the stock firmware's *second* UART to an on-board peripheral:
// routing Serial there silences the log entirely, so they are NOT the console.
#define UART_BAUD 115200
#define STOCK_UART2_RX 38
#define STOCK_UART2_TX 45

// ---- Audio I2S -> external amplifier -> speaker ----
// PROVEN by RE. The stock firmware shipped ESP32-audioI2S (mp3/aac/flac/wav
// alarm playback). The I2S object is configured by one call in the audioplay
// task (tgtdec.txt:29) and the argument order is fixed by the ESP-IDF field
// names the same helper checks against (mtxdec.txt ~82-107):
//   FUN_42011ca0(obj, 0xc, 0xb, 0xd, -1)  ->  (bck=12, ws=11, dout=13, in=none)
// Disassembly confirms the immediates: pin_clusters.txt:628-630
//   movi.n a13,0xd / movi.n a12,0xb / movi.n a11,0xc
// The roles come from the i2s_pin_config_t order the setter's error strings
// validate: data_out_num / ws_io_num / bck_io_num.
// NOTE: none of these collide with TFT_BL (21) — see the GPIO21 warning below.
#define AUD_BCK 12
#define AUD_WS  11
#define AUD_DOUT 13

// Amplifier enable. STRONG (not proven-by-name): the stock audio path brings up
// GPIO40 then GPIO39 as an enable PAIR, both OUTPUT and resting LOW, raising
// 40 first, waiting 100 ms, then 39 (tgtdec.txt:126-138, enable) and lowering
// them in reverse (disable). keydec.txt:6-13 shows the same pair plus GPIO48
// configured just before the audioplay task is spawned.
// GPIO48 is the WEAK one: open-drain, driven LOW once, never raised anywhere in
// the recovered code. So 39/40 are used; 48's purpose is unresolved.
// GPIO21 IS NOT AN AUDIO PIN: pinscan3 flagged 21 in the audio function, but
// disassembly shows it is the `map(vol,0,100,0,0x15)` bound — a max volume
// index, not a GPIO (tgtdec.txt:30). The only real GPIO21 user is the LEDC
// backlight; driving it here would kill the screen.
#define AUDIO_AMP_EN1 40  // raised first / lowered last
#define AUDIO_AMP_EN2 39  // raised second / lowered first
#define AUDIO_AMP_EN_ACTIVE HIGH

// ---- Battery sense ----
// STRONG. The stock firmware configures exactly one ADC1 attenuation
// (keydec.txt:28-29): adc1_config_width(3) = 12-bit, then
// adc1_config_channel_atten(0, 3) = ADC1 channel 0 at 11 dB. batt.c is the
// wrapper pair and accepts only those codes (batt.c:10 `== 3`, batt.c:37 `< 4`).
// On the ESP32-S3 ADC1_CHANNEL_0 is GPIO1 (soc/adc_channel.h).
// ADC1 is used deliberately: ADC2 is owned by the WiFi driver.
// Calibration caveat: the divider ratio is NOT recovered — see battery.cpp.
#define BATT_ADC_PIN 1
#define BATT_ADC_CHANNEL ADC1_CHANNEL_0

// ---- Bluetooth LE ----
// No pins: the radio is internal. The stock firmware provisioned over BLE
// using the Nordic UART Service; the UUIDs are recovered evidence and are
// reused by src/ble.cpp so generic BLE-UART apps work.
// (BLE needs no board profile entry — listed here so the hardware inventory is
// complete in one place.)

// ---- Buttons: 3 tactile keys, active-low (user-probed 2026-10-09) ----
// Stock registers 3 debounced inputs on pins 4/5/6; the physical mapping
// exists only in PCB traces and was identified with a live GPIO probe:
//   POWER = GPIO4, PLUS = GPIO5, MINUS = GPIO6.
// (GPIO6 flickered once at probe boot with no press — treat a lone
//  boot-time edge as suspect; the press mapping itself is user-confirmed.)
#define BTN_POWER 4
#define BTN_PLUS 5
#define BTN_MINUS 6
#define BTN_DEBOUNCE_MS 50
#define BTN_REPEAT_START_MS 400
#define BTN_REPEAT_MS 150

// ---- Unrecovered / reserved on the stock board ----
// Stock also drove misc outputs (GPIO8/9, pulse GPIO27, and an audio "pulse
// trio" 39/40/48). Their exact purpose is not established, so the open
// firmware leaves them alone rather than guessing at a speaker enable.

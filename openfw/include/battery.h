// Battery monitor for the MiliTV open firmware.
//
// The board has a built-in Li-ion cell; the stock firmware read it on
// ADC1 channel 0 = GPIO1 (evidence in batt.c: adc1_config_width /
// adc1_config_channel_atten). GPIO1 is an ADC1 channel, so it stays usable
// while WiFi runs (ADC2 is claimed by the WiFi driver on ESP32-S3).
//
// IMPORTANT — calibration is per-board. The cell almost certainly sits behind
// a resistor divider, and we do not know its ratio or which ADC attenuation the
// stock firmware used. So:
//   * BATT_DIVIDER (default 2.0) and the empty/full voltages are the knobs.
//   * batteryRawMv() reports what the ADC literally sees (the pin voltage),
//     so you can calibrate: read it against a multimeter and set the ratio.
//   * The percentage is deliberately coarse; treat it as a gauge, not a meter.
//
// Sampling is slow (a few hundred ms) and bursty to let the input settle, so it
// runs on a timer inside batteryPump() and never blocks the loop.
#pragma once
#include <Arduino.h>

// Bring up the ADC. Returns false if disabled at build time.
bool batteryBegin();

// Sample on a slow schedule; call from loop(). Cheap when not due.
void batteryPump();

// Voltage at the battery terminals (after applying BATT_DIVIDER), in mV.
// 0 before the first sample.
uint16_t batteryMv();

// Coarse state of charge 0..100, or -1 when unknown.
int batteryPercent();

// True when the reading looks like an external supply (USB) rather than the
// cell: voltage above the full-charge knee or rising under load.
bool batteryCharging();

// True when the pack is low enough to warn on screen.
bool batteryLow();

// One-line status for the footer / BLE, e.g. "BATT 3.92V 76%".
void batteryStatus(char *out, size_t n);

// Diagnostic: sample every ADC1 channel and print the pin voltage of each.
// Exists because the battery channel is inferred from RE, not named in any
// string — if the reportable pin reads ~0 V, this finds which channel actually
// has a divider on it. Writes one line per channel to serial.
void batteryScan();

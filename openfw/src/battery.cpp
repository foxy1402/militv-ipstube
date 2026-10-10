// See battery.h for the design notes.
#include "battery.h"
#include "pins.h"

#if MILITV_BATTERY

#include <driver/adc.h>
#include <esp_adc_cal.h>

// Pin voltage -> battery voltage. The bare pin is expected to sit behind a
// divider; the stock ratio is not documented, so this is the calibration knob
// (see header). 2.0 assumes a 1:1 divider, which is the common case for
// reading a single Li-ion cell on an ESP32 pin.
#ifndef BATT_DIVIDER
#define BATT_DIVIDER 2.0f
#endif
// Reportable cell window. Li-ion: ~3.0 V empty, 4.2 V full.
#define BATT_EMPTY_MV 3300
#define BATT_FULL_MV 4150
#define BATT_LOW_MV 3500
#define BATT_PERIOD_MS 30000UL  // slow: the gauge does not need to be live

static bool sUp = false;
static bool sValid = false;  // a plausible reading has been seen
static uint16_t sMv = 0;
static int sPct = -1;
static bool sCharging = false;
static uint32_t sLast = 0;
static esp_adc_cal_characteristics_t sChars;

bool batteryBegin() {
  // Read the pin with no pull-up/down of our own: a battery divider is a
  // passive source and a leftover pull would fight it.
  pinMode(BATT_ADC_PIN, INPUT);
  // 12 dB attenuation covers ~0-3.1 V at the pin, which with a 2:1 divider
  // spans a full Li-ion cell with headroom.
  if (adc1_config_width(ADC_WIDTH_BIT_12) != ESP_OK) return false;
  if (adc1_config_channel_atten(BATT_ADC_CHANNEL, ADC_ATTEN_DB_12) != ESP_OK) return false;
  esp_adc_cal_value_t t =
      esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &sChars);
  sUp = true;
  Serial.printf("[batt] ADC1 ch%d up (cal=%s)\n", BATT_ADC_CHANNEL,
                t == ESP_ADC_CAL_VAL_EFUSE_VREF ? "efuse-vref"
                : t == ESP_ADC_CAL_VAL_EFUSE_TP ? "efuse-tp"
                                                 : "default");
  return true;
}

void batteryPump() {
  if (!sUp) return;
  uint32_t now = millis();
  if (sLast && now - sLast < BATT_PERIOD_MS) return;
  sLast = now;

  // Median of a short burst: rejects the odd noisy conversion cheaply.
  uint32_t acc = 0;
  const int N = 9;
  for (int i = 0; i < N; i++) {
    acc += adc1_get_raw(BATT_ADC_CHANNEL);
  }
  uint32_t raw = acc / N;
  uint32_t pinMv = esp_adc_cal_raw_to_voltage(raw, &sChars);
  sMv = (uint16_t)(pinMv * BATT_DIVIDER);

  // A pin sitting at (or within noise of) ground is NOT a flat battery — it
  // means nothing is connected to that ADC channel, or the divider is on a
  // different pin. Reporting 0% and beeping about it would be wrong; report
  // "unknown" and stop the low-battery logic from firing.
  if (pinMv < 100) {
    if (sValid) Serial.println("[batt] reading dropped to ~0 — treating as unknown");
    sValid = false;
    sMv = 0;
    sPct = -1;
    sCharging = false;
    return;
  }
  sValid = true;

  // Charging heuristic: an external supply pushes the terminal voltage above
  // the resting full-charge knee. Coarse but honest (no charge-status pin was
  // recovered from the stock firmware).
  sCharging = sMv >= BATT_FULL_MV;

  if (sMv <= BATT_EMPTY_MV) {
    sPct = 0;
  } else if (sMv >= BATT_FULL_MV) {
    sPct = 100;
  } else {
    sPct = (int)((uint32_t)(sMv - BATT_EMPTY_MV) * 100 / (BATT_FULL_MV - BATT_EMPTY_MV));
  }
  Serial.printf("[batt] %umV pin=%umV %d%%%s\n", sMv, (unsigned)pinMv, sPct,
                sCharging ? " (charging?)" : "");
}

uint16_t batteryMv() { return sValid ? sMv : 0; }
int batteryPercent() { return sValid ? sPct : -1; }
bool batteryCharging() { return sValid && sCharging; }
bool batteryLow() { return sValid && sPct >= 0 && sMv < BATT_LOW_MV; }

void batteryStatus(char *out, size_t n) {
  if (!sUp) {
    snprintf(out, n, "BATT off");
    return;
  }
  if (!sValid) {
    // Distinguish "no sensor on this channel" from "not sampled yet" so the
    // status line is honest on the bench.
    snprintf(out, n, "BATT n/a");
    return;
  }
  snprintf(out, n, "BATT %.2fV %d%%%s", sMv / 1000.0f, sPct, sCharging ? " CHG" : "");
}

void batteryScan() {
  if (!sUp) {
    Serial.println("[batt] scan unavailable (ADC not up)");
    return;
  }
  Serial.println("[batt] ADC1 scan (pin mV per channel; ch0 = GPIO1 per RE):");
  for (int ch = 0; ch <= 9; ch++) {
    adc1_channel_t c = (adc1_channel_t)ch;
    if (adc1_config_channel_atten(c, ADC_ATTEN_DB_12) != ESP_OK) continue;
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += adc1_get_raw(c);
    uint32_t mv = esp_adc_cal_raw_to_voltage(acc / 8, &sChars);
    Serial.printf("[batt]   ch%d: %u mV%s\n", ch, (unsigned)mv, ch == 0 ? "  <- configured" : "");
  }
  // Restore the configured channel after probing the others.
  adc1_config_channel_atten(BATT_ADC_CHANNEL, ADC_ATTEN_DB_12);
}

#else  // MILITV_BATTERY == 0

bool batteryBegin() { return false; }
void batteryPump() {}
uint16_t batteryMv() { return 0; }
int batteryPercent() { return -1; }
bool batteryCharging() { return false; }
bool batteryLow() { return false; }
void batteryStatus(char *out, size_t n) { snprintf(out, n, "BATT off"); }
void batteryScan() { Serial.println("[batt] scan unavailable (battery support compiled out)"); }

#endif

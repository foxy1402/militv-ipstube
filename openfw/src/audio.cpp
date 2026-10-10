// See audio.h for the design notes.
#include "audio.h"
#include "pins.h"

#if MILITV_AUDIO

#include <driver/i2s.h>

#define AUD_SAMPLE_RATE 16000
#define AUD_DMA_BUF 256  // frames per DMA buffer (16 ms @16 kHz)
#define AUD_DMA_N 4      // buffers queued

// One DMA-sized staging block. 16-bit mono samples; the amp is mono.
static int16_t sBuf[AUD_DMA_BUF];

static bool sUp = false;
static bool sEn = false;      // amp enabled (and a tone is active)
static uint32_t sPhase = 0;   // fixed-point phase accumulator
static uint32_t sStep = 0;    // phase increment per sample
static uint32_t sEndMs = 0;   // 0 = play until replaced
static uint16_t sFreq = 0;
static uint8_t sAmp = 0;      // 0..255 amplitude envelope

// The amp is enabled while a tone plays and released when it stops, so a
// silent board does not hiss. The stock firmware raises GPIO40, waits 100 ms,
// then GPIO39; both rest LOW. We mirror that order: the amp wants its supplies
// settled before the second enable, hence the small settle delay.
static void ampEnable(bool on) {
  if (!sUp) return;
#ifdef AUDIO_AMP_EN1
  pinMode(AUDIO_AMP_EN1, OUTPUT);
  pinMode(AUDIO_AMP_EN2, OUTPUT);
  if (on) {
    digitalWrite(AUDIO_AMP_EN1, AUDIO_AMP_EN_ACTIVE);
    delay(20);  // shortened from stock's 100 ms: this runs on the loop task
    digitalWrite(AUDIO_AMP_EN2, AUDIO_AMP_EN_ACTIVE);
  } else {
    digitalWrite(AUDIO_AMP_EN2, !AUDIO_AMP_EN_ACTIVE);
    digitalWrite(AUDIO_AMP_EN1, !AUDIO_AMP_EN_ACTIVE);
  }
#endif
  sEn = on;
}

bool audioBegin() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUD_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;  // mono amp
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = AUD_DMA_N;
  cfg.dma_buf_len = AUD_DMA_BUF;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;  // send silence instead of repeating the last buffer

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[audio] i2s_driver_install failed");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = AUD_BCK;
  pins.ws_io_num = AUD_WS;
  pins.data_out_num = AUD_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("[audio] i2s_set_pin failed");
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  sUp = true;
  ampEnable(false);
  Serial.printf("[audio] I2S up: BCK=%d WS=%d DOUT=%d @%dHz\n", AUD_BCK, AUD_WS, AUD_DOUT,
                AUD_SAMPLE_RATE);
  return true;
}

void audioTone(uint16_t freqHz, uint16_t ms) {
  if (!sUp) return;
  sFreq = freqHz;
  // phase step = freq / sampleRate * 2^32
  sStep = (uint32_t)(((uint64_t)freqHz << 32) / AUD_SAMPLE_RATE);
  sPhase = 0;
  sAmp = 90;  // ~35% of full scale: audible, not startling
  sEndMs = ms ? (millis() + ms) : 0;
  ampEnable(true);
}

void audioAlert() {
  // Two rising beeps: C6 then E6.
  audioTone(1046, 110);
  // The second beep is queued by the pump when the first expires (see below).
}

void audioClick() {
  if (!sUp) return;
  // A very short high tick reads as a click without a full tone.
  audioTone(2400, 12);
  sAmp = 40;
}

void audioStop() {
  sAmp = 0;
  sEndMs = 0;
  sFreq = 0;
  sStep = 0;
  ampEnable(false);
}

bool audioBusy() { return sEn && (sAmp > 0); }

void audioPump() {
  if (!sUp || !sEn) return;  // idle: one comparison per loop

  // Expire the tone. The alert's second beep is chained here so the caller
  // only ever has to say "alert".
  if (sEndMs && (int32_t)(millis() - sEndMs) >= 0) {
    static uint8_t alertStage = 0;
    if (alertStage == 0 && sFreq == 1046) {
      alertStage = 1;
      audioTone(1318, 110);  // E6
      return;
    }
    alertStage = 0;
    audioStop();
    return;
  }

  // Fill one DMA block. Square wave with a short attack/release ramp so the
  // speaker does not pop at the start and end of a tone.
  uint32_t remainMs = sEndMs ? (sEndMs - millis()) : 1000;
  for (int i = 0; i < AUD_DMA_BUF; i++) {
    sPhase += sStep;
    int16_t v = (sPhase >> 31) ? (int16_t)(sAmp * 128) : (int16_t)(-(sAmp * 128));
    // Fade out over the final 20 ms of a timed tone.
    if (sEndMs && remainMs <= 20) {
      uint32_t left = (sEndMs > millis()) ? (sEndMs - millis()) : 0;
      v = (int16_t)((int32_t)v * (int32_t)left / 20);
    }
    sBuf[i] = v;
  }
  size_t written = 0;
  i2s_write(I2S_NUM_0, sBuf, sizeof(sBuf), &written, 0);
}

#else  // MILITV_AUDIO == 0

// Stubs so callers do not need #ifdefs of their own.
bool audioBegin() { return false; }
void audioTone(uint16_t, uint16_t) {}
void audioAlert() {}
void audioClick() {}
void audioStop() {}
void audioPump() {}
bool audioBusy() { return false; }

#endif

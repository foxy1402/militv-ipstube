// Clean-room ST7789 driver for the MiliTV panel.
//
// Rendering model: a 320x240 RGB565 framebuffer lives in PSRAM (150 KB) and
// every primitive draws into it. flush() then pushes ONLY the dirty rectangle
// to the panel in one CASET/RASET/RAMWR window. Two consequences:
//   * erase-then-write partial redraws (the pattern every app uses) reach the
//     panel as one clean update — no black flash between erase and text, which
//     is what made refreshes read as "blinking".
//   * a glyph becomes a few RAM writes instead of one SPI window command PER
//     FONT PIXEL, and a chart line becomes RAM writes instead of one window
//     command per point. That is where the "painted on pixels" feel came from.
// Rows are pushed through a byte-swapped line buffer with one writeBytes() per
// row instead of one transfer16() per pixel.
// If the PSRAM allocation fails, every primitive falls back to the original
// direct-to-panel path, so the driver still works on a board without PSRAM.
//
// RE notes it reimplements (hardware facts only, no stock code copied):
//  - SPIClass::begin(obj, 17, -1, 14, -1)  => manual SPI on SCLK=17/MOSI=14
//  - writecommand wrapper 0x4203fc74: DC low + 1 command byte over SPI3
//    (base 0x60025000, FIFO 0x60025098, CMD 0x60025000, busy 0x60025010)
//  - writedata wrapper 0x4203fd14: DC high + 1 data byte
//  - rotation helper 0x4203fdbc: MADCTL(0x36) + per-rotation value
//    (rot3 => MADCTL 0xA8; stock boots the panel in rotation 3)
//  - init sequence 0x420433e4: SWRESET/SLPOUT/COLMOD=0x55/MADCTL/INVON/...,
//    CASET 0..239, RASET 0..319 (see ST7789_Init for the full table)
//  - window-fill helper 0x4203d6d8: CASET(0x2A)/RASET(0x2B)/RAMWR(0x2C)
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include "pins.h"

class MiniTFT {
 public:
  void begin();
  void setRotation(uint8_t r);
  void fill(uint16_t color);
  void rect(int x, int y, int w, int h, uint16_t color);
  void text(int x, int y, const char *s, uint16_t color, uint8_t scale = 2);
  void sleep();
  void wake();
  void pixel(int x, int y, uint16_t color);
  void line(int x0, int y0, int x1, int y1, uint16_t color);
  void flush();     // push the dirty rect to the panel (no-op when clean)
  void flushAll();  // force a full-screen push
  bool hasFb() const { return fb != nullptr; }

 private:
  void cmd(uint8_t c);
  void dat(uint8_t d);
  void window(int x0, int y0, int x1, int y1);
  void push(uint16_t color, uint32_t n);
  void pushRect(int x0, int y0, int x1, int y1);
  void glyph(int x, int y, char ch, uint16_t color, uint8_t scale);
  void markDirty(int x0, int y0, int x1, int y1);

  uint16_t *fb = nullptr;  // PSRAM framebuffer; nullptr => direct-to-panel
  uint16_t *lb = nullptr;  // byte-swapped row staging buffer (TFT_W entries)
  // Dirty rectangles, inclusive. A LIST, not a bounding box: a tick touches
  // the price field (top) and the live dot (right-middle), and one bounding
  // box over both would push ~80 KB every tick. Up to 8 disjoint rects stay
  // separate; a ninth collapses everything into one box (which is exactly
  // what a full-screen repaint wants anyway).
  static const int DIRTY_MAX = 8;
  struct DRect {
    int x0, y0, x1, y1;
  };
  DRect dirty[DIRTY_MAX];
  int ndirty = 0;
};

inline void MiniTFT::cmd(uint8_t c) {
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  SPI.transfer(c);
  digitalWrite(TFT_CS, HIGH);
}

inline void MiniTFT::dat(uint8_t d) {
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  SPI.transfer(d);
  digitalWrite(TFT_CS, HIGH);
}

inline void MiniTFT::begin() {
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TFT_DC, HIGH);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(5);
  digitalWrite(TFT_RST, LOW);
  delay(20);
  digitalWrite(TFT_RST, HIGH);
  delay(150);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, -1);
  SPI.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));

  cmd(0x01);
  delay(150);  // SWRESET
  cmd(0x11);
  delay(120);  // SLPOUT
  cmd(0x13);   // NORON
  cmd(0x3A);
  dat(0x55);  // COLMOD: RGB565
  delay(10);
  cmd(0xB6);
  dat(0x0A);
  dat(0x82);  // DISCTRL
  cmd(0xB0);
  dat(0x00);
  dat(0xE0);  // RAMCTRL
  cmd(0xB2);  // PORCTRL porch settings
  dat(0x0C);
  dat(0x0C);
  dat(0x00);
  dat(0x33);
  dat(0x33);
  cmd(0xB7);
  dat(0x35);  // GCTRL
  cmd(0xBB);
  dat(0x28);  // VCOMS
  cmd(0xC0);
  dat(0x0C);  // LCMCTRL
  cmd(0xC2);
  dat(0x01);
  dat(0xFF);  // VDVVRHEN
  cmd(0xC3);
  dat(0x10);  // VRH
  cmd(0xC4);
  dat(0x20);  // VDV
  cmd(0xC6);
  dat(0x0F);  // FRCTRL2
  cmd(0xD0);
  dat(0xA4);
  dat(0xA1);  // PWCTRL1
  // Positive gamma (E0)
  cmd(0xE0);
  dat(0xD0);
  dat(0x00);
  dat(0x02);
  dat(0x07);
  dat(0x0A);
  dat(0x28);
  dat(0x32);
  dat(0x44);
  dat(0x42);
  dat(0x06);
  dat(0x0E);
  dat(0x12);
  dat(0x14);
  dat(0x17);
  // Negative gamma (E1)
  cmd(0xE1);
  dat(0xD0);
  dat(0x00);
  dat(0x02);
  dat(0x07);
  dat(0x0A);
  dat(0x28);
  dat(0x31);
  dat(0x54);
  dat(0x47);
  dat(0x0E);
  dat(0x1C);
  dat(0x17);
  dat(0x1B);
  dat(0x1E);
  cmd(0x21);  // INVON
  delay(10);
  cmd(0x29);  // DISPON
  delay(20);
  setRotation(3);  // landscape 320x240: matches TFT_W/TFT_H and the panel
  // mounting (stock also boots rot 3; portrait left the image sideways).

  // Framebuffer. Allocated once — setup() calls begin() twice on the
  // POWER-held-at-boot path and must not leak a second 150 KB block.
  if (!fb) {
    fb = (uint16_t *)heap_caps_malloc((size_t)TFT_W * TFT_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lb = (uint16_t *)malloc((size_t)TFT_W * 2);
    if (!fb || !lb) {
      if (fb) heap_caps_free(fb);
      if (lb) free(lb);
      fb = nullptr;
      lb = nullptr;
    }
    Serial.printf("[tft] framebuffer %s (psram free %u)\n", fb ? "in PSRAM" : "UNAVAILABLE -> direct mode",
                  (unsigned)ESP.getFreePsram());
  }
  ndirty = 0;  // clean
}

inline void MiniTFT::setRotation(uint8_t r) {
  r &= 3;
  cmd(0x36);
  // Panel glass is BGR-wired: stock boots rot3 with MADCTL 0xA8 (BGR bit
  // set) and the "red shows as blue" symptom confirms R/B are swapped
  // without it. Green is unaffected by the swap (G middle bits), which is
  // why green looked right while down-trend red rendered blue. Use stock's
  // BGR variants so 0xF800 really is red.
  static const uint8_t kMadctl[4] = {0x08, 0x68, 0xC8, 0xA8};
  dat(kMadctl[r]);
  delay(10);
}

inline void MiniTFT::sleep() {
  cmd(0x28);  // DISPOFF
  delay(20);
  cmd(0x10);  // SLPIN
  delay(60);
}

inline void MiniTFT::wake() {
  cmd(0x11);  // SLPOUT
  delay(120);
  cmd(0x29);  // DISPON
  delay(20);
  // SLPIN/DISPON do not guarantee the panel kept our GRAM, so the next flush
  // must re-send the whole framebuffer, not just the next dirty rect.
  if (fb) {
    ndirty = 1;
    dirty[0].x0 = 0;
    dirty[0].y0 = 0;
    dirty[0].x1 = TFT_W - 1;
    dirty[0].y1 = TFT_H - 1;
  }
}

inline void MiniTFT::window(int x0, int y0, int x1, int y1) {
  cmd(0x2A);
  dat((x0 >> 8) & 0xFF);
  dat(x0 & 0xFF);
  dat((x1 >> 8) & 0xFF);
  dat(x1 & 0xFF);
  cmd(0x2B);
  dat((y0 >> 8) & 0xFF);
  dat(y0 & 0xFF);
  dat((y1 >> 8) & 0xFF);
  dat(y1 & 0xFF);
  cmd(0x2C);
}

inline void MiniTFT::push(uint16_t color, uint32_t n) {
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (uint32_t i = 0; i < n; i++) SPI.transfer16(color);
  digitalWrite(TFT_CS, HIGH);
}

// One window command + one bulk transfer row per scanline. The panel wants
// big-endian RGB565 and the S3 is little-endian, so each row is byte-swapped
// into lb first.
inline void MiniTFT::pushRect(int x0, int y0, int x1, int y1) {
  int w = x1 - x0 + 1;
  window(x0, y0, x1, y1);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (int y = y0; y <= y1; y++) {
    const uint16_t *src = fb + (size_t)y * TFT_W + x0;
    for (int i = 0; i < w; i++) lb[i] = __builtin_bswap16(src[i]);
    SPI.writeBytes((const uint8_t *)lb, (size_t)w * 2);
  }
  digitalWrite(TFT_CS, HIGH);
}

inline void MiniTFT::flush() {
  if (ndirty == 0) return;
  if (fb)
    for (int i = 0; i < ndirty; i++) pushRect(dirty[i].x0, dirty[i].y0, dirty[i].x1, dirty[i].y1);
  ndirty = 0;
}

inline void MiniTFT::flushAll() {
  if (fb) pushRect(0, 0, TFT_W - 1, TFT_H - 1);
  ndirty = 0;
}

inline void MiniTFT::markDirty(int x0, int y0, int x1, int y1) {
  // Merge into any rect that overlaps (or touches) this one — the many small
  // rects a single glyph emits must not each cost a panel window.
  for (int i = 0; i < ndirty; i++) {
    DRect &d = dirty[i];
    if (x0 <= d.x1 + 1 && x1 >= d.x0 - 1 && y0 <= d.y1 + 1 && y1 >= d.y0 - 1) {
      if (x0 < d.x0) d.x0 = x0;
      if (y0 < d.y0) d.y0 = y0;
      if (x1 > d.x1) d.x1 = x1;
      if (y1 > d.y1) d.y1 = y1;
      return;
    }
  }
  if (ndirty < DIRTY_MAX) {
    dirty[ndirty].x0 = x0;
    dirty[ndirty].y0 = y0;
    dirty[ndirty].x1 = x1;
    dirty[ndirty].y1 = y1;
    ndirty++;
    return;
  }
  // List full: collapse to the union of everything.
  int ax0 = dirty[0].x0, ay0 = dirty[0].y0, ax1 = dirty[0].x1, ay1 = dirty[0].y1;
  for (int i = 1; i < DIRTY_MAX; i++) {
    if (dirty[i].x0 < ax0) ax0 = dirty[i].x0;
    if (dirty[i].y0 < ay0) ay0 = dirty[i].y0;
    if (dirty[i].x1 > ax1) ax1 = dirty[i].x1;
    if (dirty[i].y1 > ay1) ay1 = dirty[i].y1;
  }
  if (x0 < ax0) ax0 = x0;
  if (y0 < ay0) ay0 = y0;
  if (x1 > ax1) ax1 = x1;
  if (y1 > ay1) ay1 = y1;
  ndirty = 1;
  dirty[0].x0 = ax0;
  dirty[0].y0 = ay0;
  dirty[0].x1 = ax1;
  dirty[0].y1 = ay1;
}

inline void MiniTFT::fill(uint16_t color) {
  if (!fb) {
    window(0, 0, TFT_W - 1, TFT_H - 1);
    push(color, (uint32_t)TFT_W * TFT_H);
    return;
  }
  // Written only to the framebuffer, not the panel: the caller's text/chart
  // lands in the same flush, so the clear is never seen on its own (that
  // black flash is what the apps used to blink with).
  size_t n = (size_t)TFT_W * TFT_H;
  for (size_t i = 0; i < n; i++) fb[i] = color;
  // Full-screen paint subsumes any pending rects.
  ndirty = 1;
  dirty[0].x0 = 0;
  dirty[0].y0 = 0;
  dirty[0].x1 = TFT_W - 1;
  dirty[0].y1 = TFT_H - 1;
}

inline void MiniTFT::rect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > TFT_W - 1) x1 = TFT_W - 1;
  if (y1 > TFT_H - 1) y1 = TFT_H - 1;
  if (x0 > x1 || y0 > y1) return;
  if (!fb) {
    window(x0, y0, x1, y1);
    push(color, (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1));
    return;
  }
  for (int yy = y0; yy <= y1; yy++) {
    uint16_t *row = fb + (size_t)yy * TFT_W + x0;
    for (int xx = x0; xx <= x1; xx++) *row++ = color;
  }
  markDirty(x0, y0, x1, y1);
}

inline void MiniTFT::pixel(int x, int y, uint16_t color) { rect(x, y, 1, 1, color); }

// Gap-free line: one span per column (shallow) or per row (steep), so the
// rendered line has no Bresenham holes and costs one window per scanline
// instead of one per point. Vertical spans also make shallow slopes read as a
// continuous curve rather than a dotted one.
inline void MiniTFT::line(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  if (dx >= dy) {
    if (x0 > x1) {
      int t = x0; x0 = x1; x1 = t;
      t = y0; y0 = y1; y1 = t;
    }
    int y = y0;
    for (int x = x0; x <= x1; x++) {
      int ny = dx ? y0 + (int)((long)(x - x0) * (y1 - y0) / dx) : y0;
      int a = y, b = ny;
      if (a > b) { int t = a; a = b; b = t; }
      rect(x, a, 1, b - a + 1, color);
      y = ny;
    }
  } else {
    if (y0 > y1) {
      int t = y0; y0 = y1; y1 = t;
      t = x0; x0 = x1; x1 = t;
    }
    int x = x0;
    for (int y = y0; y <= y1; y++) {
      int nx = x0 + (int)((long)(y - y0) * (x1 - x0) / dy);
      int a = x, b = nx;
      if (a > b) { int t = a; a = b; b = t; }
      rect(a, y, b - a + 1, 1, color);
      x = nx;
    }
  }
}

// 5x7 classic font, MSB-top, 1px spacing via scale blocks.
static const uint8_t kFont[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // space
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // !
    {0x00, 0x07, 0x00, 0x07, 0x00},  // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // $
    {0x23, 0x13, 0x08, 0x64, 0x62},  // %
    {0x36, 0x49, 0x55, 0x22, 0x50},  // &
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // (
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // )
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // *
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // +
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ,
    {0x08, 0x08, 0x08, 0x08, 0x08},  // -
    {0x00, 0x60, 0x60, 0x00, 0x00},  // .
    {0x20, 0x10, 0x08, 0x04, 0x02},  // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
    {0x01, 0x71, 0x0B, 0x0D, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
    {0x00, 0x36, 0x36, 0x00, 0x00},  // :
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ;
    {0x08, 0x14, 0x22, 0x41, 0x00},  // <
    {0x14, 0x14, 0x14, 0x14, 0x14},  // =
    {0x00, 0x41, 0x22, 0x14, 0x08},  // >
    {0x02, 0x01, 0x51, 0x09, 0x06},  // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
    {0x7F, 0x48, 0x48, 0x48, 0x40},  // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
    {0x46, 0x49, 0x49, 0x49, 0x31},  // S
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
    {0x7F, 0x40, 0x40, 0x40, 0x7F},  // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // W
    {0x63, 0x14, 0x08, 0x14, 0x63},  // X
    {0x07, 0x08, 0x70, 0x08, 0x07},  // Y
    {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
};

// One rect per contiguous run of set pixels in each font column: a typical
// glyph is ~5 windows instead of ~16 (one per lit pixel), and in framebuffer
// mode it is pure RAM writes with a single dirty mark for the whole glyph.
inline void MiniTFT::glyph(int x, int y, char ch, uint16_t color, uint8_t scale) {
  if (ch < 32 || ch > 90) ch = '?';
  const uint8_t *g = kFont[ch - 32];
  for (int col = 0; col < 5; col++) {
    uint8_t bits = g[col];
    if (!bits) continue;
    int row = 0;
    while (row < 7) {
      if (bits & (1 << row)) {
        int len = 1;
        while (row + len < 7 && (bits & (1 << (row + len)))) len++;
        rect(x + col * scale, y + row * scale, scale, len * scale, color);
        row += len;
      } else {
        row++;
      }
    }
  }
}

inline void MiniTFT::text(int x, int y, const char *s, uint16_t color, uint8_t scale) {
  int cx = x;
  while (*s) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;  // font covers A-Z only
    if (ch == '\n') {
      y += 8 * scale;
      cx = x;
    } else {
      glyph(cx, y, ch, color, scale);
      cx += 6 * scale;
    }
    s++;
  }
}

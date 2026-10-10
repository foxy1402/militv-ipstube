// Scrolling price chart for the 320x240 ST7789.
//
// Data: Binance klines over WS. Every message updates the live (rightmost)
// point; when a candle closes (x==true) the column commits and the chart
// scrolls. History is bootstrapped from the REST klines endpoint at boot so
// the chart is full immediately instead of growing one candle at a time.
//
// WINDOW: the chart must cover the same span as the header, which shows
// *rolling 24 h* stats (price + 24 h %). The old build sampled 1 m candles,
// so 150 points only reached back 2.5 h — a spiky chart measuring a different
// window than the percentage above it. We now sample 5 m candles across a full
// day. 288 x 5 m = 24 h exactly, and the plot is CH_X1-CH_X0 = 260 px wide, so
// 288 points lands at ~1.1 points per pixel: the panel's own resolution, with
// no wasted points. One REST call covers it (Binance caps a request at 1000
// candles, so 1 m x 1440 for the same span would need two).
//
// Change CH_INTERVAL and CH_N together — they must describe the same span.
// (CH_N is the count; the interval is the candle size.)
#pragma once
#include <Arduino.h>
#include "tft.h"

#define CH_INTERVAL "5m"   // candle size; must match CH_N's span (24 h)
#define CH_N 288           // 288 x 5 m = 24 h, ~1.1 points per plot pixel
// Stringify CH_N so the REST URL can be built from the same constant the plot
// uses (two-stage macro: #x alone would expand to the literal "CH_N").
#define CH_STR_(x) #x
#define CH_STR(x) CH_STR_(x)
#define CH_X0 40           // plot left: bezel covers x<40, labels live right
#define CH_X1 300          // plot right: 20 px margin for the glued frame
#define CH_Y0 32           // plot area: header occupies y 0..31, no gap
#define CH_Y1 215          // footer occupies y 216..239, no gap
#define CH_BG 0x0000
#define CH_LINE_UP 0x07E0    // green: session up vs first close
#define CH_LINE_DN 0xF800    // red: session down vs first close
#define CH_LIVE_UP 0x07E0    // live dot follows the same trend
#define CH_LIVE_DN 0xF800
#define CH_GRID 0x2104       // dark grid
#define CH_TXT_UP 0x07E0     // price / pct text: green when up...
#define CH_TXT_DN 0xF800     // ...red when down (never blue: blue = links)

struct Chart {
  float v[CH_N];
  int n = 0;          // valid points (<= CH_N)
  float lo = 0, hi = 0;
  float live = 0;     // most recent tick price
  bool liveUp = true;   // tick-vs-previous-tick (kept for logs)
  bool haveLive = false;
  bool dirtyScale = true;
  float open24 = 0;   // 24 h rolling open (Binance ticker/WS, not the chart)
  bool have24 = false;
  // Where the live dot currently sits on the panel (-1 = none). Chart-owned,
  // not a function static: a full redraw / rescale / app switch / wake must
  // invalidate it, or the next tick erases a black hole at stale coordinates.
  int dotX = -1, dotY = -1;
};

extern Chart chart;
extern MiniTFT *chartTft;

// 24 h trend: gain/loss of the live price vs the rolling 24 h open (Binance
// 24hr ticker REST bootstrap + miniTicker WS feed). One trend drives line +
// price + pct so the chart and the header can never disagree. Falls back to
// the chart's own oldest close until the 24 h feed answers — and now that the
// chart spans 24 h, that fallback is a *correct* baseline rather than the
// 2.5 h one it used to be.
inline bool chartTrendUp() {
  if (!chart.haveLive) return true;
  if (chart.have24 && chart.open24 > 0) return chart.live >= chart.open24;
  if (chart.n == 0) return true;
  return chart.live >= chart.v[0];
}

// 24 h % gain/loss of live vs rolling open. 0 when no data.
inline float chartTrendPct() {
  if (!chart.haveLive) return 0;
  if (chart.have24 && chart.open24 > 0)
    return (chart.live - chart.open24) / chart.open24 * 100.0f;
  if (chart.n == 0 || chart.v[0] == 0) return 0;
  return (chart.live - chart.v[0]) / chart.v[0] * 100.0f;
}


// Bind the display (called once from setup).
inline void chartBind(MiniTFT *tft) { chartTft = tft; }

int chartY(float price);
void chartRescale();
void chartFull();              // erase + axes + all segments
void chartPush(float close);   // commit one closed candle
void chartTick(float price);   // update live point only
void chartPriceText(char *out, size_t n, float price);
void chartPctText(char *out, size_t n, float pct);  // "+1.23%" / "-0.45%"

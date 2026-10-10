// See chart.h for the design notes.
#include "chart.h"

Chart chart;
MiniTFT *chartTft = nullptr;

int chartY(float price) {
  if (chart.hi <= chart.lo) return (CH_Y0 + CH_Y1) / 2;
  float f = (price - chart.lo) / (chart.hi - chart.lo);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  return CH_Y1 - (int)(f * (CH_Y1 - CH_Y0));
}

static int chartX(int idx) {
  // idx 0 = oldest. Right-align: newest point sits at CH_X1.
  int span = CH_X1 - CH_X0;
  if (chart.n <= 1) return CH_X1;
  return CH_X1 - (chart.n - 1 - idx) * span / (CH_N - 1);
}

void chartRescale() {
  if (chart.n == 0) return;
  float lo = chart.v[0], hi = chart.v[0];
  for (int i = 1; i < chart.n; i++) {
    if (chart.v[i] < lo) lo = chart.v[i];
    if (chart.v[i] > hi) hi = chart.v[i];
  }
  if (chart.haveLive) {
    if (chart.live < lo) lo = chart.live;
    if (chart.live > hi) hi = chart.live;
  }
  // Guard a degenerate range so the scale never collapses. This used to be an
  // absolute "$1" floor, which is invisible next to BTC's 24 h range (~$1700)
  // but is 100x the ENTIRE range of a pair like DOGE (24 h range ~$0.002):
  // the line flattened to a straight squiggle. Scale the floor to the price
  // instead, and keep a tiny absolute minimum so a zero/negative price cannot
  // produce a divide-by-zero.
  float mid = (hi + lo) * 0.5f;
  float minWin = mid * 0.002f;  // 0.2% of price
  if (minWin < 1e-7f) minWin = 1e-7f;
  if (hi - lo < minWin) {
    lo = mid - minWin * 0.5f;
    hi = mid + minWin * 0.5f;
  }
  // 4% headroom so the line never touches the frame.
  float pad = (hi - lo) * 0.04f;
  chart.lo = lo - pad;
  chart.hi = hi + pad;
  chart.dirtyScale = true;
}

void chartFull() {
  if (!chartTft) return;
  chartTft->rect(CH_X0, CH_Y0, CH_X1 - CH_X0 + 1, CH_Y1 - CH_Y0 + 1, CH_BG);
  // Grid: 3 dashed lines + frame. Dashes are 2 px rects, not per-dot windows.
  for (int g = 1; g <= 3; g++) {
    int y = CH_Y0 + (CH_Y1 - CH_Y0) * g / 4;
    for (int x = CH_X0; x <= CH_X1; x += 4) chartTft->rect(x, y, 2, 1, CH_GRID);
  }
  chartTft->rect(CH_X0, CH_Y0, CH_X1 - CH_X0 + 1, 1, CH_GRID);
  chartTft->rect(CH_X0, CH_Y1, CH_X1 - CH_X0 + 1, 1, CH_GRID);
  chartTft->rect(CH_X0, CH_Y0, 1, CH_Y1 - CH_Y0 + 1, CH_GRID);
  // Segments in the 24 h trend color. Hoisted: the trend is one value for the
  // whole redraw, not a per-segment question.
  uint16_t lc = chartTrendUp() ? CH_LINE_UP : CH_LINE_DN;
  for (int i = 1; i < chart.n; i++)
    chartTft->line(chartX(i - 1), chartY(chart.v[i - 1]), chartX(i), chartY(chart.v[i]), lc);
  // Min/max labels: inside the plot's right edge (x CH_X1-46), NOT the
  // left — the bezel covers x<40 and the glued frame covers x>~300, so
  // edge-to-edge labels truncate to "$83,". 46 px fits "$83,122" (42 px).
  char buf[16];
  chartPriceText(buf, sizeof(buf), chart.hi);
  chartTft->rect(CH_X1 - 46, CH_Y0 + 2, 46, 10, CH_BG);
  chartTft->text(CH_X1 - 46, CH_Y0 + 2, buf, 0x8410, 1);
  chartPriceText(buf, sizeof(buf), chart.lo);
  chartTft->rect(CH_X1 - 46, CH_Y1 - 10, 46, 10, CH_BG);
  chartTft->text(CH_X1 - 46, CH_Y1 - 10, buf, 0x8410, 1);
  // Live dot in the 24 h trend color.
  if (chart.haveLive && chart.n > 0) {
    int x = chartX(chart.n - 1), y = chartY(chart.live);
    chartTft->rect(x - 1, y - 1, 3, 3, chartTrendUp() ? CH_LIVE_UP : CH_LIVE_DN);
    chart.dotX = x;
    chart.dotY = y;
  } else {
    chart.dotX = chart.dotY = -1;
  }
  chart.dirtyScale = false;
}

void chartPush(float close) {
  if (chart.n < CH_N) {
    chart.v[chart.n++] = close;
  } else {
    memmove(chart.v, chart.v + 1, (CH_N - 1) * sizeof(float));
    chart.v[CH_N - 1] = close;
  }
  chart.live = close;
  chart.haveLive = true;
  // Rescale check before drawing: if the new close breaks the range,
  // full redraw; else incremental append.
  float lo = chart.lo, hi = chart.hi;
  bool fits = chart.n > 1 && close >= lo && close <= hi;
  if (!fits || chart.dirtyScale) {
    chartRescale();
    chartFull();
    return;
  }
  if (!chartTft || chart.n < 2) return;
  // Buffer full: every point shifted one slot left, so there is no small
  // column to erase — a whole-plot redraw is the correct move. It is one clean
  // framebuffer push with no erase flash, and with CH_INTERVAL = 5m it happens
  // once per 5 minutes, not every minute.
  if (chart.n == CH_N) {
    chartFull();
    return;
  }
  int x0 = chartX(chart.n - 2), y0 = chartY(chart.v[chart.n - 2]);
  int x1 = chartX(chart.n - 1), y1 = chartY(chart.v[chart.n - 1]);
  chartTft->line(x0, y0, x1, y1, chartTrendUp() ? CH_LINE_UP : CH_LINE_DN);
  // The live dot sits at the newest point; it must move with the segment or
  // the next tick will erase a stale location.
  chart.dotX = x1;
  chart.dotY = y1;
}

void chartTick(float price) {
  chart.liveUp = !chart.haveLive || price >= chart.live;
  chart.live = price;
  chart.haveLive = true;
  if (!chartTft || chart.n == 0) return;
  if (price < chart.lo || price > chart.hi) {
    chartRescale();
    chartFull();  // resets dotX/dotY itself
    return;
  }
  // Move the live dot: erase the previous one, draw the new. Positions live
  // on the Chart (not in function statics) because a full redraw, an app
  // switch or a sleep/wake would otherwise leave the stale coordinates behind
  // and the erase would punch a black hole in the committed line.
  uint16_t liveC = chartTrendUp() ? CH_LIVE_UP : CH_LIVE_DN;
  uint16_t lineC = chartTrendUp() ? CH_LINE_UP : CH_LINE_DN;
  int x = chartX(chart.n - 1);
  int y = chartY(price);
  if (chart.dotX >= 0 && (chart.dotX != x || chart.dotY != y))
    chartTft->rect(chart.dotX - 1, chart.dotY - 1, 3, 3, CH_BG);
  // Redraw the committed segment the erase may have bitten into, then the dot.
  if (chart.n >= 2)
    chartTft->line(chartX(chart.n - 2), chartY(chart.v[chart.n - 2]), x, y, lineC);
  chartTft->rect(x - 1, y - 1, 3, 3, liveC);
  chart.dotX = x;
  chart.dotY = y;
}

void chartPctText(char *out, size_t n, float pct) {
  // "+1.23%" / "-0.45%": sign + 2 decimals + percent glyph.
  char sign = pct < 0 ? '-' : '+';
  float a = pct < 0 ? -pct : pct;
  snprintf(out, n, "%c%.2f%%", sign, a);
}

void chartPriceText(char *out, size_t n, float price) {
  // "$82,656" for BTC, but a fixed whole-dollar format prints "$0" for every
  // sub-dollar pair (DOGE $0.086, ADA $0.25) — pairs this firmware advertises
  // support for. Pick decimals from the magnitude, then group the integer part.
  //
  // The label slot is 7 characters (46 px at the font's 6 px cell), so the
  // decimal count is capped by what actually fits: "$" + digits + "." + decs
  // <= 7. Without that cap, DOGE's "$0.086000" would be truncated mid-number.
  const int MAXCHARS = 7;
  float a = price < 0 ? -price : price;
  int preferred;
  if (a >= 1000.0f) preferred = 0;       // $82,656
  else if (a >= 1.0f) preferred = 2;     // $1.40
  else if (a >= 0.01f) preferred = 4;    // $0.0860
  else preferred = 6;                    // $0.000123
  // Digit count of the integer part (a >= 1 always has at least one digit).
  int intDigits = 1;
  for (float t = a; t >= 10.0f; t /= 10.0f) intDigits++;
  // "$" + intDigits + "." + dec <= MAXCHARS, but commas add one per 3 digits.
  int commas = (intDigits - 1) / 3;
  int budget = MAXCHARS - 1 - intDigits - commas - 1;
  int dec = preferred < budget ? preferred : (budget > 0 ? budget : 0);

  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%.*f", dec, a);
  char *dot = strchr(tmp, '.');
  int intLen = dot ? (int)(dot - tmp) : (int)strlen(tmp);
  char *p = out;
  size_t left = n;
  auto put = [&](char c) {
    if (left > 1) {
      *p++ = c;
      left--;
    }
  };
  put('$');
  if (price < 0) put('-');
  for (int i = 0; i < intLen; i++) {
    put(tmp[i]);
    int rest = intLen - 1 - i;
    if (rest > 0 && rest % 3 == 0) put(',');
  }
  if (dot) {
    for (const char *q = dot; *q; q++) put(*q);  // includes the '.'
  }
  *p = 0;
}

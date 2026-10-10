// See stockapp.h for the design notes.
#include "stockapp.h"
#include "tft.h"
#include "cfg.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static MiniTFT *st = nullptr;
void stockBind(MiniTFT *tft) { st = tft; }

#define ST_N 60
static float hist[ST_N];
static int histN = 0;
static float quote = 0, prevClose = 0;
static bool haveQuote = false, haveHist = false;
static bool marketOpen = false;
static bool fetchFailed = false;
static uint32_t lastQ = 0;
static char symUp[16] = "";
static char footBuf[48] = "";
// Partial-redraw memory: what is currently on screen, so steady values cost
// zero pixels (no blink). Same discipline as the clock app.
static char shownSym[16] = "";
static char shownPrice[20] = "";
static char shownPct[12] = "";
static char shownBanner[16] = "";
static char shownFoot[48] = "";
static float shownHist[ST_N];
static int shownHistN = -1;  // -1 = sparkline never drawn
static bool stockLaidOut = false;

// cfg.stock ("spy.us", "aapl.us", or plain "SPY") -> Yahoo symbol ("SPY").
static void yahooSym(char *out, size_t n) {
  size_t i = 0, j = 0;
  for (; cfg.stock[i] && cfg.stock[i] != '.' && j + 1 < n; i++, j++) {
    char c = cfg.stock[i];
    out[j] = (c >= 'a' && c <= 'z') ? c - 32 : c;
  }
  out[j] = 0;
  if (j == 0) {
    strncpy(out, "SPY", n);
  }
}

static void upperSym() {
  yahooSym(symUp, sizeof(symUp));
}

// Single Yahoo chart call -> quote + history + market-hours flag.
// Probed 2026-10-09: HTTP 200, ~3.6 KB, no key, fields regularMarketPrice /
// chartPreviousClose / currentTradingPeriod.regular.{start,end} /
// regularMarketTime / indicators.quote[0].close[] all present.
static void fetchAll() {
  if (WiFi.status() != WL_CONNECTED) return;
  char ysym[16], url[128];
  yahooSym(ysym, sizeof(ysym));
  snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1mo",
           ysym);
  fetchFailed = false;
  WiFiClientSecure cli;
  cli.setInsecure();  // personal use: skip CA bundle
  cli.setTimeout(4000);  // fail fast: a dead fetch must not eat the tap window
  HTTPClient http;
  http.setTimeout(4000);
  http.setUserAgent("Mozilla/5.0");  // Yahoo 403s the default ESP32 UA
  if (!http.begin(cli, url)) {
    fetchFailed = true;
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[stock] %s HTTP %d\n", ysym, code);
    http.end();
    fetchFailed = true;
    return;
  }
  static JsonDocument filter;
  static bool filterInit = false;
  if (!filterInit) {
    filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
    filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
    filter["chart"]["result"][0]["meta"]["regularMarketTime"] = true;
    filter["chart"]["result"][0]["meta"]["currentTradingPeriod"]["regular"]["start"] = true;
    filter["chart"]["result"][0]["meta"]["currentTradingPeriod"]["regular"]["end"] = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;
    filterInit = true;
  }
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[stock] parse: %s\n", err.c_str());
    fetchFailed = true;
    return;
  }
  JsonVariantConst res = doc["chart"]["result"][0];
  if (res.isNull()) {
    Serial.printf("[stock] %s: no result (bad symbol?)\n", ysym);
    fetchFailed = true;
    return;
  }
  float price = res["meta"]["regularMarketPrice"] | 0.0f;
  float pcl = res["meta"]["chartPreviousClose"] | 0.0f;
  if (price <= 0) {
    fetchFailed = true;
    return;
  }
  quote = price;
  prevClose = (pcl > 0) ? pcl : price;
  haveQuote = true;
  // Market-hours from the server's own clock: regularMarketTime inside the
  // regular session => OPEN. Self-contained, no NTP dependency.
  long mt = res["meta"]["regularMarketTime"] | 0;
  long rs = res["meta"]["currentTradingPeriod"]["regular"]["start"] | 0;
  long re = res["meta"]["currentTradingPeriod"]["regular"]["end"] | 0;
  marketOpen = (rs > 0 && re > 0 && mt >= rs && mt <= re);
  int total = 0;
  JsonArrayConst closes = res["indicators"]["quote"][0]["close"].as<JsonArrayConst>();
  for (JsonVariantConst c : closes)
    if (c.as<float>() > 0) total++;  // count valid closes (null on holidays)
  int skip = total > ST_N ? total - ST_N : 0;
  int seen = 0;
  histN = 0;
  for (JsonVariantConst c : closes) {
    float v = c.as<float>();
    if (v <= 0) continue;
    if (seen++ < skip) continue;
    if (histN < ST_N) hist[histN++] = v;
  }
  haveHist = histN >= 5;
  lastQ = millis();
  upperSym();
  Serial.printf("[stock] %s quote=%.2f prev=%.2f hist=%d %s\n", ysym, quote, prevClose, histN,
                marketOpen ? "OPEN" : "CLOSED");
}

// Left-aligned field at fixed x: erase-then-write only when changed.
static void textLUpd(int x, int y, const char *s, uint16_t color, uint8_t scale, char *shown,
                     size_t shownN, int clearW) {
  if (strcmp(shown, s) == 0) return;
  st->rect(x, y, clearW, 8 * scale, 0x0000);
  st->text(x, y, s, color, scale);
  strncpy(shown, s, shownN - 1);
  shown[shownN - 1] = 0;
}

void stockDraw() {
  if (!st) return;
  if (!stockLaidOut) {
    // Full layout once per entry: black fill + static geometry. After that,
    // only textLUpd + the sparkline touch pixels.
    st->fill(0x0000);
    st->rect(0, 216, TFT_W, 24, 0x0000);
    shownSym[0] = 0;
    shownPrice[0] = 0;
    shownPct[0] = 0;
    shownBanner[0] = 0;
    shownFoot[0] = 0;
    shownHistN = -1;
    stockLaidOut = true;
  }
  upperSym();
  // Symbol (x40, scale 2, slot ~84 px) — same slot as crypto SYM.
  textLUpd(40, 8, symUp, 0xFFFF, 2, shownSym, sizeof(shownSym), 84);
  bool up = haveQuote && quote >= prevClose;
  uint16_t tc = up ? 0x07E0 : 0xF800;
  char buf[24];
  if (haveQuote) {
    snprintf(buf, sizeof(buf), "$%.2f", quote);
    textLUpd(176, 8, buf, tc, 2, shownPrice, sizeof(shownPrice), 124);
    float pct = prevClose > 0 ? (quote - prevClose) / prevClose * 100 : 0;
    snprintf(buf, sizeof(buf), "%c%.2f%%", pct < 0 ? '-' : '+', pct < 0 ? -pct : pct);
    textLUpd(88, 8, buf, tc, 2, shownPct, sizeof(shownPct), 84);
    textLUpd(40, 32, marketOpen ? "" : "MARKET CLOSED", 0xFFE0, 1, shownBanner,
             sizeof(shownBanner), 96);
  } else if (fetchFailed) {
    // Compact error that never traps the buttons: two lines + footer hint.
    if (strcmp(shownBanner, "STFAIL") != 0) {
      st->rect(40, 60, 260, 76, 0x0000);
      st->text(40, 60, "FETCH FAILED", 0xF800, 2);
      st->text(40, 90, "CHECK SYMBOL IN SETUP", 0xFFE0, 1);
      st->text(40, 104, "PORTAL, TAP + OR -", 0xFFE0, 1);
      strncpy(shownBanner, "STFAIL", sizeof(shownBanner) - 1);
      shownPrice[0] = 0;
      shownPct[0] = 0;
      shownHistN = -1;
    }
  } else {
    textLUpd(40, 100, "LOADING QUOTE...", 0xFFE0, 2, shownBanner, sizeof(shownBanner), 216);
  }
  // Sparkline, CENTERED: the Yahoo month holds ~23 closes, but the old code
  // divided by ST_N-1 (59), squeezing all 23 points into the right 40% of the
  // plot. Divide by (histN-1) instead so the series always spans the box.
  // Redraws only when the data actually changes (once per 60 s fetch).
  const int X0 = 40, X1 = 300, Y0 = 44, Y1 = 215;
  bool histChanged = (histN != shownHistN);
  if (!histChanged && haveHist)
    for (int i = 0; i < histN && !histChanged; i++)
      if (hist[i] != shownHist[i]) histChanged = true;
  if (haveHist && histN >= 2 && histChanged) {
    st->rect(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1, 0x0000);
    float lo = hist[0], hi = hist[0];
    for (int i = 1; i < histN; i++) {
      if (hist[i] < lo) lo = hist[i];
      if (hist[i] > hi) hi = hist[i];
    }
    if (hi - lo < 0.01f) {
      hi += 0.005f;
      lo -= 0.005f;
    }
    auto py = [&](float p) { return Y1 - (int)((p - lo) / (hi - lo) * (Y1 - Y0)); };
    auto px = [&](int i) { return X0 + i * (X1 - X0) / (histN - 1); };
    for (int x = X0; x <= X1; x += 4) st->rect(x, (Y0 + Y1) / 2, 2, 1, 0x2104);
    uint16_t lc = (hist[histN - 1] >= hist[0]) ? 0x07E0 : 0xF800;
    for (int i = 1; i < histN; i++) st->line(px(i - 1), py(hist[i - 1]), px(i), py(hist[i]), lc);
    for (int i = 0; i < histN; i++) shownHist[i] = hist[i];
    shownHistN = histN;
  } else if (!haveHist && shownHistN != 0) {
    st->rect(X0, Y0, X1 - X0 + 1, Y1 - Y0 + 1, 0x0000);
    shownHistN = 0;
  }
  const char *fs = stockStatus();
  if (strcmp(shownFoot, fs) != 0) {
    st->rect(40, 222, 260, 8, 0x0000);
    st->text(40, 222, fs, 0x07FF, 1);
    strncpy(shownFoot, fs, sizeof(shownFoot) - 1);
  }
}

const char *stockStatus() {
  if (haveQuote)
    snprintf(footBuf, sizeof(footBuf), "%s $%.2f %s", symUp, quote,
             marketOpen ? "OPEN" : "CLOSED");
  else if (fetchFailed)
    snprintf(footBuf, sizeof(footBuf), "%s fetch failed", symUp);
  else
    snprintf(footBuf, sizeof(footBuf), "%s loading...", symUp);
  return footBuf;
}

void stockEnter() {
  // Layout + cached paint instantly so the buttons stay alive; the fetch
  // happens on the next pump() instead of blocking the tap that got us here.
  stockLaidOut = false;  // force a fresh layout (clears the old app)
  stockDraw();
}

void stockExit() {}

void stockPump() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  // First pump after entering (or every 60 s): one request refreshes both
  // quote and history.
  if ((!haveQuote && !fetchFailed) || (now - lastQ > 60000UL)) {
    fetchAll();
    stockDraw();
  }
}

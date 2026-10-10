// MiliTV open firmware — multi-app: crypto chart + clock/weather + stock.
//
// Apps (tap -/+ to switch, hold -/+ for backlight, POWER sleep/wake):
//   0 CRYPTO  any Binance USDT ticker (cfg.ticker, e.g. ETHUSDT): klines over
//             WS + REST history bootstrap. Candle size and span come from
//             chart.h (CH_INTERVAL/CH_N = 5 m x 288 = a full 24 h, matching
//             the 24 h stats in the header).
//   1 CLOCK   NTP clock (cfg.gmtOff) + Open-Meteo weather (cfg.lat/lon/city)
//             as text (condition, temp, high/low). No key, free.
//   2 STOCK   Yahoo Finance quote + daily sparkline (cfg.stock, e.g. spy.us
//             for S&P500, aapl.us for Apple). Shows MARKET CLOSED outside
//             hours; data is the last close.
// WiFi: WiFiManager. Normal boot auto-joins NVS creds. Hold POWER at power-on
//   (>3 s) for the MILITV-SETUP portal: WiFi creds + ALL app settings
//   (ticker, timezone, weather lat/lon/city, stock) on one page. Leaving the
//   WiFi fields blank keeps the current network; app settings save to our own
//   NVS namespace and never touch WiFi creds.
// Reconnect watchdog (the stock drop-bug fix): modem sleep off,
// auto-reconnect on, DISCONNECTED/LOST_IP events, 15 s watchdog with a full
// disconnect()+begin().
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "pins.h"
#include "tft.h"
#include "chart.h"
#include "netcfg.h"
#include "cfg.h"
#include "clockapp.h"
#include "stockapp.h"
#include "audio.h"
#include "ble.h"
#include "battery.h"
#include "assets.h"

#define WS_HOST "data-stream.binance.vision"
#define WS_PORT 9443
// data-api/data-stream .vision mirrors serve market data globally without
// the geo restrictions api.binance.com sometimes returns (HTTP 451).
// The interval and limit come from chart.h so the REST bootstrap, the WS
// subscription and the plot all describe the same 24 h window.
#define HIST_FMT \
  "https://data-api.binance.vision/api/v3/klines?symbol=%s&interval=" CH_INTERVAL "&limit=" CH_STR(CH_N)
#define TICKER_FMT "https://data-api.binance.vision/api/v3/ticker/24hr?symbol=%s"

enum App : uint8_t { APP_CRYPTO = 0, APP_CLOCK = 1, APP_STOCK = 2, APP_N = 3 };

static MiniTFT tft;
static WebSocketsClient ws;
static uint8_t bl = 255;
static bool screenOn = true;
static App app = APP_CRYPTO;
static bool appEntered = false;   // current app's enter() has run
static bool wasUp = false;        // wifi-up edge tracker
static bool wsWant = false;       // STA up and crypto stream should run
static bool wsStarted = false;    // beginSSL issued (handshake in flight)
static uint32_t wsStartAt = 0;
static char loadedTicker[16] = "";  // ticker the chart buffer belongs to
// Partial-redraw memory for the crypto header (see drawPriceField).
static char shownCryptoSym[16] = "";
static char shownCryptoPct[20] = "";
static char shownCryptoPrice[20] = "";
static uint16_t shownCryptoColor = 0;
static char shownCryptoFoot[40] = "";
// Line buffer for the serial console (shared command set with BLE).
static char sCmd[BLE_LINE_MAX];
static size_t sCmdN = 0;
// BLE advertised name (fits the 29-byte advertise payload with the prefix).
static char sBleName[20] = "MILITV-DEV";

void clockBind(MiniTFT *tft);
void stockBind(MiniTFT *tft);

struct Key {
  int pin;
  bool lastPhys;
  uint32_t since;
  bool debounced;
  uint32_t pressedAt;
  uint32_t lastRep;
};

static Key kPlus{BTN_PLUS, false, 0, false, 0, 0};
static Key kMinus{BTN_MINUS, false, 0, false, 0, 0};
static Key kPower{BTN_POWER, false, 0, false, 0, 0};

static void setBacklight(uint8_t v) {
  bl = v;
  if (screenOn) ledcWrite(TFT_BL_CH, v);
}

// ---------- crypto screen (layout kept from v3) ----------
// Bezel covers x<40, glued frame covers x>~300: nothing readable outside.
//   y  0..31  header: SYM (x40) + session % (x88) + price (x176)
//   y 32..215 chart plot (CH_Y0..CH_Y1 in chart.h)
//   y 216..239 footer: wifi status.
static void drawHeader() {
  char sym[16];
  cfgBaseSym(sym, sizeof(sym));
  if (strcmp(shownCryptoSym, sym) != 0) {
    // Symbol slot (x40, scale 2): clear + rewrite only on ticker change.
    tft.rect(40, 0, 48, 32, 0x0000);
    tft.text(40, 8, sym, 0xFFFF, 2);
    strncpy(shownCryptoSym, sym, sizeof(shownCryptoSym) - 1);
    tft.flush();
  }
}

// Header field updater: erase-then-write only when the text OR color
// changes. Steady ticks (same price string, same trend) cost zero pixels —
// this is what kills the per-tick blink the old drawPrice had.
static void drawPriceField(int x, int clearW, const char *s, uint16_t color, char *shown,
                           uint16_t *shownColor) {
  if (strcmp(shown, s) == 0 && *shownColor == color) return;
  tft.rect(x, 4, clearW, 24, 0x0000);
  tft.text(x, 8, s, color, 2);
  strncpy(shown, s, 19);
  shown[19] = 0;
  *shownColor = color;
}

static void drawPrice() {
  char buf[20], pct[12];
  // Pct hugs SYM (x88, scale 2); price at x176 leaves room for 7 figures
  // before the x300 frame edge.
  if (!chart.haveLive) {
    drawPriceField(176, 124, "--", 0x8410, shownCryptoPrice, &shownCryptoColor);
    return;
  }
  // One 24 h trend drives pct + price + line + dot: green up, red down,
  // never blue (blue reads as link/neutral).
  uint16_t tc = chartTrendUp() ? CH_TXT_UP : CH_TXT_DN;
  chartPctText(pct, sizeof(pct), chartTrendPct());
  drawPriceField(88, 84, pct, tc, shownCryptoPct, &shownCryptoColor);
  chartPriceText(buf, sizeof(buf), chart.live);
  drawPriceField(176, 124, buf, tc, shownCryptoPrice, &shownCryptoColor);
}

static void drawFooter() {
  char buf[40];
  netStatus(buf, sizeof(buf));
  buf[34] = 0;
  // Footer band: rewrite only when the text changes (WiFi drop/rejoin), so
  // the 5 s refresh no longer flashes the whole band every pass.
  if (strcmp(shownCryptoFoot, buf) == 0) return;
  tft.rect(0, 216, TFT_W, 24, 0x0000);
  tft.text(40, 222, buf, net.portalArmed ? 0xFFE0 : 0x07FF, 1);
  strncpy(shownCryptoFoot, buf, sizeof(shownCryptoFoot) - 1);
}

static void drawScreen() {
  tft.fill(0x0000);
  // drawPriceField skips steady values, so reset its memory for a full paint.
  shownCryptoSym[0] = 0;
  shownCryptoPct[0] = 0;
  shownCryptoPrice[0] = 0;
  shownCryptoColor = 0x0000;
  shownCryptoFoot[0] = 0;
  // 0x0000 is a legal text color (black), but drawPriceField also compares
  // the string — empty shown strings already force the first paint.
  drawHeader();
  drawPrice();
  chartFull();
  drawFooter();
}

// ---------- crypto data (ticker-dynamic) ----------
static void lowerTicker(char *out, size_t n) {
  size_t i = 0;
  for (; cfg.ticker[i] && i + 1 < n; i++) {
    char c = cfg.ticker[i];
    out[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
  }
  out[i] = 0;
}

static void fetchHistory() {
  if (WiFi.status() != WL_CONNECTED) return;
  char url[128];
  snprintf(url, sizeof(url), HIST_FMT, cfg.ticker);
  Serial.printf("[chart] fetching %s %sx%s history...\n", cfg.ticker, CH_STR(CH_N), CH_INTERVAL);
  tft.text(40, 216, "LOADING HISTORY...", 0xFFE0, 1);
  // HTTPS on this core needs an explicit client; personal use => insecure.
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(4000);  // fail fast: a dead fetch must not eat the tap window
  HTTPClient http;
  http.setTimeout(8000);  // history body is ~20 KB; parse takes longer
  if (!http.begin(cli, url)) {
    Serial.println("[chart] history begin failed");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[chart] history HTTP %d\n", code);
    http.end();
    return;
  }
  // No filter: the body is ~20 KB of arrays; a growing v7 doc + stream
  // parse handles it (S3 heap is ~300 KB free). Stream avoids a copy.
  // A bad ticker returns a JSON object, not an array -> 0 candles.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[chart] history parse: %s\n", err.c_str());
    return;
  }
  int added = 0;
  if (doc.is<JsonArray>()) {
    for (JsonVariant k : doc.as<JsonArray>()) {
      const char *c = k[4];
      if (c) {
        chartPush(atof(c));
        added++;
      }
    }
  }
  chartRescale();
  Serial.printf("[chart] history: %d candles\n", added);
  if (added == 0) tft.text(40, 100, "NO DATA - CHECK TICKER", 0xF800, 2);
}

// 24 h rolling open bootstrap (REST, once per ticker). The ticker endpoint is
// authoritative for the rolling window (probed 2026-10-10:
// openPrice/lastPrice/priceChangePercent all present, ~550 B) and the live WS
// miniTicker feed keeps it rolling. The chart now also spans 24 h, so the two
// agree; this feed stays the source of truth because it is Binance's own
// rolling-open definition (a fixed 24 h lookback, not a candle boundary).
static void fetchTicker24() {
  if (WiFi.status() != WL_CONNECTED) return;
  char url[128];
  snprintf(url, sizeof(url), TICKER_FMT, cfg.ticker);
  WiFiClientSecure cli;
  cli.setInsecure();  // personal use: skip CA bundle
  cli.setTimeout(4000);  // fail fast: a dead fetch must not eat the tap window
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(cli, url)) return;
  int code24 = http.GET();
  if (code24 != 200) {
    Serial.printf("[chart] 24h HTTP %d\n", code24);
    http.end();
    return;
  }
  static JsonDocument filter;
  static bool filterInit = false;
  if (!filterInit) {
    filter["openPrice"] = true;
    filter["lastPrice"] = true;
    filter["priceChangePercent"] = true;
    filterInit = true;
  }
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[chart] 24h parse: %s\n", err.c_str());
    return;
  }
  const char *openS = doc["openPrice"];
  float open = openS ? atof(openS) : 0;
  if (open > 0) {
    chart.open24 = open;
    chart.have24 = true;
    const char *pctS = doc["priceChangePercent"];
    // Use the same adaptive formatter as the screen: "%.0f" would print a
    // sub-dollar pair's open as "$0" in the log.
    char ob[16];
    chartPriceText(ob, sizeof(ob), open);
    Serial.printf("[chart] 24h open=%s pct=%s%%\n", ob, pctS ? pctS : "?");
  }
}

// ---------- websocket ----------
// WS task -> loop() handoff. The event callback runs on the WebSockets task,
// and both the PSRAM framebuffer and the SPI bus belong to loop(): drawing
// from the callback would race loop()'s own drawing, tear the framebuffer and
// interleave two writers on SPI. So the callback only records the newest
// values; loop() drains them. (Pre-fix the callback drew directly, which also
// meant chartPush's memmove raced loop's reads of chart.v.)
static volatile float wsPend24 = 0;
static volatile bool wsPend24New = false;
static volatile float wsPendTick = 0;
static volatile bool wsPendTickNew = false;
static volatile float wsPendClose = 0;
static volatile bool wsPendCloseNew = false;

static void wsStart() {
  wsStarted = true;
  wsStartAt = millis();
  char lt[16], path[80];
  lowerTicker(lt, sizeof(lt));
  // Combined stream: the kline feed drives the chart + live price, miniTicker
  // carries the rolling 24 h open/close for trend + pct. One socket, both
  // feeds. Messages self-identify: kline has "k", miniTicker has "o"/"c".
  // The kline stream name must use the SAME interval as the chart's history
  // (CH_INTERVAL), or the live candle commits at the wrong cadence and the
  // chart's rightmost point would not line up with its 5 m timebase.
  snprintf(path, sizeof(path), "/stream?streams=%s@kline_%s/%s@miniTicker", lt, CH_INTERVAL, lt);
  ws.beginSSL(WS_HOST, WS_PORT, path);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(15000, 3000, 2);
  ws.onEvent([](WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
      case WStype_CONNECTED:
        wsStarted = false;
        Serial.printf("[ws] connected, subscribed %s@kline_%s\n", cfg.ticker, CH_INTERVAL);
        break;
      case WStype_DISCONNECTED:
        wsStarted = false;
        // 24 h server cut, router drop, whatever — resubscribe path is in
        // loop() (wsWant + !connected => wsStart). Just log here: the
        // callback runs on the WS task, keep it short.
        Serial.println("[ws] disconnected");
        break;
      case WStype_TEXT: {
        static JsonDocument filter;
        static bool filterInit = false;
        if (!filterInit) {
          filter["data"]["k"]["c"] = true;  // combined stream wraps in "data"
          filter["data"]["k"]["x"] = true;
          filter["data"]["o"] = true;  // miniTicker 24 h open
          filter["data"]["c"] = true;  // miniTicker close (= live price)
          filter["k"]["c"] = true;     // (single-stream shape, just in case)
          filter["k"]["x"] = true;
          filterInit = true;
        }
        JsonDocument doc;
        DeserializationError derr =
            deserializeJson(doc, payload, len, DeserializationOption::Filter(filter));
        if (derr) return;
        // Combined stream nests the payload under "data"; tolerate both.
        JsonVariantConst d = doc["data"].isNull() ? doc.as<JsonVariantConst>()
                                                  : doc["data"].as<JsonVariantConst>();
        // miniTicker: rolling 24 h open + live price.
        const char *o = d["o"];
        if (o) {
          float open = atof(o);
          if (open > 0) {
            wsPend24 = open;
            wsPend24New = true;
          }
          const char *mc = d["c"];
          if (mc) {
            wsPendTick = atof(mc);
            wsPendTickNew = true;
          }
          break;
        }
        // kline: chart points.
        const char *c = d["k"]["c"];
        if (!c) return;
        bool closed = d["k"]["x"] | false;
        if (closed) {
          wsPendClose = atof(c);
          wsPendCloseNew = true;
        } else {
          // Kline interim closes duplicate the miniTicker price; the live dot
          // is already placed by the miniTicker branch (or the fallback below
          // when only one feed is up), so just track the tick for the log.
          wsPendTick = atof(c);
          wsPendTickNew = true;
        }
        break;
      }
      default:
        break;
    }
  });
  Serial.println("[ws] stream started");
}

// Drain the WS mailbox on the loop task (the only task allowed to draw).
static void cryptoDrainWs() {
  if (wsPend24New) {
    wsPend24New = false;
    float open = wsPend24;
    if (open > 0) {
      chart.open24 = open;
      chart.have24 = true;
    }
  }
  if (wsPendCloseNew) {
    wsPendCloseNew = false;
    float close = wsPendClose;
    chartPush(close);
    drawPrice();
    char cb[16];
    chartPriceText(cb, sizeof(cb), close);
    Serial.printf("[ws] candle close %s n=%d 24h %+.2f%%\n", cb, chart.n, chartTrendPct());
  } else if (wsPendTickNew) {
    wsPendTickNew = false;
    // Live price: miniTicker carries the 24 h open and the tick, and the
    // kline interim close duplicates it — whichever arrives last wins, so the
    // dot keeps moving even if one feed stalls.
    float price = wsPendTick;
    chartTick(price);
    drawPrice();
    static uint32_t nTick = 0, lastTickLog = 0;
    nTick++;
    uint32_t tnow = millis();
    if (tnow - lastTickLog > 20000) {
      lastTickLog = tnow;
      char pb[16];
      chartPriceText(pb, sizeof(pb), price);
      Serial.printf("[ws] tick %s 24h %+.2f%% (#%lu)\n", pb, chartTrendPct(),
                    (unsigned long)nTick);
    }
  }
}

// ---------- app lifecycle ----------
// Deferred crypto bootstrap: 0 none, 1 fetch 24 h open next, 2 fetch history
// next. Set by cryptoEnter(); drained one stage per loop() pass so the tap
// that opens CRYPTO returns instantly (same non-blocking discipline as clock
// and stock — no fetch ever runs inside the tap handler).
static uint8_t cryptoFetchStage = 0;

static void cryptoEnter() {
  // Drop anything the WS task queued for a previous visit — a stale tick must
  // not paint into a freshly reset chart.
  wsPend24New = wsPendTickNew = wsPendCloseNew = false;
  // New ticker (or first run): drop the old buffer so the trend baseline
  // and scale belong to the new pair. The fetches run staged on the next
  // loop() passes — drawScreen() paints the LOADING state now.
  if (strcmp(loadedTicker, cfg.ticker) != 0) {
    chart.n = 0;
    chart.haveLive = false;
    chart.have24 = false;
    chart.open24 = 0;
    chart.lo = chart.hi = 0;
    chart.dirtyScale = true;
    chart.dotX = chart.dotY = -1;
    strncpy(loadedTicker, cfg.ticker, sizeof(loadedTicker) - 1);
    cryptoFetchStage = 1;  // 24 h first, then history (see cryptoPumpFetch)
  } else if (chart.n == 0) {
    cryptoFetchStage = chart.have24 ? 2 : 1;
  } else {
    cryptoFetchStage = 0;
  }
  drawScreen();
  wsWant = true;
}

// One bootstrap stage per call: returns true if a fetch ran (caller skips
// other app pumps that pass, so slow TLS handshakes never stack).
static bool cryptoPumpFetch() {
  if (cryptoFetchStage == 0 || WiFi.status() != WL_CONNECTED) return false;
  if (cryptoFetchStage == 1) {
    fetchTicker24();  // 24 h baseline first: trend/pct are right from tick 1
    if (chart.n == 0) {
      cryptoFetchStage = 2;
    } else {
      cryptoFetchStage = 0;
      drawScreen();  // 24 h arrived late: repaint trend/pct colors
    }
    return true;
  }
  fetchHistory();
  cryptoFetchStage = 0;
  drawScreen();
  return true;
}

static void cryptoExit() {
  wsWant = false;
  wsStarted = false;
  ws.disconnect();
}

static void enterApp(App a) {
  if (appEntered) {
    if (app == APP_CRYPTO) cryptoExit();
    else if (app == APP_CLOCK) clockExit();
    else if (app == APP_STOCK) stockExit();
  }
  app = a;
  cfg.app = (uint8_t)a;
  cfgSaveApp();
  if (!screenOn) return;  // woken later via screenOn_(): re-enters there
  if (a == APP_CRYPTO) cryptoEnter();
  else if (a == APP_CLOCK) clockEnter();
  else stockEnter();
  appEntered = true;
  // enterApp() also runs from setup(), before loop()'s flush can push
  // anything, so the entry paint is pushed here. At runtime this is a no-op
  // for the current pass — loop() flushes whatever is left at its end.
  tft.flush();
  const char *nm = a == APP_CRYPTO ? "CRYPTO" : a == APP_CLOCK ? "CLOCK" : "STOCK";
  Serial.printf("[militv] app -> %s\n", nm);
}

static void switchApp(int dir) {
  App next = (App)(((int)app + dir + APP_N) % APP_N);
  enterApp(next);
}

// Shared setup-portal screen (boot AND runtime): AP name, browser URL, and
// the exit hint. Called after netPortalStart().
static void drawPortalScreen() {
  tft.fill(0x0000);
  tft.text(40, 40, "SETUP", 0xFFFF, 3);
  tft.text(40, 80, "JOIN AP:", 0xFFE0, 2);
  tft.text(40, 102, PORTAL_AP_NAME, 0x07E0, 2);
  tft.text(40, 132, "OPEN 192.168.4.1", 0x07FF, 2);
  tft.text(40, 160, "WIFI + TICKER + APPS", 0x8410, 1);
  tft.text(40, 176, "SAVE=APPLY+TAP POWER", 0x8410, 1);
  tft.text(40, 190, "EXITS W/O SAVING", 0x8410, 1);
  drawFooter();
  tft.flush();
}

// Enter the setup portal at runtime: freeze app traffic (WS off, fetches
// staged off), open the AP, paint the screen. Forces the screen on: entering
// from a tap-wake press must never leave the panel asleep under the portal.
// (screenOn_ is defined below; forward-declared here for enterPortal.)
static void screenOn_();
static void enterPortal() {
  if (net.portalArmed) return;
  if (!screenOn) screenOn_();  // wake first (re-enters app harmlessly)
  Serial.println("[militv] POWER hold 3s — entering setup portal");
  if (app == APP_CRYPTO) {
    wsWant = false;
    wsStarted = false;
    ws.disconnect();
    cryptoFetchStage = 0;
  }
  appEntered = false;  // exit path re-enters the app fresh
  netPortalStart();
  drawPortalScreen();
}

// Exit WITHOUT saving: abort the AP, resume STA, re-enter the current app.
static void exitPortal() {
  if (!net.portalArmed) return;
  Serial.println("[militv] exit setup — resuming (no save)");
  netPortalAbort();
  appEntered = false;
  enterApp(app);
}

static void screenOff() {
  screenOn = false;
  ledcWrite(TFT_BL_CH, 0);
  tft.sleep();
  Serial.println("[militv] screen OFF (POWER to wake)");
}

static void screenOn_() {
  tft.wake();
  screenOn = true;
  ledcWrite(TFT_BL_CH, bl);
  appEntered = false;  // force a fresh enter(): redraw + resume stream
  enterApp(app);
  Serial.println("[militv] screen ON");
}

// pollKey events: 1 = press, 2 = hold-repeat, 3 = release (added for v4 so a
// tap can switch apps while a hold drives the backlight).
static int pollKey(Key &k, uint32_t now) {
  bool phys = digitalRead(k.pin) == LOW;
  if (phys != k.lastPhys) {
    k.lastPhys = phys;
    k.since = now;
  }
  if ((now - k.since) < BTN_DEBOUNCE_MS) return 0;
  if (phys != k.debounced) {
    k.debounced = phys;
    if (phys) {
      k.pressedAt = now;
      k.lastRep = now;
      return 1;
    }
    return 3;
  }
  if (k.debounced && (now - k.pressedAt) > BTN_REPEAT_START_MS &&
      (now - k.lastRep) > BTN_REPEAT_MS) {
    k.lastRep = now;
    return 2;
  }
  return 0;
}

static void bumpBl(int delta) {
  int v = (int)bl + delta;
  if (v < 5) v = 5;
  if (v > 255) v = 255;
  setBacklight((uint8_t)v);
  Serial.printf("[militv] backlight %d\n", bl);
}

// ---------- BLE / serial command console ----------
// One shared command set for both transports: the BLE UART service and, for
// convenience while tethered, the USB serial line. Commands are deliberately
// small and text-only so any BLE-UART app (or `pio device monitor`) can drive
// the device without a custom client.
//
//   help              list commands
//   app <crypto|clock|stock>   switch app
//   next | prev       cycle apps
//   status            one-line telemetry
//   batt              battery detail
//   fs                asset partition usage
//   beep [hz] [ms]    play a tone (defaults 880 Hz 150 ms)
//   quiet             stop audio
//   bright <0-100>    backlight percent
//   portal            open the WiFi setup portal
//   quit              leave the setup portal without saving
static void runCommand(const char *line, char *resp, size_t respN, bool fromBle) {
  auto ok = [&](const char *s) { snprintf(resp, respN, "%s", s); };
  // Skip leading spaces.
  while (*line == ' ') line++;
  if (!*line) return;

  if (!strcmp(line, "help")) {
    ok("cmds: help app next prev status px ticker fb batt 'batt scan' fs beep quiet bright portal quit");
  } else if (!strncmp(line, "app", 3)) {
    const char *a = line + 3;
    while (*a == ' ') a++;
    App target = app;
    if (!strncmp(a, "crypto", 3)) target = APP_CRYPTO;
    else if (!strncmp(a, "clock", 3)) target = APP_CLOCK;
    else if (!strncmp(a, "stock", 3)) target = APP_STOCK;
    else {
      ok("usage: app crypto|clock|stock");
      return;
    }
    if (screenOn) enterApp(target);
    snprintf(resp, respN, "app -> %s",
             target == APP_CRYPTO ? "CRYPTO" : target == APP_CLOCK ? "CLOCK" : "STOCK");
  } else if (!strcmp(line, "next")) {
    if (screenOn) switchApp(1);
    ok("ok");
  } else if (!strcmp(line, "prev")) {
    if (screenOn) switchApp(-1);
    ok("ok");
  } else if (!strcmp(line, "status")) {
    char nb[40], bb[24];
    netStatus(nb, sizeof(nb));
    batteryStatus(bb, sizeof(bb));
    snprintf(resp, respN, "%s ws=%s n=%d bl=%d %s %s",
             app == APP_CRYPTO ? "CRYPTO" : app == APP_CLOCK ? "CLOCK" : "STOCK",
             ws.isConnected() ? "up" : "down", chart.n, bl, nb, bb);
  } else if (!strcmp(line, "px")) {
    // Report exactly what the chart would draw right now: the min/max axis
    // labels and the live price. Lets the ticker format be checked over serial
    // without reading the glass (sub-dollar pairs were printing "$0").
    char hi[16], lo[16], lv[16];
    chartPriceText(hi, sizeof(hi), chart.hi);
    chartPriceText(lo, sizeof(lo), chart.lo);
    chartPriceText(lv, sizeof(lv), chart.live);
    snprintf(resp, respN, "axis %s / %s  live %s  n=%d", hi, lo, lv, chart.n);
  } else if (!strncmp(line, "ticker", 6)) {
    // Switch the crypto pair at runtime: rewrites cfg, restarts the chart.
    const char *t = line + 6;
    while (*t == ' ') t++;
    size_t tl = strlen(t);
    if (tl < 6 || tl > 12) {
      ok("usage: ticker BTCUSDT (6-12 chars, e.g. ETHUSDT)");
      return;
    }
    char up[16];
    size_t k = 0;
    for (; t[k] && k < sizeof(up) - 1; k++) {
      char c = t[k];
      up[k] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    up[k] = 0;
    strncpy(cfg.ticker, up, sizeof(cfg.ticker) - 1);
    cfg.ticker[sizeof(cfg.ticker) - 1] = 0;
    cfgSave();
    // Force cryptoEnter() to treat this as a new pair: drop the buffer, stop
    // the socket, and let the staged bootstrap refetch on the next passes.
    wsWant = false;
    wsStarted = false;
    ws.disconnect();
    cryptoExit();
    loadedTicker[0] = 0;
    appEntered = false;
    enterApp(APP_CRYPTO);
    snprintf(resp, respN, "ticker -> %s", cfg.ticker);
  } else if (!strncmp(line, "fb", 2)) {
    // ASCII dump of a framebuffer region: "fb [x0 y0 x1 y1]" (default the
    // whole screen). Prints one char per pixel — '#' lit, '.' black — so the
    // rendered layout can be checked over serial instead of by eye on glass.
    // Handy when the panel is behind a bezel or the text is clipped.
    if (!tft.hasFb()) {
      ok("no framebuffer (direct mode)");
      return;
    }
    int x0 = 0, y0 = 0, x1 = TFT_W - 1, y1 = TFT_H - 1;
    sscanf(line + 2, "%d %d %d %d", &x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > TFT_W - 1) x1 = TFT_W - 1;
    if (y1 > TFT_H - 1) y1 = TFT_H - 1;
    Serial.printf("[fb] %d,%d..%d,%d\n", x0, y0, x1, y1);
    // Column ruler every 10 px so a row can be located without counting.
    Serial.print("     ");
    for (int x = x0; x <= x1; x++) Serial.print((x % 10 == 0) ? ('0' + (x / 10) % 10) : ' ');
    Serial.println();
    char row[TFT_W + 1];
    for (int y = y0; y <= y1; y++) {
      int n = 0;
      for (int x = x0; x <= x1; x++) row[n++] = tft.at(x, y) ? '#' : '.';
      row[n] = 0;
      Serial.printf("%3d  %s\n", y, row);
    }
    ok("fb dumped above");
  } else if (!strcmp(line, "batt")) {
    batteryStatus(resp, respN);
  } else if (!strcmp(line, "batt scan")) {
    batteryScan();
    ok("scan printed on serial");
  } else if (!strcmp(line, "fs")) {
    assetsStatus(resp, respN);
  } else if (!strncmp(line, "beep", 4)) {
    int hz = 880, ms = 150;
    sscanf(line + 4, "%d %d", &hz, &ms);
    if (hz < 60) hz = 60;
    if (hz > 8000) hz = 8000;
    audioTone((uint16_t)hz, (uint16_t)ms);
    ok("beep");
  } else if (!strcmp(line, "quiet")) {
    audioStop();
    ok("quiet");
  } else if (!strncmp(line, "bright", 6)) {
    int p = atoi(line + 6);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    setBacklight((uint8_t)(p * 255 / 100));
    snprintf(resp, respN, "bl %d%%", p);
  } else if (!strcmp(line, "portal")) {
    if (!net.portalArmed && screenOn) {
      enterPortal();
      ok("portal open");
    } else {
      ok("already in portal");
    }
  } else if (!strcmp(line, "quit")) {
    if (net.portalArmed) {
      exitPortal();
      ok("portal closed");
    } else {
      ok("not in portal");
    }
  } else {
    ok("unknown cmd (try help)");
  }
  Serial.printf("[%s] %s\n", fromBle ? "ble" : "cmd", resp);
}

static void bleCmd(const char *line, char *resp, size_t respN) {
  runCommand(line, resp, respN, true);
}


void setup() {
  Serial.begin(UART_BAUD);
  delay(300);
  Serial.println("\n[militv] v6 boot — multi-app + audio/BLE/battery/assets");
  cfgLoad();
  app = (cfg.app < APP_N) ? (App)cfg.app : APP_CRYPTO;
  chartBind(&tft);
  clockBind(&tft);
  stockBind(&tft);

  // Peripherals that need no display: bring them up first so a failure is
  // reported on serial before the screen is even initialized.
  assetsBegin();
  batteryBegin();
  audioBegin();
  bleSetHandler(bleCmd);
  // BLE name: "MILITV-" + city. BLE advertise names are limited to 29 bytes of
  // payload, and cfg.city can be 23 chars, so clamp hard and strip spaces.
  snprintf(sBleName, sizeof(sBleName), "MILITV-%s", cfg.city[0] ? cfg.city : "DEV");
  for (char *p = sBleName; *p; p++)
    if (*p == ' ') *p = '-';
  // NOTE: bleBegin() is deferred until after netBegin() — WiFi and BLE share
  // the S3 radio, and bringing WiFi up first is the supported coexistence
  // order (see netcfg.cpp for the modem-sleep requirement).

  ledcSetup(TFT_BL_CH, TFT_BL_FREQ, TFT_BL_BITS);
  ledcAttachPin(TFT_BL, TFT_BL_CH);
  ledcWrite(TFT_BL_CH, 0);

  pinMode(BTN_PLUS, INPUT_PULLUP);
  pinMode(BTN_MINUS, INPUT_PULLUP);
  pinMode(BTN_POWER, INPUT_PULLUP);
  uint32_t now = millis();
  for (Key *k : {&kPlus, &kMinus, &kPower}) {
    bool phys = digitalRead(k->pin) == LOW;
    k->lastPhys = phys;
    k->debounced = phys;
    k->since = now;
    k->pressedAt = phys ? now : 0;
    k->lastRep = now;
  }
  bool powerHeld = kPower.debounced;
  // POWER doubles as portal trigger only if HELD through boot. Sample it
  // for 3 s so a stray boot edge can't trap the user in the portal.
  if (powerHeld) {
    Serial.println("[militv] POWER down — hold 3s for setup portal...");
    tft.begin();
    tft.fill(0x0000);
    tft.text(40, 100, "HOLD POWER 3S", 0xFFE0, 2);
    tft.text(40, 130, "FOR WIFI + APPS", 0xFFE0, 2);
    setBacklight(255);
    tft.flush();  // loop() has not started yet: nothing else will push this
    uint32_t t0 = millis();
    while (millis() - t0 < 3000) {
      if (digitalRead(BTN_POWER) != LOW) {
        powerHeld = false;
        break;
      }
      delay(20);
    }
    if (powerHeld) Serial.println("[militv] portal trigger confirmed");
  }
  if (kMinus.debounced)
    Serial.println("[militv] NOTE: MINUS reads pressed at boot — check for stuck key");

  tft.begin();
  tft.fill(0x0000);
  tft.text(40, 8, "MILITV", 0xFFFF, 2);
  tft.text(40, 100, "CONNECTING WIFI", 0xFFE0, 2);
  setBacklight(255);
  tft.flush();

  bool up = netBegin(powerHeld);
  if (net.portalArmed) {
    drawPortalScreen();
  } else if (up) {
    wasUp = true;
    enterApp(app);
  } else {
    tft.fill(0x0000);
    tft.text(40, 8, "MILITV", 0xFFFF, 2);
    tft.text(40, 100, "WIFI FAILED", 0xF800, 2);
    tft.text(40, 130, "HOLD POWER 3S", 0xFFE0, 2);
    tft.text(40, 152, "FOR SETUP PORTAL", 0xFFE0, 2);
    drawFooter();
    tft.flush();
    // Watchdog may still join later; loop() enters the app on the up-edge.
  }
  // BLE last: WiFi is up (or the portal is running), so the radio is already
  // in a known state before we add the second stack on top of it.
  bleBegin(sBleName);
  Serial.println("[militv] ready — tap -/+ apps, hold -/+ light, POWER tap sleep / 3s hold setup");
}

void loop() {
  uint32_t now = millis();

  // ---- 1. buttons + serial FIRST, every pass. No network call in this loop
  // ever runs before the keys are polled, so a slow fetch can delay a tap's
  // *effect* by one pass at most — it can never swallow it. (This is the fix
  // for the "WEATHER FAILED trapped my buttons" report.)
  // Line-oriented input, so a typed command like "batt scan" is never
  // misread as the single-char shortcuts it happens to contain. A one-char
  // line is still the classic shortcut; anything longer is a command.
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (sCmdN == 1) {
        char k = sCmd[0];
        if (k == '+') bumpBl(5);
        else if (k == '-') bumpBl(-5);
        else if (k == 'n' || k == 'p') { if (screenOn) switchApp(k == 'n' ? 1 : -1); }
        else if (k == 'w' && screenOn && !net.portalArmed) enterPortal();  // test hook
        else if (k == 'x' && net.portalArmed) exitPortal();  // test hook = tap POWER
        else Serial.println("[cmd] single-char keys: +/- light, n/p app, w/x portal");
      } else if (sCmdN > 1) {
        sCmd[sCmdN] = 0;
        char resp[BLE_LINE_MAX];
        resp[0] = 0;
        runCommand(sCmd, resp, sizeof(resp), false);
      }
      sCmdN = 0;
      continue;
    }
    if (sCmdN < sizeof(sCmd) - 1) sCmd[sCmdN++] = c;
    else sCmdN = 0;  // overlong: discard
  }
  // Tap -/+ = switch app; hold = backlight repeat. The tap fires on release
  // (event 3) so a hold never also switches; the hold cancels the pending tap.
  static bool pendPlus = false, pendMinus = false;
  static uint32_t tPlus = 0, tMinus = 0;
  int e;
  e = pollKey(kPlus, now);
  if (e == 1 && screenOn) {
    pendPlus = true;
    tPlus = now;
  } else if (e == 2 && screenOn) {
    pendPlus = false;
    bumpBl(5);
  } else if (e == 3 && screenOn) {
    if (pendPlus) switchApp(1);
    pendPlus = false;
  }
  e = pollKey(kMinus, now);
  if (e == 1 && screenOn) {
    pendMinus = true;
    tMinus = now;
  } else if (e == 2 && screenOn) {
    pendMinus = false;
    bumpBl(-5);
  } else if (e == 3 && screenOn) {
    if (pendMinus) switchApp(-1);
    pendMinus = false;
  }
  // Safety: a press with no release in 12 s isn't a tap. (12 s, not 5:
  // a tap that lands just before a ~4 s TLS fetch must still be pending
  // when its release is polled after the fetch returns.)
  if (pendPlus && now - tPlus > 12000) pendPlus = false;
  if (pendMinus && now - tMinus > 12000) pendMinus = false;
  // POWER: tap (release < 3 s after press) = sleep, or exit-portal when the
  // portal runs; hold >= 3 s = setup portal (boot AND runtime — the battery
  // means no power cycle). Sleep fires on RELEASE, not press, so a hold never
  // sleeps first. While the portal runs, a tap exits WITHOUT saving; a hold
  // that opened the portal must NOT exit on its own release (the release that
  // ends the opening hold is ignored — only a FRESH tap exits).
  static uint32_t powerPressMs = 0;      // press start (0 = none in flight)
  static bool powerPortalFired = false;  // this press already opened portal
  e = pollKey(kPower, now);
  if (e == 1) {
    if (!screenOn) {
      screenOn_();
      powerPressMs = 0;
      powerPortalFired = false;
    } else if (powerPressMs == 0) {
      powerPressMs = now;
      powerPortalFired = false;
    }
  } else if (e == 2 && screenOn && powerPressMs != 0 && !powerPortalFired &&
             !net.portalArmed && now - powerPressMs >= 3000) {
    powerPortalFired = true;
    enterPortal();
  } else if (e == 3 && screenOn) {
    if (net.portalArmed) {
      if (powerPortalFired) {
        // Release ending the hold that OPENED the portal: swallow it. The
        // portal stays up; only a fresh tap (press+release with fired=false)
        // exits. (This was the "must keep holding" bug.)
      } else {
        // Fresh tap while in portal = "Exit Setup": abort WITHOUT saving.
        exitPortal();
      }
    } else if (powerPressMs != 0 && !powerPortalFired && now - powerPressMs < 3000) {
      screenOff();
    }
    powerPressMs = 0;
    powerPortalFired = false;
  }

  // ---- 2. networking + app pumps (at most ONE fetch per pass: crypto
  // history/ticker, clock job, or stock fetch — whichever fires first skips
  // the rest via the didFetch flag, so a slow TLS handshake never stacks two
  // blocking calls in one loop).
  bool wasPortal = net.portalArmed;
  bool inPortal = netPump(now);
  // Portal just ended WITHOUT save (tap-POWER exit or 120 s timeout via
  // netPortalAbort): re-enter the current app to repaint it. The save path
  // keeps portalArmed until the reboot below, so this only fires on abort.
  if (wasPortal && !inPortal && !cfgNeedReboot && screenOn) {
    appEntered = false;
    enterApp(app);
  }

  // Portal save (WiFi and/or app settings): reboot once joined so the new
  // network + ticker/WS path apply atomically. No-change save still reboots
  // (WiFiManager gives no "dirty" signal) — harmless, ~5 s.
  // Reboot REGARDLESS of screen state: a save-while-asleep must not park the
  // device in portal-limbo with the panel off (the reported black-screen
  // brick). Boot always wakes with backlight on.
  if (cfgNeedReboot && !net.portalArmed) {
    Serial.println("[cfg] settings saved — rebooting to apply");
    delay(800);
    ESP.restart();
  }

  bool upNow = !inPortal && WiFi.status() == WL_CONNECTED;
  bool didFetch = false;
  if (upNow && !wasUp) {
    // (Re)joined: first time enters the app; later rejoins resume crypto.
    // enterApp() only stages the bootstrap — no fetch runs here.
    if (!appEntered && screenOn) enterApp(app);
  }
  wasUp = upNow;

  // Crypto staged bootstrap (one stage per pass, set by cryptoEnter()).
  if (!didFetch && app == APP_CRYPTO && screenOn && !inPortal && upNow && cryptoFetchStage) {
    didFetch = cryptoPumpFetch();
  }

  // Crypto WS lifecycle: start once STA is up, pump while up.
  // wsStarted latches the in-flight attempt so it isn't doubled before the
  // handshake completes; the 24 h server cut comes back through here.
  if (!didFetch && app == APP_CRYPTO && !inPortal && wsWant && upNow && !ws.isConnected() &&
      !wsStarted && cryptoFetchStage == 0) {
    static uint32_t lastWsTry = 0;
    if (now - lastWsTry > 5000) {
      lastWsTry = now;
      if (chart.n == 0) {
        // Bootstrap drained but chart still empty (fetch failed): retry the
        // staged bootstrap instead of jumping straight to WS.
        cryptoFetchStage = chart.have24 ? 2 : 1;
        didFetch = cryptoPumpFetch();
      } else {
        drawScreen();
        wsStart();
        didFetch = true;
      }
    }
  }
  // Attempt stuck in handshake (>20 s, no CONNECTED)? Clear the latch so
  // the block above retries.
  if (wsStarted && !ws.isConnected() && now - wsStartAt > 20000) {
    Serial.println("[ws] start stuck >20s — will retry");
    wsStarted = false;
  }
  if (app == APP_CRYPTO && screenOn && !inPortal && upNow) {
    ws.loop();
    // Apply whatever the WS task queued: price/24h/dot updates happen here on
    // the loop task, never in the callback.
    cryptoDrainWs();
  }
  if (!didFetch && app == APP_CLOCK && screenOn && !inPortal) clockPump();
  if (!didFetch && app == APP_STOCK && screenOn && !inPortal) stockPump();

  static uint32_t lastStat = 0, lastFoot = 0;
  if (now - lastStat > 10000) {
    lastStat = now;
    char nb[40];
    netStatus(nb, sizeof(nb));
    const char *nm = app == APP_CRYPTO ? "CRYPTO" : app == APP_CLOCK ? "CLOCK" : "STOCK";
    Serial.printf("[militv] alive %lu app=%s bl=%d screen=%s %s ws=%s n=%d%s\n",
                  (unsigned long)now, nm, bl, screenOn ? "on" : "off", nb,
                  ws.isConnected() ? "up" : "down", chart.n,
                  inPortal ? " PORTAL" : "");
  }
  // Crypto footer: drawFooter already skips steady text, so this is free.
  if (app == APP_CRYPTO && screenOn && !inPortal && now - lastFoot > 5000) {
    lastFoot = now;
    drawFooter();
  }

  // ---- 3. peripherals that ride on the loop but own no screen space.
  // BLE console: drain commands queued by the BLE host task.
  blePump();
  // Audio: feed the I2S DMA. Idle is a single comparison.
  audioPump();
  // Battery: slow ADC sample on its own schedule, plus a low-pack warning.
  batteryPump();
  static bool lowWarned = false;
  if (batteryLow() && !lowWarned) {
    lowWarned = true;
    Serial.println("[batt] LOW — warning beep");
    audioAlert();
    bleNotify("batt low");
  } else if (!batteryLow()) {
    lowWarned = false;  // re-arm once charged back above the threshold
  }

  // ---- 4. push this pass's drawing to the panel: ONE clean update per loop.
  // In framebuffer mode the app primitives only touched PSRAM, so the erase,
  // the text and the chart segment all land in a single window — the panel
  // never shows the intermediate black state (that was the refresh "blink").
  // Repeated flushes within a pass are harmless: flush() is a no-op when clean.
  if (screenOn) tft.flush();
}

// See clockapp.h for the design notes.
#include "clockapp.h"
#include "tft.h"
#include "cfg.h"
#include "netcfg.h"
#include <ArduinoJson.h>
#include <time.h>

static MiniTFT *ct = nullptr;
void clockBind(MiniTFT *tft) { ct = tft; }

static bool haveTime = false;
static bool wxFailed = false;
static float wTemp = 0, wHi = 0, wLo = 0;
static int wCode = -1;
static bool haveWx = false;
static uint32_t lastNtp = 0, lastWx = 0, lastMin = 0;
// Only one network job runs per loop() pass (staggered): 0 none, 1 NTP,
// 2 weather. A job is *started* on one pump and *finished* on the next, so
// the tap that opens the clock (and every loop) stays responsive.
static uint8_t pendingJob = 0;
static char footBuf[48] = "";
// Partial-redraw memory: what is currently on screen, so only changed fields
// repaint (no full clear => no blink).
static char shownTime[8] = "";
static char shownDate[16] = "";
static char shownTemp[24] = "";
static char shownCond[24] = "";
static char shownHiLo[24] = "";
static char shownCity[24] = "";
static char shownFoot[48] = "";
static bool clockLaidOut = false;

// ---- layout: the same safe frame the crypto and stock apps use ----
// The bezel covers x<40 and the glued frame covers x>300, so every field lives
// inside x40..299 and the header/footer sit outside an inner border box that
// matches the crypto chart's plot box (x40..300, y32..215). Two rules make
// "text will be missed" impossible here:
//   1. every string is truncated to its slot width before drawing, so it can
//      never run under the frame;
//   2. every field's erase box spans its whole slot, so a value that shrinks
//      ("28C" -> "-5C", "PARTLY CLDY" -> "OVERCAST") cannot leave fragments.
// The old screen drew centered text with no truncation and erased it with a
// box narrower than the text: a partial value lingering outside the erase box
// is what showed up as the phantom "IG" (the leftover "NG" of the loading
// message, half-erased by the weather-icon box).
static const int FX = 40;            // safe frame left
static const int FR = 299;           // safe frame right (inclusive)
static const int FW = FR - FX + 1;   // safe frame width, 260 px
// Content is inset by 1 px so a field's erase band never covers the border
// columns FX/FR — the border is a 1 px line and a full-width erase would slice
// it into dashes exactly as tall as the fields.
static const int FXI = FX + 1;       // content area left
static const int FWI = FW - 2;       // content area width, 258 px
static const int BX_Y0 = 32;         // inner border top (matches crypto plot)
static const int BX_Y1 = 215;        // inner border bottom
static const int ROW_HEADER = 8;     // city (left) + date (right), scale 2
static const int ROW_TIME = 44;      // HH:MM, scale 8
static const int ROW_TEMP = 112;     // temp, scale 6
static const int ROW_COND = 164;     // condition, scale 2
static const int ROW_HILO = 186;     // high/low or the NTP status, scale 2
static const int ROW_FOOT = 222;     // wifi status, scale 1
// Header slots: city is left-aligned, the date right-aligns to the frame edge.
static const int CITY_W = 136;       // city slot (x40..175)
static const int DATE_W = 120;       // date slot (x180..299)

static const char *wmoText(int c) {
  if (c == 0) return "SUNNY";
  if (c <= 3) return c == 3 ? "OVERCAST" : "PARTLY CLDY";
  if (c <= 48) return "FOGGY";
  if (c <= 57) return "DRIZZLE";
  if (c <= 67) return "RAINY";
  if (c <= 77) return "SNOWY";
  if (c <= 82) return "SHOWERS";
  if (c <= 86) return "SNOW";
  return "STORM";
}

static void upper(char *s) {
  for (; *s; s++)
    if (*s >= 'a' && *s <= 'z') *s -= 32;
}

// Longest prefix of src that fits maxw px at this scale, so the drawn text and
// its erase box are always the same width and nothing crosses the frame.
static void fit(char *dst, size_t dstN, const char *src, int maxw, uint8_t scale) {
  int maxChars = maxw / (6 * scale);
  size_t n = strlen(src);
  if ((int)n > maxChars) n = (size_t)maxChars;
  if (n >= dstN) n = dstN - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

// Centered field: erase the whole content band (inside the border), then draw
// centered. The band excludes FX/FR so the 1 px border survives.
static void fieldCenter(int y, const char *s, uint16_t color, uint8_t scale, char *shown,
                        size_t shownN) {
  char buf[40];
  fit(buf, sizeof(buf), s, FWI, scale);
  if (strcmp(shown, buf) == 0) return;
  int w = (int)strlen(buf) * 6 * scale;
  ct->rect(FXI, y, FWI, 8 * scale, 0x0000);
  if (buf[0]) ct->text(FXI + (FWI - w) / 2, y, buf, color, scale);
  strncpy(shown, buf, shownN - 1);
  shown[shownN - 1] = 0;
}

// Left-aligned field in a fixed-width slot (the header city).
static void fieldLeft(int x, int y, const char *s, uint16_t color, uint8_t scale, char *shown,
                      size_t shownN, int clearW) {
  char buf[40];
  fit(buf, sizeof(buf), s, clearW, scale);
  if (strcmp(shown, buf) == 0) return;
  ct->rect(x, y, clearW, 8 * scale, 0x0000);
  if (buf[0]) ct->text(x, y, buf, color, scale);
  strncpy(shown, buf, shownN - 1);
  shown[shownN - 1] = 0;
}

// Right-aligned field ending at the frame edge (the header date).
static void fieldRight(int xR, int y, const char *s, uint16_t color, uint8_t scale, char *shown,
                       size_t shownN, int clearW) {
  char buf[40];
  fit(buf, sizeof(buf), s, clearW, scale);
  if (strcmp(shown, buf) == 0) return;
  int w = (int)strlen(buf) * 6 * scale;
  int x = xR - w;
  if (x < FX) x = FX;
  ct->rect(xR - clearW + 1, y, clearW, 8 * scale, 0x0000);
  if (buf[0]) ct->text(x, y, buf, color, scale);
  strncpy(shown, buf, shownN - 1);
  shown[shownN - 1] = 0;
}

// NTP, split across two pump() passes: start (cheap: configTime only) then
// finish (bounded getLocalTime poll ~2 s max). Never more than one pass of
// blocking per loop, and the opening tap never blocks at all.
static void ntpStart() {
  long gmtSec = (long)(cfg.gmtOff * 3600);
  configTime(gmtSec, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  pendingJob = 1;
}

static void ntpFinish() {
  pendingJob = 0;
  lastNtp = millis();
  struct tm tm;
  if (getLocalTime(&tm, 2000)) {
    haveTime = true;
    Serial.println("[clock] NTP synced");
  } else {
    Serial.println("[clock] NTP failed — will retry");
  }
}

static void wxFetch() {
  if (WiFi.status() != WL_CONNECTED) return;
  // Raw TCP GET, not HTTPClient: Open-Meteo serves Transfer-Encoding: chunked
  // and ignores Accept-Encoding: identity, and this HTTPClient build misreads
  // chunked bodies (headers OK, body -> HTTPC_ERROR_READ_TIMEOUT -11, probed
  // 2026-10-09). So we read the socket ourselves and de-chunk manually.
  // Plain HTTP, not HTTPS: Open-Meteo's TLS stack demands renegotiation,
  // which the ESP32 mbedTLS client cannot do (start_ssl_client: -1).
  // Weather is public data, no auth involved. HTTPS kept for Binance/Yahoo.
  // Buffer must hold the fully rendered request (~144 chars at these
  // lat/lon magnitudes). snprintf below checks the return so a future edit
  // that outgrows this cannot silently send a truncated, malformed query.
  char path[176];
  int need = snprintf(path, sizeof(path),
                      "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,"
                      "weather_code&daily=temperature_2m_max,temperature_2m_min&timezone=auto",
                      cfg.lat, cfg.lon);
  if (need < 0 || need >= (int)sizeof(path)) {
    Serial.printf("[clock] wx URL truncated (%d >= %d)\n", need, (int)sizeof(path));
    wxFailed = true;
    return;
  }
  wxFailed = false;
  lastWx = millis();  // attempt-stamped: failures retry on the 60 s lane
  WiFiClient cli;
  cli.setTimeout(4000);  // fail fast: a dead fetch must not eat the tap window
  if (!cli.connect("api.open-meteo.com", 80)) {
    Serial.println("[clock] wx TCP connect failed");
    wxFailed = true;
    return;
  }
  cli.printf("GET %s HTTP/1.1\r\nHost: api.open-meteo.com\r\nConnection: close\r\nUser-Agent: "
             "militv\r\n\r\n",
             path);
  // Skip response headers (until the blank line).
  uint32_t t0 = millis();
  String line;
  bool got200 = false;
  while (millis() - t0 < 4000) {
    line = cli.readStringUntil('\n');
    if (line.startsWith("HTTP/")) got200 = line.indexOf(" 200 ") > 0;
    if (line == "\r" || line.length() == 0) break;
  }
  if (!got200) {
    Serial.println("[clock] wx bad status");
    cli.stop();
    wxFailed = true;
    return;
  }
  // Read chunked body: <hexlen>\r\n <bytes>\r\n ... 0\r\n.
  String body;
  body.reserve(1024);
  t0 = millis();
  while (millis() - t0 < 4000 && body.length() < 2048) {
    String hlen = cli.readStringUntil('\n');
    hlen.trim();
    if (hlen.length() == 0) continue;
    long len = strtol(hlen.c_str(), nullptr, 16);
    if (len <= 0) break;
    if (len > 1500) break;  // sanity: our payload is ~700 B
    while (len > 0 && millis() - t0 < 4000) {
      int c = cli.read();
      if (c < 0) {
        delay(5);
        continue;
      }
      body += (char)c;
      len--;
    }
    cli.readStringUntil('\n');  // trailing CRLF after the chunk
  }
  cli.stop();
  if (body.length() < 50) {
    Serial.printf("[clock] wx short body %d\n", body.length());
    wxFailed = true;
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[clock] wx JSON parse failed");
    wxFailed = true;
    return;
  }
  wTemp = doc["current"]["temperature_2m"] | 0.0f;
  wCode = doc["current"]["weather_code"] | -1;
  wHi = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  wLo = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  haveWx = true;
  lastWx = millis();
  Serial.printf("[clock] wx %.1fC code=%d hi=%.0f lo=%.0f\n", wTemp, wCode, wHi, wLo);
}

// Full layout once (per entry): black fill, the inner border box, then the
// cached paint. After that only the field updaters touch pixels — steady
// content costs zero pixels.
static void clockLayout() {
  ct->fill(0x0000);
  ct->rect(0, 216, TFT_W, 24, 0x0000);  // footer band, same as the other apps
  // Inner border: the crypto chart's plot box, so all three apps share one
  // visible content frame aligned inside the bezel/glued frame.
  ct->rect(FX, BX_Y0, FW, 1, 0x2104);
  ct->rect(FX, BX_Y1, FW, 1, 0x2104);
  ct->rect(FX, BX_Y0, 1, BX_Y1 - BX_Y0 + 1, 0x2104);
  ct->rect(FR, BX_Y0, 1, BX_Y1 - BX_Y0 + 1, 0x2104);
  shownTime[0] = 0;
  shownDate[0] = 0;
  shownTemp[0] = 0;
  shownCond[0] = 0;
  shownHiLo[0] = 0;
  shownCity[0] = 0;
  shownFoot[0] = 0;
  clockLaidOut = true;
}

// Partial repaint: every field erases + rewrites only when its text changed.
static void clockPaint() {
  if (!ct) return;
  if (!clockLaidOut) clockLayout();
  char line[40];
  bool timeOk = false;
  if (haveTime) {
    struct tm tm;
    if (getLocalTime(&tm, 500)) {
      timeOk = true;
      strftime(line, sizeof(line), "%H:%M", &tm);
      fieldCenter(ROW_TIME, line, 0xFFFF, 8, shownTime, sizeof(shownTime));
      strftime(line, sizeof(line), "%a %d %b", &tm);
      upper(line);
      fieldRight(FR, ROW_HEADER, line, 0x07FF, 2, shownDate, sizeof(shownDate), DATE_W);
    }
  }
  if (!timeOk) {
    fieldCenter(ROW_TIME, "--:--", 0x8410, 8, shownTime, sizeof(shownTime));
    fieldRight(FR, ROW_HEADER, "", 0x07FF, 2, shownDate, sizeof(shownDate), DATE_W);
  }
  // City: header-left, same slot discipline as crypto's SYM.
  fieldLeft(FX, ROW_HEADER, cfg.city, 0x8410, 2, shownCity, sizeof(shownCity), CITY_W);

  if (haveWx && wCode >= 0) {
    snprintf(line, sizeof(line), "%.0fC", wTemp);
    fieldCenter(ROW_TEMP, line, 0xFFFF, 6, shownTemp, sizeof(shownTemp));
    fieldCenter(ROW_COND, wmoText(wCode), 0x07E0, 2, shownCond, sizeof(shownCond));
    if (timeOk) {
      snprintf(line, sizeof(line), "HI %.0f  LO %.0f", wHi, wLo);
      fieldCenter(ROW_HILO, line, 0x8410, 2, shownHiLo, sizeof(shownHiLo));
    } else {
      fieldCenter(ROW_HILO, "SYNCING TIME", 0xFFE0, 2, shownHiLo, sizeof(shownHiLo));
    }
  } else if (wxFailed) {
    fieldCenter(ROW_TEMP, "WEATHER FAILED", 0xF800, 2, shownTemp, sizeof(shownTemp));
    fieldCenter(ROW_COND, "RETRYING", 0xFFE0, 2, shownCond, sizeof(shownCond));
    fieldCenter(ROW_HILO, timeOk ? "" : "SYNCING TIME", 0xFFE0, 2, shownHiLo, sizeof(shownHiLo));
  } else {
    fieldCenter(ROW_TEMP, "LOADING WEATHER", 0xFFE0, 2, shownTemp, sizeof(shownTemp));
    fieldCenter(ROW_COND, "", 0x07E0, 2, shownCond, sizeof(shownCond));
    fieldCenter(ROW_HILO, timeOk ? "" : "SYNCING TIME", 0xFFE0, 2, shownHiLo, sizeof(shownHiLo));
  }
  // Footer, same geometry as the other apps.
  const char *fs = clockStatus();
  if (strcmp(shownFoot, fs) != 0) {
    ct->rect(40, ROW_FOOT, 260, 8, 0x0000);
    ct->text(40, ROW_FOOT, fs, 0x07FF, 1);
    strncpy(shownFoot, fs, sizeof(shownFoot) - 1);
  }
}

void clockDraw() { clockPaint(); }

const char *clockStatus() {
  // Footer mirrors the crypto/stock apps: WiFi state, not the time — the big
  // HH:MM + city/condition lines already show time + place (the old footer
  // repeated "23:57 SINGAPORE 27C" redundantly).
  netStatus(footBuf, sizeof(footBuf));
  // A 32-char SSID would push "WIFI <ssid> -100DBM" past the 260 px footer box
  // (the crypto app clamps for the same reason): keep it inside the frame.
  footBuf[42] = 0;
  return footBuf;
}

void clockEnter() {
  // Layout + cached paint instantly; network starts on the next pump() pass
  // (staggered: NTP first, weather second) so the opening tap never blocks.
  clockLayout();
  clockPaint();
  struct tm tm;
  if (haveTime && getLocalTime(&tm, 500)) lastMin = tm.tm_min;
}

void clockExit() { pendingJob = 0; }

void clockPump() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  // Finish a started NTP job first (one bounded poll per loop max).
  if (pendingJob == 1) {
    ntpFinish();
    clockPaint();
    return;
  }
  // Start jobs staggered: NTP (if due) else weather (if due). Only one job
  // starts per pump so a slow TLS handshake never stacks behind NTP.
  // The lane is keyed off the ATTEMPT stamp, never off "do we have data yet":
  // a `!haveX ||` guard reads naturally but short-circuits the interval, so a
  // failing fetch retried on every single loop pass (observed: ~20 failed
  // weather attempts in 10 s, hammering the API and the radio).
  bool ntpDue = lastNtp == 0 || now - lastNtp > (haveTime ? 3600000UL : 30000UL);
  bool wxDue = lastWx == 0 || now - lastWx > (haveWx ? 600000UL : 60000UL);
  if (ntpDue) {
    ntpStart();
    return;  // finish lands on the next pump
  }
  if (wxDue && pendingJob == 0) {
    wxFetch();
    clockPaint();
    return;
  }
  struct tm tm;
  if (haveTime && getLocalTime(&tm, 500) && (uint32_t)tm.tm_min != lastMin) {
    lastMin = tm.tm_min;
    clockPaint();
    return;
  }
  // Footer shows WiFi state: repaint when it changes (drop/rejoin visible
  // without touching the time/weather fields). Cheap: clockStatus + paint
  // both skip steady values.
  static char lastNet[48] = "";
  char nb[48];
  netStatus(nb, sizeof(nb));
  if (strcmp(lastNet, nb) != 0) {
    strncpy(lastNet, nb, sizeof(lastNet) - 1);
    clockPaint();
  }
}

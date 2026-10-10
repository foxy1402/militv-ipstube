// See clockapp.h for the design notes.
#include "clockapp.h"
#include "tft.h"
#include "cfg.h"
#include "netcfg.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

static MiniTFT *ct = nullptr;
void clockBind(MiniTFT *tft) { ct = tft; }

static bool haveTime = false;
static bool wxFailed = false;
static float wTemp = 0, wHi = 0, wLo = 0;
static int wCode = -1;
static int wIsDay = 1;  // Open-Meteo is_day: 1 day, 0 night
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
static char shownTemp[12] = "";
static char shownCond[24] = "";
static char shownCity[24] = "";
static int shownCode = -2;
static char shownFoot[48] = "";
static bool clockLaidOut = false;

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

// Centered text, clamped to x>=40 (bezel) — returns nothing, just draws.
static void textC(int y, const char *s, uint16_t color, uint8_t scale) {
  int w = strlen(s) * 6 * scale;
  int x = (TFT_W - w) / 2;
  if (x < 40) x = 40;
  ct->text(x, y, s, color, scale);
}

// Erase-then-write a centered field, but only if the text actually changed.
// Same slot geometry both passes, so a steady value costs zero pixels.
static void textCUpd(int y, const char *s, uint16_t color, uint8_t scale, char *shown,
                     size_t shownN) {
  if (strcmp(shown, s) == 0) return;
  int wOld = strlen(shown) * 6 * scale, wNew = strlen(s) * 6 * scale;
  int xOld = wOld ? (TFT_W - wOld) / 2 : 0, xNew = (TFT_W - wNew) / 2;
  if (xOld < 40) xOld = 40;
  if (xNew < 40) xNew = 40;
  int x0 = xOld < xNew ? xOld : xNew;
  int x1 = (xOld + wOld) > (xNew + wNew) ? (xOld + wOld) : (xNew + wNew);
  ct->rect(x0, y, x1 - x0, 8 * scale, 0x0000);
  ct->text(xNew, y, s, color, scale);
  strncpy(shown, s, shownN - 1);
  shown[shownN - 1] = 0;
}

static void fillCircle(int ox, int oy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    ct->rect(ox - dx, oy + dy, dx * 2 + 1, 1, c);
  }
}

static void cloudShape(int ox, int oy, uint16_t c) {
  fillCircle(ox + 14, oy, 12, c);
  fillCircle(ox + 30, oy - 6, 15, c);
  fillCircle(ox + 46, oy, 11, c);
  ct->rect(ox + 4, oy, 52, 12, c);
}

static void drawWxIcon(int ox, int oy, int code) {
  // Clear sky shows a moon at night: Open-Meteo code 0 means "clear" either
  // way, and is_day tells which. (Midnight sun in Singapore was the giveaway.)
  bool night = (code <= 3 && wIsDay == 0);
  if (code == 0 && !night) {  // sun: disc + 8 rays
    fillCircle(ox + 16, oy + 16, 12, 0xFFE0);
    for (int i = 0; i < 8; i++) {
      float a = i * 3.14159f / 4;
      int x0 = ox + 16 + (int)(cosf(a) * 16), y0 = oy + 16 + (int)(sinf(a) * 16);
      int x1 = ox + 16 + (int)(cosf(a) * 23), y1 = oy + 16 + (int)(sinf(a) * 23);
      ct->line(x0, y0, x1, y1, 0xFFE0);
    }
  } else if (code == 0) {  // moon: crescent via overlapping discs
    fillCircle(ox + 16, oy + 16, 12, 0xC618);
    fillCircle(ox + 21, oy + 12, 10, 0x0000);
  } else if (code <= 3) {  // partly cloudy: small sun/moon + cloud
    if (night)
      fillCircle(ox + 12, oy + 8, 8, 0xC618);
    else
      fillCircle(ox + 12, oy + 8, 8, 0xFFE0);
    cloudShape(ox, oy + 8, 0xC618);
  } else if (code == 95 || code >= 96) {  // storm: cloud + bolt
    cloudShape(ox, oy, 0x8410);
    ct->line(ox + 32, oy + 12, ox + 24, oy + 26, 0xFFE0);
    ct->line(ox + 24, oy + 26, ox + 30, oy + 26, 0xFFE0);
    ct->line(ox + 30, oy + 26, ox + 22, oy + 40, 0xFFE0);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {  // snow
    cloudShape(ox, oy, 0xC618);
    for (int i = 0; i < 3; i++) ct->rect(ox + 16 + i * 14, oy + 26 + (i & 1) * 6, 3, 3, 0xFFFF);
  } else if (code == 45 || code == 48) {  // fog: bars
    for (int i = 0; i < 3; i++) ct->rect(ox + 4 + (i & 1) * 10, oy + 4 + i * 12, 52, 4, 0x8410);
  } else {  // rain / drizzle / showers
    cloudShape(ox, oy, 0x8410);
    for (int i = 0; i < 3; i++)
      ct->line(ox + 16 + i * 14, oy + 26, ox + 10 + i * 14, oy + 38, 0x07FF);
  }
}

// Erase the whole icon box (icons differ in shape). Redraws when the code OR
// the day/night flag changes — steady weather costs zero pixels.
static int shownIsDay = -1;
static void drawWxIconUpd(int ox, int oy, int code) {
  if (code == shownCode && wIsDay == shownIsDay) return;
  ct->rect(ox - 8, oy - 10, 76, 56, 0x0000);
  drawWxIcon(ox, oy, code);
  shownCode = code;
  shownIsDay = wIsDay;
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
  char path[144];
  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,is_day&daily="
           "temperature_2m_max,temperature_2m_min&timezone=auto",
           cfg.lat, cfg.lon);
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
  // is_day arrives only when requested; a missing field keeps the last value
  // (day default) instead of forcing night on old cached payloads.
  if (!doc["current"]["is_day"].isNull()) wIsDay = doc["current"]["is_day"] | 1;
  wHi = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  wLo = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  haveWx = true;
  lastWx = millis();
  Serial.printf("[clock] wx %.1fC code=%d day=%d hi=%.0f lo=%.0f\n", wTemp, wCode, wIsDay, wHi,
                wLo);
}

// Full layout once (per entry): black fill + static geometry. After that,
// only textCUpd/drawWxIconUpd touch pixels — steady content = no drawing.
static void clockLayout() {
  ct->fill(0x0000);
  ct->rect(0, 216, TFT_W, 24, 0x0000);
  shownTime[0] = 0;
  shownDate[0] = 0;
  shownTemp[0] = 0;
  shownCond[0] = 0;
  shownCity[0] = 0;
  shownFoot[0] = 0;
  shownCode = -2;
  clockLaidOut = true;
}

// Partial repaint: every field erases + rewrites only when its text changed.
static void clockPaint() {
  if (!ct) return;
  if (!clockLaidOut) clockLayout();
  char line[32];
  if (haveTime) {
    struct tm tm;
    if (getLocalTime(&tm, 500)) {
      strftime(line, sizeof(line), "%H:%M", &tm);
      textCUpd(44, line, 0xFFFF, 6, shownTime, sizeof(shownTime));
      strftime(line, sizeof(line), "%a %d %b", &tm);
      for (char *p = line; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
      textCUpd(104, line, 0x07FF, 2, shownDate, sizeof(shownDate));
    } else {
      textCUpd(44, "--:--", 0x8410, 6, shownTime, sizeof(shownTime));
    }
  } else {
    textCUpd(44, "--:--", 0x8410, 6, shownTime, sizeof(shownTime));
    textCUpd(104, "SYNCING TIME...", 0xFFE0, 2, shownDate, sizeof(shownDate));
  }
  if (haveWx && wCode >= 0) {
    drawWxIconUpd(52, 132, wCode);
    snprintf(line, sizeof(line), "%.0fC", wTemp);
    // Temp field at fixed x (not centered): digits change width every update.
    if (strcmp(shownTemp, line) != 0) {
      ct->rect(140, 132, 120, 32, 0x0000);
      ct->text(140, 132, line, 0xFFFF, 4);
      strncpy(shownTemp, line, sizeof(shownTemp) - 1);
    }
    snprintf(line, sizeof(line), "%s %.0f/%.0f", wmoText(wCode), wHi, wLo);
    if (strcmp(shownCond, line) != 0) {
      ct->rect(140, 170, 160, 16, 0x0000);
      ct->text(140, 170, line, 0x07E0, 2);
      strncpy(shownCond, line, sizeof(shownCond) - 1);
    }
    if (strcmp(shownCity, cfg.city) != 0) {
      ct->rect(52, 192, 248, 8, 0x0000);
      ct->text(52, 192, cfg.city, 0x8410, 1);
      strncpy(shownCity, cfg.city, sizeof(shownCity) - 1);
    }
  } else if (wxFailed) {
    // Compact error: two lines, leaves the tap hint in the footer.
    if (strcmp(shownCond, "WXFAIL") != 0) {
      ct->rect(52, 140, 248, 44, 0x0000);
      ct->text(52, 140, "WEATHER FAILED", 0xF800, 2);
      ct->text(52, 168, "RETRYING...", 0xFFE0, 1);
      strncpy(shownCond, "WXFAIL", sizeof(shownCond) - 1);
      shownTemp[0] = 0;
      shownCode = -2;  // force icon redraw when weather recovers
    }
  } else {
    textCUpd(104, "SYNCING TIME...", 0xFFE0, 2, shownDate, sizeof(shownDate));
    if (strcmp(shownCond, "WXLOAD") != 0) {
      ct->rect(52, 150, 248, 16, 0x0000);
      ct->text(52, 150, "LOADING WEATHER...", 0xFFE0, 2);
      strncpy(shownCond, "WXLOAD", sizeof(shownCond) - 1);
    }
  }
  // Footer, same geometry as the other apps.
  const char *fs = clockStatus();
  if (strcmp(shownFoot, fs) != 0) {
    ct->rect(40, 222, 260, 8, 0x0000);
    ct->text(40, 222, fs, 0x07FF, 1);
    strncpy(shownFoot, fs, sizeof(shownFoot) - 1);
  }
}

void clockDraw() { clockPaint(); }

const char *clockStatus() {
  // Footer mirrors the crypto/stock apps: WiFi state, not the time — the big
  // HH:MM + city/condition lines already show time + place (the old footer
  // repeated "23:57 SINGAPORE 27C" redundantly).
  netStatus(footBuf, sizeof(footBuf));
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
  bool ntpDue = !haveTime || now - lastNtp > (haveTime ? 3600000UL : 30000UL);
  bool wxDue = !haveWx || now - lastWx > (haveWx ? 600000UL : 60000UL);
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

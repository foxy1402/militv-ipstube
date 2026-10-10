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
// Header slots: the city is left-aligned, the date right-aligns to HDR_R —
// NOT to the frame edge. A physical overlay tape on the glass covers roughly
// x>=290, and at scale 2 the date's last glyph is lit up to (right edge - 2),
// so aligning to FR=299 hid the final "T" of "SAT 10 OCT". HDR_R leaves the
// text clear of the tape while staying inside the x40..299 safe frame.
static const int HDR_R = 282;        // header right edge (date ends here)
static const int HDR_GAP = 8;        // blank gutter between the two slots
static const int DATE_W = 120;       // date slot "DDD DD MMM" = 10 chars @ scale 2
static const int CITY_W = HDR_R - DATE_W - HDR_GAP - FX + 1;  // city slot, x40..154

// WMO weather code -> text. Open-Meteo's `weather_code` is a number (0..99);
// it never returns these words, so the wording is ours — use the full WMO
// description rather than an abbreviation ("PARTLY CLOUDY", not "PARTLY CLDY").
// Switch on the exact codes: the old range checks fell through for gaps in the
// table (e.g. `c <= 48` labelled codes 4..44 "FOGGY").
static const char *wmoText(int c) {
  switch (c) {
    case 0: return "CLEAR";
    case 1: return "MAINLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45: return "FOG";
    case 48: return "RIME FOG";
    case 51: return "LIGHT DRIZZLE";
    case 53: return "DRIZZLE";
    case 55: return "HEAVY DRIZZLE";
    case 56:
    case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66:
    case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: return "LIGHT SHOWERS";
    case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: return "SNOW SHOWERS";
    case 86: return "HEAVY SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96:
    case 99: return "THUNDERSTORM HAIL";
    default: return "UNKNOWN";
  }
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

// Centered message that auto-fits the band: the erase band always uses the
// row's own scale (so switching between a value and a message on the same row
// erases the identical rectangle), while the text is drawn at the largest
// scale that fits the width. Without this a long status drawn at the temp
// row's scale 6 ("WEATHER FAILED" = 504 px in a 258 px band) silently
// truncated to "WEATHER".
static void fieldMsg(int y, uint8_t boxScale, const char *s, uint16_t color, char *shown,
                     size_t shownN) {
  if (strlen(s) > 39) return;
  if (strcmp(shown, s) == 0) return;
  ct->rect(FXI, y, FWI, 8 * boxScale, 0x0000);  // constant per row
  if (*s) {
    uint8_t sc = boxScale;
    while (sc > 1 && (int)strlen(s) * 6 * sc > FWI) sc--;
    int w = (int)strlen(s) * 6 * sc;
    int ty = y + (8 * boxScale - 8 * sc) / 2;  // vertically centered in the band
    ct->text(FXI + (FWI - w) / 2, ty, s, color, sc);
  }
  strncpy(shown, s, shownN - 1);
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

// Right-aligned field whose LAST drawn pixel lands on xR, with the erase box
// covering the whole slot [xR-clearW+1, xR]. Draw and erase must agree: an
// earlier version drew from xR-w while erasing from xR-clearW+1, so the
// leftmost column of a full-width value was drawn but never erased, and
// switching between 9- and 10-char dates ("SAT 9 OCT" / "SAT 10 OCT") left a
// 1 px remnant behind.
static void fieldRight(int xR, int y, const char *s, uint16_t color, uint8_t scale, char *shown,
                       size_t shownN, int clearW) {
  char buf[40];
  fit(buf, sizeof(buf), s, clearW, scale);
  if (strcmp(shown, buf) == 0) return;
  int w = (int)strlen(buf) * 6 * scale;
  int x = xR - w + 1;
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
      fieldRight(HDR_R, ROW_HEADER, line, 0x07FF, 2, shownDate, sizeof(shownDate), DATE_W);
    }
  }
  if (!timeOk) {
    fieldCenter(ROW_TIME, "--:--", 0x8410, 8, shownTime, sizeof(shownTime));
    fieldRight(HDR_R, ROW_HEADER, "", 0x07FF, 2, shownDate, sizeof(shownDate), DATE_W);
  }
  // City: header-left, same slot discipline as crypto's SYM.
  fieldLeft(FX, ROW_HEADER, cfg.city, 0x8410, 2, shownCity, sizeof(shownCity), CITY_W);

  if (haveWx && wCode >= 0) {
    snprintf(line, sizeof(line), "%.0fC", wTemp);
    fieldMsg(ROW_TEMP, 6, line, 0xFFFF, shownTemp, sizeof(shownTemp));
    fieldMsg(ROW_COND, 2, wmoText(wCode), 0x07E0, shownCond, sizeof(shownCond));
    if (timeOk) {
      snprintf(line, sizeof(line), "HI %.0f  LO %.0f", wHi, wLo);
      fieldMsg(ROW_HILO, 2, line, 0x8410, shownHiLo, sizeof(shownHiLo));
    } else {
      fieldMsg(ROW_HILO, 2, "SYNCING TIME", 0xFFE0, shownHiLo, sizeof(shownHiLo));
    }
  } else if (wxFailed) {
    // Same box scale as the value states: a smaller erase band here would
    // leave the bottom of the previous big temperature on screen when weather
    // went from loaded to failed.
    fieldMsg(ROW_TEMP, 6, "WEATHER FAILED", 0xF800, shownTemp, sizeof(shownTemp));
    fieldMsg(ROW_COND, 2, "RETRYING", 0xFFE0, shownCond, sizeof(shownCond));
    fieldMsg(ROW_HILO, 2, timeOk ? "" : "SYNCING TIME", 0xFFE0, shownHiLo, sizeof(shownHiLo));
  } else {
    fieldMsg(ROW_TEMP, 6, "LOADING WEATHER", 0xFFE0, shownTemp, sizeof(shownTemp));
    fieldMsg(ROW_COND, 2, "", 0x07E0, shownCond, sizeof(shownCond));
    fieldMsg(ROW_HILO, 2, timeOk ? "" : "SYNCING TIME", 0xFFE0, shownHiLo, sizeof(shownHiLo));
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

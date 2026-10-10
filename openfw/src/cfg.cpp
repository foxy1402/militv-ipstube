// See cfg.h for the design notes.
#include "cfg.h"
#include <Preferences.h>

AppCfg cfg;
bool cfgNeedReboot = false;

static Preferences pref;

static void defaults() {
  strncpy(cfg.ticker, "BTCUSDT", sizeof(cfg.ticker));
  cfg.gmtOff = 8;
  cfg.lat = 1.3521f;  // Singapore (user's timezone default)
  cfg.lon = 103.8198f;
  strncpy(cfg.city, "SINGAPORE", sizeof(cfg.city));
  strncpy(cfg.stock, "spy.us", sizeof(cfg.stock));
  cfg.app = 0;
}

void cfgLoad() {
  defaults();
  if (!pref.begin("militv", false)) return;
  pref.getString("ticker", cfg.ticker, sizeof(cfg.ticker));
  cfg.gmtOff = pref.getFloat("gmtoff", cfg.gmtOff);
  cfg.lat = pref.getFloat("lat", cfg.lat);
  cfg.lon = pref.getFloat("lon", cfg.lon);
  pref.getString("city", cfg.city, sizeof(cfg.city));
  pref.getString("stock", cfg.stock, sizeof(cfg.stock));
  cfg.app = pref.getUChar("app", cfg.app);
  pref.end();
  Serial.printf("[cfg] ticker=%s tz=%+.1f lat=%.2f lon=%.2f city=%s stock=%s app=%d\n", cfg.ticker,
                cfg.gmtOff, cfg.lat, cfg.lon, cfg.city, cfg.stock, cfg.app);
}

void cfgSave() {
  if (!pref.begin("militv", false)) return;
  pref.putString("ticker", cfg.ticker);
  pref.putFloat("gmtoff", cfg.gmtOff);
  pref.putFloat("lat", cfg.lat);
  pref.putFloat("lon", cfg.lon);
  pref.putString("city", cfg.city);
  pref.putString("stock", cfg.stock);
  pref.putUChar("app", cfg.app);
  pref.end();
  Serial.println("[cfg] saved");
}

void cfgSaveApp() {
  if (!pref.begin("militv", false)) return;
  pref.putUChar("app", cfg.app);
  pref.end();
}

// "BTCUSDT" -> "BTC" (strip the trailing USDT quote). Falls back to the raw
// string if it isn't a USDT pair.
void cfgBaseSym(char *out, size_t n) {
  size_t len = strlen(cfg.ticker);
  if (len > 4 && strcmp(cfg.ticker + len - 4, "USDT") == 0) len -= 4;
  if (len >= n) len = n - 1;
  for (size_t i = 0; i < len; i++) {
    char c = cfg.ticker[i];
    out[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
  }
  out[len] = 0;
}

// Portal fields (heap-allocated by WiFiManager convention: new, never freed,
// lives until reboot — the portal only runs at setup time anyway).
static WiFiManagerParameter *pTicker, *pTz, *pLat, *pLon, *pCity, *pStock;

void cfgPortalBind(WiFiManager &wm) {
  static char bTicker[16], bTz[16], bLat[12], bLon[12], bCity[24], bStock[16];
  snprintf(bTicker, sizeof(bTicker), "%s", cfg.ticker);
  snprintf(bTz, sizeof(bTz), "%+.1f", cfg.gmtOff);
  snprintf(bLat, sizeof(bLat), "%.4f", cfg.lat);
  snprintf(bLon, sizeof(bLon), "%.4f", cfg.lon);
  snprintf(bCity, sizeof(bCity), "%s", cfg.city);
  snprintf(bStock, sizeof(bStock), "%s", cfg.stock);
  pTicker = new WiFiManagerParameter("ticker", "Crypto ticker (e.g. ETHUSDT)", bTicker, 15);
  pTz = new WiFiManagerParameter("tz", "Timezone: 8, -5, 5.5, GMT+7, UTC-4:30", bTz, 15);
  pLat =
      new WiFiManagerParameter("lat", "Weather latitude (e.g. 1.3521 Singapore)", bLat, 11);
  pLon =
      new WiFiManagerParameter("lon", "Weather longitude (e.g. 103.8198)", bLon, 11);
  pCity = new WiFiManagerParameter("city", "City label (e.g. SINGAPORE)", bCity, 23);
  pStock = new WiFiManagerParameter("stock", "Stock (e.g. spy.us, aapl.us, qqq.us)", bStock,
                                    15);
  wm.addParameter(pTicker);
  wm.addParameter(pTz);
  wm.addParameter(pLat);
  wm.addParameter(pLon);
  wm.addParameter(pCity);
  wm.addParameter(pStock);
  wm.setSaveParamsCallback(cfgPortalSaved);
}

void cfgPortalSaved() {
  // Copy + validate. Anything malformed keeps the previous value.
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%s", pTicker->getValue());
  for (char *p = tmp; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  if (strlen(tmp) >= 6 && strlen(tmp) <= 12) {
    bool ok = true;
    for (char *p = tmp; *p; p++)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) ok = false;
    if (ok) {
      strncpy(cfg.ticker, tmp, sizeof(cfg.ticker) - 1);
      cfg.ticker[sizeof(cfg.ticker) - 1] = 0;
    }
  }
  // Timezone: accepts "8", "-5", "5.5", "GMT+7", "UTC-4:30", "+8:00".
  // Anything unparseable keeps the previous value.
  snprintf(tmp, sizeof(tmp), "%s", pTz->getValue());
  for (char *p = tmp; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  const char *t = tmp;
  while (*t == ' ' || *t == '\t') t++;
  // Skip optional GMT/UTC prefix, then optional second G/U letter.
  if ((t[0] == 'G' || t[0] == 'U') && (t[1] == 'M' || t[1] == 'T' || t[1] == 'C')) t += 2;
  if (*t == 'G' || *t == 'U') t++;
  while (*t == ' ' || *t == '\t') t++;
  float tz = 0;
  bool tzOk = false;
  if (*t == '+' || *t == '-' || (*t >= '0' && *t <= '9')) {
    char *end = nullptr;
    float h = strtof(t, &end);
    tz = h;
    tzOk = true;
    if (end && *end == ':') {
      float m = strtof(end + 1, nullptr);
      tz = (h < 0 ? h - m / 60.0f : h + m / 60.0f);
    }
  }
  if (tzOk && tz >= -12 && tz <= 14) cfg.gmtOff = tz;
  float la = atof(pLat->getValue()), lo = atof(pLon->getValue());
  if (la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
    cfg.lat = la;
    cfg.lon = lo;
  }
  snprintf(tmp, sizeof(tmp), "%s", pCity->getValue());
  for (char *p = tmp; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  if (strlen(tmp) > 0) {
    strncpy(cfg.city, tmp, sizeof(cfg.city) - 1);
    cfg.city[sizeof(cfg.city) - 1] = 0;
  }
  snprintf(tmp, sizeof(tmp), "%s", pStock->getValue());
  for (char *p = tmp; *p; p++)
    if (*p >= 'A' && *p <= 'Z') *p += 32;
  if (strlen(tmp) >= 4 && strlen(tmp) <= 12) {
    strncpy(cfg.stock, tmp, sizeof(cfg.stock) - 1);
    cfg.stock[sizeof(cfg.stock) - 1] = 0;
  }
  cfgSave();
  // Ticker/WS path changes need a clean restart of networking, so reboot.
  cfgNeedReboot = true;
}

// Persistent app settings for the MiliTV open firmware (NVS "militv").
//
// Why a separate store: WiFiManager owns the WiFi creds in its own NVS keys.
// Our app settings (ticker, timezone, weather location, stock, last app)
// live under OUR namespace, so opening the portal to tweak an app setting
// can never wipe the saved AP — the portal only overwrites WiFi creds when
// the user actually enters a new SSID/password. Leaving those fields blank
// keeps the current network; our save-callback only writes the fields below.
#pragma once
#include <Arduino.h>
#include <WiFiManager.h>

struct AppCfg {
  char ticker[16];  // crypto pair, e.g. "BTCUSDT" (USDT spot only)
  float gmtOff;     // clock offset from UTC, hours (e.g. 8, -5, 5.5)
  float lat, lon;   // weather location (Open-Meteo, no key needed)
  char city[24];    // label shown on the clock screen
  char stock[16];   // stooq id, e.g. "spy.us" (S&P500) or "aapl.us"
  uint8_t app;      // last active app: 0 crypto, 1 clock, 2 stock
};

extern AppCfg cfg;
extern bool cfgNeedReboot;  // set by portal save: main reboots to apply

void cfgLoad();                    // read NVS (or write defaults first boot)
void cfgSave();                    // persist everything
void cfgSaveApp();                 // persist just the app index
void cfgBaseSym(char *out, size_t n);  // "BTCUSDT" -> "BTC"

// WiFiManager portal wiring: registers text fields on the setup page and
// installs the save callback. Called from netBegin() before autoConnect or
// startConfigPortal, so params exist in both paths.
void cfgPortalBind(WiFiManager &wm);
// Save callback: validates portal fields into cfg + NVS. Never touches WiFi.
void cfgPortalSaved();

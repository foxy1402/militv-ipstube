// WiFi provisioning + reconnect watchdog for the MiliTV open firmware.
//
// Provisioning: WiFiManager captive portal, button-triggered on demand.
//   Normal boot: autoConnect() joins NVS-saved creds (~3 s, no interaction).
//   POWER held 3 s (at boot, or any time while running): startConfigPortal("MILITV-SETUP") —
//     join that AP from phone/laptop, browser opens, pick SSID, done.
//   The portal page also carries ALL app settings (ticker/tz/weather/stock)
//     via cfgPortalBind. Save => settings persist + reboot applies.
//     "Exit Setup" (tap POWER, or 'x' on serial, or 120 s timeout) aborts
//     WITHOUT rebooting, so a stray entry never disrupts the running apps.
// This replaces the stock BLE-app provisioning (NimBLE UART service);
// nothing else in the firmware depends on BLE.
//
// Reconnect (the stock drop-bug fix): stock has no reconnect handler or
// watchdog anywhere in its image — hence "dead after hours, never retries".
// v3 does: modem sleep off, auto-reconnect on, disconnect/LOST_IP events,
// and a 15 s watchdog that does a full disconnect()+begin() (stronger than
// reconnect() after a router reboot or channel change).
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#define PORTAL_AP_NAME "MILITV-SETUP"
#define NET_RETRY_MS 15000

struct NetState {
  WiFiManager wm;
  bool portalArmed = false;  // set while the setup portal is running
  uint32_t portalStartMs = 0;  // millis() when the portal opened (120 s idle cap)
  bool connected = false;
  bool dhcpLost = false;
  uint32_t lostAt = 0;
  uint32_t lastRetry = 0;
  char ssid[33] = {0};
  int8_t rssi = 0;
};

extern NetState net;

// Call from setup() AFTER buttons are snapshotted: powerHeld = POWER key
// reads pressed at boot. Returns true if STA is up.
bool netBegin(bool powerHeld);

// Start the setup portal at runtime (POWER held 3 s while running).
// Safe to call when already in portal (no-op). Stops app traffic first.
void netPortalStart();

// Abort the portal WITHOUT saving: stop AP, resume STA, redraw caller.
void netPortalAbort();

// Call every loop(): pumps portal (if armed) + watchdog. Returns true while
// the portal is running (caller should skip WS/chart work meanwhile).
bool netPump(uint32_t now);

// One-line status for screen footer / serial, e.g. "WIFI BTC-AP -61dBm".
void netStatus(char *out, size_t n);

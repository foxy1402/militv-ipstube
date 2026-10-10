// See netcfg.h for the design notes.
#include "netcfg.h"
#include "cfg.h"
#include "pins.h"  // MILITV_BLE feature switch

NetState net;

static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      net.connected = true;
      net.dhcpLost = false;
      strncpy(net.ssid, WiFi.SSID().c_str(), sizeof(net.ssid) - 1);
      net.rssi = (int8_t)WiFi.RSSI();
      Serial.printf("[net] GOT_IP %s rssi=%d\n", WiFi.localIP().toString().c_str(), net.rssi);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (net.connected) Serial.println("[net] STA disconnected — watchdog will retry");
      net.connected = false;
      net.lostAt = millis();
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      // Stale DHCP lease: status may still claim "connected" but no traffic
      // flows. Treat like a disconnect so the watchdog re-begins.
      Serial.println("[net] LOST_IP — flagging for re-begin");
      net.dhcpLost = true;
      break;
    default:
      break;
  }
}

bool netBegin(bool powerHeld) {
  WiFi.mode(WIFI_STA);
  // Modem sleep was disabled for 24/7 streaming stability (the stock drop-bug
  // fix). But on the ESP32-S3 WiFi and Bluetooth share one radio, and the WiFi
  // driver hard-aborts during coexistence setup if modem sleep is off while BT
  // is enabled — a boot loop, not a warning. So when the BLE console is built
  // in, sleep has to stay on. The reconnect watchdog below is what actually
  // keeps the link alive; sleep only adds a small latency in the worst case.
#if MILITV_BLE
  WiFi.setSleep(true);  // required for WiFi+BLE coexistence
#else
  WiFi.setSleep(false);  // BLE compiled out: keep the lower-latency setting
#endif
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  net.wm.setConfigPortalTimeout(120);
  net.wm.setConnectTimeout(10);
  // Keep the portal from hijacking a normal boot: only auto-join here.
  net.wm.setEnableConfigPortal(false);
  net.portalArmed = false;
  // App settings fields (ticker/tz/weather/stock) ride on the same setup
  // page via cfgPortalBind. Saving them never touches WiFi creds: WiFiManager
  // only overwrites the AP when new SSID/password fields are entered.
  cfgPortalBind(net.wm);

  if (powerHeld) {
    Serial.println("[net] POWER held at boot — opening setup portal");
    netPortalStart();
    return false;
  }

  Serial.println("[net] auto-joining saved WiFi...");
  bool ok = net.wm.autoConnect();
  net.connected = (WiFi.status() == WL_CONNECTED);
  if (net.connected) {
    strncpy(net.ssid, WiFi.SSID().c_str(), sizeof(net.ssid) - 1);
    net.rssi = (int8_t)WiFi.RSSI();
    Serial.printf("[net] joined %s rssi=%d\n", net.ssid, net.rssi);
  } else {
    Serial.println("[net] no saved WiFi — hold POWER at boot for setup portal");
    net.lastRetry = millis();
  }
  return net.connected;
}

// Runtime + boot portal entry. Non-blocking: caller must keep calling
// netPump() and must skip app traffic while portalArmed. Snapshot STA state
// so abort can only resume (never wipe) the previous connection.
void netPortalStart() {
  if (net.portalArmed) return;  // already there
  Serial.println("[net] opening setup portal MILITV-SETUP");
  net.wm.setEnableConfigPortal(true);
  net.wm.setConfigPortalBlocking(false);
  net.wm.startConfigPortal(PORTAL_AP_NAME);
  net.portalArmed = true;
  net.portalStartMs = millis();
  net.connected = (WiFi.status() == WL_CONNECTED);
}

// Abort WITHOUT saving: stop the AP + webserver, drop back to STA on the
// previously-saved creds (untouched — abort writes nothing to NVS).
void netPortalAbort() {
  if (!net.portalArmed) return;
  Serial.println("[net] portal aborted — resuming apps");
  net.wm.stopConfigPortal();
  net.wm.setEnableConfigPortal(false);
  net.portalArmed = false;
  net.portalStartMs = 0;
  // Resume STA on saved creds; the watchdog re-begins if it takes a moment.
  WiFi.mode(WIFI_STA);
  WiFi.begin();
}

bool netPump(uint32_t now) {
  if (net.portalArmed) {
    net.wm.process();
    // Save path: the save-callback sets cfgNeedReboot. Wait until the portal
    // has actually joined STA (it flips us over on save), then reboot once —
    // this applies new WiFi AND new app settings atomically. The reboot is
    // issued from main's loop (see cfgNeedReboot handling), not from inside
    // the WiFiManager callback.
    // Timeout path: 120 s with no save => abort silently and resume apps.
    // (Non-blocking portal has no built-in timeout; we enforce our own.
    // Guard now>=stamp: the stamp is taken with millis() INSIDE the same
    // loop pass that captured `now` at its top, so stamp can exceed `now`
    // by a few ms — without the guard the subtraction underflows and the
    // portal aborts on its very first pump.
    if (net.portalStartMs && now >= net.portalStartMs && now - net.portalStartMs > 120000UL) {
      Serial.println("[net] portal idle 120s — auto exit, resuming apps");
      netPortalAbort();
      return false;
    }
    // Portal library flips us to STA when the user saves creds.
    if (WiFi.status() == WL_CONNECTED && !net.connected) {
      net.connected = true;
      strncpy(net.ssid, WiFi.SSID().c_str(), sizeof(net.ssid) - 1);
      net.rssi = (int8_t)WiFi.RSSI();
      Serial.printf("[net] portal saved, joined %s — rebooting to apply\n", net.ssid);
      // Disarm FIRST so the reboot check in main's loop fires even if the AP
      // iface lingers a pass: cfgNeedReboot + !portalArmed => ESP.restart().
      net.portalArmed = false;
      net.portalStartMs = 0;
    }
    // Abort path (tap POWER / serial 'x' / timeout) clears portalArmed via
    // netPortalAbort(); the save path disarms above, then main reboots.
    return net.portalArmed;
  }

  if (WiFi.status() == WL_CONNECTED) net.rssi = (int8_t)WiFi.RSSI();
  if ((WiFi.status() != WL_CONNECTED || net.dhcpLost) && (now - net.lastRetry > NET_RETRY_MS)) {
    net.lastRetry = now;
    net.dhcpLost = false;
    Serial.println("[net] watchdog re-begin...");
    WiFi.disconnect();
    delay(200);
    // Persistent creds were saved by the portal; re-begin with no args.
    WiFi.begin();
  }
  return false;
}

void netStatus(char *out, size_t n) {
  if (net.portalArmed) {
    snprintf(out, n, "SETUP AP: %s", PORTAL_AP_NAME);
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(out, n, "WIFI %s %ddBm", net.ssid, net.rssi);
  } else {
    snprintf(out, n, "WIFI DOWN retrying");
  }
}

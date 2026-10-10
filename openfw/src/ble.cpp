// See ble.h for the design notes.
#include "ble.h"

#if MILITV_BLE

#include <NimBLEDevice.h>

// Recovered from the stock firmware (morepins.txt): the Nordic UART Service.
// Keeping the same UUIDs means generic BLE-UART apps work out of the box.
#define BLE_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // central -> device
#define BLE_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // device -> central

#define BLE_QUEUE_LEN 4

static NimBLEServer *sServer = nullptr;
static NimBLECharacteristic *sTx = nullptr;
static volatile bool sConnected = false;
static BleCommandHandler sHandler = nullptr;
static char sName[24] = "MILITV";

// BLE task -> loop task handoff. The mailbox holds whole command lines (or
// explicit drops when full), so the loop task never blocks on the BLE host.
struct BleMsg {
  char line[BLE_LINE_MAX];
};
static QueueHandle_t sRX = nullptr;
static char sCur[BLE_LINE_MAX];
static size_t sCurN = 0;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, ble_gap_conn_desc *desc) override {
    sConnected = true;
    Serial.printf("[ble] central connected (handle %d)\n", desc->conn_handle);
  }
  void onDisconnect(NimBLEServer *s) override {
    sConnected = false;
    Serial.println("[ble] central disconnected — re-advertising");
    // Keep the device discoverable after the phone walks away.
    NimBLEDevice::startAdvertising();
  }
};

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    for (char ch : v) {
      if (ch == '\r') continue;
      if (ch == '\n') {
        if (sCurN == 0) continue;
        sCur[sCurN] = 0;
        // Copy into the queue; drop if the loop task is behind (never block).
        BleMsg m;
        strncpy(m.line, sCur, BLE_LINE_MAX - 1);
        m.line[BLE_LINE_MAX - 1] = 0;
        if (xQueueSend(sRX, &m, 0) != pdTRUE) Serial.println("[ble] rx queue full, dropped");
        sCurN = 0;
      } else if (sCurN < BLE_LINE_MAX - 1) {
        sCur[sCurN++] = ch;
      } else {
        sCurN = 0;  // overlong line: discard
      }
    }
  }
};

void bleSetHandler(BleCommandHandler h) { sHandler = h; }

bool bleBegin(const char *deviceName) {
  if (deviceName && *deviceName) {
    strncpy(sName, deviceName, sizeof(sName) - 1);
    sName[sizeof(sName) - 1] = 0;
  }
  sRX = xQueueCreate(BLE_QUEUE_LEN, sizeof(BleMsg));
  if (!sRX) return false;

  NimBLEDevice::init(sName);
  // Personal device: an open UART service is intentional (matches the stock
  // behavior and every generic BLE-UART example). Power kept modest.
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  sServer = NimBLEDevice::createServer();
  if (!sServer) return false;
  sServer->setCallbacks(new ServerCB());

  NimBLEService *svc = sServer->createService(BLE_SVC_UUID);
  sTx = svc->createCharacteristic(BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx = svc->createCharacteristic(BLE_RX_UUID, NIMBLE_PROPERTY::WRITE);
  rx->setCallbacks(new RxCB());
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setName(sName);
  adv->start();
  Serial.printf("[ble] advertising as \"%s\" (NUS %s)\n", sName, BLE_SVC_UUID);
  return true;
}

void bleAdvertise() {
  if (sServer && !sConnected) NimBLEDevice::startAdvertising();
}

bool bleConnected() { return sConnected; }

void bleNotify(const char *line) {
  if (!sConnected || !sTx) return;
  sTx->setValue((uint8_t *)line, strlen(line));
  sTx->notify();
}

void blePump() {
  if (!sRX) return;
  BleMsg m;
  // Drain everything queued this pass so a burst of commands is not throttled
  // to one per loop.
  while (xQueueReceive(sRX, &m, 0) == pdTRUE) {
    char resp[BLE_LINE_MAX];
    resp[0] = 0;
    if (sHandler) sHandler(m.line, resp, sizeof(resp));
    if (resp[0]) {
      // Reply is newline-terminated for line-based clients.
      size_t n = strlen(resp);
      if (n < sizeof(resp) - 1) {
        resp[n] = '\n';
        resp[n + 1] = 0;
      }
      bleNotify(resp);
    }
  }
}

#else  // MILITV_BLE == 0

bool bleBegin(const char *) { return false; }
void bleSetHandler(BleCommandHandler) {}
void blePump() {}
bool bleConnected() { return false; }
void bleNotify(const char *) {}
void bleAdvertise() {}

#endif

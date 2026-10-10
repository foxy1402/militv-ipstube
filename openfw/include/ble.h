// Bluetooth LE console for the MiliTV open firmware.
//
// The stock firmware provisioned itself over BLE with the Nordic UART Service
// (service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E, RX 6E400002/TX 6E400003 — the
// UUIDs are recovered evidence, see morepins.txt in the RE artifacts). We do
// NOT replicate the stock's proprietary phone protocol, but we keep the SAME
// service so any generic "BLE UART" app (nRF Connect, Serial Bluetooth
// Terminal, the Arduino BLE examples) can talk to the device. WiFi setup stays
// on the captive portal; BLE is a live control/telemetry channel that also
// works when the device is out of WiFi range.
//
// Threading: the NimBLE callbacks run on the BLE host task. They must not
// touch the framebuffer, NVS or the network, so received lines are copied into
// a FreeRTOS queue and executed later by blePump() on the loop task. Replies
// are sent from the loop task as notifications.
//
// Protocol: newline-terminated ASCII commands, newline-terminated replies.
#pragma once
#include <Arduino.h>

// Max length of a command line or reply, including the terminating NUL. Shared
// with main's serial console so both transports use the same buffer size.
#define BLE_LINE_MAX 160

// Handler signature: given a command line, write a reply into resp.
// Registered by main so the command set can reach every app/subsystem.
typedef void (*BleCommandHandler)(const char *line, char *resp, size_t respN);

// Start BLE + advertising. Sealed with the device name from cfg (or a
// default). Returns false if the stack could not start.
bool bleBegin(const char *deviceName);

// Register the command handler. Call before bleBegin() ideally, but any time
// before the first command is fine.
void bleSetHandler(BleCommandHandler h);

// Drain received commands and send replies. Call from loop().
void blePump();

// True while a central is connected (used for the on-screen indicator).
bool bleConnected();

// Push an unsolicited line to the connected central (no-op when disconnected).
void bleNotify(const char *line);

// Restart advertising so a disconnected phone can reconnect. Also called
// automatically on disconnect.
void bleAdvertise();

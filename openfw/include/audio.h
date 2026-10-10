// I2S audio for the MiliTV open firmware.
//
// The stock board drives a small speaker through an external I2S amplifier
// (the factory firmware shipped ESP32-audioI2S for mp3/aac/wav alarm playback).
// We do NOT pull in a full MP3 decoder: this module is a small, dependency-free
// tone engine, which is what the apps actually need (price alerts, key clicks,
// a boot chime). It also gives the community a working I2S bring-up to build a
// real decoder on top of.
//
// Hardware (pin evidence in pins.h):
//   BCK  = GPIO12   bit clock
//   WS   = GPIO11   word select / LRCK
//   DOUT = GPIO13   serial data to the amplifier
//   AMP_EN (optional) = amplifier shutdown/enable, defined in pins.h
//
// Non-blocking by design, matching the rest of the firmware: audioTone()
// enqueues and returns immediately, and audioPump() feeds the I2S DMA in
// small slices from loop(). A tone never blocks the key scan or a fetch.
#pragma once
#include <Arduino.h>

// Bring up I2S + the amplifier. Safe to call when audio is disabled (no-op).
// Returns false if the I2S driver could not be installed.
bool audioBegin();

// Queue a tone. Replaces any tone already playing (alerts should not stack).
// freqHz 0 = silence. ms = duration; 0 means "until replaced or stopped".
void audioTone(uint16_t freqHz, uint16_t ms);

// Convenience: the standard alert (two rising beeps) — used by price alerts.
void audioAlert();

// One short click, for UI feedback.
void audioClick();

// Stop immediately (also releases the amp enable pin, if any).
void audioStop();

// Drive the DMA. Call from loop(); cheap when idle (a single comparison).
void audioPump();

// True while a tone is still being emitted.
bool audioBusy();

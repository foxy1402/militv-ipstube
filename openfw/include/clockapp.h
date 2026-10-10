// Clock + weather app for the MiliTV open firmware.
//
// Clock: NTP (pool.ntp.org + fallbacks), offset = cfg.gmtOff hours. Sync is
// split across loop() passes (start = configTime, finish = bounded poll) and
// staggered against the weather fetch, so entering the app or tapping keys
// never blocks — the "WEATHER FAILED trapped my buttons" bug was an inline
// fetch inside the tap handler.
// Weather: Open-Meteo forecast API, free, no key: current temp + weathercode
// + daily high/low for cfg.lat/lon. Refresh every 10 min (60 s after a
// failure). Weathercode -> icon drawn with line/rect primitives.
// Drawing: one full layout per entry, then partial repaints — each field
// erases + rewrites only when its text changes, so steady content costs zero
// pixels (no blink). No "CLOCK" header: the time is the title.
#pragma once
#include <Arduino.h>

void clockEnter();          // NTP sync + first weather fetch + full draw
void clockExit();           // nothing to tear down (stateless HTTP)
void clockPump();           // hourly NTP + 10 min weather + minute redraw
void clockDraw();           // full screen
const char *clockStatus();  // footer line, e.g. "14:32 SIN 31C"

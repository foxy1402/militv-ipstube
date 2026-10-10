// Single-stock tracker for the MiliTV open firmware.
//
// Data: Yahoo Finance v8 chart API, free, no key:
//   https://query1.finance.yahoo.com/v8/finance/chart/SPY?interval=1d&range=1mo
// One call returns the live quote (regularMarketPrice), the previous close,
// the market-hours window (currentTradingPeriod.regular), and ~23 daily
// closes for the sparkline (probed 2026-10-09: HTTP 200, ~3.6 KB, plain
// HTTPS + browser UA — Yahoo 403s the default ESP32 UA).
// Outside market hours the quote is the last close and the screen says
// MARKET CLOSED — data is still correct, just not moving. Market-hours come
// from the server's own clock (regularMarketTime inside the regular session),
// so no NTP dependency.
// Refresh: one request every 60 s. Identity: cfg.stock ("spy.us", "SPY",
// "aapl.us" all work — the ".us" suffix is stripped).
// Non-blocking UI: enter() draws instantly (cached or LOADING); the fetch
// runs on the next pump() so a tap never freezes the buttons. A failed fetch
// shows FETCH FAILED + "TAP + OR - TO LEAVE" and retries every 60 s.
#pragma once
#include <Arduino.h>

void stockEnter();          // quote + history fetch + full draw
void stockExit();           // nothing to tear down (stateless HTTP)
void stockPump();           // 60 s quote / 30 min history refresh
void stockDraw();           // full screen
const char *stockStatus();  // footer line

// On-flash asset storage (LittleFS) for the MiliTV open firmware.
//
// The device carries an 11 MB LittleFS partition — the factory firmware filled
// it with album art, boot animations and alarm sounds, and the open firmware
// previously ignored all of it. This module mounts it and exposes a small read
// API, which is the hook the community needs: drop files into `data/` and they
// are uploaded with `pio run -t uploadfs`, no recompile of the program logic.
//
// Intended uses (see README):
//   /fonts/*.fnt   extra/alternate glyph fonts
//   /icons/*.bin   weather / UI icons
//   /sounds/*.pcm  raw 16-bit PCM clips for the tone engine to play
//   /config/*.txt  user-tunable text
//
// Nothing here is required for the built-in apps: if the partition is empty or
// corrupt the firmware runs exactly as before, just without extra assets.
#pragma once
#include <Arduino.h>

// Mount LittleFS. Safe to call when already mounted. Returns false if the
// filesystem is missing/corrupt (mount failure is formatted only on demand —
// see assetsFormat()).
bool assetsBegin();

// Total / used bytes on the asset partition (0 when not mounted).
size_t assetsTotalBytes();
size_t assetsUsedBytes();

// Read a whole file into a PSRAM buffer (caller frees with assetsFree).
// Returns nullptr when missing. *outLen receives the size.
uint8_t *assetsLoad(const char *path, size_t *outLen);

// Free a buffer from assetsLoad.
void assetsFree(uint8_t *p);

// True if the path exists.
bool assetsExists(const char *path);

// One-line status for serial / BLE, e.g. "FS 46/11264KB 12 files".
void assetsStatus(char *out, size_t n);

// Erase and reformat the asset partition. DESTRUCTIVE — only called from an
// explicit operator action (portal/serial/BLE), never automatically.
bool assetsFormat();

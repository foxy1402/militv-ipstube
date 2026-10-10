// See assets.h for the design notes.
#include "assets.h"

#include <LittleFS.h>

static bool sUp = false;

bool assetsBegin() {
  if (sUp) return true;
  // formatOnFail=false: a mount failure must never silently wipe the
  // community's uploaded assets. Formatting is explicit (assetsFormat()).
  if (!LittleFS.begin(false, "/littlefs")) {
    Serial.println("[fs] LittleFS mount failed (assets disabled)");
    return false;
  }
  sUp = true;
  Serial.printf("[fs] LittleFS mounted: %u/%u KB used\n", (unsigned)(LittleFS.usedBytes() / 1024),
                (unsigned)(LittleFS.totalBytes() / 1024));
  return true;
}

size_t assetsTotalBytes() { return sUp ? LittleFS.totalBytes() : 0; }
size_t assetsUsedBytes() { return sUp ? LittleFS.usedBytes() : 0; }

bool assetsExists(const char *path) { return sUp && LittleFS.exists(path); }

uint8_t *assetsLoad(const char *path, size_t *outLen) {
  if (outLen) *outLen = 0;
  if (!sUp) return nullptr;
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  size_t n = f.size();
  // Assets are user-supplied, so cap the allocation: a corrupt size field must
  // not ask PSRAM for gigabytes.
  if (n == 0 || n > 4 * 1024 * 1024) {
    f.close();
    return nullptr;
  }
  uint8_t *buf = (uint8_t *)ps_malloc(n);
  if (!buf) {
    f.close();
    return nullptr;
  }
  size_t got = f.read(buf, n);
  f.close();
  if (got != n) {
    free(buf);
    return nullptr;
  }
  if (outLen) *outLen = n;
  return buf;
}

void assetsFree(uint8_t *p) {
  if (p) free(p);
}

bool assetsFormat() {
  Serial.println("[fs] formatting asset partition (destructive)");
  bool ok = LittleFS.format();
  if (ok) {
    LittleFS.end();
    sUp = false;
    ok = assetsBegin();
  }
  Serial.printf("[fs] format %s\n", ok ? "ok" : "FAILED");
  return ok;
}

void assetsStatus(char *out, size_t n) {
  if (!sUp) {
    snprintf(out, n, "FS not mounted");
    return;
  }
  File root = LittleFS.open("/");
  int files = 0;
  if (root && root.isDirectory()) {
    File e = root.openNextFile();
    while (e) {
      if (!e.isDirectory()) files++;
      e = root.openNextFile();
    }
  }
  snprintf(out, n, "FS %d files %u/%uKB", files, (unsigned)(LittleFS.usedBytes() / 1024),
           (unsigned)(LittleFS.totalBytes() / 1024));
}

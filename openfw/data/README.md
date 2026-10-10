# Asset partition contents

Files here are uploaded to the device's 11 MB LittleFS partition with:

```bat
pio run -t uploadfs
```

They are **optional** — the firmware runs fine with an empty filesystem. This is
the extension point for community content, so the program logic never has to be
recompiled to add assets.

Suggested layout (nothing here is implemented yet — pick one up and send a PR):

```
data/
  fonts/      alternate glyph fonts (see the wanted-features list)
  icons/      extra weather / UI icons
  sounds/     raw 16-bit mono PCM clips for the tone engine
  config/     user-tunable text
```

Notes:

- LittleFS is flat-ish: keep names short and avoid special characters.
- The device mounts at `/littlefs`; `assetsLoad()` in `src/assets.cpp` reads a
  whole file into PSRAM (4 MB cap) and hands you a pointer.
- The factory firmware used this partition for album art and boot animations.
  Uploading your own files replaces that content — it is *your* partition now,
  and the stock files are preserved in your full-flash backup.

If you do not want to ship any assets, delete the placeholder `.gitkeep` files
and PlatformIO will still build (the uploadfs target just has nothing to send).

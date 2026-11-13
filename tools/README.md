# tools/

Helper scripts for the MiliTV open firmware.

## Font atlas generator

`militv-font-atlas-gen.py` renders a TTF into the 4-bit-alpha bitmap atlas
headers used by `MiniTFT::textAA()` / `textAA28()` in `openfw/include/tft.h`
(`roboto_mono_20.h` / `roboto_mono_28.h`).

The font file itself is NOT vendored here, on purpose — no binary blobs in
git. Get Roboto Mono (SIL Open Font License 1.1, free) from Google Fonts and
place `RobotoMono.ttf` next to the script, then run from the repo root:

```sh
python3 tools/militv-font-atlas-gen.py tools/RobotoMono.ttf 20 openfw/include/roboto_mono_20.h
python3 tools/militv-font-atlas-gen.py tools/RobotoMono.ttf 28 openfw/include/roboto_mono_28.h
```

Requires Python 3 + Pillow (`pip install pillow`).

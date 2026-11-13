# tools/

Helper scripts for the MiliTV open firmware.

## Font atlas generator

`militv-font-atlas-gen.py` renders a TTF into the 4-bit-alpha bitmap atlas
headers that `MiniTFT::textAA()` draws from (`openfw/include/tft.h`). One size
per old block-font scale step, so a call site keeps its x/y:

| size | advance | replaces          | header                  | symbol     |
| ---- | ------- | ----------------- | ----------------------- | ---------- |
| 10   | 6 px    | `text(..., scale 1)` | `aa_roboto_10.h`     | `kAASmall` |
| 20   | 12 px   | `text(..., scale 2)` | `aa_roboto_20.h`     | `kAAMed`   |
| 40   | 24 px   | `text(..., scale 4)` | `aa_roboto_40.h`     | `kAATitle` |
| 60   | 36 px   | `text(..., scale 6)` | `aa_roboto_60.h`     | `kAABig`   |
| 80   | 48 px   | `text(..., scale 8)` | `aa_roboto_80.h`     | `kAAHuge`  |

The advance is exactly `0.6 * px` for Roboto Mono, so `size = 10 * scale` lands
on the block font's `6 * scale` advance. That is what makes the swap
layout-preserving: existing slot widths and x anchors stay correct.

Each header also records the ink extent of the font (`ink_top` / `ink_bot`,
relative to the cap-top anchor) because the AA ink is taller than the old
`8 * scale` cell — at 20 px a `$` starts 2 px above the caps and a `,` reaches
19 px below them. Erase boxes must cover the ink, not the cell, or the previous
value's tail stays on screen. Use `aaBandH()` / `aaEraseBand()` from
`openfw/include/aafont.h` rather than hardcoding a height.

The font file itself is NOT vendored, on purpose — no binary blobs in git. Get
Roboto Mono (SIL Open Font License 1.1, free) from Google Fonts, place
`RobotoMono.ttf` somewhere outside the repo, and run from the repo root:

```sh
TTF=/path/to/RobotoMono.ttf
python3 tools/militv-font-atlas-gen.py $TTF 10 openfw/include/aa_roboto_10.h kAASmall
python3 tools/militv-font-atlas-gen.py $TTF 20 openfw/include/aa_roboto_20.h kAAMed
python3 tools/militv-font-atlas-gen.py $TTF 40 openfw/include/aa_roboto_40.h kAATitle
python3 tools/militv-font-atlas-gen.py $TTF 60 openfw/include/aa_roboto_60.h kAABig
python3 tools/militv-font-atlas-gen.py $TTF 80 openfw/include/aa_roboto_80.h kAAHuge
```

Requires Python 3 + Pillow (`pip install pillow`). The generated atlas data is
derived from Roboto Mono and therefore carries the OFL: keep
`openfw/include/OFL.txt` with the headers when redistributing.

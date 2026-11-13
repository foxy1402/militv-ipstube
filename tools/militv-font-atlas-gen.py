#!/usr/bin/env python3
"""Generate an anti-aliased 4-bit-alpha bitmap font atlas header from a TTF.

Usage:
    python3 militv-font-atlas-gen.py RobotoMono.ttf 20 aa_roboto_20.h kAAMed

Why this exists: MiliTV's MiniTFT used to render only a 5x7 bitmap font scaled
in whole blocks, so every glyph edge was a hard pixel step. This script does the
anti-aliasing offline, once, with PIL: each glyph is rendered at the target
pixel size into a grayscale image, quantised to 4-bit alpha and packed 2
pixels/byte. The firmware then just alpha-blends the atlas into its PSRAM
framebuffer (see MiniTFT::textAA) - no TTF parser on the device, no extra RAM
at runtime beyond the atlas itself in flash.

SIZE SELECTION: the atlas is rasterised at `px` pixels and Roboto Mono's
advance is 0.6*px, so a size of 10*s reproduces the old bitmap font's
`text(..., scale=s)` advance exactly:

    size 10 -> 6 px  == old scale 1   (footers, chart axis labels)
    size 20 -> 12 px == old scale 2   (headers, banners)
    size 40 -> 24 px == old scale 4   (portal / boot titles)
    size 60 -> 36 px == old scale 6   (clock temperature)
    size 80 -> 48 px == old scale 8   (clock HH:MM)

That is why a call site keeps its x/y: the erase boxes and slot widths computed
for the blocky font stay valid after the swap.

Output header format (all atlases share one struct, so the firmware has a
single textAA implementation rather than one per size):
    AA_FIRST / AA_COUNT          first char code, glyph count
    struct AAFont { first, count, cap_top, ascent, adv, ink_top, ink_bot,
                    glyphs, bitmap }
        cap_top - yoff of 'A', i.e. the ink top of the cap-height band
        ascent  - font ascent in px (used for newline advance)
        adv     - the monospace advance (constant by construction)
        ink_top - topmost ink of ANY glyph, relative to the cap-top anchor.
                  Negative: '$', '(', '|' etc. overshoot the caps.
        ink_bot - bottommost ink of any glyph, relative to the same anchor
                  (includes descenders and the ',' comma tail).
    struct AAGlyph { adv, w, h; xoff, yoff; off }:
        w,h  - ink-box size in px
        xoff - ink left edge relative to the glyph origin
        yoff - ink top edge relative to the font ascender line
        off  - byte offset of the glyph's packed nibbles in the bitmap
                 (uint32_t: the 80 px atlas is ~78 KB, past a uint16_t's range)
    bitmap: 4-bit alpha, row-major, high nibble first, 0=transparent.

Rendering rule (firmware side): glyph top = y + (g.yoff - f.cap_top), glyph
left = x + g.xoff, pixel alpha = nibble * 17, blended over the framebuffer. So a
call that used text(x, y, s, color, s) keeps the same x and y.

ink_top/ink_bot exist because the AA ink is NOT the old block font's 8*scale
cell: at 20 px a '$' starts 2 px above the caps and a ',' reaches 19 px below
them, and at 80 px the overshoot is -9/+75. Erase boxes derived from the old
8*scale geometry would clip the top of '$' and leave tails of the previous
value on screen, so call sites size their band from these two fields.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FIRST, LAST = 32, 90  # printable ASCII, uppercase only (the old font's range)


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    ttf_path, px, out_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    # Symbol name in the header: default kAA<px>, overridable so call sites can
    # say kAASmall/kAAMed/kAABig/kAAHuge instead of kAA20/kAA60/...
    sym = sys.argv[4] if len(sys.argv) == 5 else f"kAA{px}"
    font = ImageFont.truetype(ttf_path, px)
    ascent, descent = font.getmetrics()
    charset = [chr(c) for c in range(FIRST, LAST + 1)]

    # Roboto Mono's advance is exactly 0.6 em, so derive it from the pixel
    # size instead of measuring: `font.getlength()` and the 1px guard-border
    # offset both introduce +-1 px of rounding, which at size 10/20 produced
    # 7/11 instead of 6/12 and would drift text out of its slot. Choosing
    # size = 10*scale makes this equal the old bitmap font's 6*scale advance.
    adv = round(0.6 * px)

    glyphs = []
    bitmap = bytearray()
    draw = ImageDraw.Draw(Image.new("L", (1, 1)))
    for ch in charset:
        l, t, r, b = draw.textbbox((0, 0), ch, font=font, anchor="la")
        w, h = r - l, b - t
        if w <= 0 or h <= 0:
            glyphs.append((adv, 0, 0, 0, t, len(bitmap)))
            continue
        im = Image.new("L", (w + 2, h + 2), 0)
        ImageDraw.Draw(im).text((1 - l, 1 - t), ch, font=font, fill=255, anchor="la")
        pxdata = list(im.getdata())
        # Crop the 1px guard border back off; keep the ink box only.
        ink = [pxdata[(yy + 1) * (w + 2) + (xx + 1)] for yy in range(h) for xx in range(w)]
        off = len(bitmap)
        for i in range(0, len(ink), 2):
            hi = ink[i] >> 4
            lo = (ink[i + 1] >> 4) if i + 1 < len(ink) else 0
            bitmap.append((hi << 4) | lo)
        glyphs.append((adv, w, h, l, t, off))

    cap_top = glyphs[ord("A") - FIRST][4]
    # Ink extent over every glyph, relative to the cap-top anchor, so callers
    # can size an erase band from the actual ink instead of the old 8*scale
    # cell (which both clipped '$' and left tails at the big sizes).
    ink_top, ink_bot = 0, 0
    for _, w, h, _, yoff, _ in glyphs:
        if h == 0:
            continue
        ink_top = min(ink_top, yoff - cap_top)
        ink_bot = max(ink_bot, yoff - cap_top + h - 1)
    with open(out_path, "w") as f:
        f.write(f"// Generated by militv-font-atlas-gen.py from {ttf_path} at {px}px.\n")
        f.write("// Roboto Mono, SIL Open Font License 1.1 - see OFL.txt.\n")
        f.write("// Do not edit by hand; regenerate with the script.\n#pragma once\n")
        f.write('#include <stdint.h>\n#include "aafont.h"\n\n')
        f.write(f'static const AAGlyph kAA{px}Glyphs[] = {{\n')
        for g in glyphs:
            f.write("  {%d, %d, %d, %d, %d, %d},\n" % g)
        f.write("};\n\n")
        f.write(f"static const uint8_t kAA{px}Bitmap[{len(bitmap)}] = {{\n")
        for i in range(0, len(bitmap), 20):
            f.write("  " + ",".join(f"0x{b:02x}" for b in bitmap[i:i + 20]) + ",\n")
        f.write("};\n\n")
        f.write(f"static const AAFont {sym} = {{ {FIRST}, {len(charset)}, {cap_top}, "
                f"{ascent}, {adv}, {ink_top}, {ink_bot}, kAA{px}Glyphs, kAA{px}Bitmap }};\n")
    print(f"{out_path}: {len(glyphs)} glyphs, {len(bitmap)} bitmap bytes, "
          f"adv={adv}, cap_top={cap_top}, ascent={ascent}, "
          f"ink={ink_top}..{ink_bot} (band {ink_bot - ink_top + 1}px)")


if __name__ == "__main__":
    main()

// Shared metadata for the anti-aliased font atlases.
//
// Every generated atlas (roboto_mono_*.h) describes itself with this one
// struct, so MiniTFT has a SINGLE textAA() implementation that takes a font
// rather than a copy per size. Adding a size is then a generator run plus a
// #include - no new rendering code.
//
// Generated files, all from RobotoMono.ttf (SIL OFL 1.1 - see OFL.txt):
//
//   atlas        size   advance   old bitmap equivalent   used for
//   kAASmall      10      6 px     text(..., scale 1)     footers, axis labels
//   kAAMed        20     12 px     text(..., scale 2)     headers, banners
//   kAATitle      40     24 px     text(..., scale 4)     portal / boot titles
//   kAABig        60     36 px     text(..., scale 6)     clock temperature
//   kAAHuge       80     48 px     text(..., scale 8)     clock HH:MM
//
// The advances match the old block-font scales exactly (Roboto Mono's advance
// is 0.6 * size), which is what lets call sites keep their x/y and their
// erase boxes after the swap.
#pragma once
#include <stdint.h>

struct AAGlyph {
  uint8_t adv;   // horizontal advance in px (constant: monospace)
  uint8_t w, h;  // ink-box size in px
  int8_t xoff;   // ink left edge relative to the glyph origin
  int8_t yoff;   // ink top edge relative to the font ascender line
  uint32_t off;  // byte offset into the atlas bitmap (uint32: the 80 px atlas
                 // is ~78 KB, past what a uint16_t could address)
};

struct AAFont {
  uint8_t first;    // first character code in the atlas (32)
  uint8_t count;    // number of glyphs (59: ASCII 32..90)
  int8_t cap_top;   // yoff of 'A' -> the cap-height band the caller anchors to
  uint8_t ascent;   // font ascent in px (newline advance)
  uint8_t adv;      // the monospace advance in px
  int8_t ink_top;   // topmost ink over ALL glyphs, relative to cap_top
  int8_t ink_bot;   // bottommost ink over ALL glyphs, relative to cap_top
  const AAGlyph *glyphs;
  const uint8_t *bitmap;  // 4-bit alpha, row-major, high nibble first
};

// Rows an erase band must cover to clear everything this font can draw on a
// line anchored at y (cap top at y): ink_top is usually negative because '$'
// and parens overshoot the caps, and ink_bot includes descenders and the
// comma tail.
//
// The whole-font extent is the WORST case over every glyph, so it is only safe
// when nothing else lives nearby (a full-width banner). For a field that shares
// the screen with its neighbours, use the string-aware aaStrTop/aaStrBot below:
// at 80 px the font-wide band is -9..+75 (85 px) while the digits a clock
// actually prints are only -1..+57, and the 85 px band would reach past the
// temperature row and erase it.
inline int aaBandH(const AAFont &f) { return (int)f.ink_bot - (int)f.ink_top + 1; }
// Top of that band relative to the anchor y (<= 0).
inline int aaBandTop(const AAFont &f) { return (int)f.ink_top; }

// Ink extent of an actual string, relative to the anchor y. Characters the
// atlas does not cover (and spaces) contribute nothing.
inline int aaStrTop(const AAFont &f, const char *s) {
  int top = 0;  // the anchor row itself is always part of the band
  for (; *s; s++) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    int idx = (int)(uint8_t)ch - f.first;
    if (idx < 0 || idx >= f.count) continue;
    const AAGlyph &g = f.glyphs[idx];
    if (!g.h) continue;
    int t = (int)g.yoff - (int)f.cap_top;
    if (t < top) top = t;
  }
  return top;
}

inline int aaStrBot(const AAFont &f, const char *s) {
  int bot = 0;
  for (; *s; s++) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    int idx = (int)(uint8_t)ch - f.first;
    if (idx < 0 || idx >= f.count) continue;
    const AAGlyph &g = f.glyphs[idx];
    if (!g.h) continue;
    int b = (int)g.yoff - (int)f.cap_top + g.h - 1;
    if (b > bot) bot = b;
  }
  return bot;
}

// Erase band for a field that shows `oldS` and is about to show `s`: the union
// of both ink extents, so a value that shrinks (or swaps to a shorter message)
// still clears its predecessor. `fOld` may be a larger atlas than `fNew` when
// a field shrinks to fit — passing the row's atlas for the old string is safe
// (it only widens the band). Both are relative to the anchor y.
inline void aaEraseBand(const AAFont &fNew, const char *s, const AAFont &fOld, const char *oldS,
                        int &top, int &bot) {
  top = aaStrTop(fNew, s);
  bot = aaStrBot(fNew, s);
  if (oldS && oldS[0]) {
    int t = aaStrTop(fOld, oldS);
    int b = aaStrBot(fOld, oldS);
    if (t < top) top = t;
    if (b > bot) bot = b;
  }
}

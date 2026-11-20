// gdi32: device contexts, fonts and metrics. The game draws through
// DirectDraw; GDI is only queried (metrics, device caps). Drawing calls
// succeed without output. If the game turns out to render glyphs through
// GDI (TextOutW/GetDIBits suggest IME text), real rasterisation goes here.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "runtime/hle/gdi_objects.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

std::atomic<uint32_t> g_next_gdi{0x00E00010};
uint32_t NewGdi() { return g_next_gdi += 0x10; }

// Stock objects get fixed handles so repeated GetStockObject calls agree.
HLE("gdi32.dll", GetStockObject, 1) { return 0x00E00000u | (Arg(c, 0) & 0xFF); }
HLE("gdi32.dll", CreateFontA, 14) { return NewGdi(); }
HLE("gdi32.dll", CreateFontIndirectA, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateHatchBrush, 2) { return NewGdi(); }
HLE("gdi32.dll", CreateSolidBrush, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateCompatibleDC, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateCompatibleBitmap, 3) { return NewGdi(); }
std::mutex g_bitmaps_mu;
std::map<uint32_t, std::shared_ptr<const GdiBitmap>> g_bitmaps;

HLE("gdi32.dll", DeleteObject, 1) {
  std::lock_guard<std::mutex> l(g_bitmaps_mu);
  g_bitmaps.erase(uint32_t(Arg(c, 0)));
  return 1;
}

// GetObjectA(h, size, buf): a bitmap's BITMAP {bmType, bmWidth, bmHeight,
// bmWidthBytes, bmPlanes, bmBitsPixel, bmBits} (bmBits NULL: a
// device-dependent bitmap); at most `size` bytes; NULL buf gives the size.
// Other objects are not modelled yet: fatal, named.
HLE("gdi32.dll", GetObjectA, 3) {
  const uint32_t h = uint32_t(Arg(c, 0));
  const gaddr buf = Arg(c, 2);
  const int size = int(Arg(c, 1));
  auto b = FindGdiBitmap(h);
  if (!b) FatalF("GetObjectA(%08X): not a bitmap; other GDI objects are not modelled yet", h);
  const uint32_t ptr = g_proc->x64 ? 8 : 4, total = 20 + (g_proc->x64 ? 4 : 0) + ptr;
  if (!buf) return total;
  uint8_t bm[32] = {};
  const int32_t f[4] = {0, b->width, b->height, b->width * 4};
  std::memcpy(bm, f, 16);
  const uint16_t planes = 1, bits = 32;
  std::memcpy(bm + 16, &planes, 2);
  std::memcpy(bm + 18, &bits, 2);
  const uint32_t n = std::min<uint32_t>(uint32_t(std::max(size, 0)), total);
  std::memcpy(GPtr(buf), bm, n);
  return n;
}
HLE("gdi32.dll", DeleteDC, 1) { return 1; }
HLE("gdi32.dll", SelectObject, 2) { return 0x00E00000u; }

HLE("gdi32.dll", SetBkMode, 2) { return 2; }      // previous: OPAQUE
HLE("gdi32.dll", SetTextColor, 2) { return 0; }
HLE("gdi32.dll", SetBkColor, 2) { return 0xFFFFFF; }

HLE("gdi32.dll", GetDeviceCaps, 2) {
  switch (Arg(c, 1)) {
    case 8: return uint32_t(g_display_w);   // HORZRES
    case 10: return uint32_t(g_display_h);  // VERTRES
    case 12: return 32;                     // BITSPIXEL
    case 14: return 1;                      // PLANES
    case 24: return 0xFFFFFFFF;             // NUMCOLORS: > 8 bpp
    case 88: case 90: return 96;            // LOGPIXELSX/Y
    case 116: return 60;                    // VREFRESH
    case 4: return 320;                     // HORZSIZE (mm)
    case 6: return 240;                     // VERTSIZE
    case 38: return 0x7E99;                 // RASTERCAPS
    default:
      WarnF("GetDeviceCaps(%u): returning 0\n", uint32_t(Arg(c, 1)));
      return 0;
  }
}

// Metrics of a generic 16 px proportional font.
HLE("gdi32.dll", GetTextMetricsA, 2) {
  uint32_t tm = Arg(c, 1);
  std::memset(GPtr(tm), 0, 56);
  Wr32(tm + 0, 16);   // tmHeight
  Wr32(tm + 4, 13);   // tmAscent
  Wr32(tm + 8, 3);    // tmDescent
  Wr32(tm + 20, 7);   // tmAveCharWidth
  Wr32(tm + 24, 14);  // tmMaxCharWidth
  Wr32(tm + 28, 400); // tmWeight
  Wr32(tm + 40, 96);  // tmDigitizedAspectX
  Wr32(tm + 44, 96);
  Wr8(tm + 48, 0x20); // tmFirstChar
  Wr8(tm + 49, 0xFF); // tmLastChar
  Wr8(tm + 50, '?');  // tmDefaultChar
  Wr8(tm + 51, ' ');  // tmBreakChar
  Wr8(tm + 55, 0x26); // tmPitchAndFamily: variable pitch, TrueType, swiss
  return 1;
}

HLE("gdi32.dll", GetCharWidthA, 4) {
  uint32_t first = Arg(c, 1), last = Arg(c, 2), buf = Arg(c, 3);
  for (uint32_t ch = first; ch <= last; ch++) Wr32(buf + 4 * (ch - first), 7);
  return 1;
}

HLE("gdi32.dll", TextOutW, 5) { return 1; }
HLE("gdi32.dll", ExtTextOutA, 8) { return 1; }
HLE("gdi32.dll", GetPixel, 3) { return 0; }
HLE("gdi32.dll", DPtoLP, 3) { return 1; }

HLE("gdi32.dll", GetDIBits, 7) {
  WarnF("GetDIBits: no GDI surfaces; returning 0 lines\n");
  return 0;
}

}  // namespace

uint32_t NewGdiBitmap(GdiBitmap b) {
  const uint32_t h = NewGdi();
  std::lock_guard<std::mutex> l(g_bitmaps_mu);
  g_bitmaps[h] = std::make_shared<const GdiBitmap>(std::move(b));
  return h;
}
std::shared_ptr<const GdiBitmap> FindGdiBitmap(uint32_t h) {
  std::lock_guard<std::mutex> l(g_bitmaps_mu);
  auto it = g_bitmaps.find(h);
  return it == g_bitmaps.end() ? nullptr : it->second;
}

}  // namespace recomp::hle

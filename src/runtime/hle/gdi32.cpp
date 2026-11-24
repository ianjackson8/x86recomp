// gdi32: device contexts, fonts, DIB sections and text.
//
// Fonts are real (host_font.h: CoreText on macOS/iOS): CreateFont* keep
// the LOGFONT, and metrics, glyphs and text come from the host's face of
// that name. Libraries that rasterise text through GDI (D3DX's ID3DXFont:
// GetCharacterPlacement for glyph indices, GetCharABCWidthsI, then
// GetGlyphOutline or ExtTextOut into a DIB section) get real glyphs.
// ExtTextOut draws into the DIB section selected into the DC; with
// another bitmap selected (a device-dependent one, or none) it draws
// nothing, as before. On hosts without a font engine the metric queries
// keep their stand-in answers (a 16 px proportional font) and the glyph
// functions are fatal, named.
//
// A DC's state: the selected font and bitmap, text and background
// colours, background mode, text alignment, map mode (MM_TEXT only) and
// current position. Any handle used as a DC gets one.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "runtime/hle/gdi_objects.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/host_font.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

std::atomic<uint32_t> g_next_gdi{0x00E00010};
uint32_t NewGdi() { return g_next_gdi += 0x10; }

// Stock objects have fixed handles (0x00E00000 | index) so repeated
// GetStockObject calls agree.
constexpr uint32_t kStock = 0x00E00000u;
constexpr uint32_t kSystemFont = kStock | 13, kWhiteBrush = kStock | 0, kBlackPen = kStock | 7;
// The 1x1 monochrome bitmap a new memory DC starts with.
constexpr uint32_t kStockBitmap = kStock | 0xFF;
bool IsStockFont(uint32_t h) {
  const uint32_t i = h & 0xFF;
  return (h & ~0xFFu) == kStock && ((i >= 10 && i <= 14) || i == 16 || i == 17);
}

struct GdiFont {
  // The LOGFONTW (92 bytes: 28 of fields, then 32 WCHARs of face name).
  uint8_t lf[92] = {};
  host_font::Desc desc;
  std::shared_ptr<const host_font::Font> host;  // made on first use
};

struct GdiDib {
  int32_t width = 0, height = 0;  // height < 0: top-down
  uint16_t bpp = 0;
  uint32_t stride = 0;
  gaddr bits = 0;
  uint8_t header[40] = {};  // the BITMAPINFOHEADER it was made from
  uint32_t rows() const { return uint32_t(height < 0 ? -height : height); }
  // Guest address of pixel row y (0 = top).
  gaddr Row(uint32_t y) const { return bits + stride * (height < 0 ? y : rows() - 1 - y); }
};

struct Dc {
  uint32_t font = kSystemFont;
  uint32_t bitmap = kStockBitmap;
  uint32_t text_color = 0, bk_color = 0xFFFFFF;  // COLORREF 0x00BBGGRR
  uint32_t bk_mode = 2;                          // OPAQUE
  uint32_t align = 0;                            // TA_LEFT | TA_TOP | TA_NOUPDATECP
  uint32_t map_mode = 1;                         // MM_TEXT
  int32_t x = 0, y = 0;                          // current position
};

std::mutex g_mu;  // everything below
std::map<uint32_t, std::shared_ptr<const GdiBitmap>> g_bitmaps;
std::map<uint32_t, std::shared_ptr<GdiFont>> g_fonts;
std::map<uint32_t, std::shared_ptr<GdiDib>> g_dibs;
std::map<uint32_t, Dc> g_dcs;

Dc& DcOf(uint32_t h) { return g_dcs[h]; }  // under g_mu

uint32_t NewFont(const uint8_t (&lfw)[92]) {
  auto f = std::make_shared<GdiFont>();
  std::memcpy(f->lf, lfw, 92);
  int32_t height, width, weight;
  std::memcpy(&height, lfw, 4);
  std::memcpy(&width, lfw + 4, 4);
  std::memcpy(&weight, lfw + 16, 4);
  f->desc.height = height;
  f->desc.width = width;
  f->desc.weight = weight ? weight : 400;
  f->desc.italic = lfw[20] != 0;
  f->desc.antialias = lfw[26] != 3;  // lfQuality NONANTIALIASED_QUALITY
  for (int i = 0; i < 32; i++) {
    char16_t ch;
    std::memcpy(&ch, lfw + 28 + 2 * i, 2);
    if (!ch) break;
    f->desc.face.push_back(ch);
  }
  if (width) WarnF("CreateFont: lfWidth %d (condensed/expanded faces) not modelled; using the face's own\n", width);
  const uint32_t h = NewGdi();
  std::lock_guard<std::mutex> l(g_mu);
  g_fonts[h] = std::move(f);
  return h;
}

// LOGFONTA (60 bytes: the same 28 bytes of fields, then 32 chars).
uint32_t NewFontA(const uint8_t* lfa) {
  uint8_t lfw[92] = {};
  std::memcpy(lfw, lfa, 28);
  for (int i = 0; i < 32 && lfa[28 + i]; i++) {
    const char16_t ch = win32::Cp1252ToUnicode(lfa[28 + i]);
    std::memcpy(lfw + 28 + 2 * i, &ch, 2);
  }
  return NewFont(lfw);
}

// The host font for a DC's selected font (stock fonts: the default face
// at 16 px), or null where the host has no font engine.
std::shared_ptr<const host_font::Font> HostFontOf(uint32_t dc) {
  if (!host_font::Available()) return nullptr;
  std::shared_ptr<GdiFont> f;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_fonts.find(DcOf(dc).font);
    if (it != g_fonts.end()) {
      f = it->second;
    } else {
      static auto stock = [] {
        auto s = std::make_shared<GdiFont>();
        s->desc.height = 16;
        return s;
      }();
      f = stock;
    }
    if (f->host) return f->host;
  }
  auto host = host_font::Create(f->desc);
  std::lock_guard<std::mutex> l(g_mu);
  if (!f->host) f->host = host;
  return f->host;
}

std::shared_ptr<const host_font::Font> NeedHostFont(uint32_t dc, const char* fn) {
  auto f = HostFontOf(dc);
  if (!f) FatalF("%s: needs a host font engine (none on this host yet)", fn);
  return f;
}

HLE("gdi32.dll", GetStockObject, 1) { return kStock | (Arg(c, 0) & 0xFF); }

// CreateFontA(height, width, escapement, orientation, weight, italic,
// underline, strikeout, charset, outprecision, clipprecision, quality,
// pitchandfamily, face)
HLE("gdi32.dll", CreateFontA, 14) {
  uint8_t lfa[60] = {};
  for (int i = 0; i < 5; i++) {
    const int32_t v = int32_t(Arg(c, i));
    std::memcpy(lfa + 4 * i, &v, 4);
  }
  for (int i = 0; i < 8; i++) lfa[20 + i] = uint8_t(Arg(c, 5 + i));
  if (gaddr face = Arg(c, 13)) {
    const std::string s = GStr(face);
    std::memcpy(lfa + 28, s.data(), std::min<size_t>(s.size(), 31));
  }
  return NewFontA(lfa);
}
// CreateFontW: CreateFontA's arguments, the face name in WCHARs.
HLE("gdi32.dll", CreateFontW, 14) {
  uint8_t lfw[92] = {};
  for (int i = 0; i < 5; i++) {
    const int32_t v = int32_t(Arg(c, i));
    std::memcpy(lfw + 4 * i, &v, 4);
  }
  for (int i = 0; i < 8; i++) lfw[20 + i] = uint8_t(Arg(c, 5 + i));
  if (gaddr face = Arg(c, 13))
    for (int i = 0; i < 31; i++) {
      const char16_t ch = Rd16(face + 2 * i);
      if (!ch) break;
      std::memcpy(lfw + 28 + 2 * i, &ch, 2);
    }
  return NewFont(lfw);
}
HLE("gdi32.dll", CreateFontIndirectA, 1) {
  if (!Arg(c, 0)) return 0;
  uint8_t lfa[60];
  std::memcpy(lfa, GPtr(Arg(c, 0)), 60);
  return NewFontA(lfa);
}
HLE("gdi32.dll", CreateFontIndirectW, 1) {
  if (!Arg(c, 0)) return 0;
  uint8_t lfw[92];
  std::memcpy(lfw, GPtr(Arg(c, 0)), 92);
  return NewFont(lfw);
}

HLE("gdi32.dll", CreateHatchBrush, 2) { return NewGdi(); }
HLE("gdi32.dll", CreateSolidBrush, 1) { return NewGdi(); }
HLE("gdi32.dll", CreateCompatibleDC, 1) {
  const uint32_t h = NewGdi();
  std::lock_guard<std::mutex> l(g_mu);
  g_dcs[h] = Dc{};
  return h;
}
HLE("gdi32.dll", CreateCompatibleBitmap, 3) { return NewGdi(); }

// CreateDIBSection(hdc, bmi, usage, &bits, section, offset): pixels in
// guest memory the caller writes directly. BI_RGB at 1/4/8/16/24/32 bits
// and BI_BITFIELDS are kept; ExtTextOut draws into 24 and 32 bits.
HLE("gdi32.dll", CreateDIBSection, 6) {
  const gaddr bmi = Arg(c, 1), out = Arg(c, 3);
  if (!bmi) return 0;
  if (Arg(c, 4)) FatalF("CreateDIBSection: file-mapping sections not modelled yet");
  auto d = std::make_shared<GdiDib>();
  std::memcpy(d->header, GPtr(bmi), 40);
  std::memcpy(&d->width, d->header + 4, 4);
  std::memcpy(&d->height, d->header + 8, 4);
  std::memcpy(&d->bpp, d->header + 14, 2);
  uint32_t compression;
  std::memcpy(&compression, d->header + 16, 4);
  if (compression != 0 && compression != 3)
    FatalF("CreateDIBSection: compression %u not modelled", compression);
  if (d->width <= 0 || d->height == 0) return 0;
  d->stride = (uint32_t(d->width) * d->bpp + 31) / 32 * 4;
  d->bits = HeapAllocProcess(uint64_t(d->stride) * d->rows(), true);
  if (!d->bits) return 0;
  if (out) Wr32(out, uint32_t(d->bits));
  const uint32_t h = NewGdi();
  std::lock_guard<std::mutex> l(g_mu);
  g_dibs[h] = std::move(d);
  return h;
}

HLE("gdi32.dll", DeleteObject, 1) {
  const uint32_t h = uint32_t(Arg(c, 0));
  gaddr free_bits = 0;
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_bitmaps.erase(h);
    g_fonts.erase(h);
    if (auto it = g_dibs.find(h); it != g_dibs.end()) {
      free_bits = it->second->bits;
      g_dibs.erase(it);
    }
  }
  if (free_bits) HeapFreeProcess(free_bits);
  return 1;
}

HLE("gdi32.dll", DeleteDC, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  g_dcs.erase(uint32_t(Arg(c, 0)));
  return 1;
}

// SelectObject(hdc, h): fonts and bitmaps (DIB sections included) become
// the DC's and the previous one is returned; brushes, pens and other
// objects are not tracked (the stock handle).
HLE("gdi32.dll", SelectObject, 2) {
  const uint32_t dc = uint32_t(Arg(c, 0)), h = uint32_t(Arg(c, 1));
  std::lock_guard<std::mutex> l(g_mu);
  Dc& d = DcOf(dc);
  if (g_fonts.count(h) || IsStockFont(h)) return std::exchange(d.font, h);
  if (g_dibs.count(h) || g_bitmaps.count(h)) return std::exchange(d.bitmap, h);
  return kStock;
}

// GetCurrentObject(hdc, type): OBJ_PEN 1, OBJ_BRUSH 2, OBJ_FONT 6,
// OBJ_BITMAP 7.
HLE("gdi32.dll", GetCurrentObject, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  const Dc& d = DcOf(uint32_t(Arg(c, 0)));
  switch (Arg(c, 1)) {
    case 1: return kBlackPen;
    case 2: return kWhiteBrush;
    case 6: return d.font;
    case 7: return d.bitmap;
    default: FatalF("GetCurrentObject: object type %u not modelled", uint32_t(Arg(c, 1)));
  }
}

// GetObject(h, size, buf) for bitmaps (BITMAP), DIB sections (BITMAP, or
// DIBSECTION when size allows) and fonts (LOGFONTA / LOGFONTW). At most
// `size` bytes; a NULL buf gives the size. Other objects: fatal, named.
uint32_t GetObjectImpl(Ctx& c, bool wide) {
  const uint32_t h = uint32_t(Arg(c, 0));
  const gaddr buf = Arg(c, 2);
  const uint32_t size = uint32_t(std::max<int32_t>(int32_t(Arg(c, 1)), 0));
  const uint32_t ptr = g_proc->x64 ? 8 : 4, bitmap_size = 20 + (g_proc->x64 ? 4 : 0) + ptr;
  uint8_t out[128] = {};
  uint32_t total = 0;
  auto bitmap = [&](int32_t w, int32_t hgt, int32_t stride, uint16_t bpp, uint64_t bits) {
    const int32_t f[4] = {0, w, hgt, stride};
    std::memcpy(out, f, 16);
    const uint16_t planes = 1;
    std::memcpy(out + 16, &planes, 2);
    std::memcpy(out + 18, &bpp, 2);
    std::memcpy(out + bitmap_size - ptr, &bits, ptr);
  };
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (auto b = g_bitmaps.find(h); b != g_bitmaps.end()) {
      bitmap(b->second->width, b->second->height, b->second->width * 4, 32, 0);
      total = bitmap_size;
    } else if (auto d = g_dibs.find(h); d != g_dibs.end()) {
      const GdiDib& s = *d->second;
      bitmap(s.width, int32_t(s.rows()), int32_t(s.stride), s.bpp, s.bits);
      total = bitmap_size;
      // DIBSECTION: BITMAP, BITMAPINFOHEADER, dsBitfields[3], dshSection,
      // dsOffset.
      const uint32_t ds = bitmap_size + 40 + 12 + ptr + 4;
      if (size >= ds) {
        std::memcpy(out + bitmap_size, s.header, 40);
        total = ds;
      }
    } else if (auto f = g_fonts.find(h); f != g_fonts.end()) {
      if (wide) {
        std::memcpy(out, f->second->lf, 92);
        total = 92;
      } else {
        std::memcpy(out, f->second->lf, 28);
        for (int i = 0; i < 32; i++) {
          char16_t ch;
          std::memcpy(&ch, f->second->lf + 28 + 2 * i, 2);
          bool ok;
          out[28 + i] = ch ? win32::UnicodeToCp1252(ch, &ok) : 0;
          if (!ch) break;
        }
        total = 60;
      }
    }
  }
  if (!total)
    FatalF("GetObject%c(%08X): this kind of GDI object is not modelled yet", wide ? 'W' : 'A', h);
  if (!buf) return total;
  const uint32_t n = std::min(size, total);
  std::memcpy(GPtr(buf), out, n);
  return n;
}
HLE("gdi32.dll", GetObjectA, 3) { return GetObjectImpl(c, false); }
HLE("gdi32.dll", GetObjectW, 3) { return GetObjectImpl(c, true); }

// DC state setters: each returns the previous value.
#define RECOMP_DC_SET(fn, field)                                  \
  HLE("gdi32.dll", fn, 2) {                                       \
    std::lock_guard<std::mutex> l(g_mu);                          \
    return std::exchange(DcOf(uint32_t(Arg(c, 0))).field, uint32_t(Arg(c, 1))); \
  }
RECOMP_DC_SET(SetBkMode, bk_mode)
RECOMP_DC_SET(SetTextColor, text_color)
RECOMP_DC_SET(SetBkColor, bk_color)
RECOMP_DC_SET(SetTextAlign, align)
#undef RECOMP_DC_SET

// SetMapMode: MM_TEXT (pixels, y down) is the only mode modelled.
HLE("gdi32.dll", SetMapMode, 2) {
  if (Arg(c, 1) != 1) FatalF("SetMapMode(%u): only MM_TEXT is modelled", uint32_t(Arg(c, 1)));
  std::lock_guard<std::mutex> l(g_mu);
  return std::exchange(DcOf(uint32_t(Arg(c, 0))).map_mode, 1u);
}

// MoveToEx(hdc, x, y, &previous)
HLE("gdi32.dll", MoveToEx, 4) {
  std::lock_guard<std::mutex> l(g_mu);
  Dc& d = DcOf(uint32_t(Arg(c, 0)));
  if (gaddr p = Arg(c, 3)) {
    Wr32(p, uint32_t(d.x));
    Wr32(p + 4, uint32_t(d.y));
  }
  d.x = int32_t(Arg(c, 1));
  d.y = int32_t(Arg(c, 2));
  return 1;
}

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

// TEXTMETRIC: 11 LONGs, then tmFirstChar, tmLastChar, tmDefaultChar,
// tmBreakChar (BYTEs in the A form, WCHARs in the W form), then tmItalic,
// tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet.
void WriteTextMetrics(gaddr tm, bool wide, const host_font::Metrics* m) {
  std::memset(GPtr(tm), 0, wide ? 60 : 56);
  int32_t v[11];
  uint16_t chars[4];
  uint8_t pitch;
  bool italic = false;
  if (m) {
    const int32_t r[11] = {m->height, m->ascent, m->descent, m->internal_leading,
                           m->external_leading, m->ave_char_width, m->max_char_width,
                           m->weight, 0, 96, 96};
    std::memcpy(v, r, sizeof v);
    chars[0] = m->first_char;
    chars[1] = m->last_char;
    chars[2] = m->default_char;
    chars[3] = m->break_char;
    // TMPF_VECTOR | TMPF_TRUETYPE, TMPF_FIXED_PITCH set for a variable
    // pitch (sic), FF_SWISS.
    pitch = uint8_t(0x06 | (m->fixed_pitch ? 0 : 0x01) | 0x20);
    italic = m->italic;
  } else {  // no host font engine: a generic 16 px proportional font
    const int32_t r[11] = {16, 13, 3, 0, 0, 7, 14, 400, 0, 96, 96};
    std::memcpy(v, r, sizeof v);
    chars[0] = 0x20;
    chars[1] = 0xFF;
    chars[2] = '?';
    chars[3] = ' ';
    pitch = 0x26;
  }
  std::memcpy(GPtr(tm), v, 44);
  gaddr p = tm + 44;
  for (uint16_t ch : chars) {
    if (wide) {
      Wr16(p, ch);
      p += 2;
    } else {
      Wr8(p, uint8_t(std::min<uint16_t>(ch, 0xFF)));
      p += 1;
    }
  }
  Wr8(p, italic ? 0xFF : 0);  // tmItalic
  Wr8(p + 3, pitch);
  Wr8(p + 4, 0);  // tmCharSet: ANSI_CHARSET
}

HLE("gdi32.dll", GetTextMetricsA, 2) {
  auto f = HostFontOf(uint32_t(Arg(c, 0)));
  WriteTextMetrics(Arg(c, 1), false, f ? &f->metrics() : nullptr);
  return 1;
}
HLE("gdi32.dll", GetTextMetricsW, 2) {
  auto f = HostFontOf(uint32_t(Arg(c, 0)));
  WriteTextMetrics(Arg(c, 1), true, f ? &f->metrics() : nullptr);
  return 1;
}

// GetCharWidthA(hdc, first, last, widths): advances of the selected font
// (code page 1252 characters).
HLE("gdi32.dll", GetCharWidthA, 4) {
  const uint32_t first = Arg(c, 1), last = Arg(c, 2), buf = Arg(c, 3);
  auto f = HostFontOf(uint32_t(Arg(c, 0)));
  for (uint32_t ch = first; ch <= last; ch++) {
    int32_t w = 7;
    if (f) {
      const char16_t u = win32::Cp1252ToUnicode(uint8_t(ch));
      w = f->Measure(f->Glyph(&u, 1)).advance;
    }
    Wr32(buf + 4 * (ch - first), uint32_t(w));
  }
  return 1;
}

// GetCharABCWidthsI(hdc, first, count, glyphs, abc): ABC {A, B, C} per
// glyph index (glyphs, or first..first+count-1 when it is NULL). An empty
// glyph has a 1-pixel black box, as in GDI.
HLE("gdi32.dll", GetCharABCWidthsI, 5) {
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetCharABCWidthsI");
  const uint32_t first = Arg(c, 1), count = Arg(c, 2);
  const gaddr glyphs = Arg(c, 3), abc = Arg(c, 4);
  if (!abc) return 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint16_t g = glyphs ? Rd16(glyphs + 2 * i) : uint16_t(first + i);
    const host_font::GlyphMetrics m = f->Measure(g);
    const int32_t a = m.box_w ? m.box_x : 0;
    const uint32_t b = m.box_w ? m.box_w : 1;
    Wr32(abc + 12 * i, uint32_t(a));
    Wr32(abc + 12 * i + 4, b);
    Wr32(abc + 12 * i + 8, uint32_t(m.advance - a - int32_t(b)));
  }
  return 1;
}

// GetCharWidthI(hdc, first, count, glyphs, widths): advances per glyph
// index (glyphs, or first..first+count-1 when it is NULL).
HLE("gdi32.dll", GetCharWidthI, 5) {
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetCharWidthI");
  const uint32_t first = Arg(c, 1), count = Arg(c, 2);
  const gaddr glyphs = Arg(c, 3), out = Arg(c, 4);
  if (!out) return 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint16_t g = glyphs ? Rd16(glyphs + 2 * i) : uint16_t(first + i);
    Wr32(out + 4 * i, uint32_t(f->Measure(g).advance));
  }
  return 1;
}

// AddFontResourceEx(path, flags, 0): a font file's faces for this process
// (FR_PRIVATE or not, they are the process's only); the number added, 0 if
// the file is not a font. RemoveFontResourceEx: nonzero if it was added.
uint32_t AddFontFile(const std::string& guest_path) {
  return uint32_t(host_font::RegisterFile(HostPath(guest_path)));
}
HLE("gdi32.dll", AddFontResourceExA, 3) { return AddFontFile(GStr(Arg(c, 0))); }
HLE("gdi32.dll", AddFontResourceExW, 3) { return AddFontFile(NarrowPath(GWStr(Arg(c, 0)))); }
HLE("gdi32.dll", RemoveFontResourceExA, 3) {
  return host_font::UnregisterFile(HostPath(GStr(Arg(c, 0)))) ? 1 : 0;
}
HLE("gdi32.dll", RemoveFontResourceExW, 3) {
  return host_font::UnregisterFile(HostPath(NarrowPath(GWStr(Arg(c, 0))))) ? 1 : 0;
}

// EnumFontFamiliesExW(hdc, &logfont, proc, lparam, flags): proc(&elf,
// &ntm, TRUETYPE_FONTTYPE, lparam) for each family matching lfFaceName
// (all of them for ""), one regular style each, in the requested charset
// (ANSI for DEFAULT_CHARSET); stops when proc returns 0. ENUMLOGFONTEXW is
// a LOGFONTW, then elfFullName[64], elfStyle[32], elfScript[32]; the
// NEWTEXTMETRICEXW a TEXTMETRICW, ntmFlags, ntmSizeEM, ntmCellHeight,
// ntmAvgWidth, then a FONTSIGNATURE. Returns the last value proc returned.
HLE("gdi32.dll", EnumFontFamiliesExW, 5) {
  if (!host_font::Available()) FatalF("EnumFontFamiliesExW: needs a host font engine");
  const gaddr lf = Arg(c, 1), proc = Arg(c, 2);
  const uint64_t lparam = Arg(c, 3);
  std::u16string want;
  for (int i = 0; i < 32; i++) {
    const char16_t ch = Rd16(lf + 28 + 2 * i);
    if (!ch) break;
    want.push_back(ch);
  }
  const uint8_t charset = Rd8(lf + 23) == 1 ? 0 : Rd8(lf + 23);  // DEFAULT_CHARSET
  std::vector<std::u16string> faces;
  auto lower = [](std::u16string s) {
    for (char16_t& ch : s)
      if (ch >= u'A' && ch <= u'Z') ch = char16_t(ch + 32);
    return s;
  };
  for (const auto& f : host_font::FamilyNames())
    if (want.empty() || lower(f) == lower(want)) faces.push_back(f);
  const gaddr elf = HeapAllocProcess(348, true), ntm = HeapAllocProcess(100, true);
  uint32_t ret = 1;
  for (const auto& face : faces) {
    host_font::Desc d;
    d.face = face;
    d.height = -16;
    const host_font::Metrics m = host_font::Create(d)->metrics();
    std::memset(GPtr(elf), 0, 348);
    const int32_t head[5] = {m.height, m.ave_char_width, 0, 0, m.weight};
    std::memcpy(GPtr(elf), head, sizeof head);
    Wr8(elf + 20, m.italic ? 1 : 0);
    Wr8(elf + 23, charset);
    Wr8(elf + 24, 3);  // OUT_STROKE_PRECIS
    Wr8(elf + 25, 2);  // CLIP_STROKE_PRECIS
    Wr8(elf + 26, 1);  // DRAFT_QUALITY
    Wr8(elf + 27, uint8_t((m.fixed_pitch ? 1 : 2) | 0x20));
    auto put = [&](gaddr at, const std::u16string& s, size_t n) {
      for (size_t i = 0; i < s.size() && i + 1 < n; i++) Wr16(at + 2 * gaddr(i), s[i]);
    };
    put(elf + 28, face, 32);
    put(elf + 92, face, 64);
    put(elf + 220, u"Regular", 32);
    put(elf + 284, u"Western", 32);
    WriteTextMetrics(ntm, true, &m);
    Wr32(ntm + 60, 0x40);  // NTM_REGULAR
    Wr32(ntm + 64, 2048);  // ntmSizeEM
    Wr32(ntm + 68, uint32_t(m.height));
    Wr32(ntm + 72, uint32_t(m.ave_char_width));
    Wr32(ntm + 76 + 16, 1);  // fsCsb[0]: Latin 1
    ret = uint32_t(CallGuest(c, proc, {elf, ntm, 4, lparam}));  // TRUETYPE_FONTTYPE
    if (!ret) break;
  }
  HeapFreeProcess(elf);
  HeapFreeProcess(ntm);
  return ret;
}

// GetKerningPairsW(hdc, n, pairs): the selected font's KERNINGPAIRs
// {wFirst, wSecond, iKernAmount} (host_font: the 'kern' table GDI reads);
// with no array, how many there are.
HLE("gdi32.dll", GetKerningPairsW, 3) {
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetKerningPairsW");
  const auto pairs = f->KerningPairs();
  const uint32_t n = uint32_t(Arg(c, 1));
  const gaddr out = Arg(c, 2);
  if (!out) return uint32_t(pairs.size());
  const uint32_t m = std::min<uint32_t>(n, uint32_t(pairs.size()));
  for (uint32_t i = 0; i < m; i++) {
    Wr16(out + 8 * i, pairs[i].first);
    Wr16(out + 8 * i + 2, pairs[i].second);
    Wr32(out + 8 * i + 4, uint32_t(pairs[i].amount));
  }
  return m;
}

// GetFontUnicodeRanges(hdc, glyphset): the characters of the Basic
// Multilingual Plane the face maps (GLYPHSET: cbThis, flAccel,
// cGlyphsSupported, cRanges, then WCRANGE {wcLow, cGlyphs} each); returns
// its size, the size needed when glyphset is NULL.
HLE("gdi32.dll", GetFontUnicodeRanges, 2) {
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetFontUnicodeRanges");
  std::u16string all;
  for (uint32_t ch = 0; ch < 0x10000; ch++)
    if (ch < 0xD800 || ch >= 0xE000) all.push_back(char16_t(ch));
  std::vector<uint16_t> g(all.size());
  f->Glyphs(all.data(), all.size(), g.data());
  std::vector<std::pair<char16_t, uint16_t>> ranges;
  uint32_t supported = 0;
  for (size_t i = 0; i < all.size(); i++) {
    if (!g[i]) continue;
    supported++;
    if (!ranges.empty() && ranges.back().first + ranges.back().second == all[i])
      ranges.back().second++;
    else
      ranges.push_back({all[i], 1});
  }
  const uint32_t size = 16 + 4 * uint32_t(ranges.size());
  const gaddr out = Arg(c, 1);
  if (!out) return size;
  Wr32(out, size);
  Wr32(out + 4, 0);
  Wr32(out + 8, supported);
  Wr32(out + 12, uint32_t(ranges.size()));
  for (size_t i = 0; i < ranges.size(); i++) {
    Wr16(out + 16 + 4 * i, ranges[i].first);
    Wr16(out + 18 + 4 * i, ranges[i].second);
  }
  return size;
}

// GetGlyphIndicesW(hdc, str, count, glyphs, flags): a glyph index per
// character; a missing one is 0, or 0xFFFF with GGI_MARK_NONEXISTING_GLYPHS.
HLE("gdi32.dll", GetGlyphIndicesW, 5) {
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetGlyphIndicesW");
  const gaddr s = Arg(c, 1), out = Arg(c, 3);
  const uint32_t n = Arg(c, 2), flags = Arg(c, 4);
  for (uint32_t i = 0; i < n; i++) {
    const char16_t ch = Rd16(s + 2 * i);
    uint16_t g = f->Glyph(&ch, 1);
    if (!g && (flags & 1)) g = 0xFFFF;
    Wr16(out + 2 * i, g);
  }
  return n;
}

// GGO_NATIVE / GGO_BEZIER data: per contour a TTPOLYGONHEADER {cb, dwType
// TT_POLYGON_TYPE (24), pfxStart} then TTPOLYCURVEs {wType, cpfx,
// POINTFX[cpfx]}: TT_PRIM_LINE (1) runs of lines, TT_PRIM_QSPLINE (2) a
// quadratic (control, end), TT_PRIM_CSPLINE (3) a cubic (two controls,
// end); GGO_BEZIER has lines and cubics only (quadratics raised). Points
// are 16.16 fixed (FIXED: fract, value), pixels, y up from the pen. A
// contour that does not end at its start gets the closing line written
// out: GDI's polygons close implicitly, but some readers (a SWF player's
// font provider) fill only what is written. Returns the size, or writes
// it when it fits in `size`.
uint32_t GlyphOutlineData(const host_font::Font& f, uint16_t glyph, bool bezier, gaddr buf,
                          uint32_t size) {
  std::vector<uint8_t> out;
  auto u16 = [&](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i))); };
  auto fx = [&](float v) { u32(uint32_t(int32_t(std::lround(double(v) * 65536.0)))); };
  size_t header = SIZE_MAX;           // offset of the open contour's header
  size_t run = SIZE_MAX;              // offset of an open TT_PRIM_LINE curve
  float px = 0, py = 0, sx = 0, sy = 0;  // current point, contour start
  auto line_to = [&](float x, float y) {
    if (run == SIZE_MAX) {
      run = out.size();
      u16(1);  // TT_PRIM_LINE
      u16(0);
    }
    fx(x);
    fx(y);
    const uint16_t n = uint16_t((out[run + 2] | out[run + 3] << 8) + 1);  // cpfx
    out[run + 2] = uint8_t(n);
    out[run + 3] = uint8_t(n >> 8);
    px = x;
    py = y;
  };
  auto close = [&] {
    if (header == SIZE_MAX) return;
    if (px != sx || py != sy) line_to(sx, sy);
    const uint32_t cb = uint32_t(out.size() - header);
    for (int i = 0; i < 4; i++) out[header + i] = uint8_t(cb >> (8 * i));
    header = SIZE_MAX;
    run = SIZE_MAX;
  };
  auto curve = [&](uint16_t type, const float* pts, int n) {
    u16(type);
    u16(uint16_t(n));
    for (int i = 0; i < n; i++) {
      fx(pts[2 * i]);
      fx(pts[2 * i + 1]);
    }
  };
  for (const auto& e : f.Outline(glyph)) {
    switch (e.kind) {
      case host_font::Font::PathElem::kMove:
        close();
        header = out.size();
        u32(0);   // cb, set by close()
        u32(24);  // TT_POLYGON_TYPE
        fx(e.pts[0]);
        fx(e.pts[1]);
        px = sx = e.pts[0];
        py = sy = e.pts[1];
        break;
      case host_font::Font::PathElem::kLine:
        line_to(e.pts[0], e.pts[1]);
        break;
      case host_font::Font::PathElem::kQuad:
        run = SIZE_MAX;
        if (bezier) {  // raised to a cubic
          const float c[6] = {px + 2.0f / 3 * (e.pts[0] - px), py + 2.0f / 3 * (e.pts[1] - py),
                              e.pts[2] + 2.0f / 3 * (e.pts[0] - e.pts[2]),
                              e.pts[3] + 2.0f / 3 * (e.pts[1] - e.pts[3]), e.pts[2], e.pts[3]};
          curve(3, c, 3);
        } else {
          curve(2, e.pts, 2);
        }
        px = e.pts[2];
        py = e.pts[3];
        break;
      case host_font::Font::PathElem::kCubic:
        run = SIZE_MAX;
        curve(3, e.pts, 3);
        px = e.pts[4];
        py = e.pts[5];
        break;
      case host_font::Font::PathElem::kClose:
        close();
        px = sx;
        py = sy;
        break;
    }
  }
  close();
  if (!buf || !size) return uint32_t(out.size());
  if (size < out.size()) return 0xFFFFFFFF;  // GDI_ERROR
  if (!out.empty()) std::memcpy(GPtr(buf), out.data(), out.size());
  return uint32_t(out.size());
}

// GetGlyphOutline(hdc, ch, format, &gm, size, buf, &mat2): GGO_METRICS and
// the GGO_BITMAP / GGO_GRAY2,4,8_BITMAP formats (rows DWORD-aligned, top
// down; grey levels 0..4, 16 or 64), for a character or, with
// GGO_GLYPH_INDEX, a glyph index. Identity MAT2 only. Outlines
// (GGO_NATIVE, GGO_BEZIER) are not modelled.
uint32_t GetGlyphOutlineImpl(Ctx& c, bool wide) {
  constexpr uint32_t kGdiError = 0xFFFFFFFF;
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetGlyphOutline");
  const uint32_t code = Arg(c, 1), format = Arg(c, 2), size = Arg(c, 4);
  const gaddr gm_out = Arg(c, 3), buf = Arg(c, 5), mat = Arg(c, 6);
  if (!gm_out || !mat) return kGdiError;
  static const uint8_t kIdentity[16] = {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
  if (std::memcmp(GPtr(mat), kIdentity, 16) != 0)
    FatalF("GetGlyphOutline: a transform (MAT2) other than identity is not modelled");
  uint16_t glyph;
  if (format & 0x80) {  // GGO_GLYPH_INDEX
    glyph = uint16_t(code);
  } else {
    const char16_t u = wide ? char16_t(code) : win32::Cp1252ToUnicode(uint8_t(code));
    glyph = f->Glyph(&u, 1);
  }
  const host_font::GlyphMetrics m = f->Measure(glyph);
  // GLYPHMETRICS: box size, origin (y up), cell increments.
  Wr32(gm_out, m.box_w ? m.box_w : 1);
  Wr32(gm_out + 4, m.box_h ? m.box_h : 1);
  Wr32(gm_out + 8, uint32_t(m.box_w ? m.box_x : 0));
  Wr32(gm_out + 12, uint32_t(m.box_h ? m.box_y : 0));
  Wr16(gm_out + 16, uint16_t(int16_t(m.advance)));
  Wr16(gm_out + 18, 0);
  const uint32_t kind = format & 0x0F;
  if (kind == 0) return 1;  // GGO_METRICS
  if (kind == 2 || kind == 3) return GlyphOutlineData(*f, glyph, kind == 3, buf, size);
  int levels;
  switch (kind) {
    case 1: levels = 1; break;   // GGO_BITMAP
    case 4: levels = 4; break;   // GGO_GRAY2_BITMAP
    case 5: levels = 16; break;  // GGO_GRAY4_BITMAP
    case 6: levels = 64; break;  // GGO_GRAY8_BITMAP
    default: FatalF("GetGlyphOutline: format %u not modelled", kind);
  }
  if (!m.box_w || !m.box_h) return 0;  // nothing to draw
  const uint32_t pitch =
      levels == 1 ? (m.box_w + 31) / 32 * 4 : (m.box_w + 3) & ~3u;
  const uint32_t need = pitch * m.box_h;
  if (!buf || !size) return need;
  if (size < need) return kGdiError;
  const std::vector<uint8_t> cov = f->Render(glyph, m);
  std::vector<uint8_t> out(need, 0);
  for (uint32_t y = 0; y < m.box_h; y++)
    for (uint32_t x = 0; x < m.box_w; x++) {
      const uint32_t v = cov[size_t(y) * m.box_w + x];
      if (levels == 1) {
        if (v >= 128) out[y * pitch + x / 8] |= uint8_t(0x80 >> (x % 8));
      } else {
        out[y * pitch + x] = uint8_t((v * uint32_t(levels) + 127) / 255);
      }
    }
  std::memcpy(GPtr(buf), out.data(), need);
  return need;
}
HLE("gdi32.dll", GetGlyphOutlineA, 7) { return GetGlyphOutlineImpl(c, false); }
HLE("gdi32.dll", GetGlyphOutlineW, 7) { return GetGlyphOutlineImpl(c, true); }

// GetCharacterPlacement(hdc, str, count, max_extent, &results, flags):
// left-to-right layout one glyph per character, no ligatures, reordering
// or kerning (GCP_* requests for them are not modelled). GCP_RESULTS:
// lStructSize, lpOutString, lpOrder, lpDx, lpCaretPos, lpClass, lpGlyphs,
// nGlyphs (in: array size), nMaxFit. Returns MAKELONG(width, height).
uint32_t GetCharacterPlacementImpl(Ctx& c, const std::u16string& s, bool wide) {
  auto f = NeedHostFont(uint32_t(Arg(c, 0)), "GetCharacterPlacement");
  const gaddr r = Arg(c, 4);
  const uint32_t max_extent = Arg(c, 3), flags = Arg(c, 5);
  for (char16_t ch : s)
    if (ch >= 0xD800 && ch < 0xE000)
      FatalF("GetCharacterPlacement: surrogate pairs not modelled yet");
  uint32_t n = uint32_t(s.size());
  if (r && Rd32(r + 28)) n = std::min(n, Rd32(r + 28));
  int32_t x = 0;
  uint32_t fit = 0;
  std::vector<int32_t> dx(n);
  std::vector<uint16_t> glyphs(n);
  for (uint32_t i = 0; i < n; i++) {
    glyphs[i] = f->Glyph(&s[i], 1);
    dx[i] = f->Measure(glyphs[i]).advance;
    if ((flags & 0x100000) && uint32_t(x + dx[i]) > max_extent) break;  // GCP_MAXEXTENT
    x += dx[i];
    fit = i + 1;
  }
  if (r) {
    if (gaddr out = Rd32(r + 4))
      for (uint32_t i = 0; i < fit; i++) {
        bool ok;
        if (wide) Wr16(out + 2 * i, s[i]);
        else Wr8(out + i, win32::UnicodeToCp1252(s[i], &ok));
      }
    int32_t caret = 0;
    for (uint32_t i = 0; i < fit; i++) {
      if (gaddr o = Rd32(r + 8)) Wr32(o + 4 * i, i);
      if (gaddr d = Rd32(r + 12)) Wr32(d + 4 * i, uint32_t(dx[i]));
      if (gaddr p = Rd32(r + 16)) Wr32(p + 4 * i, uint32_t(caret));
      if (gaddr k = Rd32(r + 20)) Wr8(k + i, 1);  // GCPCLASS_LATIN
      if (gaddr g = Rd32(r + 24)) Wr16(g + 2 * i, glyphs[i]);
      caret += dx[i];
    }
    Wr32(r + 28, fit);
    Wr32(r + 32, fit);
  }
  return (uint32_t(x) & 0xFFFF) | uint32_t(f->metrics().height) << 16;
}
HLE("gdi32.dll", GetCharacterPlacementW, 6) {
  std::u16string s(size_t(Arg(c, 2)), u'\0');
  std::memcpy(s.data(), GPtr(Arg(c, 1)), s.size() * 2);
  return GetCharacterPlacementImpl(c, s, true);
}
HLE("gdi32.dll", GetCharacterPlacementA, 6) {
  std::u16string s;
  for (uint32_t i = 0; i < Arg(c, 2); i++) s.push_back(win32::Cp1252ToUnicode(Rd8(Arg(c, 1) + i)));
  return GetCharacterPlacementImpl(c, s, false);
}

// GetFontLanguageInfo: what GetCharacterPlacement does beyond one glyph
// per character, here nothing (0).
HLE("gdi32.dll", GetFontLanguageInfo, 1) { return 0; }

// TranslateCharsetInfo(src, &info, flags): TCI_SRCCHARSET (1) and
// TCI_SRCCODEPAGE (2), src being the value itself. CHARSETINFO: ciCharset,
// ciACP, FONTSIGNATURE {fsUsb[4], fsCsb[2]}.
HLE("gdi32.dll", TranslateCharsetInfo, 3) {
  struct Cs { uint32_t charset, cp, csb; };
  static const Cs kTable[] = {
      {0, 1252, 1u << 0},     {238, 1250, 1u << 1},  {204, 1251, 1u << 2},  {161, 1253, 1u << 3},
      {162, 1254, 1u << 4},   {177, 1255, 1u << 5},  {178, 1256, 1u << 6},  {186, 1257, 1u << 7},
      {163, 1258, 1u << 8},   {222, 874, 1u << 16},  {128, 932, 1u << 17},  {134, 936, 1u << 18},
      {129, 949, 1u << 19},   {136, 950, 1u << 20},  {130, 1361, 1u << 21}, {2, 42, 1u << 31},
  };
  const uint32_t src = uint32_t(Arg(c, 0)), flags = uint32_t(Arg(c, 2));
  const gaddr out = Arg(c, 1);
  if (flags != 1 && flags != 2) FatalF("TranslateCharsetInfo: flags %u not modelled", flags);
  for (const Cs& e : kTable)
    if ((flags == 1 ? e.charset : e.cp) == src) {
      std::memset(GPtr(out), 0, 32);
      Wr32(out, e.charset);
      Wr32(out + 4, e.cp);
      Wr32(out + 24, e.csb);
      return 1;
    }
  return 0;
}

// ExtTextOut(hdc, x, y, options, rect, str, count, dx): draws into the DIB
// section selected into the DC (24 or 32 bits), in the DC's text colour,
// with its alignment and background mode; ETO_OPAQUE fills the rectangle,
// ETO_CLIPPED clips to it, ETO_GLYPH_INDEX takes glyph indices, `dx` the
// advances. Coverage blends the text colour over the pixels (no
// ClearType); the alpha byte of 32-bit pixels written is 0, as GDI's.
// Other bitmaps (or none): nothing is drawn, as before.
uint32_t ExtTextOutImpl(uint32_t hdc, int32_t x, int32_t y, uint32_t options, gaddr rect,
                        const std::u16string& s, gaddr dxs) {
  const bool glyph_indices = (options & 0x10) != 0;  // ETO_GLYPH_INDEX
  std::shared_ptr<GdiDib> dib;
  Dc dc;
  {
    std::lock_guard<std::mutex> l(g_mu);
    dc = DcOf(hdc);
    auto it = g_dibs.find(dc.bitmap);
    if (it != g_dibs.end()) dib = it->second;
  }
  if (!dib) return 1;
  if (dib->bpp != 32 && dib->bpp != 24)
    FatalF("ExtTextOut: into a %u-bit DIB section not modelled yet", dib->bpp);
  if (options & 0x2000) FatalF("ExtTextOut: ETO_PDY not modelled yet");
  auto f = NeedHostFont(hdc, "ExtTextOut");
  const host_font::Metrics& fm = f->metrics();

  std::vector<uint16_t> glyphs;
  for (char16_t ch : s) glyphs.push_back(glyph_indices ? uint16_t(ch) : f->Glyph(&ch, 1));
  std::vector<int32_t> adv(glyphs.size());
  int32_t width = 0;
  for (size_t i = 0; i < glyphs.size(); i++) {
    adv[i] = dxs ? int32_t(Rd32(dxs + 4 * i)) : f->Measure(glyphs[i]).advance;
    width += adv[i];
  }
  if (dc.align & 1) {  // TA_UPDATECP
    x = dc.x;
    y = dc.y;
  }
  switch (dc.align & 6) {
    case 2: x -= width; break;      // TA_RIGHT
    case 6: x -= width / 2; break;  // TA_CENTER
    default: break;
  }
  int32_t base;
  switch (dc.align & 24) {
    case 8: base = y - fm.descent; break;  // TA_BOTTOM
    case 24: base = y; break;              // TA_BASELINE
    default: base = y + fm.ascent; break;  // TA_TOP
  }

  const int32_t W = dib->width, H = int32_t(dib->rows());
  int32_t cx0 = 0, cy0 = 0, cx1 = W, cy1 = H;
  int32_t r[4] = {0, 0, 0, 0};
  if (rect) std::memcpy(r, GPtr(rect), 16);
  const uint32_t bpp = dib->bpp / 8;
  auto put = [&](int32_t px, int32_t py, uint32_t color, uint32_t cov) {
    if (px < cx0 || py < cy0 || px >= cx1 || py >= cy1 || !cov) return;
    uint8_t* p = GPtr(dib->Row(uint32_t(py)) + uint32_t(px) * bpp);
    const uint32_t rgb[3] = {(color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF};  // B, G, R
    for (int k = 0; k < 3; k++) p[k] = uint8_t(p[k] + (int32_t(rgb[k]) - p[k]) * int32_t(cov) / 255);
    if (bpp == 4) p[3] = 0;
  };
  auto fill = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    for (int32_t py = std::max(y0, cy0); py < std::min(y1, cy1); py++)
      for (int32_t px = std::max(x0, cx0); px < std::min(x1, cx1); px++) put(px, py, color, 255);
  };
  if (rect && (options & 2)) fill(r[0], r[1], r[2], r[3], dc.bk_color);  // ETO_OPAQUE
  if (rect && (options & 4)) {                                            // ETO_CLIPPED
    cx0 = std::max(cx0, r[0]);
    cy0 = std::max(cy0, r[1]);
    cx1 = std::min(cx1, r[2]);
    cy1 = std::min(cy1, r[3]);
  }
  if (dc.bk_mode == 2) fill(x, base - fm.ascent, x + width, base + fm.descent, dc.bk_color);
  int32_t pen = x;
  for (size_t i = 0; i < glyphs.size(); i++) {
    const host_font::GlyphMetrics m = f->Measure(glyphs[i]);
    if (m.box_w && m.box_h) {
      const std::vector<uint8_t> cov = f->Render(glyphs[i], m);
      const int32_t gx = pen + m.box_x, gy = base - m.box_y;
      for (uint32_t yy = 0; yy < m.box_h; yy++)
        for (uint32_t xx = 0; xx < m.box_w; xx++)
          put(gx + int32_t(xx), gy + int32_t(yy), dc.text_color, cov[size_t(yy) * m.box_w + xx]);
    }
    pen += adv[i];
  }
  if (dc.align & 1) {
    std::lock_guard<std::mutex> l(g_mu);
    DcOf(hdc).x = pen;
  }
  return 1;
}
std::u16string WideArg(gaddr p, uint32_t n) {
  std::u16string s(n, u'\0');
  if (n) std::memcpy(s.data(), GPtr(p), size_t(n) * 2);
  return s;
}
HLE("gdi32.dll", ExtTextOutW, 8) {
  return ExtTextOutImpl(uint32_t(Arg(c, 0)), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
                        uint32_t(Arg(c, 3)), Arg(c, 4), WideArg(Arg(c, 5), uint32_t(Arg(c, 6))),
                        Arg(c, 7));
}
HLE("gdi32.dll", ExtTextOutA, 8) {
  std::u16string s;
  const bool gi = (Arg(c, 3) & 0x10) != 0;
  for (uint32_t i = 0; i < Arg(c, 6); i++) {
    const uint8_t b = Rd8(Arg(c, 5) + i);
    s.push_back(gi ? char16_t(b) : win32::Cp1252ToUnicode(b));
  }
  return ExtTextOutImpl(uint32_t(Arg(c, 0)), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
                        uint32_t(Arg(c, 3)), Arg(c, 4), s, Arg(c, 7));
}
// TextOutW(hdc, x, y, str, count)
// TextOutW(hdc, x, y, str, count): ExtTextOut without options, rectangle
// or advances.
HLE("gdi32.dll", TextOutW, 5) {
  return ExtTextOutImpl(uint32_t(Arg(c, 0)), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), 0, 0,
                        WideArg(Arg(c, 3), uint32_t(Arg(c, 4))), 0);
}

HLE("gdi32.dll", GetPixel, 3) { return 0; }
HLE("gdi32.dll", DPtoLP, 3) { return 1; }

HLE("gdi32.dll", GetDIBits, 7) {
  WarnF("GetDIBits: no GDI surfaces; returning 0 lines\n");
  return 0;
}

}  // namespace

uint32_t NewGdiBitmap(GdiBitmap b) {
  const uint32_t h = NewGdi();
  std::lock_guard<std::mutex> l(g_mu);
  g_bitmaps[h] = std::make_shared<const GdiBitmap>(std::move(b));
  return h;
}
std::shared_ptr<const GdiBitmap> FindGdiBitmap(uint32_t h) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_bitmaps.find(h);
  return it == g_bitmaps.end() ? nullptr : it->second;
}

}  // namespace recomp::hle

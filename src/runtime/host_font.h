#pragma once
// The host's font engine, for GDI text (gdi32.cpp): fonts made from a
// LOGFONT, their metrics as GDI reports them, glyphs and coverage. macOS
// and iOS: CoreText (host_font_coretext.cpp). Other hosts have none yet:
// Available() is false and gdi32 keeps its stand-ins.
//
// Units are device pixels (GDI's MM_TEXT at 96 dpi): a font's em is
// CreateFont's character height. Coverage is 8-bit, 0 (none) to 255.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recomp::host_font {

bool Available();

struct Desc {
  std::u16string face;   // LOGFONT lfFaceName ("" : the default face)
  int32_t height = 0;    // lfHeight: < 0 em height, > 0 cell height, 0 default
  int32_t width = 0;     // lfWidth (0: the face's own; others are not modelled)
  int32_t weight = 400;  // lfWeight (0: 400)
  bool italic = false;
  bool antialias = true;  // from lfQuality: NONANTIALIASED_QUALITY turns it off
};

// TEXTMETRIC's values (integer pixels), from the face's OS/2 and hhea
// tables as GDI computes them.
struct Metrics {
  int32_t height, ascent, descent, internal_leading, external_leading;
  int32_t ave_char_width, max_char_width, weight;
  uint16_t first_char, last_char, default_char, break_char;
  bool italic, fixed_pitch;
  std::u16string face;  // the face actually used
};

// GLYPHMETRICS: the black box (inked pixels) relative to the pen position
// on the baseline, y up; advance is the pen's move. An empty glyph (a
// space) has a 0x0 box.
struct GlyphMetrics {
  int32_t box_x = 0, box_y = 0;  // gmptGlyphOrigin: left, top (y up)
  uint32_t box_w = 0, box_h = 0;
  int32_t advance = 0;           // gmCellIncX
};

class Font {
 public:
  virtual ~Font() = default;
  virtual const Metrics& metrics() const = 0;
  // Glyph index of a UTF-16 unit (surrogate pairs: pass both), 0 if the
  // face has none.
  virtual uint16_t Glyph(const char16_t* s, size_t n) const = 0;
  // Glyph indices of n UTF-16 units that are not surrogates, 0 where the
  // face has none.
  virtual void Glyphs(const char16_t* s, size_t n, uint16_t* out) const = 0;
  virtual GlyphMetrics Measure(uint16_t glyph) const = 0;
  // The glyph's black box as 8-bit coverage, rows top-down, box_w wide.
  virtual std::vector<uint8_t> Render(uint16_t glyph, const GlyphMetrics& m) const = 0;
};

// The font for `d`, its face matched by family name (as GDI's
// lfFaceName), else by full or PostScript name; it falls back to the
// host's default when not installed (the face used is in metrics().face).
std::shared_ptr<const Font> Create(const Desc& d);

// A font file (TrueType, OpenType, collections) made available to Create
// for this process (GDI's AddFontResourceEx(FR_PRIVATE)): the number of
// faces it holds, 0 if it cannot be read. UnregisterFile undoes it.
int RegisterFile(const std::string& host_path);
bool UnregisterFile(const std::string& host_path);

}  // namespace recomp::host_font

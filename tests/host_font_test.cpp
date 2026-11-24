// host_font.h on the host's font engine (macOS/iOS: CoreText): GDI-style
// metrics of a face every Mac has, glyph lookup, glyph boxes and coverage.
// Skipped where the host has no font engine.

#include <cstdio>

#include "runtime/host_font.h"

namespace hf = recomp::host_font;
static int g_fail = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_fail++;                                                  \
    }                                                            \
  } while (0)

int main() {
  if (!hf::Available()) {
    std::printf("host_font: no font engine on this host, skipped\n");
    return 0;
  }
  // An em of 16 px (lfHeight -16): the cell is taller by the internal
  // leading, and ascent + descent is the cell.
  hf::Desc d;
  d.face = u"Arial";
  d.height = -16;
  auto f = hf::Create(d);
  const hf::Metrics& m = f->metrics();
  CHECK(m.face == u"Arial");
  CHECK(m.ascent + m.descent == m.height);
  CHECK(m.height > 16 && m.height < 22);
  CHECK(m.internal_leading == m.height - 16);
  CHECK(m.ave_char_width > 4 && m.ave_char_width < 12);
  CHECK(!m.fixed_pitch);

  // A cell height of 20 (lfHeight 20) is that cell.
  d.height = 20;
  CHECK(hf::Create(d)->metrics().height == 20);

  // 'A': a glyph with a box inside the cell, coverage in it; ' ' has none.
  const char16_t A = u'A', sp = u' ', missing = 0xE000;  // private use
  const uint16_t ga = f->Glyph(&A, 1);
  CHECK(ga != 0);
  CHECK(f->Glyph(&missing, 1) == 0);
  const hf::GlyphMetrics gm = f->Measure(ga);
  CHECK(gm.box_w > 5 && gm.box_h > 8);
  CHECK(gm.box_y <= m.ascent && gm.box_y > 0);
  CHECK(gm.advance >= int32_t(gm.box_w) - 1);
  const auto cov = f->Render(ga, gm);
  CHECK(cov.size() == size_t(gm.box_w) * gm.box_h);
  unsigned full = 0;
  for (uint8_t v : cov) full += v > 200;
  CHECK(full > 10);
  const hf::GlyphMetrics gs = f->Measure(f->Glyph(&sp, 1));
  CHECK(gs.box_w == 0 && gs.advance > 0);

  // Bulk lookup agrees with single lookups.
  const char16_t s[3] = {u'A', u' ', 0xE000};
  uint16_t g[3];
  f->Glyphs(s, 3, g);
  CHECK(g[0] == ga && g[2] == 0);

  if (g_fail) return 1;
  std::printf("host_font: ok (Arial -16: cell %d, ascent %d, 'A' %ux%u)\n", m.height, m.ascent,
              gm.box_w, gm.box_h);
  return 0;
}

// Windows .bmp decoding (runtime/hle/bmp.h): bottom-up and top-down 24/32-
// bit files, row padding, rejected formats. Portable.

#include <cstdio>
#include <string>

#include "runtime/hle/bmp.h"

namespace bmp = recomp::hle::bmp;
static int g_fail = 0;
static void True(bool v, const char* what) {
  if (!v) {
    std::printf("FAIL %s\n", what);
    g_fail++;
  }
}
static void Put32(std::string& s, size_t o, uint32_t v) {
  for (int i = 0; i < 4; i++) s[o + i] = char(v >> (8 * i));
}
static void Put16(std::string& s, size_t o, uint16_t v) {
  s[o] = char(v);
  s[o + 1] = char(v >> 8);
}
// A w x h file; pixel (x, y) (top-left origin) gets B=x, G=y, R=7, A=9.
static std::string Make(int w, int h, int bpp, bool top_down) {
  const size_t stride = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
  std::string s(54 + stride * h, '\0');
  s[0] = 'B', s[1] = 'M';
  Put32(s, 2, uint32_t(s.size()));
  Put32(s, 10, 54);
  Put32(s, 14, 40);
  Put32(s, 18, uint32_t(w));
  Put32(s, 22, uint32_t(top_down ? -h : h));
  Put16(s, 26, 1);
  Put16(s, 28, uint16_t(bpp));
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      size_t o = 54 + stride * size_t(top_down ? y : h - 1 - y) + size_t(x) * bpp / 8;
      s[o] = char(x), s[o + 1] = char(y), s[o + 2] = 7;
      if (bpp == 32) s[o + 3] = 9;
    }
  return s;
}

int main() {
  for (int bpp : {24, 32})
    for (bool td : {false, true}) {
      bmp::Image im;
      std::string why;
      // 3 pixels wide: 24-bit rows are padded from 9 to 12 bytes.
      True(bmp::Decode(Make(3, 2, bpp, td), &im, &why), "decodes");
      True(im.width == 3 && im.height == 2 && im.bits == bpp, "size and depth");
      const uint32_t a = bpp == 32 ? 9 : 0xFF;
      True(im.bgra[0] == (0u | 0u << 8 | 7u << 16 | a << 24), "top-left pixel");
      True(im.bgra[5] == (2u | 1u << 8 | 7u << 16 | a << 24), "bottom-right pixel");
    }
  bmp::Image im;
  std::string why, s = Make(2, 2, 32, false);
  Put32(s, 30, 1);  // BI_RLE8
  True(!bmp::Decode(s, &im, &why) && why == "a compressed bitmap", "compressed rejected");
  True(!bmp::Decode(Make(2, 2, 32, false).substr(0, 60), &im, &why), "truncated rejected");
  std::printf("bmp: %d failures\n", g_fail);
  return g_fail ? 1 : 0;
}

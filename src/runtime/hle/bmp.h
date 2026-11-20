#pragma once
// Windows .bmp files (BITMAPFILEHEADER + BITMAPINFOHEADER, uncompressed 24-
// and 32-bit), decoded to top-down 32-bit BGRA rows, for LoadImage's
// LR_LOADFROMFILE bitmaps. Other formats are rejected (the caller makes
// that a named error). Pure; tested by tests/bmp_test.cpp.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace recomp::hle::bmp {

struct Image {
  int width = 0, height = 0;
  int bits = 0;                 // the file's bits per pixel
  std::vector<uint32_t> bgra;   // width * height, top row first
};

inline uint32_t U32(const std::string& d, size_t o) {
  uint32_t v;
  std::memcpy(&v, d.data() + o, 4);
  return v;
}
inline uint16_t U16(const std::string& d, size_t o) {
  uint16_t v;
  std::memcpy(&v, d.data() + o, 2);
  return v;
}

// false (with *why) for anything but an uncompressed 24/32-bit bitmap.
inline bool Decode(const std::string& d, Image* out, std::string* why) {
  if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return *why = "not a BMP file", false;
  const uint32_t pixels = U32(d, 10), hdr = U32(d, 14);
  if (hdr < 40) return *why = "an OS/2 bitmap header", false;
  int32_t w = int32_t(U32(d, 18)), h = int32_t(U32(d, 22));
  const uint16_t bpp = U16(d, 28);
  const uint32_t comp = U32(d, 30);
  if (comp != 0) return *why = "a compressed bitmap", false;
  if (bpp != 24 && bpp != 32) return *why = std::to_string(bpp) + "-bit pixels", false;
  if (w <= 0 || h == 0) return *why = "an empty bitmap", false;
  const bool top_down = h < 0;
  if (top_down) h = -h;
  const size_t stride = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
  if (pixels + stride * size_t(h) > d.size()) return *why = "a truncated bitmap", false;
  out->width = w;
  out->height = h;
  out->bits = bpp;
  out->bgra.resize(size_t(w) * size_t(h));
  for (int y = 0; y < h; y++) {
    const size_t row = pixels + stride * size_t(top_down ? y : h - 1 - y);
    for (int x = 0; x < w; x++) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(d.data()) + row + size_t(x) * bpp / 8;
      const uint32_t a = bpp == 32 ? p[3] : 0xFF;
      out->bgra[size_t(y) * size_t(w) + size_t(x)] = p[0] | p[1] << 8 | p[2] << 16 | a << 24;
    }
  }
  return true;
}

}  // namespace recomp::hle::bmp

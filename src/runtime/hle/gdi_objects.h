#pragma once
// GDI objects with real contents, shared by gdi32.cpp and user32 (LoadImage).
// So far: bitmaps loaded from files, as device-dependent bitmaps at the
// display's depth (32 bits), their pixels top-down BGRA.

#include <cstdint>
#include <memory>
#include <vector>

namespace recomp::hle {

struct GdiBitmap {
  int width = 0, height = 0;
  std::vector<uint32_t> bgra;
};

// A new GDI handle for `b`.
uint32_t NewGdiBitmap(GdiBitmap b);
// The bitmap behind a handle, or null.
std::shared_ptr<const GdiBitmap> FindGdiBitmap(uint32_t h);

}  // namespace recomp::hle

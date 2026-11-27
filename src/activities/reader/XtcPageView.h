#pragma once
#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace xtcview {
struct View {
  int x = 0, y = 0, width = 0, height = 0;
  int sourceWidth = 0, sourceHeight = 0;
  int originX = 0, originY = 0, scale = 1;  // zoomed window into the source (see zoom())
  int sourceX(int dx) const {
    return originX + (width > 1 ? int(int64_t(dx) * (sourceWidth - 1) / (width - 1)) : 0) / scale;
  }
  int sourceY(int dy) const {
    return originY + (height > 1 ? int(int64_t(dy) * (sourceHeight - 1) / (height - 1)) : 0) / scale;
  }
};
// 2x window into a fitted page. Clamps the requested top-left source pixel in place so the window stays on the page.
inline View zoom(View view, int& originX, int& originY) {
  originX = std::clamp(originX, 0, view.sourceWidth - (view.sourceWidth + 1) / 2);
  originY = std::clamp(originY, 0, view.sourceHeight - (view.sourceHeight + 1) / 2);
  view.originX = originX;
  view.originY = originY;
  view.scale = 2;
  return view;
}
// Preserve the complete page and leave the status overlay outside its viewport.
inline View fit(int sourceWidth, int sourceHeight, int screenWidth, int screenHeight, int top, int right, int bottom,
                int left) {
  View view;
  const int width = screenWidth - left - right, height = screenHeight - top - bottom;
  if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0) return view;
  view.sourceWidth = sourceWidth;
  view.sourceHeight = sourceHeight;
  view.width = std::min(width, sourceWidth);
  view.height = std::min(height, sourceHeight);
  if (int64_t(view.width) * sourceHeight > int64_t(view.height) * sourceWidth)
    view.width = std::max(1, int(int64_t(view.height) * sourceWidth / sourceHeight));
  else
    view.height = std::max(1, int(int64_t(view.width) * sourceHeight / sourceWidth));
  view.x = left + (width - view.width) / 2;
  view.y = top + (height - view.height) / 2;
  return view;
}
// XTC uses row-major inverted bits; XTCH stores two reversed column-major planes.
inline uint8_t pixel(const uint8_t* data, int width, int height, int depth, int x, int y) {
  if (depth == 2) {
    const size_t columnBytes = (size_t(height) + 7) / 8;
    const size_t offset = size_t(width - 1 - x) * columnBytes + y / 8;
    const unsigned shift = 7 - y % 8;
    return ((data[offset] >> shift) & 1) * 2 + ((data[size_t(width) * columnBytes + offset] >> shift) & 1);
  }
  return !((data[size_t(y) * ((size_t(width) + 7) / 8) + x / 8] >> (7 - x % 8)) & 1);
}
// Same result as drawPixel(view.x + dx, view.y + dy, true) for every black 1-bit source pixel, but written straight
// into the 1bpp panel framebuffer one byte at a time. orientation uses GfxRenderer::Orientation order (Portrait,
// LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise). Returns false, fb untouched, when out of memory.
inline bool blit1(const uint8_t* src, int srcWidth, const View& view, uint8_t* fb, int panelW, int panelH,
                  int fbStride, int orientation) {
  if (view.width <= 0 || view.height <= 0) return true;
  auto table = makeUniqueNoThrow<uint32_t[]>(size_t(view.width) + view.height);
  if (!table) return false;
  uint32_t* const col = table.get();       // source x per dx
  uint32_t* const row = col + view.width;  // source row offset per dy (no per-pixel int64 divides)
  const size_t srcStride = (size_t(srcWidth) + 7) / 8;
  for (int dx = 0; dx < view.width; ++dx) col[dx] = view.sourceX(dx);
  for (int dy = 0; dy < view.height; ++dy) row[dy] = view.sourceY(dy) * srcStride;
  // Inverse of rotateCoordinates: u = uStart + uStep * py is fixed per panel row, v = vStart + vStep * px steps along
  // it, and (dx, dy) = portrait ? (u, v) : (v, u).
  const bool portrait = orientation == 0 || orientation == 2;
  int uStart, uStep, vStart, vStep;
  switch (orientation) {
    case 0: uStart = panelH - 1 - view.x, uStep = -1, vStart = -view.y, vStep = 1; break;
    case 1: uStart = panelH - 1 - view.y, uStep = -1, vStart = panelW - 1 - view.x, vStep = -1; break;
    case 2: uStart = -view.x, uStep = 1, vStart = panelW - 1 - view.y, vStep = -1; break;
    default: uStart = -view.y, uStep = 1, vStart = -view.x, vStep = 1; break;
  }
  // Panel range [lo, hi] where start + step * p lands in [0, n), clamped to the panel like drawPixel's bounds check.
  auto span = [](int start, int step, int n, int size, int& lo, int& hi) {
    lo = step > 0 ? -start : start - (n - 1);
    hi = std::min(lo + n - 1, size - 1);
    lo = std::max(lo, 0);
  };
  int px0, px1, py0, py1;
  span(uStart, uStep, portrait ? view.width : view.height, panelH, py0, py1);
  span(vStart, vStep, portrait ? view.height : view.width, panelW, px0, px1);
  for (int py = py0; py <= py1; ++py) {
    const int u = uStart + uStep * py;
    uint8_t* const line = fb + size_t(py) * fbStride;
    for (int byte = px0 / 8; byte <= px1 / 8; ++byte) {
      uint8_t mask = 0;
      for (int bit = 0; bit < 8; ++bit) {
        const int px = byte * 8 + bit;
        if (px < px0 || px > px1) continue;
        const int v = vStart + vStep * px;
        const uint32_t sx = col[portrait ? u : v];
        if (!(src[row[portrait ? v : u] + sx / 8] & (0x80 >> sx % 8))) mask |= 0x80 >> bit;
      }
      line[byte] &= ~mask;
    }
  }
  return true;
}
}  // namespace xtcview

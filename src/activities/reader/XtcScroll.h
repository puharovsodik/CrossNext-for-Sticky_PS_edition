#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Continuous scroll through tall 1-bit XTC pages (webtoon strips). The position is one global source row
// y = page * pageHeight + rowInPage for the top screen row. No Arduino or GfxRenderer here, so host tests run it.
namespace xtcscroll {
// The last screen ends on the last row of the book.
inline int64_t clampY(int64_t y, uint32_t pageCount, int pageHeight, int viewHeight) {
  return std::clamp<int64_t>(y, 0, std::max<int64_t>(0, int64_t(pageCount) * pageHeight - viewHeight));
}
struct Row {
  uint32_t page;
  int row;
};
// Source page and row shown on screen row r.
inline Row sourceRow(int64_t y, int r, int pageHeight) {
  const int64_t g = y + r;
  return {uint32_t(g / pageHeight), int(g % pageHeight)};
}
// A finished drag scrolls only when it moved mostly vertically and past the tap slop; anything else stays a tap or a
// sideways page-turn swipe.
inline bool isScrollDrag(int dx, int dy, int slop) { return std::abs(dy) >= slop && std::abs(dy) > std::abs(dx); }
// At 2x any drag past the tap slop pans, whatever its direction.
inline bool isPanDrag(int dx, int dy, int slop) { return std::max(std::abs(dx), std::abs(dy)) >= slop; }
// Buttons and taps move 80% of the view, so the bottom fifth of the old screen stays visible for context.
inline int step(int viewHeight) { return viewHeight * 4 / 5; }
// Drag multiplier from finger speed: 1x up to 0.5 px/ms, linear to 3x at 2 px/ms, capped at 3x.
inline float flickMultiplier(int distance, unsigned long durationMs) {
  const float speed = float(distance < 0 ? -distance : distance) / float(std::max(durationMs, 1ul));
  return std::clamp(1.0f + (speed - 0.5f) * (2.0f / 1.5f), 1.0f, 3.0f);
}
inline constexpr auto reversedBits = [] {
  std::array<uint8_t, 256> table{};
  for (int i = 0; i < 256; ++i)
    for (int bit = 0; bit < 8; ++bit)
      if (i & (1 << bit)) table[i] |= uint8_t(0x80 >> bit);
  return table;
}();
// Book and 1x screen geometry, for the double-tap 2x zoom inside the strip.
struct Strip {
  int pageWidth, pageHeight;
  uint32_t pageCount;
  int viewWidth, viewHeight;
};
// At 2x the view shows (viewWidth + 1) / 2 x (viewHeight + 1) / 2 source pixels (rounded up so the last screen row
// still has a source row).
inline int zoomRows(int viewHeight) { return (viewHeight + 1) / 2; }
// x: source column at the left edge, a multiple of 4 (2 screen px) so each output byte comes from one source nibble.
// y: global source row at the top, the same space as the 1x scroll offset.
struct Zoom {
  int x;
  int64_t y;
};
inline Zoom clampZoom(int64_t x, int64_t y, const Strip& s) {
  const int64_t maxX = std::max(0, s.pageWidth - (s.viewWidth + 1) / 2) / 4 * 4;
  return {int(std::min((std::clamp<int64_t>(x, 0, maxX) + 2) / 4 * 4, maxX)),
          clampY(y, s.pageCount, s.pageHeight, zoomRows(s.viewHeight))};
}
// Zoom window centred on the tapped point: screen column x, viewRow rows below the top of the 1x view at scrollY.
inline Zoom zoomAt(int x, int viewRow, int64_t scrollY, const Strip& s) {
  return clampZoom(x - s.viewWidth / 4, scrollY + viewRow - s.viewHeight / 4, s);
}
// The content follows the finger: a drag of (dx, dy) screen px moves the window half that many source px back.
inline Zoom pan(Zoom z, int dx, int dy, const Strip& s) { return clampZoom(z.x - dx / 2, z.y - dy / 2, s); }
// Leaving zoom: the 1x view centred on the zoom window, then clamped.
inline int64_t unzoomY(int64_t zoomY, const Strip& s) {
  return clampY(zoomY + zoomRows(s.viewHeight) / 2 - s.viewHeight / 2, s.pageCount, s.pageHeight, s.viewHeight);
}
// Source nibble (MSB first) to the output byte with every pixel doubled.
inline constexpr auto doubledNibble = [] {
  std::array<uint8_t, 16> table{};
  for (int n = 0; n < 16; ++n)
    for (int bit = 0; bit < 4; ++bit)
      if (n & (8 >> bit)) table[n] |= uint8_t(0xC0 >> (2 * bit));
  return table;
}();
// Writes screen rows [0, viewHeight) of the strip at y into a 1bpp panel framebuffer whose rows are exactly one
// source row (stride bytes) wide; both are MSB first, 1 = white. pages[0] is page y / pageHeight and pages[1] the
// next page (may be null when the view ends inside pages[0]); viewHeight <= pageHeight. Logical row top + r lands on
// the same panel row (LandscapeCounterClockwise), or rotated 180° (LandscapeClockwise) on panel row
// panelHeight - 1 - (top + r) with its bytes and bits reversed. Other framebuffer rows are not touched.
inline void blitRows(const uint8_t* const pages[2], int pageHeight, int64_t y, int viewHeight, int top, uint8_t* fb,
                     int stride, int panelHeight, bool rotated180) {
  const uint32_t first = uint32_t(y / pageHeight);
  for (int r = 0; r < viewHeight; ++r) {
    const auto [page, row] = sourceRow(y, r, pageHeight);
    const uint8_t* const src = pages[page - first] + size_t(row) * stride;
    if (!rotated180) {
      std::memcpy(fb + size_t(top + r) * stride, src, stride);
      continue;
    }
    uint8_t* const dst = fb + size_t(panelHeight - 1 - top - r) * stride;
    for (int b = 0; b < stride; ++b) dst[b] = reversedBits[src[stride - 1 - b]];
  }
}
// blitRows at 2x: screen pixel (x, r) shows source column zoomX + x / 2 of global row zoomY + r / 2. zoomX is a multiple
// of 4 and the window lies inside the book (clampZoom); pages[0] is page zoomY / pageHeight and pages[1] the next one.
inline void blitZoomRows(const uint8_t* const pages[2], int pageHeight, int zoomX, int64_t zoomY, int viewHeight,
                         int top, uint8_t* fb, int stride, int panelHeight, bool rotated180) {
  const uint32_t first = uint32_t(zoomY / pageHeight);
  for (int r = 0; r < viewHeight; ++r) {
    const auto [page, row] = sourceRow(zoomY, r / 2, pageHeight);
    const uint8_t* const src = pages[page - first] + size_t(row) * stride;
    uint8_t* const dst = fb + size_t(rotated180 ? panelHeight - 1 - top - r : top + r) * stride;
    for (int b = 0; b < stride; ++b) {
      const int n = zoomX / 4 + b;  // source nibble feeding output byte b
      const uint8_t out = doubledNibble[n % 2 ? src[n / 2] & 0x0F : src[n / 2] >> 4];
      if (rotated180)
        dst[stride - 1 - b] = reversedBits[out];
      else
        dst[b] = out;
    }
  }
}
}  // namespace xtcscroll

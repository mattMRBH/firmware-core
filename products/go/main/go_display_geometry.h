#ifndef GO_DISPLAY_GEOMETRY_H
#define GO_DISPLAY_GEOMETRY_H

#include <cstddef>
#include <cstdint>

namespace go_display_geometry {

inline constexpr size_t ROW_BYTES = 16;
inline constexpr size_t RENDER_ROWS = 256;
inline constexpr size_t PHYSICAL_ROWS = 250;
inline constexpr size_t RENDER_BYTES = ROW_BYTES * RENDER_ROWS;
inline constexpr size_t PHYSICAL_BYTES = ROW_BYTES * PHYSICAL_ROWS;
inline constexpr uint16_t BODY_Y = 18;
inline constexpr uint16_t BODY_HEIGHT = 232;

struct PartialRegion {
  uint16_t y;
  uint16_t height;
  size_t byte_offset;
  size_t byte_count;
};

constexpr PartialRegion partial_region(bool full_canvas) {
  const uint16_t y = full_canvas ? 0 : BODY_Y;
  const uint16_t height = full_canvas ? static_cast<uint16_t>(PHYSICAL_ROWS) : BODY_HEIGHT;
  return {y, height, static_cast<size_t>(y) * ROW_BYTES, static_cast<size_t>(height) * ROW_BYTES};
}

static_assert(RENDER_BYTES == 4096);
static_assert(PHYSICAL_BYTES == 4000);
static_assert(BODY_Y + BODY_HEIGHT == PHYSICAL_ROWS);
static_assert(partial_region(false).byte_offset + partial_region(false).byte_count ==
              PHYSICAL_BYTES);
static_assert(partial_region(true).byte_count == PHYSICAL_BYTES);

} // namespace go_display_geometry

#endif // GO_DISPLAY_GEOMETRY_H

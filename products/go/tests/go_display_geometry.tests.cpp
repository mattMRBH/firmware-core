#include <catch2/catch_test_macros.hpp>

#include "go_display_geometry.h"

TEST_CASE("Display geometry selects the body-only partial span") {
  const auto region = go_display_geometry::partial_region(false);

  CHECK(region.y == 18);
  CHECK(region.height == 232);
  CHECK(region.byte_offset == 288);
  CHECK(region.byte_count == 3712);
  CHECK(region.byte_offset + region.byte_count == go_display_geometry::PHYSICAL_BYTES);
}

TEST_CASE("Display geometry selects the full-canvas partial span") {
  const auto region = go_display_geometry::partial_region(true);

  CHECK(region.y == 0);
  CHECK(region.height == 250);
  CHECK(region.byte_offset == 0);
  CHECK(region.byte_count == 4000);
  CHECK(region.byte_count < go_display_geometry::RENDER_BYTES);
}

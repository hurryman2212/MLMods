#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>

#include "Geometry.hpp"

int run_geometry_tests() {
  int passed{};
  const auto check = [&passed](bool value, const char *name) {
    if (!value)
      throw std::runtime_error(name);
    ++passed;
  };
  ManorGeometry geometry;
  geometry.complete = true;
  geometry.module_quads.push_back(
      {{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}});
  geometry.module_quads.push_back(
      {{{18, 0, 0}, {22, 0, 0}, {22, 20, 0}, {18, 20, 0}}});
  const auto collision = [&geometry](std::array<Vec3, 3> triangle) {
    return triangles_intersect_manor_modules(geometry, triangle);
  };
  try {
    check(point_in_manor_modules(geometry, {5, 5, 0}), "inside module");
    check(point_in_manor_modules(geometry, {0, 0, 0}), "module corner");
    check(!point_in_manor_modules(geometry, {15, 15, 0}), "empty courtyard");
    check(point_in_manor_modules(geometry, {21.9, 10, 0}), "wall width");
    check(!point_in_manor_modules(geometry, {22.1, 10, 0}), "outside wall");
    check(segment_intersects_manor_modules(geometry, {-1, 5, 0}, {11, 5, 0}) ==
              GeometryVerdict::Blocked,
          "segment crosses module");
    check(segment_intersects_manor_modules(
              geometry, {12, 12, 0}, {16, 16, 0}) == GeometryVerdict::Clear,
          "segment in courtyard");
    check(segment_intersects_manor_modules(geometry, {15, 5, 0}, {25, 5, 0}) ==
              GeometryVerdict::Blocked,
          "segment crosses wall");
    check(segment_intersects_manor_modules(geometry, {10.5, 2, 0}, {10.5, 8, 0},
                                           1) == GeometryVerdict::Blocked,
          "segment width clips module");
    check(segment_intersects_manor_modules(geometry, {12, 2, 0}, {12, 8, 0},
                                           1) == GeometryVerdict::Clear,
          "segment width clear");
    check(segment_intersects_manor_modules(geometry, {12, 2, 0}, {12, 2, 0},
                                           1) == GeometryVerdict::Unknown,
          "degenerate wide segment");
    check(point_in_manor_modules(geometry, {20, 0, 0}), "wall end edge");
    check(!point_in_manor_modules(geometry, {20, -1, 0}), "past wall end edge");
    check(collision({{{1, 1, 0}, {2, 1, 0}, {1, 2, 0}}}) ==
              GeometryVerdict::Blocked,
          "triangle inside module");
    check(collision({{{-100, -100, 0}, {100, -100, 0}, {0, 100, 0}}}) ==
              GeometryVerdict::Blocked,
          "module inside triangle");
    check(collision({{{-1, 5, 0}, {11, 5, 0}, {5, 12, 0}}}) ==
              GeometryVerdict::Blocked,
          "edge crossing module");
    check(collision({{{12, 12, 0}, {14, 12, 0}, {12, 14, 0}}}) ==
              GeometryVerdict::Clear,
          "empty courtyard triangle");
    check(collision({{{19, 5, 0}, {21, 5, 0}, {20, 7, 0}}}) ==
              GeometryVerdict::Blocked,
          "triangle across wall");
    check(collision({{{17, 4, 0}, {23, 4, 0}, {23, 5, 0}}}) ==
              GeometryVerdict::Blocked,
          "wall crossed with all vertices outside");
    check(collision({{{-1, -3, 0}, {41, -3, 0}, {20, 23, 0}}}) ==
              GeometryVerdict::Blocked,
          "wall inside triangle");
    check(collision({{{19, -1, 0}, {21, -1, 0}, {20, 1, 0}}}) ==
              GeometryVerdict::Blocked,
          "wall end edge overlap");
    check(collision({{{24, 4, 0}, {26, 4, 0}, {25, 6, 0}}}) ==
              GeometryVerdict::Clear,
          "clear of wall width");
    check(collision({{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}}) ==
              GeometryVerdict::Unknown,
          "degenerate candidate fails closed");
    check(triangles_intersect_manor_modules(geometry, {}) ==
              GeometryVerdict::Unknown,
          "empty candidate fails closed");
    const std::array<Vec3, 2> malformed{{{0, 0, 0}, {1, 1, 0}}};
    check(triangles_intersect_manor_modules(geometry, malformed) ==
              GeometryVerdict::Unknown,
          "malformed triangle count");
    geometry.complete = false;
    check(collision({{{12, 12, 0}, {14, 12, 0}, {12, 14, 0}}}) ==
              GeometryVerdict::Unknown,
          "unknown module cannot permit placement");
    geometry.complete = true;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check(!point_in_manor_modules(geometry, {nan, 0, 0}), "NaN point");
    check(collision({{{nan, 0, 0}, {1, 0, 0}, {0, 1, 0}}}) ==
              GeometryVerdict::Unknown,
          "NaN candidate");
    geometry.module_quads.clear();
    geometry.module_quads.push_back(
        {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}});
    check(!point_in_manor_modules(geometry, {1, 0, 0}), "degenerate quad");
    check(!point_in_manor_modules(geometry, {3, 0, 0}),
          "outside degenerate quad");
    std::printf("Geometry checks passed: %d\n", passed);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Geometry check failed: %s\n", error.what());
    return 1;
  }
}

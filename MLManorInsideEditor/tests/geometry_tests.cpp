#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>

#include "Geometry.hpp"

// Independent copies of the pre-optimization predicates. The generated tests
// compare classifications, not approximately equal coordinates.
static double legacy_cross(const Vec3 &a, const Vec3 &b, const Vec3 &p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

static bool legacy_triangle(const Vec3 &p, const Vec3 &a, const Vec3 &b,
                            const Vec3 &c) {
  const float x = static_cast<float>(legacy_cross(a, b, p));
  const float y = static_cast<float>(legacy_cross(b, c, p));
  const float z = static_cast<float>(legacy_cross(c, a, p));
  if (legacy_cross(a, b, c) == 0.0)
    return false;
  return !((x < 0 || y < 0 || z < 0) && (x > 0 || y > 0 || z > 0));
}

static bool legacy_segment(const Vec3 &a, const Vec3 &b, const Vec3 &c,
                           const Vec3 &d) {
  if (std::max(a.x, b.x) < std::min(c.x, d.x) ||
      std::max(c.x, d.x) < std::min(a.x, b.x) ||
      std::max(a.y, b.y) < std::min(c.y, d.y) ||
      std::max(c.y, d.y) < std::min(a.y, b.y))
    return false;
  const double x = legacy_cross(a, b, c), y = legacy_cross(a, b, d);
  const double z = legacy_cross(c, d, a), w = legacy_cross(c, d, b);
  return ((x <= 0 && y >= 0) || (x >= 0 && y <= 0)) &&
         ((z <= 0 && w >= 0) || (z >= 0 && w <= 0));
}

static bool legacy_finite(const Vec3 &point) {
  return std::isfinite(point.x) && std::isfinite(point.y) &&
         std::isfinite(point.z);
}

static bool legacy_point(const ManorGeometry &geometry, const Vec3 &point) {
  if (!legacy_finite(point))
    return false;
  for (const auto &quad : geometry.module_quads)
    if (legacy_triangle(point, quad[0], quad[1], quad[2]) ||
        legacy_triangle(point, quad[0], quad[2], quad[3]))
      return true;
  return false;
}

static GeometryVerdict legacy_triangles(const ManorGeometry &geometry,
                                        std::span<const Vec3> triangles) {
  if (!geometry.complete || triangles.empty() || triangles.size() % 3 != 0 ||
      !std::all_of(triangles.begin(), triangles.end(), legacy_finite))
    return GeometryVerdict::Unknown;
  for (std::size_t i = 0; i < triangles.size(); i += 3) {
    const auto triangle = triangles.subspan(i, 3);
    if (legacy_cross(triangle[0], triangle[1], triangle[2]) == 0.0)
      return GeometryVerdict::Unknown;
    for (const auto &point : triangle)
      if (legacy_point(geometry, point))
        return GeometryVerdict::Blocked;
    for (const auto &quad : geometry.module_quads) {
      for (const auto &point : quad)
        if (legacy_triangle(point, triangle[0], triangle[1], triangle[2]))
          return GeometryVerdict::Blocked;
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t b = 0; b < 4; ++b)
          if (legacy_segment(triangle[a], triangle[(a + 1) % 3], quad[b],
                             quad[(b + 1) % 4]))
            return GeometryVerdict::Blocked;
    }
  }
  return GeometryVerdict::Clear;
}

static GeometryVerdict legacy_segments(const ManorGeometry &geometry,
                                       const Vec3 &a, const Vec3 &b,
                                       double width = 0.0) {
  if (!geometry.complete || !legacy_finite(a) || !legacy_finite(b) ||
      !std::isfinite(width) || width < 0)
    return GeometryVerdict::Unknown;
  if (width > 0) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length = std::hypot(dx, dy);
    if (length == 0)
      return GeometryVerdict::Unknown;
    const double x = -dy * width / length, y = dx * width / length;
    const Vec3 left_a{a.x + x, a.y + y, a.z};
    const Vec3 right_a{a.x - x, a.y - y, a.z};
    const Vec3 left_b{b.x + x, b.y + y, b.z};
    const Vec3 right_b{b.x - x, b.y - y, b.z};
    const std::array<Vec3, 6> triangles{left_a, left_b,  right_b,
                                        left_a, right_b, right_a};
    return legacy_triangles(geometry, triangles);
  }
  if (legacy_point(geometry, a) || legacy_point(geometry, b))
    return GeometryVerdict::Blocked;
  for (const auto &quad : geometry.module_quads)
    for (std::size_t i = 0; i < quad.size(); ++i)
      if (legacy_segment(a, b, quad[i], quad[(i + 1) % quad.size()]))
        return GeometryVerdict::Blocked;
  return GeometryVerdict::Clear;
}

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

    std::mt19937_64 random(0x4b89040); // Fixed test seed, not a native RVA.
    const auto coordinate = [&]() {
      return (static_cast<double>(random() % 1000001) - 500000) / 100.0;
    };
    for (unsigned sample = 0; sample < 12000; ++sample) {
      ManorGeometry fixture;
      fixture.complete = sample % 97 != 0;
      const double scale = std::pow(10.0, static_cast<int>(sample % 61) - 30);
      for (unsigned module = 0; module < 4; ++module) {
        std::array<Vec3, 4> quad;
        for (auto &point : quad)
          point = {coordinate() * scale, coordinate() * scale, 0};
        if (sample % 13 == 0)
          quad[1] = quad[0];
        if (sample % 71 == 0)
          quad.fill(quad[0]);
        fixture.module_quads.push_back(quad);
      }
      prepare_manor_geometry_bounds(fixture);
      const Vec3 a{coordinate() * scale, coordinate() * scale, 0};
      const Vec3 b{coordinate() * scale, coordinate() * scale, 0};
      const Vec3 c{coordinate() * scale, coordinate() * scale, 0};
      const std::array triangles{a, b, c};
      check(point_in_manor_modules(fixture, a) == legacy_point(fixture, a),
            "indexed point exactly matches legacy over varied scales");
      check(segment_intersects_manor_modules(fixture, a, b) ==
                legacy_segments(fixture, a, b),
            "indexed segment exactly matches legacy over varied scales");
      check(triangles_intersect_manor_modules(fixture, triangles) ==
                legacy_triangles(fixture, triangles),
            "indexed triangles exactly match legacy over varied scales");
      check(segment_intersects_manor_modules(fixture, a, b, scale) ==
                legacy_segments(fixture, a, b, scale),
            "indexed thick segment exactly matches legacy");
      for (const auto &quad : fixture.module_quads) {
        const auto point = quad[1];
        check(point_in_manor_modules(fixture, point) ==
                  legacy_point(fixture, point),
              "boundary vertex matches native acceptance");
        const Vec3 near{std::nextafter(point.x, -INFINITY),
                        std::nextafter(point.y, INFINITY), 0};
        check(point_in_manor_modules(fixture, near) ==
                  legacy_point(fixture, near),
              "one-ULP boundary displacement matches native acceptance");
      }
      fixture.module_bounds.clear();
      check(triangles_intersect_manor_modules(fixture, triangles) ==
                legacy_triangles(fixture, triangles),
            "hand-built snapshot without bounds keeps exact legacy result");
    }

    for (const double scale :
         {1e-160, 1e-30, 1e-23, 1e-20, 1.0, 1e80, 1e150, 1e200}) {
      ManorGeometry fixture;
      fixture.complete = true;
      fixture.module_quads.push_back(
          {{{0, 0, 0}, {scale, 0, 0}, {scale, scale, 0}, {0, scale, 0}}});
      prepare_manor_geometry_bounds(fixture);
      const Vec3 outside{2 * scale, 2 * scale, 0};
      check(point_in_manor_modules(fixture, outside) ==
                legacy_point(fixture, outside),
            "float underflow and double overflow preserve legacy point");
      check(segment_intersects_manor_modules(fixture, outside, outside) ==
                legacy_segments(fixture, outside, outside),
            "extreme zero-length segment keeps legacy classification");
      const std::array triangles{outside, Vec3{3 * scale, 2 * scale, 0},
                                 Vec3{2 * scale, 3 * scale, 0}};
      check(triangles_intersect_manor_modules(fixture, triangles) ==
                legacy_triangles(fixture, triangles),
            "extreme triangles keep legacy classification");
    }

    for (const double value :
         {nan, static_cast<double>(INFINITY), -static_cast<double>(INFINITY)}) {
      ManorGeometry fixture;
      fixture.complete = true;
      fixture.module_quads.push_back(
          {{{value, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, value}}});
      prepare_manor_geometry_bounds(fixture);
      const Vec3 a{100, 100, 0}, b{110, 100, 0}, c{100, 110, 0};
      const std::array triangle{a, b, c};
      check(point_in_manor_modules(fixture, a) == legacy_point(fixture, a),
            "nonfinite module data retains the old point predicate");
      check(triangles_intersect_manor_modules(fixture, triangle) ==
                legacy_triangles(fixture, triangle),
            "nonfinite module data retains the old triangle predicate");
      check(segment_intersects_manor_modules(fixture, a, b) ==
                legacy_segments(fixture, a, b),
            "nonfinite module data retains the old segment predicate");
    }

    ManorGeometry overflowing_products;
    overflowing_products.complete = true;
    overflowing_products.module_quads.push_back(
        {{{0, 0, 0}, {1e154, 1e154, 0}, {1e154, 1.5e154, 0}, {0, 0, 0}}});
    prepare_manor_geometry_bounds(overflowing_products);
    const Vec3 overflowing_point{5e154, 4e154, 0};
    const Vec3 overflowing_end{6e154, 4e154, 0};
    const std::array overflowing_triangle{overflowing_point, overflowing_end,
                                          Vec3{5e154, 5e154, 0}};
    check(legacy_point(overflowing_products, overflowing_point),
          "finite extreme fixture reproduces legacy NaN/Inf sign acceptance");
    check(point_in_manor_modules(overflowing_products, overflowing_point) ==
              legacy_point(overflowing_products, overflowing_point),
          "finite coordinates with overflowing products retain point result");
    check(segment_intersects_manor_modules(
              overflowing_products, overflowing_point, overflowing_end) ==
              legacy_segments(overflowing_products, overflowing_point,
                              overflowing_end),
          "finite coordinates with overflowing products retain segment result");
    check(
        triangles_intersect_manor_modules(overflowing_products,
                                          overflowing_triangle) ==
            legacy_triangles(overflowing_products, overflowing_triangle),
        "finite coordinates with overflowing products retain triangle result");

    ManorGeometry ordering;
    ordering.complete = true;
    ordering.module_quads.push_back(
        {{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}});
    prepare_manor_geometry_bounds(ordering);
    std::array<Vec3, 6> ordered{{{1, 1, 0},
                                 {2, 1, 0},
                                 {1, 2, 0},
                                 {100, 100, 0},
                                 {100, 100, 0},
                                 {100, 100, 0}}};
    check(triangles_intersect_manor_modules(ordering, ordered) ==
              GeometryVerdict::Blocked,
          "early blocked triangle still precedes later degenerate Unknown");
    std::rotate(ordered.begin(), ordered.begin() + 3, ordered.end());
    check(triangles_intersect_manor_modules(ordering, ordered) ==
              GeometryVerdict::Unknown,
          "early degenerate Unknown still precedes later blocked triangle");
    ordered.back().z = nan;
    check(triangles_intersect_manor_modules(ordering, ordered) ==
              GeometryVerdict::Unknown,
          "whole-input finite validation remains before every early result");

    for (unsigned sample = 0; sample < 2000; ++sample) {
      const double offset = std::pow(10.0, 3.0 + sample % 14);
      const double width =
          std::ldexp(offset, -static_cast<int>(2 + sample % 48));
      ManorGeometry fixture;
      fixture.complete = true;
      fixture.module_quads.push_back(
          {{{offset, offset, 0},
            {offset + width, offset + width, 0},
            {offset + width, offset + width * 1.01, 0},
            {offset, offset + width * 0.01, 0}}});
      prepare_manor_geometry_bounds(fixture);
      const Vec3 point{offset + width * (coordinate() / 1000.0),
                       offset + width * (coordinate() / 1000.0), 0};
      check(point_in_manor_modules(fixture, point) ==
                legacy_point(fixture, point),
            "large translated thin wall preserves cancellation-edge signs");
    }

    ManorGeometry many_walls;
    many_walls.complete = true;
    for (unsigned index = 0; index < 1600; ++index) {
      const double x = 1000 + (index % 40) * 100.0;
      const double y = 1000 + (index / 40) * 100.0;
      many_walls.module_quads.push_back(
          {{{x, y, 0}, {x + 80, y, 0}, {x + 80, y + 50, 0}, {x, y + 50, 0}}});
    }
    prepare_manor_geometry_bounds(many_walls);
    std::array<Vec3, 3> query{{{-100, -100, 0}, {100, -100, 0}, {0, 100, 0}}};
    unsigned legacy_total{}, indexed_total{};
    const auto legacy_start = std::chrono::steady_clock::now();
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
      query[0].x = -100.0 - repeat * 0.01;
      legacy_total +=
          static_cast<unsigned>(legacy_triangles(many_walls, query));
    }
    const auto indexed_start = std::chrono::steady_clock::now();
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
      query[0].x = -100.0 - repeat * 0.01;
      indexed_total += static_cast<unsigned>(
          triangles_intersect_manor_modules(many_walls, query));
    }
    const auto finish = std::chrono::steady_clock::now();
    check(legacy_total == indexed_total,
          "large Manor benchmark preserves every collision verdict");
    std::printf(
        "Geometry benchmark 1600 modules x 1000 triangles: "
        "legacy %.3f ms, indexed %.3f ms\n",
        std::chrono::duration<double, std::milli>(indexed_start - legacy_start)
            .count(),
        std::chrono::duration<double, std::milli>(finish - indexed_start)
            .count());
    std::printf("Geometry checks passed: %d\n", passed);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Geometry check failed: %s\n", error.what());
    return 1;
  }
}

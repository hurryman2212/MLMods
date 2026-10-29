#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct Vec3 {
  double x{};
  double y{};
  double z{};
};

template <typename T> struct NativeArray {
  T *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct ManorGeometry {
  std::vector<std::array<Vec3, 4>> module_quads;
  std::vector<void *> actors;
  bool complete{};
};

enum class GeometryVerdict { Unknown, Clear, Blocked };

ManorGeometry build_manor_geometry(void *manor, std::uintptr_t image);
bool point_in_manor_modules(const ManorGeometry &geometry, const Vec3 &point);
GeometryVerdict
triangles_intersect_manor_modules(const ManorGeometry &geometry,
                                  std::span<const Vec3> triangles);
GeometryVerdict segment_intersects_manor_modules(const ManorGeometry &geometry,
                                                 const Vec3 &a, const Vec3 &b,
                                                 double half_width = 0.0);
std::optional<bool> manor_component_hit(void *pawn, void *manor,
                                        const ManorGeometry &geometry,
                                        std::uintptr_t image);

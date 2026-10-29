#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <Windows.h>

#include "Geometry.hpp"

// Defensive read limits, not game rules. Unusual or corrupt arrays stay
// blocked.
static constexpr std::int32_t maximum_module_count = 4096;
static constexpr std::int32_t maximum_wall_sample_count = 65536;
static constexpr std::int32_t maximum_actor_tag_count = 1024;

static bool read_bytes(std::uintptr_t address, void *output, std::size_t size) {
  SIZE_T count{};
  return address &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &count) &&
         count == size;
}

template <typename T> static bool read_value(std::uintptr_t address, T &value) {
  return read_bytes(address, &value, sizeof(value));
}

static bool valid_object(std::uintptr_t object) {
  std::uint32_t flags{};
  // Native predicates reject 0x40000000. Also reject the UE destruction phases
  // RF_BeginDestroyed and RF_FinishDestroyed (0x8000 and 0x10000).
  return object && read_value(object + 8, flags) && !(flags & 0x40018000U);
}

static bool object_is_a(std::uintptr_t object, std::uintptr_t type) {
  std::uintptr_t object_class{}, table{}, ancestor{};
  std::int32_t index{}, count{};
  return object && type && read_value(object + 0x10, object_class) &&
         read_value(type + 0x38, index) && index >= 0 &&
         read_value(object_class + 0x38, count) && count >= index &&
         read_value(object_class + 0x30, table) &&
         read_value(table + static_cast<std::size_t>(index) * 8, ancestor) &&
         ancestor == type + 0x30;
}

static std::int32_t module_type_from_tag(std::wstring_view tag) {
  // Native 0x144cb23f0: case-insensitive Find("bType"), RightChop(6), _wtoi.
  // The match is not required at position zero, and RightChop is absolute.
  constexpr std::wstring_view marker = L"bType";
  const auto found = std::search(tag.begin(), tag.end(), marker.begin(),
                                 marker.end(), [](wchar_t a, wchar_t b) {
                                   if (a >= L'a' && a <= L'z')
                                     a -= L'a' - L'A';
                                   if (b >= L'a' && b <= L'z')
                                     b -= L'a' - L'A';
                                   return a == b;
                                 });
  if (found == tag.end() || tag.size() <= 6)
    return 0;
  const std::wstring digits(tag.substr(6));
  const auto value = _wtoi(digits.c_str());
  return value > 0 ? value : 0;
}

static std::int32_t snapshot_module_type(std::uintptr_t actor,
                                         std::uintptr_t image) {
  struct Name {
    std::uint32_t index, number;
  };
  NativeArray<Name> header{}, after{};
  if (!image || !valid_object(actor) || !read_value(actor + 0x1e0, header) ||
      header.count < 1 || header.count > maximum_actor_tag_count ||
      header.capacity < header.count || !header.data)
    return 0;
  std::vector<Name> names(static_cast<std::size_t>(header.count));
  if (!read_bytes(reinterpret_cast<std::uintptr_t>(header.data), names.data(),
                  names.size() * sizeof(Name)))
    return 0;
  std::int32_t result{};
  for (const auto &name : names) {
    const auto block_index = name.index >> 16;
    const auto byte_offset = (name.index & 0xffffU) * 2U;
    std::array<std::uint32_t, 2> pool_position{};
    std::uintptr_t block{};
    if (!read_value(image + 0x91d49c0 + 8, pool_position) ||
        pool_position[0] >= 8192 || block_index > pool_position[0] ||
        !read_value(image + 0x91d49c0 + 0x10 + block_index * 8ULL, block) ||
        !block)
      return 0;
    std::uint16_t entry_header{};
    if (!read_value(block + byte_offset, entry_header))
      return 0;
    const std::size_t length = entry_header >> 6;
    const bool wide = (entry_header & 1U) != 0;
    const std::size_t bytes = length * (wide ? 2U : 1U);
    if (length == 0 || byte_offset + 2 + bytes > 131072 ||
        (block_index == pool_position[0] &&
         byte_offset + 2 + bytes > pool_position[1]))
      return 0;
    std::wstring tag(length, L'\0');
    if (wide) {
      if (!read_bytes(block + byte_offset + 2, tag.data(), bytes))
        return 0;
    } else {
      std::string narrow(length, '\0');
      if (!read_bytes(block + byte_offset + 2, narrow.data(), bytes))
        return 0;
      for (std::size_t i = 0; i < length; ++i)
        tag[i] = static_cast<unsigned char>(narrow[i]);
    }
    if (name.number != 0)
      tag += L"_" + std::to_wstring(name.number - 1);
    result = module_type_from_tag(tag);
    if (result > 0)
      break;
  }
  // Actor tags can be reallocated or edited while the snap graph worker runs.
  // Only consume a stable private copy; never pass their live array to native
  // FName/FString code. A changing snapshot keeps the original restriction.
  std::vector<Name> verification(names.size());
  if (!read_value(actor + 0x1e0, after) || header.data != after.data ||
      header.count != after.count || header.capacity != after.capacity ||
      !read_bytes(reinterpret_cast<std::uintptr_t>(header.data),
                  verification.data(), verification.size() * sizeof(Name)) ||
      std::memcmp(names.data(), verification.data(),
                  names.size() * sizeof(Name)) != 0 ||
      !valid_object(actor))
    return 0;
  return result;
}

static bool finite_point(const Vec3 &point) {
  return std::isfinite(point.x) && std::isfinite(point.y) &&
         std::isfinite(point.z);
}

static double cross(const Vec3 &a, const Vec3 &b, const Vec3 &p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

static bool in_triangle(const Vec3 &p, const Vec3 &a, const Vec3 &b,
                        const Vec3 &c) {
  // The game's 0x144b4d1f0 computes with doubles then narrows the signs to
  // float.
  const float x = static_cast<float>(cross(a, b, p));
  const float y = static_cast<float>(cross(b, c, p));
  const float z = static_cast<float>(cross(c, a, p));
  if (cross(a, b, c) == 0.0)
    return false;
  return !((x < 0 || y < 0 || z < 0) && (x > 0 || y > 0 || z > 0));
}

static bool segments_intersect(const Vec3 &a, const Vec3 &b, const Vec3 &c,
                               const Vec3 &d) {
  if (std::max(a.x, b.x) < std::min(c.x, d.x) ||
      std::max(c.x, d.x) < std::min(a.x, b.x) ||
      std::max(a.y, b.y) < std::min(c.y, d.y) ||
      std::max(c.y, d.y) < std::min(a.y, b.y))
    return false;
  const double x = cross(a, b, c), y = cross(a, b, d);
  const double z = cross(c, d, a), w = cross(c, d, b);
  return ((x <= 0 && y >= 0) || (x >= 0 && y <= 0)) &&
         ((z <= 0 && w >= 0) || (z >= 0 && w <= 0));
}

ManorGeometry build_manor_geometry(void *manor_pointer, std::uintptr_t image) {
  ManorGeometry result;
  const auto manor = reinterpret_cast<std::uintptr_t>(manor_pointer);
  std::int32_t type{};
  NativeArray<void *> modules;
  if (!image || !valid_object(manor) || !read_value(manor + 0x3b8, type) ||
      type != 98 || !read_value(manor + 0xef0, modules) || modules.count <= 0 ||
      modules.capacity < modules.count ||
      modules.count > maximum_module_count || !modules.data)
    return result;
  result.actors.resize(static_cast<std::size_t>(modules.count));
  if (!read_bytes(reinterpret_cast<std::uintptr_t>(modules.data),
                  result.actors.data(),
                  result.actors.size() * sizeof(void *))) {
    result.actors.clear();
    return result;
  }
  using StaticClass = std::uintptr_t (*)();
  const auto road_class = reinterpret_cast<StaticClass>(image + 0x4ad99a0)();
  const auto mesh_class = reinterpret_cast<StaticClass>(image + 0x3ebbb80)();
  result.complete = true;
  for (void *pointer : result.actors) {
    const auto actor = reinterpret_cast<std::uintptr_t>(pointer);
    if (!valid_object(actor)) {
      result.complete = false;
      continue;
    }
    if (object_is_a(actor, road_class)) {
      NativeArray<Vec3> left, right;
      std::uint8_t road_type{}, wall_type{};
      std::int32_t center_count{};
      if (!read_value(actor + 0x2b4, road_type) ||
          !read_value(actor + 0x2b5, wall_type)) {
        result.complete = false;
        continue;
      }
      // Manor source arrays can contain ordinary road previews. They are not
      // physical walls and must not invalidate otherwise complete geometry.
      if (road_type <= 1 && wall_type == 0)
        continue;
      if (road_type != 3 || wall_type == 0 ||
          !read_value(actor + 0x3a0, center_count) ||
          !read_value(actor + 0x3d8, left) ||
          !read_value(actor + 0x3e8, right) || center_count < 2 ||
          center_count > maximum_wall_sample_count ||
          left.count != center_count || right.count != center_count ||
          left.capacity < left.count || right.capacity < right.count ||
          left.capacity > maximum_wall_sample_count ||
          right.capacity > maximum_wall_sample_count || !left.data ||
          !right.data) {
        result.complete = false;
        continue;
      }
      std::vector<Vec3> left_positions(static_cast<std::size_t>(left.count));
      std::vector<Vec3> right_positions(static_cast<std::size_t>(right.count));
      if (!read_bytes(reinterpret_cast<std::uintptr_t>(left.data),
                      left_positions.data(),
                      left_positions.size() * sizeof(Vec3)) ||
          !read_bytes(reinterpret_cast<std::uintptr_t>(right.data),
                      right_positions.data(),
                      right_positions.size() * sizeof(Vec3)) ||
          !std::all_of(left_positions.begin(), left_positions.end(),
                       finite_point) ||
          !std::all_of(right_positions.begin(), right_positions.end(),
                       finite_point)) {
        result.complete = false;
        continue;
      }
      // Native obstacle registration 0x144b889b0 triangulates these +3d8/+3e8
      // borders using +3a0 samples. +3a8/+3b8 are separate snap-side points.
      // The Road dispatch at 0x144ba31f0 does not exclude wall-type roads.
      for (std::size_t index = 1; index < left_positions.size(); ++index)
        result.module_quads.push_back(
            {left_positions[index - 1], left_positions[index],
             right_positions[index], right_positions[index - 1]});
    } else if (object_is_a(actor, mesh_class)) {
      const auto module_type = snapshot_module_type(actor, image);
      std::uintptr_t component{}, game{};
      Vec3 position, rotation;
      std::array<double, 4> quaternion;
      if (module_type <= 0 || !read_value(actor + 0x1b8, component) ||
          !component || !read_value(component + 0x1f0, position) ||
          !finite_point(position) ||
          !read_value(component + 0x1d0, quaternion) ||
          !std::all_of(quaternion.begin(), quaternion.end(),
                       [](double value) { return std::isfinite(value); }) ||
          !read_value(manor + 0x2e8, game) || !game) {
        result.complete = false;
        continue;
      }
      using ToRotator = Vec3 *(*)(const double *, Vec3 *);
      reinterpret_cast<ToRotator>(image + 0x11aaf40)(quaternion.data(),
                                                     &rotation);
      using Footprint =
          Vec3 *(*)(void *, Vec3 *, const Vec3 *, const Vec3 *, std::int32_t);
      std::array<Vec3, 4> corners;
      reinterpret_cast<Footprint>(image + 0x4ca9830)(
          reinterpret_cast<void *>(game), corners.data(), &position, &rotation,
          module_type);
      if (!std::all_of(corners.begin(), corners.end(), finite_point) ||
          (cross(corners[0], corners[1], corners[2]) == 0.0 &&
           cross(corners[0], corners[2], corners[3]) == 0.0)) {
        result.complete = false;
        continue;
      }
      result.module_quads.push_back(corners);
    } else {
      result.complete = false;
    }
  }
  return result;
}

bool point_in_manor_modules(const ManorGeometry &geometry, const Vec3 &point) {
  if (!finite_point(point))
    return false;
  for (const auto &quad : geometry.module_quads)
    if (in_triangle(point, quad[0], quad[1], quad[2]) ||
        in_triangle(point, quad[0], quad[2], quad[3]))
      return true;
  return false;
}

GeometryVerdict
triangles_intersect_manor_modules(const ManorGeometry &geometry,
                                  std::span<const Vec3> triangles) {
  if (!geometry.complete || triangles.empty() || triangles.size() % 3 != 0 ||
      !std::all_of(triangles.begin(), triangles.end(), finite_point))
    return GeometryVerdict::Unknown;
  for (std::size_t i = 0; i < triangles.size(); i += 3) {
    const auto triangle = triangles.subspan(i, 3);
    if (cross(triangle[0], triangle[1], triangle[2]) == 0.0)
      return GeometryVerdict::Unknown;
    for (const auto &point : triangle)
      if (point_in_manor_modules(geometry, point))
        return GeometryVerdict::Blocked;
    for (const auto &quad : geometry.module_quads) {
      for (const auto &point : quad)
        if (in_triangle(point, triangle[0], triangle[1], triangle[2]))
          return GeometryVerdict::Blocked;
      for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t b = 0; b < 4; ++b)
          if (segments_intersect(triangle[a], triangle[(a + 1) % 3], quad[b],
                                 quad[(b + 1) % 4]))
            return GeometryVerdict::Blocked;
    }
  }
  return GeometryVerdict::Clear;
}

GeometryVerdict segment_intersects_manor_modules(const ManorGeometry &geometry,
                                                 const Vec3 &a, const Vec3 &b,
                                                 double half_width) {
  if (!geometry.complete || !finite_point(a) || !finite_point(b) ||
      !std::isfinite(half_width) || half_width < 0)
    return GeometryVerdict::Unknown;
  if (half_width > 0) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length = std::hypot(dx, dy);
    if (length == 0)
      return GeometryVerdict::Unknown;
    const double x = -dy * half_width / length;
    const double y = dx * half_width / length;
    const Vec3 left_a{a.x + x, a.y + y, a.z};
    const Vec3 right_a{a.x - x, a.y - y, a.z};
    const Vec3 left_b{b.x + x, b.y + y, b.z};
    const Vec3 right_b{b.x - x, b.y - y, b.z};
    const std::array<Vec3, 6> triangles{left_a, left_b,  right_b,
                                        left_a, right_b, right_a};
    return triangles_intersect_manor_modules(geometry, triangles);
  }
  if (point_in_manor_modules(geometry, a) ||
      point_in_manor_modules(geometry, b))
    return GeometryVerdict::Blocked;
  for (const auto &quad : geometry.module_quads)
    for (std::size_t i = 0; i < quad.size(); ++i)
      if (segments_intersect(a, b, quad[i], quad[(i + 1) % quad.size()]))
        return GeometryVerdict::Blocked;
  return GeometryVerdict::Clear;
}

std::optional<bool> manor_component_hit(void *pawn_pointer, void *manor,
                                        const ManorGeometry &geometry,
                                        std::uintptr_t image) {
  const auto pawn = reinterpret_cast<std::uintptr_t>(pawn_pointer);
  std::array<double, 2> mouse;
  std::uintptr_t local_player{};
  if (!image || !valid_object(pawn) ||
      !read_value(pawn + 0x2d8, local_player) || !local_player ||
      !read_value(pawn + 0x490, mouse) || !std::isfinite(mouse[0]) ||
      !std::isfinite(mouse[1]))
    return std::nullopt;
  using Controller = void *(*)(void *, std::int32_t);
  void *controller =
      reinterpret_cast<Controller>(image + 0x38cabd0)(pawn_pointer, 0);
  if (!controller)
    return std::nullopt;
  alignas(16) std::array<std::byte, 0x120> hit{};
  using HitConstructor = void *(*)(void *);
  reinterpret_cast<HitConstructor>(image + 0x329a7f0)(hit.data());
  using ScreenTrace =
      bool (*)(void *, const double *, std::int32_t, bool, void *);
  if (!reinterpret_cast<ScreenTrace>(image + 0x3cba4c0)(
          controller, mouse.data(), 4, false, hit.data()))
    return false;
  using ActorFromHit = void *(*)(void *);
  void *actor =
      reinterpret_cast<ActorFromHit>(image + 0x388a950)(hit.data() + 0xb8);
  if (!actor)
    return false;
  if (std::find(geometry.actors.begin(), geometry.actors.end(), actor) !=
      geometry.actors.end())
    return true;
  using StaticClass = std::uintptr_t (*)();
  const auto part_class = reinterpret_cast<StaticClass>(image + 0x4aee150)();
  const auto address = reinterpret_cast<std::uintptr_t>(actor);
  void *owner{};
  return object_is_a(address, part_class) &&
         read_value(address + 0x2c8, owner) && owner == manor;
}

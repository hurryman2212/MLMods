#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>

#include <Windows.h>

#include "RoadPlacement.hpp"

static std::array<std::byte, 0x1200> pawn{}, region{}, manor{}, road{};
static std::array<std::byte, 8> game{}, cache{};
static std::array<Vec3, 6> captured_triangles{};
static bool complete = true, module_blocked = false;
static GeometryVerdict segment_verdict = GeometryVerdict::Clear;
static unsigned checks{}, native_calls{};
static unsigned geometry_calls{}, path_calls{};
static bool native_clear{}, nested_path{}, throw_path{}, in_path{};
static void *last_ignore{};
static std::uint64_t last_weak{};
static float last_width{};
static float last_radius{};
static unsigned circle_calls{};
static std::uint8_t last_flag{};

static void check(bool condition, const char *description) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    std::exit(1);
  }
}

template <typename T, std::size_t N>
static void put(std::array<std::byte, N> &target, std::size_t offset,
                const T &value) {
  std::memcpy(target.data() + offset, &value, sizeof(value));
}

static void *find_region(void *source, const Vec3 *, bool skip) {
  check(source == game.data() && !skip, "native region lookup arguments");
  return region.data();
}

static void assign_weak(std::uint64_t *output, void *actor) {
  check(actor == manor.data(), "only manor becomes ignored weak actor");
  *output = 0x12340000002aULL;
}

static std::uint8_t native_endpoint(void *source, const Vec3 *, float width,
                                    void *ignored) {
  check(source == game.data() && width == 50, "endpoint arguments preserved");
  ++native_calls;
  last_ignore = ignored;
  return native_clear || ignored == manor.data() ? 0 : 1;
}

static std::uint8_t native_point(void *source, const Vec3 *, std::int32_t width,
                                 std::uint64_t ignored) {
  check(source == cache.data() && width == 50, "point arguments preserved");
  ++native_calls;
  last_weak = ignored;
  return native_clear || ignored == 0x12340000002aULL ? 0 : 1;
}

static std::uint8_t native_segment(void *source, const double *, const double *,
                                   float width, void *ignored,
                                   std::uint8_t flag) {
  check(source == cache.data(), "segment cache preserved");
  ++native_calls;
  last_width = width;
  last_flag = flag;
  last_ignore = ignored;
  return native_clear || ignored == manor.data() ? 0 : 1;
}

static NativeArray<Vec3> *native_path(void *source, NativeArray<Vec3> *output,
                                      const Vec3 *start, const Vec3 *end) {
  ++path_calls;
  check(source == cache.data() && start->x == 1 && end->x == 100,
        "path query retains cache and endpoint arguments");
  if (throw_path)
    throw std::runtime_error("native path exception");
  if (nested_path && !in_path) {
    in_path = true;
    NativeArray<Vec3> nested;
    road_path_query(source, &nested, start, end, nullptr);
    check(nested.count == 1, "ineligible nested path masks outer policy");
    in_path = false;
  }
  bool blocked = road_path_point_collision(source, start, 50, 0xffffffff) != 0;
  for (unsigned i{}; i < 3; ++i)
    blocked = road_path_segment_collision(source, &start->x, &end->x, 10,
                                          nullptr, 1) != 0 ||
              blocked;
  output->count = blocked ? 1 : 2;
  return output;
}

static std::uint8_t native_circle(const Vec3 *point, float radius,
                                  const NativeArray<Vec3> *triangles) {
  check(point->x == 1 && point->y == 2 && triangles->count == 6,
        "circle receives point and actual module triangles");
  ++circle_calls;
  last_radius = radius;
  return module_blocked ? 1 : 0;
}

// Stub only geometry acquisition/evaluation here. The independent geometry
// tests exercise the real module/wall intersection implementation.
ManorGeometry build_manor_geometry(void *actor, std::uintptr_t image) {
  check(actor == manor.data() && image, "geometry uses original manor");
  ++geometry_calls;
  ManorGeometry result;
  result.complete = complete;
  result.module_quads.push_back(
      {{{100, 100, 0}, {200, 100, 0}, {200, 200, 0}, {100, 200, 0}}});
  return result;
}

bool point_in_manor_modules(const ManorGeometry &, const Vec3 &) {
  return module_blocked;
}

GeometryVerdict
triangles_intersect_manor_modules(const ManorGeometry &geometry,
                                  std::span<const Vec3> triangles) {
  check(triangles.size() == captured_triangles.size(),
        "road uses two triangles");
  std::copy(triangles.begin(), triangles.end(), captured_triangles.begin());
  return geometry.complete ? segment_verdict : GeometryVerdict::Unknown;
}

static void thunk(std::byte *image, std::uintptr_t offset,
                  const void *callback) {
  auto *page = image + (offset & ~std::uintptr_t{0xfff});
  check(VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == page,
        "commit isolated test code page");
  const std::array<std::uint8_t, 6> instruction{0xff, 0x25, 0, 0, 0, 0};
  std::memcpy(image + offset, instruction.data(), instruction.size());
  std::memcpy(image + offset + 6, &callback, sizeof(callback));
  check(FlushInstructionCache(GetCurrentProcess(), image + offset, 14),
        "flush isolated test code");
}

int main() {
  auto *image = static_cast<std::byte *>(
      VirtualAlloc(nullptr, 0x5000000, MEM_RESERVE, PAGE_NOACCESS));
  check(image != nullptr, "reserve isolated test image");
  thunk(image, 0x4cc9ab0, reinterpret_cast<const void *>(&find_region));
  thunk(image, 0x14938f0, reinterpret_cast<const void *>(&assign_weak));
  thunk(image, 0x4c90000, reinterpret_cast<const void *>(&native_endpoint));
  thunk(image, 0x4b9c590, reinterpret_cast<const void *>(&native_point));
  thunk(image, 0x4b9dd70, reinterpret_cast<const void *>(&native_segment));
  thunk(image, 0x4b16cf0, reinterpret_cast<const void *>(&native_circle));
  thunk(image, 0x4b90890, reinterpret_cast<const void *>(&native_path));
  configure_road_placement(reinterpret_cast<std::uintptr_t>(image));
  put(pawn, 0x34c, std::uint8_t{1});
  put(pawn, 0x7a0, std::uint8_t{1});
  put(pawn, 0x340, game.data());
  put(pawn, 0x778, road.data());
  put(region, 0x738, cache.data());
  put(region, 0x358, manor.data());
  put(manor, 0x3b8, std::int32_t{98});
  put(manor, 0x2e0, pawn.data());
  put(manor, 0x2e8, game.data());
  put(road, 0x2a8, game.data());
  put(pawn, 0x840, manor.data());
  put(pawn, 0x848, std::uint8_t{1});
  const auto initial_path_pawn = pawn;
  const auto initial_path_manor = manor;
  const auto initial_path_road = road;
  const auto initial_path_native = native_calls;
  const auto initial_path_geometry = geometry_calls;
  check(road_use_normal_obstacle_path(pawn.data()) &&
            road_normal_path_count() == 1,
        "valid CastlePlan ordinary road enters the native obstacle path");
  put(road, 0x2b4, std::uint8_t{1});
  check(road_use_normal_obstacle_path(pawn.data()) &&
            road_normal_path_count() == 2,
        "both native ordinary road types acquire the same obstacle path");
  road = initial_path_road;
  for (int invalid{}; invalid < 19; ++invalid) {
    pawn = initial_path_pawn;
    manor = initial_path_manor;
    road = initial_path_road;
    if (invalid == 0)
      put(pawn, 0x34c, std::uint8_t{0});
    else if (invalid == 1)
      put(pawn, 0x848, std::uint8_t{0});
    else if (invalid == 2)
      put(pawn, 0x7a0, std::uint8_t{0});
    else if (invalid == 3)
      put(pawn, 0x598, std::uint8_t{1});
    else if (invalid == 4)
      put(pawn, 0x7a1, std::uint8_t{1});
    else if (invalid == 5)
      put(pawn, 0x5f8, std::int32_t{98});
    else if (invalid == 6)
      put(pawn, 0xc58, std::uint8_t{1});
    else if (invalid == 7)
      put(pawn, 0x840, static_cast<void *>(nullptr));
    else if (invalid == 8)
      put(pawn, 0x778, static_cast<void *>(nullptr));
    else if (invalid == 9)
      put(manor, 0x3b8, std::int32_t{99});
    else if (invalid == 10)
      put(manor, 0x2e0, road.data());
    else if (invalid == 11)
      put(manor, 0x2e8, road.data());
    else if (invalid == 12)
      put(road, 0x2a8, road.data());
    else if (invalid == 13)
      put(road, 0x2b4, std::uint8_t{3});
    else if (invalid == 14)
      put(road, 0x2b5, std::uint8_t{1});
    else if (invalid == 15)
      put(pawn, 8, std::uint32_t{0x40000000});
    else if (invalid == 16)
      put(manor, 8, std::uint32_t{0x8000});
    else if (invalid == 17)
      put(road, 8, std::uint32_t{0x10000});
    else
      put(pawn, 0x840, reinterpret_cast<void *>(1));
    const auto before_pawn = pawn;
    const auto before_manor = manor;
    const auto before_road = road;
    check(
        !road_use_normal_obstacle_path(pawn.data()) &&
            road_normal_path_count() == 2,
        "unsupported or foreign CastlePlan state retains the original branch");
    check(pawn == before_pawn && manor == before_manor && road == before_road,
          "branch predicate never modifies tool, Manor or preview state");
  }
  pawn = initial_path_pawn;
  manor = initial_path_manor;
  road = initial_path_road;
  check(!road_use_normal_obstacle_path(nullptr),
        "null pawn keeps original branch");
  configure_road_placement(0);
  check(!road_use_normal_obstacle_path(pawn.data()),
        "missing image keeps original branch");
  configure_road_placement(reinterpret_cast<std::uintptr_t>(image));
  check(native_calls == initial_path_native &&
            geometry_calls == initial_path_geometry,
        "branch predicate performs no native queries or geometry capture");
  put(pawn, 0x840, static_cast<void *>(nullptr));
  put(pawn, 0x848, std::uint8_t{0});
  const auto original_manor = manor;
  const auto original_region = region;
  const auto original_road = road;
  const Vec3 point{1, 2, 3};
  const std::array<double, 2> a{0, 0}, b{100, 0};
  check(!road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "ordinary endpoint admitted");
  check(last_ignore == manor.data(), "endpoint ignores only manor");
  check(circle_calls == 1 && last_radius == 50,
        "endpoint preserves native full-radius collision test");
  check(
      !road_point_collision(cache.data(), &point, 50, 0xffffffff, pawn.data()),
      "ordinary point admitted");
  check(last_weak == 0x12340000002aULL, "weak object serial retained");
  check(circle_calls == 2 && last_radius == 50,
        "start point preserves native full-radius collision test");
  check(!road_segment_collision(cache.data(), a.data(), b.data(), 10, nullptr,
                                1, pawn.data()),
        "ordinary segment admitted");
  check(last_ignore == manor.data() && last_width == 10 && last_flag == 1,
        "segment ignore, width and check flag preserved");
  const std::array<Vec3, 6> expected{{{0, -10, 0},
                                      {0, 10, 0},
                                      {100, 10, 0},
                                      {0, -10, 0},
                                      {100, 10, 0},
                                      {100, -10, 0}}};
  check(std::memcmp(captured_triangles.data(), expected.data(),
                    sizeof(expected)) == 0,
        "native butt-ended road strip geometry");
  for (const auto offset : {0x598, 0x7a1, 0xc58}) {
    put(pawn, offset, std::uint8_t{1});
    check(
        road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "alternate tool, removal and platform retain native restrictions");
    check(last_ignore == nullptr,
          "excluded modes do not replace ignored actor");
    put(pawn, offset, std::uint8_t{0});
  }
  put(pawn, 0x840, manor.data());
  const auto castle_geometry = geometry_calls;
  check(!road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "CastlePlan ordinary road retains native current-manor ignore");
  check(last_ignore == manor.data() && geometry_calls == castle_geometry,
        "CastlePlan wall crossing never acquires ordinary wall veto");
  const auto castle_native = native_calls;
  check(!road_endpoint_collision(game.data(), &point, 50, manor.data(),
                                 pawn.data()) &&
            native_calls == castle_native + 1,
        "existing CastlePlan manor ignore is forwarded once");
  put(pawn, 0x840, road.data());
  check(road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "unrelated CastlePlan owner cannot ignore this region manor");
  put(pawn, 0x840, static_cast<void *>(nullptr));
  put(pawn, 0x5f8, std::int32_t{72});
  check(road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "ordinary building tool excluded");
  put(pawn, 0x5f8, std::int32_t{0});
  put(road, 0x2b4, std::uint8_t{3});
  check(road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "wall road type excluded");
  put(road, 0x2b4, std::uint8_t{0});
  put(road, 0x2b5, std::uint8_t{1});
  check(road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "fence wall type excluded");
  put(road, 0x2b5, std::uint8_t{0});
  complete = false;
  check(road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "unknown point geometry fails closed");
  check(road_segment_collision(cache.data(), a.data(), b.data(), 10, nullptr, 1,
                               pawn.data()),
        "unknown segment geometry fails closed");
  complete = true;
  module_blocked = true;
  const auto before = native_calls;
  check(road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()),
        "real module blocks point");
  check(native_calls == before + 1,
        "module block preserves native blocked probe");
  segment_verdict = GeometryVerdict::Blocked;
  check(road_segment_collision(cache.data(), a.data(), b.data(), 10, nullptr, 1,
                               pawn.data()),
        "real wall blocks segment");
  check(native_calls == before + 2,
        "wall block preserves native blocked probe");
  check(road_endpoint_collision(game.data(), &point, 50, road.data(),
                                pawn.data()),
        "pre-existing actor ignore preserved");
  check(last_ignore == road.data(), "never replaces another ignored actor");
  check(road_point_collision(cache.data(), &point, 50, 0x234500000008ULL,
                             pawn.data()),
        "pre-existing weak ignore preserved");
  check(last_weak == 0x234500000008ULL,
        "never replaces another weak reference");
  native_clear = true;
  const auto clear_geometry = geometry_calls;
  const auto clear_native = native_calls;
  check(
      !road_endpoint_collision(game.data(), &point, 50, nullptr, pawn.data()) &&
          !road_point_collision(cache.data(), &point, 50, 0, pawn.data()) &&
          !road_segment_collision(cache.data(), a.data(), b.data(), 10, nullptr,
                                  1, pawn.data()),
      "native clear probes never gain an extra conservative geometry veto");
  check(geometry_calls == clear_geometry && native_calls == clear_native + 3,
        "native clear path uses one call and no geometry capture per probe");
  native_clear = false;
  module_blocked = false;
  segment_verdict = GeometryVerdict::Clear;
  const Vec3 end{100, 2, 3};
  NativeArray<Vec3> output;
  const auto path_geometry = geometry_calls;
  check(road_path_query(cache.data(), &output, &point, &end, pawn.data()) ==
                &output &&
            output.count == 2,
        "native fallback path's probes ignore only false manor footprint");
  check(geometry_calls == path_geometry + 1,
        "fallback query captures physical geometry once for all four probes");
  segment_verdict = GeometryVerdict::Blocked;
  road_path_query(cache.data(), &output, &point, &end, pawn.data());
  check(output.count == 1, "ordinary fallback path still respects real walls");
  put(pawn, 0x840, manor.data());
  const auto castle_path_geometry = geometry_calls;
  road_path_query(cache.data(), &output, &point, &end, pawn.data());
  check(output.count == 2 && geometry_calls == castle_path_geometry,
        "CastlePlan fallback preserves native gate-crossing exception");
  put(region, 0x738, game.data());
  road_path_query(cache.data(), &output, &point, &end, pawn.data());
  check(output.count == 1,
        "CastlePlan fallback with a different region cache stays native");
  put(region, 0x738, cache.data());
  put(pawn, 0x840, road.data());
  road_path_query(cache.data(), &output, &point, &end, pawn.data());
  check(output.count == 1,
        "CastlePlan fallback cannot ignore another Manor's walls");
  put(pawn, 0x840, static_cast<void *>(nullptr));
  segment_verdict = GeometryVerdict::Clear;
  nested_path = true;
  road_path_query(cache.data(), &output, &point, &end, pawn.data());
  check(output.count == 2,
        "eligible context resumes after nested native query");
  nested_path = false;
  throw_path = true;
  bool caught{};
  try {
    road_path_query(cache.data(), &output, &point, &end, pawn.data());
  } catch (const std::runtime_error &) {
    caught = true;
  }
  throw_path = false;
  check(caught && road_path_point_collision(cache.data(), &point, 50, 0),
        "native exception restores scope and unscoped probes remain original");
  check(road_path_segment_collision(cache.data(), a.data(), b.data(), 10,
                                    nullptr, 1),
        "unscoped fallback segment never borrows a completed query");
  check(manor == original_manor && region == original_region &&
            road == original_road,
        "manor, region cache and road bytes never modified");
  check(VirtualFree(image, 0, MEM_RELEASE), "release isolated test image");
  std::printf("PASS: %u road wrapper checks\n", checks);
}

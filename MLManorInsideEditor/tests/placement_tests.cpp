#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <Windows.h>

#include "Placement.hpp"

static std::array<std::byte, 0x1200> pawn{}, region{}, manor{};
static std::array<std::byte, 8> game{}, other_actor{};
static std::array<std::byte, 0x2b0> cache{}, other_cache{};
static constexpr std::uint32_t manor_index = 318448;
static unsigned checks{}, triangle_calls{}, segment_calls{}, point_calls{},
    bounds_calls{}, ignored_frees{}, geometry_builds{},
    segment_geometry_checks{};
static GeometryVerdict module_verdict = GeometryVerdict::Clear;
static bool geometry_complete = true, geometry_throws{}, point_module_blocked{};
static bool native_result{}, native_uses_records{}, unrelated_obstacle{};
static bool seen_skip{}, seen_point_skip{}, nested_segment{}, nested_point{},
    inside_nested{}, check_other_thread{};
static NativeArray<Vec3> *expected_triangles{};
static NativeArray<void *> *expected_ignored{};
static const double *expected_bounds{}, *expected_start{}, *expected_end{};
static const Vec3 *expected_point{};
static void *ignored_allocation{};
static std::vector<void *> thunk_allocations;

static void check(bool condition, const char *description) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, description);
    std::exit(1);
  }
}

template <typename T, std::size_t N>
static void put(std::array<std::byte, N> &target, std::size_t offset,
                const T &value) {
  check(offset <= N && sizeof(value) <= N - offset,
        "fixture write stays within its allocation");
  std::memcpy(target.data() + offset, &value, sizeof(value));
}

static bool observe_record_scope(void *source) {
  check(source == cache.data(), "native collision preserves cache argument");
  const bool skip = placement_skip_record(source, manor_index);
  check(!placement_skip_record(source, manor_index + 1) &&
            !placement_skip_record(other_cache.data(), manor_index),
        "only the exact Manor record in the current cache can be skipped");
  return skip;
}

static bool native_triangles(void *source, NativeArray<Vec3> *triangles,
                             const double *bounds, NativeArray<void *> *ignored,
                             std::uint8_t skip_segments) {
  ++triangle_calls;
  seen_skip = observe_record_scope(source);
  check(triangles == expected_triangles && bounds == expected_bounds &&
            ignored == expected_ignored && skip_segments == 0xa5,
        "native triangle query receives every original argument");
  check(ignored != nullptr && ignored->data == ignored_allocation &&
            ignored->count == 1 && ignored->capacity == 1,
        "native triangle query receives its original owned ignored array");
  check(HeapFree(GetProcessHeap(), 0, ignored_allocation) != 0,
        "native triangle query frees its owned ignored array");
  ++ignored_frees;
  ignored_allocation = nullptr;
  return native_uses_records ? !seen_skip || unrelated_obstacle : native_result;
}

static bool native_segment(void *source, const double *start,
                           const double *end) {
  ++segment_calls;
  const bool skip = observe_record_scope(source);
  check(start == expected_start && end == expected_end,
        "native segment query preserves both FVector2D pointers");
  if (nested_segment && !inside_nested) {
    inside_nested = true;
    const bool nested_result =
        placement_segment_collision(source, start, end, nullptr);
    check(nested_result == native_result && !seen_skip,
          "nested ineligible segment masks the outer Manor exclusion");
    inside_nested = false;
    check(placement_skip_record(source, manor_index) == skip,
          "nested segment restores the outer Manor exclusion");
  }
  if (check_other_thread) {
    bool thread_skip = true;
    std::thread worker(
        [&] { thread_skip = placement_skip_record(source, manor_index); });
    worker.join();
    check(!thread_skip, "placement record scope never leaks to another thread");
  }
  seen_skip = skip;
  return native_uses_records ? !skip || unrelated_obstacle : native_result;
}

static bool native_bounds(void *building, const Vec3 *point) {
  ++bounds_calls;
  check(point == expected_point,
        "native initial-point bounds preserves its point pointer");
  check(building == manor.data() || building == other_actor.data(),
        "bounds query receives an expected original actor");
  return building == manor.data() || unrelated_obstacle;
}

static bool native_point(void *source, const Vec3 *point) {
  ++point_calls;
  check(source == game.data() && point == expected_point,
        "native initial-point query preserves game and position arguments");
  const auto before_bounds = bounds_calls;
  const bool manor_blocked =
      placement_initial_point_bounds(manor.data(), point);
  const bool skip = bounds_calls == before_bounds;
  const bool other_blocked =
      placement_initial_point_bounds(other_actor.data(), point);
  check(bounds_calls == before_bounds + (skip ? 1 : 2),
        "initial point can bypass only the exact scoped Manor bounds");
  if (nested_point && !inside_nested) {
    inside_nested = true;
    const bool nested_result = placement_initial_point(source, point, nullptr);
    check(nested_result == native_result && !seen_point_skip,
          "nested ineligible first point masks outer Manor exclusion");
    inside_nested = false;
    check(placement_initial_point_bounds(manor.data(), point) == !skip,
          "nested first point restores the outer Manor exclusion");
  }
  if (check_other_thread) {
    bool thread_blocked{};
    std::thread worker([&] {
      thread_blocked = placement_initial_point_bounds(manor.data(), point);
    });
    worker.join();
    check(thread_blocked,
          "initial-point Manor exclusion never leaks to another thread");
  }
  seen_point_skip = skip;
  return native_uses_records ? manor_blocked || other_blocked : native_result;
}

// Only geometry acquisition/evaluation is stubbed. Every placement wrapper and
// its record/bounds scope below executes the production Placement.cpp.
ManorGeometry build_manor_geometry(void *actor, std::uintptr_t image) {
  ++geometry_builds;
  check(actor == manor.data() && image == reinterpret_cast<std::uintptr_t>(
                                              GetModuleHandleW(nullptr)),
        "geometry uses the exact Manor and executable image");
  if (geometry_throws)
    throw std::runtime_error("synthetic geometry failure");
  ManorGeometry result;
  result.complete = geometry_complete;
  return result;
}

bool point_in_manor_modules(const ManorGeometry &, const Vec3 &) {
  return point_module_blocked;
}

GeometryVerdict
triangles_intersect_manor_modules(const ManorGeometry &geometry,
                                  std::span<const Vec3> triangles) {
  check(triangles.size() == 3,
        "geometry evaluates a copied complete native triangle");
  return geometry.complete ? module_verdict : GeometryVerdict::Unknown;
}

GeometryVerdict segment_intersects_manor_modules(const ManorGeometry &geometry,
                                                 const Vec3 &start,
                                                 const Vec3 &end,
                                                 double half_width) {
  ++segment_geometry_checks;
  check(start.z == 0 && end.z == 0 && half_width == 0,
        "placement evaluates FVector2D inputs without reading a third double");
  return geometry.complete ? module_verdict : GeometryVerdict::Unknown;
}

static void install_native_thunk(std::uintptr_t offset, const void *callback) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const auto target = image + offset;
  const auto allocation_base =
      target & ~(static_cast<std::uintptr_t>(info.dwAllocationGranularity) - 1);
  MEMORY_BASIC_INFORMATION memory{};
  check(VirtualQuery(reinterpret_cast<void *>(allocation_base), &memory,
                     sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_FREE &&
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) <=
                allocation_base &&
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
                    memory.RegionSize >=
                allocation_base + info.dwAllocationGranularity,
        "native thunk target is entirely unmapped before reservation");
  void *allocation =
      VirtualAlloc(reinterpret_cast<void *>(allocation_base),
                   info.dwAllocationGranularity, MEM_RESERVE, PAGE_NOACCESS);
  check(allocation == reinterpret_cast<void *>(allocation_base),
        "reserve only the exact free test thunk allocation");
  thunk_allocations.push_back(allocation);
  const auto page =
      target & ~(static_cast<std::uintptr_t>(info.dwPageSize) - 1);
  check(VirtualAlloc(reinterpret_cast<void *>(page), info.dwPageSize,
                     MEM_COMMIT,
                     PAGE_READWRITE) == reinterpret_cast<void *>(page),
        "commit only the owned test thunk page");
  const std::array<std::uint8_t, 6> instruction{0xff, 0x25, 0, 0, 0, 0};
  std::memcpy(reinterpret_cast<void *>(target), instruction.data(),
              instruction.size());
  std::memcpy(reinterpret_cast<void *>(target + instruction.size()), &callback,
              sizeof(callback));
  DWORD old_protection{};
  check(VirtualProtect(reinterpret_cast<void *>(page), info.dwPageSize,
                       PAGE_EXECUTE_READ, &old_protection) != 0 &&
            FlushInstructionCache(GetCurrentProcess(),
                                  reinterpret_cast<void *>(target), 14) != 0,
        "publish executable test thunk without overwriting existing mappings");
}

int main() {
  install_native_thunk(0x4b9d080,
                       reinterpret_cast<const void *>(&native_triangles));
  install_native_thunk(0x4bc73f0,
                       reinterpret_cast<const void *>(&native_segment));
  install_native_thunk(0x4cd5b20,
                       reinterpret_cast<const void *>(&native_point));
  install_native_thunk(0x4d47920,
                       reinterpret_cast<const void *>(&native_bounds));
  put(pawn, 0x340, game.data());
  put(pawn, 0xf68, region.data());
  put(region, 0x738, cache.data());
  put(region, 0x358, manor.data());
  put(manor, 0x3b8, std::int32_t{98});
  put(manor, 0xc, manor_index);
  const auto original_pawn = pawn, original_region = region,
             original_manor = manor;
  const auto original_cache = cache;
  const auto original_game = game;
  std::array<Vec3, 3> vertices{{{10, 20, 0}, {30, 20, 0}, {30, 40, 0}}};
  NativeArray<Vec3> triangles{vertices.data(), 3, 3};
  const auto original_vertices = vertices;
  const auto original_triangles = triangles;
  const std::array<double, 4> bounds{10, 20, 30, 40};
  const std::array<double, 2> start{10, 20}, end{30, 40};
  const Vec3 point{15, 25, 0};
  expected_triangles = &triangles;
  expected_bounds = bounds.data();
  expected_start = start.data();
  expected_end = end.data();
  expected_point = &point;

  const auto run_triangles = [&](bool result, bool skip,
                                 const char *description) {
    const auto before_calls = triangle_calls, before_frees = ignored_frees;
    ignored_allocation = HeapAlloc(GetProcessHeap(), 0, sizeof(void *));
    check(ignored_allocation != nullptr, "allocate native-owned ignored array");
    auto **ignored_data = static_cast<void **>(ignored_allocation);
    *ignored_data = other_actor.data();
    NativeArray<void *> ignored{ignored_data, 1, 1};
    expected_ignored = &ignored;
    check(placement_collision(cache.data(), &triangles, bounds.data(), &ignored,
                              0xa5, pawn.data()) == result,
          description);
    check(triangle_calls == before_calls + 1 &&
              ignored_frees == before_frees + 1 &&
              ignored_allocation == nullptr && seen_skip == skip,
          "triangle wrapper calls native exactly once with correct exclusion "
          "and owned-array lifetime");
    check(!placement_skip_record(cache.data(), manor_index),
          "triangle scope is cleared after return");
  };
  const auto run_segment = [&](bool result, bool skip,
                               const char *description) {
    const auto before = segment_calls;
    check(placement_segment_collision(cache.data(), expected_start,
                                      expected_end, pawn.data()) == result,
          description);
    check(segment_calls == before + (nested_segment ? 2 : 1) &&
              seen_skip == skip,
          "segment wrapper calls native once per query with correct exclusion");
    check(!placement_skip_record(cache.data(), manor_index),
          "segment scope is cleared after return");
  };
  const auto run_point = [&](bool result, bool skip, const char *description) {
    const auto before = point_calls;
    check(placement_initial_point(game.data(), expected_point, pawn.data()) ==
              result,
          description);
    check(point_calls == before + (nested_point ? 2 : 1) &&
              seen_point_skip == skip,
          "initial-point wrapper calls native once per query with correct "
          "exclusion");
    check(placement_initial_point_bounds(manor.data(), expected_point),
          "initial-point bounds scope is cleared after return");
  };

  for (const auto verdict : {GeometryVerdict::Clear, GeometryVerdict::Blocked,
                             GeometryVerdict::Unknown}) {
    module_verdict = verdict;
    for (bool blocked : {false, true}) {
      native_result = blocked;
      run_triangles(blocked, verdict == GeometryVerdict::Clear,
                    "triangle result matches native for every module verdict");
      run_segment(blocked, verdict == GeometryVerdict::Clear,
                  "first-edge result matches native for every module verdict");
    }
  }
  for (bool module_blocked : {false, true}) {
    point_module_blocked = module_blocked;
    for (bool blocked : {false, true}) {
      native_result = blocked;
      run_point(blocked, !module_blocked,
                "first-point result matches native for both module outcomes");
    }
  }

  module_verdict = GeometryVerdict::Clear;
  point_module_blocked = false;
  native_uses_records = true;
  for (bool blocked : {false, true}) {
    unrelated_obstacle = blocked;
    run_triangles(
        blocked, true,
        "clear courtyard triangles exclude Manor but retain other obstacles");
    run_segment(blocked, true,
                "clear courtyard first edge excludes Manor but retains other "
                "obstacles");
    run_point(blocked, true,
              "clear courtyard first point excludes Manor but retains other "
              "obstacles");
  }
  unrelated_obstacle = false;
  module_verdict = GeometryVerdict::Blocked;
  point_module_blocked = true;
  run_triangles(true, false,
                "wall-overlapping triangles retain the native Manor record");
  run_segment(true, false,
              "wall-overlapping first edge retains the native Manor record");
  run_point(true, false,
            "wall-overlapping first point retains native Manor bounds");
  native_uses_records = false;
  native_result = false;
  module_verdict = GeometryVerdict::Clear;
  point_module_blocked = false;

  // A load-time snap task can leave the obstacle queues pending. Native
  // queries against that incomplete cache may return clear even over existing
  // houses. Preserve native ownership but never allow the UI to use that
  // result.
  for (const auto offset : {0x230, 0x280}) {
    put(cache, offset, std::int32_t{1});
    native_result = false;
    run_triangles(true, false,
                  "pending obstacle registration blocks overlapping plot "
                  "despite native false clearance");
    run_point(true, false,
              "pending obstacle registration blocks the first plot point");
    const auto segment_before = segment_calls;
    check(placement_segment_collision(cache.data(), start.data(), end.data(),
                                      pawn.data()) &&
              segment_calls == segment_before,
          "UI first edge rejects pending collision data without graph work");
    const auto pending_snapshot = cache;
    ManorGeometry worker_snapshot;
    worker_snapshot.complete = true;
    check(!placement_segment_for_manor(cache.data(), start.data(), end.data(),
                                       manor.data(), &worker_snapshot) &&
              seen_skip && cache == pending_snapshot,
          "shared worker helper keeps native results and cannot consume "
          "pending UI obstacle queues");
    cache = original_cache;
    run_triangles(false, true,
                  "plot placement resumes automatically when pending "
                  "obstacles have been registered");
  }
  put(cache, 0x230, std::int32_t{5});
  put(cache, 0x25c, std::int32_t{5});
  put(cache, 0x280, std::int32_t{3});
  put(cache, 0x2ac, std::int32_t{3});
  run_triangles(false, true,
                "empty sparse queues with allocated free slots remain ready");
  run_segment(false, true, "ready first edge retains native acceptance");
  run_point(false, true, "ready first point retains native acceptance");
  cache = original_cache;

  for (const auto region_offset : {0x978, 0xf68}) {
    pawn = original_pawn;
    put(pawn, 0xf68, static_cast<void *>(nullptr));
    put(pawn, region_offset, region.data());
    run_triangles(false, true,
                  "both building and common polygon region paths receive "
                  "courtyard support");
    run_segment(false, true,
                "common first-edge wrapper does not depend on a plot-type "
                "discriminator");
  }
  pawn = original_pawn;
  geometry_complete = false;
  run_triangles(false, false,
                "incomplete geometry retains native triangle clearance");
  run_segment(false, false,
              "incomplete geometry retains native segment clearance");
  run_point(false, false, "incomplete geometry retains native point clearance");
  geometry_complete = true;
  geometry_throws = true;
  run_triangles(false, false,
                "geometry exception retains native triangle clearance");
  run_segment(false, false,
              "geometry exception retains native segment clearance");
  run_point(false, false, "geometry exception retains native point clearance");
  geometry_throws = false;

  for (const auto invalid :
       {NativeArray<Vec3>{vertices.data(), 2, 3},
        NativeArray<Vec3>{nullptr, 3, 3},
        NativeArray<Vec3>{reinterpret_cast<Vec3 *>(1), 3, 3},
        NativeArray<Vec3>{vertices.data(), 3, 65537}}) {
    triangles = invalid;
    run_triangles(false, false,
                  "malformed triangles preserve the original native query");
  }
  triangles = original_triangles;
  vertices[0].x = std::numeric_limits<double>::quiet_NaN();
  run_triangles(false, false,
                "nonfinite triangles retain native collision semantics");
  vertices = original_vertices;
  expected_start = reinterpret_cast<const double *>(1);
  run_segment(false, false,
              "unreadable edge position retains native collision semantics");
  expected_start = start.data();
  expected_point = reinterpret_cast<const Vec3 *>(1);
  run_point(false, false,
            "unreadable first point retains native collision semantics");
  expected_point = &point;

  put(pawn, 0x840, manor.data());
  run_triangles(false, false,
                "castle planning retains native triangle collision");
  run_segment(false, false,
              "castle planning retains native first-edge collision");
  run_point(false, false,
            "castle planning retains native first-point collision");
  pawn = original_pawn;
  put(region, 0x738, other_cache.data());
  run_triangles(false, false,
                "mismatched region cache cannot exclude Manor records");
  run_segment(false, false,
              "mismatched region cache cannot exclude Manor on first edge");
  region = original_region;
  put(pawn, 0x340, static_cast<void *>(nullptr));
  run_point(false, false,
            "mismatched game cannot exclude Manor on first point");
  pawn = original_pawn;

  nested_segment = true;
  run_segment(false, true,
              "nested segment queries preserve the outer native result");
  nested_segment = false;
  nested_point = true;
  run_point(false, true,
            "nested first-point queries preserve the outer native result");
  nested_point = false;
  check_other_thread = true;
  run_segment(false, true, "segment exclusion remains thread-local");
  run_point(false, true, "first-point exclusion remains thread-local");
  check_other_thread = false;

  ManorGeometry snapshot;
  snapshot.complete = true;
  const auto previous_builds = geometry_builds;
  check(!placement_segment_for_manor(cache.data(), start.data(), end.data(),
                                     manor.data(), &snapshot) &&
            seen_skip && geometry_builds == previous_builds,
        "shared graph segment entry uses the supplied snapshot and native "
        "result");
  PlacementSegmentMemo memo;
  const auto before_evaluations = segment_geometry_checks;
  const auto before_native = segment_calls;
  check(!placement_segment_for_manor(cache.data(), start.data(), end.data(),
                                     manor.data(), &snapshot, &memo) &&
            seen_skip && memo.size() == 1 &&
            memo.begin()->second == GeometryVerdict::Clear,
        "first frozen-snapshot edge stores only its geometry verdict");
  native_result = true;
  check(placement_segment_for_manor(cache.data(), start.data(), end.data(),
                                    manor.data(), &snapshot, &memo) &&
            seen_skip && segment_geometry_checks == before_evaluations + 1 &&
            segment_calls == before_native + 2,
        "reused geometry still executes native query and returns its updated "
        "result");
  native_result = false;
  module_verdict = GeometryVerdict::Blocked;
  memo.clear();
  check(!placement_segment_for_manor(cache.data(), start.data(), end.data(),
                                     manor.data(), &snapshot, &memo) &&
            !seen_skip && memo.size() == 1 &&
            memo.begin()->second == GeometryVerdict::Blocked,
        "cached physical overlap retains native Manor boundary acceptance");
  memo.begin()->second = GeometryVerdict::Clear;
  const auto fresh_evaluations = segment_geometry_checks;
  check(!placement_segment_for_manor(cache.data(), start.data(), end.data(),
                                     manor.data(), nullptr, &memo) &&
            !seen_skip && segment_geometry_checks == fresh_evaluations + 1 &&
            memo.size() == 1 && memo.begin()->second == GeometryVerdict::Clear,
        "fresh live geometry never trusts a caller memo from another snapshot");
  memo.clear();
  std::array<double, 2> changing_start = start, changing_end = end;
  expected_start = changing_start.data();
  expected_end = changing_end.data();
  module_verdict = GeometryVerdict::Clear;
  check(!placement_segment_for_manor(cache.data(), expected_start, expected_end,
                                     manor.data(), &snapshot, &memo) &&
            seen_skip && memo.size() == 1,
        "production memo keys copied endpoint values");
  module_verdict = GeometryVerdict::Blocked;
  changing_start[0] = std::nextafter(changing_start[0], INFINITY);
  check(!placement_segment_for_manor(cache.data(), expected_start, expected_end,
                                     manor.data(), &snapshot, &memo) &&
            !seen_skip && memo.size() == 2,
        "same pointers with changed coordinates cannot reuse a clear verdict");
  changing_start = start;
  const auto before_key_hit = segment_geometry_checks;
  check(!placement_segment_for_manor(cache.data(), expected_start, expected_end,
                                     manor.data(), &snapshot, &memo) &&
            seen_skip && segment_geometry_checks == before_key_hit,
        "restored exact copied coordinates reuse their original verdict");
  changing_start[0] = 0.0;
  check(!placement_segment_for_manor(cache.data(), expected_start, expected_end,
                                     manor.data(), &snapshot, &memo) &&
            !seen_skip && memo.size() == 3,
        "positive zero endpoint is a distinct exact key");
  changing_start[0] = -0.0;
  check(!placement_segment_for_manor(cache.data(), expected_start, expected_end,
                                     manor.data(), &snapshot, &memo) &&
            !seen_skip && memo.size() == 4,
        "negative zero endpoint preserves native floating sign distinctions");
  const auto before_invalid = segment_geometry_checks;
  for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
    changing_start[0] = invalid;
    const auto native_before = segment_calls;
    check(!placement_segment_for_manor(cache.data(), expected_start,
                                       expected_end, manor.data(), &snapshot,
                                       &memo) &&
              !seen_skip && memo.size() == 4 &&
              segment_geometry_checks == before_invalid &&
              segment_calls == native_before + 1,
          "nonfinite endpoints never enter memo and native still runs once");
  }
  memo.clear();
  module_verdict = GeometryVerdict::Clear;
  const auto before_capacity = segment_geometry_checks;
  const auto native_before_capacity = segment_calls;
  for (int i = 0; i < 4098; ++i) {
    changing_start = {static_cast<double>(i), 20};
    check(!placement_segment_for_manor(cache.data(), expected_start,
                                       expected_end, manor.data(), &snapshot,
                                       &memo) &&
              seen_skip,
          "memo capacity never changes a fresh geometry/native verdict");
  }
  check(memo.size() == 4096 &&
            segment_geometry_checks == before_capacity + 4098 &&
            segment_calls == native_before_capacity + 4098,
        "production memo has a strict bound without dropping native calls");
  changing_start = {0, 20};
  module_verdict = GeometryVerdict::Unknown;
  check(!placement_segment_for_manor(cache.data(), expected_start, expected_end,
                                     manor.data(), &snapshot, &memo) &&
            seen_skip && segment_geometry_checks == before_capacity + 4098,
        "full production memo still finds existing exact keys");
  expected_start = start.data();
  expected_end = end.data();
  module_verdict = GeometryVerdict::Clear;
  check(pawn == original_pawn && region == original_region &&
            manor == original_manor && cache == original_cache &&
            game == original_game &&
            std::memcmp(vertices.data(), original_vertices.data(),
                        sizeof(vertices)) == 0 &&
            std::memcmp(&triangles, &original_triangles, sizeof(triangles)) ==
                0,
        "all game buffers and native triangle storage remain byte-identical");
  for (void *allocation : thunk_allocations)
    check(VirtualFree(allocation, 0, MEM_RELEASE) != 0,
          "release only the test-owned thunk allocation");
  std::printf("PASS: %u actual placement wrapper checks\n", checks);
  return 0;
}

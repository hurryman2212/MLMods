#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <Windows.h>

#include "Placement.hpp"
#include "Snap.hpp"

enum class Lane { Native, Manor };
struct ObservedCall {
  Lane lane;
  void *cache;
  const double *start;
  const double *end;
  void *context;
  const ManorGeometry *snapshot{};
  std::uintptr_t verdict_slot{};
  bool verdict_hit{};
};
static std::vector<ObservedCall> calls;
static bool native_result = true;
static bool placement_result = false;
static void *secondary_cache{}, *secondary_pawn{};
static void *primary_cache{}, *primary_pawn{}, *primary_manor{},
    *secondary_manor{};
static bool geometry_complete = true, geometry_throws{};
static int geometry_builds{};
static bool cross_thread_query{};
static bool graph_throws{}, graph_wrong_cache{}, graph_thread_check{};
static bool graph_nested_query{}, graph_path_query{}, path_graph_query{};
static bool graph_nested_eligible{};
static int graph_calls{}, weak_calls{}, cache_weak_calls{};
static void *last_work{};
static std::uint64_t last_cache_weak{};
static constexpr std::uint64_t cache_weak = 0x123456789abcdef0ULL;
static constexpr std::uint64_t manor_weak = 0x223456789abcdef0ULL;
static constexpr std::uint64_t other_manor_weak = 0x323456789abcdef0ULL;
static constexpr std::uint64_t road_weak = 0x423456789abcdef0ULL;
static void *road_actor{};
static const void *graph_node_a{}, *graph_node_b{};
static std::uint64_t *mutating_creator{};
static std::byte *mutating_descriptor{};
static bool creator_throws{};
static std::vector<bool> graph_results;
static std::vector<std::array<Vec3, 4>> fixture_module_quads;
static constexpr std::uintptr_t graph_return = 0x8877665544332211ULL;
static int native_path_calls{}, nested_exceptions{}, path_depth{};
static std::array<double, 2> start_xy{10, 20}, end_xy{30, 40};
static std::array<Vec3, 2> native_points{{{10, 20, 0}, {30, 40, 0}}};
static NativeArray<Vec3> alternate_return{};
static bool use_alternate_return{};
static void *last_plot_points{};
static float last_threshold{};
static std::uint8_t last_mode{};
static std::vector<bool> path_results;
static GeometryVerdict computed_verdict = GeometryVerdict::Clear;
enum class PathPlan {
  Simple,
  MemoInputs,
  MemoCapacity,
  ChangingNativeResult,
  OtherCache,
  NestedIneligible,
  NestedEligible,
  ThrowDirect,
  ThrowNested
};
static PathPlan path_plan = PathPlan::Simple;

static bool original_segment(void *cache, const double *start,
                             const double *end) {
  calls.push_back({Lane::Native, cache, start, end, nullptr});
  return native_result;
}

void *resolve_placement_manor(void *cache, void *pawn) noexcept {
  if (cache == primary_cache && pawn == primary_pawn)
    return primary_manor;
  if (cache == secondary_cache && pawn == secondary_pawn)
    return secondary_manor;
  return nullptr;
}

ManorGeometry build_manor_geometry(void *manor, std::uintptr_t) {
  ++geometry_builds;
  if (geometry_throws)
    throw std::runtime_error("synthetic geometry failure");
  ManorGeometry result;
  result.complete = geometry_complete;
  result.actors.push_back(manor);
  result.module_quads = fixture_module_quads;
  return result;
}

bool placement_segment_for_manor(void *cache, const double *start,
                                 const double *end, void *manor,
                                 const ManorGeometry *snapshot,
                                 PlacementSegmentMemo *segment_memo) noexcept {
  GeometryVerdict *cached_verdict{};
  bool hit{};
  std::array<double, 2> first{}, second{};
  SIZE_T copied{};
  // Emulate Placement.cpp's memo after its single validated coordinate read.
  // These tests verify ownership/lifetime; placement_tests exercise the real
  // implementation and its native-call behavior.
  if (snapshot != nullptr && segment_memo != nullptr &&
      ReadProcessMemory(GetCurrentProcess(), start, first.data(), sizeof(first),
                        &copied) &&
      copied == sizeof(first) &&
      ReadProcessMemory(GetCurrentProcess(), end, second.data(), sizeof(second),
                        &copied) &&
      copied == sizeof(second) && std::isfinite(first[0]) &&
      std::isfinite(first[1]) && std::isfinite(second[0]) &&
      std::isfinite(second[1])) {
    const std::array key{std::bit_cast<std::uint64_t>(first[0]),
                         std::bit_cast<std::uint64_t>(first[1]),
                         std::bit_cast<std::uint64_t>(second[0]),
                         std::bit_cast<std::uint64_t>(second[1])};
    try {
      const auto found = segment_memo->find(key);
      if (found != segment_memo->end()) {
        cached_verdict = &found->second;
        hit = true;
      } else if (segment_memo->size() < 4096) {
        cached_verdict =
            &segment_memo->try_emplace(key, computed_verdict).first->second;
      }
    } catch (...) {
      // A failed optional memo does not suppress the native-call fixture.
    }
  }
  calls.push_back({Lane::Manor, cache, start, end, manor, snapshot,
                   reinterpret_cast<std::uintptr_t>(cached_verdict), hit});
  return placement_result;
}

struct PathDepthScope {
  PathDepthScope() { ++path_depth; }
  ~PathDepthScope() { --path_depth; }
};

static NativeArray<Vec3> *original_path(void *cache, NativeArray<Vec3> *output,
                                        void *plot_points, float threshold,
                                        std::uint8_t mode) {
  PathDepthScope depth;
  ++native_path_calls;
  last_plot_points = plot_points;
  last_threshold = threshold;
  last_mode = mode;
  if (path_plan == PathPlan::ThrowDirect ||
      (path_plan == PathPlan::ThrowNested && path_depth == 2)) {
    snap_segment_collision(cache, start_xy.data(), end_xy.data());
    throw std::runtime_error("synthetic native path exception");
  }
  if (path_plan == PathPlan::MemoInputs) {
    std::array<double, 2> first = start_xy, second = end_xy;
    snap_segment_collision(cache, start_xy.data(), end_xy.data());
    snap_segment_collision(cache, first.data(), second.data());
    first[0] =
        std::nextafter(first[0], std::numeric_limits<double>::infinity());
    snap_segment_collision(cache, first.data(), second.data());
    first[1] =
        std::nextafter(first[1], std::numeric_limits<double>::infinity());
    snap_segment_collision(cache, first.data(), second.data());
    second[1] =
        std::nextafter(second[1], std::numeric_limits<double>::infinity());
    snap_segment_collision(cache, first.data(), second.data());
    snap_segment_collision(cache, end_xy.data(), start_xy.data());
    first = {0.0, 20.0};
    snap_segment_collision(cache, first.data(), end_xy.data());
    first[0] = -0.0;
    snap_segment_collision(cache, first.data(), end_xy.data());
    first[0] = std::numeric_limits<double>::quiet_NaN();
    snap_segment_collision(cache, first.data(), end_xy.data());
    first[0] = std::numeric_limits<double>::infinity();
    snap_segment_collision(cache, first.data(), end_xy.data());
    snap_segment_collision(cache, nullptr, end_xy.data());
    snap_segment_collision(cache, start_xy.data(),
                           reinterpret_cast<double *>(1));
  } else if (path_plan == PathPlan::MemoCapacity) {
    for (int i = 0; i < 4098; ++i) {
      const std::array<double, 2> first{static_cast<double>(i), 20.0};
      snap_segment_collision(cache, first.data(), end_xy.data());
    }
    const std::array<double, 2> first{0.0, 20.0};
    snap_segment_collision(cache, first.data(), end_xy.data());
  } else if (path_depth == 1 && (path_plan == PathPlan::NestedIneligible ||
                                 path_plan == PathPlan::NestedEligible ||
                                 path_plan == PathPlan::ThrowNested)) {
    snap_segment_collision(cache, start_xy.data(), end_xy.data());
    NativeArray<Vec3> nested_output;
    try {
      snap_project_path(secondary_cache, &nested_output, plot_points, threshold,
                        path_plan == PathPlan::NestedEligible ? 3 : 2,
                        secondary_pawn);
    } catch (const std::runtime_error &) {
      ++nested_exceptions;
    }
    snap_segment_collision(cache, start_xy.data(), end_xy.data());
  } else {
    const bool previous_result = placement_result;
    for (int call = 0; call < 5; ++call) {
      if (path_plan == PathPlan::ChangingNativeResult)
        placement_result = call % 2 != 0;
      path_results.push_back(snap_segment_collision(
          path_plan == PathPlan::OtherCache ? secondary_cache : cache,
          start_xy.data(), end_xy.data()));
    }
    placement_result = previous_result;
  }
  if (cross_thread_query) {
    std::thread worker([cache] {
      snap_segment_collision(cache, start_xy.data(), end_xy.data());
    });
    worker.join();
  }
  if (path_graph_query)
    snap_graph_segment_collision(cache, start_xy.data(), end_xy.data(),
                                 graph_node_a, graph_node_b);
  if (output)
    *output = {native_points.data(), 2, 2};
  return use_alternate_return ? &alternate_return : output;
}

static void *original_weak_cache(const std::uint64_t *weak) {
  ++weak_calls;
  const auto value = *weak;
  if (value == cache_weak) {
    ++cache_weak_calls;
    last_cache_weak = value;
    return primary_cache;
  }
  if (creator_throws)
    throw std::runtime_error("synthetic weak creator failure");
  if (mutating_creator != nullptr) {
    *mutating_creator = 0;
    mutating_creator = nullptr;
  }
  if (mutating_descriptor != nullptr) {
    const std::int32_t invalid_count = -1;
    std::memcpy(mutating_descriptor + 8, &invalid_count, sizeof(invalid_count));
    mutating_descriptor = nullptr;
  }
  if (value == manor_weak)
    return primary_manor;
  if (value == other_manor_weak)
    return secondary_manor;
  if (value == road_weak)
    return road_actor;
  return nullptr;
}

static std::uintptr_t original_graph(void *work) {
  ++graph_calls;
  last_work = work;
  const bool nested_query = graph_nested_query;
  graph_nested_query = false;
  for (int i = 0; i < 4; ++i) {
    if (nested_query && i == 2)
      snap_build_graph(graph_nested_eligible ? work : nullptr);
    graph_results.push_back(snap_graph_segment_collision(
        graph_wrong_cache ? secondary_cache : primary_cache, start_xy.data(),
        end_xy.data(), graph_node_a, graph_node_b));
  }
  graph_nested_query = nested_query;
  if (graph_path_query)
    snap_segment_collision(primary_cache, start_xy.data(), end_xy.data());
  if (graph_thread_check) {
    std::thread worker([] {
      snap_graph_segment_collision(primary_cache, start_xy.data(),
                                   end_xy.data(), graph_node_a, graph_node_b);
    });
    worker.join();
  }
  if (graph_throws)
    throw std::runtime_error("synthetic graph-worker exception");
  return graph_return;
}

template <typename T, std::size_t N>
static void put(std::array<std::byte, N> &object, std::size_t offset,
                const T &value) {
  if (offset + sizeof(value) > N)
    throw std::runtime_error("fixture write exceeds its own allocation");
  std::memcpy(object.data() + offset, &value, sizeof(value));
}

int main() {
  int checks{};
  const auto require = [&checks](bool value, const char *message) {
    ++checks;
    if (!value)
      throw std::runtime_error(message);
  };
  try {
    std::array<std::byte, 0x1000> pawn{}, other_pawn{}, manor{}, other_manor{},
        road{};
    std::array<std::byte, 0x800> cache{}, other_cache{}, region{};
    put(pawn, 0x34c, std::uint8_t{1});
    put(cache, 0x20, region.data());
    put(region, 0x738, cache.data());
    put(region, 0x350, pawn.data());
    put(region, 0x358, manor.data());
    put(manor, 0x3b8, std::int32_t{98});
    // Sentinels representing cached data must not be rewritten by callbacks.
    cache[0x61a] = std::byte{1};
    cache[0x63a] = std::byte{1};
    put(cache, 0x708, std::int32_t{37});
    const auto original_pawn = pawn, original_other_pawn = other_pawn;
    const auto original_cache = cache, original_other_cache = other_cache;
    const auto original_region = region;
    const auto original_manor = manor;
    secondary_cache = other_cache.data();
    secondary_pawn = other_pawn.data();
    secondary_manor = other_manor.data();
    primary_cache = cache.data();
    primary_pawn = pawn.data();
    primary_manor = manor.data();
    road_actor = road.data();
    std::array<std::byte, 0x68> node_a{}, node_b{};
    std::array<std::uint64_t, 1> manor_creators{manor_weak};
    std::array<std::uint64_t, 1> road_creators{road_weak};
    const NativeArray<std::uint64_t> manor_refs{manor_creators.data(), 1, 1};
    const NativeArray<std::uint64_t> road_refs{road_creators.data(), 1, 1};
    put(node_a, 0x38, road_refs);
    put(node_b, 0x38, road_refs);
    const auto original_node_a = node_a, original_node_b = node_b;
    graph_node_a = node_a.data();
    graph_node_b = node_b.data();
    configure_snap_query_functions(original_path, original_segment);
    configure_snap_graph_functions(original_graph, original_weak_cache);
    NativeArray<Vec3> output;
    std::array<std::byte, 0x40> plot_points{};
    const auto original_plot_points = plot_points;

    calls.clear();
    require(
        snap_segment_collision(cache.data(), start_xy.data(), end_xy.data()),
        "unscoped segment preserves original result");
    require(calls.size() == 1 && calls[0].lane == Lane::Native,
            "unscoped segment invokes native once");
    calls.clear();
    native_path_calls = 0;
    geometry_builds = 0;
    auto *result = snap_project_path(cache.data(), &output, plot_points.data(),
                                     25.5f, 3, pawn.data());
    require(result == &output && output.data == native_points.data() &&
                output.count == 2 && output.capacity == 2 &&
                native_path_calls == 1 && geometry_builds == 1,
            "native path return and output storage are preserved");
    require(last_plot_points == plot_points.data() && last_threshold == 25.5f &&
                last_mode == 3 && plot_points == original_plot_points,
            "native path receives every original argument unchanged");
    require(calls.size() == 5, "all five inner segment queries execute");
    for (const auto &call : calls)
      require(call.lane == Lane::Manor && call.cache == cache.data() &&
                  call.context == manor.data() && call.snapshot != nullptr &&
                  call.snapshot == calls[0].snapshot &&
                  call.start == start_xy.data() && call.end == end_xy.data(),
              "eligible query routes each inner segment to exact acting pawn");
    require(calls[0].verdict_slot != 0 && !calls[0].verdict_hit,
            "first segment starts with an empty geometry memo entry");
    for (std::size_t i = 1; i < calls.size(); ++i)
      require(calls[i].verdict_slot == calls[0].verdict_slot &&
                  calls[i].verdict_hit,
              "repeated exact geometry queries reuse the same memo entry");
    calls.clear();
    use_alternate_return = true;
    require(snap_project_path(cache.data(), &output, plot_points.data(), 25.5f,
                              3, pawn.data()) == &alternate_return,
            "non-output original return pointer is not replaced by wrapper");
    require(!calls[0].verdict_hit && calls[1].verdict_hit,
            "memo entries never survive into the next projection call");
    use_alternate_return = false;

    path_plan = PathPlan::MemoInputs;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 12 && calls[1].verdict_hit &&
                calls[0].verdict_slot == calls[1].verdict_slot,
            "coordinate values rather than pointer identity key the memo");
    for (std::size_t i = 2; i < 8; ++i) {
      require(calls[i].verdict_slot != 0 && !calls[i].verdict_hit,
              "changed coordinates, endpoint order and signed zero use fresh "
              "entries");
      for (std::size_t j = 0; j < i; ++j)
        require(calls[i].verdict_slot != calls[j].verdict_slot,
                "distinct finite exact keys never alias memo slots");
    }
    for (std::size_t i = 8; i < calls.size(); ++i)
      require(
          calls[i].lane == Lane::Manor && calls[i].verdict_slot == 0,
          "nonfinite or unreadable endpoints still execute uncached helper");

    path_plan = PathPlan::MemoCapacity;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 4099, "cache capacity never skips segment calls");
    for (std::size_t i = 0; i < 4096; ++i)
      require(calls[i].verdict_slot != 0 && !calls[i].verdict_hit,
              "unique entries are memoized up to the memory bound");
    require(
        calls[4096].verdict_slot == 0 && calls[4097].verdict_slot == 0 &&
            calls.back().verdict_hit &&
            calls.back().verdict_slot == calls.front().verdict_slot,
        "full cache falls back for new keys but still reuses existing entries");

    path_plan = PathPlan::ChangingNativeResult;
    for (const auto verdict : {GeometryVerdict::Clear, GeometryVerdict::Blocked,
                               GeometryVerdict::Unknown}) {
      computed_verdict = verdict;
      calls.clear();
      path_results.clear();
      snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                        pawn.data());
      require(calls.size() == 5 && path_results.size() == 5 &&
                  !calls[0].verdict_hit,
              "each geometry verdict begins in a new projection scope");
      for (std::size_t i = 0; i < path_results.size(); ++i)
        require(path_results[i] == (i % 2 != 0) &&
                    (i == 0 || calls[i].verdict_hit),
                "memoized geometry never substitutes a cached native boolean");
    }
    computed_verdict = GeometryVerdict::Clear;
    path_plan = PathPlan::Simple;

    for (const auto mode : {0, 1, 2, 4, 255}) {
      calls.clear();
      snap_project_path(cache.data(), &output, plot_points.data(), 25.5f,
                        static_cast<std::uint8_t>(mode), pawn.data());
      require(calls.size() == 5, "excluded mode keeps all native queries");
      for (const auto &call : calls)
        require(call.lane == Lane::Native,
                "excluded mode never relaxes collision");
    }
    put(pawn, 0x840, manor.data());
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Native &&
                calls[4].lane == Lane::Native,
            "castle planner is outside ordinary plot scope");
    pawn = original_pawn;
    for (void *bad_pawn :
         {static_cast<void *>(nullptr), reinterpret_cast<void *>(1)}) {
      calls.clear();
      snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                        bad_pawn);
      require(calls.size() == 5 && calls[0].lane == Lane::Native,
              "missing or unreadable pawn delegates to original");
    }
    for (const float bad_threshold :
         {std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()}) {
      calls.clear();
      snap_project_path(cache.data(), &output, plot_points.data(),
                        bad_threshold, 3, pawn.data());
      require(calls.size() == 5 && calls[0].lane == Lane::Native,
              "nonfinite threshold cannot enable special scope");
    }
    calls.clear();
    snap_project_path(nullptr, &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Native,
            "null cache delegates without enabling scope");
    path_plan = PathPlan::OtherCache;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Native &&
                calls[0].cache == other_cache.data(),
            "another cache is never admitted by a scoped query");
    path_plan = PathPlan::NestedIneligible;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 7 && calls.front().lane == Lane::Manor &&
                calls.back().lane == Lane::Manor,
            "outer eligible scope is restored after nested excluded query");
    for (std::size_t i = 1; i < 6; ++i)
      require(calls[i].lane == Lane::Native,
              "nested excluded query masks the outer eligible scope");
    path_plan = PathPlan::NestedEligible;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 7 && calls.front().context == manor.data() &&
                calls.back().context == manor.data(),
            "nested eligible scope restores exact outer pawn");
    for (std::size_t i = 1; i < 6; ++i)
      require(calls[i].lane == Lane::Manor &&
                  calls[i].context == other_manor.data() &&
                  calls[i].cache == other_cache.data(),
              "nested eligible query uses only its own cache and pawn");
    require(!calls.front().verdict_hit && !calls[1].verdict_hit &&
                calls.front().verdict_slot != calls[1].verdict_slot &&
                calls.back().verdict_hit &&
                calls.back().verdict_slot == calls.front().verdict_slot,
            "nested eligible path owns a separate memo and restores the outer "
            "memo");
    path_plan = PathPlan::ThrowNested;
    calls.clear();
    nested_exceptions = 0;
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(nested_exceptions == 1 && calls.size() == 3 &&
                calls.front().lane == Lane::Manor &&
                calls[1].lane == Lane::Native &&
                calls.back().lane == Lane::Manor,
            "nested exception restores outer context before continuing");
    path_plan = PathPlan::ThrowDirect;
    bool caught{};
    try {
      snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                        pawn.data());
    } catch (const std::runtime_error &) {
      caught = true;
    }
    calls.clear();
    snap_segment_collision(cache.data(), start_xy.data(), end_xy.data());
    require(caught && calls.size() == 1 && calls[0].lane == Lane::Native &&
                path_depth == 0,
            "escaping native exception restores TLS and propagates normally");
    path_plan = PathPlan::Simple;

    cross_thread_query = true;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 6 && calls[0].lane == Lane::Manor &&
                calls[5].lane == Lane::Native,
            "preview query scope never leaks into another thread");
    cross_thread_query = false;
    geometry_complete = false;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Native,
            "incomplete preview snapshot preserves native collision");
    geometry_complete = true;
    geometry_throws = true;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Native,
            "preview snapshot failure still calls the original path once");
    geometry_throws = false;

    std::array<std::byte, 0x40> work{};
    put(work, 0x18, cache_weak);
    const auto original_work = work;
    calls.clear();
    graph_calls = geometry_builds = weak_calls = cache_weak_calls = 0;
    require(snap_build_graph(work.data()) == graph_return && graph_calls == 1 &&
                last_work == work.data() && cache_weak_calls == 1 &&
                last_cache_weak == cache_weak && weak_calls > 1 &&
                geometry_builds == 1,
            "graph worker snapshots once and preserves all 64 return bits and "
            "weak handle");
    require(calls.size() == 4, "all four graph link tests run");
    for (const auto &call : calls)
      require(call.lane == Lane::Manor && call.context == manor.data() &&
                  call.cache == cache.data() && call.start == start_xy.data() &&
                  call.end == end_xy.data() && call.snapshot != nullptr &&
                  call.snapshot == calls[0].snapshot,
              "graph link checks share the one immutable batch snapshot");
    require(calls[0].verdict_slot != 0 && !calls[0].verdict_hit,
            "graph batch starts with a fresh geometry memo");
    for (std::size_t i = 1; i < calls.size(); ++i)
      require(calls[i].verdict_hit &&
                  calls[i].verdict_slot == calls[0].verdict_slot,
              "repeated graph candidate coordinates reuse pure geometry only");
    calls.clear();
    require(
        snap_graph_segment_collision(cache.data(), start_xy.data(),
                                     end_xy.data(), graph_node_a,
                                     graph_node_b) &&
            calls.size() == 1 && calls[0].lane == Lane::Native,
        "graph link callback outside its worker batch preserves native query");

    const auto graph_uses = [&](Lane expected_lane, bool expected_result) {
      calls.clear();
      graph_results.clear();
      if (snap_build_graph(work.data()) != graph_return || calls.size() != 4 ||
          graph_results.size() != 4)
        return false;
      for (std::size_t i = 0; i < calls.size(); ++i)
        if (calls[i].lane != expected_lane ||
            graph_results[i] != expected_result ||
            calls[i].cache != cache.data() ||
            calls[i].start != start_xy.data() || calls[i].end != end_xy.data())
          return false;
      return true;
    };
    put(node_a, 0x38, manor_refs);
    put(node_b, 0x38, manor_refs);
    require(graph_uses(Lane::Native, true),
            "Manor boundary candidates retain native obstacle rejection");
    native_result = false;
    placement_result = true;
    require(graph_uses(Lane::Native, false),
            "Manor boundary candidates retain native clearance even when the "
            "module collision mock would block them");
    put(node_b, 0x38, road_refs);
    require(graph_uses(Lane::Manor, true),
            "Manor-to-road candidates retain physical module collision");
    put(node_a, 0x38, road_refs);
    put(node_b, 0x38, manor_refs);
    require(graph_uses(Lane::Manor, true),
            "road-to-Manor candidates retain physical module collision");
    put(node_b, 0x38, road_refs);
    require(graph_uses(Lane::Manor, true),
            "road-to-road candidates retain physical module collision");
    placement_result = false;
    native_result = true;
    require(graph_uses(Lane::Manor, false),
            "ordinary candidates preserve a clear physical module result");

    std::array<std::uint64_t, 1> other_manor_creators{other_manor_weak};
    const NativeArray<std::uint64_t> other_manor_refs{
        other_manor_creators.data(), 1, 1};
    put(node_a, 0x38, other_manor_refs);
    put(node_b, 0x38, other_manor_refs);
    require(graph_uses(Lane::Manor, false),
            "two nodes from another Manor do not qualify as scoped boundary");
    put(node_a, 0x38, manor_refs);
    require(graph_uses(Lane::Manor, false),
            "different Manor creators do not form a scoped boundary pair");

    std::array<std::uint64_t, 3> mixed_creators{road_weak, manor_weak,
                                                road_weak};
    std::array<std::uint64_t, 2> duplicate_creators{manor_weak, manor_weak};
    const NativeArray<std::uint64_t> mixed_refs{mixed_creators.data(), 3, 3};
    const NativeArray<std::uint64_t> duplicate_refs{duplicate_creators.data(),
                                                    2, 2};
    put(node_a, 0x38, mixed_refs);
    put(node_b, 0x38, duplicate_refs);
    require(graph_uses(Lane::Native, true),
            "mixed and duplicate creator lists still identify both Manor "
            "boundary nodes");
    put(node_a, 0x38, duplicate_refs);
    put(node_b, 0x38, mixed_refs);
    require(graph_uses(Lane::Native, true),
            "Manor membership is independent of creator ordering");
    put(node_b, 0x38, road_refs);
    require(graph_uses(Lane::Manor, false),
            "a changed second node is reclassified on every callback");

    put(node_a, 0x38, road_refs);
    put(node_b, 0x38, road_refs);
    put(node_a, 0x48, manor_refs);
    put(node_b, 0x48, manor_refs);
    require(graph_uses(Lane::Manor, false),
            "Manor references in the separate road array cannot qualify a "
            "boundary candidate");
    node_a = original_node_a;
    node_b = original_node_b;
    put(node_a, 0x38, NativeArray<std::uint64_t>{});
    put(node_b, 0x38, NativeArray<std::uint64_t>{});
    require(graph_uses(Lane::Manor, false),
            "valid empty creator arrays remain ordinary candidates");
    node_a = original_node_a;
    node_b = original_node_b;

    const std::array<NativeArray<std::uint64_t>, 8> invalid_arrays{{
        {nullptr, 1, 1},
        {road_creators.data(), -1, 1},
        {road_creators.data(), 2, 1},
        {road_creators.data(), 1, -1},
        {road_creators.data(), 1, 65},
        {road_creators.data(), 65, 65},
        {reinterpret_cast<std::uint64_t *>(1), 1, 1},
        {reinterpret_cast<std::uint64_t *>(
             std::numeric_limits<std::uintptr_t>::max() - 3),
         1, 1},
    }};
    for (const auto &invalid_refs : invalid_arrays) {
      put(node_a, 0x38, invalid_refs);
      require(graph_uses(Lane::Native, true),
              "malformed first-node creator array preserves native query");
      node_a = original_node_a;
      put(node_b, 0x38, invalid_refs);
      require(graph_uses(Lane::Native, true),
              "malformed second-node creator array preserves native query");
      node_b = original_node_b;
    }
    for (const auto *bad_node :
         {static_cast<const void *>(nullptr), reinterpret_cast<const void *>(1),
          reinterpret_cast<const void *>(
              std::numeric_limits<std::uintptr_t>::max() - 3)}) {
      graph_node_a = bad_node;
      require(graph_uses(Lane::Native, true),
              "missing or unreadable first node preserves native query");
      graph_node_a = node_a.data();
      graph_node_b = bad_node;
      require(graph_uses(Lane::Native, true),
              "missing or unreadable second node preserves native query");
      graph_node_b = node_b.data();
    }

    // Same object index, different serial: only the exact weak handle resolves.
    const auto stale_manor_weak = manor_weak + (std::uint64_t{1} << 32);
    std::array<std::uint64_t, 2> stale_creators{manor_weak, stale_manor_weak};
    const NativeArray<std::uint64_t> stale_refs{stale_creators.data(), 2, 2};
    for (bool stale_first : {false, true}) {
      stale_creators = stale_first ? std::array{stale_manor_weak, manor_weak}
                                   : std::array{manor_weak, stale_manor_weak};
      put(node_a, 0x38, stale_refs);
      require(graph_uses(Lane::Native, true),
              "stale creator anywhere in first node preserves native query");
      node_a = original_node_a;
      put(node_b, 0x38, stale_refs);
      require(graph_uses(Lane::Native, true),
              "stale creator anywhere in second node preserves native query");
      node_b = original_node_b;
    }
    creator_throws = true;
    require(graph_uses(Lane::Native, true),
            "creator resolver exception preserves native collision");
    creator_throws = false;
    std::array<std::uint64_t, 1> changing_creators{road_weak};
    put(node_a, 0x38,
        NativeArray<std::uint64_t>{changing_creators.data(), 1, 1});
    mutating_creator = changing_creators.data();
    require(graph_uses(Lane::Native, true),
            "creator content changing during weak resolution fails closed");
    node_a = original_node_a;
    mutating_descriptor = node_a.data() + 0x38;
    require(graph_uses(Lane::Native, true),
            "creator descriptor changing during weak resolution fails closed");
    node_a = original_node_a;

    // Read-only runtime capture, wall-correlation-2464900.json, 2026-10-06:
    // nodes 6566, 6567, 6565 each had zero links and suppression byte zero.
    // Their XY positions are exactly the adjoining wall-strip centerlines.
    const std::array<Vec3, 3> captured_positions{{
        {-12137.130903685342, 25674.15472939656, 1129.5155029296875},
        {-12344.52904552224, 25572.887439463353, 1151.8919677734375},
        {-11914.133276511577, 25733.633082228313, 1110.444580078125},
    }};
    const std::array<std::array<Vec3, 4>, 2> captured_wall_quads{{
        {{{-12426.733142522116, 25721.69095567327, 1151.75},
          {-12196.610075741974, 25833.409958797223, 1129.176513671875},
          {-12077.65173162871, 25514.899499995896, 1129.176513671875},
          {-12262.324948522364, 25424.083923253434, 1151.75}}},
        {{{-12196.610075741974, 25833.409958797223, 1129.176513671875},
          {-11937.511532692406, 25902.017930531074, 1110.356201171875},
          {-11890.755020330747, 25565.24823392555, 1110.356201171875},
          {-12077.65173162871, 25514.899499995896, 1129.176513671875}}},
    }};
    const auto original_start_xy = start_xy, original_end_xy = end_xy;
    fixture_module_quads.assign(captured_wall_quads.begin(),
                                captured_wall_quads.end());
    for (std::size_t i = 0; i < captured_wall_quads.size(); ++i) {
      const auto &quad = captured_wall_quads[i];
      const auto &first = captured_positions[i == 0 ? 1 : 0];
      const auto &second = captured_positions[i == 0 ? 0 : 2];
      require(
          std::abs(first.x - (quad[0].x + quad[3].x) / 2) < 1e-8 &&
              std::abs(first.y - (quad[0].y + quad[3].y) / 2) < 1e-8 &&
              std::abs(second.x - (quad[1].x + quad[2].x) / 2) < 1e-8 &&
              std::abs(second.y - (quad[1].y + quad[2].y) / 2) < 1e-8 &&
              std::abs(
                  std::hypot(quad[0].x - quad[3].x, quad[0].y - quad[3].y) / 2 -
                  170) < 1e-8,
          "captured zero-link Manor nodes lie on the 170cm half-width "
          "wall centerline");
      start_xy = {captured_positions[0].x, captured_positions[0].y};
      end_xy = {captured_positions[i + 1].x, captured_positions[i + 1].y};
      put(node_a, 0x38, manor_refs);
      put(node_b, 0x38, manor_refs);
      native_result = false;
      placement_result = true;
      require(graph_uses(Lane::Native, false),
              "captured boundary segment uses the unmodified native result "
              "despite a blocking wall-module fixture");
      native_result = true;
      placement_result = false;
      require(graph_uses(Lane::Native, true),
              "captured boundary segment still preserves native rejection");
      put(node_b, 0x38, road_refs);
      native_result = false;
      placement_result = true;
      require(graph_uses(Lane::Manor, true),
              "the same captured coordinates remain physically blocked for "
              "a Manor-to-road candidate");
    }
    // This tests callback dispatch with captured coordinates, not a substitute
    // implementation of the native graph's candidate search or link insertion.
    fixture_module_quads.clear();
    start_xy = original_start_xy;
    end_xy = original_end_xy;
    node_a = original_node_a;
    node_b = original_node_b;
    native_result = true;
    placement_result = false;

    graph_nested_query = true;
    calls.clear();
    snap_build_graph(work.data());
    require(calls.size() == 8 && calls[0].lane == Lane::Manor &&
                calls[1].lane == Lane::Manor && calls[6].lane == Lane::Manor &&
                calls[7].lane == Lane::Manor,
            "nested ineligible graph restores the outer eligible scope");
    for (std::size_t i = 2; i < 6; ++i)
      require(calls[i].lane == Lane::Native,
              "nested ineligible graph masks outer graph scope");
    require(calls[6].verdict_hit &&
                calls[6].verdict_slot == calls[0].verdict_slot,
            "nested ineligible graph preserves the outer memo");
    graph_nested_eligible = true;
    calls.clear();
    snap_build_graph(work.data());
    require(
        calls.size() == 8 && !calls[0].verdict_hit && !calls[2].verdict_hit &&
            calls[0].verdict_slot != 0 && calls[2].verdict_slot != 0 &&
            calls[0].verdict_slot != calls[2].verdict_slot &&
            calls[6].verdict_hit &&
            calls[6].verdict_slot == calls[0].verdict_slot,
        "nested eligible graph with the same cache owns an independent memo");
    graph_nested_eligible = false;
    graph_nested_query = false;
    path_graph_query = true;
    calls.clear();
    snap_project_path(cache.data(), &output, plot_points.data(), 25.5f, 3,
                      pawn.data());
    require(calls.size() == 6 && calls[0].lane == Lane::Manor &&
                calls[5].lane == Lane::Native,
            "an active plot query never grants graph collision scope");
    path_graph_query = false;
    graph_path_query = true;
    calls.clear();
    snap_build_graph(work.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Manor &&
                calls[4].lane == Lane::Native,
            "an active graph query never grants plot collision scope");
    graph_path_query = false;
    const auto rejected_graph = [&] {
      calls.clear();
      const auto previous_calls = graph_calls;
      return snap_build_graph(work.data()) == graph_return &&
             graph_calls == previous_calls + 1 && calls.size() == 4 &&
             calls[0].lane == Lane::Native && calls[3].lane == Lane::Native;
    };
    put(cache, 0x20, std::uintptr_t{0});
    require(rejected_graph(), "missing region retains stock graph collision");
    put(cache, 0x20, std::uintptr_t{1});
    require(rejected_graph(),
            "unreadable region retains stock graph collision");
    cache = original_cache;
    put(region, 0x738, other_cache.data());
    require(rejected_graph(), "reverse cache identity must match");
    region = original_region;
    put(region, 0x350, std::uintptr_t{0});
    require(rejected_graph(),
            "region without owner retains stock graph collision");
    put(region, 0x350, std::uintptr_t{1});
    require(rejected_graph(), "unreadable owner retains stock graph collision");
    region = original_region;
    for (std::uint8_t player : {0, 2}) {
      put(pawn, 0x34c, player);
      require(rejected_graph(), "only exact main-player ownership is admitted");
    }
    pawn = original_pawn;
    put(region, 0x358, std::uintptr_t{0});
    require(rejected_graph(), "missing Manor retains stock graph collision");
    put(region, 0x358, std::uintptr_t{1});
    require(rejected_graph(), "unreadable Manor retains stock graph collision");
    region = original_region;
    put(manor, 0x3b8, std::int32_t{99});
    require(rejected_graph(),
            "other building types never receive Manor exemption");
    manor = original_manor;
    for (std::uint32_t flags : {0x8000U, 0x10000U, 0x40000000U}) {
      put(manor, 8, flags);
      require(rejected_graph(), "destroying and garbage Manor fail closed");
    }
    manor = original_manor;
    geometry_complete = false;
    require(rejected_graph(), "incomplete graph snapshot retains stock query");
    geometry_complete = true;
    geometry_throws = true;
    require(rejected_graph(), "failed graph snapshot retains stock query");
    geometry_throws = false;
    graph_wrong_cache = true;
    require(rejected_graph(), "graph scope cannot relax another cache");
    graph_wrong_cache = false;
    put(work, 0x18, std::uint64_t{0});
    require(rejected_graph(), "missing weak cache handle retains stock query");
    work = original_work;
    calls.clear();
    require(
        snap_build_graph(nullptr) == graph_return && calls.size() == 4 &&
            calls[0].lane == Lane::Native,
        "unreadable work descriptor still reaches native original unchanged");
    graph_thread_check = true;
    calls.clear();
    snap_build_graph(work.data());
    require(calls.size() == 5 && calls[0].lane == Lane::Manor &&
                calls[4].lane == Lane::Native,
            "graph batch scope is local to its own worker thread");
    graph_thread_check = false;
    graph_throws = true;
    caught = false;
    try {
      snap_build_graph(work.data());
    } catch (const std::runtime_error &) {
      caught = true;
    }
    graph_throws = false;
    calls.clear();
    snap_graph_segment_collision(cache.data(), start_xy.data(), end_xy.data(),
                                 graph_node_a, graph_node_b);
    require(caught && calls.size() == 1 && calls[0].lane == Lane::Native,
            "graph native exception restores worker scope");
    require(work == original_work && cache[0x61a] == std::byte{1} &&
                cache[0x63a] == std::byte{1},
            "work handle and cached node flags are never directly edited");

    require(
        pawn == original_pawn && other_pawn == original_other_pawn &&
            cache == original_cache && other_cache == original_other_cache &&
            region == original_region && manor == original_manor &&
            plot_points == original_plot_points && node_a == original_node_a &&
            node_b == original_node_b && manor_creators[0] == manor_weak &&
            road_creators[0] == road_weak,
        "all fake game objects and plot data remain byte-identical");
    std::printf("Snap scope tests passed: %d\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Snap tests failed after %d checks: %s\n", checks,
                 error.what());
    return 1;
  }
}

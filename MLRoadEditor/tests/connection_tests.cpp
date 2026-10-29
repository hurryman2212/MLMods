#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

#include "Connections.hpp"

static const NativeArray<Vec3> *manor_triangles{}, *other_triangles{};
static void *expected_manor{}, *expected_nav{}, *expected_cache{};
static bool complete{true}, empty_geometry{}, geometry_throws{},
    physical_block{}, weak_failure{}, nested_query{}, native_throws{},
    thread_query{}, other_query{};
static int geometry_calls{}, nav_calls{}, cache_calls{},
    original_circle_calls{}, physical_circle_calls{}, weak_calls{}, depth{};
static int circle_repetitions{1}, query_mismatch{};
static std::uint8_t native_value{7}, cache_value{7}, nested_value{},
    thread_value{};
static std::uint64_t observed_weak{}, assigned_weak{0x3322110000000066ULL};
static float observed_radius{};
static std::int32_t observed_cache_radius{};
static const Vec3 *observed_point{};

ManorGeometry build_manor_geometry(void *manor, std::uintptr_t image) {
  ++geometry_calls;
  if (geometry_throws) {
    throw std::runtime_error("synthetic geometry exception");
  }
  if (manor != expected_manor || image != 0x140000000) {
    throw std::runtime_error("wrong Manor geometry context");
  }
  ManorGeometry result;
  result.complete = complete;
  if (!empty_geometry) {
    result.module_quads.push_back(
        {{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}});
  }
  return result;
}

static std::uint8_t original_circle(const Vec3 *point, float radius,
                                    const NativeArray<Vec3> *triangles) {
  observed_radius = radius;
  if (triangles == manor_triangles || triangles == other_triangles) {
    ++original_circle_calls;
    observed_point = point;
    return native_value;
  }
  ++physical_circle_calls;
  if (triangles == nullptr || triangles->count != 6 ||
      triangles->capacity != 6) {
    throw std::runtime_error("invalid private physical footprint triangles");
  }
  return physical_block ? 1 : 0;
}

static std::uint8_t original_nav(void *nav, const Vec3 *point, float radius) {
  ++nav_calls;
  if (nav != expected_nav) {
    return native_value;
  }
  ++depth;
  if (native_throws) {
    --depth;
    throw std::runtime_error("synthetic native exception");
  }
  if (nested_query && depth == 1) {
    nested_value = connections_initial_point(nav, point, radius, nullptr);
  }
  if (thread_query) {
    std::thread worker([&] {
      thread_value = connections_nav_circle(point, radius, manor_triangles);
    });
    worker.join();
  }
  const Vec3 copied_point = *point;
  std::uint8_t result{};
  for (int index{}; index < circle_repetitions; ++index) {
    result =
        connections_nav_circle(query_mismatch == 1 ? &copied_point : point,
                               query_mismatch == 2 ? radius + 1.0F : radius,
                               other_query ? other_triangles : manor_triangles);
  }
  --depth;
  return result;
}

static std::uint8_t original_cache(void *cache, const Vec3 *point,
                                   std::int32_t radius, std::uint64_t ignored) {
  ++cache_calls;
  if (cache != expected_cache) {
    throw std::runtime_error("wrong original regional cache");
  }
  observed_cache_radius = radius;
  observed_weak = ignored;
  observed_point = point;
  return cache_value;
}

static void assign_weak(std::uint64_t *output, void *manor) {
  ++weak_calls;
  if (manor != expected_manor) {
    throw std::runtime_error("wrong ignored Manor identity");
  }
  *output = weak_failure ? 0 : assigned_weak;
}

template <class T, std::size_t Size>
static void put(std::array<std::byte, Size> &object, std::size_t offset,
                const T &value) {
  std::memcpy(object.data() + offset, &value, sizeof(value));
}

struct ConnectionFixture {
  std::array<std::byte, 0x1000> building{}, manor{}, other{}, pawn{};
  std::array<std::byte, 0x800> region{};
  std::array<std::byte, 0x400> manager{};
  std::array<std::byte, 0x40> nav{}, cache{};
  Vec3 point{25, 35, 45};

  ConnectionFixture() {
    // Current native cache construction reads Region+318 as its manager;
    // owner fields match the audited Building+2c8/+2e8 and Region+738 links.
    put(building, 0x2c8, region.data());
    put(building, 0x2e8, manager.data());
    put(manor, 0x2c8, region.data());
    put(manor, 0x2e8, manager.data());
    put(manor, 0x3b8, std::int32_t{98});
    put(other, 0x2c8, region.data());
    put(other, 0x2e8, manager.data());
    put(region, 0x318, manager.data());
    put(region, 0x350, pawn.data());
    put(region, 0x358, manor.data());
    put(region, 0x738, cache.data());
    put(manager, 0x2c0, nav.data());
    put(pawn, 0x34c, std::uint8_t{1});
    put(pawn, 0x8d8, std::uint8_t{0xa5});
    put(pawn, 0x8d9, std::uint8_t{0x5a});
    manor_triangles =
        reinterpret_cast<const NativeArray<Vec3> *>(manor.data() + 0x688);
    other_triangles =
        reinterpret_cast<const NativeArray<Vec3> *>(other.data() + 0x688);
    expected_manor = manor.data();
    expected_nav = nav.data();
    expected_cache = cache.data();
    configure_connections(0x140000000);
    configure_connection_functions(original_nav, original_cache,
                                   original_circle, assign_weak);
    reset();
  }
  static void reset() {
    complete = true;
    empty_geometry = geometry_throws = physical_block = weak_failure = false;
    nested_query = native_throws = thread_query = other_query = false;
    geometry_calls = nav_calls = cache_calls = original_circle_calls = 0;
    physical_circle_calls = weak_calls = depth = 0;
    circle_repetitions = 1;
    query_mismatch = 0;
    native_value = cache_value = 7;
    nested_value = thread_value = 0;
    observed_weak = 0;
    observed_point = nullptr;
  }
};

int main() {
  int checks{}, failures{};
  const auto require = [&](bool value, const char *message) {
    ++checks;
    if (!value) {
      ++failures;
      std::fprintf(stderr, "connections: %s\n", message);
    }
  };
  {
    ConnectionFixture fixture;
    const auto building = fixture.building, manor = fixture.manor,
               other = fixture.other, pawn = fixture.pawn;
    const auto manager = fixture.manager;
    const auto region = fixture.region;
    require(
        resolve_connection_manor(fixture.building.data(), fixture.nav.data(),
                                 fixture.cache.data()) == fixture.manor.data(),
        "current native owner/cache/Nav links resolve the player Manor");
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 2.5F,
                                      fixture.building.data()) == 0,
            "initial entrance query bypasses only the empty courtyard portion");
    require(nav_calls == 1 && original_circle_calls == 1 &&
                physical_circle_calls == 1 && geometry_calls == 1 &&
                observed_radius == 2.5F && observed_point == &fixture.point,
            "initial native query and original circle execute once with "
            "preserved arguments");
    require(connections_nav_circle(&fixture.point, 2.5F, manor_triangles) == 7,
            "initial-query exception cannot escape its lexical scope");
    require(connections_mask_circle(&fixture.point, 3.25F, manor_triangles,
                                    fixture.manor.data()) == 0,
            "Manor masking preserves an entrance outside physical components");
    require(connections_mask_circle(&fixture.point, 3.25F, other_triangles,
                                    fixture.other.data()) == 7,
            "ordinary building masks retain native collision results");
    require(connections_mask_circle(&fixture.point, 3.25F, other_triangles,
                                    fixture.manor.data()) == 7,
            "wrong triangle ownership cannot gain the Manor exception");
    require(connections_restore_point(fixture.cache.data(), &fixture.point, 5,
                                      0xffffffffU,
                                      fixture.building.data()) == 7,
            "restoration retains the complete native return byte");
    require(cache_calls == 1 && weak_calls == 1 &&
                observed_weak == assigned_weak && observed_cache_radius == 5 &&
                observed_point == &fixture.point,
            "restoration replaces only its local null-serial ignore with the "
            "exact Manor");
    require(fixture.building == building && fixture.manor == manor &&
                fixture.other == other && fixture.pawn == pawn &&
                fixture.region == region && fixture.manager == manager,
            "callbacks never modify geometry, save state, actor fields or UI "
            "toggle bits");
  }
  for (int settings{}; settings < 4; ++settings) {
    ConnectionFixture fixture;
    put(fixture.pawn, 0x8d8, static_cast<std::uint8_t>(settings & 1));
    put(fixture.pawn, 0x8d9, static_cast<std::uint8_t>((settings >> 1) & 1));
    const auto pawn = fixture.pawn;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 2.5F,
                                      fixture.building.data()) == 0 &&
                connections_mask_circle(&fixture.point, 2.5F, manor_triangles,
                                        fixture.manor.data()) == 0 &&
                connections_restore_point(fixture.cache.data(), &fixture.point,
                                          5, 0, fixture.building.data()) == 7,
            "all four road/building toggle combinations retain Manor courtyard "
            "connections");
    require(fixture.pawn == pawn,
            "road/building toggle bytes are unchanged for all four settings");
  }
  {
    ConnectionFixture fixture;
    physical_block = true;
    cache_value = 0;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 1.5F,
                                      fixture.building.data()) == 1,
            "physical Manor components continue to block initial entrances");
    require(connections_mask_circle(&fixture.point, 1.5F, manor_triangles,
                                    fixture.manor.data()) == 1,
            "physical Manor components continue to mask overlapping entrances");
    require(connections_restore_point(fixture.cache.data(), &fixture.point, 2,
                                      0, fixture.building.data()) == 1 &&
                cache_calls == 1 && weak_calls == 0 && observed_weak == 0,
            "physical collision blocks restoration while native query still "
            "executes once");
  }
  for (int mode{}; mode < 7; ++mode) {
    ConnectionFixture fixture;
    switch (mode) {
    case 0:
      complete = false;
      break;
    case 1:
      empty_geometry = true;
      break;
    case 2:
      geometry_throws = true;
      break;
    case 3:
      put(fixture.pawn, 0x34c, std::uint8_t{0});
      break;
    case 4:
      put(fixture.region, 0x318, fixture.other.data());
      break;
    case 5:
      put(fixture.manor, 8, std::uint32_t{0x8000});
      break;
    case 6:
      put(fixture.manor, 0x3b8, std::int32_t{57});
      break;
    }
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 2.5F,
                                      fixture.building.data()) == 7 &&
                nav_calls == 1,
            "invalid context or incomplete geometry preserves the initial "
            "native query");
    require(connections_mask_circle(&fixture.point, 2.5F, manor_triangles,
                                    fixture.manor.data()) == 7,
            "invalid masking context preserves the original circle");
    require(connections_restore_point(fixture.cache.data(), &fixture.point, 5,
                                      0, fixture.building.data()) == 7 &&
                cache_calls == 1 && weak_calls == 0,
            "invalid restore context preserves the original query and ignore");
  }
  {
    ConnectionFixture fixture;
    const std::uint64_t existing = 0x1234567800000001ULL;
    require(connections_restore_point(fixture.cache.data(), &fixture.point, 5,
                                      existing, fixture.building.data()) == 7 &&
                observed_weak == existing && weak_calls == 0 &&
                geometry_calls == 0,
            "a pre-existing native ignored actor is never replaced");
    weak_failure = true;
    require(
        connections_restore_point(fixture.cache.data(), &fixture.point, 5,
                                  0xffffffffU, fixture.building.data()) == 7 &&
            observed_weak == 0xffffffffU,
        "failed weak assignment restores the exact original ignore encoding");
  }
  {
    ConnectionFixture fixture;
    nested_query = true;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 1.25F,
                                      fixture.building.data()) == 0 &&
                nested_value == 7 && nav_calls == 2,
            "ineligible nested native query masks and then restores the outer "
            "context");
    nested_query = false;
    thread_query = true;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 1.25F,
                                      fixture.building.data()) == 0 &&
                thread_value == 7,
            "Manor filtering context never leaks to another thread");
    fixture.reset();
    other_query = true;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 1.25F,
                                      fixture.building.data()) == 7 &&
                geometry_calls == 0 && physical_circle_calls == 0 &&
                nav_calls == 1 && original_circle_calls == 1,
            "unrelated triangles retain native behavior without building Manor "
            "geometry");
    fixture.reset();
    native_throws = true;
    bool caught{};
    try {
      connections_initial_point(fixture.nav.data(), &fixture.point, 1.25F,
                                fixture.building.data());
    } catch (const std::runtime_error &) {
      caught = true;
    }
    require(
        caught && geometry_calls == 0 &&
            connections_nav_circle(&fixture.point, 1.25F, manor_triangles) == 7,
        "native exceptions restore the previous thread-local scope");
  }
  for (int mismatch = 1; mismatch <= 2; ++mismatch) {
    ConnectionFixture fixture;
    query_mismatch = mismatch;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 1.25F,
                                      fixture.building.data()) == 7 &&
                geometry_calls == 0 && physical_circle_calls == 0 &&
                nav_calls == 1 && original_circle_calls == 1,
            "a different point pointer or radius cannot activate or build "
            "Manor geometry");
  }
  for (int mode{}; mode < 4; ++mode) {
    ConnectionFixture fixture;
    circle_repetitions = 2;
    complete = mode != 1;
    empty_geometry = mode == 2;
    geometry_throws = mode == 3;
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 1.25F,
                                      fixture.building.data()) ==
                    (mode == 0 ? 0 : 7) &&
                nav_calls == 1 && geometry_calls == 1 &&
                original_circle_calls == 2 &&
                physical_circle_calls == (mode == 0 ? 2 : 0),
            "repeated matching callbacks attempt geometry only once, including "
            "incomplete, empty, and failed snapshots");
  }
  {
    ConnectionFixture fixture;
    fixture.point.x = std::numeric_limits<double>::quiet_NaN();
    require(connections_initial_point(fixture.nav.data(), &fixture.point, 2.5F,
                                      fixture.building.data()) == 7 &&
                physical_circle_calls == 0,
            "nonfinite points do not activate custom geometry filtering");
    require(connections_restore_point(fixture.cache.data(), &fixture.point, -1,
                                      0, fixture.building.data()) == 7 &&
                observed_cache_radius == -1 && weak_calls == 0,
            "negative native clearance retains native behavior unchanged");
  }
  std::printf("connections: %d checks, %d failures\n", checks, failures);
  return failures;
}

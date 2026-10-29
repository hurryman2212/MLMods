#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "CastlePreview.hpp"
#include "Geometry.hpp"

struct ReadableRange {
  std::uintptr_t begin{};
  std::size_t size{};
};

static std::vector<ReadableRange> readable_ranges;
static std::vector<std::pair<void *, std::uint64_t>> weak_objects;
static std::atomic<std::uint64_t> test_time{1000}, native_count{};
static bool buttons_down{}, unreadable{}, weak_invalid{};
static void *last_road{};
static std::uint8_t last_force{};
static std::function<void()> native_action;
static int checks{};
static std::array<std::uintptr_t, 3> counted_addresses{};
static std::array<std::size_t, 3> counted_reads{};

static void require(bool condition, const char *message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

template <std::size_t Size, class T>
static void put(std::array<std::byte, Size> &object, std::size_t offset,
                const T &value) {
  if (offset > Size || sizeof(T) > Size - offset)
    throw std::runtime_error("fixture write outside allocation");
  std::memcpy(object.data() + offset, &value, sizeof(value));
}

static bool fake_read(std::uintptr_t address, void *output, std::size_t size) {
  if (unreadable)
    return false;
  for (const auto &range : readable_ranges)
    if (address >= range.begin && address - range.begin <= range.size &&
        size <= range.size - (address - range.begin)) {
      std::memcpy(output, reinterpret_cast<const void *>(address), size);
      return true;
    }
  return false;
}

static bool counted_read(std::uintptr_t address, void *output,
                         std::size_t size) {
  for (std::size_t index{}; index < counted_addresses.size(); ++index)
    if (address == counted_addresses[index])
      ++counted_reads[index];
  return fake_read(address, output, size);
}

static std::uint64_t fake_clock() { return test_time.load(); }
static bool fake_buttons() { return buttons_down; }

static void fake_weak(std::uint64_t *weak, void *object) {
  *weak = 0;
  if (weak_invalid)
    return;
  for (const auto &entry : weak_objects)
    if (entry.first == object) {
      *weak = entry.second;
      return;
    }
}

static void original_preview(void *road, std::uint8_t force) {
  ++native_count;
  last_road = road;
  last_force = force;
  const auto action = native_action;
  if (action)
    action();
}

struct Fixture {
  std::array<std::byte, 0xd00> pawn{};
  std::array<std::byte, 0x1080> master{};
  std::array<std::byte, 0xf00> parent{};
  std::array<std::byte, 0x500> road{}, active{};
  std::array<std::byte, 0x40> road_class{};
  std::array<std::byte, 0x280> root{}, spline{}, component{}, other_component{},
      selected{};
  std::array<Vec3, 2> source{{{0, 0, 0}, {100, 0, 0}}};
  std::array<Vec3, 2> active_source{{{1000, 1000, 0}, {1100, 1000, 0}}};
  std::array<std::uintptr_t, 1> components{};
  std::array<std::uintptr_t, 2> roads{}, materials{};

  template <class T, std::size_t Size>
  void readable(std::array<T, Size> &values) {
    readable_ranges.push_back(
        {reinterpret_cast<std::uintptr_t>(values.data()), sizeof(values)});
  }

  template <std::size_t Size> void object(std::array<std::byte, Size> &bytes) {
    readable(bytes);
    const auto index = static_cast<std::uint32_t>(weak_objects.size() + 1);
    put(bytes, 0xc, index);
    std::uintptr_t existing_class{};
    std::memcpy(&existing_class, bytes.data() + 0x10, sizeof(existing_class));
    if (existing_class == 0)
      put(bytes, 0x10, bytes.data());
    put(bytes, 0x18, std::uint64_t{index});
    weak_objects.emplace_back(bytes.data(), (std::uint64_t{1} << 32) | index);
  }

  void active_box(double min_x, double min_y, double max_x, double max_y) {
    active_source = {{{min_x, min_y, 0}, {max_x, max_y, 0}}};
    put(active, 0x370, std::array<double, 4>{min_x, min_y, max_x, max_y});
  }

  Fixture() {
    readable_ranges.clear();
    weak_objects.clear();
    test_time = 1000;
    native_count = 0;
    buttons_down = unreadable = weak_invalid = false;
    native_action = {};
    object(pawn);
    object(master);
    object(parent);
    object(road);
    object(active);
    readable(road_class);
    put(road, 0x10, road_class.data());
    put(active, 0x10, road_class.data());
    object(root);
    object(spline);
    object(component);
    object(other_component);
    object(selected);
    readable(source);
    readable(active_source);
    readable(components);
    readable(roads);
    readable(materials);
    put(pawn, 0x340, master.data());
    put(pawn, 0x34c, std::uint8_t{1});
    put(pawn, 0x778, active.data());
    put(pawn, 0x7a0, std::uint8_t{1});
    put(pawn, 0x840, parent.data());
    put(pawn, 0x848, std::uint8_t{1});
    put(parent, 0x2e8, master.data());
    put(master, 0x5b0, pawn.data());
    put(road, 0x2a8, master.data());
    put(road, 0x2b3, std::uint8_t{1});
    put(road, 0x2b4, std::uint8_t{3});
    put(road, 0x2b5, std::uint8_t{1});
    put(road, 0x2b8, 1.0f);
    put(road, 0x2e8, 20.0f);
    put(road, 0x1b8, root.data());
    put(road, 0x2c0, spline.data());
    put(road, 0x318, NativeArray<Vec3>{source.data(), 2, 2});
    put(road, 0x370, std::array<double, 4>{-10, -10, 110, 10});
    put(road, 0x390, std::uint8_t{1});
    put(root, 0x1d0,
        std::array<double, 12>{0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0});
    components = {reinterpret_cast<std::uintptr_t>(component.data())};
    roads = {reinterpret_cast<std::uintptr_t>(road.data()),
             reinterpret_cast<std::uintptr_t>(active.data())};
    materials = {101, 102};
    put(road, 0x2d8, NativeArray<std::uintptr_t>{components.data(), 1, 1});
    put(master, 0x590, NativeArray<std::uintptr_t>{roads.data(), 2, 2});
    put(parent, 0xef0, NativeArray<std::uintptr_t>{roads.data(), 2, 2});
    put(master, 0xd68, NativeArray<std::uintptr_t>{materials.data(), 2, 2});
    put(master, 0xfe0, NativeArray<std::uintptr_t>{materials.data(), 2, 2});
    put(active, 0x2a8, master.data());
    put(active, 0x318, NativeArray<Vec3>{active_source.data(), 2, 2});
    put(active, 0x390, std::uint8_t{1});
    put(active, 0x2e8, 20.0f);
    active_box(1000, 1000, 1100, 1010);
    configure_castle_preview_functions(original_preview, fake_clock,
                                       fake_buttons, fake_weak, fake_read);
  }

  void call_road(void *value, std::uint8_t force = 1) {
    NativeArray<std::uintptr_t> siblings{};
    std::memcpy(&siblings, parent.data() + 0xef0, sizeof(siblings));
    auto *iterator = siblings.data;
    for (std::int32_t index = 0; index < siblings.count; ++index)
      if (siblings.data[index] == reinterpret_cast<std::uintptr_t>(value)) {
        iterator = siblings.data + index;
        break;
      }
    castle_preview_regenerate(
        value, force, pawn.data(), reinterpret_cast<std::uintptr_t>(iterator),
        reinterpret_cast<std::uintptr_t>(siblings.data + siblings.count));
  }

  void call(std::uint8_t force = 1) { call_road(road.data(), force); }

  void prime() {
    call();
    call();
    require(native_count == 1, "fixture must reach the reuse path");
  }
};

template <std::size_t Count> struct BatchFixture : Fixture {
  std::array<std::array<std::byte, 0x500>, Count> siblings;
  std::array<std::uintptr_t, Count> parent_roads{};

  BatchFixture() {
    for (std::size_t index = 0; index < Count; ++index) {
      siblings[index] = road;
      object(siblings[index]);
      parent_roads[index] =
          reinterpret_cast<std::uintptr_t>(siblings[index].data());
    }
    readable(parent_roads);
    put(parent, 0xef0,
        NativeArray<std::uintptr_t>{parent_roads.data(),
                                    static_cast<std::int32_t>(Count),
                                    static_cast<std::int32_t>(Count)});
  }

  void pass() {
    for (const auto pointer : parent_roads)
      call_road(reinterpret_cast<void *>(pointer));
  }
};

static void test_reuse_and_geometry() {
  Fixture fixture;
  const auto initial_pawn = fixture.pawn;
  const auto initial_road = fixture.road;
  const auto initial_master = fixture.master;
  const auto initial_component = fixture.component;
  const auto before = castle_preview_metrics();
  fixture.prime();
  fixture.active_box(2000, 1000, 2200, 1001);
  fixture.call();
  require(native_count == 1, "moving farther away reuses unchanged graphics");
  require(fixture.pawn == initial_pawn && fixture.road == initial_road &&
              fixture.master == initial_master &&
              fixture.component == initial_component,
          "cache never mutates game inputs or retained component data");
  fixture.active_box(110, -10, 130, 10);
  fixture.call();
  fixture.call();
  require(native_count == 3,
          "touching retained expanded bounds always rebuilds");
  fixture.active_box(20, -5, 80, 5);
  fixture.call();
  require(native_count == 4, "overlapping preview always rebuilds");
  fixture.active_box(2000, 1000, 2200, 1001);
  fixture.call();
  require(native_count == 5, "leaving an overlap rebuilds once to remove cuts");
  fixture.call();
  require(native_count == 5, "the next distant frame reuses restored graphics");
  // Cached native bounds may be temporarily behind active source movement.
  fixture.active_source = {{{20, -5, 0}, {80, 5, 0}}};
  fixture.call();
  require(native_count == 6,
          "active source union prevents stale-box false misses");
  const auto after = castle_preview_metrics();
  require(after.calls - before.calls == 9 &&
              after.reused - before.reused == 3 &&
              after.native - before.native == 6,
          "tick metrics account for each native or reused wrapper invocation");
}

static void test_lifetime_and_buttons() {
  Fixture fixture;
  fixture.prime();
  test_time = 1249;
  fixture.call();
  require(native_count == 1, "249 ms remains inside the bounded lifetime");
  test_time = 1250;
  fixture.call();
  require(native_count == 2, "250 ms forces native refresh");
  test_time = 1200;
  fixture.call();
  require(native_count == 3, "backward time invalidates snapshots");
  fixture.call();
  require(native_count == 3, "cache recovers after backward time refresh");
  buttons_down = true;
  fixture.call();
  fixture.call();
  require(native_count == 5, "either held mouse button always uses native");
  buttons_down = false;
  fixture.call();
  fixture.call();
  require(native_count == 6,
          "released mouse needs a full rebuild before reuse");
  castle_preview_reset();
  native_action = [] { test_time += 1000; };
  fixture.call();
  native_action = {};
  fixture.call();
  require(native_count == 7,
          "lifetime starts when a costly native call completes");
}

static void test_castle_wall_preview() {
  Fixture fixture;
  put(fixture.active, 0x2b4, std::uint8_t{3});
  fixture.prime();
  fixture.active_box(20, -5, 80, 5);
  fixture.call();
  require(native_count == 1,
          "overlapping active castle walls are ignored by native road cuts");
  put(fixture.active, 0x4b8, std::uint8_t{1});
  fixture.call();
  fixture.call();
  require(native_count == 2,
          "active wall render flags require one native refresh before reuse");
  put(fixture.active, 0x2b4, std::uint8_t{0});
  fixture.call();
  fixture.call();
  require(native_count == 4,
          "changing castle wall to overlapping ordinary road always rebuilds");
  put(fixture.active, 0x2b4, std::uint8_t{3});
  fixture.call();
  fixture.call();
  require(
      native_count == 5,
      "changing cut-producing road to castle wall cleans up once then reuses");
  put(fixture.active, 0x2b4, std::uint8_t{0});
  fixture.active_box(1000, 1000, 1100, 1010);
  fixture.call();
  fixture.call();
  require(native_count == 6,
          "changing wall to distant ordinary road rebuilds once then reuses");
  put(fixture.active, 0x2b4, std::uint8_t{3});
  put(fixture.active, 0x390, std::uint8_t{0});
  fixture.call();
  fixture.call();
  require(native_count == 8,
          "castle wall previews still require complete validated geometry");
}

static void test_exact_dependencies() {
  Fixture fixture;
  fixture.prime();
  const auto changed = [&fixture](const char *message) {
    const auto before = native_count.load();
    fixture.call();
    fixture.call();
    require(native_count == before + 1, message);
  };
  fixture.source[1].x = 101;
  changed("same-buffer source edits invalidate cached geometry");
  put(fixture.road, 0x2b8, 2.0f);
  changed("tangent changes invalidate cached geometry");
  put(fixture.road, 0x2e8, 21.0f);
  changed("width changes invalidate cached geometry");
  put(fixture.road, 0x330, std::uintptr_t{99});
  changed("road material changes invalidate cached geometry");
  put(fixture.road, 0x420, std::uintptr_t{101});
  changed("dynamic material changes invalidate cached geometry");
  put(fixture.road, 0x4b8, std::uint8_t{1});
  changed("render flag changes invalidate cached geometry");
  put(fixture.root, 0x1f0, 11.0);
  changed("root world transform changes invalidate cached geometry");
  fixture.components[0] =
      reinterpret_cast<std::uintptr_t>(fixture.other_component.data());
  changed("generated component replacement invalidates retained output");
  for (auto &entry : weak_objects)
    if (entry.first == fixture.other_component.data())
      entry.second += std::uint64_t{1} << 32;
  changed("component serial reuse invalidates retained output");
  fixture.materials[0] = 103;
  changed("same-buffer master material changes invalidate cached geometry");
  put(fixture.master, 0x1068, std::uintptr_t{123});
  changed("master material selector changes invalidate cached geometry");
  std::swap(fixture.roads[0], fixture.roads[1]);
  changed("same-buffer world road list changes invalidate cached geometry");
  put(fixture.master, 0x5b8, std::uintptr_t{77});
  changed("landscape identity changes invalidate cached geometry");
  put(fixture.pawn, 0x858, fixture.selected.data());
  changed("selected object changes invalidate cached geometry");
  for (auto &entry : weak_objects)
    if (entry.first == fixture.master.data())
      entry.second += std::uint64_t{1} << 32;
  changed("master serial reuse invalidates the session");
}

static void test_scope_and_failures() {
  Fixture fixture;
  fixture.prime();
  const auto rejected = [&fixture](const char *message) {
    const auto before = native_count.load();
    fixture.call();
    fixture.call();
    require(native_count == before + 2, message);
  };
  put(fixture.pawn, 0x34c, std::uint8_t{0});
  rejected("non-player pawn is never optimized");
  put(fixture.pawn, 0x34c, std::uint8_t{1});
  put(fixture.pawn, 0x848, std::uint8_t{0});
  rejected("non-planning placement is never optimized");
  put(fixture.pawn, 0x848, std::uint8_t{1});
  put(fixture.pawn, 0x7a0, std::uint8_t{0});
  rejected("non-road mode is never optimized");
  put(fixture.pawn, 0x7a0, std::uint8_t{1});
  put(fixture.pawn, 0xc58, std::uint8_t{1});
  rejected("platform mode is never optimized");
  put(fixture.pawn, 0xc58, std::uint8_t{0});
  put(fixture.road, 0x2b3, std::uint8_t{0});
  rejected("finalized walls are never optimized");
  put(fixture.road, 0x2b3, std::uint8_t{1});
  put(fixture.road, 0x2b4, std::uint8_t{2});
  rejected("other road types are never optimized");
  put(fixture.road, 0x2b4, std::uint8_t{3});
  put(fixture.road, 0x2b5, std::uint8_t{0});
  rejected("non-wall roads are never optimized");
  put(fixture.road, 0x2b5, std::uint8_t{1});
  put(fixture.active, 0x2b4, std::uint8_t{1});
  rejected("unsupported active preview type always uses native");
  put(fixture.active, 0x2b4, std::uint8_t{2});
  rejected("unsupported active preview type two always uses native");
  put(fixture.active, 0x2b4, std::uint8_t{0});
  put(fixture.pawn, 0x778, fixture.road.data());
  rejected("the active road itself is never optimized");
  put(fixture.pawn, 0x778, fixture.active.data());
  put(fixture.parent, 0x2e8, fixture.pawn.data());
  rejected("mismatched parent world always uses native");
  put(fixture.parent, 0x2e8, fixture.master.data());
  put(fixture.master, 0x5b0, fixture.parent.data());
  rejected("mismatched acting player always uses native");
  put(fixture.master, 0x5b0, fixture.pawn.data());
  put(fixture.parent, 0xef8, std::int32_t{0});
  rejected("roads removed from their parent are never optimized");
  put(fixture.parent, 0xef8, std::int32_t{2});
  put(fixture.road, 0x390, std::uint8_t{0});
  rejected("incomplete native bounds always use native");
  put(fixture.road, 0x390, std::uint8_t{1});
  fixture.source[1].z = std::numeric_limits<double>::quiet_NaN();
  rejected("non-finite geometry always uses native");
  fixture.source[1].z = 0;
  put(fixture.road, 0x320, std::int32_t{2049});
  rejected("oversized source arrays always use native");
  put(fixture.road, 0x320, std::int32_t{2});
  put(fixture.road, 0x324, std::int32_t{1});
  rejected("malformed source arrays always use native");
  put(fixture.road, 0x324, std::int32_t{2});
  weak_invalid = true;
  rejected("unverifiable object serials always use native");
  weak_invalid = false;
  put(fixture.road, 8, std::uint32_t{0x40000000});
  rejected("destroyed UObject flags always use native");
  put(fixture.road, 8, std::uint32_t{0});
  unreadable = true;
  rejected("unreadable objects always use native");
  unreadable = false;
  const auto before = native_count.load();
  fixture.call(0);
  fixture.call(7);
  require(native_count == before + 2 && last_road == fixture.road.data() &&
              last_force == 7,
          "other force values are forwarded unchanged exactly once");
  fixture.call_road(reinterpret_cast<void *>(1));
  require(native_count == before + 3 &&
              last_road == reinterpret_cast<void *>(1),
          "invalid road pointers fall back without unsafe cache reads");
}

static void test_native_post_state_and_exceptions() {
  Fixture fixture;
  native_action = [&fixture] { put(fixture.road, 0x420, std::uintptr_t{321}); };
  fixture.call();
  native_action = {};
  fixture.call();
  require(native_count == 1, "snapshot captures native generated post-state");
  castle_preview_reset();
  native_action = [] { throw std::runtime_error("native exception"); };
  bool threw{};
  try {
    fixture.call();
  } catch (const std::runtime_error &) {
    threw = true;
  }
  require(threw && native_count == 2,
          "native exceptions propagate with no retry");
  native_action = {};
  fixture.call();
  fixture.call();
  require(native_count == 3, "exception leaves no reusable partial snapshot");
  castle_preview_reset();
  native_action = [&fixture] {
    native_action = {};
    fixture.call();
  };
  fixture.call();
  fixture.call();
  require(native_count == 6,
          "nested native rebuild invalidates outer snapshot");
  fixture.call();
  require(native_count == 6, "cache recovers after nested rebuild");
}

static void test_flush_and_threads() {
  Fixture fixture;
  fixture.prime();
  const auto before = castle_preview_metrics();
  test_time = 2000;
  buttons_down = true;
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 2 && last_force == 1,
          "final placement flush rebuilds reused siblings even after lifetime");
  const auto flushed = castle_preview_metrics();
  require(flushed.flushed == before.flushed + 1 &&
              flushed.calls == before.calls && flushed.native == before.native,
          "flush metrics remain separate from tick call accounting");
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 2, "repeated flush has no retained work");
  buttons_down = false;
  fixture.call();
  fixture.call();
  require(native_count == 3, "final placement flush clears reusable state");
  for (auto &entry : weak_objects)
    if (entry.first == fixture.road.data())
      entry.second += std::uint64_t{1} << 32;
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 3,
          "flush never rebuilds an address reused by another object");
  fixture.call();
  fixture.call();
  castle_preview_flush(fixture.parent.data());
  require(native_count == 4, "flush rejects a different acting pawn");
  fixture.call();
  fixture.call();
  std::thread other([&fixture] { fixture.call(); });
  other.join();
  require(native_count == 6,
          "another thread owns a separate first-native cache");
  fixture.call();
  require(native_count == 6,
          "another thread cannot overwrite this thread's cache");
  std::thread resetter([] { castle_preview_reset(); });
  resetter.join();
  fixture.call();
  require(native_count == 7,
          "cross-thread reset invalidates all cache generations");
  fixture.call();
  fixture.roads[0] = reinterpret_cast<std::uintptr_t>(fixture.parent.data());
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 7,
          "flush does not resurrect a live road removed from the parent list");
}

static void test_cache_saturation() {
  Fixture fixture;
  std::array<std::array<std::byte, 0x500>, 129> roads;
  std::array<std::uintptr_t, 129> parent_roads{};
  for (std::size_t index = 0; index < roads.size(); ++index) {
    roads[index] = fixture.road;
    fixture.object(roads[index]);
    parent_roads[index] = reinterpret_cast<std::uintptr_t>(roads[index].data());
  }
  fixture.readable(parent_roads);
  put(fixture.parent, 0xef0,
      NativeArray<std::uintptr_t>{parent_roads.data(), 129, 129});
  for (auto &road : roads)
    fixture.call_road(road.data());
  require(native_count == 129, "every road receives its first native rebuild");
  fixture.call_road(roads.back().data());
  require(native_count == 130, "cache saturation conservatively calls native");
  fixture.call_road(roads.front().data());
  require(native_count == 130,
          "cache saturation retains bounded valid entries");
  std::swap(parent_roads[0], parent_roads[1]);
  fixture.call_road(roads.front().data());
  require(native_count == 131,
          "same-buffer parent topology changes invalidate");
}

static void test_slow_native_batches() {
  BatchFixture<23> fixture;
  std::array<int, 23> visits{};
  native_action = [&] {
    test_time += 15;
    for (std::size_t index = 0; index < visits.size(); ++index)
      if (last_road == fixture.siblings[index].data())
        ++visits[index];
  };
  const auto initial = castle_preview_metrics();
  fixture.pass();
  require(native_count == 23 && test_time == 1345,
          "the initial 23-road native pass takes 345 ms");
  fixture.pass();
  require(native_count == 23 &&
              castle_preview_metrics().reused - initial.reused == 23,
          "the completed slow pass makes all 23 siblings reusable");
  test_time = 1445;
  fixture.pass();
  test_time = 1545;
  fixture.pass();
  require(native_count == 23,
          "unchanged passes reuse within the original refresh window");
  test_time = 1595;
  fixture.pass();
  require(native_count == 25 && test_time == 1625,
          "250 ms expiry schedules only two 15 ms native calls in a pass");
  require(castle_preview_metrics().periodic - initial.periodic == 2 &&
              castle_preview_metrics().deferred - initial.deferred == 21,
          "expiry records two periodic rebuilds and 21 deferred snapshots");
  for (std::size_t pass = 1; pass < 12; ++pass) {
    const auto before = native_count.load();
    fixture.pass();
    require(native_count - before == (pass == 11 ? 1 : 2),
            "each periodic sweep pass obeys the two-road work limit");
  }
  for (const auto count : visits)
    require(count == 2, "every sibling is refreshed exactly once per sweep");
  require(native_count == 46 && test_time == 1940 &&
              castle_preview_metrics().periodic - initial.periodic == 23 &&
              castle_preview_metrics().deferred - initial.deferred == 121 &&
              castle_preview_metrics().max_periodic_per_pass == 2,
          "all 23 siblings finish one fair sweep in 12 bounded passes");
  fixture.pass();
  require(native_count == 46, "the refreshed slow cohort is reusable again");
  test_time = 2189;
  fixture.pass();
  require(native_count == 46,
          "249 ms after the latest full pass still reuses all siblings");
  test_time = 2190;
  fixture.pass();
  require(native_count == 48,
          "the next periodic sweep also starts with only two native calls");
  for (std::size_t pass = 1; pass < 12; ++pass)
    fixture.pass();
  require(native_count == 69,
          "the next sweep refreshes all roads without repeating early roads");
  fixture.pass();
  std::swap(fixture.parent_roads[0], fixture.parent_roads[1]);
  fixture.pass();
  require(native_count == 92,
          "same-buffer sibling reordering requires a complete native pass");
  fixture.pass();
  require(native_count == 92, "the reordered complete pass can be reused");
  castle_preview_reset();
  fixture.pass();
  require(native_count == 115,
          "load-generation reset requires every sibling to rebuild");
  fixture.pass();
  test_time = 1000;
  fixture.pass();
  require(native_count == 138,
          "a backward clock restarts the entire native cohort");
  fixture.pass();
  buttons_down = true;
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 161,
          "final placement refreshes every reused slow-pass sibling");
  buttons_down = false;
  fixture.pass();
  require(native_count == 184,
          "final placement invalidates the next complete native cohort");
}

static void test_periodic_mandatory_work() {
  BatchFixture<8> fixture;
  fixture.pass();
  fixture.pass();
  const auto initial = castle_preview_metrics();
  test_time += 250;
  fixture.call_road(fixture.siblings[0].data());
  fixture.call_road(fixture.siblings[1].data());
  put(fixture.siblings[2], 0x420, std::uintptr_t{777});
  fixture.call_road(fixture.siblings[2].data());
  fixture.active_box(20, -5, 80, 5);
  fixture.call_road(fixture.siblings[3].data());
  fixture.active_box(1000, 1000, 1100, 1010);
  for (std::size_t index = 4; index < fixture.siblings.size(); ++index)
    fixture.call_road(fixture.siblings[index].data());
  auto current = castle_preview_metrics();
  require(native_count == 12 && current.periodic - initial.periodic == 2 &&
              current.changed - initial.changed == 1 &&
              current.overlap - initial.overlap == 1 &&
              current.deferred - initial.deferred == 4,
          "changed and overlapping pending roads rebuild after the quota");
  fixture.pass();
  current = castle_preview_metrics();
  require(native_count == 15 && current.periodic - initial.periodic == 4 &&
              current.overlap - initial.overlap == 2,
          "leaving an overlap is mandatory and leaves two periodic slots");
  fixture.pass();
  require(native_count == 17 &&
              castle_preview_metrics().periodic - initial.periodic == 6,
          "mandatory rebuilds satisfy their own pending periodic work");
  fixture.pass();
  require(native_count == 17, "the completed mixed sweep becomes reusable");
}

static void test_interrupted_periodic_sweep() {
  BatchFixture<6> fixture;
  fixture.pass();
  fixture.pass();
  test_time += 250;
  fixture.call_road(fixture.siblings[0].data());
  fixture.call_road(fixture.siblings[1].data());
  for (int pass = 0; pass < 2; ++pass) {
    test_time += 1000;
    fixture.call_road(fixture.siblings[0].data());
    fixture.call_road(fixture.siblings[1].data());
  }
  require(native_count == 8,
          "partial traversal cannot requeue already serviced early roads");
  fixture.pass();
  require(native_count == 10,
          "resumed traversal preserves pending work after an interrupted pass");
  fixture.pass();
  require(native_count == 12,
          "late pending siblings finish before a new sweep");
  fixture.pass();
  test_time += 249;
  fixture.pass();
  require(native_count == 12,
          "quiet time begins only after all pending siblings are serviced");
  ++test_time;
  fixture.pass();
  require(native_count == 14,
          "a completed sweep starts a fresh bounded sweep after 250 ms");
  castle_preview_reset();
  fixture.pass();
  require(native_count == 20,
          "reset during a sweep requires a full native warmup");
  fixture.pass();
  require(native_count == 20, "the reset warmup becomes reusable");
  test_time += 250;
  native_action = [] { throw std::runtime_error("interrupted periodic call"); };
  bool threw{};
  try {
    fixture.pass();
  } catch (const std::runtime_error &) {
    threw = true;
  }
  native_action = {};
  fixture.pass();
  require(threw && native_count == 27,
          "a periodic native exception discards the sweep and forces warmup");
  fixture.pass();
  require(native_count == 27, "exception recovery completes a reusable pass");
  test_time += 250;
  native_action = [&fixture] {
    native_action = {};
    fixture.call_road(fixture.siblings[0].data());
  };
  fixture.call_road(fixture.siblings[0].data());
  fixture.pass();
  require(native_count == 35,
          "nested periodic rebuild invalidates all outer pending state");
  fixture.pass();
  require(native_count == 35, "nested rebuild recovery completes warmup");
}

static void test_pending_final_flush() {
  BatchFixture<6> fixture;
  fixture.pass();
  test_time += 250;
  fixture.call_road(fixture.siblings[0].data());
  const auto before = castle_preview_metrics();
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 12 &&
              castle_preview_metrics().flushed - before.flushed == 5,
          "final flush includes pending roads never visited by a reused pass");
  fixture.pass();
  fixture.pass();
  test_time += 250;
  fixture.pass();
  fixture.call_road(fixture.siblings[0].data());
  fixture.call_road(fixture.siblings[1].data());
  fixture.call_road(fixture.siblings[2].data());
  const auto mixed = castle_preview_metrics();
  buttons_down = true;
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 26 &&
              castle_preview_metrics().flushed - mixed.flushed == 5,
          "final flush includes pending and previously serviced reused roads");
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 26, "pending flush removes all retained ownership");
}

static void test_batch_iterator_validation() {
  Fixture fixture;
  const auto begin = reinterpret_cast<std::uintptr_t>(fixture.roads.data());
  const auto end = begin + sizeof(fixture.roads);
  const auto rejected = [&](std::uintptr_t iterator, std::uintptr_t limit) {
    fixture.prime();
    const auto before = native_count.load();
    castle_preview_regenerate(fixture.road.data(), 1, fixture.pawn.data(),
                              iterator, limit);
    require(native_count == before + 1,
            "invalid sibling-loop bounds always call native");
    fixture.call();
    require(native_count == before + 2,
            "invalid sibling-loop bounds invalidate the previous cohort");
    native_count = 0;
    castle_preview_reset();
  };
  rejected(begin + 1, end);
  rejected(begin, end + sizeof(std::uintptr_t));
  rejected(end, end);
  rejected(begin - sizeof(std::uintptr_t), end);
  rejected(begin + sizeof(std::uintptr_t), end);
}

static void test_mixed_manor_sources() {
  Fixture fixture;
  // Actual Manor source lists include standalone buildings, not just roads.
  // These short fixtures deliberately make +0x2b3 unreadable.
  std::array<std::array<std::byte, 0x28>, 21> buildings{};
  std::array<std::uintptr_t, 22> sources{};
  sources[0] = reinterpret_cast<std::uintptr_t>(fixture.road.data());
  for (std::size_t i{}; i < buildings.size(); ++i) {
    fixture.object(buildings[i]);
    sources[i + 1] = reinterpret_cast<std::uintptr_t>(buildings[i].data());
  }
  fixture.readable(sources);
  put(fixture.parent, 0xef0,
      NativeArray<std::uintptr_t>{sources.data(), 22, 22});
  fixture.prime();
  require(native_count == 1, "standalone Manor actors do not reject an "
                             "otherwise reusable road cohort");
  castle_preview_flush(fixture.pawn.data());
  require(native_count == 2,
          "mixed Manor sources preserve validated pre-click refresh");
}

static void test_batch_topology_and_controller() {
  Fixture fixture;
  std::array<std::array<std::byte, 0x500>, 3> roads;
  std::array<std::uintptr_t, 3> parent_roads{};
  for (std::size_t index = 0; index < roads.size(); ++index) {
    roads[index] = fixture.road;
    fixture.object(roads[index]);
    parent_roads[index] = reinterpret_cast<std::uintptr_t>(roads[index].data());
  }
  fixture.readable(parent_roads);
  put(fixture.parent, 0xef0,
      NativeArray<std::uintptr_t>{parent_roads.data(), 2, 3});
  void *pawn = fixture.pawn.data();
  std::size_t count = 2;
  const auto pass = [&] {
    for (std::size_t index = 0; index < count; ++index)
      castle_preview_regenerate(
          reinterpret_cast<void *>(parent_roads[index]), 1, pawn,
          reinterpret_cast<std::uintptr_t>(parent_roads.data() + index),
          reinterpret_cast<std::uintptr_t>(parent_roads.data() + count));
  };
  pass();
  pass();
  require(native_count == 2, "initial sibling topology can be reused");
  count = 3;
  put(fixture.parent, 0xef8, std::int32_t{3});
  pass();
  require(native_count == 5,
          "adding a wall refreshes every sibling in the expanded pass");
  pass();
  require(native_count == 5, "the expanded sibling pass can be reused");
  auto replacement_pawn = fixture.pawn;
  fixture.object(replacement_pawn);
  pawn = replacement_pawn.data();
  put(fixture.master, 0x5b0, pawn);
  pass();
  require(native_count == 8,
          "changing the acting controller requires a complete native pass");
  pass();
  require(native_count == 8, "the replacement controller has its own cohort");
  native_action = [] { throw std::runtime_error("aborted native pass"); };
  test_time += 250;
  bool threw{};
  try {
    pass();
  } catch (const std::runtime_error &) {
    threw = true;
  }
  native_action = {};
  pass();
  require(threw && native_count == 12,
          "an aborted native pass cannot publish a reusable partial cohort");
  pass();
  require(native_count == 12, "a complete pass recovers after an exception");
  test_time += 250;
  put(roads.front(), 0x2b3, std::uint8_t{0});
  for (std::size_t index = 1; index < count; ++index)
    castle_preview_regenerate(
        reinterpret_cast<void *>(parent_roads[index]), 1, pawn,
        reinterpret_cast<std::uintptr_t>(parent_roads.data() + index),
        reinterpret_cast<std::uintptr_t>(parent_roads.data() + count));
  put(roads.front(), 0x2b3, std::uint8_t{1});
  pass();
  require(native_count == 17,
          "restoring a skipped sibling refreshes the changed native cohort");
  pass();
  require(native_count == 17,
          "the restored sibling can reuse only after its own native rebuild");
  // Topology is unchanged while the callable subset moves to a later cursor.
  put(roads[1], 0x2b3, std::uint8_t{0});
  put(roads[2], 0x2b3, std::uint8_t{0});
  const auto call_at = [&](std::size_t index) {
    castle_preview_regenerate(
        reinterpret_cast<void *>(parent_roads[index]), 1, pawn,
        reinterpret_cast<std::uintptr_t>(parent_roads.data() + index),
        reinterpret_cast<std::uintptr_t>(parent_roads.data() + count));
  };
  call_at(0);
  call_at(0);
  require(native_count == 18, "one callable sibling forms a reusable cohort");
  test_time += 1000;
  put(roads[0], 0x2b3, std::uint8_t{0});
  put(roads[1], 0x2b3, std::uint8_t{1});
  call_at(1);
  require(native_count == 19,
          "a forward-moving filtered cursor still starts a native cohort");
  call_at(1);
  require(native_count == 19, "the changed callable subset can then be reused");
}

static void test_common_capture_cost_and_mutations() {
  BatchFixture<64> fixture;
  std::array<std::uintptr_t, 4096> world_roads{};
  world_roads.fill(reinterpret_cast<std::uintptr_t>(fixture.active.data()));
  fixture.readable(world_roads);
  put(fixture.master, 0x590,
      NativeArray<std::uintptr_t>{world_roads.data(), 4096, 4096});
  counted_addresses = {
      reinterpret_cast<std::uintptr_t>(fixture.parent_roads.data()),
      reinterpret_cast<std::uintptr_t>(world_roads.data()),
      reinterpret_cast<std::uintptr_t>(fixture.materials.data())};
  configure_castle_preview_functions(original_preview, fake_clock, fake_buttons,
                                     fake_weak, counted_read);
  fixture.pass();
  counted_reads = {};
  const auto warmup = native_count.load();
  fixture.pass();
  // The iterator check reads the first parent slot once in addition to the
  // one bulk parent snapshot. Material selectors use two native arrays.
  require(native_count == warmup &&
              counted_reads == std::array<std::size_t, 3>{2, 1, 2},
          "64 unchanged siblings capture parent and 4096 world-road entries "
          "once per pass");
  counted_reads = {};
  fixture.pass();
  require(
      counted_reads == std::array<std::size_t, 3>{2, 1, 2},
      "each new parent pass validates complete same-buffer common contents");
  world_roads.back() = reinterpret_cast<std::uintptr_t>(fixture.road.data());
  fixture.pass();
  require(native_count == warmup + 64,
          "same-buffer world-road mutation invalidates all dependent wall "
          "snapshots");
  fixture.pass();
  const auto before_native_mutation = native_count.load();
  put(fixture.siblings.front(), 0x420, std::uintptr_t{999});
  native_action = [&] {
    native_action = {};
    fixture.materials[0] = 321;
  };
  fixture.pass();
  require(native_count == before_native_mutation + 64,
          "a native callback changing common material data invalidates later "
          "siblings in the same pass");
  counted_reads = {};
  fixture.pass();
  require(native_count == before_native_mutation + 64 &&
              counted_reads == std::array<std::size_t, 3>{2, 1, 2},
          "native post-state is reusable with one common capture in the "
          "following pass");
  const auto before_backtracking = native_count.load();
  fixture.call_road(fixture.siblings[10].data());
  world_roads.front() =
      reinterpret_cast<std::uintptr_t>(fixture.siblings[20].data());
  fixture.call_road(fixture.siblings[5].data());
  require(
      native_count == before_backtracking + 1,
      "backtracking recaptures common array contents before considering reuse");
  counted_addresses = {};
}

int main() {
  try {
    test_reuse_and_geometry();
    test_lifetime_and_buttons();
    test_castle_wall_preview();
    test_exact_dependencies();
    test_scope_and_failures();
    test_native_post_state_and_exceptions();
    test_flush_and_threads();
    test_cache_saturation();
    test_slow_native_batches();
    test_periodic_mandatory_work();
    test_interrupted_periodic_sweep();
    test_pending_final_flush();
    test_batch_iterator_validation();
    test_mixed_manor_sources();
    test_batch_topology_and_controller();
    test_common_capture_cost_and_mutations();
    std::printf("castle_preview_tests: %d checks passed\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "castle_preview_tests: %s (check %d)\n", error.what(),
                 checks);
    return 1;
  }
}

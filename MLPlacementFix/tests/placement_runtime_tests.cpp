#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "PlacementRuntime.hpp"

struct PlacementTestArray {
  std::uintptr_t data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct PlacementVisibilityCall {
  std::uintptr_t component{};
  bool visible{};
  std::uint8_t propagate{};
};

static std::map<std::uintptr_t, std::vector<std::uint8_t>>
    *placement_test_memory{};
static std::array<PlacementVisibilityCall, 256> visibility_calls;
static std::size_t visibility_count{};
static std::size_t collision_count{};
static std::array<std::uintptr_t, 256> retired_helpers;
static std::size_t retirement_count{}, first_visibility_retirements{};
static int retirement_failure{};
static std::uintptr_t denied_address{}, recycled_object{};
static std::size_t recycled_reads{};

static auto placement_test_read(std::uintptr_t address, void *output,
                                std::size_t size) noexcept -> bool {
  if (placement_test_memory == nullptr || address == denied_address) {
    return false;
  }
  const auto end = placement_test_memory->upper_bound(address);
  if (end == placement_test_memory->begin()) {
    return false;
  }
  const auto &span = *std::prev(end);
  if (address - span.first > span.second.size() ||
      size > span.second.size() - (address - span.first)) {
    return false;
  }
  std::memcpy(output, span.second.data() + address - span.first, size);
  if (address == recycled_object && size == 24 && ++recycled_reads > 1) {
    const std::int32_t changed_index{999};
    std::memcpy(static_cast<std::uint8_t *>(output) + 12, &changed_index,
                sizeof(changed_index));
  }
  return true;
}

static auto placement_test_visibility(void *component, bool visible,
                                      std::uint8_t propagate) noexcept -> void {
  const auto address = reinterpret_cast<std::uintptr_t>(component);
  if (visibility_count == 0)
    first_visibility_retirements = retirement_count;
  if (visibility_count < visibility_calls.size()) {
    visibility_calls[visibility_count] = {address, visible, propagate};
  }
  ++visibility_count;
  auto &bytes = placement_test_memory->at(address);
  bytes[0x188] = static_cast<std::uint8_t>((bytes[0x188] & ~0x20U) |
                                           (visible ? 0x20U : 0U));
}

static auto placement_test_collision(void *actor, bool enabled) noexcept
    -> void {
  const auto address = reinterpret_cast<std::uintptr_t>(actor);
  ++collision_count;
  placement_test_memory->at(address)[0x2f2] = enabled ? 1 : 2;
}

static auto placement_test_retire_helper(void *helper) noexcept -> void {
  const auto address = reinterpret_cast<std::uintptr_t>(helper);
  retired_helpers[retirement_count++] = address;
  auto &bytes = placement_test_memory->at(address);
  std::uintptr_t module{};
  std::memcpy(&module, bytes.data() + 0x2a8, sizeof(module));
  auto &module_bytes = placement_test_memory->at(module);
  if (retirement_failure == 1)
    return;
  std::memset(module_bytes.data() + 0x308, 0, sizeof(std::uintptr_t));
  if (retirement_failure != 2)
    module_bytes[0x311] = 0;
  if (retirement_failure == 3)
    module_bytes[12] ^= 0x10;
  const std::uint32_t flags = 0x40000000;
  std::memcpy(bytes.data() + 8, &flags, sizeof(flags));
}

struct PlacementFixture {
  std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
  std::map<std::string, std::uint64_t> names;
  std::uintptr_t next{0x200000000}, object_class{}, name_block{}, manager{},
      owner{}, outhouse{}, other{}, outhouse_component{}, other_component{},
      fence{}, unmarked_fence{}, second_module_fence{};
  std::size_t name_cursor{2};

  auto allocate(std::size_t size = 0x1000) -> std::uintptr_t {
    const auto result = next;
    next += 0x10000;
    memory.emplace(result, std::vector<std::uint8_t>(size));
    return result;
  }
  template <class T> auto put(std::uintptr_t address, const T &value) -> void {
    auto found = std::prev(memory.upper_bound(address));
    std::memcpy(found->second.data() + address - found->first, &value,
                sizeof(value));
  }
  auto name(const std::string &value, bool wide = false) -> std::uint64_t {
    const auto key = (wide ? "wide:" : "ascii:") + value;
    if (const auto found = names.find(key); found != names.end()) {
      return found->second;
    }
    const auto result = name_cursor / 2;
    const auto width = wide ? std::size_t{2} : std::size_t{1};
    put(name_block + name_cursor,
        static_cast<std::uint16_t>((value.size() << 6) | (wide ? 1U : 0U)));
    for (std::size_t i{}; i < value.size(); ++i) {
      put(name_block + name_cursor + 2 + i * width, value[i]);
    }
    name_cursor = (name_cursor + value.size() * width + 3) & ~std::size_t{1};
    names[key] = result;
    return result;
  }
  auto object() -> std::uintptr_t {
    const auto address = allocate();
    put(address, placement_image + 0x1000);
    put(address + 12, std::int32_t{1});
    put(address + 16, object_class);
    return address;
  }
  auto array(const std::vector<std::uintptr_t> &values) -> PlacementTestArray {
    const auto data = allocate(values.size() * sizeof(std::uintptr_t));
    for (std::size_t i{}; i < values.size(); ++i) {
      put(data + i * sizeof(std::uintptr_t), values[i]);
    }
    return {data, static_cast<std::int32_t>(values.size()),
            static_cast<std::int32_t>(values.size())};
  }
  PlacementFixture() {
    placement_image = 0x140000000;
    object_class = allocate();
    put(object_class, placement_image + 0x1000);
    name_block = allocate(0x20000);
    next += 0x20000;
    memory.emplace(placement_image + 0x091d49c0,
                   std::vector<std::uint8_t>(0x20));
    put(placement_image + 0x091d49d0, name_block);
    manager = object();
    put(manager + 0x318, std::uint8_t{1});
    owner = object();
    outhouse = object();
    other = object();
    outhouse_component = object();
    other_component = object();
    fence = object();
    unmarked_fence = object();
    second_module_fence = object();
    put(manager + 0x508, array({owner, owner}));
    put(manager + 0x518, array({owner}));
    put(owner + 0x2e8, manager);
    put(owner + 0x3b8, std::int32_t{987});
    put(owner + 0x3c1, std::uint8_t{1});
    put(owner + 0x2f8, array({outhouse, other}));
    put(outhouse + 0x2c0, std::int32_t{777});
    put(other + 0x2c0, std::int32_t{145});
    put(outhouse + 0x2d0, outhouse_component);
    put(other + 0x2d0, other_component);
    put(outhouse + 0x1e0, array({name("outhouseForRemoval")}));
    put(outhouse + 0x2f8, array({fence, unmarked_fence, fence}));
    put(other + 0x2f8, array({second_module_fence}));
    put(fence + 0x60, array({name("overlapped")}));
    put(second_module_fence + 0x60, array({name("overlapped")}));
    put(outhouse_component + 0xf0, PlacementBounds{{0, 0, 0}, {1, 1, 1}});
    put(other_component + 0xf0, PlacementBounds{{2, 0, 0}, {1, 1, 1}});
    put(outhouse_component + 0x188, std::uint8_t{0xa5});
    put(fence + 0x188, std::uint8_t{0xa5});
    put(unmarked_fence + 0x188, std::uint8_t{0xa5});
    put(second_module_fence + 0x188, std::uint8_t{0xa5});
    // Hidden other modules, including model 145, still participate in overlaps.
    put(other_component + 0x188, std::uint8_t{0x85});
    placement_test_memory = &memory;
    visibility_count = 0;
    collision_count = 0;
    retirement_count = first_visibility_retirements = 0;
    retirement_failure = 0;
    denied_address = 0;
    recycled_object = 0;
    recycled_reads = 0;
    configure_placement_api({placement_test_read, placement_test_visibility,
                             placement_test_collision,
                             placement_test_retire_helper});
  }
  ~PlacementFixture() {
    configure_placement_api({});
    placement_test_memory = nullptr;
    placement_image = 0;
  }
  auto run(bool world = false) -> PlacementStats {
    return world
               ? placement_reconcile_world(reinterpret_cast<void *>(manager))
               : placement_reconcile_building(reinterpret_cast<void *>(owner));
  }
  auto called(std::uintptr_t component, bool visible) const -> bool {
    for (std::size_t i{}; i < visibility_count && i < visibility_calls.size();
         ++i) {
      if (visibility_calls[i].component == component &&
          visibility_calls[i].visible == visible &&
          visibility_calls[i].propagate == 1) {
        return true;
      }
    }
    return false;
  }
};

auto run_placement_runtime_tests() -> int {
  int failures{}, checks{};
  const auto expect = [&](bool value, const char *message) {
    ++checks;
    if (!value) {
      ++failures;
      std::fprintf(stderr, "placement runtime: %s\n", message);
    }
  };
  const PlacementBounds box{{0, 0, 0}, {1, 1, 1}};
  expect(placement_bounds_intersect(box, box),
         "a valid AABB intersects itself");
  expect(placement_bounds_intersect(box, {{2, 2, 2}, {1, 1, 1}}),
         "corner contact is inclusive on all axes");
  expect(placement_bounds_intersect(box, {{0, 0, 0}, {0, 0, 0}}),
         "native zero-extent point inside a box is included");
  for (std::size_t axis{}; axis < 3; ++axis) {
    auto other = box;
    other.center[axis] = 2;
    expect(placement_bounds_intersect(box, other) &&
               placement_bounds_intersect(other, box),
           "exact face contact is symmetric and inclusive");
    other.center[axis] = std::nextafter(2.0, 3.0);
    expect(!placement_bounds_intersect(box, other),
           "next representable gap on any axis does not intersect");
    other = box;
    other.extent[axis] = -1;
    expect(!placement_bounds_intersect(box, other),
           "negative extent is rejected");
    other = box;
    other.center[axis] = std::numeric_limits<double>::quiet_NaN();
    expect(!placement_bounds_intersect(box, other),
           "nonfinite center is rejected");
    other = box;
    other.extent[axis] = std::numeric_limits<double>::infinity();
    expect(!placement_bounds_intersect(box, other),
           "nonfinite extent is rejected");
  }
  expect(!placement_bounds_intersect(
             box, {{std::numeric_limits<double>::max(), 0, 0},
                   {std::numeric_limits<double>::max(), 1, 1}}),
         "overflowing bound endpoints are rejected");
  for (const bool world : {false, true}) {
    PlacementFixture fixture;
    auto expected = fixture.memory;
    expected.at(fixture.outhouse_component)[0x188] = 0x85;
    expected.at(fixture.fence)[0x188] = 0x85;
    const auto stats = fixture.run(world);
    expect(
        stats.buildings == 1 && stats.candidates == 1 &&
            stats.hidden_priority == 1 && stats.hidden_marked == 1 &&
            stats.shown_priority == 0 && stats.shown_marked == 0 &&
            stats.skipped == 0,
        "world and single-building paths apply identical completed policies");
    expect(visibility_count == 2 &&
               fixture.called(fixture.outhouse_component, false) &&
               fixture.called(fixture.fence, false),
           "only overlapping priority mesh and marked first-module fence hide");
    expect(fixture.memory == expected,
           "only native visibility bits change, preserving ordinary modules");
    const auto again = fixture.run(world);
    expect(visibility_count == 2 && again.candidates == 1 &&
               again.hidden_priority == 0 && again.hidden_marked == 0 &&
               again.shown_priority == 0 && again.shown_marked == 0,
           "repeated reconciliation is stable without redundant native calls");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.other_component + 0xf0,
                PlacementBounds{{3, 0, 0}, {1, 1, 1}});
    fixture.put(fixture.outhouse_component + 0x188, std::uint8_t{0x85});
    fixture.put(fixture.unmarked_fence + 0x188, std::uint8_t{0x85});
    const auto stats = fixture.run();
    expect(stats.shown_priority == 1 && stats.hidden_marked == 1 &&
               stats.shown_marked == 1 && visibility_count == 3 &&
               fixture.called(fixture.outhouse_component, true) &&
               fixture.called(fixture.unmarked_fence, true),
           "clear priority meshes and unmarked first-module children show");
    expect(!fixture.called(fixture.other_component, true) &&
               !fixture.called(fixture.second_module_fence, false),
           "excluded model 145 and later-module marked children stay put");
  }
  {
    PlacementFixture fixture;
    fixture.run();
    visibility_count = 0;
    fixture.put(fixture.fence + 0x60, PlacementTestArray{});
    const auto stats = fixture.run();
    expect(stats.shown_marked == 1 && visibility_count == 1 &&
               fixture.called(fixture.fence, true),
           "clearing native overlapped marker restores first-module child");
    visibility_count = 0;
    fixture.put(fixture.fence + 0x60,
                fixture.array({fixture.name("overlapped")}));
    expect(fixture.run().hidden_marked == 1 && visibility_count == 1 &&
               fixture.called(fixture.fence, false),
           "adding native overlapped marker hides first-module child again");
  }
  for (const auto extension_type : {0, 1, 37, 1000}) {
    PlacementFixture fixture;
    fixture.put(fixture.outhouse + 0x1e0,
                fixture.array({fixture.name("outhouse")}));
    const auto extension = fixture.allocate();
    fixture.put(extension, std::int32_t{extension_type});
    fixture.put(fixture.owner + 0xb88, PlacementTestArray{extension, 1, 1});
    const auto stats = fixture.run();
    expect(stats.hidden_priority == (extension_type == 0 ? 0U : 1U),
           "any nonzero native extension type enables outhouse priority rule");
  }
  for (int mode{}; mode < 10; ++mode) {
    PlacementFixture fixture;
    switch (mode) {
    case 0:
      fixture.put(fixture.outhouse + 0x1e0, PlacementTestArray{});
      break;
    case 1:
      fixture.put(fixture.outhouse + 0x2c0, std::int32_t{145});
      break;
    case 2:
      fixture.put(fixture.other_component + 0xf0,
                  PlacementBounds{{3, 0, 0}, {1, 1, 1}});
      break;
    case 3:
      fixture.put(fixture.other_component + 0xf0,
                  PlacementBounds{{0, 0, 3}, {1, 1, 1}});
      break;
    case 4:
      fixture.put(fixture.owner + 0x2f8, fixture.array({fixture.outhouse}));
      break;
    case 5:
      fixture.put(fixture.outhouse + 0x1e0,
                  fixture.array({fixture.name("outhouseForRemoval") |
                                 (std::uint64_t{1} << 32)}));
      break;
    case 6:
      fixture.put(fixture.outhouse + 0x1e0,
                  fixture.array({fixture.name("outhouseForRemovalExtra")}));
      break;
    case 7:
      fixture.put(fixture.outhouse + 0x1e0,
                  fixture.array({fixture.name("outhouse")}));
      break;
    case 8:
      fixture.put(fixture.outhouse + 0x1e0,
                  fixture.array({fixture.name(std::string(1023, 'x'))}));
      break;
    case 9:
      fixture.put(fixture.outhouse + 0x1e0,
                  fixture.array({fixture.name("OuthouseForRemoval")}));
      break;
    }
    const auto stats = fixture.run();
    expect(stats.hidden_priority == 0 && stats.hidden_marked == 1 &&
               stats.skipped == 0 && visibility_count == 1 &&
               fixture.called(fixture.fence, false),
           "intentional overlaps, native exclusions and nonexact tags survive");
  }
  for (const bool initially_visible : {false, true}) {
    PlacementFixture fixture;
    fixture.put(fixture.outhouse + 0x1e0, PlacementTestArray{});
    fixture.put(fixture.outhouse_component + 0x188,
                static_cast<std::uint8_t>(initially_visible ? 0xa5 : 0x85));
    const auto stats = fixture.run();
    expect(!fixture.called(fixture.outhouse_component, false) &&
               fixture.called(fixture.outhouse_component, true) ==
                   !initially_visible &&
               stats.shown_general == (initially_visible ? 0U : 1U),
           "completed ordinary final meshes follow native FinishConstruction");
  }
  {
    PlacementFixture fixture;
    std::vector<std::uintptr_t> actors;
    std::vector<std::uintptr_t> components;
    // Captured new brewery: 89 zero-work decorative actors, including town
    // wall pillars (271), BrewerShed_WoodShed (522) and BrewerOven_Full (523).
    for (std::size_t i{}; i < 89; ++i) {
      const auto actor = fixture.object();
      const auto component = fixture.object();
      const std::int32_t model = i < 4 ? 271 : i == 4 ? 522 : i < 7 ? 523 : 0;
      fixture.put(actor + 0x2c0, model);
      fixture.put(actor + 0x2d0, component);
      fixture.put(actor + 0x2e8, std::uint8_t{1});
      fixture.put(actor + 0x2f2, std::uint8_t{2});
      if (model == 523) {
        fixture.put(actor + 0x1e0,
                    fixture.array({fixture.name("upgrade_37_475")}));
      }
      fixture.put(component + 0xf0, box);
      fixture.put(component + 0x188, std::uint8_t{0x43});
      actors.push_back(actor);
      components.push_back(component);
    }
    fixture.put(fixture.owner + 0x2f8, fixture.array(actors));
    const auto stats = fixture.run();
    expect(stats.skipped == 0 && stats.candidates == 0 &&
               stats.shown_general == 89 && stats.collisions_restored == 89 &&
               visibility_count == 89 && collision_count == 89,
           "completed brewery restores all zero-work final meshes and disabled "
           "collisions without first-module children");
    bool all_restored = true;
    for (std::size_t i{}; i < actors.size(); ++i) {
      all_restored = all_restored &&
                     fixture.memory.at(components[i])[0x188] == 0x63 &&
                     fixture.memory.at(actors[i])[0x2f2] == 1;
    }
    expect(all_restored, "restoration preserves unrelated component bits and "
                         "enables native module collisions");
    const auto again = fixture.run();
    expect(again.shown_general == 0 && again.collisions_restored == 0 &&
               visibility_count == 89 && collision_count == 89,
           "ordinary completion repair is idempotent");
  }
  for (const bool world : {false, true}) {
    PlacementFixture fixture;
    fixture.put(fixture.other + 0x2c0, std::int32_t{522});
    fixture.put(fixture.other + 0x2f2, std::uint8_t{2});
    if (!world)
      fixture.put(fixture.manager + 0x318, std::uint8_t{0});
    const auto stats = fixture.run(world);
    expect(stats.skipped == 0 && stats.shown_general == 0 &&
               stats.collisions_restored == 0 && stats.hidden_priority == 1 &&
               !fixture.called(fixture.other_component, true),
           "load preserves ordinary visibility and collision while applying "
           "the same native overlap priority");
  }
  for (const std::uint8_t mode : {0, 1, 2}) {
    PlacementFixture fixture;
    fixture.put(fixture.other + 0x2c0, std::int32_t{522});
    fixture.put(fixture.other + 0x2f2, mode);
    const auto stats = fixture.run();
    expect(stats.shown_general == 1 && stats.hidden_priority == 1 &&
               stats.collisions_restored == (mode == 2 ? 1U : 0U) &&
               collision_count == (mode == 2 ? 1U : 0U),
           "native ordinary visibility restores alongside priority hiding "
           "while collision modes zero and one stay intact");
  }
  for (const auto &work : std::array<std::array<float, 2>, 8>{
           {{0, 5},
            {4, 5},
            {-1, 0},
            {0, -1},
            {std::numeric_limits<float>::quiet_NaN(), 0},
            {0, std::numeric_limits<float>::quiet_NaN()},
            {std::numeric_limits<float>::infinity(), 0},
            {0, std::numeric_limits<float>::infinity()}}}) {
    PlacementFixture fixture;
    fixture.put(fixture.other + 0x2c0, std::int32_t{522});
    fixture.put(fixture.other + 0x2f2, std::uint8_t{2});
    fixture.put(fixture.other + 0x314, work);
    const auto original = fixture.memory;
    expect(fixture.run().skipped == 1 && visibility_count == 0 &&
               collision_count == 0 && fixture.memory == original,
           "incomplete or invalid module work defers the entire owner before "
           "visibility and collision writes");
    fixture.put(fixture.other + 0x314, std::array<float, 2>{5, 5});
    const auto ready = fixture.run();
    expect(ready.skipped == 0 && ready.shown_general == 1 &&
               ready.collisions_restored == 1 && ready.hidden_priority == 1,
           "completion of the last module permits the deferred repair");
  }
  for (const auto *tag :
       {"construction_50", "preCONSTRUCTION_75", "Construction"}) {
    PlacementFixture fixture;
    fixture.put(fixture.other + 0x2c0, std::int32_t{522});
    fixture.put(fixture.other + 0x2f2, std::uint8_t{2});
    fixture.put(fixture.other + 0x1e0, fixture.array({fixture.name(tag)}));
    fixture.put(fixture.other + 0x314, std::array<float, 2>{0, 5});
    fixture.put(fixture.owner + 0x2f8,
                fixture.array({fixture.other, fixture.outhouse}));
    fixture.put(fixture.outhouse_component + 0x188, std::uint8_t{0x85});
    const auto original_actor = fixture.memory.at(fixture.other);
    const auto original_component = fixture.memory.at(fixture.other_component);
    const auto stats = fixture.run();
    expect(stats.skipped == 0 && stats.candidates == 1 &&
               stats.shown_priority == 1 && stats.hidden_marked == 1 &&
               stats.shown_general == 0 && collision_count == 0 &&
               fixture.memory.at(fixture.other) == original_actor &&
               fixture.memory.at(fixture.other_component) ==
                   original_component &&
               !fixture.called(fixture.second_module_fence, false),
           "construction substring matches case-insensitively and excludes "
           "temporary actors from writes, blockers and first-module selection");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.outhouse + 0x1e0, PlacementTestArray{});
    fixture.put(fixture.outhouse + 0x2f2, std::uint8_t{2});
    fixture.put(fixture.outhouse + 0x2f8,
                fixture.array({fixture.outhouse_component}));
    fixture.put(fixture.outhouse_component + 0x60,
                fixture.array({fixture.name("overlapped")}));
    const auto stats = fixture.run();
    expect(stats.hidden_marked == 1 && stats.shown_general == 0 &&
               stats.collisions_restored == 0 && collision_count == 0 &&
               fixture.called(fixture.outhouse_component, false),
           "explicit overlapped child wins over ordinary final showing and "
           "collision restoration");
  }
  for (int failure{}; failure < 4; ++failure) {
    PlacementFixture fixture;
    fixture.put(fixture.other + 0x2c0, std::int32_t{522});
    fixture.put(fixture.other + 0x2f2, std::uint8_t{2});
    if (failure == 0) {
      denied_address = fixture.other + 0x2f2;
    } else if (failure == 1) {
      denied_address = fixture.unmarked_fence + 0x188;
    } else if (failure == 2) {
      recycled_object = fixture.other_component;
    } else {
      configure_placement_api({placement_test_read, placement_test_visibility});
    }
    const auto original = fixture.memory;
    expect(fixture.run().skipped == 1 && visibility_count == 0 &&
               collision_count == 0 && fixture.memory == original,
           "failed collision validation, late child, identity or callback "
           "cancels every pending change");
  }
  for (const bool wide : {false, true}) {
    PlacementFixture fixture;
    fixture.put(fixture.outhouse + 0x1e0,
                fixture.array({fixture.name("outhouseForRemoval", wide)}));
    fixture.put(fixture.fence + 0x60,
                fixture.array({fixture.name("overlapped", wide)}));
    const auto stats = fixture.run();
    expect(stats.hidden_priority == 1 && stats.hidden_marked == 1 &&
               stats.skipped == 0,
           "both narrow and wide ASCII native tags are decoded completely");
  }
  for (int mode{}; mode < 22; ++mode) {
    PlacementFixture fixture;
    switch (mode) {
    case 0:
      fixture.put(fixture.owner + 0x3c1, std::uint8_t{0});
      break;
    case 1:
      fixture.put(fixture.owner + 0x3a2, std::uint8_t{1});
      break;
    case 2:
      fixture.put(fixture.owner + 0x2e8, std::uintptr_t{});
      break;
    case 3:
      fixture.put(fixture.outhouse + 0x1e0, PlacementTestArray{1, 1, 1});
      break;
    case 4:
      fixture.put(fixture.outhouse + 0x2d0, std::uintptr_t{});
      break;
    case 5:
      fixture.put(fixture.other_component + 0xf0,
                  std::numeric_limits<double>::quiet_NaN());
      break;
    case 6:
      fixture.put(fixture.owner + 0xb88, PlacementTestArray{0, 1, 0});
      break;
    case 7:
      fixture.put(fixture.owner + 0x2f8, PlacementTestArray{0, -1, 0});
      break;
    case 8:
      fixture.put(fixture.other + 0x2d0, std::uintptr_t{1});
      break;
    case 9:
      fixture.put(fixture.other, std::uintptr_t{});
      break;
    case 10:
      fixture.put(fixture.fence + 8, std::uint32_t{0x40000000});
      break;
    case 11:
      fixture.put(fixture.unmarked_fence + 12, std::int32_t{});
      break;
    case 12:
      fixture.put(fixture.other_component + 16, std::uintptr_t{1});
      break;
    case 13:
      fixture.put(fixture.owner + 0x2f8, PlacementTestArray{0, 100001, 100001});
      break;
    case 14:
      fixture.put(fixture.fence + 0x60, PlacementTestArray{0, 16385, 16385});
      break;
    case 15:
      fixture.put(fixture.owner + 0xb88, PlacementTestArray{0, 1025, 1025});
      break;
    case 16:
      denied_address = fixture.unmarked_fence + 0x188;
      break;
    case 17:
      recycled_object = fixture.other_component;
      break;
    case 18:
      fixture.put(fixture.fence + 0x60,
                  fixture.array({std::uint64_t{0x10000}}));
      break;
    case 19:
      fixture.put(fixture.fence + 0x60,
                  fixture.array({fixture.name(std::string("x\0y", 3))}));
      break;
    case 20:
      fixture.put(fixture.fence + 0x60, fixture.array({fixture.name("")}));
      break;
    case 21:
      fixture.put(fixture.outhouse + 0x2f8,
                  fixture.array({fixture.fence, fixture.unmarked_fence + 1}));
      break;
    }
    const auto original = fixture.memory;
    const auto stats = fixture.run();
    expect(
        stats.skipped == 1 && stats.candidates == 0 && visibility_count == 0 &&
            fixture.memory == original,
        "unsafe or incomplete owners fail before every native visibility call");
  }
  for (const auto offset : {0x3a1, 0x3a2, 0x3a3, 0x3a4}) {
    PlacementFixture fixture;
    fixture.put(fixture.owner + static_cast<std::size_t>(offset),
                std::uint8_t{1});
    expect(fixture.run().skipped == 1 && visibility_count == 0,
           "each construction transition byte defers reconciliation");
    fixture.put(fixture.owner + static_cast<std::size_t>(offset),
                std::uint8_t{0});
    expect(fixture.run().hidden_priority == 1 && visibility_count == 2,
           "completion after a deferred transition applies normally");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.owner + 0x3c1, std::uint8_t{0});
    fixture.put(fixture.outhouse + 0x2d0, std::uintptr_t{});
    expect(fixture.run().skipped == 1 && visibility_count == 0,
           "unfinished asynchronous owner stays untouched");
    fixture.put(fixture.owner + 0x3c1, std::uint8_t{1});
    expect(fixture.run().skipped == 1 && visibility_count == 0,
           "completion waits for the last asynchronous source component");
    fixture.put(fixture.outhouse + 0x2d0, fixture.outhouse_component);
    expect(fixture.run().hidden_priority == 1 && visibility_count == 2,
           "final asynchronous model becomes eligible without cached state");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.owner + 0x2f8, fixture.array({fixture.outhouse}));
    fixture.put(fixture.outhouse_component + 0x188, std::uint8_t{0x85});
    expect(fixture.run().shown_priority == 1,
           "currently clear partial module set can show priority component");
    visibility_count = 0;
    fixture.put(fixture.owner + 0x2f8,
                fixture.array({fixture.outhouse, fixture.other}));
    expect(fixture.run().hidden_priority == 1 && visibility_count == 1,
           "later additional intersecting module hides the prior candidate");
    visibility_count = 0;
    fixture.put(fixture.owner + 0x2f8, fixture.array({fixture.outhouse}));
    expect(fixture.run().shown_priority == 1 && visibility_count == 1,
           "removing the intersecting module restores the candidate");
  }
  {
    PlacementFixture fixture;
    fixture.put(
        fixture.owner + 0x2f8,
        fixture.array({fixture.outhouse, fixture.outhouse, fixture.other}));
    const auto stats = fixture.run();
    expect(stats.candidates == 1 && stats.hidden_priority == 1 &&
               visibility_count == 2,
           "duplicate module and child array entries do not duplicate writes");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.other + 0x2d0, std::uintptr_t{});
    const auto stats = fixture.run();
    expect(stats.skipped == 0 && stats.candidates == 1 &&
               stats.hidden_priority == 0 && stats.hidden_marked == 1 &&
               visibility_count == 1,
           "native null ordinary final components are ignored in overlaps");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.other_component + 0xf0,
                PlacementBounds{{3, 0, 0}, {1, 1, 1}});
    fixture.put(fixture.outhouse + 0x2f8,
                fixture.array({fixture.outhouse_component}));
    fixture.put(fixture.outhouse_component + 0x60,
                fixture.array({fixture.name("overlapped")}));
    const auto stats = fixture.run();
    expect(
        stats.hidden_priority == 1 && stats.shown_priority == 0 &&
            visibility_count == 1 &&
            fixture.called(fixture.outhouse_component, false),
        "explicit overlapped child tag overrides a clear priority candidate");
    fixture.run();
    expect(visibility_count == 1,
           "same-component hide dominance remains stable on a second pass");
  }
  for (const bool wide : {false, true}) {
    PlacementFixture fixture;
    fixture.put(fixture.outhouse + 0x1e0,
                fixture.array({fixture.name(std::string(1, '\x80'), wide),
                               fixture.name("outhouseForRemoval")}));
    expect(fixture.run().hidden_priority == 1 && visibility_count == 2,
           "unrelated non-ASCII tags do not disable native placement policy");
  }
  {
    PlacementFixture fixture;
    fixture.put(fixture.manager + 0x518, PlacementTestArray{0, 1, 0});
    expect(fixture.run(true).skipped == 1 && visibility_count == 0,
           "invalid independent array rejects the world pass transaction");
    expect(fixture.run().hidden_priority == 1,
           "single-building reconciliation does not require world arrays");
  }
  {
    PlacementFixture fixture;
    const auto unrelated = fixture.object();
    fixture.put(fixture.manager + 0x508,
                fixture.array({fixture.owner, unrelated, unrelated}));
    const auto stats = fixture.run(true);
    expect(stats.buildings == 2 && stats.skipped == 1 &&
               stats.hidden_priority == 1 && stats.hidden_marked == 1,
           "one invalid world owner does not prevent another validated owner");
  }
  {
    PlacementFixture fixture;
    const auto foreign = fixture.object();
    fixture.put(fixture.owner + 0x2e8, foreign);
    expect(fixture.run(true).skipped == 1 && visibility_count == 0,
           "world pass rejects a building owned by another master");
  }
  for (const auto owner_type : {0, 1, 98, 145, 987, 10000}) {
    PlacementFixture fixture;
    fixture.put(fixture.owner + 0x3b8, std::int32_t{owner_type});
    expect(fixture.run().hidden_priority == 1 && visibility_count == 2,
           "native placement policy is independent of building type ID");
  }
  for (int mode{}; mode < 5; ++mode) {
    PlacementFixture fixture;
    if (mode == 0) {
      configure_placement_api({placement_test_read, nullptr});
    } else if (mode == 1) {
      configure_placement_api({nullptr, placement_test_visibility});
    } else if (mode == 2) {
      placement_image = 0;
    } else if (mode == 3) {
      placement_image = std::numeric_limits<std::uintptr_t>::max();
    } else {
      fixture.put(fixture.manager, std::uintptr_t{});
    }
    expect(fixture.run().skipped == 1 && fixture.run(true).skipped == 1 &&
               visibility_count == 0,
           "missing API, invalid image and invalid master are inert");
  }
  {
    PlacementFixture fixture;
    expect(placement_reconcile_building(nullptr).skipped == 1 &&
               placement_reconcile_world(nullptr).skipped == 1 &&
               visibility_count == 0,
           "null event and world targets never reach visibility API");
  }
  for (const bool world : {false, true}) {
    PlacementFixture fixture;
    const auto helper = fixture.object();
    fixture.put(fixture.other + 0x308, helper);
    fixture.put(fixture.other + 0x311, std::uint8_t{1});
    fixture.put(helper + 0x2a8, fixture.other);
    fixture.put(helper + 0x2b0, fixture.manager);
    const auto stats = fixture.run(world);
    expect(stats.skipped == 0 && stats.helpers_retired == 1 &&
               retirement_count == 1 && retired_helpers[0] == helper &&
               first_visibility_retirements == 1 &&
               fixture.memory.at(fixture.other)[0x311] == 0,
           "completed owner retires linked construction helpers before "
           "visibility, including excluded model 145");
    const auto again = fixture.run(world);
    expect(again.helpers_retired == 0 && retirement_count == 1 &&
               again.skipped == 0,
           "native-cleared helper links make later reconciliation idempotent");
  }
  {
    PlacementFixture fixture;
    fixture.run();
    visibility_count = 0;
    const auto helper = fixture.object();
    fixture.put(fixture.other + 0x2c0, std::int32_t{441});
    fixture.put(fixture.other + 0x308, helper);
    fixture.put(fixture.other + 0x311, std::uint8_t{1});
    fixture.put(fixture.other + 0x314, std::array<float, 2>{5, 5});
    fixture.put(helper + 0x2a8, fixture.other);
    fixture.put(helper + 0x2b0, fixture.manager);
    const auto stats = fixture.run();
    expect(stats.helpers_retired == 1 && stats.shown_general == 1 &&
               first_visibility_retirements == 1 &&
               fixture.called(fixture.other_component, true),
           "late streamed shed helper after first completion is retired by its "
           "later reconciliation event");
  }
  {
    PlacementFixture fixture;
    const auto helper = fixture.object();
    fixture.put(fixture.outhouse + 0x1e0,
                fixture.array({fixture.name("construction_50")}));
    fixture.put(fixture.outhouse + 0x308, helper);
    fixture.put(helper + 0x2a8, fixture.outhouse);
    fixture.put(helper + 0x2b0, fixture.manager);
    const auto stats = fixture.run();
    expect(stats.skipped == 0 && stats.helpers_retired == 1 &&
               retirement_count == 1 &&
               !fixture.called(fixture.outhouse_component, true),
           "module helper cleanup precedes the native construction-tag "
           "visibility exclusion");
  }
  for (int fault{}; fault < 15; ++fault) {
    PlacementFixture fixture;
    auto helper = fixture.object();
    if (fault == 7)
      helper = fixture.other_component;
    else if (fault == 8)
      helper = fixture.other;
    else if (fault == 9)
      helper = fixture.fence;
    fixture.put(fixture.outhouse + 0x308, helper);
    fixture.put(helper + 0x2a8, fixture.outhouse);
    fixture.put(helper + 0x2b0, fixture.manager);
    if (fault == 0)
      fixture.put(helper + 0x2a8, fixture.other);
    else if (fault == 1)
      fixture.put(helper + 0x2b0, fixture.owner);
    else if (fault == 2)
      fixture.put(helper + 8, std::uint32_t{0x40000000});
    else if (fault == 3)
      fixture.put(fixture.other + 0x308, helper);
    else if (fault == 4)
      denied_address = helper + 0x2a8;
    else if (fault == 5)
      denied_address = fixture.unmarked_fence + 0x188;
    else if (fault == 6)
      recycled_object = helper;
    else if (fault == 10)
      fixture.put(fixture.owner + 0x3a8, fixture.array({helper}));
    else if (fault == 11)
      fixture.put(fixture.owner + 0x3c1, std::uint8_t{0});
    else if (fault == 12)
      fixture.put(fixture.owner + 0x3a1, std::uint8_t{1});
    else if (fault == 13)
      fixture.put(fixture.other + 0x314, std::array<float, 2>{0, 5});
    else if (fault == 14)
      configure_placement_api({placement_test_read, placement_test_visibility,
                               placement_test_collision});
    const auto original = fixture.memory;
    const auto stats = fixture.run();
    expect(stats.skipped == 1 && stats.helpers_retired == 0 &&
               retirement_count == 0 && visibility_count == 0 &&
               collision_count == 0 && fixture.memory == original,
           "invalid helper links, aliases, late reads, identities or "
           "incomplete owner defer all native writes");
  }
  for (const int failure : {1, 2, 3}) {
    PlacementFixture fixture;
    const auto helper = fixture.object();
    fixture.put(fixture.other + 0x308, helper);
    fixture.put(fixture.other + 0x311, std::uint8_t{1});
    fixture.put(helper + 0x2a8, fixture.other);
    fixture.put(helper + 0x2b0, fixture.manager);
    retirement_failure = failure;
    const auto stats = fixture.run();
    expect(stats.skipped == 1 && stats.helpers_retired == 0 &&
               retirement_count == 1 && visibility_count == 0 &&
               collision_count == 0,
           "native retirement must clear module helper and replacement flag "
           "without recycling module before showing final meshes");
  }
  {
    PlacementFixture fixture;
    const auto module_helper = fixture.object();
    const auto replacement_helper = fixture.object();
    fixture.put(fixture.outhouse + 0x308, module_helper);
    fixture.put(module_helper + 0x2a8, fixture.outhouse);
    fixture.put(module_helper + 0x2b0, fixture.manager);
    fixture.put(fixture.owner + 0x3a8, fixture.array({replacement_helper}));
    const auto original = fixture.memory.at(replacement_helper);
    const auto stats = fixture.run();
    expect(stats.helpers_retired == 1 && retirement_count == 1 &&
               retired_helpers[0] == module_helper &&
               fixture.memory.at(replacement_helper) == original,
           "module helper retirement never cleans the separate owner "
           "replacement-proxy category");
  }
  std::printf("placement runtime: %d checks, %d failures\n", checks, failures);
  return failures;
}

auto main() -> int { return run_placement_runtime_tests(); }

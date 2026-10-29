#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "TreeRuntime.hpp"

int run_tree_profile_tests(const char *path);
int run_tree_bridge_tests();

static int checks{};
static int failures{};
static void *query_result{};
static void *query_master{};
static TreeVector query_point{};
static int query_calls{};

static auto expect(bool condition, const char *message) -> void {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL tree region: %s\n", message);
  }
}

struct Object {
  alignas(16) std::array<std::uint8_t, 0x1800> data{};
  template <class T> auto put(std::size_t offset, T value) -> void {
    std::memcpy(data.data() + offset, &value, sizeof(value));
  }
  auto pointer() -> void * { return data.data(); }
};

static auto query(void *master, const TreeVector *point) -> void * {
  ++query_calls;
  query_master = master;
  query_point = *point;
  return query_result;
}

struct Fixture {
  alignas(8) std::array<std::uintptr_t, 3> object_class{0x140001000, 0, 0};
  Object unit, original, target, player, enemy, master, other_master, component;
  std::array<void *, 2> regions{};
  std::array<void *, 1> components{};
  Fixture() {
    tree_image = 0x140000000;
    int index = 1;
    for (auto *object : {&unit, &original, &target, &player, &enemy, &master,
                         &other_master, &component}) {
      object->put(0, tree_image + 0x2000);
      object->put(12, index++);
      object->put(16, object_class.data());
    }
    player.put<std::uint8_t>(0x34c, 1);
    enemy.put<std::uint8_t>(0x34d, 1);
    master.put<std::uint8_t>(0x318, 1);
    master.put(0x5b0, player.pointer());
    regions = {original.pointer(), target.pointer()};
    master.put(0x560, TreeArray{regions.data(), 2, 2});
    for (auto *region : {&original, &target}) {
      region->put(0x318, master.pointer());
      region->put(0x350, player.pointer());
    }
    unit.put(0x318, master.pointer());
    unit.put(0x320, player.pointer());
    unit.put(0x328, original.pointer());
    unit.put<double>(0x4e8, 1250.25);
    unit.put<double>(0x4f0, -2089.5);
    unit.put<double>(0x4f8, 700.0);
    component.put(0x20, target.pointer());
    components[0] = component.pointer();
    target.put(0xf38, TreeArray{components.data(), 1, 1});
    target.put(0xf48, TreeArray{components.data(), 1, 1});
    query_result = target.pointer();
    query_master = nullptr;
    query_calls = 0;
    configure_tree_api({query});
  }
  ~Fixture() { configure_tree_api({}); }
  auto select() -> void * {
    return tree_select_region(unit.pointer(), original.pointer());
  }
  auto follow_component() -> void * {
    return tree_component_region(unit.pointer(), component.pointer(),
                                 original.pointer());
  }
};

auto main(int argc, char **argv) -> int {
  {
    Fixture f;
    const auto before =
        std::array{f.unit.data,   f.original.data, f.target.data,
                   f.player.data, f.master.data,   f.component.data};
    expect(f.select() == f.target.pointer(),
           "cross-owned planting uses geographic region");
    expect(query_calls == 1 && query_master == f.master.pointer() &&
               query_point.x == 1250.25 && query_point.y == -2089.5,
           "native lookup receives exact planting XY once");
    expect(f.follow_component() == f.target.pointer(),
           "growth reference uses actual visual component owner");
    expect(query_calls == 1,
           "growth registration does not repeat position or geographic lookup");
    expect(before == std::array{f.unit.data, f.original.data, f.target.data,
                                f.player.data, f.master.data, f.component.data},
           "unit region, coordinates, components and all game data remain "
           "unchanged");
    query_result = f.original.pointer();
    expect(f.select() == f.original.pointer(),
           "same-region planting unchanged");
  }
  {
    Fixture f;
    query_result = nullptr;
    expect(f.select() == f.original.pointer(),
           "outside-map coordinate preserves original region");
    query_result = reinterpret_cast<void *>(1);
    expect(f.select() == f.original.pointer(),
           "unreadable query result fails safely");
  }
  {
    Fixture f;
    f.target.put(0x350, f.enemy.pointer());
    const auto before =
        std::array{f.unit.data, f.original.data, f.target.data, f.enemy.data};
    expect(f.select() == f.target.pointer(),
           "tree actually planted in AI territory uses geographic region");
    expect(f.follow_component() == f.target.pointer(),
           "AI-owned region receives growth record for its actual component");
    expect(
        before == std::array{f.unit.data, f.original.data, f.target.data,
                             f.enemy.data},
        "foreign-region correction does not transfer land or worker ownership");
  }
  {
    Fixture f;
    f.target.put<void *>(0x350, nullptr);
    const auto before = std::array{f.unit.data, f.original.data, f.target.data};
    expect(f.select() == f.target.pointer(),
           "tree actually planted in unowned territory uses geographic region");
    expect(f.follow_component() == f.target.pointer(),
           "unowned region receives growth record for its actual component");
    expect(before == std::array{f.unit.data, f.original.data, f.target.data},
           "unowned-region correction does not claim the region or move the "
           "worker");
  }
  {
    Fixture f;
    f.player.put<std::uint8_t>(0x34d, 1);
    expect(f.select() == f.original.pointer() && query_calls == 0,
           "AI planting stays native");
    expect(f.follow_component() == f.original.pointer(),
           "AI growth registration stays native");
  }
  {
    Fixture f;
    f.target.put(0x318, f.other_master.pointer());
    expect(f.select() == f.original.pointer(),
           "region from another world rejected");
    expect(f.follow_component() == f.original.pointer(),
           "component belonging to another world rejected");
  }
  {
    Fixture f;
    f.component.put(0x20, f.original.pointer());
    f.original.put(0xf38, TreeArray{f.components.data(), 1, 1});
    expect(f.follow_component() == f.original.pointer(),
           "native-created original component stays in its actual region");
  }
  {
    Fixture f;
    f.target.put(0xf38, TreeArray{});
    expect(f.follow_component() == f.original.pointer(),
           "component must be registered in the claimed region");
  }
  for (TreeArray broken : {TreeArray{nullptr, 1, 1}, TreeArray{nullptr, -1, 0},
                           TreeArray{nullptr, 2, 1},
                           TreeArray{reinterpret_cast<void *>(1), 1, 1}}) {
    Fixture f;
    f.target.put(0xf38, broken);
    expect(f.follow_component() == f.original.pointer(),
           "invalid component membership array fails closed");
  }
  for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity()}) {
    Fixture f;
    f.unit.put(0x4e8, bad);
    expect(f.select() == f.original.pointer() && query_calls == 0,
           "nonfinite planting coordinates never reach native query");
  }
  {
    Fixture f;
    configure_tree_api({});
    expect(f.select() == f.original.pointer(),
           "unconfigured hook preserves native region");
    expect(tree_select_region(nullptr, f.original.pointer()) ==
               f.original.pointer(),
           "null unit safe");
    expect(tree_component_region(f.unit.pointer(), nullptr,
                                 f.original.pointer()) == f.original.pointer(),
           "null component safe");
  }
  for (auto flags : {0x40000000U, 0x10000U, 0x8000U}) {
    Fixture f;
    f.target.put(8, flags);
    expect(f.select() == f.original.pointer(),
           "invalid target UObject flags rejected");
    expect(f.follow_component() == f.original.pointer(),
           "invalid owner UObject flags rejected");
  }
  {
    Fixture f;
    f.regions[1] = f.original.pointer();
    expect(f.select() == f.original.pointer(),
           "target must be in current world's canonical region list");
    expect(f.follow_component() == f.original.pointer(),
           "component owner must be canonical region");
  }
  std::printf("behavior: %d checks, %d failures\n", checks, failures);
  tree_image = 0;
  if (argc != 2)
    return 2;
  failures += run_tree_profile_tests(argv[1]);
  failures += run_tree_bridge_tests();
  return failures == 0 ? 0 : 1;
}

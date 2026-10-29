#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "MarketSelection.hpp"

static std::map<std::uintptr_t, std::vector<std::uint8_t>> *test_memory{};
static std::uintptr_t denied_address{}, changed_identity{}, expected_grid{};
static std::int32_t index_result{}, workers_result{2};
static bool contains_result{true}, contains_throws{};
static bool replace_cells{};
static std::size_t contains_calls{}, worker_calls{}, checks{};

static void check(bool condition, const char *message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, message);
    std::exit(1);
  }
}

static bool test_read(std::uintptr_t address, void *output,
                      std::size_t size) noexcept {
  if (!test_memory || address == denied_address)
    return false;
  auto found = test_memory->upper_bound(address);
  if (found == test_memory->begin())
    return false;
  --found;
  if (address - found->first > found->second.size() ||
      size > found->second.size() - (address - found->first))
    return false;
  std::memcpy(output, found->second.data() + address - found->first, size);
  return true;
}

static bool test_contains(const MarketSelectionVec3 *cursor,
                          const MarketSelectionArray *triangles) {
  ++contains_calls;
  check(cursor && triangles && triangles->count == 6 &&
            triangles->capacity == 6 && triangles->data != 0,
        "native contains receives validated local triangles");
  const auto *vertices =
      reinterpret_cast<const MarketSelectionVec3 *>(triangles->data);
  check(vertices[0].x == 0 && vertices[1].x == 20 && vertices[2].y == 20,
        "native contains receives original vertex coordinates");
  if (changed_identity)
    test_memory->at(changed_identity)[12] ^= 0x40;
  if (replace_cells)
    test_memory->at(expected_grid)[0x440] ^= 1;
  if (contains_throws)
    throw std::runtime_error("simulated native failure");
  return contains_result;
}

static std::int32_t test_cell(void *grid, const MarketSelectionVec3 *cursor) {
  check(reinterpret_cast<std::uintptr_t>(grid) == expected_grid && cursor,
        "native cell lookup receives actual world grid and cursor");
  return index_result;
}

static std::int32_t test_workers(void *stall) {
  ++worker_calls;
  check(stall != nullptr, "native worker limit receives actual stall");
  return workers_result;
}

struct SelectionFixture {
  std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
  std::uintptr_t next{0x200000000}, object_class{}, pawn{}, master{}, region{},
      grid{}, market{}, stall{}, second_stall{}, other{}, cells{}, candidates{},
      vertices{};

  std::uintptr_t allocate(std::size_t size = 0x1800) {
    const auto address = next;
    next += 0x10000;
    memory.emplace(address, std::vector<std::uint8_t>(size));
    return address;
  }

  template <class T> void put(std::uintptr_t address, const T &value) {
    auto found = std::prev(memory.upper_bound(address));
    check(address - found->first <= found->second.size() &&
              sizeof(T) <= found->second.size() - (address - found->first),
          "test write remains in fixture allocation");
    std::memcpy(found->second.data() + address - found->first, &value,
                sizeof(value));
  }

  std::uintptr_t object() {
    const auto address = allocate();
    put(address, std::uintptr_t{0x140001000});
    put(address + 12, static_cast<std::int32_t>(memory.size() + 1));
    put(address + 16, object_class);
    return address;
  }

  void order(const std::vector<std::uintptr_t> &objects) {
    put(cells + 0x10, MarketSelectionArray{
                          candidates, static_cast<std::int32_t>(objects.size()),
                          static_cast<std::int32_t>(objects.size())});
    for (std::size_t i{}; i < objects.size(); ++i)
      put(candidates + i * sizeof(objects[i]), objects[i]);
  }

  explicit SelectionFixture() {
    test_memory = &memory;
    denied_address = changed_identity = 0;
    index_result = 0;
    workers_result = 2;
    contains_calls = worker_calls = 0;
    contains_result = true;
    contains_throws = false;
    replace_cells = false;
    object_class = allocate();
    put(object_class, std::uintptr_t{0x140002000});
    pawn = object();
    master = object();
    region = object();
    grid = object();
    market = object();
    stall = object();
    second_stall = object();
    other = object();
    cells = allocate(0x90);
    candidates = allocate(0x100);
    vertices = allocate(6 * sizeof(MarketSelectionVec3));
    put(grid + 0x438, MarketSelectionArray{cells, 2, 2});
    put(pawn + 0x340, master);
    put(pawn + 0x4e8, MarketSelectionVec3{10, 10, 3});
    put(master + 0x2c0, grid);
    put(market + 0x3b8, std::int32_t{13});
    put(market + 0x3cc, std::uint8_t{15});
    for (const auto building : {market, stall, second_stall, other}) {
      put(building + 0x2e8, master);
      put(building + 0x2e0, pawn);
      put(building + 0x2c8, region);
    }
    for (const auto building : {stall, second_stall}) {
      put(building + 0x3b8, std::int32_t{93});
      put(building + 0x3cc, std::uint8_t{3});
      put(building + 0xca8, market);
      put(building + 0x6c8, std::array<double, 4>{0, 0, 20, 20});
      put(building + 0x678, MarketSelectionArray{vertices, 6, 6});
    }
    const std::array<MarketSelectionVec3, 6> geometry{{{0, 0, 0},
                                                       {20, 0, 0},
                                                       {20, 20, 0},
                                                       {20, 20, 0},
                                                       {0, 20, 0},
                                                       {0, 0, 0}}};
    put(vertices, geometry);
    order({stall, market, other});
    expected_grid = grid;
    configure_market_selection(
        0x140000000, {test_read, test_contains, test_cell, test_workers});
  }

  void *choose(std::uintptr_t accepted = 0) {
    return market_selection_target(
        reinterpret_cast<void *>(pawn),
        reinterpret_cast<void *>(accepted ? accepted : market));
  }
};

int main() {
  {
    SelectionFixture fixture;
    auto order = std::vector{fixture.market, fixture.stall, fixture.other};
    std::sort(order.begin(), order.end());
    do {
      fixture.order(order);
      const auto before = fixture.memory;
      check(fixture.choose() == reinterpret_cast<void *>(fixture.stall),
            "every marketplace/stall/Manor ordering selects owned stall");
      check(fixture.memory == before, "runtime never writes game memory");
    } while (std::next_permutation(order.begin(), order.end()));
    check(fixture.choose(fixture.stall) ==
                  reinterpret_cast<void *>(fixture.stall) &&
              fixture.choose(fixture.other) ==
                  reinterpret_cast<void *>(fixture.other),
          "originally chosen stall and unrelated building remain unchanged");
    fixture.order({fixture.stall, fixture.second_stall, fixture.market});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.second_stall),
          "multiple eligible stalls preserve original reverse order");
    fixture.put(fixture.second_stall + 0xca8, fixture.other);
    check(fixture.choose() == reinterpret_cast<void *>(fixture.stall),
          "another marketplace's overlapping stall is not stolen");
    fixture.put(fixture.stall + 0x458, std::array<std::uint8_t, 2>{1, 1});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.stall),
          "selection and pinned-tab semantics stay with native click handler");
    fixture.put(fixture.stall + 0x3b8, std::int32_t{91});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.stall),
          "food stalls use the same priority repair");
    fixture.put(fixture.stall + 0x3b8, std::int32_t{999});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "unaudited function-3 definitions retain original behavior");
  }
  for (const auto offset : {0x560, 0x7a0, 0xe79, 0xa59, 0xb20, 0x840}) {
    SelectionFixture fixture;
    fixture.put(fixture.pawn + offset, std::uint8_t{1});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "physical selection and every construction mode remain native");
    check(contains_calls == 0, "excluded modes do not test stall geometry");
  }
  for (const auto offset : {0x2e0, 0x2e8, 0x2c8, 0xca8}) {
    SelectionFixture fixture;
    fixture.put(fixture.stall + offset, fixture.other);
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "foreign owner/world/region/market association remains untouched");
  }
  {
    SelectionFixture fixture;
    fixture.put(fixture.market + 0x2e0, fixture.other);
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "foreign marketplace remains unchanged");
  }
  for (const auto cursor :
       {MarketSelectionVec3{0, 10, 3}, MarketSelectionVec3{20, 10, 3},
        MarketSelectionVec3{10, 0, 3}, MarketSelectionVec3{10, 20, 3},
        MarketSelectionVec3{100, 100, 3}}) {
    SelectionFixture fixture;
    fixture.put(fixture.pawn + 0x4e8, cursor);
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "strict native bounds preserve edges and empty marketplace ground");
    check(contains_calls == 0, "outside-bounds triangle test is skipped");
  }
  {
    SelectionFixture fixture;
    contains_result = false;
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market) &&
              contains_calls == 1,
          "native triangle result is authoritative inside bounds");
    contains_result = true;
    fixture.put(fixture.pawn + 0x1188, fixture.other);
    fixture.put(fixture.stall + 0x1030, std::int32_t{2});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market) &&
              worker_calls == 1,
          "full worker-capacity stall is not an assignment target");
    fixture.put(fixture.stall + 0x1030, std::int32_t{1});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.stall),
          "native capacity permits a stall with available family slot");
    fixture.put(fixture.pawn + 0x1188, std::uintptr_t{});
    const auto calls = worker_calls;
    fixture.put(fixture.stall + 0x1030, std::int32_t{100});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.stall) &&
              worker_calls == calls,
          "ordinary hover does not invent a capacity restriction");
  }
  for (const auto bad_index : {-1, 2, 100}) {
    SelectionFixture fixture;
    index_result = bad_index;
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "invalid native grid cell keeps original selection");
  }
  {
    SelectionFixture fixture;
    denied_address = fixture.stall + 0xca8;
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "missing native data fails closed");
    denied_address = 0;
    changed_identity = fixture.stall;
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "recycled UObject after native predicate fails closed");
    changed_identity = 0;
    contains_throws = true;
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "native query exception preserves original target");
  }
  for (const auto target_offset : {0x438, 0x678}) {
    SelectionFixture fixture;
    const auto target = target_offset == 0x438 ? fixture.grid : fixture.stall;
    fixture.put(target + target_offset,
                MarketSelectionArray{fixture.vertices, -1, 6});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "malformed native arrays fail closed");
  }
  {
    SelectionFixture fixture;
    fixture.order({fixture.stall});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "stale accepted market outside current cell is not substituted");
    fixture.order({fixture.market});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "stalls outside current native cell are never globally scanned");
    fixture.order({fixture.stall, fixture.market});
    fixture.put(
        fixture.vertices,
        MarketSelectionVec3{std::numeric_limits<double>::quiet_NaN(), 0, 0});
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "nonfinite native geometry is not sent to predicate");
  }
  {
    SelectionFixture fixture;
    replace_cells = true;
    check(fixture.choose() == reinterpret_cast<void *>(fixture.market),
          "grid cells replaced during native query invalidate result");
  }
  std::printf("market selection: %zu checks passed\n", checks);
  return 0;
}

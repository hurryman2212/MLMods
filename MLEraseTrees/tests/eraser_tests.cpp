#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <Windows.h>

#include "EraserRuntime.hpp"

int run_eraser_profile_tests(const char *path);
int run_eraser_bridge_tests();

static int checks{}, failures{};

static auto expect(bool success, const char *message) -> void {
  ++checks;
  if (!success) {
    ++failures;
    std::fprintf(stderr, "eraser runtime: %s\n", message);
  }
}

struct EraserTestRecord {
  EraserVector point{};
  std::array<std::uint8_t, 16> metadata{};
};
static_assert(sizeof(EraserTestRecord) == 40);

struct EraserTestObject {
  alignas(16) std::array<std::uint8_t, 0x1800> data{};
  template <class T> auto put(std::size_t offset, T value) -> void {
    std::memcpy(data.data() + offset, &value, sizeof(value));
  }
  template <class T> auto get(std::size_t offset) const -> T {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
  }
  auto pointer() -> void * { return data.data(); }
};

struct EraserTestQueue {
  std::array<EraserTestRecord, 16> records{};
  std::array<const void *, 16> sources{};
  std::array<void *, 16> navigation{};
  std::array<int, 16> counts{};
  EraserTestObject *region{};
  int calls{};
};
static EraserTestQueue queued{};

static auto queue_tree_removal(void *navigation, const void *record) -> void {
  const auto index = static_cast<std::size_t>(queued.calls++);
  if (index >= queued.records.size()) {
    expect(false, "navigation receives no duplicate or excessive removals");
    return;
  }
  std::memcpy(&queued.records[index], record, sizeof(EraserTestRecord));
  queued.sources[index] = record;
  queued.navigation[index] = navigation;
  queued.counts[index] = queued.region->get<EraserArray>(0xcd0).count;
}

struct EraserFixture {
  alignas(8) std::array<std::uintptr_t, 3> object_class{0x140001000, 0, 0};
  EraserTestObject pawn, region, master, component, navigation, foreign;
  std::array<void *, 1> regions{}, components{};
  std::array<EraserTestRecord, 8> records{};
  std::array<EraserVector, 8> nav_records{};
  std::array<EraserVector, 3> selected{};
  EraserArray removed{};

  EraserFixture() {
    eraser_image = 0x140000000;
    int index = 1;
    for (auto *object :
         {&pawn, &region, &master, &component, &navigation, &foreign}) {
      object->put(0, eraser_image + 0x2000);
      object->put(12, index++);
      object->put(16, object_class.data());
    }
    pawn.put<std::uint8_t>(0x34c, 1);
    pawn.put<std::uint8_t>(0x12ec, 1);
    pawn.put(0x340, master.pointer());
    pawn.put(0x978, region.pointer());
    region.put(0x318, master.pointer());
    region.put(0x350, pawn.pointer());
    master.put<std::uint8_t>(0x318, 1);
    master.put(0x5b0, pawn.pointer());
    regions[0] = region.pointer();
    components[0] = component.pointer();
    master.put(0x560, EraserArray{regions.data(), 1, 1});
    region.put(0xf38, EraserArray{components.data(), 1, 1});
    region.put(0xf48, EraserArray{components.data(), 1, 1});
    component.put(0x20, region.pointer());
    navigation.put(0x20, region.pointer());
    navigation.put(0x2d8, EraserArray{nav_records.data(), 0, 8});
    region.put(0x738, navigation.pointer());
    records[0].point = {100, 200, 300};
    records[1].point = {400, 500, 600};
    records[2].point = records[0].point;
    records[3].point = {100.00005, 200.00005, 300.00005};
    records[4].point = {100, 200, 300.001};
    records[5].point = {800, 900, 1000};
    for (std::size_t i{}; i < records.size(); ++i)
      records[i].metadata.fill(static_cast<std::uint8_t>(i + 1));
    region.put(0xcd0, EraserArray{records.data(), 6, 8});
    selected = {records[0].point, records[0].point, records[5].point};
    removed = {selected.data(), 3, 3};
    queued = {};
    queued.region = &region;
    configure_eraser_api({queue_tree_removal});
  }
  ~EraserFixture() { configure_eraser_api({}); }
  auto allow() -> bool {
    return eraser_allow_tree(pawn.pointer(), component.pointer());
  }
  auto cleanup(bool success = true) -> void {
    eraser_cleanup(pawn.pointer(), component.pointer(), &removed, success);
  }
  auto expect_rejected(const char *message) -> void {
    const auto original_region = region.data;
    const auto original_records = records;
    expect(!allow(), message);
    cleanup();
    expect(region.data == original_region &&
               std::memcmp(records.data(), original_records.data(),
                           sizeof(records)) == 0 &&
               queued.calls == 0,
           "rejected context leaves records, count, and navigation unchanged");
  }
};

auto main(int argc, char **argv) -> int {
  {
    EraserFixture f;
    auto expected_region = f.region.data;
    const auto other_objects =
        std::array{f.pawn.data, f.master.data, f.component.data,
                   f.navigation.data, f.foreign.data};
    const auto original_records = f.records;
    const auto original_selected = f.selected;
    const auto original_removed = f.removed;
    expect(f.allow(), "active human shrubbery tool accepts registered "
                      "standing-tree component");
    f.cleanup();
    const auto tree_array = f.region.get<EraserArray>(0xcd0);
    expect(tree_array.data == f.records.data() && tree_array.count == 2 &&
               tree_array.capacity == 8,
           "all matching logical records removed without reallocating or "
           "shrinking capacity");
    expect(std::memcmp(&f.records[0], &original_records[1],
                       sizeof(EraserTestRecord)) == 0 &&
               std::memcmp(&f.records[1], &original_records[4],
                           sizeof(EraserTestRecord)) == 0,
           "stable compaction retains all metadata and order of surviving "
           "records");
    expect(queued.calls == 4,
           "duplicate selected positions remove each matching record once");
    constexpr std::array<std::size_t, 4> erased{0, 2, 3, 5};
    for (std::size_t i{}; i < erased.size(); ++i) {
      expect(std::memcmp(&queued.records[i], &original_records[erased[i]],
                         sizeof(EraserTestRecord)) == 0,
             "navigation receives complete original record before compaction");
      expect(queued.navigation[i] == f.navigation.pointer() &&
                 queued.counts[i] == 6 - static_cast<int>(i),
             "native queue called with owning navigation while pre-removal "
             "count remains visible");
      expect(queued.sources[i] >= static_cast<const void *>(f.records.data()) &&
                 queued.sources[i] < static_cast<const void *>(
                                         f.records.data() + f.records.size()),
             "native queue receives original array record address");
    }
    const std::int32_t expected_count = 2;
    std::memcpy(expected_region.data() + 0xcd8, &expected_count,
                sizeof(expected_count));
    expect(f.region.data == expected_region &&
               other_objects == std::array{f.pawn.data, f.master.data,
                                           f.component.data, f.navigation.data,
                                           f.foreign.data},
           "only logical tree array contents and count change in game objects");
    expect(std::memcmp(f.selected.data(), original_selected.data(),
                       sizeof(f.selected)) == 0 &&
               std::memcmp(&f.removed, &original_removed, sizeof(f.removed)) ==
                   0,
           "selected native XYZ data and header remain unchanged");
    f.cleanup();
    expect(queued.calls == 4 && f.region.get<EraserArray>(0xcd0).count == 2,
           "repeated cleanup is idempotent");
  }
  {
    EraserFixture f;
    const auto original_records = f.records;
    const auto original_region = f.region.data;
    f.cleanup(false);
    expect(f.region.data == original_region &&
               std::memcmp(f.records.data(), original_records.data(),
                           sizeof(f.records)) == 0 &&
               queued.calls == 0,
           "failed native instance deletion never deletes logical records");
    f.removed = {};
    f.cleanup();
    expect(f.region.data == original_region && queued.calls == 0,
           "empty native brush selection leaves records unchanged");
  }
  {
    EraserFixture f;
    f.region.put<void *>(0x738, nullptr);
    expect(f.allow(), "native optional-null navigation is supported");
    f.cleanup();
    expect(f.region.get<EraserArray>(0xcd0).count == 2 && queued.calls == 0,
           "null navigation still removes logical records without callbacks");
  }
  {
    EraserFixture f;
    const double tolerance = static_cast<double>(0.0001F);
    const double outside =
        std::nextafter(tolerance, std::numeric_limits<double>::infinity());
    f.records[0].point = {tolerance, tolerance, tolerance};
    f.records[1].point = {outside, 0, 0};
    f.records[2].point = {0, outside, 0};
    f.records[3].point = {0, 0, outside};
    f.records[4].point = {-tolerance, -tolerance, -tolerance};
    f.records[5].point = {-outside, 0, 0};
    f.selected[0] = {};
    f.removed.count = 1;
    f.cleanup();
    expect(f.region.get<EraserArray>(0xcd0).count == 4 && queued.calls == 2,
           "exact native float-derived XYZ tolerance accepts boundaries and "
           "rejects next representable values");
    expect(f.records[0].point.x == outside && f.records[1].point.y == outside &&
               f.records[2].point.z == outside &&
               f.records[3].point.x == -outside,
           "all three coordinate axes and both signs constrain matching");
  }
  for (auto mode : {0, 2, 3, 255}) {
    EraserFixture f;
    f.pawn.put<std::uint8_t>(0x12ec, static_cast<std::uint8_t>(mode));
    f.expect_rejected("other active tools cannot erase standing trees");
  }
  for (auto flags : {std::array<std::uint8_t, 2>{0, 0}, {1, 1}, {2, 0}}) {
    EraserFixture f;
    f.pawn.put(0x34c, flags);
    f.expect_rejected("AI and nonhuman pawns cannot extend deletion");
  }
  for (int mutation{}; mutation < 10; ++mutation) {
    EraserFixture f;
    switch (mutation) {
    case 0:
      f.region.put(0x350, f.foreign.pointer());
      break;
    case 1:
      f.region.put(0x318, f.foreign.pointer());
      break;
    case 2:
      f.master.put(0x5b0, f.foreign.pointer());
      break;
    case 3:
      f.master.put<std::uint8_t>(0x318, 0);
      break;
    case 4:
      f.regions[0] = f.foreign.pointer();
      break;
    case 5:
      f.component.put(0x20, f.foreign.pointer());
      break;
    case 6:
      f.region.put(0xf38, EraserArray{});
      break;
    case 7:
      f.region.put(0xf48, EraserArray{});
      break;
    case 8:
      f.navigation.put(0x20, f.foreign.pointer());
      break;
    case 9:
      f.pawn.put(0x978, f.foreign.pointer());
      break;
    }
    f.expect_rejected("foreign ownership, world, navigation or unregistered "
                      "foliage fails closed");
  }
  for (auto mask : {0x40000000U, 0x10000U, 0x8000U}) {
    for (int object{}; object < 5; ++object) {
      EraserFixture f;
      std::array<EraserTestObject *, 5> objects{&f.pawn, &f.region, &f.master,
                                                &f.component, &f.navigation};
      objects[object]->put(8, mask);
      f.expect_rejected(
          "invalid UObject lifetime flags prevent destructive callbacks");
    }
  }
  for (const auto broken :
       {EraserArray{nullptr, 1, 1}, EraserArray{nullptr, -1, 0},
        EraserArray{nullptr, 2, 1},
        EraserArray{reinterpret_cast<void *>(1), 1, 1},
        EraserArray{nullptr, 0, 1000001}}) {
    for (int target{}; target < 4; ++target) {
      EraserFixture f;
      if (target == 0)
        f.master.put(0x560, broken);
      if (target == 1)
        f.region.put(0xf48, broken);
      if (target == 2)
        f.region.put(0xcd0, broken);
      if (target == 3)
        f.navigation.put(0x2d8, broken);
      f.expect_rejected("invalid canonical, component, logical-tree or "
                        "navigation arrays rejected");
    }
    EraserFixture f;
    const auto original = f.records;
    const auto original_region = f.region.data;
    f.removed = broken;
    f.cleanup();
    expect(
        std::memcmp(f.records.data(), original.data(), sizeof(f.records)) ==
                0 &&
            f.region.data == original_region && queued.calls == 0,
        "malformed native selected positions cannot trigger partial deletion");
  }
  for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity()}) {
    for (int axis{}; axis < 3; ++axis) {
      EraserFixture f;
      std::memcpy(reinterpret_cast<std::uint8_t *>(&f.records[5].point) +
                      axis * sizeof(double),
                  &bad, sizeof(bad));
      f.expect_rejected(
          "any nonfinite logical record prevents partial deletion");
    }
    EraserFixture f;
    const auto original = f.records;
    f.selected[2].z = bad;
    f.cleanup();
    expect(
        std::memcmp(f.records.data(), original.data(), sizeof(f.records)) ==
                0 &&
            queued.calls == 0,
        "later nonfinite selected position is rejected before first mutation");
  }
  {
    EraserFixture f;
    configure_eraser_api({});
    f.expect_rejected(
        "unconfigured native navigation API blocks tree extension");
    expect(!eraser_allow_tree(nullptr, f.component.pointer()) &&
               !eraser_allow_tree(f.pawn.pointer(), nullptr) &&
               !eraser_allow_tree(reinterpret_cast<void *>(1),
                                  f.component.pointer()),
           "null and unreadable hook inputs fail safely");
  }
  {
    EraserFixture f;
    auto *readonly =
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    expect(readonly != nullptr, "read-only record fixture allocation succeeds");
    if (readonly != nullptr) {
      std::memcpy(readonly, f.records.data(), sizeof(f.records));
      DWORD old{};
      expect(VirtualProtect(readonly, 0x1000, PAGE_READONLY, &old) != 0,
             "record fixture becomes read-only");
      f.region.put(0xcd0, EraserArray{readonly, 6, 8});
      f.expect_rejected(
          "readable but nonwritable logical record storage fails closed");
      expect(VirtualFree(readonly, 0, MEM_RELEASE) != 0,
             "read-only fixture released");
    }
  }
  std::printf("eraser runtime: %d checks, %d failures\n", checks, failures);
  eraser_image = 0;
  if (argc != 2) {
    std::fprintf(stderr, "usage: eraser_tests.exe <supported game EXE>\n");
    return 2;
  }
  failures += run_eraser_profile_tests(argv[1]);
  failures += run_eraser_bridge_tests();
  return failures == 0 ? 0 : 1;
}

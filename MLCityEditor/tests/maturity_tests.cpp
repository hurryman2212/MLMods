#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "Maturity.hpp"

static int maturity_checks{}, maturity_failures{}, maturity_visual_calls{};
static int maturity_progress_calls{};
static void *maturity_visual_building{};
static void *maturity_progress_building{};
static std::array<std::uint8_t, 4> maturity_progress_flags{};
static std::vector<int> maturity_events;

static auto maturity_expect(bool success, const char *message) -> void {
  ++maturity_checks;
  if (!success) {
    ++maturity_failures;
    std::fprintf(stderr, "FAIL maturity: %s\n", message);
  }
}

static auto maturity_refresh(void *building) -> void {
  maturity_events.push_back(2);
  ++maturity_visual_calls;
  maturity_visual_building = building;
}

static auto maturity_progress(void *building, std::uint8_t plough,
                              std::uint8_t planted, std::uint8_t growth,
                              std::uint8_t harvest) -> void {
  ++maturity_progress_calls;
  maturity_progress_building = building;
  maturity_progress_flags = {plough, planted, growth, harvest};
  maturity_events.push_back(1);
}

template <class T, std::size_t N>
static auto maturity_put(std::array<std::uint8_t, N> &data, std::size_t offset,
                         const T &value) -> void {
  std::memcpy(data.data() + offset, &value, sizeof(value));
}

struct MaturityFixture {
  alignas(16) std::array<std::uint8_t, 0x1800> building{};
  alignas(16) std::array<std::uint8_t, 0x360> owner{};
  alignas(8) std::array<std::uintptr_t, 3> object_class{0x140001000, 0, 0};
  alignas(16) std::array<std::uint8_t, 0xd0 * 2> extensions{};
  alignas(16) std::array<std::uint8_t, 0xd0 * 3> plants{};

  explicit MaturityFixture(std::uint8_t crop = 9) {
    city_image = 0x140000000;
    building.fill(0xa7);
    owner.fill(0x35);
    extensions.fill(0x9a);
    plants.fill(0x6c);
    for (auto *data : {building.data(), owner.data()}) {
      const std::uintptr_t table = city_image + 0x2000;
      const std::uint32_t flags = 8;
      const std::int32_t index = data == building.data() ? 12 : 13;
      auto *object_type = object_class.data();
      std::memcpy(data, &table, sizeof(table));
      std::memcpy(data + 8, &flags, sizeof(flags));
      std::memcpy(data + 12, &index, sizeof(index));
      std::memcpy(data + 16, &object_type, sizeof(object_type));
    }
    maturity_put(owner, 0x34c, std::uint8_t{1});
    maturity_put(owner, 0x34d, std::uint8_t{0});
    maturity_put(building, 0x2e0, owner.data());
    maturity_put(building, 0x3a1, std::uint8_t{0});
    maturity_put(building, 0x3c1, std::uint8_t{1});
    maturity_put(building, 0x4c8, crop);
    maturity_put(building, 0x530, CityArray{plants.data(), 3, 3});
    maturity_put(building, 0xb88, CityArray{extensions.data(), 1, 2});
    maturity_put(extensions, 0, std::int32_t{12});
    for (std::size_t i{}; i < 3; ++i) {
      maturity_put(plants, i * 0xd0, crop);
      maturity_put(plants, i * 0xd0 + 0x20,
                   std::array<float, 3>{0.0F, 0.5F, 1.0F}[i]);
      maturity_put(plants, i * 0xd0 + 0x24,
                   std::array<float, 3>{0.0F, 0.375F, 1.0F}[i]);
      maturity_put(plants, i * 0xd0 + 0x4c,
                   std::array<std::int32_t, 3>{0, 7, 13}[i]);
    }
    maturity_visual_calls = 0;
    maturity_progress_calls = 0;
    maturity_visual_building = nullptr;
    maturity_progress_building = nullptr;
    maturity_progress_flags = {};
    maturity_events.clear();
    configure_maturity({maturity_refresh, maturity_progress});
  }

  ~MaturityFixture() { configure_maturity({}); }

  auto unchanged(std::int32_t id, bool expected, const char *message) -> void {
    const auto before_building = building;
    const auto before_owner = owner;
    const auto before_extensions = extensions;
    const auto before_plants = plants;
    const int before_calls = maturity_visual_calls;
    const int before_progress = maturity_progress_calls;
    const auto before_events = maturity_events;
    maturity_expect(city_mature_new_orchard(building.data(), id) == expected,
                    message);
    maturity_expect(building == before_building && owner == before_owner &&
                        extensions == before_extensions &&
                        plants == before_plants &&
                        maturity_visual_calls == before_calls &&
                        maturity_progress_calls == before_progress &&
                        maturity_events == before_events,
                    "no-op or failure preserves every byte and renderer state");
  }
};

auto run_maturity_tests() -> int {
  maturity_checks = maturity_failures = 0;
  const auto previous_image = city_image;
  for (const auto [crop, id] :
       {std::pair<std::uint8_t, std::int32_t>{9, 33}, {13, 34}, {14, 32}}) {
    for (const auto action : {id, std::int32_t{112}}) {
      MaturityFixture fixture(crop);
      const auto building = fixture.building;
      const auto owner = fixture.owner;
      const auto extensions = fixture.extensions;
      auto expected = fixture.plants;
      for (std::size_t i{}; i < 3; ++i) {
        maturity_put(expected, i * 0xd0 + 0x20, 1.0F);
        maturity_put(expected, i * 0xd0 + 0x24, 1.0F);
      }
      maturity_expect(
          city_mature_new_orchard(fixture.building.data(), action),
          "all three fruit cultivars reach mature growth for selection/build");
      maturity_expect(fixture.plants == expected,
                      "only planting work and Growth change; yields and "
                      "transforms preserved");
      maturity_expect(
          fixture.building == building && fixture.owner == owner &&
              fixture.extensions == extensions,
          "dates, inventories, season accumulators and owner data preserved");
      maturity_expect(maturity_visual_calls == 1 &&
                          maturity_visual_building == fixture.building.data(),
                      "changed orchard refreshes native visuals exactly once");
      maturity_expect(
          maturity_progress_calls == 1 &&
              maturity_progress_building == fixture.building.data() &&
              maturity_progress_flags ==
                  std::array<std::uint8_t, 4>{1, 1, 1, 0} &&
              maturity_events == std::vector<int>{1, 2},
          "progress cache refresh precedes visuals with native flags 1/1/1/0");
      fixture.unchanged(
          action, true,
          "already mature orchard is an idempotent successful no-op");
    }
  }
  for (std::int32_t id : {-1, 0, 2, 4, 31, 35, 104, 108, 111, 113, 114, 116}) {
    MaturityFixture fixture;
    fixture.unchanged(id, true,
                      "unrelated upgrades never authorize orchard maturity");
  }
  for (const std::size_t incomplete : {std::size_t{0x20}, std::size_t{0x24}}) {
    MaturityFixture fixture;
    for (std::size_t i{}; i < 3; ++i) {
      maturity_put(fixture.plants, i * 0xd0 + 0x20, 1.0F);
      maturity_put(fixture.plants, i * 0xd0 + 0x24, 1.0F);
    }
    auto expected = fixture.plants;
    maturity_put(fixture.plants, 0xd0 + incomplete, 0.5F);
    maturity_expect(
        city_mature_new_orchard(fixture.building.data(), 33) &&
            fixture.plants == expected &&
            maturity_events == std::vector<int>{1, 2},
        "either incomplete planting work or growth triggers full maturity");
  }
  for (std::uint8_t crop : {std::uint8_t{0}, std::uint8_t{10}, std::uint8_t{11},
                            std::uint8_t{12}, std::uint8_t{15}}) {
    MaturityFixture fixture(crop);
    fixture.unchanged(
        112, true,
        "unselected orchard and ordinary seasonal crops remain unchanged");
  }
  for (std::int32_t id : {32, 34}) {
    MaturityFixture fixture(9);
    fixture.unchanged(
        id, true, "stale cultivar completion cannot mature a different crop");
  }
  {
    MaturityFixture fixture;
    maturity_put(fixture.owner, 0x34d, std::uint8_t{1});
    fixture.unchanged(33, true, "AI crops retain native maturation");
  }
  {
    MaturityFixture fixture;
    maturity_put(fixture.owner, 0x34c, std::uint8_t{0});
    fixture.unchanged(112, true, "non-player crops retain native maturation");
  }
  for (std::int32_t extension : {0, 1, 2, 5, 8, 14}) {
    MaturityFixture fixture;
    maturity_put(fixture.extensions, 0, extension);
    fixture.unchanged(112, true, "only actual Orchard extensions qualify");
  }
  {
    MaturityFixture fixture;
    maturity_put(fixture.building, 0xb88, CityArray{});
    fixture.unchanged(112, true, "no extension is a legitimate maturity no-op");
  }
  {
    MaturityFixture fixture;
    maturity_put(fixture.building, 0x530, CityArray{});
    fixture.unchanged(33, true,
                      "empty plant array succeeds without visual refresh");
  }
  {
    MaturityFixture fixture;
    configure_maturity({});
    fixture.unchanged(
        33, false,
        "missing native renderer prevents a partial maturity update");
  }
  {
    MaturityFixture fixture;
    configure_maturity({maturity_refresh, nullptr});
    fixture.unchanged(33, false,
                      "missing progress refresher prevents partial maturity");
  }
  for (const std::size_t field : {std::size_t{0x20}, std::size_t{0x24}})
    for (float invalid : {-0.1F, 1.01F, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
      MaturityFixture fixture;
      maturity_put(fixture.plants, 2 * 0xd0 + field, invalid);
      fixture.unchanged(
          33, false,
          "all records validate before any earlier growth is written");
    }
  {
    MaturityFixture fixture;
    maturity_put(fixture.plants, 2 * 0xd0, std::uint8_t{13});
    fixture.unchanged(33, false,
                      "mixed-crop plant arrays fail without partial writes");
  }
  for (bool extension : {false, true}) {
    for (int malformed = 0; malformed < 6; ++malformed) {
      MaturityFixture fixture;
      CityArray array{
          extension ? fixture.extensions.data() : fixture.plants.data(), 1, 1};
      if (malformed == 0)
        array.count = -1;
      else if (malformed == 1)
        array.capacity = 0;
      else if (malformed == 2)
        array.data = nullptr;
      else if (malformed == 3)
        array.data = reinterpret_cast<void *>(1);
      else if (malformed == 4)
        array.count = array.capacity = std::numeric_limits<std::int32_t>::max();
      else
        array.data = reinterpret_cast<void *>(
            std::numeric_limits<std::uintptr_t>::max() - 7);
      maturity_put(fixture.building, extension ? 0xb88 : 0x530, array);
      fixture.unchanged(33, false,
                        "malformed or unreadable array header fails closed");
    }
  }
  for (bool upgrading : {false, true}) {
    MaturityFixture fixture;
    maturity_put(fixture.building, upgrading ? 0x3a1 : 0x3c1,
                 static_cast<std::uint8_t>(upgrading));
    fixture.unchanged(
        33, false, "unfinished or upgrading orchard cannot be matured early");
  }
  {
    MaturityFixture fixture;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = static_cast<std::size_t>(info.dwPageSize);
    auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
        nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    maturity_expect(memory != nullptr && page >= 0xd0,
                    "allocate adjacent writable/read-only plant pages");
    if (memory != nullptr && page >= 0xd0) {
      auto *plants = memory + page - 0xd0;
      std::memcpy(plants, fixture.plants.data(), 0xd0 * 2);
      std::array<std::uint8_t, 0xd0 * 2> before{};
      std::memcpy(before.data(), plants, before.size());
      maturity_put(fixture.building, 0x530, CityArray{plants, 2, 2});
      DWORD old{};
      const bool protected_page =
          VirtualProtect(memory + page, page, PAGE_READONLY, &old) != 0;
      maturity_expect(protected_page,
                      "second plant page is readable but not writable");
      if (protected_page) {
        maturity_expect(!city_mature_new_orchard(fixture.building.data(), 33),
                        "writability precheck rejects later read-only Growth");
        maturity_expect(
            std::memcmp(plants, before.data(), before.size()) == 0 &&
                maturity_visual_calls == 0,
            "read-only later record leaves earlier writable Growth untouched");
      }
      maturity_expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
                      "synthetic plant pages released");
    } else if (memory != nullptr) {
      VirtualFree(memory, 0, MEM_RELEASE);
    }
  }
  configure_maturity({});
  city_image = previous_image;
  std::printf("maturity: %d checks, %d failures\n", maturity_checks,
              maturity_failures);
  return maturity_failures;
}

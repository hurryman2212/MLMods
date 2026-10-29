#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "HousePlacement.hpp"

struct TestHouseArray {
  std::uintptr_t data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct TestHouseVisualUpgrade {
  std::int32_t upgrade{};
  std::int32_t model{};
  std::uintptr_t actor{};
};

static std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
static std::uintptr_t next_address{}, denied_address{};
static std::array<int, 32> events;
static std::size_t event_count{}, cleanup_count{}, rebuild_count{},
    choose_count{};
static std::int32_t first_model{}, second_model{};
static float first_scale{}, second_scale{};
static std::uint8_t received_secondary{}, received_optional{};
static HouseOptionalModel *received_model{};
static bool throw_rebuild{}, reenter{}, reentered{};
static bool query_ready{}, recycle_cleanup{}, inspect_cleanup_choice{};
static std::size_t destroy_count{}, remove_count{};
static std::array<std::uintptr_t, 16> destroyed;
static std::array<std::uintptr_t, 32> removed_headers;
static std::uint8_t destroy_mode{};
static std::int32_t cleanup_choice{};
static std::uintptr_t field_actor{};

static auto test_can_rebuild(void *) noexcept -> bool { return query_ready; }

static auto test_is_field(void *actor) -> bool {
  return reinterpret_cast<std::uintptr_t>(actor) == field_actor;
}

static auto test_destroy(void *actor, std::uint8_t mode) -> void {
  destroyed[destroy_count++] = reinterpret_cast<std::uintptr_t>(actor);
  destroy_mode = mode;
  events[event_count++] = 3;
}

static auto test_remove(void *header, void *const *value) -> std::int32_t {
  const auto address = reinterpret_cast<std::uintptr_t>(header);
  removed_headers[remove_count++] = address;
  auto location = std::prev(memory.upper_bound(address));
  TestHouseArray array{};
  std::memcpy(&array, location->second.data() + address - location->first,
              sizeof(array));
  if (array.count == 0)
    return 0;
  auto storage = std::prev(memory.upper_bound(array.data));
  auto *bytes = storage->second.data() + array.data - storage->first;
  std::int32_t kept{};
  for (std::int32_t i{}; i < array.count; ++i) {
    std::uintptr_t actor{};
    std::memcpy(&actor, bytes + i * sizeof(actor), sizeof(actor));
    if (actor != reinterpret_cast<std::uintptr_t>(*value)) {
      std::memcpy(bytes + kept * sizeof(actor), &actor, sizeof(actor));
      ++kept;
    }
  }
  const auto removed = array.count - kept;
  array.count = kept;
  std::memcpy(location->second.data() + address - location->first, &array,
              sizeof(array));
  return removed;
}

static auto test_read(std::uintptr_t address, void *output,
                      std::size_t size) noexcept -> bool {
  if (address == denied_address) {
    return false;
  }
  auto found = memory.upper_bound(address);
  if (found == memory.begin()) {
    return false;
  }
  --found;
  const auto offset = address - found->first;
  if (offset > found->second.size() || size > found->second.size() - offset) {
    return false;
  }
  std::memcpy(output, found->second.data() + offset, size);
  return true;
}

static auto test_cleanup_module(void *actor) -> void {
  ++cleanup_count;
  events[event_count++] = 1;
  std::uintptr_t owner{};
  test_read(reinterpret_cast<std::uintptr_t>(actor) + 0x2c8, &owner,
            sizeof(owner));
  if (inspect_cleanup_choice) {
    float scale{};
    cleanup_choice = house_placement_choose_primary(
        reinterpret_cast<void *>(owner), 550, &scale, 4, 0);
  }
  if (recycle_cleanup) {
    memory.at(owner)[12] = 99;
  }
}

static auto test_choose(void *, float width, float *scale, std::int32_t level,
                        std::uint8_t variant) -> std::int32_t {
  ++choose_count;
  if (scale != nullptr) {
    *scale = width / 500.0F;
  }
  return 800 + level + variant;
}

static auto test_rebuild(void *owner, std::int32_t level, std::uint8_t primary,
                         std::uint8_t secondary, HouseOptionalModel *model)
    -> void {
  ++rebuild_count;
  events[event_count++] = 2;
  received_model = model;
  received_secondary = secondary;
  HouseOptionalModel argument{};
  if (!test_read(reinterpret_cast<std::uintptr_t>(model), &argument,
                 sizeof(argument))) {
    argument = *model;
  }
  received_optional = argument.valid;
  if (throw_rebuild) {
    throw std::runtime_error("native rebuild failure");
  }
  if (reenter && !reentered) {
    reentered = true;
    house_placement_rebuild(owner, level, primary, secondary, model);
  }
  first_model =
      house_placement_choose_primary(owner, 550, &first_scale, level, 0);
  if (secondary != 0) {
    second_model =
        house_placement_choose_secondary(owner, 450, &second_scale, level, 1);
  }
}

struct HouseFixture {
  std::uintptr_t object_class{}, manager{}, owner{}, model{}, secondary{},
      extension{}, decoration{}, extension_records{};

  auto allocate() -> std::uintptr_t {
    const auto result = next_address;
    next_address += 0x10000;
    memory[result].resize(0x2000);
    return result;
  }
  template <class T> auto put(std::uintptr_t address, const T &value) -> void {
    auto found = std::prev(memory.upper_bound(address));
    std::memcpy(found->second.data() + address - found->first, &value,
                sizeof(value));
  }
  auto object() -> std::uintptr_t {
    const auto result = allocate();
    put(result, std::uintptr_t{0x140001000});
    put(result + 12, std::int32_t{1});
    put(result + 16, object_class);
    return result;
  }
  template <class T>
  auto array(const std::vector<T> &values) -> TestHouseArray {
    const auto address = allocate();
    for (std::size_t i{}; i < values.size(); ++i) {
      put(address + i * sizeof(T), values[i]);
    }
    return {address, static_cast<std::int32_t>(values.size()),
            static_cast<std::int32_t>(values.size())};
  }
  HouseFixture() {
    memory.clear();
    next_address = 0x200000000;
    denied_address = 0;
    event_count = cleanup_count = rebuild_count = choose_count = 0;
    throw_rebuild = reenter = reentered = false;
    query_ready = true;
    recycle_cleanup = inspect_cleanup_choice = false;
    destroy_count = remove_count = 0;
    field_actor = 0;
    first_model = second_model = 0;
    first_scale = second_scale = 0;
    object_class = allocate();
    put(object_class, std::uintptr_t{0x140001000});
    manager = object();
    put(manager + 0x318, std::uint8_t{1});
    owner = object();
    model = allocate();
    secondary = object();
    extension = object();
    decoration = object();
    for (const auto actor : {secondary, extension, decoration}) {
      put(actor + 0x2c8, owner);
    }
    extension_records = allocate();
    put(owner + 0x2e8, manager);
    put(owner + 0x3b8, std::int32_t{484});
    put(owner + 0xd48, std::int32_t{4});
    put(owner + 0x890, std::int32_t{461});
    put(owner + 0x894, std::int32_t{462});
    put(owner + 0x3a1, std::uint8_t{1});
    put(model, HouseOptionalModel{461, 1, {}});
    put(secondary + 0x2c0, std::int32_t{462});
    put(extension + 0x2c0, std::int32_t{521});
    put(owner + 0x2f8, array(std::vector{secondary, extension, decoration}));
    put(owner + 0x430, array(std::vector<std::int32_t>{5, 12, 105}));
    put(owner + 0xb88, TestHouseArray{extension_records, 1, 1});
    put(extension_records + 0x70, array(std::vector{extension}));
    configure_house_placement_api(
        {test_read, test_rebuild, test_choose, 0x140000000, test_can_rebuild,
         test_destroy, test_remove, test_is_field, test_cleanup_module});
    set_house_placement_enabled(true);
  }
  auto run() -> void {
    house_placement_rebuild(reinterpret_cast<void *>(owner), 4, 1, 0,
                            reinterpret_cast<HouseOptionalModel *>(model));
  }
};

auto main() -> int {
  int checks{}, failures{};
  const auto expect = [&](bool value, const char *message) {
    ++checks;
    if (!value) {
      ++failures;
      std::fprintf(stderr, "house placement: %s\n", message);
    }
  };
  {
    HouseFixture fixture;
    const auto original = memory;
    fixture.run();
    expect(
        cleanup_count == 2 && destroy_count == 2 && rebuild_count == 1 &&
            event_count == 5 && events[0] == 3 && events[1] == 1 &&
            events[4] == 2,
        "validated artisan repair performs cleanup before one native rebuild");
    expect(
        destroy_mode == 0 && received_secondary == 1 && received_optional == 0,
        "native cleanup retains extensions and rebuild requests both houses");
    expect(first_model == 461 && second_model == 462 && first_scale == 1.1F &&
               second_scale == 1.0F && choose_count == 1,
           "scoped choices retain primary and secondary IDs with separate "
           "scale policies");
    expect(
        memory.at(fixture.extension) == original.at(fixture.extension) &&
            memory.at(fixture.extension_records) ==
                original.at(fixture.extension_records) &&
            house_placement_repair_count() == 1,
        "native selective cleanup leaves extension actors and registry intact");
    float scale{};
    expect(
        house_placement_choose_primary(reinterpret_cast<void *>(fixture.owner),
                                       300, &scale, 2, 1) == 803 &&
            scale == 0.6F,
        "primary chooser outside repair uses original arguments and result");
    expect(house_placement_choose_secondary(
               reinterpret_cast<void *>(fixture.owner), 250, &scale, 3, 0) ==
                   803 &&
               scale == 0.5F,
           "secondary chooser outside repair forwards to original");
    fixture.run();
    expect(
        cleanup_count == 2 && rebuild_count == 2 &&
            house_placement_repair_count() == 2,
        "independent later artisan requests receive separate scoped repairs");
  }
  for (int mode{}; mode < 13; ++mode) {
    HouseFixture fixture;
    if (mode == 0)
      set_house_placement_enabled(false);
    else if (mode == 1)
      fixture.put(fixture.owner + 0x3b8, std::int32_t{60});
    else if (mode == 2)
      fixture.put(fixture.owner + 0xd48, std::int32_t{3});
    else if (mode == 3)
      fixture.put(fixture.owner + 0x3c1, std::uint8_t{1});
    else if (mode == 4)
      fixture.put(fixture.owner + 0x3a1, std::uint8_t{0});
    else if (mode == 5)
      fixture.put(fixture.model, HouseOptionalModel{461, 0, {}});
    else if (mode == 6)
      fixture.put(fixture.owner + 0x890, std::int32_t{462});
    else if (mode == 7)
      fixture.put(fixture.owner + 0x894, std::int32_t{0});
    else if (mode == 8)
      denied_address = fixture.extension_records + 0x70;
    else if (mode == 9)
      fixture.put(fixture.owner + 0xb88, TestHouseArray{0, 1, 0});
    else if (mode == 10)
      fixture.put(fixture.owner + 0x430, TestHouseArray{0, -1, 0});
    else if (mode == 11)
      fixture.put(fixture.secondary + 8, std::uint32_t{0x40000000});
    else
      fixture.put(
          fixture.extension_records + 0x70,
          fixture.array(std::vector{fixture.extension, fixture.secondary}));
    const auto original = memory;
    fixture.run();
    expect(
        cleanup_count == 0 && rebuild_count == 1 &&
            received_model ==
                reinterpret_cast<HouseOptionalModel *>(fixture.model) &&
            received_secondary == 0 &&
            received_optional == (mode == 5 ? 0 : 1) && memory == original,
        "unsupported or unsafe repair forwards original call without cleanup");
  }
  {
    HouseFixture fixture;
    fixture.put(fixture.owner + 0x430,
                fixture.array(std::vector<std::int32_t>{105}));
    fixture.run();
    expect(cleanup_count == 2 && received_secondary == 0 && second_model == 0,
           "single-house plot does not invent a second household");
  }
  {
    HouseFixture fixture;
    throw_rebuild = true;
    bool caught{};
    try {
      fixture.run();
    } catch (const std::runtime_error &) {
      caught = true;
    }
    float scale{};
    expect(caught && cleanup_count == 2 && rebuild_count == 1 &&
               house_placement_repair_count() == 0 &&
               house_placement_choose_primary(
                   reinterpret_cast<void *>(fixture.owner), 300, &scale, 4,
                   0) == 804,
           "native exception propagates without a second rebuild and unwinds "
           "chooser scope");
  }
  {
    HouseFixture fixture;
    reenter = true;
    fixture.run();
    expect(
        reentered && cleanup_count == 2 && rebuild_count == 2 &&
            house_placement_repair_count() == 1,
        "nested same-owner native request cannot recursively clean geometry");
  }
  {
    HouseFixture fixture;
    const auto house_resource = fixture.object();
    const auto extension_resource = fixture.object();
    fixture.put(house_resource + 0x2c8, fixture.owner);
    fixture.put(extension_resource + 0x2c8, fixture.owner);
    fixture.put(fixture.owner + 0x308,
                fixture.array(std::vector{house_resource, extension_resource,
                                          house_resource}));
    fixture.put(
        fixture.owner + 0x2f8,
        fixture.array(std::vector{fixture.secondary, fixture.extension,
                                  fixture.decoration, fixture.decoration}));
    fixture.put(
        fixture.owner + 0x318,
        fixture.array(std::vector{fixture.secondary, fixture.extension}));
    fixture.put(
        fixture.extension_records + 0x70,
        fixture.array(std::vector{fixture.extension, extension_resource}));
    fixture.put(fixture.owner + 0x370,
                fixture.array(std::vector<TestHouseVisualUpgrade>{
                    {37, 475, fixture.extension}}));
    const auto original_extension = memory.at(fixture.extension);
    const auto original_resource = memory.at(extension_resource);
    const auto original_registry = memory.at(fixture.extension_records);
    fixture.run();
    TestHouseArray resources{}, modules{}, auxiliary{};
    test_read(fixture.owner + 0x308, &resources, sizeof(resources));
    test_read(fixture.owner + 0x2f8, &modules, sizeof(modules));
    test_read(fixture.owner + 0x318, &auxiliary, sizeof(auxiliary));
    std::uintptr_t retained_resource{}, retained_module{}, retained_auxiliary{};
    test_read(resources.data, &retained_resource, sizeof(retained_resource));
    test_read(modules.data, &retained_module, sizeof(retained_module));
    test_read(auxiliary.data, &retained_auxiliary, sizeof(retained_auxiliary));
    expect(destroy_count == 3 && cleanup_count == 2 && remove_count == 8 &&
               destroyed[0] == house_resource &&
               destroyed[1] == fixture.secondary &&
               destroyed[2] == fixture.decoration && destroy_mode == 0,
           "selective native destruction classifies and deduplicates house "
           "resources and modules");
    expect(resources.count == 1 && retained_resource == extension_resource &&
               modules.count == 1 && retained_module == fixture.extension &&
               auxiliary.count == 1 && retained_auxiliary == fixture.extension,
           "native pointer-to-value removal preserves retained entries and "
           "removes all duplicates");
    expect(memory.at(fixture.extension) == original_extension &&
               memory.at(extension_resource) == original_resource &&
               memory.at(fixture.extension_records) == original_registry,
           "retained extension resource, mesh and registration receive no "
           "cleanup calls or writes");
  }
  {
    HouseFixture fixture;
    field_actor = fixture.decoration;
    fixture.put(fixture.owner + 0x370,
                fixture.array(std::vector<TestHouseVisualUpgrade>{
                    {37, 475, field_actor}}));
    fixture.run();
    expect(destroy_count == 1 && destroyed[0] == fixture.secondary &&
               cleanup_count == 1,
           "native fieldBuilding exclusion and its cached visual references "
           "are preserved");
  }
  for (int fault{}; fault < 13; ++fault) {
    HouseFixture fixture;
    if (fault == 0)
      query_ready = false;
    else if (fault == 1)
      fixture.put(fixture.manager + 0x318, std::uint8_t{0});
    else if (fault >= 2 && fault <= 4)
      fixture.put(fixture.owner + 0x3a2 + static_cast<std::size_t>(fault - 2),
                  std::uint8_t{1});
    else if (fault == 5)
      fixture.put(fixture.owner + 0x338,
                  fixture.array(std::vector{fixture.decoration}));
    else if (fault == 6)
      fixture.put(fixture.owner + 0x3a8,
                  fixture.array(std::vector{fixture.decoration}));
    else if (fault == 7)
      fixture.put(fixture.owner + 0x6f0,
                  fixture.array(std::vector{fixture.decoration}));
    else if (fault == 8)
      fixture.put(fixture.owner + 0x370,
                  fixture.array(std::vector<TestHouseVisualUpgrade>{
                      {37, 475, fixture.secondary}}));
    else if (fault == 9)
      fixture.put(fixture.decoration + 0x2c8, fixture.manager);
    else if (fault == 10)
      fixture.put(fixture.owner + 0x308,
                  fixture.array(std::vector{fixture.decoration}));
    else if (fault == 11)
      fixture.put(fixture.owner + 0x318,
                  fixture.array(std::vector{fixture.manager}));
    else
      denied_address = fixture.owner + 0x308;
    const auto original = memory;
    fixture.run();
    expect(destroy_count == 0 && cleanup_count == 0 && remove_count == 0 &&
               rebuild_count == 1 && received_optional == 1 &&
               memory == original && house_placement_skip_count() == 1,
           "unproven lifecycle, ownership, cache or actor classification "
           "forwards original before destruction");
  }
  {
    HouseFixture fixture;
    inspect_cleanup_choice = true;
    fixture.run();
    expect(cleanup_choice == 804 && first_model == 461 && second_model == 462,
           "chooser overrides are active only inside the native generator, "
           "never cleanup callbacks");
  }
  {
    HouseFixture fixture;
    const auto proxy = fixture.object();
    fixture.put(proxy + 0x2b0, fixture.manager);
    // Native 4c900c0 appends this proxy before 4ccc7c0 destroys the old main
    // and reaches our CALL. Its represented actor is intentionally unavailable.
    fixture.put(proxy + 0x2a8, std::uintptr_t{0x12340000});
    fixture.put(fixture.owner + 0x3a8, fixture.array(std::vector{proxy}));
    const auto original_proxy = memory.at(proxy);
    TestHouseArray helpers_before{}, helpers_after{};
    test_read(fixture.owner + 0x3a8, &helpers_before, sizeof(helpers_before));
    const auto original_helper_list = memory.at(helpers_before.data);
    fixture.run();
    test_read(fixture.owner + 0x3a8, &helpers_after, sizeof(helpers_after));
    expect(house_placement_repair_count() == 1 &&
               house_placement_skip_count() == 0 && destroy_count == 2 &&
               destroyed[0] != proxy && destroyed[1] != proxy &&
               std::memcmp(&helpers_before, &helpers_after,
                           sizeof(helpers_before)) == 0 &&
               memory.at(proxy) == original_proxy &&
               memory.at(helpers_before.data) == original_helper_list,
           "native pre-finish construction proxy survives selective geometry "
           "repair with stale old-main reference intact");
  }
  for (int fault{}; fault < 4; ++fault) {
    HouseFixture fixture;
    const auto proxy = fixture.object();
    fixture.put(proxy + 0x2b0, fixture.manager);
    fixture.put(proxy + 0x2a8, std::uintptr_t{0x12340000});
    fixture.put(fixture.owner + 0x3a8, fixture.array(std::vector{proxy}));
    if (fault == 0)
      fixture.put(proxy + 0x2b0, fixture.owner);
    else if (fault == 1)
      fixture.put(proxy + 0x2a8, fixture.secondary);
    else if (fault == 2)
      fixture.put(proxy + 8, std::uint32_t{0x40000000});
    else
      denied_address = proxy + 0x2a8;
    const auto original = memory;
    fixture.run();
    const auto diagnostic = house_placement_diagnostic();
    expect(destroy_count == 0 && rebuild_count == 1 && memory == original &&
               diagnostic.owner == fixture.owner &&
               diagnostic.failure == HousePlacementFailure::HelperArray &&
               std::wcscmp(house_placement_failure_name(diagnostic.failure),
                           L"helper_array") == 0,
           "invalid or ambiguous proxy rejects repair and identifies the "
           "helper validation stage");
  }
  {
    HouseFixture fixture;
    query_ready = false;
    fixture.run();
    expect(house_placement_diagnostic().failure ==
               HousePlacementFailure::AssemblyBarrier,
           "unready CityEditor barrier reports its own diagnostic stage");
    query_ready = true;
    fixture.put(fixture.owner + 0x890, std::int32_t{999});
    fixture.run();
    expect(house_placement_diagnostic().failure ==
               HousePlacementFailure::NativeContext,
           "native callback model mismatch is distinguished from assembly "
           "readiness");
  }
  {
    HouseFixture fixture;
    recycle_cleanup = true;
    fixture.run();
    expect(destroy_count == 1 && cleanup_count == 1 && rebuild_count == 0 &&
               house_placement_skip_count() == 1 &&
               house_placement_repair_count() == 0,
           "owner identity change after native destruction stops without "
           "fallback regeneration");
  }
  std::printf("house placement: %d checks, %d failures\n", checks, failures);
  return failures;
}

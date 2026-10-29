#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "ExtensionCancel.hpp"
#include "Upgrade.hpp"

static std::size_t extension_cancel_checks{};

static auto cancel_check(bool passed, const char *message) -> void {
  ++extension_cancel_checks;
  if (!passed)
    throw std::runtime_error(message);
}

template <std::size_t N> struct CancelMemory {
  alignas(16) std::array<std::uint8_t, N> bytes{};

  template <typename T> auto put(std::size_t offset, T value) -> void {
    if (offset + sizeof(value) > N)
      throw std::runtime_error("cancel fixture write exceeds allocation");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  template <typename T> auto get(std::size_t offset) const -> T {
    if (offset + sizeof(T) > N)
      throw std::runtime_error("cancel fixture read exceeds allocation");
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }

  auto pointer() -> void * { return bytes.data(); }
};

struct CancelUpgradeVisual {
  std::int32_t upgrade{}, mesh{};
  void *module{};
};

static_assert(sizeof(CancelUpgradeVisual) == 16);

struct CancelFixture {
  CancelMemory<0x1600> building, pawn, master, region, other;
  CancelMemory<0x340> module, resource_module, replacement_resource;
  CancelMemory<0x30> object_class;
  CancelMemory<0xd0> extension;
  CancelMemory<0x2a0> building_stat;
  CancelMemory<0x158> production;
  std::array<CancelMemory<0x20>, 3> production_slots;
  std::array<std::array<std::int64_t, 2>, 2> production_records{
      {{123, 7}, {124, 9}}};
  std::size_t production_resets{}, production_frees{};
  // Native Region.familyData uses an 0x50-byte record, not a pointer array.
  std::array<CancelMemory<0x50>, 3> families;
  std::array<CancelMemory<0x400>, 3> members;
  std::array<std::array<void *, 1>, 3> member_lists{};
  std::array<void *, 3> modules{};
  std::array<void *, 2> resource_modules{}, extension_modules{};
  std::array<std::int32_t, 3> workers{0, 1, 0x12345678};
  std::array<std::int32_t, 3> residents{0, 1, 0x12345678};
  std::array<std::int32_t, 4> recipes{};
  std::array<std::int32_t, 12> completed_upgrades{};
  std::array<CancelUpgradeVisual, 3> upgrade_visuals{};
  std::array<void *, 4> fuel_buildings{};
  std::array<void *, 8> grown_fuel_buildings{};
  std::array<std::uint8_t, 64> inventory{};
  std::unordered_map<std::uint64_t, void *> weak_objects;
  std::uint64_t next_weak{1};
  std::vector<std::int32_t> releases;
  std::vector<std::int32_t> disabled_recipes;
  std::vector<std::int32_t> removed_upgrades;
  std::function<void()> artisan_entry_body, artisan_body, artisan_callback_body,
      assembly_body, finish_body, disable_recipe_body, selection_body,
      upgrade_visual_body;
  std::function<void(std::int32_t)> begin_body;
  std::size_t artisan_calls{}, extension_calls{}, begin_calls{}, finishes{};
  std::size_t requests{}, callbacks{}, mature_calls{};
  std::size_t fuel_removals{}, resource_refreshes{}, region_refreshes{};
  std::size_t fuel_grows{}, upgrade_visual_calls{};
  void *upgrade_visual_module{};
  std::vector<std::int32_t> stat_types;
  std::int32_t last_type{-1}, last_index{-1};
  bool native_active{}, native_changes{true}, asynchronous{};
  bool native_recipe_changes{true};
  bool native_upgrade_changes{true}, upgrade_remove_throws{};
  bool native_selection_upgrade{};
  bool missing_stat{};
  bool grow_throws{}, native_grow_changes{true};
  float work_added{};

  CancelFixture() {
    object_class.put(0, city_image + 0x1000);
    std::int32_t object_index = 1;
    const auto initialize = [&](auto &object) {
      object.put(0, city_image + 0x2000);
      object.put(12, object_index++);
      object.put(16, object_class.pointer());
    };
    for (auto *object : {&building, &pawn, &master, &region, &other})
      initialize(*object);
    initialize(module);
    initialize(resource_module);
    initialize(replacement_resource);
    initialize(production);
    production.put(0x90, building.pointer());
    production.put<std::int64_t>(0xa0, 12345);
    production.put<std::int32_t>(0xf4, 128);
    production.put<std::int32_t>(0xf8, -1);
    production.put<std::uint64_t>(0x120, 0xfeedf00d);
    building.put(0x360, production.pointer());
    pawn.put<std::uint8_t>(0x34c, 1);
    pawn.put(0x340, master.pointer());
    master.put<std::uint8_t>(0x318, 1);
    master.put(0x5b0, pawn.pointer());
    region.put(0x318, master.pointer());
    region.put(0x350, pawn.pointer());
    building.put(0x2c8, region.pointer());
    building.put(0x2e0, pawn.pointer());
    building.put(0x2e8, master.pointer());
    building.put<std::int32_t>(0x3b8, 8);
    building.put<std::uint8_t>(0x3c1, 1);
    building.put<std::uint8_t>(0x3cc, 1);
    building.put<std::int32_t>(0xd48, 2);
    building.put<std::int32_t>(0x14c4, 16);
    building.put(0x14c8, CityArray{recipes.data(), 0, 4});
    building.put(0x430, CityArray{completed_upgrades.data(), 0, 12});
    building.put(0x370, CityArray{upgrade_visuals.data(), 0, 3});
    building.put<std::int32_t>(0xe74, -1);
    building.put<std::uint8_t>(0xe71, 1);
    inventory.fill(0x5a);
    building.put(0x448, CityArray{inventory.data(), 2, 2});
    building.put(0x100, std::uint64_t{0xaabbccdd11223344});
    building.put(0x480, std::uint64_t{0xdeadbeef12345678});
    building.put(0xb88, CityArray{extension.pointer(), 1, 1});
    extension.put<std::int32_t>(0, 3);
    modules[0] = module.pointer();
    building.put(0x2f8, CityArray{modules.data(), 1, 1});
    module.put(0x2c8, building.pointer());
    resource_module.put(0x2c8, building.pointer());
    replacement_resource.put(0x2c8, building.pointer());
    upgrade_visual_module = module.pointer();
    module.put<float>(0x314, 2.0F);
    module.put<float>(0x318, 10.0F);
    building.put(0x1028, CityArray{workers.data(), 2, 3});
    building.put(0x1038, CityArray{residents.data(), 2, 3});
    region.put(0x4e8, CityArray{families.data(), 3, 3});
    fuel_buildings = {other.pointer(), building.pointer(), nullptr, nullptr};
    region.put(0x708, CityArray{fuel_buildings.data(), 2, 4});
    for (std::size_t i{}; i < families.size(); ++i) {
      initialize(members[i]);
      members[i].put(0x348, static_cast<std::int32_t>(i));
      member_lists[i][0] = members[i].pointer();
      families[i].put(0, CityArray{member_lists[i].data(), 1, 1});
      void *home = i < 2 ? building.pointer() : other.pointer();
      members[i].put(0x320, pawn.pointer());
      members[i].put(0x328, region.pointer());
      members[i].put(0x338, home);
      members[i].put(0x340, home);
      families[i].put(0x10, home);
      families[i].put(0x20, home);
      families[i].put(0x30, std::uint64_t{0x1122334455667788});
    }
  }

  auto state() const -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> result;
    const auto append = [&](const auto &value) {
      const auto *data = reinterpret_cast<const std::uint8_t *>(&value);
      result.insert(result.end(), data, data + sizeof(value));
    };
    for (const auto *object : {&building, &pawn, &master, &region, &other})
      append(object->bytes);
    append(module.bytes);
    append(resource_module.bytes);
    append(replacement_resource.bytes);
    append(production.bytes);
    append(production_slots);
    append(production_records);
    append(extension.bytes);
    append(families);
    append(members);
    append(member_lists);
    append(modules);
    append(resource_modules);
    append(extension_modules);
    append(workers);
    append(residents);
    append(recipes);
    append(completed_upgrades);
    append(upgrade_visuals);
    append(fuel_buildings);
    append(grown_fuel_buildings);
    append(inventory);
    return result;
  }

  auto set_workshop_settings() -> void {
    building.put<std::uint8_t>(0xe71, 1);
    building.put<std::uint8_t>(0xe72, 1);
    building.put<std::int32_t>(0xe74, 72);
    building.put<std::uint8_t>(0xe78, 5);
    recipes = {17, 3, 25, 17};
    building.put(0x14c8, CityArray{recipes.data(), 4, 4});
    fuel_buildings = {other.pointer(), building.pointer(), building.pointer(),
                      nullptr};
    region.put(0x708, CityArray{fuel_buildings.data(), 3, 4});
  }

  auto set_workshop_upgrades() -> void {
    // House levels, residential expansion and unrelated upgrades surround
    // duplicates of all three artisan-only upgrades, including stale ones.
    completed_upgrades = {2, 37, 4, 42, 12, 43, 5, 42, 31, 37, 43, -1};
    building.put(0x430, CityArray{completed_upgrades.data(), 11, 12});
  }

  auto set_workshop_visuals() -> void {
    upgrade_visuals = {{{37, 5, resource_module.pointer()},
                        {37, 7, module.pointer()},
                        {0x12345678, 0x23456789, nullptr}}};
    building.put(0x370, CityArray{upgrade_visuals.data(), 2, 3});
  }

  auto upgrades_cleared() const -> bool {
    const auto upgrades = building.get<CityArray>(0x430);
    constexpr std::array<std::int32_t, 5> retained{2, 4, 12, 5, 31};
    return upgrades.data == completed_upgrades.data() && upgrades.count == 5 &&
           upgrades.capacity == 12 &&
           std::equal(retained.begin(), retained.end(),
                      completed_upgrades.begin()) &&
           completed_upgrades.back() == -1;
  }

  auto settings_cleared() -> bool {
    const auto type = building.get<std::int32_t>(0x3b8);
    const bool needs_fuel = type == 8 || type == 60 || type == 484 ||
                            building.get<std::uint8_t>(0x3cc) == 2;
    const auto registered = region.get<CityArray>(0x708);
    const auto *entries = static_cast<void **>(registered.data);
    return building.get<std::uint8_t>(0xe71) == needs_fuel &&
           building.get<std::uint8_t>(0xe72) == 0 &&
           building.get<std::int32_t>(0xe74) == -1 &&
           building.get<std::uint8_t>(0xe78) == 0 &&
           building.get<CityArray>(0x14c8).count == 0 &&
           registered.count == (needs_fuel ? 2 : 1) &&
           entries[0] == other.pointer() &&
           (!needs_fuel || entries[1] == building.pointer());
  }
};

static_assert(sizeof(CancelMemory<0x50>) == 0x50);
static CancelFixture *cancel_fixture{};

static auto cancel_mock_assign(std::uint64_t *weak, void *object) -> void {
  auto &f = *cancel_fixture;
  for (const auto &[identity, found] : f.weak_objects)
    if (found == object) {
      *weak = identity;
      return;
    }
  *weak = f.next_weak++;
  f.weak_objects[*weak] = object;
}

static auto cancel_mock_get(const std::uint64_t *weak) -> void * {
  const auto found = cancel_fixture->weak_objects.find(*weak);
  return found == cancel_fixture->weak_objects.end() ? nullptr : found->second;
}

static auto cancel_mock_extension(void *building, std::int32_t index,
                                  std::int32_t type) -> void {
  auto &f = *cancel_fixture;
  ++f.extension_calls;
  f.last_index = index;
  f.last_type = type;
  cancel_check(building == f.building.pointer(),
               "native extension receives the original building");
  if (f.native_changes && index == 0 && type == 0) {
    const auto extensions = f.building.get<CityArray>(0xb88);
    CityArray extension_modules{};
    std::memcpy(&extension_modules,
                static_cast<std::uint8_t *>(extensions.data) + 0x70,
                sizeof(extension_modules));
    const auto *extension_objects =
        static_cast<void **>(extension_modules.data);
    for (std::int32_t i{}; i < extension_modules.count; ++i) {
      void *module = extension_objects[i];
      // Native changeExtension removes an extension's actors from both the
      // main module registry and its separate resource-module registry.
      for (const std::size_t offset : {0x2f8U, 0x308U}) {
        auto registry = f.building.get<CityArray>(offset);
        if (registry.count > 0) {
          auto *objects = static_cast<void **>(registry.data);
          const auto *end =
              std::remove(objects, objects + registry.count, module);
          registry.count = static_cast<std::int32_t>(end - objects);
          f.building.put(offset, registry);
        }
      }
      const std::uint32_t destroyed = 0x40000000;
      std::memcpy(static_cast<std::uint8_t *>(module) + 8, &destroyed,
                  sizeof(destroyed));
    }
    extension_modules.count = 0;
    std::memcpy(static_cast<std::uint8_t *>(extensions.data) + 0x70,
                &extension_modules, sizeof(extension_modules));
    std::memcpy(extensions.data, &type, sizeof(type));
  }
}

static auto cancel_mock_artisan(void *building, std::int32_t type) -> void {
  auto &f = *cancel_fixture;
  ++f.artisan_calls;
  constexpr std::array<std::int32_t, 17> artisan_jobs{
      0, 0, 0, 16, 38, 22, 10, 11, 0, 20, 56, 57, 0, 29, 0, 71, 74};
  cancel_check(building == f.building.pointer() && type >= 0 &&
                   type < static_cast<std::int32_t>(artisan_jobs.size()) &&
                   (type == 0 || artisan_jobs[type] != 0),
               "artisan dispatch forwards cancellation or a supported workshop "
               "to native");
  if (type != 0) {
    // The native artisan switch changes occupation/recipes but does not
    // reinitialize the house's derived needsRefueling flag. This is the path
    // on which an incorrect false value from cancellation hid the fuel UI.
    f.building.put<std::int32_t>(0x14c4, artisan_jobs[type]);
    f.extension.put<std::int32_t>(0, type);
    f.recipes[0] = 25;
    f.building.put(0x14c8, CityArray{f.recipes.data(), 1, 4});
    // Native ChangeArtisanType reassigns every resident with 0x4c3b790 before
    // returning. Restore those relationships for a second cancellation.
    for (std::size_t i{}; i < 2; ++i) {
      f.families[i].put(0x10, building);
      f.members[i].put(0x338, building);
      f.workers[i] = static_cast<std::int32_t>(i);
    }
    f.building.put(0x1028, CityArray{f.workers.data(), 2, 3});
    if (f.native_selection_upgrade) {
      city_begin_upgrade(building, 103);
      city_upgrade_cost_setup(building, 103, 1);
      f.module.put<std::uint8_t>(0x311, 1);
      f.building.put<std::uint8_t>(0x3c1, 0);
      f.building.put<std::uint8_t>(0x3a1, 1);
    }
    if (f.selection_body)
      f.selection_body();
    return;
  }
  if (f.artisan_entry_body)
    f.artisan_entry_body();
  f.native_active = true;
  if (f.native_changes) {
    f.building.put<std::int32_t>(0x14c4, 0);
    city_change_extension(building, 0, 0);
    if (f.asynchronous) {
      f.module.put<std::uint8_t>(0x311, 1);
      f.building.put<std::uint8_t>(0x3c1, 0);
      f.building.put<std::uint8_t>(0x3a1, 1);
    }
  }
  if (f.artisan_body)
    f.artisan_body();
  f.native_active = false;
}

static auto cancel_mock_release(void *region, std::int32_t family) -> void {
  auto &f = *cancel_fixture;
  cancel_check(region == f.region.pointer() && family >= 0 && family < 2,
               "native profession release targets only old resident workers");
  cancel_check(!f.native_active,
               "profession release waits for native artisan call to return");
  f.releases.push_back(family);
  f.families[static_cast<std::size_t>(family)].put<void *>(0x10, nullptr);
  auto workers = f.building.get<CityArray>(0x1028);
  auto *ids = static_cast<std::int32_t *>(workers.data);
  // Native int-array removal 0x4af7430 removes every matching ID. The live
  // worker registry may repeat an otherwise valid resident family.
  const auto *end = std::remove(ids, ids + workers.count, family);
  workers.count = static_cast<std::int32_t>(end - ids);
  f.building.put(0x1028, workers);
}

static auto cancel_mock_begin(void *, std::int32_t upgrade) -> void {
  auto &f = *cancel_fixture;
  ++f.begin_calls;
  if (f.begin_body)
    f.begin_body(upgrade);
}

static auto cancel_mock_disable_recipe(void *building, std::int32_t recipe)
    -> void {
  auto &f = *cancel_fixture;
  cancel_check(building == f.building.pointer() && !f.native_active &&
                   f.building.get<std::int32_t>(0x14c4) == 0 &&
                   f.extension.get<std::int32_t>(0) == 0,
               "native recipe disable targets the cancelled workshop after "
               "artisan return");
  f.disabled_recipes.push_back(recipe);
  if (!f.native_recipe_changes)
    return;
  auto recipes = f.building.get<CityArray>(0x14c8);
  auto *ids = static_cast<std::int32_t *>(recipes.data);
  const auto *end = std::remove(ids, ids + recipes.count, recipe);
  recipes.count = static_cast<std::int32_t>(end - ids);
  f.building.put(0x14c8, recipes);
  if (f.disable_recipe_body)
    f.disable_recipe_body();
}

static auto cancel_mock_remove_upgrade(void *array, const std::int32_t *upgrade)
    -> std::int32_t {
  auto &f = *cancel_fixture;
  cancel_check(array == f.building.bytes.data() + 0x430 && upgrade &&
                   (*upgrade == 37 || *upgrade == 42 || *upgrade == 43) &&
                   !f.native_active,
               "native upgrade removal targets only recognized workshop IDs "
               "outside the artisan call");
  f.removed_upgrades.push_back(*upgrade);
  if (f.upgrade_remove_throws)
    throw std::runtime_error("synthetic native upgrade removal failure");
  if (!f.native_upgrade_changes)
    return 0;
  auto upgrades = f.building.get<CityArray>(0x430);
  auto *ids = static_cast<std::int32_t *>(upgrades.data);
  const auto *end = std::remove(ids, ids + upgrades.count, *upgrade);
  const auto new_count = static_cast<std::int32_t>(end - ids);
  const auto removed = upgrades.count - new_count;
  upgrades.count = new_count;
  f.building.put(0x430, upgrades);
  return removed;
}

static auto cancel_mock_upgrade_visuals(void *building) -> void {
  auto &f = *cancel_fixture;
  ++f.upgrade_visual_calls;
  const auto upgrades = f.building.get<CityArray>(0x430);
  const auto *ids = static_cast<std::int32_t *>(upgrades.data);
  auto visuals = f.building.get<CityArray>(0x370);
  cancel_check(building == f.building.pointer() && !f.native_active &&
                   f.extension.get<std::int32_t>(0) == 5 &&
                   f.building.get<std::int32_t>(0x14c4) == 22 &&
                   std::find(ids, ids + upgrades.count, 37) ==
                       ids + upgrades.count &&
                   visuals.count == 0,
               "native kettle visual rebuild runs on an idle brewery with "
               "empty cache before the kettle upgrade is purchased");
  if (f.upgrade_visual_body) {
    f.upgrade_visual_body();
    return;
  }
  if (visuals.capacity == 0) {
    visuals.data = f.upgrade_visuals.data();
    visuals.capacity = static_cast<std::int32_t>(f.upgrade_visuals.size());
  }
  static_cast<CancelUpgradeVisual *>(visuals.data)[0] = {
      37, 5, f.upgrade_visual_module};
  visuals.count = 1;
  f.building.put(0x370, visuals);
}

static auto cancel_mock_remove_pointer(void *array, void **object) -> void {
  auto &f = *cancel_fixture;
  cancel_check(array == f.region.bytes.data() + 0x708 && object &&
                   *object == f.building.pointer(),
               "native fuel-list removal targets only the cancelled building");
  ++f.fuel_removals;
  auto fuels = f.region.get<CityArray>(0x708);
  auto **buildings = static_cast<void **>(fuels.data);
  const auto *end = std::remove(buildings, buildings + fuels.count, *object);
  fuels.count = static_cast<std::int32_t>(end - buildings);
  f.region.put(0x708, fuels);
}

static auto cancel_mock_refresh_resources(void *building) -> void {
  cancel_check(building == cancel_fixture->building.pointer(),
               "native resource refresh targets the cancelled building");
  ++cancel_fixture->resource_refreshes;
}

static auto cancel_mock_refresh_region(void *region, void *building,
                                       std::int32_t mode, void *goods,
                                       std::uint8_t flag) -> void {
  cancel_check(
      region == cancel_fixture->region.pointer() &&
          building == cancel_fixture->building.pointer() && mode == 1 &&
          goods == nullptr && flag == 0,
      "native region refresh retains the documented cache-update arguments");
  ++cancel_fixture->region_refreshes;
}

static auto cancel_mock_building_stats(std::int32_t type) -> const void * {
  auto &f = *cancel_fixture;
  f.stat_types.push_back(type);
  if (f.missing_stat)
    return nullptr;
  // Current 0.8.117 BuildingStats rows: level-two, three and four homes all
  // need fuel independently of the artisan workshop occupying their extension.
  f.building_stat.put<std::uint8_t>(0x299,
                                    type == 8 || type == 60 || type == 484);
  return f.building_stat.pointer();
}

static auto cancel_mock_grow_pointers(void *array, std::int32_t old_count)
    -> void {
  auto &f = *cancel_fixture;
  auto entries = f.region.get<CityArray>(0x708);
  cancel_check(
      array == f.region.bytes.data() + 0x708 &&
          entries.count == old_count + 1 && entries.count > entries.capacity &&
          old_count >= 0 &&
          entries.count <=
              static_cast<std::int32_t>(f.grown_fuel_buildings.size()),
      "native pointer-array grow receives incremented Num and old count");
  ++f.fuel_grows;
  if (f.grow_throws)
    throw std::runtime_error("synthetic native pointer-array growth failure");
  if (!f.native_grow_changes)
    return;
  if (old_count > 0)
    std::copy_n(static_cast<void **>(entries.data), old_count,
                f.grown_fuel_buildings.data());
  entries.data = f.grown_fuel_buildings.data();
  entries.capacity = static_cast<std::int32_t>(f.grown_fuel_buildings.size());
  f.region.put(0x708, entries);
}

static auto cancel_mock_artisan_callback(void *, void **) -> void {
  if (cancel_fixture->artisan_callback_body)
    cancel_fixture->artisan_callback_body();
}

static auto cancel_mock_assembly_request(void *, void *, void *, void *,
                                         std::uint8_t, CityArray *) -> void {
  ++cancel_fixture->requests;
}

static auto cancel_mock_assembly_callback(void *, void *, void *, void *,
                                          std::uint8_t, CityArray *) -> void {
  ++cancel_fixture->callbacks;
  if (cancel_fixture->assembly_body)
    cancel_fixture->assembly_body();
}

static auto cancel_test_api() -> UpgradeApi {
  UpgradeApi api{};
  api.begin = cancel_mock_begin;
  api.setup = [](void *, std::int32_t, std::uint8_t) {};
  api.type_callback = [](void *) {};
  api.extension = cancel_mock_extension;
  api.artisan = cancel_mock_artisan;
  api.weak_assign = cancel_mock_assign;
  api.weak_get = cancel_mock_get;
  api.work_done = [](void *) {
    return cancel_fixture->module.get<float>(0x314);
  };
  api.work_required = [](void *) {
    return cancel_fixture->module.get<float>(0x318);
  };
  api.add_work = [](void *, float work) {
    cancel_fixture->work_added = work;
    cancel_fixture->module.put(0x314,
                               cancel_fixture->module.get<float>(0x314) + work);
  };
  api.visual = [](void *, float progress, std::uint8_t materials) {
    cancel_check(progress == 1.0F && materials == 1,
                 "cancellation retains native completion visual arguments");
  };
  api.finish = [](void *) {
    ++cancel_fixture->finishes;
    cancel_fixture->building.put<std::uint8_t>(0x3c1, 1);
    cancel_fixture->building.put<std::uint8_t>(0x3a1, 0);
    if (cancel_fixture->finish_body)
      cancel_fixture->finish_body();
  };
  api.resource_cost = [](void *, CityArray *result, std::int32_t) {
    if (result)
      *result = {};
    return result;
  };
  api.wealth_cost = [](void *, std::int32_t) { return std::int32_t{0}; };
  api.artisan_callback = cancel_mock_artisan_callback;
  api.assembly_request = cancel_mock_assembly_request;
  api.assembly_callback = cancel_mock_assembly_callback;
  api.mature = [](void *, std::int32_t) {
    ++cancel_fixture->mature_calls;
    return true;
  };
  api.reconstruction_cost = [](void *, CityArray *result, std::int32_t) {
    if (result)
      *result = {};
    return result;
  };
  api.initialize_type = [](void *) {};
  return api;
}

static auto configure_cancel_fixture(CancelFixture &fixture) -> void {
  cancel_fixture = &fixture;
  configure_upgrade(cancel_test_api());
  configure_extension_cancel(
      {cancel_mock_release, cancel_mock_disable_recipe,
       cancel_mock_remove_pointer, cancel_mock_refresh_resources,
       cancel_mock_refresh_region, cancel_mock_building_stats,
       cancel_mock_grow_pointers, cancel_mock_remove_upgrade,
       cancel_mock_upgrade_visuals,
       [](void *map, std::int32_t capacity) {
         auto &f = *cancel_fixture;
         cancel_check(capacity == 0 && !f.native_active &&
                          f.building.get<std::int32_t>(0x14c4) == 0,
                      "goods history reset runs only after completed artisan "
                      "cancellation");
         cancel_check(f.production.get<CityArray>(0xc8).count == 0,
                      "native cleanup receives detached map ownership");
         ++f.production_resets;
         auto *raw = static_cast<std::uint8_t *>(map);
         CityArray slots{};
         std::memcpy(&slots, raw, sizeof(slots));
         cancel_check(slots.data == f.production_slots.data(),
                      "only captured production storage is destroyed");
         const CityArray empty{};
         std::memcpy(raw, &empty, sizeof(empty));
         const std::int32_t zero{}, first_free{-1};
         std::memcpy(raw + 0x28, &zero, sizeof(zero));
         std::memcpy(raw + 0x30, &first_free, sizeof(first_free));
         std::memcpy(raw + 0x34, &zero, sizeof(zero));
       },
       [](void *) { ++cancel_fixture->production_frees; }});
}

static auto expect_cancel_rejected(CancelFixture &fixture, const char *message)
    -> void {
  const auto before = fixture.state();
  cancel_check(!city_can_cancel_artisan(fixture.building.pointer()), message);
  cancel_check(
      city_try_cancel_artisan(fixture.building.pointer(), 0, 0) !=
              ExtensionCancelResult::handled &&
          fixture.artisan_calls == 0 && fixture.extension_calls == 0 &&
          fixture.releases.empty() && fixture.disabled_recipes.empty() &&
          fixture.removed_upgrades.empty() &&
          fixture.upgrade_visual_calls == 0 && fixture.fuel_removals == 0 &&
          fixture.resource_refreshes == 0 && fixture.region_refreshes == 0 &&
          fixture.state() == before,
      "rejected cancellation preserves every fixture byte");
}

struct CancelAssembly {
  struct Path {
    std::array<std::uint64_t, 2> names{0x1234, 0x5678};
    CityArray subpath{};
  } path;
  alignas(16) std::array<std::uint8_t, 96> transform{};
  CityArray tags{};
  std::uint8_t index{0xff};

  auto request() -> void {
    city_upgrade_assembly_request(cancel_fixture->master.pointer(),
                                  cancel_fixture->building.pointer(), &path,
                                  transform.data(), index, &tags);
  }

  auto complete() -> void {
    city_upgrade_assembly_callback(cancel_fixture->master.pointer(),
                                   cancel_fixture->building.pointer(), &path,
                                   transform.data(), index, &tags);
  }
};

static auto run_cancel_valid_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0xd48, level);
    f.building.put(0x3b8, type);
    const auto initial_state = f.state();
    cancel_check(city_can_cancel_artisan(f.building.pointer()) &&
                     f.state() == initial_state && f.artisan_calls == 0 &&
                     f.releases.empty(),
                 "availability query does not cancel loaded artisans");
    auto expected_building = f.building.bytes;
    const auto pawn_before = f.pawn.bytes;
    const auto master_before = f.master.bytes;
    const auto region_before = f.region.bytes;
    const auto module_before = f.module.bytes;
    const auto members_before = f.members;
    const auto unrelated_before = f.families[2].bytes;
    auto expected_families = f.families;
    auto expected_extension = f.extension.bytes;
    const std::int32_t zero{};
    std::memcpy(expected_building.data() + 0x14c4, &zero, sizeof(zero));
    std::memcpy(expected_building.data() + 0x1030, &zero, sizeof(zero));
    std::memcpy(expected_extension.data(), &zero, sizeof(zero));
    for (std::size_t i{}; i < 2; ++i)
      expected_families[i].put<void *>(0x10, nullptr);
    cancel_check(
        city_try_cancel_artisan(f.building.pointer(), 0, 0) ==
                ExtensionCancelResult::handled &&
            f.artisan_calls == 1 && f.extension_calls == 1 &&
            f.releases == std::vector<std::int32_t>({0, 1}),
        "levels two through four cancel and release native professions");
    cancel_check(
        f.building.bytes == expected_building &&
            f.extension.bytes == expected_extension &&
            f.pawn.bytes == pawn_before && f.master.bytes == master_before &&
            f.region.bytes == region_before &&
            f.module.bytes == module_before &&
            std::memcmp(f.families.data(), expected_families.data(),
                        sizeof(f.families)) == 0 &&
            std::memcmp(f.members.data(), members_before.data(),
                        sizeof(f.members)) == 0 &&
            f.families[2].bytes == unrelated_before,
        "only native extension/job/worker changes occur; homes, levels, "
        "inventory, family records and members are preserved");
    cancel_check(upgrade_pending_count() == 0 && f.finishes == 0 &&
                     f.mature_calls == 0,
                 "native-complete cancellation needs no forced construction or "
                 "maturity");
  }
  for (const auto [extension, artisan] : {std::pair{3, 16},
                                          {4, 38},
                                          {5, 22},
                                          {6, 10},
                                          {7, 11},
                                          {9, 20},
                                          {10, 56},
                                          {11, 57},
                                          {13, 29},
                                          {15, 71},
                                          {16, 74}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.extension.put(0, extension);
    f.building.put(0x14c4, artisan);
    cancel_check(city_can_cancel_artisan(f.building.pointer()),
                 "each native artisan extension/job mapping is accepted");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    std::array<CancelMemory<0xd0>, 2> extensions{f.extension, {}};
    extensions[1].put<std::int32_t>(0, 1);
    extensions[1].put(0x18, std::uint64_t{0xdeadbeef});
    const auto second_before = extensions[1].bytes;
    f.building.put(0xb88, CityArray{extensions.data(), 2, 2});
    cancel_check(city_try_cancel_artisan(f.building.pointer(), 0, 0) ==
                         ExtensionCancelResult::handled &&
                     extensions[0].get<std::int32_t>(0) == 0 &&
                     extensions[1].bytes == second_before &&
                     f.building.get<CityArray>(0xb88).count == 2,
                 "artisan removal preserves a second ordinary extension slot");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.native_changes = false;
    const auto before = f.state();
    city_try_cancel_artisan(f.building.pointer(), 0, 0);
    cancel_check(
        f.artisan_calls == 1 && f.releases.empty() && f.state() == before,
        "mod does not write job, extension, family or home fields itself");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.artisan_body = [&] {
      f.families[0].put<void *>(0x10, nullptr);
      f.workers[0] = 1;
      f.building.put(0x1028, CityArray{f.workers.data(), 1, 3});
    };
    city_try_cancel_artisan(f.building.pointer(), 0, 0);
    cancel_check(f.releases == std::vector<std::int32_t>{1},
                 "already-unassigned resident is not released twice");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.artisan_body = [&] {
      f.families[0].put(0x10, f.other.pointer());
      f.workers[0] = 1;
      f.building.put(0x1028, CityArray{f.workers.data(), 1, 3});
    };
    city_try_cancel_artisan(f.building.pointer(), 0, 0);
    cancel_check(f.releases == std::vector<std::int32_t>{1} &&
                     f.families[0].get<void *>(0x10) == f.other.pointer(),
                 "native reassignment elsewhere is preserved during cleanup");
  }
}

static auto run_cancel_settings_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0xd48, level);
    f.building.put(0x3b8, type);
    f.set_workshop_settings();
    f.building.put<std::int32_t>(0x6f8, 4);
    f.building.put<std::uint8_t>(0xe70, 0xa1);
    f.building.put<std::uint8_t>(0xe73, 0xb2);
    f.building.put<std::uint8_t>(0xe79, 0xc3);
    std::array<std::int32_t, 3> other_recipes{25, 17, 3};
    f.other.put(0x14c8, CityArray{other_recipes.data(), 3, 3});
    f.other.put<std::uint8_t>(0xe71, 1);
    f.other.put<std::uint8_t>(0xe72, 1);
    f.other.put<std::uint8_t>(0xe78, 9);
    f.other.put<std::int32_t>(0xe74, 63);
    const auto before = f.state();
    ExtensionCancelSnapshot snapshot;
    cancel_check(
        prepare_artisan_cancel(f.building.pointer(), snapshot) &&
            f.state() == before && f.disabled_recipes.empty(),
        "availability validation preserves fuel settings and recipe priority");
    auto expected_building = f.building;
    expected_building.put<std::int32_t>(0x14c4, 0);
    expected_building.put<std::int32_t>(0x14d0, 0);
    expected_building.put<std::int32_t>(0x1030, 0);
    expected_building.put<std::uint8_t>(0xe71, 1);
    expected_building.put<std::uint8_t>(0xe72, 0);
    expected_building.put<std::int32_t>(0xe74, -1);
    expected_building.put<std::uint8_t>(0xe78, 0);
    auto expected_region = f.region;
    expected_region.put(0x708, CityArray{f.fuel_buildings.data(), 2, 4});
    const auto other_before = f.other.bytes;
    const auto inventory_before = f.inventory;
    const auto pawn_before = f.pawn.bytes;
    const auto master_before = f.master.bytes;
    cancel_check(city_try_cancel_artisan(f.building.pointer(), 0, 0) ==
                         ExtensionCancelResult::handled &&
                     f.settings_cleared() &&
                     f.disabled_recipes ==
                         std::vector<std::int32_t>({17, 3, 25}) &&
                     f.fuel_removals == 1 && f.resource_refreshes != 0 &&
                     f.region_refreshes != 0,
                 "cancellation clears workshop settings while preserving "
                 "native house fuel need and exactly one registration");
    cancel_check(
        f.building.bytes == expected_building.bytes &&
            f.region.bytes == expected_region.bytes &&
            f.other.bytes == other_before &&
            other_recipes == std::array<std::int32_t, 3>{25, 17, 3} &&
            f.inventory == inventory_before && f.pawn.bytes == pawn_before &&
            f.master.bytes == master_before &&
            f.families[0].get<void *>(0x20) == f.building.pointer() &&
            f.families[1].get<void *>(0x20) == f.building.pointer(),
        "fuel cleanup preserves adjacent bytes, build priority, inventory, "
        "wealth, homes and other building or region settings");
    const auto cleared_state = f.state();
    const auto recipe_calls = f.disabled_recipes.size();
    const auto resource_calls = f.resource_refreshes;
    const auto region_calls = f.region_refreshes;
    cancel_check(finish_artisan_cancel(f.building.pointer(), snapshot) &&
                     f.state() == cleared_state &&
                     f.disabled_recipes.size() == recipe_calls &&
                     f.fuel_removals == 1 &&
                     f.resource_refreshes == resource_calls &&
                     f.region_refreshes == region_calls,
                 "already-cleared settings do not repeat native resource or "
                 "fuel-list updates");
  }
  for (const bool fuel_array : {false, true}) {
    for (int invalid{}; invalid < 5; ++invalid) {
      CancelFixture f;
      configure_cancel_fixture(f);
      f.set_workshop_settings();
      auto header = fuel_array ? f.region.get<CityArray>(0x708)
                               : f.building.get<CityArray>(0x14c8);
      if (invalid == 0)
        header.count = -1;
      else if (invalid == 1)
        header.capacity = header.count - 1;
      else if (invalid == 2)
        header.data = nullptr;
      else if (invalid == 3)
        header.data = reinterpret_cast<void *>(1);
      else
        header.capacity = std::numeric_limits<std::int32_t>::max();
      if (fuel_array)
        f.region.put(0x708, header);
      else
        f.building.put(0x14c8, header);
      expect_cancel_rejected(f, "malformed recipes or regional fuel "
                                "registration rejects before mutation");
    }
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_settings();
    configure_extension_cancel({cancel_mock_release});
    expect_cancel_rejected(f, "missing native settings cleanup API rejects "
                              "before cancelling artisan");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_settings();
    f.building.put<std::int32_t>(0x14c4, 0);
    f.extension.put<std::int32_t>(0, 0);
    const auto before = f.state();
    void *captured_building = f.building.pointer();
    void *replacement = f.module.pointer();
    city_upgrade_artisan_callback(&captured_building, &replacement);
    CancelAssembly assembly;
    assembly.request();
    assembly.complete();
    cancel_check(!city_can_cancel_artisan(f.building.pointer()) &&
                     city_try_cancel_artisan(f.building.pointer(), 0, 0) ==
                         ExtensionCancelResult::not_applicable &&
                     f.state() == before && f.disabled_recipes.empty() &&
                     f.fuel_removals == 0 && f.resource_refreshes == 0 &&
                     f.region_refreshes == 0,
                 "previously cancelled houses keep old settings until an "
                 "explicit applicable action");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::int32_t>(0xd48, 4);
    f.building.put<std::int32_t>(0x3b8, 484);
    f.asynchronous = true;
    f.set_workshop_settings();
    CancelAssembly assembly;
    void *captured_building = f.building.pointer();
    void *replacement = f.module.pointer();
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.settings_cleared() && upgrade_pending_count() == 1,
                 "asynchronous cancellation clears fuel and recipes before "
                 "replacement finishes");
    f.artisan_callback_body = [&] {
      f.module.put<std::uint8_t>(0x311, 0);
      f.set_workshop_settings();
      assembly.request();
    };
    city_upgrade_artisan_callback(&captured_building, &replacement);
    cancel_check(
        f.settings_cleared() && f.finishes == 0,
        "replacement callback cannot reintroduce fuel or recipe priorities");
    f.assembly_body = [&] { f.set_workshop_settings(); };
    f.finish_body = [&] { f.set_workshop_settings(); };
    assembly.complete();
    cancel_check(f.settings_cleared() && f.finishes == 1 &&
                     upgrade_pending_count() == 0 &&
                     f.disabled_recipes.size() == 12 && f.fuel_removals == 4,
                 "child callbacks and native construction finalizer cannot "
                 "restore workshop settings");
    f.assembly_body = {};
    f.finish_body = {};
    f.artisan_callback_body = {};
    const auto cleared_state = f.state();
    const auto resource_calls = f.resource_refreshes;
    const auto region_calls = f.region_refreshes;
    assembly.complete();
    city_upgrade_artisan_callback(&captured_building, &replacement);
    cancel_check(f.state() == cleared_state &&
                     f.disabled_recipes.size() == 12 && f.fuel_removals == 4 &&
                     f.resource_refreshes == resource_calls &&
                     f.region_refreshes == region_calls,
                 "repeated callbacks neither refill fuel nor repeat completed "
                 "settings cleanup");
  }
}

static auto run_cancel_settings_boundary_tests() -> void {
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.recipes = {17, 3, 25, 17};
    f.building.put(0x14c8, CityArray{f.recipes.data(), 4, 4});
    f.disable_recipe_body = [&] {
      f.building.put<std::uint8_t>(0xe71, 0);
      f.building.put<std::uint8_t>(0xe72, 1);
      f.building.put<std::int32_t>(0xe74, 91);
      f.building.put<std::uint8_t>(0xe78, 7);
      f.fuel_buildings = {f.other.pointer(), f.building.pointer(),
                          f.building.pointer(), nullptr};
      f.region.put(0x708, CityArray{f.fuel_buildings.data(), 3, 4});
    };
    city_try_cancel_artisan(f.building.pointer(), 0, 0);
    cancel_check(f.settings_cleared() &&
                     f.disabled_recipes ==
                         std::vector<std::int32_t>({17, 3, 25}) &&
                     f.fuel_removals == 1,
                 "recipe removal side effects cannot restore initially-clean "
                 "fuel fields or add lingering registrations");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_settings();
    f.native_recipe_changes = false;
    city_try_cancel_artisan(f.building.pointer(), 0, 0);
    cancel_check(f.disabled_recipes == std::vector<std::int32_t>{17} &&
                     f.building.get<CityArray>(0x14c8).count == 4 &&
                     f.fuel_removals == 0 && f.resource_refreshes == 0 &&
                     f.region_refreshes == 0 &&
                     extension_cancel_declined_count() > 0,
                 "a native recipe-disable no-op fails without looping or "
                 "fabricating recipe removal");
  }
  for (const bool fuel_array : {false, true}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_settings();
    auto header = fuel_array ? f.region.get<CityArray>(0x708)
                             : f.building.get<CityArray>(0x14c8);
    const auto release_page = [](void *page) {
      VirtualFree(page, 0, MEM_RELEASE);
    };
    const std::unique_ptr<void, decltype(release_page)> page(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
        release_page);
    cancel_check(page != nullptr, "allocate isolated settings test page");
    const auto stride = fuel_array ? sizeof(void *) : sizeof(std::int32_t);
    std::memcpy(page.get(), header.data,
                static_cast<std::size_t>(header.count) * stride);
    DWORD previous_protection{};
    cancel_check(VirtualProtect(page.get(), 4096, PAGE_READONLY,
                                &previous_protection) != 0,
                 "protect recipe or fuel-list allocation for rejection test");
    header.data = page.get();
    if (fuel_array)
      f.region.put(0x708, header);
    else
      f.building.put(0x14c8, header);
    expect_cancel_rejected(
        f,
        "read-only native array rejects before artisan or settings mutation");
  }
}

static auto run_cancel_fuel_lifecycle_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0xd48, level);
    f.building.put(0x3b8, type);
    f.extension.put<std::int32_t>(0, 16);
    f.building.put<std::int32_t>(0x14c4, 74);
    f.set_workshop_settings();
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(
        f.settings_cleared() && !f.stat_types.empty() &&
            std::all_of(f.stat_types.begin(), f.stat_types.end(),
                        [type](auto queried) { return queried == type; }),
        "cheesemonger removal derives fuel need from the actual level-specific "
        "house row");
    const auto inventory_before = f.inventory;
    const auto regional_before = f.region.bytes;
    const auto disabled_before = f.disabled_recipes;
    city_change_artisan(f.building.pointer(), 3);
    cancel_check(f.artisan_calls == 2 &&
                     f.building.get<std::int32_t>(0x14c4) == 16 &&
                     f.extension.get<std::int32_t>(0) == 3 &&
                     f.building.get<std::uint8_t>(0xe71) == 1 &&
                     f.region.bytes == regional_before &&
                     f.building.get<CityArray>(0x14c8).count == 1 &&
                     f.inventory == inventory_before &&
                     f.building.get<std::int32_t>(0xd48) == level,
                 "a new brewery retains fuel-tab eligibility and fuel delivery "
                 "after cancelling cheesemonger");
    void *captured_building = f.building.pointer();
    void *replacement = f.module.pointer();
    city_upgrade_artisan_callback(&captured_building, &replacement);
    cancel_check(f.disabled_recipes == disabled_before &&
                     f.building.get<CityArray>(0x14c8).count == 1 &&
                     f.building.get<std::uint8_t>(0xe71) == 1,
                 "late unrelated callbacks cannot clear the newly selected "
                 "brewery's settings");
  }
  for (const bool needs_growth : {false, true}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::uint8_t>(0xe71, 0);
    f.region.put(0x708,
                 CityArray{f.fuel_buildings.data(), 1, needs_growth ? 1 : 4});
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.settings_cleared() &&
                     f.fuel_grows == static_cast<std::size_t>(needs_growth) &&
                     f.fuel_removals == 0,
                 "native house fuel derivation repairs a missing registration "
                 "with correct array growth");
  }
  for (const bool grow_throws : {false, true}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::uint8_t>(0xe71, 0);
    f.region.put(0x708, CityArray{f.fuel_buildings.data(), 1, 1});
    f.grow_throws = grow_throws;
    f.native_grow_changes = false;
    bool caught{};
    try {
      city_change_extension(f.building.pointer(), 0, 0);
    } catch (const std::runtime_error &) {
      caught = true;
    }
    const auto registrations = f.region.get<CityArray>(0x708);
    cancel_check(caught == grow_throws && f.fuel_grows == 1 &&
                     registrations.data == f.fuel_buildings.data() &&
                     registrations.count == 1 && registrations.capacity == 1 &&
                     f.fuel_buildings[0] == f.other.pointer() &&
                     upgrade_pending_count() == 0 &&
                     (grow_throws || extension_cancel_declined_count() > 0),
                 "native array growth failure or exception restores Num and "
                 "does not leave a pending cancellation");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_settings();
    f.artisan_body = [&] { f.building.put<std::int32_t>(0x3b8, 9); };
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.settings_cleared() &&
                     f.building.get<std::uint8_t>(0xe71) == 0 &&
                     f.region.get<CityArray>(0x708).count == 1 &&
                     std::find(f.stat_types.begin(), f.stat_types.end(), 9) !=
                         f.stat_types.end(),
                 "a nonfuel native type after artisan removal disables fuel "
                 "and removes only its registration");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_settings();
    f.artisan_body = [&] {
      f.building.put<std::int32_t>(0x3b8, 9);
      f.building.put<std::uint8_t>(0x3cc, 2);
    };
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.settings_cleared() &&
                     f.building.get<std::uint8_t>(0xe71) == 1,
                 "native building function two retains fuel even when the stat "
                 "flag is false");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.missing_stat = true;
    expect_cancel_rejected(
        f, "unavailable native building stat rejects before cancellation");
  }
}

static auto run_cancel_rejection_tests() -> void {
  for (const std::int32_t level : {-1, 0, 5, 100}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0xd48, level);
    expect_cancel_rejected(f, "invalid house levels fail closed");
  }
  for (const std::int32_t extension : {-1, 0, 1, 2, 8, 12, 14, 17, 100}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.extension.put(0, extension);
    expect_cancel_rejected(f, "non-artisan extension IDs fail closed");
  }
  for (const std::int32_t artisan : {-1, 0, 10, 100}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0x14c4, artisan);
    expect_cancel_rejected(f, "mismatched artisan job fails closed");
  }
  for (const std::size_t offset : {0x388U, 0x3a0U, 0x3a1U, 0x3a2U, 0x3a3U}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::uint8_t>(offset, 1);
    expect_cancel_rejected(
        f, "burning, demolition and upgrading block cancellation");
  }
  for (const std::uint8_t value : {0, 2}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0x3c1, value);
    expect_cancel_rejected(
        f, "unconstructed and malformed constructed flags reject");
  }
  for (const std::uint32_t flag : {0x8000U, 0x10000U, 0x40000000U}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(8, flag);
    expect_cancel_rejected(f, "destroying or unreachable owners reject");
  }
  for (int invalid{}; invalid < 11; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    if (invalid == 0)
      f.pawn.put<std::uint8_t>(0x34c, 0);
    else if (invalid == 1)
      f.pawn.put<std::uint8_t>(0x34d, 1);
    else if (invalid == 2)
      f.building.put(0x2e0, f.other.pointer());
    else if (invalid == 3)
      f.building.put<void *>(0x2c8, nullptr);
    else if (invalid == 4)
      f.master.put<std::uint8_t>(0x318, 0);
    else if (invalid == 5)
      f.building.put<void *>(0x2e8, nullptr);
    else if (invalid == 6)
      f.module.put<std::uint8_t>(0x311, 1);
    else if (invalid == 7)
      f.building.put(0x3d0, CityArray{f.workers.data(), 1, 3});
    else if (invalid == 8)
      f.building.put<std::uint8_t>(0x3cc, 2);
    else if (invalid == 9)
      f.building.put<std::uint8_t>(0x5d, 1);
    else
      configure_extension_cancel({});
    expect_cancel_rejected(
        f, "invalid ownership, transition or native API rejects");
  }
  for (const std::size_t offset : {0xb88U, 0x1028U, 0x1038U, 0x2f8U}) {
    for (int invalid{}; invalid < 5; ++invalid) {
      CancelFixture f;
      configure_cancel_fixture(f);
      auto header = f.building.get<CityArray>(offset);
      if (invalid == 0)
        header.count = -1;
      else if (invalid == 1)
        header.capacity = header.count - 1;
      else if (invalid == 2)
        header.data = nullptr;
      else if (invalid == 3)
        header.data = reinterpret_cast<void *>(1);
      else
        header.capacity = std::numeric_limits<std::int32_t>::max();
      f.building.put(offset, header);
      expect_cancel_rejected(
          f, "malformed extension, worker, resident or module array rejects");
    }
  }
  for (int invalid{}; invalid < 16; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    if (invalid == 0)
      f.region.put(0x4e8, CityArray{f.families.data(), -1, 3});
    else if (invalid == 1)
      f.region.put(0x4e8, CityArray{nullptr, 3, 3});
    else if (invalid == 2)
      f.workers[1] = 0;
    else if (invalid == 3)
      f.residents[1] = 0;
    else if (invalid == 4)
      f.workers[1] = 3;
    else if (invalid == 5)
      f.residents[1] = -1;
    else if (invalid == 6)
      f.families[1].put(0x20, f.other.pointer());
    else if (invalid == 7)
      f.families[1].put(0x10, f.other.pointer());
    else if (invalid == 8)
      f.families[1].put(0, CityArray{nullptr, 1, 1});
    else if (invalid == 9)
      f.members[1].put<std::int32_t>(0x348, 0);
    else if (invalid == 10)
      f.members[1].put<std::uint32_t>(8, 0x40000000);
    else if (invalid == 11)
      f.building.put(0x1028, CityArray{f.workers.data(), 1, 3});
    else if (invalid == 12)
      f.members[1].put(0x320, f.other.pointer());
    else if (invalid == 13)
      f.members[1].put(0x328, f.other.pointer());
    else if (invalid == 14)
      f.members[1].put(0x338, f.other.pointer());
    else
      f.members[1].put(0x340, f.other.pointer());
    expect_cancel_rejected(
        f, "malformed family identity or resident/worker association rejects");
  }
}

static auto run_cancel_async_tests() -> void {
  for (const std::uint8_t index : {0, 1, 127, 128, 254, 255}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    CancelAssembly assembly;
    assembly.index = index;
    assembly.request();
    expect_cancel_rejected(
        f, "every outstanding assembly source blocks cancellation");
    assembly.complete();
    cancel_check(
        city_can_cancel_artisan(f.building.pointer()),
        "completed assembly releases only its own cancellation barrier");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    city_begin_upgrade(f.building.pointer(), 103);
    expect_cancel_rejected(
        f,
        "pending upgrade blocks cancellation even before native flags change");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    CancelAssembly assembly;
    city_upgrade_assembly_request(f.master.pointer(), f.building.pointer(),
                                  nullptr, assembly.transform.data(), 0,
                                  &assembly.tags);
    expect_cancel_rejected(f,
                           "unreliable assembly tracking blocks cancellation");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::int32_t>(0xd48, 4);
    f.building.put<std::int32_t>(0x3b8, 484);
    f.asynchronous = true;
    CancelAssembly house, extension;
    extension.index = 0;
    void *captured_building = f.building.pointer();
    void *replacement = f.module.pointer();
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(
        f.artisan_calls == 1 && f.begin_calls == 0 && f.finishes == 0 &&
            upgrade_pending_count() == 1 && f.releases.size() == 2,
        "level-four cancel tracks a ready job without native BeginUpgrade");
    f.artisan_callback_body = [&] {
      f.module.put<std::uint8_t>(0x311, 0);
      f.families[0].put(0x10, f.building.pointer());
      f.workers[0] = 0;
      f.building.put(0x1028, CityArray{f.workers.data(), 1, 3});
      house.request();
      extension.request();
      cancel_check(f.finishes == 0,
                   "artisan callback must return before finalization");
    };
    city_upgrade_artisan_callback(&captured_building, &replacement);
    cancel_check(f.finishes == 0 && upgrade_pending_count() == 1 &&
                     f.releases == std::vector<std::int32_t>({0, 1, 0}) &&
                     f.families[0].get<void *>(0x10) == nullptr,
                 "main replacement releases a reassigned old worker and waits "
                 "for assemblies");
    house.complete();
    cancel_check(f.finishes == 0 && upgrade_pending_count() == 1,
                 "first completed child does not finalize cancellation");
    f.assembly_body = [&] {
      cancel_check(f.finishes == 0,
                   "last native child callback must finish first");
    };
    f.finish_body = [&] {
      f.families[1].put(0x10, f.building.pointer());
      f.workers[0] = 1;
      f.building.put(0x1028, CityArray{f.workers.data(), 1, 3});
    };
    extension.complete();
    f.assembly_body = {};
    cancel_check(f.finishes == 1 && upgrade_pending_count() == 0 &&
                     f.work_added == 8.0F &&
                     f.building.get<std::int32_t>(0xd48) == 4 &&
                     f.families[0].get<void *>(0x20) == f.building.pointer() &&
                     f.families[1].get<void *>(0x20) == f.building.pointer(),
                 "last callback finishes missing native work without changing "
                 "house level or homes");
    extension.complete();
    cancel_check(
        f.finishes == 1 &&
            f.releases == std::vector<std::int32_t>({0, 1, 0, 1}) &&
            f.families[1].get<void *>(0x10) == nullptr,
        "duplicate callbacks cannot finalize or release families twice");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    void *captured_building = f.building.pointer();
    void *replacement = f.module.pointer();
    const auto before = f.state();
    city_upgrade_artisan_callback(&captured_building, &replacement);
    CancelAssembly assembly;
    assembly.request();
    assembly.complete();
    cancel_check(
        f.artisan_calls == 0 && f.releases.empty() && f.finishes == 0 &&
            f.state() == before,
        "load-time or unrelated callbacks never cancel an existing artisan");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.pawn.put<std::uint8_t>(0x34d, 1);
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(
        f.extension_calls == 1 && f.artisan_calls == 0 && f.releases.empty(),
        "AI extension calls retain native dispatch without worker cleanup");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::int32_t>(0xd48, 1);
    f.building.put<std::int32_t>(0x3b8, 9);
    f.building.put<std::int32_t>(0x14c4, 0);
    f.extension.put<std::int32_t>(0, 1);
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(
        f.extension_calls == 1 && f.artisan_calls == 0 && f.releases.empty() &&
            f.last_index == 0 && f.last_type == 0,
        "ordinary level-one extension cancellation retains its native path");
  }
  for (const auto [index, type] :
       {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 3}, {0, 100}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    const auto before = f.state();
    cancel_check(
        city_try_cancel_artisan(f.building.pointer(), index, type) ==
                ExtensionCancelResult::not_applicable &&
            f.state() == before && f.artisan_calls == 0 && f.releases.empty(),
        "unrelated extension indices and requested types are not intercepted");
  }
}

static auto run_cancel_duplicate_worker_tests() -> void {
  for (const std::int32_t worker_count : {3, 16}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    std::array<std::int32_t, 16> workers{};
    workers[2] = 1;
    if (worker_count == 16)
      for (std::size_t i{}; i < workers.size(); ++i)
        workers[i] = static_cast<std::int32_t>(i % 2);
    f.building.put(0x1028, CityArray{workers.data(), worker_count, 16});
    const auto residents_before = f.residents;
    const auto region_before = f.region.bytes;
    const auto members_before = f.members;
    auto expected_families = f.families;
    expected_families[0].put<void *>(0x10, nullptr);
    expected_families[1].put<void *>(0x10, nullptr);
    cancel_check(city_can_cancel_artisan(f.building.pointer()),
                 "duplicate worker registry entries are valid when their "
                 "unique IDs equal the unique resident families");
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.releases == std::vector<std::int32_t>({0, 1}) &&
                     f.building.get<CityArray>(0x1028).count == 0 &&
                     f.residents == residents_before &&
                     f.region.bytes == region_before &&
                     std::memcmp(f.families.data(), expected_families.data(),
                                 sizeof(f.families)) == 0 &&
                     std::memcmp(f.members.data(), members_before.data(),
                                 sizeof(f.members)) == 0,
                 "native release removes all duplicate worker entries exactly "
                 "once per family without deleting residents or homes");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.workers = {0, 1, 2};
    f.building.put(0x1028, CityArray{f.workers.data(), 3, 3});
    expect_cancel_rejected(f, "a nonresident worker is still rejected after "
                              "duplicate IDs are normalized");
  }
}

static auto run_cancel_resource_module_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0xd48, level);
    f.building.put(0x3b8, type);
    f.resource_modules[0] = f.resource_module.pointer();
    f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
    f.extension_modules = {f.module.pointer(), f.resource_module.pointer()};
    f.extension.put(0x70, CityArray{f.extension_modules.data(), 2, 2});
    const auto before = f.state();
    cancel_check(city_can_cancel_artisan(f.building.pointer()) &&
                     f.state() == before,
                 "owned extension actors may span main and resource module "
                 "registries at every house level");
  }
  for (int invalid{}; invalid < 5; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.resource_modules[0] = f.resource_module.pointer();
    f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
    f.extension_modules[0] = f.resource_module.pointer();
    f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    if (invalid == 0)
      f.resource_module.put(0x2c8, f.other.pointer());
    else if (invalid == 1)
      f.building.put(0x308, CityArray{});
    else if (invalid == 2)
      f.resource_module.put<std::uint32_t>(8, 0x40000000);
    else if (invalid == 3)
      f.resource_module.put<std::uint8_t>(0x5d, 1);
    else
      f.extension_modules[0] = reinterpret_cast<void *>(1);
    expect_cancel_rejected(
        f, "foreign, unregistered, destroyed or invalid extension resource "
           "actors reject before native cleanup");
  }
  for (int invalid{}; invalid < 5; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.resource_modules[0] = f.resource_module.pointer();
    f.extension_modules[0] = f.resource_module.pointer();
    f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    CityArray resources{f.resource_modules.data(), 1, 2};
    if (invalid == 0)
      resources.count = -1;
    else if (invalid == 1)
      resources.capacity = 0;
    else if (invalid == 2)
      resources.data = nullptr;
    else if (invalid == 3)
      resources.data = reinterpret_cast<void *>(1);
    else
      resources.capacity = std::numeric_limits<std::int32_t>::max();
    f.building.put(0x308, resources);
    expect_cancel_rejected(f, "malformed resource-module registries reject "
                              "without changing workshop or family data");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.extension.put<std::int32_t>(0, 16);
    f.building.put<std::int32_t>(0x14c4, 74);
    f.resource_modules[0] = f.resource_module.pointer();
    f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
    f.extension_modules[0] = f.resource_module.pointer();
    f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    const auto main_before = f.module.bytes;
    const auto inventory_before = f.inventory;
    cancel_check(city_can_cancel_artisan(f.building.pointer()),
                 "resource-only extension members reproduce the live valid "
                 "workshop layout");
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.resource_module.get<std::uint32_t>(8) == 0x40000000 &&
                     f.building.get<CityArray>(0x308).count == 0 &&
                     f.extension.get<CityArray>(0x70).count == 0 &&
                     f.module.bytes == main_before &&
                     f.inventory == inventory_before,
                 "native cancellation removes owned resource actors while "
                 "preserving main house and goods");
    f.selection_body = [&] {
      f.resource_modules[0] = f.replacement_resource.pointer();
      f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
      f.extension_modules[0] = f.replacement_resource.pointer();
      f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    };
    // Live selected brewery: extension 5, artisan job 22. Its resource actors
    // occupy +0x308 rather than the main construction modules at +0x2f8.
    city_change_artisan(f.building.pointer(), 5);
    cancel_check(city_can_cancel_artisan(f.building.pointer()) &&
                     f.building.get<std::int32_t>(0x14c4) == 22 &&
                     f.extension.get<std::int32_t>(0) == 5,
                 "cheesemonger-to-brewery selection remains cancellable with a "
                 "new resource-only actor");
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(
        f.releases == std::vector<std::int32_t>({0, 1, 0, 1}) &&
            f.replacement_resource.get<std::uint32_t>(8) == 0x40000000 &&
            f.building.get<CityArray>(0x308).count == 0 &&
            f.module.bytes == main_before && f.inventory == inventory_before &&
            upgrade_pending_count() == 0 && upgrade_failed_count() == 0,
        "repeated resource-backed workshop cancellation completes native "
        "cleanup without a persistent gate");
  }
}

static auto run_cancel_repeated_cycle_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    CancelFixture first, second;
    configure_cancel_fixture(first);
    for (auto *fixture : {&first, &second}) {
      fixture->building.put(0xd48, level);
      fixture->building.put(0x3b8, type);
      fixture->asynchronous = level == 4;
      fixture->native_selection_upgrade = level == 4;
    }
    const auto second_before = second.state();
    const auto check_native_scope = [&] {
      cancel_check(!city_can_cancel_artisan(first.building.pointer()) &&
                       city_can_cancel_artisan(second.building.pointer()),
                   "an active change rejects its own query without blocking an "
                   "idle second house");
    };
    first.artisan_body = check_native_scope;
    first.selection_body = check_native_scope;
    const auto complete_operation = [&](CancelFixture &owner,
                                        CancelFixture &other) {
      if (!owner.asynchronous) {
        cancel_check(
            upgrade_pending_count() == 0,
            "synchronous cancellation retains no pending authorization");
        return;
      }
      cancel_check(upgrade_pending_count() == 1 &&
                       !city_can_cancel_artisan(owner.building.pointer()) &&
                       city_can_cancel_artisan(other.building.pointer()),
                   "pending level-four replacement only blocks its own house");
      CancelAssembly assembly;
      void *captured_building = owner.building.pointer();
      void *replacement = owner.module.pointer();
      owner.artisan_callback_body = [&] {
        owner.module.put<std::uint8_t>(0x311, 0);
        assembly.request();
        cancel_check(!city_can_cancel_artisan(owner.building.pointer()),
                     "native replacement callback cannot expose cancellation "
                     "while its scope is active");
      };
      owner.assembly_body = [&] {
        cancel_check(!city_can_cancel_artisan(owner.building.pointer()),
                     "last assembly callback remains temporarily ineligible");
      };
      owner.finish_body = [&] {
        cancel_check(
            !city_can_cancel_artisan(owner.building.pointer()) &&
                !city_can_cancel_artisan(other.building.pointer()),
            "native finalization temporarily rejects queries for both houses");
      };
      city_upgrade_artisan_callback(&captured_building, &replacement);
      assembly.complete();
      owner.artisan_callback_body = {};
      owner.assembly_body = {};
      owner.finish_body = {};
      cancel_check(upgrade_pending_count() == 0 &&
                       city_upgrade_cancel_ready(owner.building.pointer()) &&
                       city_can_cancel_artisan(other.building.pointer()),
                   "completed replacement releases pending state and restores "
                   "the second house query");
    };
    for (int cycle{}; cycle < 2; ++cycle) {
      cancel_check(
          city_can_cancel_artisan(first.building.pointer()),
          "a completed artisan is cancellable at the start of every cycle");
      city_change_extension(first.building.pointer(), 0, 0);
      complete_operation(first, second);
      cancel_check(!city_can_cancel_artisan(first.building.pointer()) &&
                       city_upgrade_cancel_ready(first.building.pointer()) &&
                       first.building.get<CityArray>(0x1028).count == 0 &&
                       second.state() == second_before,
                   "empty cancelled house has no stale upgrade barrier and "
                   "does not mutate another house");
      city_change_artisan(first.building.pointer(), 3);
      complete_operation(first, second);
      cancel_check(
          city_can_cancel_artisan(first.building.pointer()) &&
              city_can_cancel_artisan(second.building.pointer()) &&
              first.building.get<CityArray>(0x1028).count == 2 &&
              first.families[0].get<void *>(0x10) == first.building.pointer() &&
              first.families[1].get<void *>(0x10) == first.building.pointer() &&
              first.members[0].get<void *>(0x338) == first.building.pointer() &&
              first.members[1].get<void *>(0x338) == first.building.pointer(),
          "native resident reassignment restores eligibility for cancellation "
          "again without resetting the backend");
    }
    cancel_check(first.releases == std::vector<std::int32_t>({0, 1, 0, 1}) &&
                     upgrade_failed_count() == 0,
                 "two completed cancellation cycles release each household "
                 "once per cycle");
    // Switch only the mock's active building. Preserve every weak identity and
    // backend barrier instead of reconfiguring or resetting the upgrade API.
    second.weak_objects = std::move(first.weak_objects);
    second.next_weak = first.next_weak;
    cancel_fixture = &second;
    cancel_check(city_can_cancel_artisan(second.building.pointer()),
                 "a different house stays cancellable after repeated changes "
                 "to the first");
    city_change_extension(second.building.pointer(), 0, 0);
    complete_operation(second, first);
    cancel_check(second.releases == std::vector<std::int32_t>({0, 1}) &&
                     upgrade_pending_count() == 0 &&
                     upgrade_failed_count() == 0 &&
                     city_can_cancel_artisan(first.building.pointer()),
                 "cancelling the second house does not leave global pending "
                 "state or disable the first");
  }
}

static auto run_cancel_upgrade_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    for (const auto [extension, artisan] : {std::pair{3, 16},
                                            {4, 38},
                                            {5, 22},
                                            {6, 10},
                                            {7, 11},
                                            {9, 20},
                                            {10, 56},
                                            {11, 57},
                                            {13, 29},
                                            {15, 71},
                                            {16, 74}}) {
      CancelFixture f;
      configure_cancel_fixture(f);
      f.building.put(0xd48, level);
      f.building.put(0x3b8, type);
      f.building.put(0x14c4, artisan);
      f.extension.put(0, extension);
      f.set_workshop_upgrades();
      f.set_workshop_visuals();
      const auto before = f.state();
      ExtensionCancelSnapshot snapshot;
      cancel_check(
          prepare_artisan_cancel(f.building.pointer(), snapshot) &&
              f.state() == before && f.removed_upgrades.empty(),
          "all artisan and house-level combinations validate completed "
          "upgrades without changing them");
      auto expected_building = f.building;
      expected_building.put<std::int32_t>(0x14c4, 0);
      expected_building.put<std::int32_t>(0x1030, 0);
      expected_building.put<std::int32_t>(0x438, 5);
      expected_building.put<std::int32_t>(0x378, 0);
      const auto region_before = f.region.bytes;
      const auto module_before = f.module.bytes;
      const auto inventory_before = f.inventory;
      const auto other_before = f.other.bytes;
      const auto visuals_before = f.upgrade_visuals;
      f.artisan_entry_body = [&] {
        cancel_check(f.upgrades_cleared() &&
                         f.building.get<CityArray>(0x370).count == 0,
                     "native artisan reconstruction sees all obsolete add-on "
                     "IDs and cached visual bindings removed before it can "
                     "select assemblies");
      };
      cancel_check(city_try_cancel_artisan(f.building.pointer(), 0, 0) ==
                           ExtensionCancelResult::handled &&
                       f.upgrades_cleared() &&
                       f.removed_upgrades ==
                           std::vector<std::int32_t>({37, 42, 43}),
                   "every artisan cancellation removes kettle, gantry and oven "
                   "IDs including duplicates and stale prior-workshop add-ons");
      cancel_check(f.building.bytes == expected_building.bytes &&
                       f.region.bytes == region_before &&
                       f.module.bytes == module_before &&
                       f.inventory == inventory_before &&
                       f.other.bytes == other_before &&
                       std::memcmp(f.upgrade_visuals.data(),
                                   visuals_before.data(),
                                   sizeof(f.upgrade_visuals)) == 0 &&
                       f.upgrade_visual_calls == 0,
                   "add-on cleanup preserves housing level, residential "
                   "expansion, unrelated upgrades, goods and cache allocation "
                   "without rebuilding visuals during cancellation");
      const auto cleared_state = f.state();
      cancel_check(finish_artisan_cancel(f.building.pointer(), snapshot) &&
                       f.state() == cleared_state &&
                       f.removed_upgrades.size() == 3 &&
                       upgrade_pending_count() == 0,
                   "completed add-on cleanup is idempotent and retains no "
                   "pending construction state");
    }
  }
  for (const auto [extension, artisan] : {std::pair{4, 38}, {13, 29}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.extension.put(0, extension);
    f.building.put(0x14c4, artisan);
    f.set_workshop_upgrades();
    f.resource_modules[0] = f.resource_module.pointer();
    f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
    f.extension_modules[0] = f.resource_module.pointer();
    f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    const auto house_before = f.module.bytes;
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.upgrades_cleared() &&
                     f.resource_module.get<std::uint32_t>(8) == 0x40000000 &&
                     f.building.get<CityArray>(0x308).count == 0 &&
                     f.extension.get<CityArray>(0x70).count == 0 &&
                     f.module.bytes == house_before,
                 "gantry and stone-oven upgrades lose both their completed "
                 "flags and native extension actors while keeping the house");
  }
}

static auto run_cancel_upgrade_boundary_tests() -> void {
  for (int invalid{}; invalid < 6; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    auto upgrades = f.building.get<CityArray>(0x430);
    if (invalid == 0)
      upgrades.count = -1;
    else if (invalid == 1)
      upgrades.capacity = upgrades.count - 1;
    else if (invalid == 2)
      upgrades.data = nullptr;
    else if (invalid == 3)
      upgrades.data = reinterpret_cast<void *>(1);
    else if (invalid == 4)
      upgrades.capacity = std::numeric_limits<std::int32_t>::max();
    else {
      upgrades.data = nullptr;
      upgrades.count = 0;
    }
    f.building.put(0x430, upgrades);
    expect_cancel_rejected(f, "malformed completed-upgrade arrays reject "
                              "before any workshop or add-on mutation");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    configure_extension_cancel(
        {cancel_mock_release, cancel_mock_disable_recipe,
         cancel_mock_remove_pointer, cancel_mock_refresh_resources,
         cancel_mock_refresh_region, cancel_mock_building_stats,
         cancel_mock_grow_pointers});
    expect_cancel_rejected(f,
                           "missing native upgrade-removal API fails closed");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    const auto release_page = [](void *page) {
      VirtualFree(page, 0, MEM_RELEASE);
    };
    const std::unique_ptr<void, decltype(release_page)> page(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
        release_page);
    cancel_check(page != nullptr, "allocate isolated completed-upgrade page");
    std::memcpy(page.get(), f.completed_upgrades.data(),
                sizeof(f.completed_upgrades));
    DWORD previous_protection{};
    cancel_check(VirtualProtect(page.get(), 4096, PAGE_READONLY,
                                &previous_protection) != 0,
                 "protect completed upgrades from native removal");
    f.building.put(0x430, CityArray{page.get(), 11, 12});
    expect_cancel_rejected(f, "read-only completed upgrades reject before "
                              "native artisan reconstruction");
    cancel_check(std::memcmp(page.get(), f.completed_upgrades.data(),
                             sizeof(f.completed_upgrades)) == 0,
                 "rejected cancellation preserves the protected upgrade list");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    ExtensionCancelSnapshot snapshot;
    cancel_check(
        prepare_artisan_cancel(f.building.pointer(), snapshot),
        "capture completed upgrades before a simulated intervening edit");
    f.completed_upgrades[8] = 32;
    const auto changed = f.state();
    cancel_check(!begin_artisan_cancel(f.building.pointer(), snapshot) &&
                     f.removed_upgrades.empty() && f.state() == changed,
                 "changed upgrade contents invalidate the captured request "
                 "without erasing the intervening edit");
  }
  for (const bool throws : {false, true}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    f.native_upgrade_changes = false;
    f.upgrade_remove_throws = throws;
    const auto before = f.state();
    try {
      city_change_extension(f.building.pointer(), 0, 0);
    } catch (const std::runtime_error &) {
      cancel_check(throws, "only the synthetic removal exception propagates");
    }
    cancel_check(f.removed_upgrades == std::vector<std::int32_t>{37} &&
                     f.artisan_calls == 0 && f.extension_calls == 0 &&
                     f.releases.empty() && f.disabled_recipes.empty() &&
                     f.state() == before && upgrade_pending_count() == 0 &&
                     city_can_cancel_artisan(f.building.pointer()),
                 "native removal no-op or exception leaves the workshop "
                 "untouched and releases its pending cancellation barrier");
  }
}

static auto run_cancel_upgrade_lifecycle_tests() -> void {
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    for (const auto [extension, upgrade] :
         {std::pair{4, 43}, {5, 37}, {13, 42}}) {
      CancelFixture f;
      configure_cancel_fixture(f);
      f.building.put(0xd48, level);
      f.building.put(0x3b8, type);
      city_change_artisan(f.building.pointer(), extension);
      f.completed_upgrades = {12, upgrade, 2, 4, 5};
      f.building.put(0x430, CityArray{f.completed_upgrades.data(), 5, 12});
      f.selection_body = [&] {
        auto upgrades = f.building.get<CityArray>(0x430);
        auto *ids = static_cast<std::int32_t *>(upgrades.data);
        cancel_check(std::find(ids, ids + upgrades.count, upgrade) ==
                         ids + upgrades.count,
                     "rebuilt workshop has no stale completed flag blocking "
                     "its add-on from being built again");
        ids[upgrades.count++] = upgrade;
        f.building.put(0x430, upgrades);
      };
      for (int cycle{}; cycle < 2; ++cycle) {
        city_change_extension(f.building.pointer(), 0, 0);
        constexpr std::array<std::int32_t, 4> retained{12, 2, 4, 5};
        cancel_check(f.building.get<CityArray>(0x430).count == 4 &&
                         std::equal(retained.begin(), retained.end(),
                                    f.completed_upgrades.begin()) &&
                         f.building.get<std::int32_t>(0xd48) == level &&
                         upgrade_pending_count() == 0,
                     "each cancellation clears only the installed workshop "
                     "add-on and preserves residential upgrades");
        city_change_artisan(f.building.pointer(), extension);
        cancel_check(city_can_cancel_artisan(f.building.pointer()) &&
                         f.building.get<CityArray>(0x430).count == 5 &&
                         f.completed_upgrades[4] == upgrade,
                     "rebuilt bakery, brewery and butcher add-ons can be "
                     "installed and cancelled again without resetting hooks");
      }
      const auto rebuilt_state = f.state();
      void *captured_building = f.building.pointer();
      void *replacement = f.module.pointer();
      city_upgrade_artisan_callback(&captured_building, &replacement);
      CancelAssembly assembly;
      assembly.request();
      assembly.complete();
      cancel_check(f.state() == rebuilt_state &&
                       f.removed_upgrades ==
                           std::vector<std::int32_t>({upgrade, upgrade}) &&
                       f.releases == std::vector<std::int32_t>({0, 1, 0, 1}),
                   "late callbacks preserve the new workshop's add-on after "
                   "two completed cancellation cycles");
    }
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put<std::int32_t>(0xd48, 4);
    f.building.put<std::int32_t>(0x3b8, 484);
    f.building.put<std::int32_t>(0x14c4, 29);
    f.extension.put<std::int32_t>(0, 13);
    f.asynchronous = true;
    f.set_workshop_upgrades();
    f.set_workshop_visuals();
    f.artisan_entry_body = [&] {
      cancel_check(
          f.upgrades_cleared(),
          "level-four replacement starts without inherited add-on IDs");
    };
    CancelAssembly assembly;
    void *captured_building = f.building.pointer();
    void *replacement = f.module.pointer();
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.upgrades_cleared() && f.removed_upgrades.size() == 3 &&
                     f.building.get<CityArray>(0x370).count == 0 &&
                     upgrade_pending_count() == 1,
                 "level-four cancellation clears add-ons before asynchronous "
                 "house replacement finishes");
    f.artisan_callback_body = [&] {
      f.module.put<std::uint8_t>(0x311, 0);
      f.set_workshop_upgrades();
      f.set_workshop_visuals();
      assembly.request();
    };
    city_upgrade_artisan_callback(&captured_building, &replacement);
    cancel_check(f.upgrades_cleared() && f.removed_upgrades.size() == 6 &&
                     f.building.get<CityArray>(0x370).count == 0 &&
                     f.finishes == 0,
                 "replacement callback cannot reintroduce obsolete add-on IDs");
    f.assembly_body = [&] {
      f.set_workshop_upgrades();
      f.set_workshop_visuals();
    };
    f.finish_body = [&] {
      f.set_workshop_upgrades();
      f.set_workshop_visuals();
    };
    assembly.complete();
    cancel_check(f.upgrades_cleared() && f.removed_upgrades.size() == 12 &&
                     f.finishes == 1 && upgrade_pending_count() == 0 &&
                     f.building.get<std::int32_t>(0xd48) == 4 &&
                     f.building.get<CityArray>(0x370).count == 0 &&
                     f.upgrade_visual_calls == 0,
                 "child assembly and native finalizer cannot restore workshop "
                 "add-ons or change the house level");
    f.artisan_callback_body = {};
    f.assembly_body = {};
    f.finish_body = {};
    const auto finished = f.state();
    assembly.complete();
    city_upgrade_artisan_callback(&captured_building, &replacement);
    cancel_check(f.state() == finished && f.removed_upgrades.size() == 12 &&
                     f.finishes == 1,
                 "duplicate completion callbacks do not repeat native add-on "
                 "removal or construction completion");
  }
}

static auto run_cancel_visual_cache_tests() -> void {
  for (int invalid{}; invalid < 6; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    f.set_workshop_visuals();
    auto cache = f.building.get<CityArray>(0x370);
    if (invalid == 0)
      cache.count = -1;
    else if (invalid == 1)
      cache.capacity = cache.count - 1;
    else if (invalid == 2)
      cache.data = nullptr;
    else if (invalid == 3)
      cache.data = reinterpret_cast<void *>(1);
    else if (invalid == 4)
      cache.capacity = std::numeric_limits<std::int32_t>::max();
    else {
      cache.data = nullptr;
      cache.count = 0;
    }
    f.building.put(0x370, cache);
    expect_cancel_rejected(f, "malformed visual caches reject before changing "
                              "the workshop, completed upgrades or actors");
  }
  {
    const auto release_fixture = [](CancelFixture *fixture) {
      DWORD previous{};
      VirtualProtect(fixture, 4096, PAGE_READWRITE, &previous);
      std::destroy_at(fixture);
      VirtualFree(fixture, 0, MEM_RELEASE);
    };
    void *allocation = VirtualAlloc(nullptr, sizeof(CancelFixture),
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    cancel_check(allocation != nullptr,
                 "allocate isolated cache-header fixture");
    const std::unique_ptr<CancelFixture, decltype(release_fixture)> fixture(
        std::construct_at(static_cast<CancelFixture *>(allocation)),
        release_fixture);
    auto &f = *fixture;
    configure_cancel_fixture(f);
    f.set_workshop_visuals();
    cancel_check(city_can_cancel_artisan(f.building.pointer()),
                 "cache-header fixture is valid before page protection");
    DWORD previous{};
    cancel_check(
        VirtualProtect(allocation, 4096, PAGE_READONLY, &previous) != 0,
        "protect the building page containing the visual cache header");
    expect_cancel_rejected(f,
                           "read-only cache header prevents cancellation "
                           "without relying on writable process-memory APIs");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_visuals();
    const auto release_page = [](void *page) {
      VirtualFree(page, 0, MEM_RELEASE);
    };
    const std::unique_ptr<void, decltype(release_page)> page(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE),
        release_page);
    cancel_check(page != nullptr, "allocate isolated visual record storage");
    std::memcpy(page.get(), f.upgrade_visuals.data(),
                sizeof(f.upgrade_visuals));
    DWORD previous{};
    cancel_check(
        VirtualProtect(page.get(), 4096, PAGE_READONLY, &previous) != 0,
        "protect cached visual records while retaining a writable header");
    f.building.put(0x370, CityArray{page.get(), 2, 3});
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.building.get<CityArray>(0x370).count == 0 &&
                     f.building.get<CityArray>(0x370).data == page.get() &&
                     f.building.get<CityArray>(0x370).capacity == 3 &&
                     std::memcmp(page.get(), f.upgrade_visuals.data(),
                                 sizeof(f.upgrade_visuals)) == 0,
                 "cache invalidation changes only Num and preserves read-only "
                 "record storage and allocation capacity");
    city_change_artisan(f.building.pointer(), 5);
    const auto before = f.state();
    city_begin_upgrade(f.building.pointer(), 37);
    cancel_check(f.upgrade_visual_calls == 0 && f.begin_calls == 0 &&
                     f.state() == before && upgrade_pending_count() == 0,
                 "kettle repurchase rejects read-only retained cache capacity "
                 "before native rebuilding can write to it");
  }
  {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.set_workshop_upgrades();
    f.set_workshop_visuals();
    ExtensionCancelSnapshot snapshot;
    cancel_check(prepare_artisan_cancel(f.building.pointer(), snapshot),
                 "capture visual bindings before an intervening cache update");
    ++f.upgrade_visuals[0].mesh;
    const auto changed = f.state();
    cancel_check(!begin_artisan_cancel(f.building.pointer(), snapshot) &&
                     f.state() == changed && f.removed_upgrades.empty(),
                 "changed visual records invalidate a captured cancellation "
                 "before any purchased upgrade is removed");
  }
  for (const auto [level, type] : {std::pair{2, 8}, {3, 60}, {4, 484}}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.building.put(0xd48, level);
    f.building.put(0x3b8, type);
    f.building.put<std::int32_t>(0x14c4, 22);
    f.extension.put<std::int32_t>(0, 5);
    f.completed_upgrades = {2, 4, 5, 12, 37};
    f.building.put(0x430, CityArray{f.completed_upgrades.data(), 5, 12});
    f.set_workshop_visuals();
    f.modules[1] = f.resource_module.pointer();
    f.building.put(0x2f8, CityArray{f.modules.data(), 2, 3});
    f.resource_modules[0] = f.resource_module.pointer();
    f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
    f.extension_modules[0] = f.resource_module.pointer();
    f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.building.get<CityArray>(0x370).count == 0 &&
                     f.resource_module.get<std::uint32_t>(8) == 0x40000000 &&
                     f.upgrade_visual_calls == 0,
                 "old brewery kettle bindings are invalidated when native "
                 "cancellation destroys the old extension actor");
    f.selection_body = [&] {
      f.modules[1] = f.replacement_resource.pointer();
      f.building.put(0x2f8, CityArray{f.modules.data(), 2, 3});
      f.resource_modules[0] = f.replacement_resource.pointer();
      f.building.put(0x308, CityArray{f.resource_modules.data(), 1, 2});
      f.extension_modules[0] = f.replacement_resource.pointer();
      f.extension.put(0x70, CityArray{f.extension_modules.data(), 1, 2});
    };
    city_change_artisan(f.building.pointer(), 5);
    f.upgrade_visual_module = f.replacement_resource.pointer();
    if (level > 2)
      f.building.put(0x370, CityArray{f.upgrade_visuals.data(), 2, 3});
    f.begin_body = [&](std::int32_t upgrade) {
      auto upgrades = f.building.get<CityArray>(0x430);
      const auto cache = f.building.get<CityArray>(0x370);
      const auto *binding = static_cast<CancelUpgradeVisual *>(cache.data);
      cancel_check(upgrade == 37 && f.upgrade_visual_calls == 1 &&
                       upgrades.count == 4 && cache.count == 1 &&
                       binding[0].upgrade == 37 && binding[0].mesh == 5 &&
                       binding[0].module == f.replacement_resource.pointer(),
                   "native BeginUpgrade37 sees only the new brewery's live "
                   "kettle actor before marking the upgrade purchased");
      f.completed_upgrades[upgrades.count++] = 37;
      f.building.put(0x430, upgrades);
      city_upgrade_cost_setup(f.building.pointer(), 37, 1);
    };
    city_begin_upgrade(f.building.pointer(), 37);
    constexpr std::array<std::int32_t, 5> purchased{2, 4, 5, 12, 37};
    cancel_check(f.begin_calls == 1 && f.upgrade_visual_calls == 1 &&
                     f.building.get<CityArray>(0x430).count == 5 &&
                     std::equal(purchased.begin(), purchased.end(),
                                f.completed_upgrades.begin()) &&
                     f.building.get<std::int32_t>(0xd48) == level &&
                     upgrade_pending_count() == 0,
                 "rebuilt brewery repurchases its kettle with fresh actor "
                 "bindings and preserved house and residential upgrades");
    const auto purchased_state = f.state();
    city_begin_upgrade(f.building.pointer(), 37);
    cancel_check(f.begin_calls == 1 && f.upgrade_visual_calls == 1 &&
                     f.state() == purchased_state &&
                     upgrade_pending_count() == 0,
                 "an already-purchased kettle never rebuilds its visual cache "
                 "or begins the purchase again");
    city_change_extension(f.building.pointer(), 0, 0);
    cancel_check(f.building.get<CityArray>(0x370).count == 0 &&
                     f.building.get<CityArray>(0x430).count == 4 &&
                     f.replacement_resource.get<std::uint32_t>(8) ==
                         0x40000000 &&
                     f.upgrade_visual_calls == 1,
                 "second cancellation removes the new kettle and invalidates "
                 "its replacement actor without replaying visual application");
  }
  for (const std::uint8_t index : {0, 255}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    city_change_artisan(f.building.pointer(), 5);
    CancelAssembly assembly;
    assembly.index = index;
    assembly.request();
    const auto before = f.state();
    city_begin_upgrade(f.building.pointer(), 37);
    cancel_check(f.begin_calls == 0 && f.upgrade_visual_calls == 0 &&
                     f.state() == before && upgrade_pending_count() == 0,
                 "pending house or extension assembly blocks kettle cache "
                 "rebuilding and native purchase");
    f.assembly_body = [&] {
      city_begin_upgrade(f.building.pointer(), 37);
      cancel_check(f.begin_calls == 0 && f.upgrade_visual_calls == 0,
                   "an active native assembly callback cannot rebuild the "
                   "kettle cache before returning");
    };
    assembly.complete();
    city_begin_upgrade(f.building.pointer(), 37);
    cancel_check(
        f.begin_calls == 1 && f.upgrade_visual_calls == 1,
        "completed assembly permits a fresh cache and kettle purchase");
  }
  for (int invalid{}; invalid < 6; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    city_change_artisan(f.building.pointer(), 5);
    f.upgrade_visual_body = [&] {
      if (invalid == 0)
        return;
      if (invalid == 5)
        throw std::runtime_error("synthetic native visual failure");
      f.upgrade_visuals[0] = {37, invalid == 1 ? 0 : 5,
                              invalid == 2   ? f.other.pointer()
                              : invalid == 3 ? f.resource_module.pointer()
                                             : f.module.pointer()};
      f.building.put(0x370, CityArray{f.upgrade_visuals.data(), 1, 3});
      if (invalid == 4) {
        f.completed_upgrades[0] = 37;
        f.building.put(0x430, CityArray{f.completed_upgrades.data(), 1, 12});
      }
    };
    city_begin_upgrade(f.building.pointer(), 37);
    cancel_check(f.upgrade_visual_calls == 1 && f.begin_calls == 0 &&
                     upgrade_pending_count() == 0 &&
                     upgrade_failed_count() != 0,
                 "missing, invalid or foreign visual bindings, purchased-state "
                 "mutation and native failure prevent kettle purchase");
  }
  for (const std::int32_t upgrade : {12, 42, 43, 103}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.begin_body = [&](std::int32_t id) {
      city_upgrade_cost_setup(f.building.pointer(), id, 1);
    };
    const auto before = f.state();
    city_begin_upgrade(f.building.pointer(), upgrade);
    cancel_check(f.begin_calls == 1 && f.upgrade_visual_calls == 0 &&
                     f.state() == before && upgrade_pending_count() == 0,
                 "non-kettle upgrade paths keep native dispatch without "
                 "rebuilding artisan visual bindings");
  }
}

static auto run_cancel_production_tests() -> void {
  for (bool hole : {false, true}) {
    CancelFixture f;
    configure_cancel_fixture(f);
    f.production.put(0xc8,
                     CityArray{f.production_slots.data(), hole ? 3 : 2, 3});
    f.production.put<std::uint32_t>(0xd8, hole ? 5U : 3U);
    f.production.put<std::int32_t>(0xf0, hole ? 3 : 2);
    f.production.put<std::int32_t>(0xf8, hole ? 1 : -1);
    f.production.put<std::int32_t>(0xfc, hole ? 1 : 0);
    f.production.put<std::int32_t>(0x110, 1);
    f.production_slots[0].put<std::int32_t>(0, 348);
    f.production_slots[0].put(8,
                              CityArray{f.production_records[0].data(), 1, 1});
    f.production_slots[hole ? 2 : 1].put<std::int32_t>(0, 28);
    f.production_slots[hole ? 2 : 1].put(
        8, CityArray{f.production_records[1].data(), 1, 1});
    if (hole)
      f.production_slots[1].put(8,
                                CityArray{reinterpret_cast<void *>(1), -1, -1});
    const auto inventory = f.inventory;
    cancel_check(city_can_cancel_artisan(f.building.pointer()),
                 "occupied and sparse production maps validate");
    cancel_check(city_try_cancel_artisan(f.building.pointer(), 0, 0) ==
                     ExtensionCancelResult::handled,
                 "artisan cancellation clears prior goods keys");
    cancel_check(
        f.production_resets == 1 && f.production_frees == 0 &&
            f.production.get<CityArray>(0xc8).count == 0 &&
            f.production.get<std::int64_t>(0xa0) == 12345 &&
            f.production.get<std::uint64_t>(0x120) == 0xfeedf00d &&
            f.inventory == inventory,
        "reset preserves build date, time statistics and actual inventory");
  }
  for (int invalid = 0; invalid < 10; ++invalid) {
    CancelFixture f;
    configure_cancel_fixture(f);
    if (invalid == 0)
      f.building.put<void *>(0x360, nullptr);
    if (invalid == 1)
      f.production.put(0x90, f.other.pointer());
    if (invalid == 2)
      f.production.put<std::int32_t>(0xd0, -1);
    if (invalid == 3)
      f.production.put<std::int32_t>(0xf0, 1);
    if (invalid == 4)
      f.production.put<std::int32_t>(0xf4, 127);
    if (invalid == 5)
      f.production.put<std::int32_t>(0xf4, 65568);
    if (invalid == 6)
      f.production.put<std::int32_t>(0xfc, 1);
    if (invalid == 7)
      f.production.put<std::int32_t>(0xf8, 0);
    if (invalid == 8)
      f.production.put<std::int32_t>(0x110, 3);
    if (invalid == 9)
      f.production.put<std::int32_t>(0x110, 16);
    expect_cancel_rejected(
        f, "invalid production metadata fails closed without mutation");
  }
}

auto run_extension_cancel_tests() -> int {
  const auto previous_image = city_image;
  extension_cancel_checks = 0;
  int result{};
  try {
    city_image = 0x140000000;
    run_cancel_valid_tests();
    run_cancel_settings_tests();
    run_cancel_settings_boundary_tests();
    run_cancel_fuel_lifecycle_tests();
    run_cancel_rejection_tests();
    run_cancel_async_tests();
    run_cancel_duplicate_worker_tests();
    run_cancel_resource_module_tests();
    run_cancel_repeated_cycle_tests();
    run_cancel_upgrade_tests();
    run_cancel_upgrade_boundary_tests();
    run_cancel_upgrade_lifecycle_tests();
    run_cancel_visual_cache_tests();
    run_cancel_production_tests();
    std::printf("Extension cancel tests PASS %zu checks\n",
                extension_cancel_checks);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Extension cancel tests FAIL after %zu checks: %s\n",
                 extension_cancel_checks, error.what());
    result = 1;
  }
  configure_upgrade({});
  reset_extension_cancel();
  cancel_fixture = nullptr;
  city_image = previous_image;
  return result;
}

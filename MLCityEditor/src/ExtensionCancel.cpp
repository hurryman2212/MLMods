#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "ExtensionCancel.hpp"
#include "Upgrade.hpp"

static constexpr std::int32_t maximum_cancel_entries = 65536;
static ExtensionCancelApi extension_cancel_api{};
static std::atomic<std::uint64_t> declined_extension_cancellations{};
// All artisan-only add-ons in native getAllPossibleUpgrades, 0.8.117:
// brewery kettle, butcher's gantry, and bakery stone ovens.
static constexpr std::array<std::int32_t, 3> artisan_upgrade_ids{37, 42, 43};

static auto cancel_array(const CityArray &array) noexcept -> bool {
  return array.count >= 0 && array.capacity >= array.count &&
         array.capacity <= maximum_cancel_entries &&
         (array.count == 0 || array.data != nullptr);
}

struct WorkshopSettings {
  CityArray recipes{};
  CityArray fuel_buildings{};
  std::array<std::uint8_t, 8> fuel{};
  std::int32_t registrations{};
  std::uint8_t needs_fuel{};
};

// UProductionTrackerComponent+0xc8,
// TMap<int32,TArray<FProductionTrackerEntry>>. Entries are 0x20-byte sparse
// slots. Hash/occupancy metadata is not a TArray.
struct WorkshopProductionLog {
  CityArray slots{};
  std::array<std::uint32_t, 4> inline_bits{};
  std::uint32_t *heap_bits{};
  std::int32_t bit_count{}, bit_capacity{128};
  std::int32_t first_free{-1}, free_count{};
  std::array<std::int32_t, 2> inline_hash{};
  std::int32_t *heap_hash{};
  std::int32_t hash_count{}, padding{};
};
static_assert(sizeof(WorkshopProductionLog) == 0x50);

static auto writable_cancel_span(std::uintptr_t address,
                                 std::size_t size) noexcept -> bool {
  if (size == 0)
    return true;
  if (address == 0 ||
      address > std::numeric_limits<std::uintptr_t>::max() - size)
    return false;
  const auto end = address + size;
  while (address < end) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void *>(address), &memory,
                     sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (memory.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ==
            0 ||
        memory.RegionSize == 0 ||
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) >
            std::numeric_limits<std::uintptr_t>::max() - memory.RegionSize)
      return false;
    const auto next = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
                      memory.RegionSize;
    if (next <= address)
      return false;
    address = std::min(end, next);
  }
  return true;
}

static auto read_workshop_production(void *building, void *&tracker,
                                     WorkshopProductionLog &log) -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  void *owner{};
  if (!extension_cancel_api.empty_production_entries ||
      !extension_cancel_api.free_memory ||
      !city_read(address + 0x360, &tracker, sizeof(tracker)) ||
      !city_valid_object(tracker) ||
      !city_read(reinterpret_cast<std::uintptr_t>(tracker) + 0x90, &owner,
                 sizeof(owner)) ||
      owner != building ||
      !city_read(reinterpret_cast<std::uintptr_t>(tracker) + 0xc8, &log,
                 sizeof(log)) ||
      !writable_cancel_span(reinterpret_cast<std::uintptr_t>(tracker) + 0xc8,
                            sizeof(log)) ||
      !cancel_array(log.slots) ||
      (log.slots.capacity != 0 && log.slots.data == nullptr) ||
      log.bit_count != log.slots.count || log.bit_capacity < 128 ||
      log.bit_capacity > maximum_cancel_entries || log.bit_capacity % 32 != 0 ||
      log.bit_capacity < log.bit_count ||
      (log.bit_capacity > 128 && log.heap_bits == nullptr) ||
      log.free_count < 0 || log.free_count > log.slots.count ||
      (log.free_count == 0
           ? log.first_free != -1
           : log.first_free < 0 || log.first_free >= log.slots.count) ||
      log.hash_count < 0 || log.hash_count > maximum_cancel_entries ||
      (log.hash_count != 0 &&
       !std::has_single_bit(static_cast<unsigned>(log.hash_count))) ||
      (log.hash_count > 1 && log.heap_hash == nullptr) ||
      (log.slots.count != log.free_count && log.hash_count == 0) ||
      !writable_cancel_span(reinterpret_cast<std::uintptr_t>(log.slots.data),
                            static_cast<std::size_t>(log.slots.capacity) *
                                0x20) ||
      !writable_cancel_span(
          reinterpret_cast<std::uintptr_t>(log.heap_bits),
          log.heap_bits ? static_cast<std::size_t>(log.bit_capacity) / 8 : 0) ||
      !writable_cancel_span(
          reinterpret_cast<std::uintptr_t>(log.heap_hash),
          log.heap_hash ? static_cast<std::size_t>(log.hash_count) * 4 : 0))
    return false;
  std::vector<std::uint32_t> bits((log.bit_count + 31) / 32);
  if (!bits.empty() &&
      !city_read(reinterpret_cast<std::uintptr_t>(
                     log.heap_bits ? log.heap_bits : log.inline_bits.data()),
                 bits.data(), bits.size() * sizeof(bits[0])))
    return false;
  std::vector<void *> allocations;
  for (void *allocation : {log.slots.data, static_cast<void *>(log.heap_bits),
                           static_cast<void *>(log.heap_hash)}) {
    if (allocation != nullptr) {
      if (std::find(allocations.begin(), allocations.end(), allocation) !=
          allocations.end())
        return false;
      allocations.push_back(allocation);
    }
  }
  std::int32_t occupied{};
  for (std::int32_t i{}; i < log.bit_count; ++i) {
    if ((bits[i / 32] & (std::uint32_t{1} << (i % 32))) == 0)
      continue;
    ++occupied;
    CityArray records{};
    const auto slot = reinterpret_cast<std::uintptr_t>(log.slots.data) +
                      static_cast<std::size_t>(i) * 0x20;
    if (!city_read(slot + 8, &records, sizeof(records)) ||
        !cancel_array(records) ||
        (records.capacity != 0 && records.data == nullptr) ||
        !writable_cancel_span(reinterpret_cast<std::uintptr_t>(records.data),
                              static_cast<std::size_t>(records.capacity) * 16))
      return false;
    if (records.data != nullptr) {
      if (std::find(allocations.begin(), allocations.end(), records.data) !=
          allocations.end())
        return false;
      allocations.push_back(records.data);
    }
  }
  if (!bits.empty() && log.bit_count % 32 != 0 &&
      (bits.back() >> (log.bit_count % 32)) != 0)
    return false;
  return occupied == log.slots.count - log.free_count;
}

static auto reset_workshop_production(void *building) -> bool {
  void *tracker{};
  WorkshopProductionLog detached;
  if (!read_workshop_production(building, tracker, detached))
    return false;
  if (detached.slots.count == detached.free_count)
    return true;
  // A completed artisan cancellation has no workshop left. Detach only its
  // goods log, retaining DateCreated, timeEntries, delegates and region totals.
  // Use the exact native sparse-array destructor used by this component; never
  // zero nested TArrays in place or release game allocations with the CRT.
  const WorkshopProductionLog empty;
  SIZE_T written{};
  if (WriteProcessMemory(GetCurrentProcess(),
                         reinterpret_cast<void *>(
                             reinterpret_cast<std::uintptr_t>(tracker) + 0xc8),
                         &empty, sizeof(empty), &written) == 0 ||
      written != sizeof(empty))
    return false;
  extension_cancel_api.empty_production_entries(&detached, 0);
  for (void *allocation :
       {static_cast<void *>(detached.heap_hash),
        static_cast<void *>(detached.heap_bits), detached.slots.data})
    if (allocation != nullptr)
      extension_cancel_api.free_memory(allocation);
  WorkshopProductionLog current;
  return read_workshop_production(building, tracker, current) &&
         current.slots.count == 0 && current.free_count == 0;
}

static auto read_cancel_upgrades(void *building,
                                 std::vector<std::int32_t> &upgrades) -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(building) + 0x430;
  CityArray applied{};
  if (!extension_cancel_api.remove_upgrade ||
      !city_read(address, &applied, sizeof(applied)) ||
      !cancel_array(applied) ||
      (applied.capacity != 0 && applied.data == nullptr) ||
      !writable_cancel_span(address, sizeof(applied)) ||
      !writable_cancel_span(reinterpret_cast<std::uintptr_t>(applied.data),
                            static_cast<std::size_t>(applied.count) *
                                sizeof(std::int32_t)))
    return false;
  upgrades.resize(applied.count);
  return upgrades.empty() ||
         city_read(reinterpret_cast<std::uintptr_t>(applied.data),
                   upgrades.data(), upgrades.size() * sizeof(std::int32_t));
}

static auto
read_cancel_visuals(void *building, CityArray &cache,
                    std::vector<std::array<std::uint8_t, 16>> &records)
    -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(building) + 0x370;
  if (!city_read(address, &cache, sizeof(cache)) || !cancel_array(cache) ||
      (cache.capacity != 0 && cache.data == nullptr) ||
      !writable_cancel_span(address, sizeof(cache)))
    return false;
  records.resize(cache.count);
  return records.empty() ||
         city_read(reinterpret_cast<std::uintptr_t>(cache.data), records.data(),
                   records.size() * sizeof(records[0]));
}

static auto invalidate_artisan_visuals(void *building) -> bool {
  CityArray cache{}, cleared{};
  std::vector<std::array<std::uint8_t, 16>> records;
  if (!read_cancel_visuals(building, cache, records))
    return false;
  if (cache.count == 0)
    return true;
  // This is a derived cache of upgrade tags, not purchased upgrades. Native
  // ApplyVisualUpgrades rebuilds it only when Num is zero. Keeping unrelated
  // cached records would stop a rebuilt brewery from finding its new actors.
  const auto address = reinterpret_cast<std::uintptr_t>(building) + 0x370;
  const std::int32_t zero{};
  SIZE_T written{};
  return WriteProcessMemory(GetCurrentProcess(),
                            reinterpret_cast<void *>(address + 8), &zero,
                            sizeof(zero), &written) != 0 &&
         written == sizeof(zero) &&
         city_read(address, &cleared, sizeof(cleared)) && cleared.count == 0 &&
         cleared.data == cache.data && cleared.capacity == cache.capacity;
}

static auto reset_artisan_upgrades(void *building,
                                   std::vector<std::int32_t> &upgrades)
    -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(building) + 0x430;
  for (const auto id : artisan_upgrade_ids) {
    const auto count = std::count(upgrades.begin(), upgrades.end(), id);
    if (count == 0)
      continue;
    // Use the same native stable RemoveInt as changeExtension. Removing the
    // complete artisan-only set also clears add-ons left by an older workshop.
    if (extension_cancel_api.remove_upgrade(reinterpret_cast<void *>(address),
                                            &id) != count)
      return false;
    std::erase(upgrades, id);
    std::vector<std::int32_t> current;
    if (!read_cancel_upgrades(building, current) || current != upgrades)
      return false;
  }
  return true;
}

static auto read_workshop_settings(void *building, void *region,
                                   WorkshopSettings &settings) noexcept
    -> bool {
  if (!extension_cancel_api.disable_recipe ||
      !extension_cancel_api.remove_pointer ||
      !extension_cancel_api.refresh_resources ||
      !extension_cancel_api.refresh_region ||
      !extension_cancel_api.building_stats ||
      !extension_cancel_api.grow_pointers)
    return false;
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  std::int32_t type{};
  std::uint8_t function{}, definition_fuel{};
  if (!city_read(address + 0x3b8, &type, sizeof(type)) ||
      !city_read(address + 0x3cc, &function, sizeof(function)))
    return false;
  const void *definition = extension_cancel_api.building_stats(type);
  if (definition == nullptr ||
      !city_read(reinterpret_cast<std::uintptr_t>(definition) + 0x299,
                 &definition_fuel, sizeof(definition_fuel)))
    return false;
  // initializeBuilding uses the current building definition, independently
  // of artisanJob. Normal burgage plots also remain fuel consumers.
  settings.needs_fuel = function == 2 || definition_fuel != 0;
  // The fuel fields occupy e21/e22/e24..e28. e23 is padding and is never
  // written. Validate every span modified by the native TArray removals too.
  if (!writable_cancel_span(address + 0xe71, settings.fuel.size()) ||
      !city_read(address + 0xe71, settings.fuel.data(), settings.fuel.size()) ||
      !city_read(address + 0x14c8, &settings.recipes,
                 sizeof(settings.recipes)) ||
      !cancel_array(settings.recipes) ||
      !writable_cancel_span(address + 0x14c8, sizeof(settings.recipes)) ||
      !writable_cancel_span(
          reinterpret_cast<std::uintptr_t>(settings.recipes.data),
          static_cast<std::size_t>(settings.recipes.count) *
              sizeof(std::int32_t)) ||
      !city_read(reinterpret_cast<std::uintptr_t>(region) + 0x708,
                 &settings.fuel_buildings, sizeof(settings.fuel_buildings)) ||
      !cancel_array(settings.fuel_buildings) ||
      !writable_cancel_span(reinterpret_cast<std::uintptr_t>(region) + 0x708,
                            sizeof(settings.fuel_buildings)) ||
      !writable_cancel_span(
          reinterpret_cast<std::uintptr_t>(settings.fuel_buildings.data),
          static_cast<std::size_t>(settings.fuel_buildings.count) *
              sizeof(void *)))
    return false;
  for (std::int32_t i{}; i < settings.recipes.count; ++i) {
    std::int32_t recipe{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(settings.recipes.data) +
                       static_cast<std::size_t>(i) * sizeof(recipe),
                   &recipe, sizeof(recipe)))
      return false;
  }
  settings.registrations = 0;
  for (std::int32_t i{}; i < settings.fuel_buildings.count; ++i) {
    void *registered{};
    if (!city_read(
            reinterpret_cast<std::uintptr_t>(settings.fuel_buildings.data) +
                static_cast<std::size_t>(i) * sizeof(registered),
            &registered, sizeof(registered)))
      return false;
    settings.registrations += registered == building;
  }
  if (settings.needs_fuel != 0 && settings.registrations == 0 &&
      (settings.fuel_buildings.count >= maximum_cancel_entries ||
       (settings.fuel_buildings.count < settings.fuel_buildings.capacity &&
        !writable_cancel_span(
            reinterpret_cast<std::uintptr_t>(settings.fuel_buildings.data) +
                static_cast<std::size_t>(settings.fuel_buildings.count) *
                    sizeof(void *),
            sizeof(void *)))))
    return false;
  return true;
}

static auto reset_workshop_settings(void *building, void *region) -> bool {
  WorkshopSettings settings;
  if (!read_workshop_settings(building, region, settings))
    return false;
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  // Reset workshop preferences/status to their constructor defaults while
  // retaining the fuel requirement derived by initializeBuilding from the
  // current house definition. Inventory and construction priority are separate.
  const bool fuel_changed = settings.fuel[0] != settings.needs_fuel ||
                            settings.fuel[1] != 0 || settings.fuel[3] != 0xff ||
                            settings.fuel[4] != 0xff ||
                            settings.fuel[5] != 0xff ||
                            settings.fuel[6] != 0xff || settings.fuel[7] != 0;
  if (!fuel_changed && settings.registrations == settings.needs_fuel &&
      settings.recipes.count == 0)
    return true;
  while (settings.recipes.count > 0) {
    std::int32_t recipe{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(settings.recipes.data),
                   &recipe, sizeof(recipe)))
      return false;
    const auto previous_count = settings.recipes.count;
    extension_cancel_api.disable_recipe(building, recipe);
    if (!city_read(address + 0x14c8, &settings.recipes,
                   sizeof(settings.recipes)) ||
        !cancel_array(settings.recipes) ||
        settings.recipes.count >= previous_count)
      return false;
  }
  const std::array<std::uint8_t, 2> status{settings.needs_fuel, 0};
  constexpr std::array<std::uint8_t, 5> day_and_stock{0xff, 0xff, 0xff, 0xff,
                                                      0};
  SIZE_T written{};
  if (WriteProcessMemory(GetCurrentProcess(),
                         reinterpret_cast<void *>(address + 0xe71),
                         status.data(), status.size(), &written) == 0 ||
      written != status.size() ||
      WriteProcessMemory(
          GetCurrentProcess(), reinterpret_cast<void *>(address + 0xe74),
          day_and_stock.data(), day_and_stock.size(), &written) == 0 ||
      written != day_and_stock.size())
    return false;
  WorkshopSettings current;
  if (!read_workshop_settings(building, region, current))
    return false;
  const auto fuel_array_address =
      reinterpret_cast<std::uintptr_t>(region) + 0x708;
  if (current.registrations > current.needs_fuel) {
    extension_cancel_api.remove_pointer(
        reinterpret_cast<void *>(fuel_array_address), &building);
    if (!read_workshop_settings(building, region, current) ||
        current.registrations != 0)
      return false;
  }
  if (current.needs_fuel != 0 && current.registrations == 0) {
    const auto old_count = current.fuel_buildings.count;
    const auto new_count = old_count + 1;
    struct RestoreFuelCount {
      std::uintptr_t address;
      std::int32_t count;
      bool committed{};
      ~RestoreFuelCount() {
        if (!committed) {
          SIZE_T restored{};
          WriteProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<void *>(address), &count,
                             sizeof(count), &restored);
        }
      }
    } restore{fuel_array_address + 8, old_count};
    if (WriteProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<void *>(fuel_array_address + 8),
                           &new_count, sizeof(new_count), &written) == 0 ||
        written != sizeof(new_count))
      return false;
    if (new_count > current.fuel_buildings.capacity)
      extension_cancel_api.grow_pointers(
          reinterpret_cast<void *>(fuel_array_address), old_count);
    CityArray grown{};
    if (!city_read(fuel_array_address, &grown, sizeof(grown)) ||
        !cancel_array(grown) || grown.count != new_count ||
        !writable_cancel_span(reinterpret_cast<std::uintptr_t>(grown.data) +
                                  static_cast<std::size_t>(old_count) *
                                      sizeof(void *),
                              sizeof(void *)) ||
        WriteProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<void *>(
                reinterpret_cast<std::uintptr_t>(grown.data) +
                static_cast<std::size_t>(old_count) * sizeof(void *)),
            &building, sizeof(building), &written) == 0 ||
        written != sizeof(building))
      return false;
    restore.committed = true;
  }
  // DisableRecipe uses these same native refreshes. Run them once after the
  // fuel reset too, including a workshop whose recipe list was already empty.
  extension_cancel_api.refresh_resources(building);
  extension_cancel_api.refresh_region(region, building, 1, nullptr, 0);
  WorkshopSettings finished;
  return read_workshop_settings(building, region, finished) &&
         finished.recipes.count == 0 &&
         finished.registrations == finished.needs_fuel &&
         finished.fuel[0] == finished.needs_fuel && finished.fuel[1] == 0 &&
         finished.fuel[3] == 0xff && finished.fuel[4] == 0xff &&
         finished.fuel[5] == 0xff && finished.fuel[6] == 0xff &&
         finished.fuel[7] == 0;
}

auto configure_extension_cancel(const ExtensionCancelApi &api) noexcept
    -> void {
  extension_cancel_api = api;
  reset_extension_cancel();
}

auto reset_extension_cancel() noexcept -> void {
  declined_extension_cancellations = 0;
}

auto extension_cancel_declined_count() noexcept -> std::uint64_t {
  return declined_extension_cancellations;
}

auto decline_artisan_cancel() noexcept -> void {
  ++declined_extension_cancellations;
}

auto prepare_artisan_cancel(void *building,
                            ExtensionCancelSnapshot &snapshot) noexcept
    -> bool {
  if (!extension_cancel_api.release_family ||
      !city_upgrade_cancel_ready(building))
    return false;
  try {
    const auto address = reinterpret_cast<std::uintptr_t>(building);
    void *master{}, *region{};
    CityArray extensions{}, residents{}, workers{}, families{}, costs{},
        modules{}, resources{}, extension_modules{};
    std::uint8_t function{}, constructed{}, upgrading{}, running{};
    std::uint8_t actor_flags{}, fire{};
    std::array<std::uint8_t, 4> transitions{};
    std::int32_t level{}, artisan{}, extension{};
    if (!city_read(address + 0x3cc, &function, sizeof(function)) ||
        function != 1 || !city_read(address + 0xd48, &level, sizeof(level)) ||
        level < 1 || level > 4 ||
        !city_read(address + 0x3c1, &constructed, sizeof(constructed)) ||
        constructed != 1 ||
        !city_read(address + 0x3a1, &upgrading, sizeof(upgrading)) ||
        upgrading != 0 ||
        !city_read(address + 0x5d, &actor_flags, sizeof(actor_flags)) ||
        (actor_flags & 0x01U) != 0 ||
        !city_read(address + 0x388, &fire, sizeof(fire)) || fire != 0 ||
        !city_read(address + 0x3a0, transitions.data(), transitions.size()) ||
        std::any_of(transitions.begin(), transitions.end(),
                    [](auto value) { return value != 0; }) ||
        !city_read(address + 0x14c4, &artisan, sizeof(artisan)) ||
        !city_read(address + 0x2e8, &master, sizeof(master)) ||
        !city_valid_object(master) ||
        !city_read(reinterpret_cast<std::uintptr_t>(master) + 0x318, &running,
                   sizeof(running)) ||
        running != 1 || !city_read(address + 0x2c8, &region, sizeof(region)) ||
        !city_valid_object(region) ||
        !city_read(address + 0x3d0, &costs, sizeof(costs)) ||
        !cancel_array(costs) || costs.count != 0 ||
        !city_read(address + 0xb88, &extensions, sizeof(extensions)) ||
        !cancel_array(extensions) || extensions.count < 1 ||
        !city_read(reinterpret_cast<std::uintptr_t>(extensions.data),
                   &extension, sizeof(extension)))
      return false;
    // changeArtisanType's native extension-to-occupation mapping, 0.8.117.
    constexpr std::array<std::int32_t, 17> artisan_jobs{
        0, 0, 0, 16, 38, 22, 10, 11, 0, 20, 56, 57, 0, 29, 0, 71, 74};
    if (extension < 1 || extension >= static_cast<int>(artisan_jobs.size()) ||
        artisan_jobs[extension] == 0 || artisan != artisan_jobs[extension] ||
        !city_read(address + 0x1038, &residents, sizeof(residents)) ||
        !cancel_array(residents) ||
        !city_read(address + 0x1028, &workers, sizeof(workers)) ||
        !cancel_array(workers) ||
        !city_read(reinterpret_cast<std::uintptr_t>(region) + 0x4e8, &families,
                   sizeof(families)) ||
        !cancel_array(families))
      return false;
    WorkshopSettings settings;
    void *production_tracker{};
    WorkshopProductionLog production_log;
    std::vector<std::int32_t> applied_upgrades;
    CityArray visual_cache{};
    std::vector<std::array<std::uint8_t, 16>> visual_upgrades;
    if (!read_workshop_production(building, production_tracker,
                                  production_log) ||
        !read_workshop_settings(building, region, settings) ||
        !read_cancel_upgrades(building, applied_upgrades) ||
        !read_cancel_visuals(building, visual_cache, visual_upgrades))
      return false;
    if (!city_read(address + 0x2f8, &modules, sizeof(modules)) ||
        !cancel_array(modules) ||
        !city_read(address + 0x308, &resources, sizeof(resources)) ||
        !cancel_array(resources) ||
        !city_read(reinterpret_cast<std::uintptr_t>(extensions.data) + 0x70,
                   &extension_modules, sizeof(extension_modules)) ||
        !cancel_array(extension_modules))
      return false;
    std::vector<void *> module_objects(modules.count);
    std::vector<void *> resource_objects(resources.count);
    if (!module_objects.empty() &&
        !city_read(reinterpret_cast<std::uintptr_t>(modules.data),
                   module_objects.data(),
                   module_objects.size() * sizeof(void *)))
      return false;
    if (!resource_objects.empty() &&
        !city_read(reinterpret_cast<std::uintptr_t>(resources.data),
                   resource_objects.data(),
                   resource_objects.size() * sizeof(void *)))
      return false;
    // Native changeExtension removes extension actors from both registries;
    // resource actors need not also be present in the main-module array.
    for (std::int32_t i{}; i < extension_modules.count; ++i) {
      void *module{};
      void *parent{};
      std::uint8_t flags{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(extension_modules.data) +
                         static_cast<std::size_t>(i) * sizeof(void *),
                     &module, sizeof(module)) ||
          !city_valid_object(module) ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x5d, &flags,
                     sizeof(flags)) ||
          (flags & 0x01U) != 0 ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x2c8, &parent,
                     sizeof(parent)) ||
          parent != building ||
          (std::find(module_objects.begin(), module_objects.end(), module) ==
               module_objects.end() &&
           std::find(resource_objects.begin(), resource_objects.end(),
                     module) == resource_objects.end()))
        return false;
    }
    if (level == 4) {
      std::int32_t main_type{};
      if (!city_read(address + 0x890, &main_type, sizeof(main_type)))
        return false;
      bool main_found{};
      for (void *module : module_objects) {
        std::int32_t type{};
        if (!city_read(reinterpret_cast<std::uintptr_t>(module) + 0x2c0, &type,
                       sizeof(type)))
          return false;
        main_found = main_found || type == main_type;
      }
      if (!main_found)
        return false;
    }
    std::vector<std::int32_t> resident_ids(residents.count);
    std::vector<std::int32_t> worker_ids(workers.count);
    std::vector<std::vector<void *>> family_members;
    family_members.reserve(residents.count);
    if ((!resident_ids.empty() &&
         !city_read(reinterpret_cast<std::uintptr_t>(residents.data),
                    resident_ids.data(),
                    resident_ids.size() * sizeof(std::int32_t))) ||
        (!worker_ids.empty() &&
         !city_read(reinterpret_cast<std::uintptr_t>(workers.data),
                    worker_ids.data(),
                    worker_ids.size() * sizeof(std::int32_t))))
      return false;
    std::sort(resident_ids.begin(), resident_ids.end());
    std::sort(worker_ids.begin(), worker_ids.end());
    // Native releaseWorkerFamily uses RemoveInt, which removes every matching
    // worker ID. Release once per unique resident after validating reciprocity.
    worker_ids.erase(std::unique(worker_ids.begin(), worker_ids.end()),
                     worker_ids.end());
    if (resident_ids != worker_ids ||
        std::adjacent_find(resident_ids.begin(), resident_ids.end()) !=
            resident_ids.end())
      return false;
    for (const auto id : resident_ids) {
      if (id < 0 || id >= families.count)
        return false;
      const auto family = reinterpret_cast<std::uintptr_t>(families.data) +
                          static_cast<std::size_t>(id) * 0x50;
      void *workplace{}, *home{};
      CityArray members{};
      if (!city_read(family + 0x10, &workplace, sizeof(workplace)) ||
          workplace != building ||
          !city_read(family + 0x20, &home, sizeof(home)) || home != building ||
          !city_read(family, &members, sizeof(members)) ||
          !cancel_array(members) || members.count == 0)
        return false;
      auto &identities = family_members.emplace_back();
      identities.reserve(members.count);
      for (std::int32_t i{}; i < members.count; ++i) {
        void *member{};
        void *member_owner{}, *member_region{}, *member_home{},
            *member_workplace{};
        std::int32_t member_family{};
        if (!city_read(reinterpret_cast<std::uintptr_t>(members.data) +
                           static_cast<std::size_t>(i) * sizeof(void *),
                       &member, sizeof(member)) ||
            !city_valid_object(member) ||
            !city_read(reinterpret_cast<std::uintptr_t>(member) + 0x348,
                       &member_family, sizeof(member_family)) ||
            member_family != id ||
            !city_read(reinterpret_cast<std::uintptr_t>(member) + 0x320,
                       &member_owner, sizeof(member_owner)) ||
            member_owner != city_building_owner(building) ||
            !city_read(reinterpret_cast<std::uintptr_t>(member) + 0x328,
                       &member_region, sizeof(member_region)) ||
            member_region != region ||
            !city_read(reinterpret_cast<std::uintptr_t>(member) + 0x340,
                       &member_home, sizeof(member_home)) ||
            member_home != building ||
            !city_read(reinterpret_cast<std::uintptr_t>(member) + 0x338,
                       &member_workplace, sizeof(member_workplace)) ||
            (member_workplace != nullptr && member_workplace != building))
          return false;
        identities.push_back(member);
      }
    }
    snapshot.region = region;
    snapshot.extension_count = extensions.count;
    snapshot.families = std::move(resident_ids);
    snapshot.members = std::move(family_members);
    snapshot.upgrades = std::move(applied_upgrades);
    snapshot.visual_upgrades = std::move(visual_upgrades);
    return true;
  } catch (...) {
    return false;
  }
}

auto city_can_cancel_artisan(void *building) noexcept -> bool {
  ExtensionCancelSnapshot snapshot;
  return prepare_artisan_cancel(building, snapshot);
}

auto begin_artisan_cancel(void *building,
                          const ExtensionCancelSnapshot &snapshot) -> bool {
  std::vector<std::int32_t> upgrades;
  CityArray cache{};
  std::vector<std::array<std::uint8_t, 16>> records;
  return read_cancel_upgrades(building, upgrades) &&
         upgrades == snapshot.upgrades &&
         read_cancel_visuals(building, cache, records) &&
         records == snapshot.visual_upgrades &&
         reset_artisan_upgrades(building, upgrades) &&
         invalidate_artisan_visuals(building);
}

auto prepare_artisan_upgrade_visuals(void *building) noexcept -> bool {
  if (!extension_cancel_api.apply_upgrade_visuals ||
      !city_upgrade_cancel_ready(building))
    return false;
  try {
    const auto address = reinterpret_cast<std::uintptr_t>(building);
    std::uint8_t function{}, constructed{}, upgrading{}, actor_flags{}, fire{};
    std::array<std::uint8_t, 4> transitions{};
    std::int32_t artisan{}, extension{}, level{};
    CityArray extensions{}, modules{}, costs{}, cache{};
    void *master{}, *region{};
    std::uint8_t running{};
    std::vector<std::int32_t> upgrades;
    std::vector<std::array<std::uint8_t, 16>> records;
    if (!city_read(address + 0x3cc, &function, sizeof(function)) ||
        function != 1 ||
        !city_read(address + 0x3c1, &constructed, sizeof(constructed)) ||
        constructed != 1 ||
        !city_read(address + 0x3a1, &upgrading, sizeof(upgrading)) ||
        upgrading != 0 ||
        !city_read(address + 0x5d, &actor_flags, sizeof(actor_flags)) ||
        (actor_flags & 1U) != 0 ||
        !city_read(address + 0x388, &fire, sizeof(fire)) || fire != 0 ||
        !city_read(address + 0x3a0, transitions.data(), transitions.size()) ||
        std::any_of(transitions.begin(), transitions.end(),
                    [](auto value) { return value != 0; }) ||
        !city_read(address + 0xd48, &level, sizeof(level)) || level < 1 ||
        level > 4 || !city_read(address + 0x14c4, &artisan, sizeof(artisan)) ||
        artisan != 22 ||
        !city_read(address + 0xb88, &extensions, sizeof(extensions)) ||
        !cancel_array(extensions) || extensions.count < 1 ||
        !city_read(reinterpret_cast<std::uintptr_t>(extensions.data),
                   &extension, sizeof(extension)) ||
        extension != 5 ||
        !city_read(address + 0x2e8, &master, sizeof(master)) ||
        !city_valid_object(master) ||
        !city_read(reinterpret_cast<std::uintptr_t>(master) + 0x318, &running,
                   sizeof(running)) ||
        running != 1 || !city_read(address + 0x2c8, &region, sizeof(region)) ||
        !city_valid_object(region) ||
        !city_read(address + 0x3d0, &costs, sizeof(costs)) ||
        !cancel_array(costs) || costs.count != 0 ||
        !read_cancel_upgrades(building, upgrades) ||
        std::find(upgrades.begin(), upgrades.end(), 37) != upgrades.end() ||
        !read_cancel_visuals(building, cache, records) ||
        !writable_cancel_span(reinterpret_cast<std::uintptr_t>(cache.data),
                              static_cast<std::size_t>(cache.capacity) * 16) ||
        !city_read(address + 0x2f8, &modules, sizeof(modules)) ||
        !cancel_array(modules))
      return false;
    std::vector<void *> module_objects(modules.count);
    if (!module_objects.empty() &&
        !city_read(reinterpret_cast<std::uintptr_t>(modules.data),
                   module_objects.data(),
                   module_objects.size() * sizeof(void *)))
      return false;
    for (void *module : module_objects) {
      void *parent{};
      CityArray tags{};
      std::uint8_t flags{};
      if (!city_valid_object(module) ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x2c8, &parent,
                     sizeof(parent)) ||
          parent != building ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x5d, &flags,
                     sizeof(flags)) ||
          (flags & 1U) != 0 ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x1e0, &tags,
                     sizeof(tags)) ||
          !cancel_array(tags))
        return false;
      for (std::int32_t i{}; i < tags.count; ++i) {
        std::uint64_t tag{};
        if (!city_read(reinterpret_cast<std::uintptr_t>(tags.data) +
                           static_cast<std::size_t>(i) * sizeof(tag),
                       &tag, sizeof(tag)))
          return false;
      }
    }
    if (!invalidate_artisan_visuals(building))
      return false;
    // Native cache construction scans the current modules. ID37 must still
    // be absent here: its normal BeginUpgrade/finalizer applies the new mesh.
    extension_cancel_api.apply_upgrade_visuals(building);
    std::vector<std::int32_t> after;
    if (!read_cancel_upgrades(building, after) || after != upgrades ||
        !read_cancel_visuals(building, cache, records))
      return false;
    bool found{};
    for (std::int32_t i{}; i < cache.count; ++i) {
      const auto record = reinterpret_cast<std::uintptr_t>(cache.data) +
                          static_cast<std::size_t>(i) * 16;
      std::int32_t id{}, mesh{};
      void *module{};
      if (!city_read(record, &id, sizeof(id)) ||
          !city_read(record + 4, &mesh, sizeof(mesh)) ||
          !city_read(record + 8, &module, sizeof(module)))
        return false;
      if (id == 37) {
        if (mesh <= 0 ||
            std::find(module_objects.begin(), module_objects.end(), module) ==
                module_objects.end() ||
            !city_valid_object(module))
          return false;
        found = true;
      }
    }
    return found;
  } catch (...) {
    return false;
  }
}

auto finish_artisan_cancel(void *building,
                           const ExtensionCancelSnapshot &snapshot) -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  void *region{};
  CityArray extensions{}, families{};
  std::int32_t artisan{}, extension{};
  if (!extension_cancel_api.release_family || !city_player_building(building) ||
      !city_read(address + 0x2c8, &region, sizeof(region)) ||
      region != snapshot.region || !city_valid_object(region) ||
      !city_read(address + 0x14c4, &artisan, sizeof(artisan)) || artisan != 0 ||
      !city_read(address + 0xb88, &extensions, sizeof(extensions)) ||
      !cancel_array(extensions) ||
      extensions.count != snapshot.extension_count || extensions.count < 1 ||
      !city_read(reinterpret_cast<std::uintptr_t>(extensions.data), &extension,
                 sizeof(extension)) ||
      extension != 0 || snapshot.members.size() != snapshot.families.size())
    return false;
  for (std::size_t family_index{}; family_index < snapshot.families.size();
       ++family_index) {
    const auto id = snapshot.families[family_index];
    if (!city_read(reinterpret_cast<std::uintptr_t>(region) + 0x4e8, &families,
                   sizeof(families)) ||
        !cancel_array(families) || id < 0 || id >= families.count)
      return false;
    const auto family = reinterpret_cast<std::uintptr_t>(families.data) +
                        static_cast<std::size_t>(id) * 0x50;
    void *workplace{}, *home{};
    CityArray members{};
    if (!city_read(family + 0x10, &workplace, sizeof(workplace)) ||
        !city_read(family + 0x20, &home, sizeof(home)) || home != building ||
        !city_read(family, &members, sizeof(members)) ||
        !cancel_array(members) ||
        static_cast<std::size_t>(members.count) !=
            snapshot.members[family_index].size())
      return false;
    for (std::int32_t i{}; i < members.count; ++i) {
      void *member{};
      std::int32_t member_family{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(members.data) +
                         static_cast<std::size_t>(i) * sizeof(void *),
                     &member, sizeof(member)) ||
          member != snapshot.members[family_index][i] ||
          !city_valid_object(member) ||
          !city_read(reinterpret_cast<std::uintptr_t>(member) + 0x348,
                     &member_family, sizeof(member_family)) ||
          member_family != id)
        return false;
    }
    // The native reconstruction finalizer can assign ordinary jobs. Only
    // remove the former workshop assignment; preserve any new workplace.
    if (workplace == building) {
      extension_cancel_api.release_family(region, id);
      if (!city_read(family + 0x10, &workplace, sizeof(workplace)) ||
          workplace != nullptr)
        return false;
    }
  }
  // Assemblies and the native finalizer can run after the initial reset.
  // Revalidate and clear only artisan add-ons before reporting completion.
  std::vector<std::int32_t> upgrades;
  return read_cancel_upgrades(building, upgrades) &&
         reset_artisan_upgrades(building, upgrades) &&
         invalidate_artisan_visuals(building) &&
         reset_workshop_settings(building, region) &&
         reset_workshop_production(building);
}

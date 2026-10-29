#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "Windmill.hpp"

// Defensive container bounds; malformed or incomplete assemblies fail closed.
static constexpr std::int32_t maximum_buildings = 65536;
static constexpr std::int32_t maximum_windmill_modules = 64;
static constexpr std::uint64_t scan_interval_ms = 1000;

static WindmillApi windmill_api{};
static std::atomic<std::uint64_t> windmill_repairs{};
static thread_local bool scanning_windmills{};
static void *last_scanned_master{};
static std::int32_t last_scanned_index{};
static std::uint64_t last_scan_ms{};
static bool have_scan_time{};

static auto read_windmill_array(std::uintptr_t address, std::int32_t limit,
                                std::vector<void *> &values) -> bool {
  CityArray header{};
  if (!city_read(address, &header, sizeof(header)) || header.count < 0 ||
      header.count > limit || header.capacity < header.count ||
      header.capacity > limit) {
    return false;
  }
  const auto data = reinterpret_cast<std::uintptr_t>(header.data);
  const auto size = static_cast<std::size_t>(header.count) * sizeof(void *);
  if ((header.capacity != 0 && data == 0) || data % alignof(void *) != 0 ||
      data > std::numeric_limits<std::uintptr_t>::max() - size) {
    return false;
  }
  values.resize(static_cast<std::size_t>(header.count));
  return size == 0 || city_read(data, values.data(), size);
}

static auto eligible_windmill(void *master, void *pawn, void *building)
    -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  const auto master_address = reinterpret_cast<std::uintptr_t>(master);
  std::uintptr_t vtable{};
  void *main_pawn{}, *pawn_master{}, *building_master{};
  std::int32_t type{};
  std::uint8_t ready{}, constructed{}, on_fire{}, actor_flags{};
  std::array<std::uint8_t, 4> transition{};
  if (!city_valid_object(master) ||
      !city_read(master_address + 0x318, &ready, sizeof(ready)) || ready != 1 ||
      !city_read(master_address + 0x5b0, &main_pawn, sizeof(main_pawn)) ||
      main_pawn != pawn || !city_player_building(building, pawn) ||
      !city_read(reinterpret_cast<std::uintptr_t>(pawn) + 0x340, &pawn_master,
                 sizeof(pawn_master)) ||
      pawn_master != master || !city_read(address, &vtable, sizeof(vtable)) ||
      vtable != city_image + 0x7f51b90 ||
      !city_read(address + 0x5d, &actor_flags, sizeof(actor_flags)) ||
      (actor_flags & 0x01U) != 0 ||
      !city_read(address + 0x2e8, &building_master, sizeof(building_master)) ||
      building_master != master ||
      !city_read(address + 0x3b8, &type, sizeof(type)) || type != 20 ||
      !city_read(address + 0x3c1, &constructed, sizeof(constructed)) ||
      constructed != 1 ||
      !city_read(address + 0x388, &on_fire, sizeof(on_fire)) || on_fire != 0 ||
      !city_read(address + 0x3a0, transition.data(), transition.size()) ||
      std::any_of(transition.begin(), transition.end(),
                  [](std::uint8_t flag) { return flag != 0; })) {
    return false;
  }

  std::vector<void *> modules;
  if (!read_windmill_array(address + 0x2f8, maximum_windmill_modules,
                           modules)) {
    return false;
  }
  bool base{}, top{};
  for (void *module : modules) {
    const auto module_address = reinterpret_cast<std::uintptr_t>(module);
    void *parent{};
    std::int32_t model{};
    std::uint8_t replacing{};
    std::array<float, 2> work{};
    if (!city_valid_object(module) ||
        !city_read(module_address, &vtable, sizeof(vtable)) ||
        vtable != city_image + 0x7f51448 ||
        !city_read(module_address + 0x5d, &actor_flags, sizeof(actor_flags)) ||
        (actor_flags & 0x01U) != 0 ||
        !city_read(module_address + 0x2c8, &parent, sizeof(parent)) ||
        parent != building ||
        !city_read(module_address + 0x2c0, &model, sizeof(model)) ||
        !city_read(module_address + 0x311, &replacing, sizeof(replacing)) ||
        replacing != 0 ||
        !city_read(module_address + 0x314, work.data(), sizeof(work)) ||
        !std::isfinite(work[0]) || !std::isfinite(work[1]) || work[1] < 0.0F ||
        work[0] < work[1]) {
      return false;
    }
    // Native instant construction leaves +0x312/+0x313 set even at 200/200.
    // The confirmed 0.8.117 windmill assembly uses base 43 and top 458.
    base = base || model == 43;
    top = top || model == 458;
  }
  return base && top;
}

auto configure_windmill(const WindmillApi &api) -> void {
  windmill_api = api;
  last_scanned_master = nullptr;
  last_scanned_index = 0;
  last_scan_ms = 0;
  have_scan_time = false;
  windmill_repairs.store(0, std::memory_order_relaxed);
}

auto city_windmill_repair_count() noexcept -> std::uint64_t {
  return windmill_repairs.load(std::memory_order_relaxed);
}

auto city_repair_windmills(void *master) -> void {
  if (scanning_windmills || windmill_api.find_component == nullptr ||
      windmill_api.activate == nullptr || !city_valid_object(master)) {
    return;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(master);
  void *pawn{}, *pawn_master{};
  std::uint8_t ready{};
  std::int32_t object_index{};
  if (!city_read(address + 0x318, &ready, sizeof(ready)) || ready != 1 ||
      !city_read(address + 0x5b0, &pawn, sizeof(pawn)) ||
      !city_player_pawn(pawn) ||
      !city_read(reinterpret_cast<std::uintptr_t>(pawn) + 0x340, &pawn_master,
                 sizeof(pawn_master)) ||
      pawn_master != master ||
      !city_read(address + 12, &object_index, sizeof(object_index))) {
    return;
  }
  const auto now =
      windmill_api.clock_ms ? windmill_api.clock_ms() : GetTickCount64();
  if (have_scan_time && master == last_scanned_master &&
      object_index == last_scanned_index && now >= last_scan_ms &&
      now - last_scan_ms < scan_interval_ms) {
    return;
  }
  last_scanned_master = master;
  last_scanned_index = object_index;
  last_scan_ms = now;
  have_scan_time = true;
  struct ScanScope {
    ScanScope() { scanning_windmills = true; }
    ~ScanScope() { scanning_windmills = false; }
  } scope;

  try {
    std::vector<void *> buildings;
    if (!read_windmill_array(address + 0x508, maximum_buildings, buildings)) {
      return;
    }
    for (void *building : buildings) {
      std::int32_t type{};
      if (building == nullptr ||
          !city_read(reinterpret_cast<std::uintptr_t>(building) + 0x3b8, &type,
                     sizeof(type)) ||
          type != 20 || !eligible_windmill(master, pawn, building)) {
        continue;
      }
      void *component = windmill_api.find_component(building);
      const auto component_address =
          reinterpret_cast<std::uintptr_t>(component);
      std::uintptr_t vtable{};
      void *outer{}, *component_master{}, *owner{}, *shaft{}, *root{};
      void *shaft_outer{}, *shaft_parent{};
      std::uint64_t timer{};
      std::uint8_t visibility{};
      if (!eligible_windmill(master, pawn, building) ||
          !city_valid_object(component) ||
          !city_read(component_address, &vtable, sizeof(vtable)) ||
          vtable != city_image + 0x7f72d30 ||
          !city_read(component_address + 0x20, &outer, sizeof(outer)) ||
          outer != building ||
          !city_read(component_address + 0xa0, &component_master,
                     sizeof(component_master)) ||
          component_master != master ||
          !city_read(component_address + 0xa8, &owner, sizeof(owner)) ||
          owner != building ||
          !city_read(component_address + 0xb0, &shaft, sizeof(shaft)) ||
          !city_valid_object(shaft) ||
          !city_read(component_address + 0xb8, &timer, sizeof(timer)) ||
          timer != 0 ||
          !city_read(reinterpret_cast<std::uintptr_t>(building) + 0x1b8, &root,
                     sizeof(root)) ||
          !city_valid_object(root)) {
        continue;
      }
      const auto shaft_address = reinterpret_cast<std::uintptr_t>(shaft);
      if (!city_read(shaft_address, &vtable, sizeof(vtable)) ||
          vtable != city_image + 0x79980e0 ||
          !city_read(shaft_address + 0x20, &shaft_outer, sizeof(shaft_outer)) ||
          shaft_outer != component ||
          !city_read(shaft_address + 0xb0, &shaft_parent,
                     sizeof(shaft_parent)) ||
          shaft_parent != root ||
          !city_read(shaft_address + 0x188, &visibility, sizeof(visibility)) ||
          (visibility & 0x20U) != 0) {
        continue;
      }
      // Shaft creation explicitly disables this native component. Ordinary
      // completion enables it, but the instant-building path skips that step.
      // Replay only its visibility/timer activation, not building finalization.
      windmill_api.activate(component, true);
      void *current_shaft{};
      if (city_valid_object(component) && city_valid_object(shaft) &&
          city_read(component_address + 0xb0, &current_shaft,
                    sizeof(current_shaft)) &&
          current_shaft == shaft &&
          city_read(component_address + 0xb8, &timer, sizeof(timer)) &&
          timer != 0 &&
          city_read(shaft_address + 0x188, &visibility, sizeof(visibility)) &&
          (visibility & 0x20U) != 0) {
        windmill_repairs.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } catch (...) {
    // Allocation failure or a failed native callback must not escape the
    // game-thread command processor. Reconsider current objects next scan.
  }
}

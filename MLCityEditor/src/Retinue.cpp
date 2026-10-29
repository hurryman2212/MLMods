#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "Retinue.hpp"

// Defensive limits fail closed on corrupt arrays, not normal map sizes.
static constexpr std::int32_t max_region_count = 256;
static constexpr std::int32_t max_request_count = 4096;
static constexpr std::int32_t max_manor_modules = 65536;
static constexpr std::int32_t max_squad_count = 4096;
static constexpr std::uint64_t scan_interval_ms = 1000;

static RetinueApi retinue_api{};
static std::atomic<std::uint64_t> retinue_repairs{};
static std::atomic<std::uint64_t> retinue_conflicts{};
static thread_local bool scanning_retinue{};
static void *last_scanned_master{};
static std::int32_t last_scanned_index{};
static std::uint64_t last_scan_ms{};
static bool have_scan_time{};

static auto retinue_writable(std::uintptr_t address, std::size_t size) -> bool {
  MEMORY_BASIC_INFORMATION page{};
  if (address == 0 || size == 0 ||
      VirtualQuery(reinterpret_cast<void *>(address), &page, sizeof(page)) ==
          0 ||
      page.State != MEM_COMMIT ||
      (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
      (page.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
    return false;
  }
  const auto begin = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
  return address >= begin && address - begin <= page.RegionSize &&
         size <= page.RegionSize - (address - begin);
}

static auto read_retinue_array(std::uintptr_t address, std::int32_t limit,
                               CityArray &header, std::vector<void *> &values)
    -> bool {
  if (!city_read(address, &header, sizeof(header)) || header.count < 0 ||
      header.capacity < header.count || header.count > limit ||
      header.capacity > std::numeric_limits<std::int32_t>::max() /
                            static_cast<std::int32_t>(sizeof(void *))) {
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

static auto has_completed_palace(void *manor) -> bool {
  CityArray modules{};
  std::vector<void *> values;
  if (!read_retinue_array(reinterpret_cast<std::uintptr_t>(manor) + 0x2f8,
                          max_manor_modules, modules, values)) {
    return false;
  }
  for (void *module : values) {
    std::int32_t model{};
    void *parent{};
    std::array<float, 2> work{};
    const auto address = reinterpret_cast<std::uintptr_t>(module);
    if (!city_valid_object(module) ||
        !city_read(address + 0x2c0, &model, sizeof(model)) ||
        (model != 144 && model != 497 && model != 419) ||
        !city_read(address + 0x2c8, &parent, sizeof(parent)) ||
        parent != manor ||
        !city_read(address + 0x314, work.data(), sizeof(work))) {
      continue;
    }
    // These are actual confirmed work modules, not planner blueprint actors.
    // +0x312 can remain set on instant-built modules; completed work is the
    // relevant test. +0x2c0 stores the 0.8.117 runtime model ID, verified in
    // DT_ConstructionBPs and the Palace assembly assets (Lv1/2/3: 144/497/419).
    // It is NOT the saved/building enum domain (98/474/81).
    if (std::isfinite(work[0]) && std::isfinite(work[1]) && work[1] >= 0.0F &&
        work[0] >= work[1]) {
      return true;
    }
  }
  return false;
}

static auto eligible_retinue_manor(void *master, void *pawn, void *region,
                                   void *manor) -> bool {
  void *region_owner{}, *canonical_manor{}, *manor_region{}, *manor_master{};
  std::int32_t type{}, squad{};
  std::uint8_t constructed{}, on_fire{};
  std::array<std::uint8_t, 4> transition{};
  const auto region_address = reinterpret_cast<std::uintptr_t>(region);
  const auto manor_address = reinterpret_cast<std::uintptr_t>(manor);
  return city_valid_object(region) && city_player_building(manor, pawn) &&
         city_read(region_address + 0x350, &region_owner,
                   sizeof(region_owner)) &&
         region_owner == pawn &&
         city_read(region_address + 0x358, &canonical_manor,
                   sizeof(canonical_manor)) &&
         canonical_manor == manor &&
         city_read(region_address + 0x508, &squad, sizeof(squad)) &&
         squad == 0 &&
         city_read(manor_address + 0x2c8, &manor_region,
                   sizeof(manor_region)) &&
         manor_region == region &&
         city_read(manor_address + 0x2e8, &manor_master,
                   sizeof(manor_master)) &&
         manor_master == master &&
         city_read(manor_address + 0x3b8, &type, sizeof(type)) && type == 98 &&
         city_read(manor_address + 0x3c1, &constructed, sizeof(constructed)) &&
         constructed == 1 &&
         city_read(manor_address + 0x388, &on_fire, sizeof(on_fire)) &&
         on_fire == 0 &&
         city_read(manor_address + 0x3a0, transition.data(),
                   transition.size()) &&
         std::all_of(transition.begin(), transition.end(),
                     [](std::uint8_t flag) { return flag == 0; }) &&
         has_completed_palace(manor);
}

static auto retinue_squad_absent(void *master, void *pawn, void *region)
    -> bool {
  void *manager{};
  CityArray header{};
  if (!city_read(reinterpret_cast<std::uintptr_t>(master) + 0x2d0, &manager,
                 sizeof(manager)) ||
      !city_valid_object(manager) ||
      !city_read(reinterpret_cast<std::uintptr_t>(manager) + 0xa0, &header,
                 sizeof(header)) ||
      header.count < 0 || header.capacity < header.count ||
      header.count > max_squad_count ||
      header.capacity > std::numeric_limits<std::int32_t>::max() / 0x410) {
    return false;
  }
  using SquadRecord = std::array<std::uint8_t, 0x410>;
  const auto data = reinterpret_cast<std::uintptr_t>(header.data);
  const auto size =
      static_cast<std::size_t>(header.count) * sizeof(SquadRecord);
  if ((header.capacity != 0 && data == 0) || data % alignof(void *) != 0 ||
      data > std::numeric_limits<std::uintptr_t>::max() - size) {
    return false;
  }
  std::vector<SquadRecord> squads(static_cast<std::size_t>(header.count));
  if (size != 0 && !city_read(data, squads.data(), size)) {
    return false;
  }
  for (const auto &squad : squads) {
    void *owner{}, *origin{};
    std::memcpy(&owner, squad.data() + 0x3a8, sizeof(owner));
    std::memcpy(&origin, squad.data() + 0x1f8, sizeof(origin));
    if (squad[0x10] == 3 && owner == pawn && origin == region) {
      // An existing squad can be orphaned from region.retinueSquadID by an
      // unrelated edit. Do not duplicate, refill, delete or rebind it, even
      // when its member list or its own ID is empty/invalid.
      retinue_conflicts.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  return true;
}

auto configure_retinue(const RetinueApi &api) -> void {
  retinue_api = api;
  last_scanned_master = nullptr;
  last_scanned_index = 0;
  last_scan_ms = 0;
  have_scan_time = false;
  retinue_repairs.store(0, std::memory_order_relaxed);
  retinue_conflicts.store(0, std::memory_order_relaxed);
}

auto city_retinue_repair_count() noexcept -> std::uint64_t {
  return retinue_repairs.load(std::memory_order_relaxed);
}

auto city_retinue_conflict_count() noexcept -> std::uint64_t {
  return retinue_conflicts.load(std::memory_order_relaxed);
}

auto city_process_retinue_requests(void *master) -> void {
  if (retinue_api.process_commands == nullptr) {
    return;
  }
  retinue_api.process_commands(master);
  if (scanning_retinue || retinue_api.initialize_empty_squad == nullptr ||
      retinue_api.grow_pointer_array == nullptr || !city_valid_object(master)) {
    return;
  }
  std::uint8_t ready{};
  void *pawn{};
  void *pawn_master{};
  std::int32_t object_index{};
  const auto address = reinterpret_cast<std::uintptr_t>(master);
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
      retinue_api.clock_ms ? retinue_api.clock_ms() : GetTickCount64();
  if (have_scan_time && master == last_scanned_master &&
      object_index == last_scanned_index && now >= last_scan_ms &&
      now - last_scan_ms < scan_interval_ms) {
    return;
  }
  // Object identity is retained only for throttling, never for delayed writes.
  // Gameplay deduplication always uses the current region's authoritative ID.
  last_scanned_master = master;
  last_scanned_index = object_index;
  last_scan_ms = now;
  have_scan_time = true;
  struct ScanScope {
    ScanScope() { scanning_retinue = true; }
    ~ScanScope() { scanning_retinue = false; }
  } scope;

  try {
    CityArray regions{};
    std::vector<void *> region_values;
    if (!read_retinue_array(address + 0x560, max_region_count, regions,
                            region_values)) {
      return;
    }
    for (void *region : region_values) {
      void *manor{};
      if (!city_valid_object(region) ||
          !city_read(reinterpret_cast<std::uintptr_t>(region) + 0x358, &manor,
                     sizeof(manor)) ||
          !eligible_retinue_manor(master, pawn, region, manor) ||
          !retinue_squad_absent(master, pawn, region)) {
        continue;
      }
      CityArray requests{};
      std::vector<void *> request_values;
      if (!read_retinue_array(address + 0xf98, max_request_count, requests,
                              request_values) ||
          requests.count >= max_request_count ||
          !retinue_writable(address + 0xf98, sizeof(CityArray)) ||
          !retinue_writable(reinterpret_cast<std::uintptr_t>(region) + 0x508,
                            sizeof(std::int32_t)) ||
          std::find(request_values.begin(), request_values.end(), manor) !=
              request_values.end()) {
        continue;
      }
      auto *live_requests = reinterpret_cast<CityArray *>(address + 0xf98);
      if (requests.count == requests.capacity) {
        // UE's ResizeGrow reads Num after Add increments it, with old Num as
        // its argument. Reserve that exact slot first, then restore Num so
        // no incomplete request becomes visible during squad initialization.
        const auto previous_count = requests.count;
        struct RestoreCount {
          CityArray *array;
          std::int32_t count;
          ~RestoreCount() { array->count = count; }
        } restore{live_requests, previous_count};
        live_requests->count = previous_count + 1;
        retinue_api.grow_pointer_array(live_requests, previous_count);
      }
      if (!read_retinue_array(address + 0xf98, max_request_count, requests,
                              request_values) ||
          requests.count >= requests.capacity ||
          !retinue_writable(reinterpret_cast<std::uintptr_t>(requests.data) +
                                static_cast<std::size_t>(requests.count) *
                                    sizeof(void *),
                            sizeof(void *)) ||
          std::find(request_values.begin(), request_values.end(), manor) !=
              request_values.end() ||
          !eligible_retinue_manor(master, pawn, region, manor)) {
        continue;
      }
      // Reproduce only the two first-retinue effects from native completion.
      // Do not replay the whole finalizer or other construction rewards.
      retinue_api.initialize_empty_squad(pawn, region);
      std::int32_t squad{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(region) + 0x508, &squad,
                     sizeof(squad)) ||
          squad <= 0) {
        continue;
      }
      CityArray after{};
      if (!city_read(address + 0xf98, &after, sizeof(after)) ||
          after.data != requests.data || after.count != requests.count ||
          after.capacity != requests.capacity) {
        continue;
      }
      static_cast<void **>(after.data)[after.count] = manor;
      live_requests->count = after.count + 1;
      retinue_repairs.fetch_add(1, std::memory_order_relaxed);
    }
  } catch (...) {
    // Bad allocation or an unexpected native failure must not widen the
    // repair criteria or attempt a second squad initialization in this call.
  }
}

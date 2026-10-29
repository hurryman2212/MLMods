#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <Windows.h>

#include "ExtensionCancel.hpp"
#include "Upgrade.hpp"

static constexpr std::int32_t maximum_upgrade_modules = 65536;
static constexpr std::int32_t maximum_asset_subpath = 4096;
static constexpr std::size_t maximum_assembly_requests = 65536;
static UpgradeApi upgrade_api{};
static std::atomic<std::uint64_t> failed_upgrades{};

struct PendingUpgrade {
  std::uint64_t weak{};
  std::int32_t id{};
  bool ready{};
  std::shared_ptr<const ExtensionCancelSnapshot> cancellation;
};

static std::vector<PendingUpgrade> pending_upgrades;

struct AssemblyKey {
  std::array<std::uint8_t, 16> asset{};
  std::vector<char16_t> subpath;
  std::array<std::uint8_t, 96> transform{};
  std::uint8_t instant{};
  std::vector<std::uint64_t> tags;
  auto operator==(const AssemblyKey &) const -> bool = default;
};

struct AssemblyRequest {
  AssemblyKey key;
  std::uint64_t serial{};
  bool executing{};
};

struct AssemblyBarrier {
  std::uint64_t weak{};
  bool reliable{true};
  std::vector<AssemblyRequest> requests;
};

// Track requests before an upgrade too: an older request with the same payload
// must not be mistaken for the completion of a newer request. Tracking alone
// has no gameplay effects; only pending_upgrades authorizes a finalizer.
static std::vector<AssemblyBarrier> assembly_barriers;
static std::uint64_t next_assembly_serial{};
static bool assembly_tracking_failed{};
static thread_local void *cancelling_artisan{};

struct UpgradeScope {
  void *building{};
  bool blocked{};
  bool finalizing{};
  int exceptions{};
  UpgradeScope *previous{};
  static thread_local UpgradeScope *active;

  explicit UpgradeScope(void *object, bool block = false, bool finish = false)
      : building(object), blocked(block), finalizing(finish),
        exceptions(std::uncaught_exceptions()), previous(active) {
    active = this;
  }
  ~UpgradeScope() {
    active = previous;
    if (std::uncaught_exceptions() > exceptions && upgrade_api.weak_get) {
      try {
        const auto removed =
            std::erase_if(pending_upgrades, [&](const auto &entry) {
              return upgrade_api.weak_get(&entry.weak) == building;
            });
        if (removed != 0)
          ++failed_upgrades;
      } catch (...) {
        // Never replace a native exception with an exception from cleanup.
      }
    }
  }
  UpgradeScope(const UpgradeScope &) = delete;
  auto operator=(const UpgradeScope &) -> UpgradeScope & = delete;
};

thread_local UpgradeScope *UpgradeScope::active{};

static auto upgrade_api_ready() noexcept -> bool {
  return upgrade_api.begin && upgrade_api.setup && upgrade_api.type_callback &&
         upgrade_api.extension && upgrade_api.artisan &&
         upgrade_api.weak_assign && upgrade_api.weak_get &&
         upgrade_api.work_done && upgrade_api.work_required &&
         upgrade_api.add_work && upgrade_api.visual && upgrade_api.finish &&
         upgrade_api.resource_cost && upgrade_api.wealth_cost &&
         upgrade_api.artisan_callback && upgrade_api.assembly_request &&
         upgrade_api.assembly_callback && upgrade_api.mature &&
         upgrade_api.reconstruction_cost && upgrade_api.initialize_type;
}

static auto editable_upgrade(std::int32_t id) noexcept -> bool {
  if (id >= 100 && id <= 116)
    return true;
  if (id != 2 && (id < 4 || id > 45))
    return false;
  return id != 13 && id != 19 && id != 21 && id != 26 && id != 27 && id != 28;
}

static auto free_upgrade(void *building, std::int32_t id) noexcept -> bool {
  if (!upgrade_api_ready() || !editable_upgrade(id) ||
      !city_player_building(building))
    return false;
  for (auto *scope = UpgradeScope::active; scope; scope = scope->previous)
    if (scope->building == building && scope->blocked)
      return false;
  return true;
}

static auto active_reconstruction(void *building) -> bool {
  std::uint8_t rebuilding{};
  return free_upgrade(building, 16) &&
         city_read(reinterpret_cast<std::uintptr_t>(building) + 0x3a2,
                   &rebuilding, sizeof(rebuilding)) &&
         rebuilding == 1 &&
         std::any_of(pending_upgrades.begin(), pending_upgrades.end(),
                     [building](const auto &pending) {
                       return pending.id == 16 &&
                              upgrade_api.weak_get(&pending.weak) == building;
                     });
}

static auto prune_upgrades() -> void {
  std::erase_if(pending_upgrades, [](const PendingUpgrade &pending) {
    return upgrade_api.weak_get(&pending.weak) == nullptr;
  });
}

auto reset_upgrades() noexcept -> void {
  pending_upgrades.clear();
  assembly_barriers.clear();
  next_assembly_serial = 0;
  assembly_tracking_failed = false;
  cancelling_artisan = nullptr;
  failed_upgrades = 0;
}

auto configure_upgrade(const UpgradeApi &api) noexcept -> void {
  reset_upgrades();
  upgrade_api = api;
}

auto upgrade_pending_count() noexcept -> std::size_t {
  return pending_upgrades.size();
}

auto upgrade_failed_count() noexcept -> std::uint64_t {
  return failed_upgrades;
}

auto city_upgrade_cancel_ready(void *building) noexcept -> bool {
  if (!upgrade_api_ready() || assembly_tracking_failed ||
      !city_player_building(building))
    return false;
  for (auto *scope = UpgradeScope::active; scope; scope = scope->previous)
    if (scope->finalizing || scope->building == building)
      return false;
  try {
    std::uint64_t weak{};
    upgrade_api.weak_assign(&weak, building);
    if (weak == 0 || upgrade_api.weak_get(&weak) != building ||
        std::any_of(pending_upgrades.begin(), pending_upgrades.end(),
                    [weak](const auto &entry) { return entry.weak == weak; }))
      return false;
    const auto barrier =
        std::find_if(assembly_barriers.begin(), assembly_barriers.end(),
                     [weak](const auto &entry) { return entry.weak == weak; });
    if (barrier != assembly_barriers.end() &&
        (!barrier->reliable || !barrier->requests.empty()))
      return false;
    CityArray modules{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(building) + 0x2f8, &modules,
                   sizeof(modules)) ||
        modules.count < 1 || modules.capacity < modules.count ||
        modules.capacity > maximum_upgrade_modules || modules.data == nullptr)
      return false;
    for (std::int32_t i{}; i < modules.count; ++i) {
      void *module{};
      std::uint8_t replacing{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(modules.data) +
                         static_cast<std::size_t>(i) * sizeof(void *),
                     &module, sizeof(module)) ||
          !city_valid_object(module) ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x311,
                     &replacing, sizeof(replacing)) ||
          replacing != 0)
        return false;
    }
    return upgrade_api.weak_get(&weak) == building;
  } catch (...) {
    return false;
  }
}

static auto release_cancel_workers(void *building) -> void {
  // Synchronous extension assemblies can return before changeArtisanType
  // clears the occupation. The outer cancellation releases workers on return.
  if (cancelling_artisan == building)
    return;
  const auto found = std::find_if(
      pending_upgrades.begin(), pending_upgrades.end(), [&](const auto &entry) {
        return entry.cancellation &&
               upgrade_api.weak_get(&entry.weak) == building;
      });
  if (found == pending_upgrades.end())
    return;
  const auto snapshot = found->cancellation;
  if (!finish_artisan_cancel(building, *snapshot)) {
    decline_artisan_cancel();
    ++failed_upgrades;
  }
}

auto city_can_rebuild_house(void *building) noexcept -> bool {
  if (!upgrade_api_ready() || assembly_tracking_failed ||
      !city_player_building(building)) {
    return false;
  }
  for (auto *scope = UpgradeScope::active; scope; scope = scope->previous) {
    if (scope->finalizing || (scope->building == building && scope->blocked)) {
      return false;
    }
  }
  try {
    std::uint64_t weak{};
    upgrade_api.weak_assign(&weak, building);
    if (weak == 0 || upgrade_api.weak_get(&weak) != building) {
      return false;
    }
    const auto barrier =
        std::find_if(assembly_barriers.begin(), assembly_barriers.end(),
                     [weak](const auto &entry) { return entry.weak == weak; });
    // Assembly hooks track requests from startup, including before upgrades.
    // No barrier means no observed outstanding request. The native signed
    // extension index -1 identifies house/perimeter assemblies; nonnegative
    // indices belong to retained extensions and may complete independently.
    if (barrier != assembly_barriers.end() &&
        (!barrier->reliable ||
         std::any_of(
             barrier->requests.begin(), barrier->requests.end(),
             [](const auto &entry) { return entry.key.instant == 0xff; }))) {
      return false;
    }
    if (barrier != assembly_barriers.end() && !barrier->requests.empty()) {
      CityArray extensions{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(building) + 0xb88,
                     &extensions, sizeof(extensions)) ||
          extensions.count < 0 || extensions.capacity < extensions.count ||
          extensions.capacity > maximum_upgrade_modules ||
          (extensions.count > 0 && extensions.data == nullptr) ||
          std::any_of(
              barrier->requests.begin(), barrier->requests.end(),
              [&](const auto &request) {
                return request.key.instant >= 128 ||
                       request.key.instant >= extensions.count;
              })) {
        return false;
      }
    }
    CityArray modules{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(building) + 0x2f8, &modules,
                   sizeof(modules)) ||
        modules.count < 0 || modules.count > maximum_upgrade_modules ||
        modules.capacity < modules.count ||
        modules.capacity > maximum_upgrade_modules ||
        (modules.count > 0 && modules.data == nullptr)) {
      return false;
    }
    for (std::int32_t i{}; i < modules.count; ++i) {
      void *module{};
      std::uint8_t replacing{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(modules.data) +
                         static_cast<std::size_t>(i) * sizeof(void *),
                     &module, sizeof(module)) ||
          !city_valid_object(module) ||
          !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x311,
                     &replacing, sizeof(replacing)) ||
          replacing != 0) {
        return false;
      }
    }
    return upgrade_api.weak_get(&weak) == building;
  } catch (...) {
    return false;
  }
}

static auto finish_ready_upgrade(void *building) -> void {
  if (!upgrade_api_ready())
    return;
  for (auto *scope = UpgradeScope::active; scope; scope = scope->previous)
    if (scope->building == building)
      return;
  const auto found = std::find_if(
      pending_upgrades.begin(), pending_upgrades.end(),
      [&](const auto &pending) {
        return pending.ready && upgrade_api.weak_get(&pending.weak) == building;
      });
  if (found == pending_upgrades.end())
    return;
  const auto barrier = std::find_if(
      assembly_barriers.begin(), assembly_barriers.end(),
      [weak = found->weak](const auto &entry) { return entry.weak == weak; });
  if (assembly_tracking_failed ||
      (barrier != assembly_barriers.end() &&
       (!barrier->reliable || !barrier->requests.empty())))
    return;
  // Level-4 artisan changes mark the old main module before scheduling its
  // replacement. Keep the request until that callback has replaced the old
  // module and every subsequently scheduled assembly has actually spawned.
  CityArray replacing_modules{};
  if (city_read(reinterpret_cast<std::uintptr_t>(building) + 0x2f8,
                &replacing_modules, sizeof(replacing_modules)) &&
      replacing_modules.count >= 0 &&
      replacing_modules.count <= maximum_upgrade_modules &&
      replacing_modules.capacity >= replacing_modules.count) {
    for (std::int32_t i = 0; i < replacing_modules.count; ++i) {
      void *module{};
      std::uint8_t replacing{};
      if (city_read(reinterpret_cast<std::uintptr_t>(replacing_modules.data) +
                        static_cast<std::size_t>(i) * sizeof(void *),
                    &module, sizeof(module)) &&
          city_read(reinterpret_cast<std::uintptr_t>(module) + 0x311,
                    &replacing, sizeof(replacing)) &&
          replacing != 0)
        return;
    }
  }
  // Remove before native callbacks: they may recurse, allocate, or destroy an
  // object. No vector references survive a native call.
  const auto weak = found->weak;
  const auto upgrade_id = found->id;
  const auto cancellation = found->cancellation;
  pending_upgrades.erase(found);
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  std::uint8_t upgrading{}, constructed{};
  CityArray costs{}, modules{};
  if (!city_player_building(building) ||
      !city_read(address + 0x3a1, &upgrading, sizeof(upgrading)) ||
      !city_read(address + 0x3c1, &constructed, sizeof(constructed)) ||
      !city_read(address + 0x3d0, &costs, sizeof(costs))) {
    ++failed_upgrades;
    return;
  }
  // Some upgrades have no construction phase and are already complete natively.
  if (constructed == 1 && upgrading == 0) {
    // Fruit selection does not enter the construction phase, but still needs
    // the newly created orchard's one-time maturity treatment.
    if ((upgrade_id == 112 || (upgrade_id >= 32 && upgrade_id <= 34)) &&
        upgrade_api.weak_get(&weak) == building &&
        !upgrade_api.mature(building, upgrade_id))
      ++failed_upgrades;
    return;
  }
  if (upgrading != 1 || constructed != 0 || costs.count != 0 ||
      !city_read(address + 0x2f8, &modules, sizeof(modules)) ||
      modules.count < 0 || modules.count > maximum_upgrade_modules ||
      modules.capacity < modules.count ||
      (modules.count > 0 && modules.data == nullptr)) {
    ++failed_upgrades;
    return;
  }
  for (std::int32_t i = 0; i < modules.count; ++i) {
    void *module{};
    std::array<float, 2> work{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(modules.data) +
                       static_cast<std::size_t>(i) * sizeof(void *),
                   &module, sizeof(module)) ||
        !city_valid_object(module) ||
        !city_read(reinterpret_cast<std::uintptr_t>(module) + 0x314,
                   work.data(), sizeof(work)) ||
        !std::isfinite(work[0]) || !std::isfinite(work[1]) || work[0] < 0.0F ||
        work[1] < 0.0F || work[0] > work[1]) {
      ++failed_upgrades;
      return;
    }
  }
  const float done = upgrade_api.work_done(building);
  const float required = upgrade_api.work_required(building);
  if (!std::isfinite(done) || !std::isfinite(required) || required < 0.0F ||
      done < 0.0F || done > required ||
      upgrade_api.weak_get(&weak) != building) {
    ++failed_upgrades;
    return;
  }
  UpgradeScope finishing(building, true, true);
  upgrade_api.add_work(building, required - done);
  upgrade_api.visual(building, 1.0F, 1);
  upgrade_api.finish(building);
  if (cancellation && upgrade_api.weak_get(&weak) == building &&
      !finish_artisan_cancel(building, *cancellation)) {
    decline_artisan_cancel();
    ++failed_upgrades;
  }
  if ((upgrade_id == 112 || (upgrade_id >= 32 && upgrade_id <= 34)) &&
      upgrade_api.weak_get(&weak) == building &&
      city_player_building(building) &&
      !upgrade_api.mature(building, upgrade_id))
    ++failed_upgrades;
}

auto city_begin_upgrade(void *building, std::int32_t upgrade_id) -> void {
  bool blocked = !upgrade_api_ready();
  for (auto *scope = UpgradeScope::active; scope; scope = scope->previous)
    if (scope->building == building && (scope->blocked || scope->finalizing))
      blocked = true;
  if (!blocked && free_upgrade(building, upgrade_id)) {
    if (upgrade_id == 37 && !prepare_artisan_upgrade_visuals(building)) {
      ++failed_upgrades;
      return;
    }
    try {
      prune_upgrades();
      std::uint64_t weak{};
      upgrade_api.weak_assign(&weak, building);
      if (weak == 0 || upgrade_api.weak_get(&weak) != building) {
        blocked = true;
      } else {
        const auto found = std::find_if(
            pending_upgrades.begin(), pending_upgrades.end(),
            [weak](const auto &pending) { return pending.weak == weak; });
        if (found == pending_upgrades.end())
          pending_upgrades.push_back({weak, upgrade_id, false, {}});
        else
          *found = {weak, upgrade_id, false, {}};
      }
    } catch (...) {
      blocked = true;
      ++failed_upgrades;
    }
  }
  {
    UpgradeScope scope(building, blocked);
    if (upgrade_api.begin)
      upgrade_api.begin(building, upgrade_id);
  }
  finish_ready_upgrade(building);
}

auto city_upgrade_cost_setup(void *building, std::int32_t upgrade_id,
                             std::uint8_t play_sound) -> void {
  if (upgrade_api.setup)
    upgrade_api.setup(building, upgrade_id, play_sound);
  if (!upgrade_api_ready() || !free_upgrade(building, upgrade_id))
    return;
  for (auto &pending : pending_upgrades)
    if (pending.id == upgrade_id &&
        upgrade_api.weak_get(&pending.weak) == building)
      pending.ready = true;
  finish_ready_upgrade(building);
}

auto city_upgrade_type_callback(void *capture) -> void {
  void *building{};
  city_read(reinterpret_cast<std::uintptr_t>(capture), &building,
            sizeof(building));
  {
    UpgradeScope scope(building);
    if (upgrade_api.type_callback)
      upgrade_api.type_callback(capture);
  }
  finish_ready_upgrade(building);
}

auto city_try_cancel_artisan(void *building, std::int32_t index,
                             std::int32_t type) -> ExtensionCancelResult {
  if (index != 0 || type != 0 || cancelling_artisan == building)
    return ExtensionCancelResult::not_applicable;
  std::int32_t artisan{};
  if (!city_valid_object(building) ||
      !city_read(reinterpret_cast<std::uintptr_t>(building) + 0x14c4, &artisan,
                 sizeof(artisan))) {
    decline_artisan_cancel();
    return ExtensionCancelResult::rejected;
  }
  if (artisan == 0)
    return ExtensionCancelResult::not_applicable;
  if (!city_player_building(building))
    return ExtensionCancelResult::not_applicable;
  ExtensionCancelSnapshot snapshot;
  if (!prepare_artisan_cancel(building, snapshot)) {
    decline_artisan_cancel();
    return ExtensionCancelResult::rejected;
  }
  std::uint64_t weak{};
  std::shared_ptr<const ExtensionCancelSnapshot> retained;
  try {
    upgrade_api.weak_assign(&weak, building);
    if (weak == 0 || upgrade_api.weak_get(&weak) != building) {
      decline_artisan_cancel();
      return ExtensionCancelResult::rejected;
    }
    retained = std::make_shared<ExtensionCancelSnapshot>(std::move(snapshot));
    pending_upgrades.push_back({weak, 100, true, retained});
  } catch (...) {
    decline_artisan_cancel();
    return ExtensionCancelResult::rejected;
  }
  {
    UpgradeScope scope(building);
    // Clear completed add-on IDs before native assembly can consult them.
    // The snapshot comparison rejects any change since the initial preflight.
    if (!begin_artisan_cancel(building, *retained)) {
      std::erase_if(pending_upgrades,
                    [weak](const auto &entry) { return entry.weak == weak; });
      decline_artisan_cancel();
      return ExtensionCancelResult::rejected;
    }
    void *previous = std::exchange(cancelling_artisan, building);
    try {
      // Clutter has no BeginUpgrade phase. The explicit pending request above
      // authorizes only this cancellation's possible level-four replacement.
      upgrade_api.artisan(building, 0);
    } catch (...) {
      cancelling_artisan = previous;
      throw;
    }
    cancelling_artisan = previous;
    if (upgrade_api.weak_get(&weak) == building)
      release_cancel_workers(building);
  }
  finish_ready_upgrade(building);
  return ExtensionCancelResult::handled;
}

auto city_change_extension(void *building, std::int32_t index,
                           std::int32_t type) -> void {
  if (city_try_cancel_artisan(building, index, type) !=
      ExtensionCancelResult::not_applicable)
    return;
  {
    UpgradeScope scope(building);
    if (upgrade_api.extension)
      upgrade_api.extension(building, index, type);
  }
  finish_ready_upgrade(building);
}

auto city_change_artisan(void *building, std::int32_t type) -> void {
  {
    UpgradeScope scope(building);
    if (upgrade_api.artisan)
      upgrade_api.artisan(building, type);
  }
  finish_ready_upgrade(building);
}

auto city_upgrade_artisan_callback(void *capture, void **replacement) -> void {
  void *building{};
  city_read(reinterpret_cast<std::uintptr_t>(capture), &building,
            sizeof(building));
  {
    UpgradeScope scope(building);
    if (upgrade_api.artisan_callback)
      upgrade_api.artisan_callback(capture, replacement);
    release_cancel_workers(building);
  }
  finish_ready_upgrade(building);
}

static auto read_assembly_key(void *path, void *transform, std::uint8_t instant,
                              CityArray *tags, AssemblyKey &key) -> bool {
  CityArray subpath{}, names{};
  const auto address = reinterpret_cast<std::uintptr_t>(path);
  key.instant = instant;
  if (!city_read(address, key.asset.data(), key.asset.size()) ||
      !city_read(address + 16, &subpath, sizeof(subpath)) ||
      !city_read(reinterpret_cast<std::uintptr_t>(transform),
                 key.transform.data(), key.transform.size()) ||
      !city_read(reinterpret_cast<std::uintptr_t>(tags), &names,
                 sizeof(names)) ||
      subpath.count < 0 || subpath.count > maximum_asset_subpath ||
      subpath.capacity < subpath.count || names.count < 0 ||
      names.count > maximum_upgrade_modules || names.capacity < names.count)
    return false;
  key.subpath.resize(static_cast<std::size_t>(subpath.count));
  key.tags.resize(static_cast<std::size_t>(names.count));
  return (key.subpath.empty() ||
          city_read(reinterpret_cast<std::uintptr_t>(subpath.data),
                    key.subpath.data(),
                    key.subpath.size() * sizeof(char16_t))) &&
         (key.tags.empty() ||
          city_read(reinterpret_cast<std::uintptr_t>(names.data),
                    key.tags.data(), key.tags.size() * sizeof(std::uint64_t)));
}

auto city_upgrade_assembly_request(void *master, void *building, void *path,
                                   void *transform, std::uint8_t instant,
                                   CityArray *tags) -> void {
  if (!upgrade_api.assembly_request)
    return;
  if (upgrade_api_ready() && city_valid_object(building)) {
    try {
      std::erase_if(assembly_barriers, [](const auto &entry) {
        return upgrade_api.weak_get(&entry.weak) == nullptr;
      });
      std::uint64_t weak{};
      upgrade_api.weak_assign(&weak, building);
      if (weak == 0 || upgrade_api.weak_get(&weak) != building)
        throw std::runtime_error("assembly weak identity unavailable");
      auto found = std::find_if(
          assembly_barriers.begin(), assembly_barriers.end(),
          [weak](const auto &entry) { return entry.weak == weak; });
      if (found == assembly_barriers.end()) {
        assembly_barriers.push_back({weak, true, {}});
        found = assembly_barriers.end() - 1;
      }
      AssemblyKey key;
      // Both native functions own/free some argument buffers. Only immutable
      // deep copies may be retained; allocation addresses are not identifiers.
      if (!read_assembly_key(path, transform, instant, tags, key) ||
          found->requests.size() >= maximum_assembly_requests ||
          next_assembly_serial == std::numeric_limits<std::uint64_t>::max()) {
        found->reliable = false;
        ++failed_upgrades;
      } else {
        found->requests.push_back(
            {std::move(key), ++next_assembly_serial, false});
      }
    } catch (...) {
      assembly_tracking_failed = true;
      ++failed_upgrades;
    }
  }
  {
    UpgradeScope scope(building);
    upgrade_api.assembly_request(master, building, path, transform, instant,
                                 tags);
  }
  finish_ready_upgrade(building);
}

auto city_upgrade_assembly_callback(void *master, void *building, void *path,
                                    void *transform, std::uint8_t instant,
                                    CityArray *tags) -> void {
  if (!upgrade_api.assembly_callback)
    return;
  std::uint64_t weak{}, serial{};
  if (upgrade_api_ready()) {
    try {
      auto barrier =
          std::find_if(assembly_barriers.begin(), assembly_barriers.end(),
                       [building](const auto &entry) {
                         return upgrade_api.weak_get(&entry.weak) == building;
                       });
      if (barrier != assembly_barriers.end()) {
        AssemblyKey key;
        if (!read_assembly_key(path, transform, instant, tags, key)) {
          barrier->reliable = false;
          ++failed_upgrades;
        } else {
          auto request =
              std::find_if(barrier->requests.begin(), barrier->requests.end(),
                           [&](const auto &entry) {
                             return !entry.executing && entry.key == key;
                           });
          if (request != barrier->requests.end()) {
            weak = barrier->weak;
            serial = request->serial;
            request->executing = true;
          }
        }
      }
    } catch (...) {
      assembly_tracking_failed = true;
      ++failed_upgrades;
    }
  }
  {
    // An unmatched completion cannot prove which generation produced it.
    // Execute it natively, but do not authorize nested repair or free upgrades.
    UpgradeScope scope(building, serial == 0);
    upgrade_api.assembly_callback(master, building, path, transform, instant,
                                  tags);
    if (serial != 0 && upgrade_api.weak_get(&weak) == building)
      release_cancel_workers(building);
  }
  if (serial == 0)
    return;
  // Reacquire by identity: callbacks may schedule more assemblies and
  // reallocate either vector. The executing request remains a barrier until
  // native return.
  for (auto &barrier : assembly_barriers) {
    if (barrier.weak == weak) {
      std::erase_if(barrier.requests, [serial](const auto &entry) {
        return entry.serial == serial;
      });
      break;
    }
  }
  if (upgrade_api.weak_get(&weak) == building)
    finish_ready_upgrade(building);
}

auto city_reconstruction_resource_cost(void *building, CityArray *result,
                                       std::int32_t building_type)
    -> CityArray * {
  // Initialize obtains rebuilding materials separately from upgrade costs.
  // Return the empty native result before it is assigned to constructionGoods
  // and registered as regional demand. Only an explicit Rebuild (ID16)
  // request authorizes this; loading an existing work site does not.
  if (result != nullptr && active_reconstruction(building)) {
    *result = {};
    return result;
  }
  return upgrade_api.reconstruction_cost
             ? upgrade_api.reconstruction_cost(building, result, building_type)
             : result;
}

auto city_initialize_reconstruction(void *building) -> void {
  if (!upgrade_api.initialize_type)
    return;
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  std::int32_t type{};
  std::uint8_t function{};
  // This call site is after native region bookkeeping and the restored type
  // assignment. Initialize derives the native function only from Default;
  // leaving Rubble here disables the rebuilt building's normal work and UI.
  if (active_reconstruction(building) &&
      city_read(address + 0x3b8, &type, sizeof(type)) && type > 0 &&
      type != 75 && city_read(address + 0x3cc, &function, sizeof(function)) &&
      function == 21) {
    const std::uint8_t default_function{};
    SIZE_T written{};
    if (!WriteProcessMemory(
            GetCurrentProcess(), reinterpret_cast<void *>(address + 0x3cc),
            &default_function, sizeof(default_function), &written) ||
        written != sizeof(default_function)) {
      std::erase_if(pending_upgrades, [building](const auto &pending) {
        return pending.id == 16 &&
               upgrade_api.weak_get(&pending.weak) == building;
      });
      ++failed_upgrades;
    }
  }
  // Run once in its original position, so all function-specific native
  // registration and later FinishConstruction effects see the restored type.
  upgrade_api.initialize_type(building);
}

auto city_upgrade_resource_cost(void *building, CityArray *result,
                                std::int32_t upgrade_id) -> CityArray * {
  if (result != nullptr && free_upgrade(building, upgrade_id)) {
    *result = {};
    return result;
  }
  return upgrade_api.resource_cost
             ? upgrade_api.resource_cost(building, result, upgrade_id)
             : result;
}

auto city_upgrade_wealth_cost(void *building, std::int32_t upgrade_id)
    -> std::int32_t {
  if (free_upgrade(building, upgrade_id))
    return 0;
  return upgrade_api.wealth_cost ? upgrade_api.wealth_cost(building, upgrade_id)
                                 : 0;
}

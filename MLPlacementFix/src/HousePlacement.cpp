#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "HousePlacement.hpp"

static constexpr std::int32_t maximum_house_modules = 100000;
static constexpr std::int32_t maximum_house_extensions = 1024;
static constexpr std::int32_t maximum_house_upgrades = 1024;

static HousePlacementApi house_api;
static std::atomic<bool> house_enabled{};
static std::atomic<std::uint64_t> house_repairs{};
static std::atomic<std::uint64_t> house_skips{};
static std::atomic<std::uintptr_t> house_failed_owner{};
static std::atomic<HousePlacementFailure> house_failure{};

struct HouseArray {
  std::uintptr_t data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct HouseVisualUpgrade {
  std::int32_t upgrade{};
  std::int32_t model{};
  std::uintptr_t actor{};
};

struct HouseActor {
  std::uintptr_t address{};
  bool module{};
  std::array<std::uint8_t, 24> identity{};
};

struct HouseContext {
  void *owner{};
  std::int32_t primary{};
  std::int32_t secondary{};
  HouseContext *previous{};
  bool generating{};
  static thread_local HouseContext *active;

  HouseContext(void *building, std::int32_t first, std::int32_t second)
      : owner(building), primary(first), secondary(second), previous(active) {
    active = this;
  }
  ~HouseContext() { active = previous; }
};

thread_local HouseContext *HouseContext::active{};

static auto house_read(std::uintptr_t object, std::size_t offset, void *output,
                       std::size_t size) -> bool {
  return object != 0 && output != nullptr && house_api.read_memory != nullptr &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - object &&
         size <= std::numeric_limits<std::uintptr_t>::max() - object - offset &&
         house_api.read_memory(object + offset, output, size);
}

template <class T>
static auto house_read_array(std::uintptr_t object, std::size_t offset,
                             std::int32_t limit, std::vector<T> &values)
    -> bool {
  HouseArray array{};
  if (!house_read(object, offset, &array, sizeof(array)) || array.count < 0 ||
      array.capacity < array.count || array.capacity > limit ||
      (array.count > 0 && array.data % alignof(T) != 0)) {
    return false;
  }
  values.resize(static_cast<std::size_t>(array.count));
  return values.empty() ||
         house_read(array.data, 0, values.data(), values.size() * sizeof(T));
}

static auto house_object(std::uintptr_t address) -> bool {
  std::array<std::uint8_t, 24> header{};
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  if (address % alignof(void *) != 0 ||
      !house_read(address, 0, header.data(), header.size())) {
    return false;
  }
  std::memcpy(&vtable, header.data(), sizeof(vtable));
  std::memcpy(&flags, header.data() + 8, sizeof(flags));
  std::memcpy(&index, header.data() + 12, sizeof(index));
  std::memcpy(&object_class, header.data() + 16, sizeof(object_class));
  return house_api.image != 0 && vtable >= house_api.image &&
         vtable - house_api.image < 0x09dfd000 && (flags & 0x40018010U) == 0 &&
         index > 0 && object_class % alignof(void *) == 0 &&
         house_read(object_class, 0, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= house_api.image &&
         class_vtable - house_api.image < 0x09dfd000;
}

auto configure_house_placement_api(const HousePlacementApi &api) noexcept
    -> void {
  house_api = api;
  house_repairs = 0;
  house_skips = 0;
  house_failed_owner = 0;
  house_failure = HousePlacementFailure::None;
}

auto set_house_placement_enabled(bool enabled) noexcept -> void {
  house_enabled = enabled;
}

auto house_placement_repair_count() noexcept -> std::uint64_t {
  return house_repairs.load();
}

auto house_placement_skip_count() noexcept -> std::uint64_t {
  return house_skips.load();
}

auto house_placement_diagnostic() noexcept -> HousePlacementDiagnostic {
  return {house_failed_owner.load(), house_failure.load()};
}

auto house_placement_failure_name(HousePlacementFailure failure) noexcept
    -> const wchar_t * {
  switch (failure) {
  case HousePlacementFailure::None:
    return L"none";
  case HousePlacementFailure::NativeContext:
    return L"native_context";
  case HousePlacementFailure::AssemblyBarrier:
    return L"assembly_barrier";
  case HousePlacementFailure::OwnerArrays:
    return L"owner_arrays";
  case HousePlacementFailure::ModuleOwner:
    return L"module_owner";
  case HousePlacementFailure::ExtensionMembers:
    return L"extension_members";
  case HousePlacementFailure::ResourceOwner:
    return L"resource_owner";
  case HousePlacementFailure::HelperArray:
    return L"helper_array";
  case HousePlacementFailure::VisualCache:
    return L"visual_cache";
  case HousePlacementFailure::Identity:
    return L"identity";
  case HousePlacementFailure::Exception:
    return L"exception";
  }
  return L"unknown";
}

auto house_placement_choose_primary(void *owner, float width, float *scale,
                                    std::int32_t level, std::uint8_t variant)
    -> std::int32_t {
  if (house_api.choose == nullptr) {
    return 0;
  }
  const auto selected = house_api.choose(owner, width, scale, level, variant);
  const auto *context = HouseContext::active;
  return context != nullptr && context->generating && context->owner == owner
             ? context->primary
             : selected;
}

auto house_placement_choose_secondary(void *owner, float width, float *scale,
                                      std::int32_t level, std::uint8_t variant)
    -> std::int32_t {
  const auto *context = HouseContext::active;
  if (context != nullptr && context->generating && context->owner == owner &&
      context->secondary > 0 && scale != nullptr) {
    // Load retains the secondary model and the generator's default unit scale.
    *scale = 1.0F;
    return context->secondary;
  }
  return house_api.choose
             ? house_api.choose(owner, width, scale, level, variant)
             : 0;
}

auto house_placement_rebuild(void *owner, std::int32_t level,
                             std::uint8_t primary, std::uint8_t secondary,
                             HouseOptionalModel *model) -> void {
  if (house_api.rebuild == nullptr) {
    return;
  }
  const bool attempted =
      house_enabled && level == 4 && primary == 1 && secondary == 0;
  bool supported =
      attempted && house_api.is_field != nullptr &&
      house_api.cleanup_module != nullptr && house_api.choose != nullptr &&
      house_api.can_rebuild_house != nullptr &&
      house_api.destroy_actor != nullptr && house_api.remove_actor != nullptr;
  for (auto *context = HouseContext::active; context;
       context = context->previous) {
    supported = supported && context->owner != owner;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(owner);
  auto failure = HousePlacementFailure::NativeContext;
  HouseOptionalModel requested{};
  std::int32_t secondary_model{};
  bool secondary_house{};
  std::vector<HouseActor> selected;
  std::array<std::uint8_t, 24> original_owner{};
  try {
    std::uintptr_t master{};
    std::int32_t type{}, current_level{}, primary_model{};
    std::uint8_t constructed{}, upgrading{}, master_ready{};
    std::array<std::uint8_t, 3> transition{};
    supported =
        supported && house_object(address) &&
        house_read(address, 0, original_owner.data(), original_owner.size()) &&
        house_read(reinterpret_cast<std::uintptr_t>(model), 0, &requested,
                   sizeof(requested)) &&
        requested.valid == 1 && requested.model > 0 &&
        house_read(address, 0x2e8, &master, sizeof(master)) &&
        house_object(master) &&
        house_read(master, 0x318, &master_ready, sizeof(master_ready)) &&
        master_ready == 1 && house_read(address, 0x3b8, &type, sizeof(type)) &&
        type == 484 &&
        house_read(address, 0xd48, &current_level, sizeof(current_level)) &&
        current_level == 4 &&
        house_read(address, 0x890, &primary_model, sizeof(primary_model)) &&
        primary_model == requested.model &&
        house_read(address, 0x3c1, &constructed, sizeof(constructed)) &&
        constructed == 0 &&
        house_read(address, 0x3a1, &upgrading, sizeof(upgrading)) &&
        upgrading == 1 &&
        house_read(address, 0x3a2, transition.data(), transition.size()) &&
        std::all_of(transition.begin(), transition.end(),
                    [](auto value) { return value == 0; });
    if (supported) {
      failure = HousePlacementFailure::AssemblyBarrier;
      supported = house_api.can_rebuild_house(owner);
    }
    if (supported) {
      std::vector<std::int32_t> upgrades;
      std::vector<std::uintptr_t> modules;
      std::vector<std::uintptr_t> resources, pending_removal,
          construction_helpers, standalone_components, auxiliary_modules;
      std::vector<HouseVisualUpgrade> visual_upgrades;
      HouseArray extensions{};
      failure = HousePlacementFailure::OwnerArrays;
      supported =
          house_read_array(address, 0x430, maximum_house_upgrades, upgrades) &&
          house_read_array(address, 0x2f8, maximum_house_modules, modules) &&
          house_read_array(address, 0x308, maximum_house_modules, resources) &&
          house_read_array(address, 0x338, maximum_house_modules,
                           pending_removal) &&
          pending_removal.empty() &&
          house_read_array(address, 0x3a8, maximum_house_modules,
                           construction_helpers) &&
          house_read_array(address, 0x6f0, maximum_house_modules,
                           standalone_components) &&
          standalone_components.empty() &&
          house_read_array(address, 0x318, maximum_house_modules,
                           auxiliary_modules) &&
          house_read_array(address, 0x370, maximum_house_modules,
                           visual_upgrades) &&
          house_read(address, 0xb88, &extensions, sizeof(extensions)) &&
          extensions.count >= 0 && extensions.capacity >= extensions.count &&
          extensions.capacity <= maximum_house_extensions &&
          (extensions.count == 0 || extensions.data % alignof(void *) == 0);
      secondary_house =
          std::find(upgrades.begin(), upgrades.end(), 12) != upgrades.end();
      if (supported && secondary_house) {
        supported = house_read(address, 0x894, &secondary_model,
                               sizeof(secondary_model)) &&
                    secondary_model > 0;
      }
      std::map<std::uintptr_t, std::int32_t> models;
      std::set<std::uintptr_t> extension_members;
      if (supported)
        failure = HousePlacementFailure::ModuleOwner;
      for (const auto module : modules) {
        std::int32_t module_model{};
        std::uintptr_t parent{};
        if (!supported || !house_object(module) ||
            !house_read(module, 0x2c0, &module_model, sizeof(module_model)) ||
            !house_read(module, 0x2c8, &parent, sizeof(parent)) ||
            parent != address) {
          supported = false;
          break;
        }
        models[module] = module_model;
      }
      for (std::int32_t i{}; supported && i < extensions.count; ++i) {
        failure = HousePlacementFailure::ExtensionMembers;
        std::vector<std::uintptr_t> members;
        supported = house_read_array(extensions.data,
                                     static_cast<std::size_t>(i) * 0xd0 + 0x70,
                                     maximum_house_modules, members);
        for (const auto member : members) {
          extension_members.insert(member);
          const auto found = models.find(member);
          if (found != models.end() &&
              (found->second == requested.model ||
               (secondary_house && found->second == secondary_model))) {
            supported = false;
          }
        }
      }
      std::set<std::uintptr_t> seen, retained;
      if (supported)
        failure = HousePlacementFailure::ResourceOwner;
      for (const auto resource : resources) {
        std::uintptr_t parent{};
        if (!supported || models.count(resource) != 0 ||
            !house_object(resource) ||
            !house_read(resource, 0x2c8, &parent, sizeof(parent)) ||
            parent != address) {
          supported = false;
          break;
        }
        if (extension_members.count(resource) != 0) {
          retained.insert(resource);
        } else if (seen.insert(resource).second) {
          HouseActor actor{resource, false};
          supported = house_read(resource, 0, actor.identity.data(),
                                 actor.identity.size());
          selected.push_back(actor);
        }
      }
      for (const auto module : modules) {
        if (!supported)
          break;
        if (extension_members.count(module) != 0 ||
            house_api.is_field(reinterpret_cast<void *>(module))) {
          retained.insert(module);
        } else if (seen.insert(module).second) {
          HouseActor actor{module, true};
          supported = house_read(module, 0, actor.identity.data(),
                                 actor.identity.size());
          selected.push_back(actor);
        }
      }
      if (supported)
        failure = HousePlacementFailure::HelperArray;
      // The native replacement path appends a construction proxy before this
      // callback. FinishConstruction owns its lifetime; preserve the proxy and
      // its stale old-main reference rather than demanding an empty array.
      for (const auto helper : construction_helpers) {
        std::uintptr_t helper_master{}, represented_actor{};
        if (!supported || !house_object(helper) ||
            !house_read(helper, 0x2b0, &helper_master, sizeof(helper_master)) ||
            helper_master != master ||
            !house_read(helper, 0x2a8, &represented_actor,
                        sizeof(represented_actor)) ||
            seen.count(helper) != 0 || seen.count(represented_actor) != 0) {
          supported = false;
          break;
        }
      }
      if (supported)
        failure = HousePlacementFailure::VisualCache;
      for (const auto &upgrade : visual_upgrades) {
        if (retained.count(upgrade.actor) == 0 ||
            !house_object(upgrade.actor)) {
          supported = false;
        }
      }
      for (const auto module : auxiliary_modules) {
        if (models.count(module) == 0)
          supported = false;
      }
      if (supported)
        failure = HousePlacementFailure::AssemblyBarrier;
      supported = supported && house_api.can_rebuild_house(owner);
    }
  } catch (...) {
    supported = false;
    failure = HousePlacementFailure::Exception;
  }
  if (!supported) {
    if (attempted) {
      house_failed_owner = address;
      house_failure = failure;
      ++house_skips;
    }
    house_api.rebuild(owner, level, primary, secondary, model);
    return;
  }
  // All validation and allocations finish before the first destructive native
  // operation. The existing artisan callback and its async barrier own
  // lifetime.
  HouseContext context(owner, requested.model,
                       secondary_house ? secondary_model : 0);
  HouseOptionalModel automatic{};
  const auto owner_unchanged = [&] {
    std::array<std::uint8_t, 24> current{};
    return house_read(address, 0, current.data(), current.size()) &&
           current == original_owner && house_object(address);
  };
  // Follow ChangeExtension's selective destruction, and the artisan callback's
  // module cleanup order. No global worker/resource/component reset is needed.
  for (const auto &target : selected) {
    std::array<std::uint8_t, 24> current{};
    if (!owner_unchanged() || !house_object(target.address) ||
        !house_read(target.address, 0, current.data(), current.size()) ||
        current != target.identity) {
      house_failed_owner = address;
      house_failure = HousePlacementFailure::Identity;
      ++house_skips;
      return;
    }
    void *actor = reinterpret_cast<void *>(target.address);
    house_api.destroy_actor(actor, 0);
    if (!owner_unchanged()) {
      house_failed_owner = address;
      house_failure = HousePlacementFailure::Identity;
      ++house_skips;
      return;
    }
    house_api.remove_actor(reinterpret_cast<void *>(address + 0x2f8), &actor);
    house_api.remove_actor(reinterpret_cast<void *>(address + 0x308), &actor);
    if (target.module) {
      house_api.remove_actor(reinterpret_cast<void *>(address + 0x318), &actor);
      house_api.cleanup_module(actor);
    }
  }
  if (!owner_unchanged()) {
    house_failed_owner = address;
    house_failure = HousePlacementFailure::Identity;
    ++house_skips;
    return;
  }
  context.generating = true;
  house_api.rebuild(owner, level, 1, secondary_house ? 1 : 0, &automatic);
  ++house_repairs;
}

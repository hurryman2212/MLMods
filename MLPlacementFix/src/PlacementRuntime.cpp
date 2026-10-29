#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "PlacementRuntime.hpp"

static constexpr std::int32_t maximum_buildings = 100000;
static constexpr std::int32_t maximum_modules = 100000;
static constexpr std::int32_t maximum_component_tags = 16384;
static constexpr std::int32_t maximum_extensions = 1024;

std::uintptr_t placement_image{};
static PlacementApi placement_api{};

struct PlacementArray {
  std::uintptr_t data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct PlacementIdentity {
  std::uintptr_t object{};
  std::array<std::uint8_t, 24> header{};
};

struct PlacementModule {
  std::uintptr_t actor{};
  std::uintptr_t component{};
  bool candidate{};
  PlacementBounds bounds{};
  bool general{};
  std::uint8_t collision{};
};

struct PlacementVisibility {
  bool visible{};
  bool priority{};
  std::uint8_t current{};
  bool general{};
};

struct PlacementHelper {
  std::uintptr_t actor{};
  PlacementIdentity module{};
};

static auto placement_read(std::uintptr_t base, std::size_t offset,
                           void *output, std::size_t size) noexcept -> bool {
  return placement_api.read_memory != nullptr && base != 0 &&
         output != nullptr && size != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - base &&
         size <= std::numeric_limits<std::uintptr_t>::max() - base - offset &&
         placement_api.read_memory(base + offset, output, size);
}

static auto placement_object(std::uintptr_t object,
                             PlacementIdentity &identity) noexcept -> bool {
  identity.object = object;
  if (object % alignof(void *) != 0 ||
      !placement_read(object, 0, identity.header.data(),
                      identity.header.size())) {
    return false;
  }
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::memcpy(&vtable, identity.header.data(), sizeof(vtable));
  std::memcpy(&flags, identity.header.data() + 8, sizeof(flags));
  std::memcpy(&index, identity.header.data() + 12, sizeof(index));
  std::memcpy(&object_class, identity.header.data() + 16, sizeof(object_class));
  return vtable >= placement_image && vtable - placement_image < 0x09dfd000 &&
         (flags & 0x40018010U) == 0 && index > 0 &&
         object_class % alignof(void *) == 0 &&
         placement_read(object_class, 0, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= placement_image &&
         class_vtable - placement_image < 0x09dfd000;
}

template <class T>
static auto placement_array(std::uintptr_t object, std::size_t offset,
                            std::int32_t limit, std::vector<T> &values)
    -> bool {
  PlacementArray array{};
  if (!placement_read(object, offset, &array, sizeof(array)) ||
      array.count < 0 || array.capacity < array.count ||
      array.capacity > limit ||
      (array.count != 0 && array.data % alignof(T) != 0)) {
    return false;
  }
  values.resize(static_cast<std::size_t>(array.count));
  return values.empty() || placement_read(array.data, 0, values.data(),
                                          values.size() * sizeof(T));
}

static auto placement_name(std::uint64_t encoded, std::string &name) -> bool {
  const auto index = static_cast<std::uint32_t>(encoded);
  const auto number = static_cast<std::uint32_t>(encoded >> 32);
  const auto pool = placement_image + 0x091d49c0;
  std::uint32_t current_block{};
  std::uintptr_t block{};
  std::uint16_t header{};
  if (!placement_read(pool, 8, &current_block, sizeof(current_block)) ||
      current_block >= 8192 || (index >> 16) > current_block ||
      !placement_read(pool, 0x10 + (index >> 16) * 8, &block, sizeof(block)) ||
      !placement_read(block, (index & 65535) * 2, &header, sizeof(header))) {
    return false;
  }
  const auto length = static_cast<std::size_t>(header >> 6);
  const auto width = (header & 1) != 0 ? std::size_t{2} : std::size_t{1};
  // Native policy tags are ASCII, including FName entries stored as UTF-16.
  // Decode complete entries; unrelated non-ASCII tags are valid nonmatches.
  std::array<std::uint8_t, 2048> bytes{};
  if (length == 0 || length >= 1024 ||
      !placement_read(block, (index & 65535) * 2 + 2, bytes.data(),
                      length * width)) {
    return false;
  }
  name.clear();
  name.reserve(length + 11);
  bool ascii = true;
  for (std::size_t i{}; i < length; ++i) {
    const auto character =
        static_cast<std::uint16_t>(bytes[i * width]) |
        (width == 2 ? static_cast<std::uint16_t>(bytes[i * width + 1]) << 8
                    : 0);
    if (character == 0) {
      return false;
    }
    ascii = ascii && character <= 127;
    name.push_back(static_cast<char>(bytes[i * width]));
  }
  if (!ascii) {
    name.clear();
    return true;
  }
  if (number != 0) {
    name += '_';
    name += std::to_string(number - 1);
  }
  return true;
}

auto placement_bounds_intersect(const PlacementBounds &first,
                                const PlacementBounds &second) noexcept
    -> bool {
  for (std::size_t axis{}; axis < 3; ++axis) {
    const auto left_min = first.center[axis] - first.extent[axis];
    const auto left_max = first.center[axis] + first.extent[axis];
    const auto right_min = second.center[axis] - second.extent[axis];
    const auto right_max = second.center[axis] + second.extent[axis];
    if (!std::isfinite(first.center[axis]) ||
        !std::isfinite(second.center[axis]) ||
        !std::isfinite(first.extent[axis]) ||
        !std::isfinite(second.extent[axis]) || first.extent[axis] < 0 ||
        second.extent[axis] < 0 || !std::isfinite(left_min) ||
        !std::isfinite(left_max) || !std::isfinite(right_min) ||
        !std::isfinite(right_max) || left_min > right_max ||
        right_min > left_max) {
      return false;
    }
  }
  return true;
}

static auto placement_building(std::uintptr_t master, std::uintptr_t owner,
                               PlacementStats &stats, bool restore_general)
    -> bool {
  PlacementIdentity identity{};
  std::vector<PlacementIdentity> identities;
  std::uintptr_t owning_master{};
  std::uint8_t complete{};
  std::array<std::uint8_t, 4> transition{};
  if (!placement_object(owner, identity) ||
      !placement_read(owner, 0x2e8, &owning_master, sizeof(owning_master)) ||
      owning_master != master ||
      !placement_read(owner, 0x3c1, &complete, sizeof(complete)) ||
      complete != 1 ||
      !placement_read(owner, 0x3a1, transition.data(), transition.size()) ||
      std::any_of(transition.begin(), transition.end(),
                  [](std::uint8_t value) { return value != 0; })) {
    return false;
  }
  identities.push_back(identity);
  if (!placement_object(master, identity)) {
    return false;
  }
  identities.push_back(identity);
  std::uint8_t ready{};
  if (!placement_read(master, 0x318, &ready, sizeof(ready)) || ready > 1) {
    return false;
  }
  // Load already creates ordinary final meshes in their native display state.
  // Only live completion/visual-update callbacks repair the construction path
  // that disabled those meshes; the load pass only reconciles native priority.
  restore_general = restore_general && ready == 1;
  std::vector<std::uintptr_t> actors;
  if (!placement_array(owner, 0x2f8, maximum_modules, actors) ||
      actors.empty()) {
    return false;
  }

  PlacementArray extensions{};
  if (!placement_read(owner, 0xb88, &extensions, sizeof(extensions)) ||
      extensions.count < 0 || extensions.capacity < extensions.count ||
      extensions.capacity > maximum_extensions ||
      (extensions.count != 0 && extensions.data % alignof(std::int32_t) != 0)) {
    return false;
  }
  bool active_extension{};
  for (std::int32_t i{}; i < extensions.count; ++i) {
    std::int32_t type{};
    if (!placement_read(extensions.data, static_cast<std::size_t>(i) * 0xd0,
                        &type, sizeof(type))) {
      return false;
    }
    active_extension = active_extension || type != 0;
  }

  std::set<std::uintptr_t> seen_actors;
  std::set<std::uintptr_t> final_components;
  std::map<std::uintptr_t, PlacementHelper> helpers;
  std::set<std::uintptr_t> temporary_components;
  std::vector<PlacementModule> modules;
  std::uint64_t candidates{};
  for (const auto actor : actors) {
    if (!seen_actors.insert(actor).second) {
      continue;
    }
    PlacementModule module{actor};
    std::int32_t model{};
    std::vector<std::uint64_t> tags;
    if (!placement_object(actor, identity) ||
        !placement_read(actor, 0x2c0, &model, sizeof(model)) ||
        !placement_read(actor, 0x2d0, &module.component,
                        sizeof(module.component)) ||
        !placement_array(actor, 0x1e0, maximum_component_tags, tags)) {
      return false;
    }
    identities.push_back(identity);
    const auto module_identity = identity;
    final_components.insert(module.component);
    std::uintptr_t helper{};
    if (!placement_read(actor, 0x308, &helper, sizeof(helper))) {
      return false;
    }
    if (helper != 0) {
      std::uintptr_t represented_actor{}, helper_master{};
      if (placement_api.retire_helper == nullptr || helper == owner ||
          helper == master || !placement_object(helper, identity) ||
          !placement_read(helper, 0x2a8, &represented_actor,
                          sizeof(represented_actor)) ||
          represented_actor != actor ||
          !placement_read(helper, 0x2b0, &helper_master,
                          sizeof(helper_master)) ||
          helper_master != master ||
          !helpers.emplace(helper, PlacementHelper{actor, module_identity})
               .second) {
        return false;
      }
      identities.push_back(identity);
    }
    bool marked{}, outhouse{}, temporary{};
    for (const auto tag : tags) {
      std::string name;
      if (!placement_name(tag, name)) {
        return false;
      }
      marked = marked || name == "outhouseForRemoval";
      outhouse = outhouse || name == "outhouse";
      std::transform(name.begin(), name.end(), name.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value + ('a' - 'A'))
                   : value;
      });
      temporary = temporary || name.find("construction") != std::string::npos;
    }
    // FinishConstruction removes construction-tagged actors before evaluating
    // final meshes. Leave those actors intact, outside both writes and
    // overlaps.
    if (temporary) {
      temporary_components.insert(module.component);
      continue;
    }
    std::array<float, 2> work{};
    if (!placement_read(actor, 0x314, work.data(), sizeof(work)) ||
        !std::isfinite(work[0]) || !std::isfinite(work[1]) || work[0] < 0 ||
        work[1] < 0 || (work[1] > 0 && work[0] < work[1])) {
      return false;
    }
    module.candidate =
        model != 0x91 && (marked || (active_extension && outhouse));
    module.general = restore_general && model != 0x91 && !module.candidate;
    candidates += module.candidate ? 1U : 0U;
    // Native ignores null ordinary final components. An unavailable candidate
    // is incomplete, so defer the whole owner until its model becomes readable.
    if (module.component == 0 && !module.candidate) {
      modules.push_back(module);
      continue;
    }
    if (!placement_object(module.component, identity) ||
        !placement_read(module.component, 0xf0, &module.bounds,
                        sizeof(module.bounds)) ||
        !placement_bounds_intersect(module.bounds, module.bounds)) {
      return false;
    }
    if (module.general) {
      if (!placement_read(actor, 0x2f2, &module.collision,
                          sizeof(module.collision)) ||
          module.collision > 2 ||
          (module.collision == 2 &&
           placement_api.set_module_collision == nullptr)) {
        return false;
      }
    }
    identities.push_back(identity);
    modules.push_back(module);
  }

  std::map<std::uintptr_t, PlacementVisibility> desired;
  // Native FinishConstruction shows every ordinary final mesh, including
  // zero-work decorations whose construction-progress path leaves them hidden.
  for (const auto &module : modules) {
    if (module.general && module.component != 0 &&
        temporary_components.count(module.component) == 0) {
      desired[module.component] = {true, false, 0, true};
    }
  }
  std::set<std::uintptr_t> marked_components;
  std::vector<std::uintptr_t> first_components;
  if (!modules.empty() && !placement_array(modules.front().actor, 0x2f8,
                                           maximum_modules, first_components)) {
    return false;
  }
  // FinishConstruction sets every first-module child to !overlapped. These
  // children are a separate native policy from the priority final meshes.
  for (const auto component : first_components) {
    if (temporary_components.count(component) != 0) {
      continue;
    }
    std::vector<std::uint64_t> tags;
    if (!placement_object(component, identity) ||
        !placement_array(component, 0x60, maximum_component_tags, tags)) {
      return false;
    }
    identities.push_back(identity);
    bool marked{};
    for (const auto tag : tags) {
      std::string name;
      if (!placement_name(tag, name)) {
        return false;
      }
      marked = marked || name == "overlapped";
    }
    desired[component] = {!marked, false};
    if (marked) {
      marked_components.insert(component);
    }
  }
  for (const auto &candidate : modules) {
    if (!candidate.candidate ||
        temporary_components.count(candidate.component) != 0) {
      continue;
    }
    const auto overlaps = std::any_of(
        modules.begin(), modules.end(),
        [&candidate](const PlacementModule &other) {
          return candidate.actor != other.actor && other.component != 0 &&
                 placement_bounds_intersect(candidate.bounds, other.bounds);
        });
    desired[candidate.component] = {
        !overlaps && marked_components.count(candidate.component) == 0, true};
  }

  if (!helpers.empty()) {
    std::vector<std::uintptr_t> replacement_helpers;
    if (!placement_array(owner, 0x3a8, maximum_modules, replacement_helpers)) {
      return false;
    }
    for (const auto &[helper, module] : helpers) {
      if (seen_actors.count(helper) != 0 ||
          final_components.count(helper) != 0 || desired.count(helper) != 0 ||
          std::find(replacement_helpers.begin(), replacement_helpers.end(),
                    helper) != replacement_helpers.end()) {
        return false;
      }
    }
  }

  // Validate every pending write and every source identity before committing
  // any change. A bad late child or recycled object cannot cause a partial
  // pass.
  for (auto &[component, visibility] : desired) {
    if (!placement_read(component, 0x188, &visibility.current,
                        sizeof(visibility.current))) {
      return false;
    }
  }
  for (const auto &original : identities) {
    PlacementIdentity current{};
    if (!placement_object(original.object, current) ||
        current.header != original.header) {
      return false;
    }
  }
  stats.candidates += candidates;
  // Native FinishConstruction retires every module's construction helper
  // before restoring final meshes. A streamed helper can arrive after that
  // pass; its own dirty-visual event brings it through this same completion
  // rule.
  for (const auto &[helper, module] : helpers) {
    placement_api.retire_helper(reinterpret_cast<void *>(helper));
    PlacementIdentity current{};
    std::uintptr_t remaining_helper{};
    std::uint8_t replacing{};
    if (!placement_object(module.actor, current) ||
        current.header != module.module.header ||
        !placement_read(module.actor, 0x308, &remaining_helper,
                        sizeof(remaining_helper)) ||
        remaining_helper != 0 ||
        !placement_read(module.actor, 0x311, &replacing, sizeof(replacing)) ||
        replacing != 0) {
      return false;
    }
    ++stats.helpers_retired;
  }
  for (const auto &[component, visibility] : desired) {
    if (((visibility.current & 0x20U) != 0) == visibility.visible) {
      continue;
    }
    placement_api.set_visibility(reinterpret_cast<void *>(component),
                                 visibility.visible, 1);
    if (visibility.priority) {
      if (visibility.visible) {
        ++stats.shown_priority;
      } else {
        ++stats.hidden_priority;
      }
    } else if (visibility.general) {
      ++stats.shown_general;
    } else if (visibility.visible) {
      ++stats.shown_marked;
    } else {
      ++stats.hidden_marked;
    }
  }
  for (const auto &module : modules) {
    const auto found = desired.find(module.component);
    if (module.general && module.collision == 2 && found != desired.end() &&
        found->second.visible) {
      placement_api.set_module_collision(reinterpret_cast<void *>(module.actor),
                                         true);
      ++stats.collisions_restored;
    }
  }
  return true;
}

auto configure_placement_api(const PlacementApi &api) noexcept -> void {
  placement_api = api;
}

auto placement_reconcile_building(void *building) noexcept -> PlacementStats {
  PlacementStats stats{};
  const auto owner = reinterpret_cast<std::uintptr_t>(building);
  if (placement_image == 0 ||
      placement_image >
          std::numeric_limits<std::uintptr_t>::max() - 0x09dfd000 ||
      placement_api.set_visibility == nullptr) {
    stats.skipped = 1;
    return stats;
  }
  stats.buildings = 1;
  try {
    std::uintptr_t master{};
    if (!placement_read(owner, 0x2e8, &master, sizeof(master)) ||
        !placement_building(master, owner, stats, true)) {
      ++stats.skipped;
    }
  } catch (...) {
    ++stats.skipped;
  }
  return stats;
}

auto placement_reconcile_world(void *master) noexcept -> PlacementStats {
  PlacementStats stats{};
  const auto manager = reinterpret_cast<std::uintptr_t>(master);
  PlacementIdentity identity{};
  if (placement_image == 0 ||
      placement_image >
          std::numeric_limits<std::uintptr_t>::max() - 0x09dfd000 ||
      placement_api.set_visibility == nullptr ||
      !placement_object(manager, identity)) {
    stats.skipped = 1;
    return stats;
  }
  try {
    std::vector<std::uintptr_t> buildings, independent;
    if (!placement_array(manager, 0x508, maximum_buildings, buildings) ||
        !placement_array(manager, 0x518, maximum_buildings, independent)) {
      stats.skipped = 1;
      return stats;
    }
    std::set<std::uintptr_t> owners(buildings.begin(), buildings.end());
    owners.insert(independent.begin(), independent.end());
    stats.buildings = owners.size();
    for (const auto owner : owners) {
      if (!placement_building(manager, owner, stats, false)) {
        ++stats.skipped;
      }
    }
  } catch (...) {
    ++stats.skipped;
  }
  return stats;
}

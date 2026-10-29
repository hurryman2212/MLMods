#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "ManorLoadRuntime.hpp"

static constexpr char gate_mesh_package[] =
    "/Game/ManorLords/Buildings/Merged/SM_MERGED_C_SG_Manor_GateHouse_Lv2";
static constexpr char gate_mesh_asset[] = "SM_MERGED_C_SG_Manor_GateHouse_Lv2";
static constexpr std::int32_t maximum_plan_records = 100000;
static constexpr std::int32_t maximum_source_actors = 65536;
static constexpr std::int32_t maximum_source_tags = 4096;
static constexpr std::int32_t maximum_primitive_floats = 256;

struct alignas(16) ManorLoadRecord {
  std::int32_t type{};
  std::array<std::uint8_t, 12> padding{};
  std::array<double, 12> transform{};
  std::array<std::uint64_t, 2> mesh{};
  ManorLoadArray subpath{};
  ManorLoadArray primitive_data{};
  ManorLoadArray tags{};
};
static_assert(sizeof(ManorLoadRecord) == 0xb0);
static_assert(offsetof(ManorLoadRecord, transform) == 0x10);
static_assert(offsetof(ManorLoadRecord, mesh) == 0x70);
static_assert(offsetof(ManorLoadRecord, primitive_data) == 0x90);
static_assert(offsetof(ManorLoadRecord, tags) == 0xa0);

struct ManorLoadSession {
  void *manor{};
  std::size_t depth{};
  std::size_t active_depth{};
  ManorLoadArray array{};
  std::vector<ManorLoadRecord> records;
  std::vector<std::vector<std::uint64_t>> tags;
  std::vector<std::vector<float>> primitive_data;
  std::vector<bool> roads;
};
static thread_local ManorLoadSession manor_load_session;

std::uintptr_t manor_load_image{};
static ManorLoadApi manor_load_api{};

static auto manor_load_read(std::uintptr_t address, void *output,
                            std::size_t size) noexcept -> bool {
  return manor_load_api.read_memory != nullptr && address != 0 &&
         output != nullptr && size != 0 &&
         size <= std::numeric_limits<std::uintptr_t>::max() - address &&
         manor_load_api.read_memory(address, output, size);
}

static auto manor_load_read_at(std::uintptr_t address, std::size_t offset,
                               void *output, std::size_t size) noexcept
    -> bool {
  return address != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - address &&
         manor_load_read(address + offset, output, size);
}

template <class T>
static auto manor_load_vector(const ManorLoadArray &array, std::int32_t limit,
                              std::vector<T> &output) -> bool {
  if (array.count < 0 || array.capacity < array.count ||
      array.capacity > limit || (array.count != 0 && array.data == nullptr)) {
    return false;
  }
  output.resize(static_cast<std::size_t>(array.count));
  return output.empty() ||
         manor_load_read(reinterpret_cast<std::uintptr_t>(array.data),
                         output.data(), output.size() * sizeof(T));
}

static auto manor_load_name(std::uint64_t encoded, std::string &name) -> bool {
  const auto index = static_cast<std::uint32_t>(encoded);
  const auto number = static_cast<std::uint32_t>(encoded >> 32);
  const auto pool = manor_load_image + 0x091d49c0;
  std::uint32_t current_block{};
  std::uintptr_t block{};
  if (!manor_load_read_at(pool, 8, &current_block, sizeof(current_block)) ||
      current_block >= 8192 || (index >> 16) > current_block ||
      !manor_load_read_at(pool, 0x10 + (index >> 16) * 8, &block,
                          sizeof(block))) {
    return false;
  }
  const auto offset = static_cast<std::uintptr_t>(index & 65535) * 2;
  std::uint16_t header{};
  if (!manor_load_read_at(block, offset, &header, sizeof(header))) {
    return false;
  }
  const auto length = static_cast<std::size_t>(header >> 6);
  const auto width = (header & 1) != 0 ? std::size_t{2} : std::size_t{1};
  // FName entry headers have ten length bits; these names are ASCII asset
  // paths, class names and native tags, including when stored as UTF-16.
  std::array<std::uint8_t, 2048> bytes{};
  if (length == 0 || length >= 1024 ||
      !manor_load_read_at(block, offset + 2, bytes.data(), length * width)) {
    return false;
  }
  name.clear();
  name.reserve(length + 11);
  for (std::size_t i{}; i < length; ++i) {
    if (bytes[i * width] == 0 || bytes[i * width] > 127 ||
        (width == 2 && bytes[i * width + 1] != 0)) {
      return false;
    }
    name.push_back(static_cast<char>(bytes[i * width]));
  }
  if (number != 0) {
    name += '_';
    name += std::to_string(number - 1);
  }
  return true;
}

static auto manor_load_object(std::uintptr_t object, std::string &class_name)
    -> bool {
  std::array<std::uint8_t, 24> header{};
  if (object % alignof(void *) != 0 ||
      !manor_load_read(object, header.data(), header.size())) {
    return false;
  }
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::uint64_t name{};
  std::memcpy(&vtable, header.data(), sizeof(vtable));
  std::memcpy(&flags, header.data() + 8, sizeof(flags));
  std::memcpy(&index, header.data() + 12, sizeof(index));
  std::memcpy(&object_class, header.data() + 16, sizeof(object_class));
  return vtable >= manor_load_image && vtable - manor_load_image < 0x09dfd000 &&
         index > 0 && (flags & 0x40018010U) == 0 &&
         manor_load_read(object_class, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= manor_load_image &&
         class_vtable - manor_load_image < 0x09dfd000 &&
         manor_load_read_at(object_class, 0x18, &name, sizeof(name)) &&
         manor_load_name(name, class_name);
}

static auto manor_load_transform(const std::array<double, 12> &value) noexcept
    -> bool {
  for (const auto field : {0, 1, 2, 3, 4, 5, 6, 8, 9, 10}) {
    if (!std::isfinite(value[static_cast<std::size_t>(field)])) {
      return false;
    }
  }
  const auto norm = value[0] * value[0] + value[1] * value[1] +
                    value[2] * value[2] + value[3] * value[3];
  return std::isfinite(norm) && norm != 0 && value[8] != 0 && value[9] != 0 &&
         value[10] != 0;
}

static auto manor_load_tag_type(const std::vector<std::uint64_t> &tags,
                                std::int32_t &type, bool &marker, bool &gate)
    -> bool {
  type = 0;
  marker = false;
  gate = false;
  for (const auto tag : tags) {
    std::string name;
    if (!manor_load_name(tag, name)) {
      return false;
    }
    marker = marker || name == "marker";
    gate = gate || name == "gate";
    if (!name.starts_with("bType_")) {
      continue;
    }
    std::int32_t candidate{};
    const auto parsed =
        std::from_chars(name.data() + 6, name.data() + name.size(), candidate);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) {
      return false;
    }
    if (candidate > 0) {
      if (type != 0 && type != candidate) {
        return false;
      }
      type = candidate;
    }
  }
  return true;
}

static auto manor_load_mesh(std::uintptr_t component, ManorLoadRecord &record,
                            std::string &package, std::string &asset) -> bool {
  std::uintptr_t mesh{}, outer{};
  std::string class_name;
  if (!manor_load_read_at(component, 0x528, &mesh, sizeof(mesh)) ||
      !manor_load_object(mesh, class_name) || class_name != "StaticMesh" ||
      !manor_load_read_at(mesh, 0x20, &outer, sizeof(outer)) ||
      !manor_load_object(outer, class_name) || class_name != "Package" ||
      !manor_load_read_at(outer, 0x18, &record.mesh[0],
                          sizeof(record.mesh[0])) ||
      !manor_load_read_at(mesh, 0x18, &record.mesh[1],
                          sizeof(record.mesh[1])) ||
      !manor_load_name(record.mesh[0], package) ||
      !manor_load_name(record.mesh[1], asset)) {
    return false;
  }
  return package.starts_with("/Game/") && asset != "None" &&
         package.ends_with('/' + asset);
}

auto manor_load_plan_active() noexcept -> bool {
  return manor_load_session.manor != nullptr;
}

auto manor_load_end(void *manor) noexcept -> void {
  auto &session = manor_load_session;
  if (session.depth == session.active_depth && session.manor == manor) {
    session.manor = nullptr;
    session.active_depth = 0;
    session.array = {};
    session.records.clear();
    session.tags.clear();
    session.primitive_data.clear();
    session.roads.clear();
  }
  if (session.depth != 0) {
    --session.depth;
  }
}

auto manor_load_begin(void *master, void *manor, void *saved_building) noexcept
    -> const ManorLoadArray * {
  auto &session = manor_load_session;
  ++session.depth;
  if (session.depth != 1 || manor_load_image == 0 ||
      manor_load_image >
          std::numeric_limits<std::uintptr_t>::max() - 0x09dfd000) {
    return nullptr;
  }
  try {
    const auto manager = reinterpret_cast<std::uintptr_t>(master);
    const auto owner = reinterpret_cast<std::uintptr_t>(manor);
    const auto saved = reinterpret_cast<std::uintptr_t>(saved_building);
    std::string class_name;
    std::array<std::uint8_t, 0x3e0> owner_bytes{};
    std::uint8_t ready{};
    ManorLoadArray originals{}, sources{}, pending{}, saved_pending{},
        saved_gate_states{};
    std::int32_t owner_type{};
    if (!manor_load_object(manager, class_name) ||
        !manor_load_read_at(manager, 0x318, &ready, sizeof(ready)) ||
        ready != 0 || !manor_load_object(owner, class_name) ||
        class_name != "SMBuildingMaster" ||
        !manor_load_read(owner, owner_bytes.data(), owner_bytes.size()) ||
        !manor_load_read_at(saved, 0x398, &originals, sizeof(originals)) ||
        !manor_load_read_at(saved, 0x220, &saved_pending,
                            sizeof(saved_pending)) ||
        !manor_load_read_at(saved, 0x420, &saved_gate_states,
                            sizeof(saved_gate_states)) ||
        !manor_load_read_at(owner, 0xef0, &sources, sizeof(sources))) {
      return nullptr;
    }
    std::memcpy(&owner_type, owner_bytes.data() + 0x3b8, sizeof(owner_type));
    std::memcpy(&pending, owner_bytes.data() + 0x3d0, sizeof(pending));
    if (owner_type != 98 || owner_bytes[0x3c1] != 1 ||
        owner_bytes[0x3a1] != 0 || owner_bytes[0x3a2] != 0 ||
        owner_bytes[0x3a3] != 0 || owner_bytes[0x3a4] != 0 ||
        pending.count != 0 || pending.capacity < 0 ||
        saved_pending.count != 0 || saved_pending.capacity < 0 ||
        originals.count <= 0 || sources.count <= 0) {
      return nullptr;
    }
    std::vector<ManorLoadRecord> original_records;
    std::vector<std::uintptr_t> source_actors;
    std::vector<std::uint8_t> gate_states;
    if (!manor_load_vector(originals, maximum_plan_records, original_records) ||
        !manor_load_vector(sources, maximum_source_actors, source_actors) ||
        !manor_load_vector(saved_gate_states, maximum_plan_records,
                           gate_states)) {
      return nullptr;
    }
    using Identity = std::tuple<std::int32_t, std::string, std::string>;
    std::map<Identity, std::int32_t> original_mesh_counts, road_mesh_counts;
    std::map<Identity, std::int32_t> original_gate_counts,
        generated_gate_counts;
    std::map<std::int32_t, std::int32_t> original_counts, generated_counts;
    std::vector<std::pair<Identity, std::array<double, 12>>> original_gates;
    for (const auto &record : original_records) {
      std::string package, asset;
      std::vector<std::uint64_t> tags;
      std::int32_t tag_type{};
      bool marker{}, gate{};
      std::uint16_t subpath_terminator{};
      const bool empty_subpath =
          record.subpath.capacity >= record.subpath.count &&
          record.subpath.capacity >= 0 &&
          (record.subpath.count == 0 ||
           (record.subpath.count == 1 &&
            manor_load_read(
                reinterpret_cast<std::uintptr_t>(record.subpath.data),
                &subpath_terminator, sizeof(subpath_terminator)) &&
            subpath_terminator == 0));
      if (record.type <= 0 || !manor_load_transform(record.transform) ||
          !manor_load_name(record.mesh[0], package) ||
          !manor_load_name(record.mesh[1], asset) ||
          !package.starts_with("/Game/") || !package.ends_with('/' + asset) ||
          !empty_subpath ||
          !manor_load_vector(record.tags, maximum_source_tags, tags) ||
          !manor_load_tag_type(tags, tag_type, marker, gate) || marker ||
          (tag_type != 0 && tag_type != record.type)) {
        return nullptr;
      }
      const Identity identity{record.type, package, asset};
      ++original_counts[record.type];
      ++original_mesh_counts[identity];
      if (gate) {
        ++original_gate_counts[identity];
        original_gates.emplace_back(identity, record.transform);
      }
    }
    std::vector<ManorLoadRecord> records;
    std::vector<std::vector<std::uint64_t>> record_tags;
    std::vector<std::vector<float>> primitive_data;
    std::vector<bool> roads, gates;
    std::vector<Identity> identities;
    std::set<std::uintptr_t> seen_actors, seen_components;
    records.reserve(original_records.size());
    for (const auto actor : source_actors) {
      if (!seen_actors.insert(actor).second ||
          !manor_load_object(actor, class_name)) {
        return nullptr;
      }
      const auto road = class_name == "Road";
      if (!road && class_name != "StaticMeshActor") {
        return nullptr;
      }
      std::vector<std::uintptr_t> components;
      if (road) {
        ManorLoadArray array{};
        if (!manor_load_read_at(actor, 0x2d8, &array, sizeof(array)) ||
            !manor_load_vector(array, maximum_plan_records, components)) {
          return nullptr;
        }
      } else {
        std::uintptr_t component{};
        if (!manor_load_read_at(actor, 0x2a8, &component, sizeof(component))) {
          return nullptr;
        }
        components.push_back(component);
      }
      for (const auto component : components) {
        std::uintptr_t outer{}, root = component;
        ManorLoadArray tags_array{};
        std::vector<std::uint64_t> tags;
        ManorLoadRecord record;
        bool marker{}, gate{};
        if (!seen_components.insert(component).second ||
            !manor_load_object(component, class_name) ||
            class_name != "StaticMeshComponent" ||
            !manor_load_read_at(component, 0x20, &outer, sizeof(outer)) ||
            outer != actor ||
            !manor_load_read_at(road ? component : actor, road ? 0x60 : 0x1e0,
                                &tags_array, sizeof(tags_array)) ||
            !manor_load_vector(tags_array, maximum_source_tags, tags) ||
            !manor_load_tag_type(tags, record.type, marker, gate)) {
          return nullptr;
        }
        if (road && (marker || record.type <= 0)) {
          continue;
        }
        if (record.type <= 0 || marker ||
            records.size() >= original_records.size()) {
          return nullptr;
        }
        if (!road && (!manor_load_read_at(actor, 0x1b8, &root, sizeof(root)) ||
                      !manor_load_object(root, class_name))) {
          return nullptr;
        }
        std::string package, asset;
        if (!manor_load_read_at(root, 0x1d0, record.transform.data(),
                                sizeof(record.transform)) ||
            !manor_load_transform(record.transform) ||
            !manor_load_mesh(component, record, package, asset)) {
          return nullptr;
        }
        std::vector<float> values;
        if (road) {
          ManorLoadArray array{};
          if (!manor_load_read_at(component, 0x2a8, &array, sizeof(array)) ||
              !manor_load_vector(array, maximum_primitive_floats, values) ||
              !std::all_of(values.begin(), values.end(),
                           [](float value) { return std::isfinite(value); })) {
            return nullptr;
          }
        }
        const Identity identity{record.type, package, asset};
        ++generated_counts[record.type];
        if (road) {
          ++road_mesh_counts[identity];
        }
        if (gate) {
          ++generated_gate_counts[identity];
        }
        identities.push_back(identity);
        records.push_back(record);
        record_tags.push_back(std::move(tags));
        primitive_data.push_back(std::move(values));
        roads.push_back(road);
        gates.push_back(gate);
      }
    }
    if (generated_counts != original_counts ||
        generated_gate_counts != original_gate_counts ||
        records.size() != original_records.size()) {
      return nullptr;
    }
    // Standalone assemblies may intentionally use a full preview mesh but
    // save a Common mesh (for example type 479). Their native assembly owns
    // that conversion; road components retain exact saved mesh identities.
    for (const auto &[identity, count] : road_mesh_counts) {
      if (original_mesh_counts[identity] < count) {
        return nullptr;
      }
    }
    // Native RVA 0x4c93435 skips state assignment when bGateStates is empty.
    // Otherwise its positional states require the same unique gate order;
    // untagged records must not stand in for a matching gate.
    if (!gate_states.empty()) {
      std::vector<std::size_t> gate_order;
      for (std::size_t i{}; i < records.size(); ++i) {
        if (!gates[i]) {
          continue;
        }
        std::size_t matches{}, matched{};
        for (std::size_t j{}; j < original_gates.size(); ++j) {
          const auto &gate = original_gates[j];
          if (identities[i] == gate.first &&
              std::abs(records[i].transform[4] - gate.second[4]) < 1.0 &&
              std::abs(records[i].transform[5] - gate.second[5]) < 1.0) {
            ++matches;
            matched = j;
          }
        }
        if (matches > 1) {
          return nullptr;
        }
        if (matches == 1) {
          gate_order.push_back(matched);
        }
      }
      if (gate_order.size() != original_gates.size()) {
        return nullptr;
      }
      for (std::size_t i{}; i < gate_order.size(); ++i) {
        if (gate_order[i] != i) {
          return nullptr;
        }
      }
    }
    session.records = std::move(records);
    session.tags = std::move(record_tags);
    session.primitive_data = std::move(primitive_data);
    session.roads = std::move(roads);
    for (std::size_t i{}; i < session.records.size(); ++i) {
      auto &record = session.records[i];
      auto &tags = session.tags[i];
      auto &values = session.primitive_data[i];
      record.tags = {tags.empty() ? nullptr : tags.data(),
                     static_cast<std::int32_t>(tags.size()),
                     static_cast<std::int32_t>(tags.size())};
      record.primitive_data = {values.empty() ? nullptr : values.data(),
                               static_cast<std::int32_t>(values.size()),
                               static_cast<std::int32_t>(values.size())};
    }
    session.array = {session.records.data(),
                     static_cast<std::int32_t>(session.records.size()),
                     static_cast<std::int32_t>(session.records.size())};
    session.manor = manor;
    session.active_depth = session.depth;
    return &session.array;
  } catch (...) {
    // Allocation failure or malformed metadata keeps the native saved loop.
    return nullptr;
  }
}

static auto manor_load_name_equals(const std::uint8_t *name,
                                   std::string_view expected) noexcept -> bool {
  std::uint32_t index{}, number{}, current_block{};
  std::memcpy(&index, name, sizeof(index));
  std::memcpy(&number, name + 4, sizeof(number));
  const auto pool = manor_load_image + 0x091d49c0;
  if (number != 0 ||
      !manor_load_read(pool + 8, &current_block, sizeof(current_block)) ||
      current_block >= 8192 || (index >> 16) > current_block) {
    return false;
  }
  std::uintptr_t block{};
  const auto offset = static_cast<std::uintptr_t>(index & 65535) * 2;
  if (!manor_load_read(pool + 0x10 + (index >> 16) * 8, &block,
                       sizeof(block)) ||
      block == 0 ||
      block > std::numeric_limits<std::uintptr_t>::max() - offset - 2) {
    return false;
  }
  std::uint16_t header{};
  if (!manor_load_read(block + offset, &header, sizeof(header)) ||
      (header >> 6) != expected.size()) {
    return false;
  }
  const auto width = (header & 1) != 0 ? std::size_t{2} : std::size_t{1};
  std::array<std::uint8_t, sizeof(gate_mesh_package) * 2> characters{};
  if (expected.size() * width > characters.size() ||
      !manor_load_read(block + offset + 2, characters.data(),
                       expected.size() * width)) {
    return false;
  }
  for (std::size_t i{}; i < expected.size(); ++i) {
    if (characters[i * width] != static_cast<std::uint8_t>(expected[i]) ||
        (width == 2 && characters[i * width + 1] != 0)) {
      return false;
    }
  }
  return true;
}

auto configure_manor_load_api(const ManorLoadApi &api) noexcept -> void {
  manor_load_api = api;
}

auto manor_load_restore_saved(void *record, void *manor,
                              void *definition) noexcept -> bool {
  if (manor_load_plan_active()) {
    const auto &session = manor_load_session;
    const auto address = reinterpret_cast<std::uintptr_t>(record);
    const auto begin = reinterpret_cast<std::uintptr_t>(session.records.data());
    const auto bytes = session.records.size() * sizeof(ManorLoadRecord);
    return session.manor == manor && session.depth == session.active_depth &&
           address >= begin && address - begin < bytes &&
           (address - begin) % sizeof(ManorLoadRecord) == 0 &&
           session.roads[(address - begin) / sizeof(ManorLoadRecord)];
  }
  const auto record_address = reinterpret_cast<std::uintptr_t>(record);
  const auto manor_address = reinterpret_cast<std::uintptr_t>(manor);
  const auto definition_address = reinterpret_cast<std::uintptr_t>(definition);
  std::array<std::uint8_t, 0xb0> saved{};
  std::array<std::uint8_t, 0x3c2> owner{};
  std::uintptr_t assembly_name{};
  if (manor_load_image == 0 ||
      manor_load_image >
          std::numeric_limits<std::uintptr_t>::max() - 0x09dfd000 ||
      record_address % alignof(void *) != 0 ||
      manor_address % alignof(void *) != 0 || definition_address == 0 ||
      definition_address % alignof(void *) != 0 ||
      definition_address > std::numeric_limits<std::uintptr_t>::max() - 0x180 ||
      !manor_load_read(record_address, saved.data(), saved.size()) ||
      !manor_load_read(manor_address, owner.data(), owner.size()) ||
      !manor_load_read(definition_address + 0x180, &assembly_name,
                       sizeof(assembly_name)) ||
      assembly_name == 0) {
    return false;
  }

  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{}, owner_type{}, saved_type{};
  std::memcpy(&vtable, owner.data(), sizeof(vtable));
  std::memcpy(&flags, owner.data() + 8, sizeof(flags));
  std::memcpy(&index, owner.data() + 12, sizeof(index));
  std::memcpy(&object_class, owner.data() + 16, sizeof(object_class));
  std::memcpy(&owner_type, owner.data() + 0x3b8, sizeof(owner_type));
  std::memcpy(&saved_type, saved.data(), sizeof(saved_type));
  // These offsets and the image extent belong to the verified 0.8.117 build.
  if (vtable < manor_load_image || vtable - manor_load_image >= 0x09dfd000 ||
      (flags & 0x40018010U) != 0 || index <= 0 ||
      !manor_load_read(object_class, &class_vtable, sizeof(class_vtable)) ||
      class_vtable < manor_load_image ||
      class_vtable - manor_load_image >= 0x09dfd000 || owner_type != 98 ||
      saved_type != 478 || owner[0x3c1] != 1 || owner[0x3a1] != 0 ||
      owner[0x3a2] != 0 || owner[0x3a3] != 0 || owner[0x3a4] != 0) {
    return false;
  }

  std::array<double, 12> transform{};
  std::memcpy(transform.data(), saved.data() + 0x10, sizeof(transform));
  for (const auto field : {0, 1, 2, 3, 4, 5, 6, 8, 9, 10}) {
    if (!std::isfinite(transform[static_cast<std::size_t>(field)])) {
      return false;
    }
  }
  const auto rotation_length =
      transform[0] * transform[0] + transform[1] * transform[1] +
      transform[2] * transform[2] + transform[3] * transform[3];
  if (!std::isfinite(rotation_length) || rotation_length == 0 ||
      transform[8] == 0 || transform[9] == 0 || transform[10] == 0 ||
      !manor_load_name_equals(saved.data() + 0x70, gate_mesh_package) ||
      !manor_load_name_equals(saved.data() + 0x78, gate_mesh_asset)) {
    return false;
  }

  // A full static mesh has no subobject suffix. FString may retain storage
  // for an empty string, so accept both its empty representations.
  std::uintptr_t subpath_data{};
  std::int32_t subpath_count{}, subpath_capacity{};
  std::memcpy(&subpath_data, saved.data() + 0x80, sizeof(subpath_data));
  std::memcpy(&subpath_count, saved.data() + 0x88, sizeof(subpath_count));
  std::memcpy(&subpath_capacity, saved.data() + 0x8c, sizeof(subpath_capacity));
  if (subpath_count < 0 || subpath_count > 1 ||
      subpath_capacity < subpath_count) {
    return false;
  }
  if (subpath_count == 1) {
    std::uint16_t terminator{};
    if (!manor_load_read(subpath_data, &terminator, sizeof(terminator)) ||
        terminator != 0) {
      return false;
    }
  }
  return true;
}

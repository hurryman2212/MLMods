#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ManorLoadRuntime.hpp"

struct ManorLoadTestMemory {
  alignas(16) std::array<std::uint8_t, 0xb0> record{};
  alignas(16) std::array<std::uint8_t, 0x400> manor{};
  alignas(16) std::array<std::uint8_t, 0x200> definition{};
  alignas(8) std::array<std::uint8_t, 0x20> object_class{};
  alignas(8) std::array<std::uint8_t, 0x20> name_pool{};
  alignas(8) std::array<std::uint8_t, 1024> names{};
  std::uintptr_t denied_address{};

  template <class T, std::size_t Size>
  static auto put(std::array<std::uint8_t, Size> &bytes, std::size_t offset,
                  const T &value) -> void {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  auto set_name(std::size_t offset, std::string_view value, bool wide = false)
      -> void {
    const auto header =
        static_cast<std::uint16_t>((value.size() << 6) | (wide ? 1U : 0U));
    put(names, offset, header);
    for (std::size_t i{}; i < value.size(); ++i) {
      names[offset + 2 + i * (wide ? 2 : 1)] =
          static_cast<std::uint8_t>(value[i]);
      if (wide) {
        names[offset + 3 + i * 2] = 0;
      }
    }
  }

  ManorLoadTestMemory() {
    manor_load_image = 0x140000000;
    put(manor, 0, manor_load_image + 0x07f37700);
    put(manor, 12, std::int32_t{42});
    put(manor, 16, reinterpret_cast<std::uintptr_t>(object_class.data()));
    put(object_class, 0, manor_load_image + 0x1000);
    put(manor, 0x3b8, std::int32_t{98});
    manor[0x3c1] = 1;
    put(record, 0, std::int32_t{478});
    put(record, 0x10,
        std::array<double, 12>{0, 0, 0.90921107599185, 0.416335464852254,
                               52743.72440923868, -16796.74789355846,
                               4924.8994140625, 0, 1, 1, 1, 0});
    put(record, 0x70, std::uint32_t{1});
    put(record, 0x78, std::uint32_t{128});
    put(definition, 0x180, std::uint64_t{7});
    put(name_pool, 0x10, reinterpret_cast<std::uintptr_t>(names.data()));
    set_name(2, "/Game/ManorLords/Buildings/Merged/"
                "SM_MERGED_C_SG_Manor_GateHouse_Lv2");
    set_name(256, "SM_MERGED_C_SG_Manor_GateHouse_Lv2");
  }

  auto allow() -> bool {
    return manor_load_restore_saved(record.data(), manor.data(),
                                    definition.data());
  }
};

static ManorLoadTestMemory *test_memory{};
static std::map<std::uintptr_t, std::vector<std::uint8_t>> *plan_memory{};

static auto test_read(std::uintptr_t address, void *output,
                      std::size_t size) noexcept -> bool {
  if (plan_memory != nullptr) {
    const auto end = plan_memory->upper_bound(address);
    if (end == plan_memory->begin()) {
      return false;
    }
    const auto &span = *std::prev(end);
    if (address - span.first <= span.second.size() &&
        size <= span.second.size() - (address - span.first)) {
      std::memcpy(output, span.second.data() + (address - span.first), size);
      return true;
    }
    return false;
  }
  if (test_memory == nullptr || address == test_memory->denied_address) {
    return false;
  }
  const std::array spans{
      std::pair{reinterpret_cast<std::uintptr_t>(test_memory->record.data()),
                test_memory->record.size()},
      std::pair{reinterpret_cast<std::uintptr_t>(test_memory->manor.data()),
                test_memory->manor.size()},
      std::pair{
          reinterpret_cast<std::uintptr_t>(test_memory->definition.data()),
          test_memory->definition.size()},
      std::pair{
          reinterpret_cast<std::uintptr_t>(test_memory->object_class.data()),
          test_memory->object_class.size()},
      std::pair{reinterpret_cast<std::uintptr_t>(test_memory->names.data()),
                test_memory->names.size()},
  };
  const auto pool = manor_load_image + 0x091d49c0;
  if (address >= pool && address - pool <= test_memory->name_pool.size() &&
      size <= test_memory->name_pool.size() - (address - pool)) {
    std::memcpy(output, test_memory->name_pool.data() + (address - pool), size);
    return true;
  }
  for (const auto &[begin, length] : spans) {
    if (address >= begin && address - begin <= length &&
        size <= length - (address - begin)) {
      std::memcpy(output, reinterpret_cast<const void *>(address), size);
      return true;
    }
  }
  return false;
}

struct ManorPlanFixture {
  std::map<std::uintptr_t, std::vector<std::uint8_t>> memory;
  std::map<std::string, std::uint64_t> names;
  std::map<std::string, std::uintptr_t> classes;
  std::uintptr_t next{0x200000000}, name_block{}, manager{}, manor{}, saved{},
      originals{}, road{}, placed{}, components{}, actors{}, wall{}, gate_a{},
      gate_b{}, tower{}, wall_mesh{}, gate_mesh{}, tower_mesh{}, road_cpd{};
  std::size_t name_cursor{2};

  auto allocate(std::size_t size = 0x1000) -> std::uintptr_t {
    const auto address = next;
    next += 0x10000;
    memory.emplace(address, std::vector<std::uint8_t>(size));
    return address;
  }
  template <class T> auto put(std::uintptr_t address, const T &value) -> void {
    auto iterator = std::prev(memory.upper_bound(address));
    std::memcpy(iterator->second.data() + address - iterator->first, &value,
                sizeof(value));
  }
  template <class T> auto get(std::uintptr_t address) -> T {
    T value{};
    const auto iterator = std::prev(memory.upper_bound(address));
    std::memcpy(&value, iterator->second.data() + address - iterator->first,
                sizeof(value));
    return value;
  }
  auto name(const std::string &value) -> std::uint64_t {
    if (const auto found = names.find(value); found != names.end()) {
      return found->second;
    }
    const auto index = name_cursor / 2;
    put(name_block + name_cursor,
        static_cast<std::uint16_t>(value.size() << 6));
    for (std::size_t i{}; i < value.size(); ++i) {
      put(name_block + name_cursor + 2 + i, value[i]);
    }
    name_cursor = (name_cursor + 2 + value.size() + 1) & ~std::size_t{1};
    names[value] = index;
    return index;
  }
  auto object(const std::string &class_name) -> std::uintptr_t {
    if (!classes.contains(class_name)) {
      const auto cls = allocate();
      put(cls, manor_load_image + 0x1000);
      put(cls + 0x18, name(class_name));
      classes[class_name] = cls;
    }
    const auto address = allocate();
    put(address, manor_load_image + 0x1000);
    put(address + 12, std::int32_t{1});
    put(address + 16, classes[class_name]);
    return address;
  }
  auto mesh(const std::string &asset) -> std::uintptr_t {
    const auto package = object("Package");
    put(package + 0x18, name("/Game/Test/" + asset));
    const auto address = object("StaticMesh");
    put(address + 0x18, name(asset));
    put(address + 0x20, package);
    return address;
  }
  template <class T>
  auto array(const std::vector<T> &values) -> ManorLoadArray {
    const auto address = allocate();
    for (std::size_t i{}; i < values.size(); ++i) {
      put(address + i * sizeof(T), values[i]);
    }
    return {reinterpret_cast<void *>(address),
            static_cast<std::int32_t>(values.size()),
            static_cast<std::int32_t>(values.size())};
  }
  auto tag(std::int32_t type) -> std::uint64_t {
    return name("bType") | ((static_cast<std::uint64_t>(type) + 1) << 32);
  }
  auto component(std::uintptr_t actor, std::uintptr_t asset,
                 const std::array<double, 12> &transform,
                 const std::vector<std::uint64_t> &tags) -> std::uintptr_t {
    const auto result = object("StaticMeshComponent");
    put(result + 0x20, actor);
    put(result + 0x528, asset);
    put(result + 0x1d0, transform);
    put(result + 0x60, array(tags));
    return result;
  }
  ManorPlanFixture() {
    manor_load_image = 0x140000000;
    name_block = allocate(0x4000);
    memory.emplace(manor_load_image + 0x091d49c0,
                   std::vector<std::uint8_t>(0x20));
    put(manor_load_image + 0x091d49d0, name_block);
    manager = object("MyRTSMultiEngineCPP_BP_C");
    manor = object("SMBuildingMaster");
    saved = allocate();
    originals = allocate();
    put(manor + 0x3b8, std::int32_t{98});
    put(manor + 0x3c1, std::uint8_t{1});
    put(saved + 0x398,
        ManorLoadArray{reinterpret_cast<void *>(originals), 4, 4});
    put(saved + 0x420, array<std::uint8_t>({0, 1}));
    road = object("Road");
    placed = object("StaticMeshActor");
    wall_mesh = mesh("Wall");
    gate_mesh = mesh("Gate");
    tower_mesh = mesh("Tower_00");
    wall = component(road, wall_mesh,
                     {0, 0, 0.1299583595021907, 0.9915194525552684,
                      -12025.632090098454, 25703.893905812434,
                      1110.444580078125, 0, -1.1482261419296265, 1, 1, 0},
                     {tag(470)});
    gate_a = component(road, gate_mesh,
                       {0, 0, 0.4714285348265434, 0.8819042672259265,
                        -13795.628727435767, 23496.55509653444,
                        1434.5235595703125, 0, 1, 1, 1, 0},
                       {tag(95), name("gate")});
    gate_b = component(road, gate_mesh,
                       {0, 0, -0.21348756862046203, 0.9769457805039762,
                        -871.3417450198576, 21824.64651435129,
                        794.8313598632812, 0, 1, 1, 1, 0},
                       {tag(95), name("gate")});
    const auto marker = component(road, 0, {}, {name("marker")});
    tower = component(placed, tower_mesh,
                      {0, 0, 0.9811242781051551, 0.19337825863482788,
                       -1363.0540228115678, 22030.07357002772, 801.403076171875,
                       0, 1, 1, 1, 0},
                      {});
    put(placed + 0x1b8, tower);
    put(placed + 0x2a8, tower);
    put(placed + 0x1e0, array<std::uint64_t>({tag(472), name("instaBuild")}));
    auto component_array =
        array<std::uintptr_t>({wall, marker, gate_a, gate_b});
    components = reinterpret_cast<std::uintptr_t>(component_array.data);
    put(road + 0x2d8, component_array);
    auto actor_array = array<std::uintptr_t>({road, placed});
    actors = reinterpret_cast<std::uintptr_t>(actor_array.data);
    put(manor + 0xef0, actor_array);
    auto values = array<float>({0.25F, 0.75F});
    road_cpd = reinterpret_cast<std::uintptr_t>(values.data);
    put(wall + 0x2a8, values);
    const std::array components_by_record{wall, gate_a, gate_b, tower};
    const std::array types{470, 95, 95, 472};
    for (std::size_t i{}; i < components_by_record.size(); ++i) {
      const auto component_address = components_by_record[i];
      const auto address = originals + i * 0xb0;
      put(address, types[i]);
      auto transform = get<std::array<double, 12>>(component_address + 0x1d0);
      transform[6] += 20;
      put(address + 0x10, transform);
      const auto asset = get<std::uintptr_t>(component_address + 0x528);
      const auto package = get<std::uintptr_t>(asset + 0x20);
      put(address + 0x70, get<std::uint64_t>(package + 0x18));
      put(address + 0x78, get<std::uint64_t>(asset + 0x18));
      put(address + 0xa0,
          array<std::uint64_t>(types[i] == 95
                                   ? std::vector{tag(types[i]), name("gate")}
                                   : std::vector{tag(types[i])}));
    }
    plan_memory = &memory;
    configure_manor_load_api({test_read});
  }
  ~ManorPlanFixture() {
    manor_load_end(reinterpret_cast<void *>(manor));
    plan_memory = nullptr;
  }
  auto begin() -> const ManorLoadArray * {
    return manor_load_begin(reinterpret_cast<void *>(manager),
                            reinterpret_cast<void *>(manor),
                            reinterpret_cast<void *>(saved));
  }
};

static auto run_manor_plan_tests() -> int {
  int failures{}, checks{};
  const auto expect = [&](bool result, const char *message) {
    ++checks;
    if (!result) {
      ++failures;
      std::fprintf(stderr, "manor plan: %s\n", message);
    }
  };
  {
    ManorPlanFixture fixture;
    const auto original_memory = fixture.memory;
    const auto *plan = fixture.begin();
    expect(plan != nullptr && plan->count == 4 && plan->capacity == 4,
           "road pieces and standalone source produce four temporary records");
    if (plan != nullptr) {
      auto *records = static_cast<std::uint8_t *>(plan->data);
      std::array<double, 12> wall{}, tower{};
      std::memcpy(wall.data(), records + 0x10, sizeof(wall));
      std::memcpy(tower.data(), records + 3 * 0xb0 + 0x10, sizeof(tower));
      expect(
          wall == fixture.get<std::array<double, 12>>(fixture.wall + 0x1d0) &&
              wall[8] < 0,
          "road pose including signed scale is copied exactly once");
      expect(
          tower == fixture.get<std::array<double, 12>>(fixture.tower + 0x1d0) &&
              tower[6] == 801.403076171875,
          "tower uses actor source basis rather than accumulated saved pose");
      expect(manor_load_restore_saved(
                 records, reinterpret_cast<void *>(fixture.manor), nullptr) &&
                 manor_load_restore_saved(
                     records + 0xb0, reinterpret_cast<void *>(fixture.manor),
                     nullptr) &&
                 !manor_load_restore_saved(
                     records + 3 * 0xb0,
                     reinterpret_cast<void *>(fixture.manor), nullptr),
             "temporary road records choose direct creation; standalone "
             "chooses native assembly");
      expect(fixture.memory == original_memory,
             "planner and route selection leave every game and save byte "
             "unchanged");
      ManorLoadArray tags{}, cpd{};
      std::memcpy(&tags, records + 0xa0, sizeof(tags));
      std::memcpy(&cpd, records + 0x90, sizeof(cpd));
      fixture.put(fixture.road_cpd, 9.0F);
      expect(
          cpd.count == 2 && static_cast<float *>(cpd.data)[0] == 0.25F &&
              tags.count == 1 &&
              tags.data != reinterpret_cast<void *>(fixture.components),
          "temporary tags and custom primitive data own independent storage");
      expect(fixture.begin() == nullptr && manor_load_plan_active(),
             "nested load declines replacement while retaining the outer plan");
      manor_load_end(reinterpret_cast<void *>(fixture.manor));
      expect(manor_load_plan_active() &&
                 manor_load_restore_saved(
                     records, reinterpret_cast<void *>(fixture.manor), nullptr),
             "nested rejected post-loop does not release outer records");
      manor_load_end(reinterpret_cast<void *>(fixture.manor));
      expect(!manor_load_plan_active(), "outer post-loop releases active plan");
    }
  }
  {
    ManorPlanFixture fixture;
    const auto common = fixture.mesh("Tower_Common");
    const auto package = fixture.get<std::uintptr_t>(common + 0x20);
    fixture.put(fixture.originals + 3 * 0xb0 + 0x70,
                fixture.get<std::uint64_t>(package + 0x18));
    fixture.put(fixture.originals + 3 * 0xb0 + 0x78,
                fixture.get<std::uint64_t>(common + 0x18));
    expect(fixture.begin() != nullptr, "standalone Common saved mesh and full "
                                       "preview mesh retain native assembly");
  }
  {
    ManorPlanFixture fixture;
    const auto empty = fixture.allocate();
    fixture.put(fixture.originals + 0x80,
                ManorLoadArray{reinterpret_cast<void *>(empty), 1, 1});
    expect(fixture.begin() != nullptr,
           "allocated NUL soft subpath is accepted");
  }
  for (bool saved_states : {false, true}) {
    ManorPlanFixture fixture;
    if (!saved_states)
      fixture.put(fixture.saved + 0x420, ManorLoadArray{});
    auto pose = fixture.get<std::array<double, 12>>(fixture.gate_a + 0x1d0);
    // Exact source-versus-saved XY drift captured in Tannenwegholz on
    // 2026-10-07. No artificial distance threshold is added for stateless
    // gates.
    pose[4] += 0.205463881824;
    pose[5] -= 2.047936044197;
    fixture.put(fixture.gate_a + 0x1d0, pose);
    const auto before = fixture.memory;
    const auto *plan = fixture.begin();
    expect((plan != nullptr) == !saved_states,
           "captured gate drift is accepted only when no positional states "
           "need reassignment");
    if (plan) {
      std::array<double, 12> planned_pose{};
      auto *gate_record = static_cast<std::uint8_t *>(plan->data) + 0xb0;
      std::memcpy(planned_pose.data(), gate_record + 0x10,
                  sizeof(planned_pose));
      expect(planned_pose == pose &&
                 manor_load_restore_saved(
                     gate_record, reinterpret_cast<void *>(fixture.manor),
                     nullptr),
             "stateless gate keeps the complete source pose and direct route");
    }
    expect(fixture.memory == before,
           "gate-state validation never modifies saved or live records");
  }
  {
    ManorPlanFixture fixture;
    fixture.put(fixture.saved + 0x420, fixture.array<std::uint8_t>({}));
    fixture.put(fixture.components + 16, fixture.gate_b);
    fixture.put(fixture.components + 24, fixture.gate_a);
    expect(fixture.begin() != nullptr,
           "allocated empty state array does not impose nonexistent gate "
           "state ordering");
  }
  for (bool saved_states : {false, true}) {
    ManorPlanFixture fixture;
    if (!saved_states)
      fixture.put(fixture.saved + 0x420, ManorLoadArray{});
    fixture.put(fixture.gate_a + 0x60,
                fixture.array<std::uint64_t>({fixture.tag(95)}));
    expect(fixture.begin() == nullptr,
           "an untagged record cannot impersonate a gate even without states");
  }
  for (const auto invalid :
       {ManorLoadArray{nullptr, -1, 0}, ManorLoadArray{nullptr, 1, 0},
        ManorLoadArray{nullptr, 0, 100001}, ManorLoadArray{nullptr, 1, 1},
        ManorLoadArray{reinterpret_cast<void *>(1), 1, 1}}) {
    ManorPlanFixture fixture;
    fixture.put(fixture.saved + 0x420, invalid);
    expect(
        fixture.begin() == nullptr,
        "malformed or unreadable saved gate-state array retains native load");
  }
  {
    ManorPlanFixture fixture;
    fixture.put(fixture.saved + 0x420, fixture.array<std::uint8_t>({0, 1, 1}));
    expect(fixture.begin() != nullptr,
           "native stateful component count need not equal tagged-gate count");
  }
  for (int mode{}; mode < 12; ++mode) {
    ManorPlanFixture fixture;
    switch (mode) {
    case 0:
      fixture.put(fixture.manager + 0x318, std::uint8_t{1});
      break;
    case 1:
      fixture.put(fixture.saved + 0x228, std::int32_t{1});
      break;
    case 2:
      fixture.put(fixture.manor + 0x3d8, std::int32_t{1});
      break;
    case 3:
      fixture.put(fixture.originals, std::int32_t{471});
      break;
    case 4:
      fixture.put(fixture.wall + 0x528, fixture.tower_mesh);
      break;
    case 5:
      fixture.put(fixture.actors + 8, fixture.road);
      break;
    case 6:
      fixture.put(fixture.components + 8, fixture.wall);
      break;
    case 7:
      fixture.put(fixture.components + 16, fixture.gate_b);
      fixture.put(fixture.components + 24, fixture.gate_a);
      break;
    case 8:
      fixture.put(fixture.wall + 0x1f0,
                  std::numeric_limits<double>::quiet_NaN());
      break;
    case 9:
      fixture.put(fixture.manor + 0x3a1, std::uint8_t{1});
      break;
    case 10:
      fixture.put(fixture.manor + 0x3c1, std::uint8_t{0});
      break;
    case 11:
      fixture.put(
          fixture.placed + 0x1e0,
          fixture.array<std::uint64_t>({fixture.tag(472), fixture.tag(95)}));
      break;
    }
    const auto original_memory = fixture.memory;
    expect(fixture.begin() == nullptr && !manor_load_plan_active(),
           "unsafe context, drifted identity/order or ambiguous source falls "
           "back");
    expect(fixture.memory == original_memory,
           "failed preparation never edits any input buffer");
  }
  configure_manor_load_api({});
  std::printf("manor plan: %d checks, %d failures\n", checks, failures);
  return failures;
}

auto run_manor_load_runtime_tests() -> int {
  int failures{}, checks{};
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "manor runtime: %s\n", message);
    }
  };
  const auto verify = [&](ManorLoadTestMemory &memory, bool accepted,
                          const char *message) {
    test_memory = &memory;
    const auto before = memory;
    expect(memory.allow() == accepted, message);
    expect(memory.record == before.record && memory.manor == before.manor &&
               memory.definition == before.definition &&
               memory.object_class == before.object_class &&
               memory.name_pool == before.name_pool &&
               memory.names == before.names,
           "callback leaves every supplied memory buffer unchanged");
  };
  configure_manor_load_api({test_read});
  {
    ManorLoadTestMemory memory;
    verify(memory, true,
           "completed stone gate with saved full mesh is restored");
    memory.set_name(2,
                    "/Game/ManorLords/Buildings/Merged/"
                    "SM_MERGED_C_SG_Manor_GateHouse_Lv2",
                    true);
    memory.set_name(256, "SM_MERGED_C_SG_Manor_GateHouse_Lv2", true);
    verify(memory, true, "wide FName entries match the same full mesh");
    ManorLoadTestMemory::put(
        memory.record, 0x80,
        reinterpret_cast<std::uintptr_t>(memory.names.data() + 900));
    ManorLoadTestMemory::put(memory.record, 0x88, std::int32_t{1});
    ManorLoadTestMemory::put(memory.record, 0x8c, std::int32_t{1});
    verify(memory, true, "allocated empty soft subobject path is accepted");
    memory.names[900] = 'x';
    verify(memory, false, "nonempty soft subobject path is rejected");
  }
  for (const auto type : {0, 95, 98, 474, 475, 479, -1}) {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.record, 0, std::int32_t{type});
    verify(memory, false,
           "unrelated saved module types retain native behavior");
  }
  for (const auto offset : {0x3a1, 0x3a2, 0x3a3, 0x3a4}) {
    ManorLoadTestMemory memory;
    memory.manor[static_cast<std::size_t>(offset)] = 1;
    verify(memory, false, "manor in a transition retains native behavior");
  }
  for (const auto value : {0, 2, 255}) {
    ManorLoadTestMemory memory;
    memory.manor[0x3c1] = static_cast<std::uint8_t>(value);
    verify(memory, false, "only the completed flag enables restoration");
  }
  for (const auto field : {0, 1, 2, 3, 4, 5, 6, 8, 9, 10}) {
    for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
      ManorLoadTestMemory memory;
      ManorLoadTestMemory::put(
          memory.record, 0x10 + static_cast<std::size_t>(field) * 8, invalid);
      verify(memory, false,
             "nonfinite saved transform retains native behavior");
    }
  }
  for (const auto field : {8, 9, 10}) {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.record,
                             0x10 + static_cast<std::size_t>(field) * 8, 0.0);
    verify(memory, false, "zero scale does not create an invisible gate");
  }
  {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.record, 0x10, std::array<double, 4>{});
    verify(memory, false, "zero quaternion is rejected");
  }
  for (const auto offset : {0x70, 0x78}) {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(
        memory.record, static_cast<std::size_t>(offset) + 4, std::uint32_t{1});
    verify(memory, false,
           "numbered mesh names are not the supported full asset");
  }
  {
    ManorLoadTestMemory memory;
    memory.names[4] = 'X';
    verify(memory, false,
           "a different package never matches by asset name alone");
  }
  {
    ManorLoadTestMemory memory;
    memory.names[258] = 'X';
    verify(memory, false, "a different mesh asset is rejected");
  }
  {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.record, 0x70, std::uint32_t{65537});
    verify(memory, false, "unpublished FName block is rejected");
  }
  {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.manor, 0x3b8, std::int32_t{57});
    verify(memory, false, "nonmanor owner retains native behavior");
  }
  for (const auto flag : {0x10U, 0x8000U, 0x10000U, 0x40000000U}) {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.manor, 8, flag);
    verify(memory, false, "default or destroyed UObject is rejected");
  }
  {
    ManorLoadTestMemory memory;
    ManorLoadTestMemory::put(memory.definition, 0x180, std::uint64_t{});
    verify(memory, false,
           "definition with no assembly keeps its native branch");
  }
  {
    ManorLoadTestMemory memory;
    memory.denied_address =
        reinterpret_cast<std::uintptr_t>(memory.record.data());
    verify(memory, false, "unreadable saved data retains native behavior");
    memory.denied_address =
        reinterpret_cast<std::uintptr_t>(memory.names.data()) + 2;
    verify(memory, false, "unreadable asset identity retains native behavior");
    memory.denied_address = 0;
    test_memory = &memory;
    expect(!manor_load_restore_saved(nullptr, memory.manor.data(),
                                     memory.definition.data()),
           "null saved record is rejected");
    expect(!manor_load_restore_saved(memory.record.data(), nullptr,
                                     memory.definition.data()),
           "null owner is rejected");
    expect(!manor_load_restore_saved(memory.record.data(), memory.manor.data(),
                                     nullptr),
           "null definition is rejected");
    expect(!manor_load_restore_saved(memory.record.data() + 1,
                                     memory.manor.data(),
                                     memory.definition.data()),
           "misaligned saved record is rejected");
    manor_load_image = 0;
    expect(!memory.allow(), "unconfigured image retains native behavior");
    manor_load_image = std::numeric_limits<std::uintptr_t>::max();
    expect(!memory.allow(), "image range overflow is rejected");
  }
  {
    ManorLoadTestMemory memory;
    test_memory = &memory;
    configure_manor_load_api({});
    expect(!memory.allow(),
           "unconfigured memory provider retains native behavior");
  }
  test_memory = nullptr;
  manor_load_image = 0;
  failures += run_manor_plan_tests();
  std::printf("manor runtime: %d checks, %d failures\n", checks, failures);
  return failures;
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "MeleeAssignmentGuard.hpp"

static std::size_t checks{};

static void require(const bool condition, const char *message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct NativeFixture {
  std::size_t calls{};
  std::size_t allocations{};
  std::size_t frees{};
  const MeleeAssignmentArray *rows{};
  const MeleeAssignmentArray *columns{};
  MeleeAssignmentArray *output{};
  bool bias{};
  std::vector<std::int32_t> indices;
  bool explicit_indices{};
  bool wrong_result_pointer{};
  std::set<void *> allocated;
};

static NativeFixture *active_fixture{};

static void mock_free(void *memory) {
  require(active_fixture != nullptr, "free callback has fixture");
  if (memory == nullptr)
    return;
  require(active_fixture->allocated.erase(memory) == 1,
          "only native-allocator-owned memory is released exactly once");
  require(HeapFree(GetProcessHeap(), 0, memory) != 0, "release fixture buffer");
  ++active_fixture->frees;
}

static void mock_reserve(MeleeAssignmentArray *array,
                         const std::int32_t count) {
  require(active_fixture != nullptr && array != nullptr && count >= 0,
          "reserve callback receives valid request");
  if (count <= array->capacity)
    return;
  auto *memory =
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                static_cast<std::size_t>(count) * sizeof(std::int32_t));
  require(memory != nullptr, "allocate native-owned integer buffer");
  if (array->data != nullptr) {
    std::memcpy(memory, array->data,
                static_cast<std::size_t>(std::min(array->count, count)) *
                    sizeof(std::int32_t));
    mock_free(array->data);
  }
  require(active_fixture->allocated.insert(memory).second,
          "allocation ownership is unique");
  ++active_fixture->allocations;
  array->data = memory;
  array->capacity = count;
}

static MeleeAssignmentArray *mock_match(MeleeAssignmentArray *output,
                                        const MeleeAssignmentArray *rows,
                                        const MeleeAssignmentArray *columns,
                                        const bool bias) {
  require(active_fixture != nullptr && output != nullptr && rows != nullptr &&
              columns != nullptr,
          "native callback arguments are present");
  require(rows->count <= columns->count,
          "unsafe native rows-greater-than-columns branch is never entered");
  ++active_fixture->calls;
  active_fixture->rows = rows;
  active_fixture->columns = columns;
  active_fixture->output = output;
  active_fixture->bias = bias;
  const auto count =
      active_fixture->explicit_indices
          ? static_cast<std::int32_t>(active_fixture->indices.size())
          : rows->count;
  mock_reserve(output, count);
  output->count = count;
  auto *indices = static_cast<std::int32_t *>(output->data);
  for (std::int32_t index = 0; index < count; ++index)
    indices[index] =
        active_fixture->explicit_indices
            ? active_fixture->indices[static_cast<std::size_t>(index)]
            : index;
  return active_fixture->wrong_result_pointer ? nullptr : output;
}

static void configure_fixture(NativeFixture &fixture) {
  active_fixture = &fixture;
  require(configure_melee_assignment_guard_for_testing(
              {mock_match, mock_reserve, mock_free}),
          "configure isolated native callbacks");
}

static MeleeAssignmentArray
point_array(std::vector<MeleeAssignmentPoint> &points) {
  return {points.data(), static_cast<std::int32_t>(points.size()),
          static_cast<std::int32_t>(points.size())};
}

static void release_output(NativeFixture &fixture,
                           MeleeAssignmentArray &output) {
  if (output.data != nullptr)
    mock_free(output.data);
  output = {};
  require(fixture.allocated.empty(),
          "no native output or temporary allocation leaks");
}

static void test_normal_native_path() {
  for (const bool bias : {false, true}) {
    for (const auto counts : {std::array{2, 5}, std::array{3, 3}}) {
      NativeFixture fixture;
      configure_fixture(fixture);
      std::vector<MeleeAssignmentPoint> own(counts[0], {1, 2, 3});
      std::vector<MeleeAssignmentPoint> enemy(counts[1], {4, 5, 6});
      auto rows = point_array(own);
      auto columns = point_array(enemy);
      const auto old_rows = rows;
      const auto old_columns = columns;
      const auto old_own = own;
      const auto old_enemy = enemy;
      MeleeAssignmentArray output{};
      std::memset(&output, 0xcc, sizeof(output));
      require(guarded_melee_assignment(&output, &rows, &columns, bias) ==
                  &output,
              "normal path preserves native return pointer");
      require(fixture.calls == 1 && fixture.output == &output &&
                  fixture.rows == &rows && fixture.columns == &columns &&
                  fixture.bias == bias,
              "normal equal and fewer-own cases call native once with exact "
              "arguments");
      require(output.count == rows.count && fixture.frees == 0,
              "normal native output remains owned by caller");
      const auto *indices = static_cast<const std::int32_t *>(output.data);
      for (std::int32_t index = 0; index < output.count; ++index)
        require(indices[index] == index,
                "normal native assignment is unchanged");
      require(std::memcmp(&rows, &old_rows, sizeof(rows)) == 0 &&
                  std::memcmp(&columns, &old_columns, sizeof(columns)) == 0 &&
                  std::memcmp(own.data(), old_own.data(),
                              own.size() * sizeof(own[0])) == 0 &&
                  std::memcmp(enemy.data(), old_enemy.data(),
                              enemy.size() * sizeof(enemy[0])) == 0,
              "normal inputs and coordinates are immutable");
      const auto counters = melee_assignment_guard_counters();
      require(counters.passed_through == 1 && counters.transposed == 0 &&
                  counters.reassigned == 0 && counters.rejected == 0,
              "normal path only changes passthrough counter");
      release_output(fixture, output);
    }
  }
}

static void test_crash_dimensions_transposed() {
  for (const bool bias : {false, true}) {
    NativeFixture fixture;
    configure_fixture(fixture);
    fixture.explicit_indices = true;
    fixture.indices = {2, 0, 5, 1, 7, 4, 10, 3, 9, 6, 8};
    std::vector<MeleeAssignmentPoint> own;
    std::vector<MeleeAssignmentPoint> enemy;
    for (int index = 0; index < 24; ++index)
      own.push_back({1000.0 + index * 10.0, 2000.0 + index * 20.0, 100.0});
    for (int index = 0; index < 11; ++index)
      enemy.push_back({1000.0 + (10 - index) * 20.0,
                       2000.0 + (10 - index) * 40.0, -1000.0 * index});
    auto rows = point_array(own);
    auto columns = point_array(enemy);
    const auto old_rows = rows;
    const auto old_columns = columns;
    const auto old_own = own;
    const auto old_enemy = enemy;
    MeleeAssignmentArray output{};
    require(guarded_melee_assignment(&output, &rows, &columns, bias) == &output,
            "crash-dimension guard preserves result pointer");
    require(fixture.calls == 1 && fixture.rows == &columns &&
                fixture.columns == &rows && fixture.bias == bias,
            "24-by-11 crash dimensions reach native only as 11-by-24 with "
            "original bias");
    require(fixture.output != &output && output.count == 24 &&
                output.capacity >= output.count,
            "native writes a separate temporary and caller receives all 24 "
            "assignments");
    const auto *indices = static_cast<const std::int32_t *>(output.data);
    std::set<std::int32_t> targets;
    for (std::int32_t index = 0; index < output.count; ++index) {
      require(indices[index] >= 0 && indices[index] < columns.count,
              "every own unit gets a valid enemy index");
      targets.insert(indices[index]);
    }
    require(targets.size() == enemy.size(),
            "all native-selected enemy targets remain covered");
    for (std::int32_t enemy_index = 0; enemy_index < columns.count;
         ++enemy_index)
      require(indices[fixture.indices[static_cast<std::size_t>(enemy_index)]] ==
                  enemy_index,
              "inversion preserves every native one-to-one matched pair");
    for (std::int32_t own_index = 11; own_index < rows.count; ++own_index) {
      double best = std::numeric_limits<double>::infinity();
      std::int32_t expected = -1;
      for (std::int32_t enemy_index = 0; enemy_index < columns.count;
           ++enemy_index) {
        const auto dx = own[own_index].x - enemy[enemy_index].x;
        const auto dy = own[own_index].y - enemy[enemy_index].y;
        const auto distance = dx * dx + dy * dy;
        if (distance < best) {
          best = distance;
          expected = enemy_index;
        }
      }
      require(indices[own_index] == expected,
              "unpaired attackers receive exact nearest-XY target without "
              "altitude bias");
    }
    require(fixture.frees == 1 && fixture.allocated.size() == 1 &&
                fixture.allocated.contains(output.data),
            "native temporary is released with engine callback while caller "
            "owns output");
    require(std::memcmp(&rows, &old_rows, sizeof(rows)) == 0 &&
                std::memcmp(&columns, &old_columns, sizeof(columns)) == 0 &&
                std::memcmp(own.data(), old_own.data(),
                            own.size() * sizeof(own[0])) == 0 &&
                std::memcmp(enemy.data(), old_enemy.data(),
                            enemy.size() * sizeof(enemy[0])) == 0,
            "crash recovery never changes either squad position array");
    const auto counters = melee_assignment_guard_counters();
    require(counters.passed_through == 0 && counters.transposed == 1 &&
                counters.reassigned == 13 && counters.rejected == 0,
            "recovery counters report one transposition and thirteen extra "
            "attackers");
    release_output(fixture, output);
  }
}

static void test_nearest_tie_and_one_enemy() {
  NativeFixture fixture;
  configure_fixture(fixture);
  fixture.explicit_indices = true;
  fixture.indices = {0, 1};
  std::vector<MeleeAssignmentPoint> own{{-1, 0, 0}, {1, 0, 0}, {0, 0, 1e8}};
  std::vector<MeleeAssignmentPoint> enemy{{-1, 0, -1e8}, {1, 0, 1e8}};
  auto rows = point_array(own);
  auto columns = point_array(enemy);
  MeleeAssignmentArray output{};
  guarded_melee_assignment(&output, &rows, &columns, true);
  require(
      output.count == 3 && static_cast<std::int32_t *>(output.data)[2] == 0,
      "equal XY distances deterministically pick first target and ignore Z");
  release_output(fixture, output);

  NativeFixture single;
  configure_fixture(single);
  enemy.resize(1);
  columns = point_array(enemy);
  guarded_melee_assignment(&output, &rows, &columns, false);
  require(output.count == 3 && single.calls == 1,
          "one enemy still assigns every attacker through a single safe native "
          "call");
  for (std::int32_t index = 0; index < output.count; ++index)
    require(static_cast<std::int32_t *>(output.data)[index] == 0,
            "all attackers pursue the sole remaining enemy");
  release_output(single, output);
}

static void test_invalid_inputs() {
  require(!configure_melee_assignment_guard_for_testing(
              {nullptr, mock_reserve, mock_free}) &&
              !configure_melee_assignment_guard_for_testing(
                  {mock_match, nullptr, mock_free}) &&
              !configure_melee_assignment_guard_for_testing(
                  {mock_match, mock_reserve, nullptr}),
          "missing callbacks are rejected");
  const auto reject = [](auto mutate) {
    NativeFixture fixture;
    configure_fixture(fixture);
    std::vector<MeleeAssignmentPoint> own(3, {1, 2, 3});
    std::vector<MeleeAssignmentPoint> enemy(2, {4, 5, 6});
    auto rows = point_array(own);
    auto columns = point_array(enemy);
    MeleeAssignmentArray output{};
    mutate(rows, columns, own, enemy);
    require(guarded_melee_assignment(&output, &rows, &columns, true) ==
                    &output &&
                output.count == 0 && fixture.calls == 0,
            "invalid coordinates/header produce no assignments or unsafe "
            "native call");
    require(fixture.allocated.empty(), "rejected input allocates no temporary");
  };
  reject([](auto &rows, auto &, auto &, auto &) { rows.count = -1; });
  reject([](auto &rows, auto &, auto &, auto &) {
    rows.count = 4097;
    rows.capacity = 4097;
  });
  reject([](auto &rows, auto &, auto &, auto &) { rows.capacity = 2; });
  reject([](auto &rows, auto &, auto &, auto &) { rows.data = nullptr; });
  reject([](auto &, auto &columns, auto &, auto &) { columns.count = -1; });
  reject([](auto &, auto &columns, auto &, auto &) { columns.capacity = -1; });
  reject([](auto &, auto &columns, auto &, auto &) { columns.data = nullptr; });
  reject([](auto &rows, auto &, auto &, auto &) {
    rows.data = reinterpret_cast<void *>(1);
  });
  reject([](auto &, auto &, auto &own, auto &) {
    own[0].x = std::numeric_limits<double>::quiet_NaN();
  });
  reject([](auto &, auto &, auto &own, auto &) {
    own[2].z = std::numeric_limits<double>::infinity();
  });
  reject([](auto &, auto &, auto &, auto &enemy) {
    enemy[1].y = -std::numeric_limits<double>::infinity();
  });
  reject([](auto &, auto &, auto &own, auto &) {
    own[0].x = std::sqrt(std::numeric_limits<double>::max()) / 2;
  });

  NativeFixture fixture;
  configure_fixture(fixture);
  std::vector<MeleeAssignmentPoint> own(3, {1, 2, 3});
  auto rows = point_array(own);
  MeleeAssignmentArray columns{};
  MeleeAssignmentArray output{};
  require(guarded_melee_assignment(&output, &rows, &columns, true) == &output &&
              output.count == 0 && fixture.calls == 0,
          "zero surviving targets yields empty result without entering solver");
  require(guarded_melee_assignment(nullptr, &rows, &columns, false) ==
                  nullptr &&
              fixture.calls == 0,
          "null output is safely rejected");
  require(guarded_melee_assignment(&output, nullptr, &columns, false) ==
                  &output &&
              output.count == 0 && fixture.calls == 0,
          "null input header is safely rejected");
  require(fixture.allocated.empty(),
          "empty and missing inputs leak no buffers");

  auto *readonly = static_cast<MeleeAssignmentArray *>(
      VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  require(readonly != nullptr, "allocate inaccessible output fixture");
  std::memset(readonly, 0xcc, sizeof(*readonly));
  const auto initial = *readonly;
  DWORD old_protection{};
  require(VirtualProtect(readonly, 4096, PAGE_READONLY, &old_protection) != 0,
          "make output fixture read only");
  require(guarded_melee_assignment(readonly, &rows, &columns, false) ==
                  readonly &&
              std::memcmp(readonly, &initial, sizeof(initial)) == 0 &&
              fixture.calls == 0,
          "read-only output is rejected without write or native invocation");
  require(VirtualFree(readonly, 0, MEM_RELEASE) != 0,
          "release read-only output fixture");

  const auto old_rows = rows;
  require(
      guarded_melee_assignment(&rows, &rows, &columns, false) == &rows &&
          std::memcmp(&rows, &old_rows, sizeof(rows)) == 0 &&
          fixture.calls == 0,
      "input-output alias is rejected without destroying native input header");
}

static void test_invalid_native_results() {
  for (const auto &indices :
       {std::vector<std::int32_t>{0, -1}, std::vector<std::int32_t>{0, 3},
        std::vector<std::int32_t>{1, 1}, std::vector<std::int32_t>{0},
        std::vector<std::int32_t>{}}) {
    NativeFixture fixture;
    configure_fixture(fixture);
    fixture.explicit_indices = true;
    fixture.indices = indices;
    std::vector<MeleeAssignmentPoint> own(3, {1, 2, 3});
    std::vector<MeleeAssignmentPoint> enemy(2, {4, 5, 6});
    auto rows = point_array(own);
    auto columns = point_array(enemy);
    MeleeAssignmentArray output{};
    require(guarded_melee_assignment(&output, &rows, &columns, false) ==
                    &output &&
                output.count == 0 && fixture.calls == 1,
            "malformed native match is rejected before indexed inversion");
    require(melee_assignment_guard_counters().rejected == 1,
            "malformed native output is diagnosed");
    release_output(fixture, output);
    fixture.explicit_indices = false;
    require(guarded_melee_assignment(&output, &rows, &columns, false) ==
                    &output &&
                output.count == 3 && fixture.calls == 2,
            "rejected match does not poison subsequent valid retry");
    release_output(fixture, output);
  }

  NativeFixture fixture;
  configure_fixture(fixture);
  fixture.wrong_result_pointer = true;
  std::vector<MeleeAssignmentPoint> own(3, {1, 2, 3});
  std::vector<MeleeAssignmentPoint> enemy(2, {4, 5, 6});
  auto rows = point_array(own);
  auto columns = point_array(enemy);
  MeleeAssignmentArray output{};
  require(guarded_melee_assignment(&output, &rows, &columns, false) ==
                  &output &&
              output.count == 0 && fixture.calls == 1 && fixture.frees == 1,
          "wrong native result pointer is rejected and temporary remains "
          "properly freed");
  release_output(fixture, output);
}

static void test_offline_installation(const char *filename) {
  std::ifstream stream(filename, std::ios::binary | std::ios::ate);
  require(stream.good(), "open current game executable read only");
  const auto length = stream.tellg();
  require(length >= static_cast<std::streamoff>(sizeof(IMAGE_NT_HEADERS64)),
          "input holds a PE header");
  std::vector<std::uint8_t> file(static_cast<std::size_t>(length));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
  require(stream.good(), "read complete executable without modification");
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
  require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
              static_cast<std::size_t>(dos->e_lfanew) <=
                  file.size() - sizeof(IMAGE_NT_HEADERS64),
          "PE header bounds are valid");
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(file.data() + dos->e_lfanew);
  require(nt->Signature == IMAGE_NT_SIGNATURE &&
              nt->FileHeader.SizeOfOptionalHeader ==
                  sizeof(IMAGE_OPTIONAL_HEADER64) &&
              nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
              nt->OptionalHeader.SizeOfHeaders <= file.size() &&
              nt->OptionalHeader.SizeOfHeaders <=
                  nt->OptionalHeader.SizeOfImage,
          "input has supported PE mapping dimensions");
  const auto *sections = IMAGE_FIRST_SECTION(nt);
  require(reinterpret_cast<const std::uint8_t *>(sections) - file.data() +
                  nt->FileHeader.NumberOfSections *
                      sizeof(IMAGE_SECTION_HEADER) <=
              file.size(),
          "all PE section headers fit file");
  const auto image_size = nt->OptionalHeader.SizeOfImage;
  auto *image = static_cast<std::uint8_t *>(VirtualAlloc(
      nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  require(image != nullptr, "allocate private offline PE fixture");
  std::memcpy(image, file.data(), nt->OptionalHeader.SizeOfHeaders);
  for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
    const auto &section = sections[index];
    require(static_cast<std::size_t>(section.PointerToRawData) +
                        section.SizeOfRawData <=
                    file.size() &&
                static_cast<std::size_t>(section.VirtualAddress) +
                        section.SizeOfRawData <=
                    image_size,
            "section copy remains within file and mapping");
    std::memcpy(image + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }

  constexpr std::size_t call_rva = 0x4d42756;
  constexpr std::array<std::array<std::size_t, 2>, 6> fingerprints{
      {{0x4d41ec0, 0x1a58},
       {0x4aeea10, 0x2b9},
       {0x4af06a0, 0x1020},
       {0x4af16c0, 0x36b},
       {0x10df930, 0x64},
       {0x1129e10, 0x34}}};
  require(call_rva + 16 < image_size, "audited caller fits mapping");
  require((reinterpret_cast<std::uintptr_t>(image + call_rva + 1) %
           alignof(LONG)) != 0,
          "fixture reproduces original unaligned CALL displacement");
  std::array<std::uint8_t, 5> original{};
  std::memcpy(original.data(), image + call_rva, original.size());
  require(original == std::array<std::uint8_t, 5>{0xe8, 0xb5, 0xc2, 0xda, 0xff},
          "actual game contains the audited assignment CALL");
  require(remove_melee_assignment_guard(),
          "removal before installation is harmless");
  require(!install_melee_assignment_guard(nullptr, image_size) &&
              !install_melee_assignment_guard(image, 0) &&
              !install_melee_assignment_guard(image, call_rva),
          "null and truncated offline images are rejected");
  auto *mapped_nt =
      reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
  const auto reject_identity = [&](auto &field) {
    const auto old = field;
    field ^= 1;
    require(!install_melee_assignment_guard(image, image_size),
            "every executable identity field is enforced");
    require(std::memcmp(image + call_rva, original.data(), original.size()) ==
                0,
            "identity mismatch never changes callsite");
    field = old;
  };
  reject_identity(mapped_nt->Signature);
  reject_identity(mapped_nt->FileHeader.Machine);
  reject_identity(mapped_nt->FileHeader.SizeOfOptionalHeader);
  reject_identity(mapped_nt->FileHeader.TimeDateStamp);
  reject_identity(mapped_nt->OptionalHeader.Magic);
  reject_identity(mapped_nt->OptionalHeader.SizeOfImage);
  reject_identity(mapped_nt->OptionalHeader.CheckSum);
  for (const auto &[rva, size] : fingerprints) {
    for (std::size_t offset = 0; offset < size; ++offset) {
      image[rva + offset] ^= 1;
      require(!install_melee_assignment_guard(image, image_size),
              "every byte of all six full native fingerprints is enforced");
      image[rva + offset] ^= 1;
    }
  }
  std::vector<std::vector<std::uint8_t>> original_functions;
  for (const auto &[rva, size] : fingerprints)
    original_functions.emplace_back(image + rva, image + rva + size);

  require(install_melee_assignment_guard(image, image_size),
          "actual current binary accepts guarded offline installation");
  std::array<std::uint8_t, 5> patched{};
  std::memcpy(patched.data(), image + call_rva, patched.size());
  require(patched != original && patched[0] == 0xe8,
          "installer preserves E8 and redirects only its displacement");
  std::memcpy(original_functions[0].data() + call_rva - fingerprints[0][0],
              patched.data(), patched.size());
  for (std::size_t index = 0; index < fingerprints.size(); ++index)
    require(std::memcmp(image + fingerprints[index][0],
                        original_functions[index].data(),
                        fingerprints[index][1]) == 0,
            "every byte outside the one CALL is untouched");
  require(install_melee_assignment_guard(image, image_size),
          "repeated installation is idempotent after full fingerprint "
          "verification");
  require(!configure_melee_assignment_guard_for_testing(
              {mock_match, mock_reserve, mock_free}),
          "published guard forbids test callback replacement");

  // Execute only a private wrapper, retained hook and mocked native entries.
  // Installation above validated untouched game code; no game subsystem runs.
  std::array<std::uint8_t, 14> saved_wrapper{};
  std::memcpy(saved_wrapper.data(), image + call_rva - 4, saved_wrapper.size());
  constexpr std::array<std::uint8_t, 4> prologue{0x48, 0x83, 0xec, 0x28};
  constexpr std::array<std::uint8_t, 5> epilogue{0x48, 0x83, 0xc4, 0x28, 0xc3};
  std::memcpy(image + call_rva - 4, prologue.data(), prologue.size());
  std::memcpy(image + call_rva + 5, epilogue.data(), epilogue.size());
  constexpr std::array<std::size_t, 3> native_entries{0x4aeea10, 0x10df930,
                                                      0x1129e10};
  const std::array<std::uintptr_t, 3> mock_entries{
      reinterpret_cast<std::uintptr_t>(&mock_match),
      reinterpret_cast<std::uintptr_t>(&mock_reserve),
      reinterpret_cast<std::uintptr_t>(&mock_free)};
  std::array<std::array<std::uint8_t, 12>, 3> saved_entries{};
  for (std::size_t index = 0; index < native_entries.size(); ++index) {
    auto *entry = image + native_entries[index];
    std::memcpy(saved_entries[index].data(), entry,
                saved_entries[index].size());
    std::array<std::uint8_t, 12> thunk{0x48, 0xb8, 0, 0, 0,    0,
                                       0,    0,    0, 0, 0xff, 0xe0};
    std::memcpy(thunk.data() + 2, &mock_entries[index],
                sizeof(mock_entries[index]));
    std::memcpy(entry, thunk.data(), thunk.size());
  }
  require(FlushInstructionCache(GetCurrentProcess(), image, image_size) != 0,
          "publish isolated mock instructions");
  NativeFixture fixture;
  active_fixture = &fixture;
  std::vector<MeleeAssignmentPoint> own(24, {100, 200, 300});
  std::vector<MeleeAssignmentPoint> enemy(11, {400, 500, 600});
  auto rows = point_array(own);
  auto columns = point_array(enemy);
  MeleeAssignmentArray output{};
  using Invoke = MeleeAssignmentArray *(*)(MeleeAssignmentArray *,
                                           const MeleeAssignmentArray *,
                                           const MeleeAssignmentArray *, bool);
  const auto invoke = reinterpret_cast<Invoke>(image + call_rva - 4);
  for (const bool bias : {false, true}) {
    const auto old_calls = fixture.calls;
    require(invoke(&output, &rows, &columns, bias) == &output &&
                output.count == 24 && fixture.calls == old_calls + 1 &&
                fixture.rows == &columns && fixture.columns == &rows &&
                fixture.bias == bias,
            "actual patched unaligned CALL preserves x64 arguments, return and "
            "stack");
    for (std::int32_t index = 0; index < output.count; ++index)
      require(static_cast<const std::int32_t *>(output.data)[index] ==
                  (index < 11 ? index : 0),
              "installed guard executes inverse and remainder assignment "
              "correctly");
    release_output(fixture, output);
  }
  require(invoke(&output, &columns, &rows, true) == &output &&
              output.count == 11 && fixture.output == &output &&
              fixture.rows == &columns && fixture.columns == &rows,
          "actual installed CALL preserves normal native return path");
  release_output(fixture, output);
  for (std::size_t index = 0; index < native_entries.size(); ++index)
    std::memcpy(image + native_entries[index], saved_entries[index].data(),
                saved_entries[index].size());
  std::memcpy(image + call_rva - 4, saved_wrapper.data(), saved_wrapper.size());
  require(FlushInstructionCache(GetCurrentProcess(), image, image_size) != 0,
          "restore private audited code before lifecycle tests");
  require(install_melee_assignment_guard(image, image_size),
          "restored private caller still passes complete validation");

  image[call_rva + 1] ^= 1;
  require(!remove_melee_assignment_guard() &&
              image[call_rva + 1] == static_cast<std::uint8_t>(patched[1] ^ 1),
          "removal refuses to overwrite a foreign hook");
  require(!install_melee_assignment_guard(image, image_size),
          "installation also refuses a foreign hook");
  std::memcpy(image + call_rva, patched.data(), patched.size());
  require(remove_melee_assignment_guard() &&
              std::memcmp(image + call_rva, original.data(), original.size()) ==
                  0,
          "owned removal restores exact original native CALL");
  require(remove_melee_assignment_guard(), "repeated removal is harmless");
  require(!configure_melee_assignment_guard_for_testing(
              {mock_match, mock_reserve, mock_free}),
          "removal retains immutable callbacks for already-fetched hook calls");
  require(install_melee_assignment_guard(image, image_size) &&
              std::memcmp(image + call_rva, patched.data(), patched.size()) ==
                  0,
          "same-image reinstall reuses retained executable stub");
  require(remove_melee_assignment_guard(), "final owned removal succeeds");
  require(VirtualFree(image, 0, MEM_RELEASE) != 0,
          "release private PE fixture");
}

int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 2,
            "usage: melee_assignment_guard_tests [game.exe]");
    test_normal_native_path();
    test_crash_dimensions_transposed();
    test_nearest_tie_and_one_enemy();
    test_invalid_inputs();
    test_invalid_native_results();
    if (argc == 2)
      test_offline_installation(argv[1]);
    std::printf("PASS: %zu checks; 24-by-11 crash dimensions covered offline\n",
                checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}

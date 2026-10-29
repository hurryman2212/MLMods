#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "HomeReferenceGuard.hpp"

static std::size_t checks{};

static void require(const bool condition, const char *message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

template <typename T>
static void store(void *base, const std::size_t offset, const T value) {
  std::memcpy(static_cast<std::uint8_t *>(base) + offset, &value,
              sizeof(value));
}

static void *load_pointer(const void *base, const std::size_t offset) {
  void *value{};
  std::memcpy(&value, static_cast<const std::uint8_t *>(base) + offset,
              sizeof(value));
  return value;
}

struct Allocation {
  std::uint8_t *data{};
  std::size_t size{};

  explicit Allocation(const std::size_t length) : size(length) {
    data = static_cast<std::uint8_t *>(VirtualAlloc(
        nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    require(data != nullptr, "allocate private test memory");
  }

  ~Allocation() {
    if (data != nullptr)
      VirtualFree(data, 0, MEM_RELEASE);
  }

  Allocation(const Allocation &) = delete;
  auto operator=(const Allocation &) -> Allocation & = delete;
};

struct ObjectFixture {
  std::array<std::uint8_t, 0x1000> unit{}, region{}, owner{}, home{}, other{},
      object_class{};
  std::array<std::uint8_t, 0xa0> families{};
  std::array<void *, 4> members{};

  void prepare(std::uint8_t *image) {
    for (auto *object :
         {&unit, &region, &owner, &home, &other, &object_class}) {
      object->fill(0);
      store(object->data(), 0, image + 0x1000);
      store(object->data(), 0xc, std::int32_t{42});
      store(object->data(), 0x10, object_class.data());
    }
    families.fill(0);
    members.fill(nullptr);
    members[0] = unit.data();
    store(unit.data(), 0x320, owner.data());
    store(unit.data(), 0x328, region.data());
    store(unit.data(), 0x348, std::int32_t{0});
    store(region.data(), 0x4e8, families.data());
    store(region.data(), 0x4f0, std::int32_t{1});
    store(region.data(), 0x4f4, std::int32_t{2});
    store(families.data(), 0, members.data());
    store(families.data(), 8, std::int32_t{1});
    store(families.data(), 0xc, std::int32_t{4});
    store(families.data(), 0x20, home.data());
    store(home.data(), 0x2c8, region.data());
    store(home.data(), 0x2e0, owner.data());
    store(home.data(), 0xcf8, std::int32_t{3});
    store(home.data(), 0xca8, std::int32_t{99});
  }
};

static void test_reference_recovery(std::uint8_t *image) {
  ObjectFixture fixture;
  fixture.prepare(image);
  store(fixture.unit.data(), 0x340, fixture.home.data());
  const auto normal_before = fixture.unit;
  auto counters = home_reference_guard_counters();
  require(guarded_home_reference(fixture.unit.data()) == fixture.home.data(),
          "valid cached home passes through unchanged");
  require(fixture.unit == normal_before &&
              home_reference_guard_counters().repaired == counters.repaired &&
              home_reference_guard_counters().skipped == counters.skipped,
          "valid cached home does not mutate unit or recovery counters");

  fixture.prepare(image);
  auto expected_unit = fixture.unit;
  store(expected_unit.data(), 0x340, fixture.home.data());
  const auto expected_families = fixture.families;
  const auto expected_region = fixture.region;
  counters = home_reference_guard_counters();
  require(guarded_home_reference(fixture.unit.data()) == fixture.home.data() &&
              fixture.unit == expected_unit &&
              fixture.families == expected_families &&
              fixture.region == expected_region,
          "canonical family home repairs only the missing Home pointer");
  require(guarded_home_reference(fixture.unit.data()) == fixture.home.data() &&
              home_reference_guard_counters().repaired ==
                  counters.repaired + 1 &&
              home_reference_guard_counters().skipped == counters.skipped,
          "a second lookup uses the repaired pointer without duplicate repair");

  fixture.prepare(image);
  store(fixture.unit.data(), 0x340, fixture.other.data());
  store(fixture.unit.data(), 0x348, std::int32_t{-1});
  require(guarded_home_reference(fixture.unit.data()) == fixture.other.data() &&
              load_pointer(fixture.unit.data(), 0x340) == fixture.other.data(),
          "existing cached Home is never overwritten by family repair");

  fixture.prepare(image);
  std::memcpy(fixture.families.data() + 0x50, fixture.families.data(), 0x50);
  std::memset(fixture.families.data(), 0, 0x50);
  store(fixture.region.data(), 0x4f0, std::int32_t{2});
  store(fixture.unit.data(), 0x348, std::int32_t{1});
  fixture.members[0] = fixture.other.data();
  fixture.members[1] = fixture.unit.data();
  store(fixture.families.data(), 0x58, std::int32_t{2});
  require(
      guarded_home_reference(fixture.unit.data()) == fixture.home.data(),
      "nonzero family index and later member use native stride and membership");

  const std::vector<std::pair<const char *, std::function<void()>>> invalid{
      {"homeless family retains null Home",
       [&] {
         store(fixture.families.data(), 0x20, static_cast<void *>(nullptr));
       }},
      {"negative family index is rejected",
       [&] { store(fixture.unit.data(), 0x348, std::int32_t{-1}); }},
      {"family index equal to count is rejected",
       [&] { store(fixture.unit.data(), 0x348, std::int32_t{1}); }},
      {"missing family membership is rejected",
       [&] { fixture.members[0] = fixture.other.data(); }},
      {"home in a different region is rejected",
       [&] { store(fixture.home.data(), 0x2c8, fixture.other.data()); }},
      {"home with a different owner is rejected",
       [&] { store(fixture.home.data(), 0x2e0, fixture.other.data()); }},
      {"missing unit region is rejected",
       [&] {
         store(fixture.unit.data(), 0x328, static_cast<void *>(nullptr));
       }},
      {"missing unit owner is rejected",
       [&] {
         store(fixture.unit.data(), 0x320, static_cast<void *>(nullptr));
       }},
      {"negative family array count is rejected",
       [&] { store(fixture.region.data(), 0x4f0, std::int32_t{-1}); }},
      {"family array count beyond capacity is rejected",
       [&] { store(fixture.region.data(), 0x4f4, std::int32_t{0}); }},
      {"negative family array capacity is rejected",
       [&] { store(fixture.region.data(), 0x4f4, std::int32_t{-1}); }},
      {"unreasonable family capacity is rejected",
       [&] { store(fixture.region.data(), 0x4f4, std::int32_t{65537}); }},
      {"unreadable family array is rejected",
       [&] {
         store(fixture.region.data(), 0x4e8, reinterpret_cast<void *>(1));
       }},
      {"missing member array is rejected",
       [&] {
         store(fixture.families.data(), 0, static_cast<void *>(nullptr));
       }},
      {"empty member array is rejected",
       [&] { store(fixture.families.data(), 8, std::int32_t{0}); }},
      {"negative member count is rejected",
       [&] { store(fixture.families.data(), 8, std::int32_t{-1}); }},
      {"member count beyond capacity is rejected",
       [&] { store(fixture.families.data(), 0xc, std::int32_t{0}); }},
      {"unreasonable member capacity is rejected",
       [&] { store(fixture.families.data(), 0xc, std::int32_t{65}); }},
      {"destroying unit actor is rejected",
       [&] { store(fixture.unit.data(), 0x5d, std::uint8_t{1}); }},
      {"destroying home actor is rejected",
       [&] { store(fixture.home.data(), 0x5d, std::uint8_t{1}); }},
      {"unreadable member array is rejected",
       [&] { store(fixture.families.data(), 0, reinterpret_cast<void *>(1)); }},
      {"unit without a native vtable is rejected",
       [&] { store(fixture.unit.data(), 0, fixture.other.data()); }},
      {"unit without object class is rejected",
       [&] { store(fixture.unit.data(), 0x10, static_cast<void *>(nullptr)); }},
      {"unit with stale internal index is rejected",
       [&] { store(fixture.unit.data(), 0xc, std::int32_t{0}); }},
      {"class with foreign vtable is rejected",
       [&] { store(fixture.object_class.data(), 0, fixture.other.data()); }},
      {"unreadable canonical home is rejected",
       [&] {
         store(fixture.families.data(), 0x20, reinterpret_cast<void *>(1));
       }},
  };
  for (const auto &[message, mutate] : invalid) {
    fixture.prepare(image);
    mutate();
    const auto unit_before = fixture.unit;
    const auto families_before = fixture.families;
    counters = home_reference_guard_counters();
    require(guarded_home_reference(fixture.unit.data()) == nullptr, message);
    require(fixture.unit == unit_before &&
                fixture.families == families_before &&
                home_reference_guard_counters().repaired == counters.repaired &&
                home_reference_guard_counters().skipped == counters.skipped + 1,
            "rejected recovery preserves all unit and family data");
  }
  for (const auto flags : {0x8000U, 0x10000U, 0x40000000U}) {
    for (auto *object :
         {&fixture.unit, &fixture.region, &fixture.owner, &fixture.home}) {
      fixture.prepare(image);
      store(object->data(), 8, flags);
      require(guarded_home_reference(fixture.unit.data()) == nullptr &&
                  load_pointer(fixture.unit.data(), 0x340) == nullptr,
              "destroying, destroyed, and garbage objects are never restored");
    }
  }
  require(guarded_home_reference(nullptr) == nullptr &&
              guarded_home_reference(reinterpret_cast<void *>(1)) == nullptr,
          "null and unreadable unit pointers are rejected without a fault");

  Allocation protected_unit(0x1000);
  fixture.prepare(image);
  std::memcpy(protected_unit.data, fixture.unit.data(), fixture.unit.size());
  fixture.members[0] = protected_unit.data;
  DWORD previous{};
  require(VirtualProtect(protected_unit.data, protected_unit.size,
                         PAGE_READONLY, &previous) != 0,
          "protect isolated unit page against writes");
  require(guarded_home_reference(protected_unit.data) == nullptr &&
              load_pointer(protected_unit.data, 0x340) == nullptr,
          "read-only unit cannot be repaired through an unsafe write");
  require(VirtualProtect(protected_unit.data, protected_unit.size,
                         PAGE_NOACCESS, &previous) != 0,
          "make isolated stale unit memory inaccessible");
  require(guarded_home_reference(protected_unit.data) == nullptr,
          "inaccessible stale unit is rejected without touching its memory");
}

struct RegisterState {
  std::array<std::uint64_t, 16> gpr{};
  std::array<std::array<std::uint8_t, 16>, 16> xmm{};
};

struct RegisterContext {
  RegisterState before{}, after{};
};

struct ProbeState {
  std::uint64_t calls{}, alignment{};
  std::array<std::uint8_t, 16> clobber{};
};

struct MachineCode {
  std::vector<std::uint8_t> bytes;

  void emit(const std::initializer_list<std::uint8_t> values) {
    bytes.insert(bytes.end(), values.begin(), values.end());
  }

  template <typename T> void value(const T number) {
    const auto *start = reinterpret_cast<const std::uint8_t *>(&number);
    bytes.insert(bytes.end(), start, start + sizeof(number));
  }

  void immediate(const unsigned reg, const std::uint64_t number) {
    emit({static_cast<std::uint8_t>(reg >= 8 ? 0x49 : 0x48),
          static_cast<std::uint8_t>(0xb8 + (reg & 7))});
    value(number);
  }

  // Memory operands use RAX as a stable context base or RSP as scratch base.
  void memory(const bool load, const bool xmm, const unsigned reg,
              const std::uint32_t offset, const bool stack = false) {
    if (xmm)
      emit({0xf3});
    const auto rex =
        static_cast<std::uint8_t>((xmm ? 0x40 : 0x48) | (reg >= 8 ? 4 : 0));
    if (rex != 0x40)
      emit({rex});
    if (xmm)
      emit({0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f)});
    else
      emit({static_cast<std::uint8_t>(load ? 0x8b : 0x89)});
    emit(
        {static_cast<std::uint8_t>(0x80 | ((reg & 7) << 3) | (stack ? 4 : 0))});
    if (stack)
      emit({0x24});
    value(offset);
  }
};

static void test_installed_stubs(std::uint8_t *image) {
  Allocation harness(0x2000);
  ProbeState probe_state;
  probe_state.clobber.fill(0xa5);
  MachineCode wrapper;
  // Save the real caller's nonvolatile state. RSP is 16-byte aligned after the
  // eight pushes and 0xe8-byte local frame, including its Windows shadow area.
  wrapper.emit({0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                0x41, 0x57, 0x48, 0x81, 0xec, 0xe8, 0, 0, 0});
  wrapper.memory(false, false, 1, 0x20, true);
  wrapper.memory(false, false, 2, 0x28, true);
  for (unsigned reg = 6; reg < 16; ++reg)
    wrapper.memory(false, true, reg, 0x40 + (reg - 6) * 16, true);
  wrapper.emit({0x48, 0x8b, 0xc1, 0x48, 0x8d, 0x4c, 0x24, 0xf0});
  wrapper.memory(false, false, 1, 4 * 8);
  for (unsigned reg = 0; reg < 16; ++reg)
    wrapper.memory(true, true, reg,
                   static_cast<std::uint32_t>(offsetof(RegisterState, xmm)) +
                       reg * 16);
  for (unsigned reg = 1; reg < 16; ++reg) {
    if (reg != 4)
      wrapper.memory(true, false, reg, reg * 8);
  }
  wrapper.memory(true, false, 0, 0);
  // An inline game patch starts with RSP % 16 == 0, unlike a function entry.
  // The extra eight bytes compensate for CALL's synthetic return address.
  wrapper.emit({0x48, 0x83, 0xec, 8, 0xff, 0x54, 0x24, 0x30, 0x48, 0x83, 0xc4,
                8, 0x50, 0x51});
  wrapper.memory(true, false, 0, 0x30, true);
  constexpr auto after =
      static_cast<std::uint32_t>(offsetof(RegisterContext, after));
  for (unsigned reg = 2; reg < 16; ++reg)
    wrapper.memory(false, false, reg, after + reg * 8);
  wrapper.memory(true, false, 1, 8, true);
  wrapper.memory(false, false, 1, after);
  wrapper.memory(true, false, 1, 0, true);
  wrapper.memory(false, false, 1, after + 8);
  for (unsigned reg = 0; reg < 16; ++reg)
    wrapper.memory(
        false, true, reg,
        after + static_cast<std::uint32_t>(offsetof(RegisterState, xmm)) +
            reg * 16);
  wrapper.emit({0x59, 0x58});
  for (unsigned reg = 6; reg < 16; ++reg)
    wrapper.memory(true, true, reg, 0x40 + (reg - 6) * 16, true);
  wrapper.emit({0x48, 0x81, 0xc4, 0xe8, 0,    0,    0,    0x41, 0x5f, 0x41,
                0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5d, 0x5b, 0xc3});
  require(wrapper.bytes.size() < 0x1000, "register wrapper fits private page");
  std::memcpy(harness.data, wrapper.bytes.data(), wrapper.bytes.size());

  MachineCode probe;
  probe.emit({0x53, 0x48, 0x83, 0xec, 0x20});
  probe.immediate(3, reinterpret_cast<std::uintptr_t>(&probe_state));
  probe.emit({0x48, 0xff, 0x03, 0x48, 0x8d, 0x44, 0x24, 0x28, 0x83, 0xe0, 0x0f,
              0x48, 0x89, 0x43, 8});
  probe.immediate(0, reinterpret_cast<std::uintptr_t>(&guarded_home_reference));
  probe.emit({0xff, 0xd0});
  probe.memory(false, false, 0, 0x18, true);
  for (const unsigned reg : {1U, 2U, 8U, 9U, 10U, 11U})
    probe.immediate(reg, 0xdead000000000000ULL + reg);
  probe.immediate(0,
                  reinterpret_cast<std::uintptr_t>(probe_state.clobber.data()));
  for (unsigned reg = 0; reg < 6; ++reg)
    probe.memory(true, true, reg, 0);
  probe.memory(true, false, 0, 0x18, true);
  probe.emit({0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
  require(probe.bytes.size() < 0x1000, "helper ABI probe fits private page");
  std::memcpy(harness.data + 0x1000, probe.bytes.data(), probe.bytes.size());
  require(FlushInstructionCache(GetCurrentProcess(), harness.data,
                                harness.size) != 0,
          "publish private machine-code harness");

  const std::array<std::array<std::size_t, 2>, 2> sites{
      {{0x4bd7ad0, 0x4bd7c25}, {0x4bd91e0, 0x4bd9325}}};
  using Invoke = void (*)(RegisterContext *, void *);
  const auto invoke = reinterpret_cast<Invoke>(harness.data);
  for (const auto &[site, tail] : sites) {
    auto *stub = static_cast<std::uint8_t *>(load_pointer(image + site, 6));
    require(stub != nullptr, "installed indirect jump publishes a stub");
    const auto helper =
        reinterpret_cast<std::uintptr_t>(&guarded_home_reference);
    std::size_t helper_offset{}, helper_matches{};
    for (std::size_t offset = 0; offset + sizeof(helper) <= 512; ++offset) {
      if (std::memcmp(stub + offset, &helper, sizeof(helper)) == 0) {
        helper_offset = offset;
        ++helper_matches;
      }
    }
    require(helper_matches == 1,
            "stub has one exact helper address to instrument");
    DWORD previous{};
    require(VirtualProtect(stub, 512, PAGE_EXECUTE_READWRITE, &previous) != 0,
            "instrument private generated helper call");
    store(stub, helper_offset, harness.data + 0x1000);
    DWORD ignored{};
    require(VirtualProtect(stub, 512, previous, &ignored) != 0,
            "restore generated stub protection");

    std::array<std::uint8_t, 6> resume_bytes{}, tail_bytes{};
    std::memcpy(resume_bytes.data(), image + site + 16, resume_bytes.size());
    std::memcpy(tail_bytes.data(), image + tail, tail_bytes.size());
    image[site + 16] = 0xc3;
    const std::array<std::uint8_t, 6> skip{0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3};
    std::memcpy(image + tail, skip.data(), skip.size());
    require(FlushInstructionCache(GetCurrentProcess(), nullptr, 0) != 0,
            "publish isolated native continuations");
    ObjectFixture fixture;
    for (int mode = 0; mode < 4; ++mode) {
      fixture.prepare(image);
      void *slot = fixture.unit.data();
      if (mode == 0)
        store(fixture.unit.data(), 0x340, fixture.home.data());
      if (mode == 2)
        store(fixture.families.data(), 0x20, static_cast<void *>(nullptr));
      if (mode == 3)
        slot = nullptr;
      RegisterContext context;
      for (unsigned reg = 0; reg < 16; ++reg) {
        context.before.gpr[reg] = 0x1234567800000000ULL + reg;
        for (unsigned byte = 0; byte < 16; ++byte)
          context.before.xmm[reg][byte] =
              static_cast<std::uint8_t>(reg * 16 + byte);
      }
      context.before.gpr[6] = reinterpret_cast<std::uintptr_t>(&slot + 1);
      context.before.gpr[7] = reinterpret_cast<std::uintptr_t>(&slot);
      const auto previous_calls = probe_state.calls;
      invoke(&context, image + site);
      require(static_cast<std::int32_t>(context.after.gpr[0]) ==
                  (mode < 2 ? 3 : -1),
              "installed guard selects exact native resume or per-unit skip");
      for (unsigned reg = 2; reg < 16; ++reg)
        require(
            context.after.gpr[reg] == context.before.gpr[reg],
            "all live integer registers and exact stack pointer survive guard");
      require(
          context.after.xmm == context.before.xmm,
          "volatile and nonvolatile vector registers survive helper clobbers");
      require(
          probe_state.calls == previous_calls + (mode == 0 ? 0 : 1),
          "valid cached home bypasses helper; exceptional paths invoke once");
      if (mode != 0)
        require(
            probe_state.alignment == 8,
            "helper enters with Windows x64 stack alignment and shadow space");
      require(load_pointer(fixture.unit.data(), 0x340) ==
                  (mode < 2 ? fixture.home.data() : nullptr),
              "only valid canonical recovery writes a missing Home");
    }

    // Exercise the real native loop tail after an invalid first unit. The
    // second unit must still reach resume, proving no whole-loop abort.
    std::memcpy(image + tail, tail_bytes.data(), tail_bytes.size());
    ObjectFixture next;
    next.prepare(image);
    store(next.unit.data(), 0x340, next.home.data());
    std::array<void *, 2> slots{nullptr, next.unit.data()};
    RegisterContext iteration;
    iteration.before.gpr[6] =
        reinterpret_cast<std::uintptr_t>(slots.data() + 2);
    iteration.before.gpr[7] = reinterpret_cast<std::uintptr_t>(slots.data());
    require(FlushInstructionCache(GetCurrentProcess(), image + tail, 13) != 0,
            "publish original per-element native loop tail");
    invoke(&iteration, image + site);
    require(
        iteration.after.gpr[0] == 3 &&
            iteration.after.gpr[7] ==
                reinterpret_cast<std::uintptr_t>(slots.data() + 1),
        "skipping one invalid unit continues native processing of the next");
    std::memcpy(image + site + 16, resume_bytes.data(), resume_bytes.size());
    require(VirtualProtect(stub, 512, PAGE_EXECUTE_READWRITE, &previous) != 0,
            "remove private helper probe");
    store(stub, helper_offset, helper);
    require(VirtualProtect(stub, 512, previous, &ignored) != 0,
            "restore original stub protection after instrumentation");
    require(FlushInstructionCache(GetCurrentProcess(), nullptr, 0) != 0,
            "restore production stub and native bytes after harness");
  }
}

template <typename T>
static void reject_changed_identity(T &field, std::uint8_t *image,
                                    const std::size_t size) {
  field ^= 1;
  require(!install_home_reference_guard(image, size),
          "changed executable identity is rejected before publication");
  field ^= 1;
}

static void test_offline_image(const char *filename) {
  std::ifstream stream(filename, std::ios::binary | std::ios::ate);
  require(stream.good(), "open current executable read only");
  const auto length = stream.tellg();
  require(length > static_cast<std::streamoff>(sizeof(IMAGE_NT_HEADERS64)),
          "input contains PE headers");
  std::vector<std::uint8_t> file(static_cast<std::size_t>(length));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
  require(stream.good(), "read complete current executable");
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
  require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
              static_cast<std::size_t>(dos->e_lfanew) <
                  file.size() - sizeof(IMAGE_NT_HEADERS64),
          "PE header bounds are valid");
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(file.data() + dos->e_lfanew);
  require(nt->Signature == IMAGE_NT_SIGNATURE &&
              nt->OptionalHeader.SizeOfHeaders <= file.size(),
          "PE mapping header is valid");
  Allocation mapped(nt->OptionalHeader.SizeOfImage);
  std::memcpy(mapped.data, file.data(), nt->OptionalHeader.SizeOfHeaders);
  const auto *sections = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
      reinterpret_cast<const std::uint8_t *>(&nt->OptionalHeader) +
      nt->FileHeader.SizeOfOptionalHeader);
  for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
    const auto &section = sections[index];
    require(static_cast<std::size_t>(section.PointerToRawData) +
                        section.SizeOfRawData <=
                    file.size() &&
                static_cast<std::size_t>(section.VirtualAddress) +
                        section.SizeOfRawData <=
                    mapped.size,
            "PE sections stay within read-only source and private mapping");
    std::memcpy(mapped.data + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }
  require(configure_home_reference_guard_for_testing(mapped.data, mapped.size),
          "configure trusted native image for isolated object validation");
  test_reference_recovery(mapped.data);
  require(!install_home_reference_guard(nullptr, mapped.size) &&
              !install_home_reference_guard(mapped.data, 0) &&
              !install_home_reference_guard(mapped.data, 0x4bd7ad0),
          "null and truncated images fail closed");
  auto *mapped_nt =
      reinterpret_cast<IMAGE_NT_HEADERS64 *>(mapped.data + dos->e_lfanew);
  reject_changed_identity(mapped_nt->Signature, mapped.data, mapped.size);
  reject_changed_identity(mapped_nt->FileHeader.Machine, mapped.data,
                          mapped.size);
  reject_changed_identity(mapped_nt->FileHeader.SizeOfOptionalHeader,
                          mapped.data, mapped.size);
  reject_changed_identity(mapped_nt->FileHeader.TimeDateStamp, mapped.data,
                          mapped.size);
  reject_changed_identity(mapped_nt->OptionalHeader.Magic, mapped.data,
                          mapped.size);
  reject_changed_identity(mapped_nt->OptionalHeader.SizeOfImage, mapped.data,
                          mapped.size);
  reject_changed_identity(mapped_nt->OptionalHeader.CheckSum, mapped.data,
                          mapped.size);
  std::array<std::uint8_t, 16> original_first{}, original_second{};
  std::memcpy(original_first.data(), mapped.data + 0x4bd7ad0, 16);
  std::memcpy(original_second.data(), mapped.data + 0x4bd91e0, 16);
  mapped.data[0x4bd91e0] ^= 1;
  require(!install_home_reference_guard(mapped.data, mapped.size) &&
              std::memcmp(original_first.data(), mapped.data + 0x4bd7ad0, 16) ==
                  0,
          "corrupt second site fails preflight without publishing the first");
  mapped.data[0x4bd91e0] ^= 1;
  const std::array<std::array<std::size_t, 2>, 3> fingerprints{
      {{0x4bd7820, 0x1490}, {0x4bd8cb0, 0xea9}, {0x4c255d0, 0x1ee}}};
  for (const auto &[rva, count] : fingerprints) {
    for (std::size_t index = 0; index < count; ++index) {
      mapped.data[rva + index] ^= 1;
      require(!install_home_reference_guard(mapped.data, mapped.size),
              "every audited demand and canonical-home fingerprint byte is "
              "enforced");
      mapped.data[rva + index] ^= 1;
      require(std::memcmp(original_first.data(), mapped.data + 0x4bd7ad0, 16) ==
                      0 &&
                  std::memcmp(original_second.data(), mapped.data + 0x4bd91e0,
                              16) == 0,
              "failed fingerprint validation never leaves a partial hook");
    }
  }
  require(install_home_reference_guard(mapped.data, mapped.size),
          "verified 0.8.111 image accepts both native home guards");
  require(mapped.data[0x4bd7ad0] == 0xff && mapped.data[0x4bd7ad1] == 0x25 &&
              mapped.data[0x4bd91e0] == 0xff && mapped.data[0x4bd91e1] == 0x25,
          "both validated sites receive indirect jumps");
  require(install_home_reference_guard(mapped.data, mapped.size),
          "repeated installation keeps the same published guards");
  require(!configure_home_reference_guard_for_testing(mapped.data, mapped.size),
          "test reconfiguration cannot replace a published image");
  test_installed_stubs(mapped.data);
  const auto patch_byte = mapped.data[0x4bd91e0];
  mapped.data[0x4bd91e0] ^= 1;
  require(
      !remove_home_reference_guard() &&
          std::memcmp(original_first.data(), mapped.data + 0x4bd7ad0, 16) ==
              0 &&
          mapped.data[0x4bd91e0] == (patch_byte ^ 1),
      "conflicted removal restores owned sites and preserves foreign bytes");
  mapped.data[0x4bd91e0] = patch_byte;
  require(remove_home_reference_guard(), "remove both owned native guards");
  require(
      std::memcmp(original_first.data(), mapped.data + 0x4bd7ad0, 16) == 0 &&
          std::memcmp(original_second.data(), mapped.data + 0x4bd91e0, 16) == 0,
      "removal restores both complete native instruction sequences");
  require(remove_home_reference_guard(), "repeated removal is harmless");
}

auto main(int argc, char **argv) -> int {
  try {
    require(argc == 2, "usage: home_reference_guard_tests.exe <game.exe>");
    test_offline_image(argv[1]);
    std::printf("Home reference guard: %zu checks passed.\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}

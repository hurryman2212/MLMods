#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <utility>
#include <vector>

#include <Windows.h>

#include "ManorLoadNative.hpp"
#include "PlacementHooks.hpp"

struct PlacementBridgeSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
  std::uint32_t mxcsr{};
  std::uint16_t x87_control{};
};

struct PlacementBridgeState {
  PlacementBridgeSnapshot before{}, after{};
  std::array<std::uint8_t, 0x380> native_stack{};
  std::uintptr_t argument{}, callback_rsp{};
  std::uint32_t calls{};
  std::uint32_t clobber_mxcsr{0x5f80};
  std::uint16_t clobber_x87{0x077f};
  std::array<std::uint8_t, 0x3b0> owner{};
};

struct PlacementCallState {
  void *owner{}, *model{}, *scale{}, *pawn{}, *hovered{};
  float width{};
  std::int32_t level{};
  std::uint8_t primary{}, secondary{}, variant{};
  unsigned artisan_calls{}, primary_calls{}, secondary_calls{}, hover_calls{};
};

static PlacementCallState call_state;

static auto hover_call(void *pawn, void *candidate) -> void {
  call_state.pawn = pawn;
  call_state.hovered = candidate;
  ++call_state.hover_calls;
}

static auto artisan_call(void *owner, std::int32_t level, std::uint8_t primary,
                         std::uint8_t secondary, void *model) -> void {
  call_state.owner = owner;
  call_state.level = level;
  call_state.primary = primary;
  call_state.secondary = secondary;
  call_state.model = model;
  ++call_state.artisan_calls;
}

static auto primary_call(void *owner, float width, float *scale,
                         std::int32_t level, std::uint8_t variant)
    -> std::int32_t {
  call_state.owner = owner;
  call_state.width = width;
  call_state.scale = scale;
  call_state.level = level;
  call_state.variant = variant;
  *scale = 1.125F;
  ++call_state.primary_calls;
  return 461;
}

static auto secondary_call(void *owner, float width, float *scale,
                           std::int32_t level, std::uint8_t variant)
    -> std::int32_t {
  call_state.owner = owner;
  call_state.width = width;
  call_state.scale = scale;
  call_state.level = level;
  call_state.variant = variant;
  *scale = 0.875F;
  ++call_state.secondary_calls;
  return 462;
}

static auto bridge_tests() -> int {
  int checks{}, failures{};
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "placement bridge: %s\n", message);
    }
  };
  const auto *placeholder = reinterpret_cast<const void *>(1);
  for (const auto kind :
       {PlacementBridgeKind::Load, PlacementBridgeKind::Finish,
        PlacementBridgeKind::Extension, PlacementBridgeKind::Visual}) {
    expect(placement_make_bridge(nullptr, placeholder, kind).empty() &&
               placement_make_bridge(placeholder, nullptr, kind).empty(),
           "incomplete callback contract rejected");
    for (const bool carry : {false, true}) {
      auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
          nullptr, 0x5000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
      if (memory == nullptr)
        return failures + 1;
      auto *state = new (memory + 0x4000) PlacementBridgeState{};
      state->owner[0x3a1] = carry ? 1 : 0;
      auto *patterns = memory + 0x4c00;
      static_assert(sizeof(PlacementBridgeState) <= 0xc00);
      for (std::size_t i{}; i < 256; ++i)
        patterns[i] = static_cast<std::uint8_t>(i * 7 + 3);
      const auto append32 = [](std::vector<std::uint8_t> &code,
                               std::int32_t value) {
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
        code.insert(code.end(), bytes, bytes + sizeof(value));
      };
      const auto immediate = [](std::vector<std::uint8_t> &code, unsigned reg,
                                std::uintptr_t value) {
        code.push_back(reg >= 8 ? 0x49 : 0x48);
        code.push_back(static_cast<std::uint8_t>(0xb8 + reg % 8));
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
        code.insert(code.end(), bytes, bytes + sizeof(value));
      };
      const auto relative = [&](std::vector<std::uint8_t> &code,
                                std::size_t origin, const void *target) {
        const auto displacement =
            reinterpret_cast<const std::uint8_t *>(target) -
            (memory + origin + code.size() + 4);
        expect(displacement >= INT32_MIN && displacement <= INT32_MAX,
               "synthetic RIP displacement fits");
        append32(code, static_cast<std::int32_t>(displacement));
      };
      const auto vector_stack = [&](std::vector<std::uint8_t> &code,
                                    unsigned reg, bool load) {
        code.push_back(0xf3);
        if (reg >= 8)
          code.push_back(0x44);
        code.insert(code.end(),
                    {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                     static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
        append32(code, static_cast<std::int32_t>(0x380 + (reg - 6) * 16));
      };
      const auto snapshot = [&](std::vector<std::uint8_t> &code,
                                std::size_t origin,
                                PlacementBridgeSnapshot *destination) {
        for (unsigned reg{}; reg < 16; ++reg) {
          code.insert(code.end(),
                      {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48), 0x89,
                       static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
          relative(code, origin, &destination->registers[reg]);
        }
        for (unsigned reg{}; reg < 16; ++reg) {
          code.push_back(0xf3);
          if (reg >= 8)
            code.push_back(0x44);
          code.insert(
              code.end(),
              {0x0f, 0x7f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
          relative(code, origin, &destination->vectors[reg]);
        }
        code.insert(code.end(), {0x9c, 0x8f, 0x05});
        relative(code, origin, &destination->flags);
        code.insert(code.end(), {0x0f, 0xae, 0x1d});
        relative(code, origin, &destination->mxcsr);
        code.insert(code.end(), {0xd9, 0x3d});
        relative(code, origin, &destination->x87_control);
      };

      // Preserve the C++ caller and reproduce native midpoint RSP%16 == 0.
      std::vector<std::uint8_t> entry{0x53, 0x55, 0x56, 0x57, 0x41,
                                      0x54, 0x41, 0x55, 0x41, 0x56,
                                      0x41, 0x57, 0x48, 0x81, 0xec};
      append32(entry, 0x428);
      for (unsigned reg = 6; reg < 16; ++reg)
        vector_stack(entry, reg, false);
      for (std::int32_t offset{}; offset < 0x380; offset += 4) {
        entry.insert(entry.end(), {0xc7, 0x84, 0x24});
        append32(entry, offset);
        append32(entry, static_cast<std::int32_t>(0xa5a5a5a5U));
      }
      for (unsigned reg{}; reg < 16; ++reg)
        if (reg != 4)
          immediate(entry, reg,
                    kind == PlacementBridgeKind::Visual && reg == 3
                        ? reinterpret_cast<std::uintptr_t>(state->owner.data())
                        : 0xabc0000000000000ULL + reg);
      for (unsigned reg{}; reg < 16; ++reg) {
        entry.push_back(0xf3);
        if (reg >= 8)
          entry.push_back(0x44);
        entry.insert(
            entry.end(),
            {0x0f, 0x6f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
        relative(entry, 0, patterns + reg * 16);
      }
      entry.push_back(carry ? 0xf9 : 0xf8);
      snapshot(entry, 0, &state->before);
      append_absolute_jump(entry, memory + 0x1000);

      // Deliberately violate even the callback's nonvolatile-register ABI.
      // The midpoint bridge must independently restore every live value.
      std::vector<std::uint8_t> callback{0x48, 0x89, 0x0d};
      relative(callback, 0x1800, &state->argument);
      callback.insert(callback.end(), {0x48, 0x89, 0x25});
      relative(callback, 0x1800, &state->callback_rsp);
      callback.insert(callback.end(), {0xff, 0x05});
      relative(callback, 0x1800, &state->calls);
      for (std::uint8_t offset = 8; offset <= 36; offset += 4) {
        callback.insert(callback.end(), {0xc7, 0x44, 0x24, offset});
        append32(callback, static_cast<std::int32_t>(0xdeadbeefU));
      }
      callback.insert(callback.end(), {0x0f, 0xae, 0x15});
      relative(callback, 0x1800, &state->clobber_mxcsr);
      callback.insert(callback.end(), {0xd9, 0x2d});
      relative(callback, 0x1800, &state->clobber_x87);
      for (unsigned reg{}; reg < 16; ++reg) {
        callback.push_back(0x66);
        if (reg >= 8)
          callback.push_back(0x45);
        callback.insert(
            callback.end(),
            {0x0f, 0xef, static_cast<std::uint8_t>(0xc0 + (reg % 8) * 9)});
      }
      for (unsigned reg{}; reg < 16; ++reg)
        if (reg != 4)
          immediate(callback, reg, 0xdef0000000000000ULL + reg);
      callback.push_back(0xc3);

      const auto bridge =
          placement_make_bridge(memory + 0x1800, memory + 0x2400, kind);
      std::vector<std::uint8_t> resume;
      snapshot(resume, 0x2400, &state->after);
      for (std::int32_t offset{}; offset < 0x380; offset += 8) {
        resume.insert(resume.end(), {0x48, 0x8b, 0x84, 0x24});
        append32(resume, offset);
        resume.insert(resume.end(), {0x48, 0x89, 0x05});
        relative(resume, 0x2400, state->native_stack.data() + offset);
      }
      for (unsigned reg = 6; reg < 16; ++reg)
        vector_stack(resume, reg, true);
      resume.insert(resume.end(), {0x48, 0x81, 0xc4});
      append32(resume, 0x428);
      resume.insert(resume.end(),
                    {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e,
                     0x5d, 0x5b, 0x31, 0xc0, 0xc3});
      const bool layout = entry.size() <= 0x1000 && bridge.size() <= 0x800 &&
                          callback.size() <= 0x800 && resume.size() <= 0x1c00;
      expect(layout, "synthetic code regions do not overlap");
      if (!layout) {
        VirtualFree(memory, 0, MEM_RELEASE);
        continue;
      }
      std::memcpy(memory, entry.data(), entry.size());
      std::memcpy(memory + 0x1000, bridge.data(), bridge.size());
      std::memcpy(memory + 0x1800, callback.data(), callback.size());
      std::memcpy(memory + 0x2400, resume.data(), resume.size());
      DWORD previous{};
      const bool executable =
          VirtualProtect(memory, 0x4000, PAGE_EXECUTE_READ, &previous) != 0 &&
          FlushInstructionCache(GetCurrentProcess(), memory, 0x4000) != 0;
      expect(executable, "code RX and separate state RW");
      if (executable) {
        expect(reinterpret_cast<int (*)()>(memory)() == 0,
               "native continuation reached");
        expect(state->before.registers[4] % 16 == 0 &&
                   state->callback_rsp % 16 == 8,
               "native and callback stack alignment correct");
        expect(state->calls == 1, "callback executes exactly once");
        expect(state->argument ==
                   state->before
                       .registers[kind == PlacementBridgeKind::Visual ? 3 : 6],
               "callback receives native master or owner register");
        for (unsigned reg{}; reg < 16; ++reg) {
          auto expected = state->before.registers[reg];
          if (kind == PlacementBridgeKind::Load && reg == 1)
            expected = state->before.registers[6] + 0x308;
          if (kind == PlacementBridgeKind::Extension && reg == 15)
            expected = 0xa5a5a5a5a5a5a5a5ULL;
          expect(expected == state->after.registers[reg],
                 "all GPRs preserved except displaced native replay");
          auto vector = state->before.vectors[reg];
          if (kind == PlacementBridgeKind::Finish && reg == 8)
            vector.fill(0xa5);
          expect(vector == state->after.vectors[reg],
                 "all XMMs preserved except native XMM8 restore");
        }
        if (kind == PlacementBridgeKind::Visual) {
          expect((state->after.flags & 0x8d5) == (carry ? 0 : 0x44),
                 "replayed upgrading comparison restores native branch flags");
        } else {
          expect(state->before.flags == state->after.flags,
                 "all flags preserved");
        }
        expect(state->before.mxcsr == state->after.mxcsr &&
                   state->before.x87_control == state->after.x87_control,
               "floating-point controls preserved");
        for (const auto byte : state->native_stack)
          expect(byte == 0xa5,
                 "native stack untouched by callback shadow space");
      }
      expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
             "synthetic memory released");
    }
  }
  expect(placement_make_bridge(placeholder, placeholder,
                               static_cast<PlacementBridgeKind>(99))
             .empty(),
         "unsupported midpoint rejected");
  std::printf("placement bridge: %d checks, %d failures\n", checks, failures);
  return failures;
}

static auto profile_tests(const char *path) -> int {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(stream),
                                       {}};
  if (file.size() < sizeof(IMAGE_DOS_HEADER)) {
    std::fprintf(stderr,
                 "Cannot read game executable for offline verification\n");
    return 1;
  }
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, file.data(), sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      static_cast<std::size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
          file.size())
    return 1;
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.OptionalHeader.SizeOfImage != 0x09dfd000 ||
      nt.OptionalHeader.SizeOfHeaders > file.size())
    return 1;
  std::vector<std::uint8_t> image(nt.OptionalHeader.SizeOfImage);
  std::memcpy(image.data(), file.data(), nt.OptionalHeader.SizeOfHeaders);
  const auto sections = static_cast<std::size_t>(dos.e_lfanew) +
                        sizeof(std::uint32_t) + sizeof(IMAGE_FILE_HEADER) +
                        nt.FileHeader.SizeOfOptionalHeader;
  for (unsigned i{}; i < nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER section{};
    if (sections + (i + 1) * sizeof(section) > file.size())
      return 1;
    std::memcpy(&section, file.data() + sections + i * sizeof(section),
                sizeof(section));
    if (static_cast<std::size_t>(section.PointerToRawData) +
                section.SizeOfRawData >
            file.size() ||
        static_cast<std::size_t>(section.VirtualAddress) +
                section.SizeOfRawData >
            image.size())
      return 1;
    std::memcpy(image.data() + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }
  int checks{}, failures{};
  const auto expect = [&](bool ok, const char *message) {
    ++checks;
    if (!ok) {
      ++failures;
      std::fprintf(stderr, "placement profile: %s\n", message);
    }
  };
  expect(placement_validate_image(image.data()), "current executable accepted");
  expect(!placement_validate_image(nullptr), "null image rejected");
  expect(!placement_validate_image(reinterpret_cast<const std::uint8_t *>(1)),
         "unreadable image rejected");
  for (const auto &fingerprint : placement_fingerprints()) {
    expect(!fingerprint.bytes.empty() && fingerprint.bytes.size() <= 32,
           "bounded context signature");
    for (std::size_t i{}; i < fingerprint.bytes.size(); ++i) {
      image[fingerprint.rva + i] ^= 1;
      expect(!placement_validate_image(image.data()),
             "any changed context byte fails closed");
      image[fingerprint.rva + i] ^= 1;
    }
  }
  for (std::size_t offset :
       {std::size_t{0}, offsetof(IMAGE_DOS_HEADER, e_lfanew),
        static_cast<std::size_t>(dos.e_lfanew),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, FileHeader) +
            offsetof(IMAGE_FILE_HEADER, Machine),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, FileHeader) +
            offsetof(IMAGE_FILE_HEADER, TimeDateStamp),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, Magic),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, CheckSum)}) {
    image[offset] ^= 1;
    expect(!placement_validate_image(image.data()),
           "wrong executable identity rejected");
    image[offset] ^= 1;
  }
  // Reproduce the other mod's entry detours; our audited middle/tail sites
  // remain independent and every callback traverses the original body.
  for (const auto &[rva, size] :
       std::array<std::pair<std::size_t, std::size_t>, 5>{{{0x4cb56d0, 10},
                                                           {0x4ce11f0, 7},
                                                           {0x4d2cd80, 5},
                                                           {0x4d2c8f0, 5},
                                                           {0x4cf9750, 15}}})
    std::memset(image.data() + rva, 0xcc, size);
  expect(placement_validate_image(image.data()),
         "CityEditor entry detours do not conflict");
  // The courtyard point-containment call is owned by ManorEditor. Our
  // accepted-candidate hook must not fingerprint or overwrite that call.
  std::memset(image.data() + 0x4b35394, 0xcc, 5);
  expect(placement_validate_image(image.data()),
         "ManorEditor selection hook remains independent");
  const auto callback =
      reinterpret_cast<const void *>(&placement_validate_image);
  const auto artisan = reinterpret_cast<const void *>(&artisan_call);
  const auto primary = reinterpret_cast<const void *>(&primary_call);
  const auto secondary = reinterpret_cast<const void *>(&secondary_call);
  const auto hover = reinterpret_cast<const void *>(&hover_call);
  const std::array<std::pair<std::uintptr_t, std::size_t>, 9> sites{
      {{0x4c99952, 7},
       {0x4d0ca5b, 9},
       {0x4cbc9ba, 5},
       {0x4d2eb31, 8},
       {0x4d09e0b, 7},
       {0x4cf9825, 5},
       {0x4d0460f, 5},
       {0x4d05b1f, 5},
       {0x4b353c5, 5}}};
  const auto before = image;
  auto after = before;
  const std::array<std::pair<std::uintptr_t, std::size_t>, 3> manor_sites{
      {{0x4c9301e, 14}, {0x4c931c7, 9}, {0x4c9340a, 7}}};
  {
    ManorLoadHook manor;
    expect(manor_load_validate_image(image.data()),
           "Manor and placement profiles both accept pristine merged input");
    expect(manor.install(image.data(), callback, callback, callback),
           "Manor group installs before placement group");
    image[0x4d0ca5b] ^= 1;
    PlacementHooks rejected;
    expect(!rejected.install(image.data(), callback, callback, callback,
                             artisan, primary, secondary, hover),
           "second group rejects a new conflict after first group installed");
    image[0x4d0ca5b] ^= 1;
    expect(manor.restore() && image == before,
           "first group can roll back if second group validation fails");
  }
  {
    ManorLoadHook manor;
    PlacementHooks hooks;
    expect(!hooks.install(image.data(), nullptr, callback, callback, artisan,
                          primary, secondary, hover),
           "null world callback rejected");
    expect(!hooks.install(image.data(), callback, nullptr, callback, artisan,
                          primary, secondary, hover),
           "null owner callback rejected");
    expect(!hooks.install(image.data(), callback, callback, nullptr, artisan,
                          primary, secondary, hover),
           "null assembly callback rejected");
    expect(!hooks.install(image.data(), callback, callback, callback, nullptr,
                          primary, secondary, hover),
           "null artisan callback rejected");
    expect(!hooks.install(image.data(), callback, callback, callback, artisan,
                          nullptr, secondary, hover),
           "null primary chooser rejected");
    expect(!hooks.install(image.data(), callback, callback, callback, artisan,
                          primary, nullptr, hover),
           "null secondary chooser rejected");
    expect(!hooks.install(image.data(), callback, callback, callback, artisan,
                          primary, secondary, nullptr),
           "null hover callback rejected");
    image[0x4d0ca5b] ^= 1;
    expect(!hooks.install(image.data(), callback, callback, callback, artisan,
                          primary, secondary, hover),
           "foreign hook rejected");
    image[0x4d0ca5b] ^= 1;
    expect(image == before,
           "rejected installations do not modify detached image");
    expect(manor.install(image.data(), callback, callback, callback),
           "three Manor hooks install for combined twelve-hook mod");
    expect(placement_validate_image(image.data()),
           "Manor hook installation leaves every placement context intact");
    const bool installed =
        hooks.install(image.data(), callback, callback, callback, artisan,
                      primary, secondary, hover);
    expect(installed && hooks.installed(),
           "all nine lifecycle, house and selection hooks install");
    if (installed) {
      expect(!hooks.install(image.data(), callback, callback, callback, artisan,
                            primary, secondary, hover),
             "double install rejected");
      for (const auto &[rva, size] : sites) {
        expect(image[rva] == (size == 5 ? 0xe8 : 0xe9),
               "native CALL or midpoint JMP preserved");
        std::memcpy(after.data() + rva, image.data() + rva, size);
      }
      for (const auto &[rva, size] : manor_sites) {
        expect(image[rva] == 0xe9, "Manor midpoint remains a jump");
        std::memcpy(after.data() + rva, image.data() + rva, size);
      }
      expect(image == after, "only twelve audited instruction ranges changed");
      const auto target = [&](std::uintptr_t rva) {
        std::int32_t displacement{};
        std::memcpy(&displacement, image.data() + rva + 1,
                    sizeof(displacement));
        return image.data() + rva + 5 + displacement;
      };
      // Execute the installed tail-jump thunks using the real Windows ABI.
      // This covers the stack fifth argument, XMM1 width, output pointer and
      // EAX result without executing any function from the game image.
      call_state = {};
      std::uint64_t model = 0x1020304050607080ULL;
      auto *owner = &model;
      reinterpret_cast<decltype(&artisan_call)>(target(0x4cf9825))(owner, 4, 1,
                                                                   2, &model);
      expect(call_state.owner == owner && call_state.level == 4 &&
                 call_state.primary == 1 && call_state.secondary == 2 &&
                 call_state.model == &model && call_state.artisan_calls == 1,
             "artisan CALL thunk preserves all five native arguments");
      float scale{};
      const auto primary_result = reinterpret_cast<decltype(&primary_call)>(
          target(0x4d0460f))(owner, 673.25F, &scale, 4, 3);
      expect(primary_result == 461 && scale == 1.125F &&
                 call_state.owner == owner && call_state.width == 673.25F &&
                 call_state.scale == &scale && call_state.level == 4 &&
                 call_state.variant == 3 && call_state.primary_calls == 1,
             "primary CALL thunk preserves mixed scalar ABI and return");
      const auto secondary_result = reinterpret_cast<decltype(&secondary_call)>(
          target(0x4d05b1f))(owner, 529.5F, &scale, 3, 4);
      expect(secondary_result == 462 && scale == 0.875F &&
                 call_state.owner == owner && call_state.width == 529.5F &&
                 call_state.scale == &scale && call_state.level == 3 &&
                 call_state.variant == 4 && call_state.secondary_calls == 1,
             "secondary CALL thunk preserves mixed scalar ABI and return");
      auto *pawn = &scale;
      reinterpret_cast<decltype(&hover_call)>(target(0x4b353c5))(pawn, owner);
      expect(call_state.pawn == pawn && call_state.hovered == owner &&
                 call_state.hover_calls == 1,
             "hover CALL thunk preserves pawn and candidate exactly once");
      manor.retain_until_exit();
      hooks.retain_until_exit();
    }
  }
  expect(image == after,
         "destruction retains hooks and executable continuations");
  std::printf("placement profile/installer: %d checks, %d failures\n", checks,
              failures);
  return failures;
}

auto main(int argc, char **argv) -> int {
  if (argc != 2) {
    std::fprintf(stderr,
                 "Usage: placement_hook_tests <game executable, read-only>\n");
    return 1;
  }
  const int failures = bridge_tests() + profile_tests(argv[1]);
  return failures == 0 ? 0 : 1;
}

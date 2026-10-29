#include <array>
#include <cstdio>
#include <cstring>

#include "../src/MLAllRegionRoads.cpp"

auto main() -> int {
  auto *image = static_cast<std::uint8_t *>(VirtualAlloc(
      nullptr, 0x09deb000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  auto *code = static_cast<std::uint8_t *>(VirtualAlloc(
      nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (image == nullptr || code == nullptr) {
    return 2;
  }
  int passed = 0;
  int failed = 0;
  const auto expect = [&](bool value, const char *name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    value ? ++passed : ++failed;
  };
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(image);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x100;
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + 0x100);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->FileHeader.TimeDateStamp = 0xb47dcd3f;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfImage = 0x09deb000;
  nt->OptionalHeader.CheckSum = 0x09a1d1f2;
  for (const auto &patch : ROAD_PATCHES) {
    std::memcpy(image + patch.context_rva, patch.expected.data(),
                patch.expected.size());
  }
  std::memcpy(image + 0x4aeff54, KINGS_ROAD_CHECK.data(),
              KINGS_ROAD_CHECK.size());
  expect(validate_image(image),
         "supported image and all original sites accepted");
  expect(!validate_image(nullptr), "null image rejected");
  --nt->FileHeader.TimeDateStamp;
  expect(!validate_image(image), "wrong timestamp rejected");
  ++nt->FileHeader.TimeDateStamp;
  --nt->OptionalHeader.CheckSum;
  expect(!validate_image(image), "wrong checksum rejected");
  ++nt->OptionalHeader.CheckSum;
  --nt->OptionalHeader.SizeOfImage;
  expect(!validate_image(image), "wrong image size rejected");
  ++nt->OptionalHeader.SizeOfImage;
  image[0x4aeff54] ^= 1;
  expect(!validate_image(image), "modified King's Road guard rejected");
  image[0x4aeff54] ^= 1;
  nt->FileHeader.TimeDateStamp = 0x93e60b7f;
  nt->OptionalHeader.SizeOfImage = 0x09de5000;
  nt->OptionalHeader.CheckSum = 0x09a195c1;
  expect(!validate_image(image), "old 0.8.110 fingerprint rejected");
  nt->FileHeader.TimeDateStamp = 0xb47dcd3f;
  nt->OptionalHeader.SizeOfImage = 0x09deb000;
  nt->OptionalHeader.CheckSum = 0x09a1d1f2;
  for (const auto &patch : ROAD_PATCHES) {
    image[patch.context_rva] ^= 1;
    expect(!validate_image(image), "modified ownership context rejected");
    image[patch.context_rva] ^= 1;
  }

  // Execute the exact original ownership comparisons and branch encodings on
  // Windows x64. The branch destinations return false; fall-through returns
  // true. RCX is region, RDX is the acting player. Preserve all borrowed ABI
  // registers.
  const std::array<std::uint8_t, 14> setup{0x53, 0x57, 0x41, 0x56, 0x48,
                                           0x89, 0xcb, 0x48, 0x89, 0xd7,
                                           0x49, 0x89, 0xce, 0x90};
  const std::array<std::uint8_t, 10> accept{0xb8, 1,    0,    0,    0,
                                            0x41, 0x5e, 0x5f, 0x5b, 0xc3};
  const std::array<std::uint8_t, 10> reject{0xb8, 0,    0,    0,    0,
                                            0x41, 0x5e, 0x5f, 0x5b, 0xc3};
  std::array<std::uint8_t, 0x800> region{};
  std::array<std::uint8_t, 0x800> road{};
  void *player = image + 0x200;
  using RunGate = int (*)(void *, void *);
  const auto run = reinterpret_cast<RunGate>(code);
  for (const auto &patch : ROAD_PATCHES) {
    std::memset(code, 0xcc, 0x1000);
    std::memcpy(code, setup.data(), setup.size());
    // Common register setup: RAX=region, RBX=player, RDI=player, R14=region.
    const std::array<std::uint8_t, 6> registers{0x48, 0x89, 0xc8,
                                                0x48, 0x89, 0xd3};
    std::memcpy(code + setup.size(), registers.data(), registers.size());
    const auto start = setup.size() + registers.size();
    const auto *original = patch.expected.data() + patch.branch_offset - 7;
    const auto length = 7 + patch.branch_length;
    std::memcpy(code + start, original, length);
    int displacement = static_cast<std::int8_t>(original[8]);
    if (patch.branch_length == 6) {
      std::memcpy(&displacement, original + 9, 4);
    }
    std::memcpy(code + start + length, accept.data(), accept.size());
    std::memcpy(code + start + length + displacement, reject.data(),
                reject.size());
    std::memcpy(region.data() + 0x350, &player, sizeof(player));
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    expect(run(region.data(), player) == 1, "original owner admitted");
    void *other = image + 0x300;
    std::memcpy(region.data() + 0x350, &other, sizeof(other));
    expect(run(region.data(), player) == 0, "original foreign owner rejected");
    const auto road_offset = patch.context_rva == 0x4aeff73 ? 0x818 : 0x778;
    void *road_pointer = road.data();
    std::memcpy(static_cast<std::uint8_t *>(player) + road_offset,
                &road_pointer, sizeof(road_pointer));
    road[0x2b4] = 0;
    road[0x2b5] = 0;
    expect(make_stub(code + 0x400, patch, code + start + 7),
           "production road-only guard created");
    std::array<std::uint8_t, 7> jump{0xe9, 0, 0, 0, 0, 0x90, 0x90};
    const auto delta = static_cast<std::int32_t>(0x400 - start - 5);
    std::memcpy(jump.data() + 1, &delta, sizeof(delta));
    expect(write_code(code + start, jump), "production guard jump installed");
    expect(run(region.data(), player) == 1, "patched foreign owner admitted");
    std::memcpy(region.data() + 0x350, &player, sizeof(player));
    expect(run(region.data(), player) == 1,
           "patched original owner still admitted");
    for (std::uint8_t type = 1; type <= 4; ++type) {
      road[0x2b4] = type;
      expect(run(region.data(), player) == 1,
             "original owner non-road permissions preserved");
      std::memcpy(region.data() + 0x350, &other, sizeof(other));
      const bool is_preview = type == 1 && patch.context_rva != 0x4aeff73;
      expect(run(region.data(), player) == is_preview,
             "foreign non-road type rejected except construction preview");
      std::memcpy(region.data() + 0x350, &player, sizeof(player));
    }
    road[0x2b4] = 0;
    road[0x2b5] = 1;
    std::memcpy(region.data() + 0x350, &other, sizeof(other));
    expect(run(region.data(), player) == 0, "foreign wall subtype rejected");
    road[0x2b5] = 0;
    road_pointer = nullptr;
    std::memcpy(static_cast<std::uint8_t *>(player) + road_offset,
                &road_pointer, sizeof(road_pointer));
    expect(run(region.data(), player) == 0, "missing road pointer denied");
    expect(write_code(code + start,
                      patch.expected.subspan(patch.branch_offset - 7, 7)),
           "original comparison restored");
    std::memcpy(region.data() + 0x350, &other, sizeof(other));
    expect(run(region.data(), player) == 0, "restored foreign owner rejected");
  }
  expect(validate_image(image), "all validation fixtures remain unchanged");
  VirtualFree(code, 0, MEM_RELEASE);
  VirtualFree(image, 0, MEM_RELEASE);
  std::printf("%d passed; %d failed\n", passed, failed);
  return failed != 0;
}

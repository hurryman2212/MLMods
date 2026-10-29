#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <Windows.h>

#include "EraserHooks.hpp"
#include "EraserRuntime.hpp"

auto run_eraser_profile_tests(const char *path) -> int {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(stream),
                                       {}};
  if (file.size() < sizeof(IMAGE_DOS_HEADER)) {
    std::fprintf(stderr, "eraser profile: cannot read game executable\n");
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
      nt.OptionalHeader.SizeOfImage != 0x09deb000 ||
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
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "eraser profile: %s\n", message);
    }
  };
  expect(eraser_validate_image(image.data()),
         "current 0.8.111 executable accepted");
  expect(!eraser_validate_image(nullptr), "null image rejected");
  expect(!eraser_validate_image(reinterpret_cast<const std::uint8_t *>(1)),
         "unreadable image rejected");
  const auto fingerprints = eraser_fingerprints();
  expect(!fingerprints.empty(),
         "native profile contains validation fingerprints");
  for (const auto &fingerprint : fingerprints) {
    const bool bounded =
        fingerprint.rva < image.size() &&
        fingerprint.bytes.size() <= image.size() - fingerprint.rva;
    expect(bounded && !fingerprint.bytes.empty(),
           "fingerprint within mapped image");
    if (!bounded)
      continue;
    expect(std::memcmp(image.data() + fingerprint.rva, fingerprint.bytes.data(),
                       fingerprint.bytes.size()) == 0,
           "profile bytes match current native binary exactly");
    for (std::size_t i{}; i < fingerprint.bytes.size(); ++i) {
      image[fingerprint.rva + i] ^= 1;
      expect(!eraser_validate_image(image.data()),
             "every fingerprint byte mutation rejects native profile");
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
    expect(!eraser_validate_image(image.data()),
           "wrong executable identity rejected");
    image[offset] ^= 1;
  }
  const auto previous_image = eraser_image;
  eraser_image = 0;
  expect(eraser_make_patches().empty(),
         "unconfigured native base creates no hooks");
  eraser_image = reinterpret_cast<std::uintptr_t>(image.data());
  const auto patches = eraser_make_patches();
  expect(patches.size() == 2,
         "exactly two EraseShrubbery midpoint sites are patched");
  constexpr std::array<std::uintptr_t, 2> sites{0x4b16b66, 0x4b16e57};
  const std::array<std::vector<std::uint8_t>, 2> prefixes{
      std::vector<std::uint8_t>{0x0f, 0x84, 0xf0, 0x02, 0x00, 0x00},
      std::vector<std::uint8_t>{0x44, 0x8b, 0x44, 0x24, 0x4c}};
  std::size_t stub_bytes{};
  for (std::size_t i{}; i < patches.size(); ++i) {
    const auto &patch = patches[i];
    expect(i < sites.size() && patch.rva == sites[i],
           "patch address is the audited EraseShrubbery midpoint");
    expect(i < prefixes.size() && patch.expected == prefixes[i],
           "displaced native prefix consists of one complete instruction");
    const bool bounded = patch.rva < image.size() &&
                         patch.expected.size() <= image.size() - patch.rva;
    expect(bounded, "patch wholly inside mapped image");
    if (bounded)
      expect(std::memcmp(image.data() + patch.rva, patch.expected.data(),
                         patch.expected.size()) == 0,
             "midpoint original prefix matches installed executable");
    expect(!patch.call && patch.trampoline == nullptr,
           "midpoint bridge uses a jump without a global-function trampoline");
    expect(!patch.stub.empty(), "midpoint has a complete executable bridge");
    stub_bytes += (patch.stub.size() + 15) & ~std::size_t{15};
  }
  expect(stub_bytes <= 0x1000, "both bridges fit one executable stub page");
  expect(eraser_validate_image(image.data()),
         "profile unchanged by patch creation");
  eraser_image = previous_image;
  std::printf(
      "eraser profile: %d checks, %d failures; %zu hooks, %zu stub bytes\n",
      checks, failures, patches.size(), stub_bytes);
  return failures;
}

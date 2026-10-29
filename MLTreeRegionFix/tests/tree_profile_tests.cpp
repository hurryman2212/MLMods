#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <Windows.h>

#include "TreeHooks.hpp"

auto run_tree_profile_tests(const char *path) -> int {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(stream),
                                       {}};
  if (file.size() < sizeof(IMAGE_DOS_HEADER))
    return 1;
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
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "tree profile: %s\n", message);
    }
  };
  expect(tree_validate_image(image.data()),
         "current Steam 0.8.117 build 26185246 executable accepted");
  expect(!tree_validate_image(nullptr), "null image rejected");
  const auto fingerprints = tree_fingerprints();
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
      expect(!tree_validate_image(image.data()),
             "every fingerprint byte mutation rejects native profile");
      image[fingerprint.rva + i] ^= 1;
    }
  }
  // Mutate independent identity fields as well as native instruction bytes.
  for (std::size_t offset :
       {std::size_t{0}, static_cast<std::size_t>(dos.e_lfanew),
        static_cast<std::size_t>(dos.e_lfanew) + 4,
        static_cast<std::size_t>(dos.e_lfanew) + 8,
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, CheckSum)}) {
    image[offset] ^= 1;
    expect(!tree_validate_image(image.data()),
           "wrong executable identity rejected");
    image[offset] ^= 1;
  }

  auto old_header = nt;
  old_header.FileHeader.TimeDateStamp = 0xb47dcd3f;
  old_header.OptionalHeader.SizeOfImage = 0x09deb000;
  old_header.OptionalHeader.CheckSum = 0x09a1d1f2;
  std::memcpy(image.data() + dos.e_lfanew, &old_header, sizeof(old_header));
  expect(!tree_validate_image(image.data()),
         "previous 0.8.111 executable identity is no longer supported");
  std::memcpy(image.data() + dos.e_lfanew, &nt, sizeof(nt));

  const auto previous = tree_image;
  tree_image = 0;
  expect(tree_make_patches().empty(),
         "unconfigured native base creates no hooks");
  tree_image = reinterpret_cast<std::uintptr_t>(image.data());
  const auto patches = tree_make_patches();
  expect(patches.size() == 2, "exactly two planting-region sites are patched");
  constexpr std::array<std::uintptr_t, 2> sites{0x4dc8cde, 0x4dc8e4d};
  const std::array<std::vector<std::uint8_t>, 2> prefixes{
      std::vector<std::uint8_t>{0x49, 0x8b, 0xbe, 0x28, 0x03, 0, 0},
      std::vector<std::uint8_t>{0x49, 0x8b, 0x9e, 0x28, 0x03, 0, 0}};
  std::size_t stub_bytes{};
  for (std::size_t i{}; i < patches.size(); ++i) {
    const auto &patch = patches[i];
    expect(i < sites.size() && patch.rva == sites[i],
           "patch address is the audited PlantTree midpoint");
    expect(i < prefixes.size() && patch.expected == prefixes[i],
           "seven-byte native prefix is one complete displaced MOV");
    const bool bounded = patch.rva < image.size() &&
                         patch.expected.size() <= image.size() - patch.rva;
    expect(bounded, "patch wholly inside mapped image");
    if (bounded)
      expect(std::memcmp(image.data() + patch.rva, patch.expected.data(),
                         patch.expected.size()) == 0,
             "both midpoint original prefixes match installed executable");
    expect(!patch.call && patch.trampoline == nullptr,
           "midpoint bridge uses jump without global-function trampoline");
    std::uintptr_t continuation{};
    if (patch.stub.size() >= 14)
      std::memcpy(&continuation, patch.stub.data() + patch.stub.size() - 8,
                  sizeof(continuation));
    expect(patch.stub.size() >= 14 &&
               std::memcmp(patch.stub.data() + patch.stub.size() - 14,
                           "\xff\x25\0\0\0\0", 6) == 0 &&
               continuation == tree_image + patch.rva + patch.expected.size(),
           "bridge resumes immediately after the full displaced instruction");
    stub_bytes += (patch.stub.size() + 15) & ~std::size_t{15};
  }
  expect(stub_bytes <= 0x1000, "two bridges fit one executable stub page");
  expect(tree_validate_image(image.data()),
         "profile remains unchanged by patch creation");
  tree_image = previous;
  std::printf(
      "tree profile: %d checks, %d failures; %zu hooks, %zu stub bytes\n",
      checks, failures, patches.size(), stub_bytes);
  return failures;
}

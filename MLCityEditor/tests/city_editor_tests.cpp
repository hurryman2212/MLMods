#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include <Windows.h>

#include "CityHooks.hpp"
#include "CityRuntime.hpp"

int run_construction_tests();
int run_demolition_tests();
int run_upgrade_tests();
int run_patch_tests();
int run_bridge_tests();
int run_retinue_tests();
int run_eligibility_tests();
int run_maturity_tests();
int run_windmill_tests();
int run_extension_cancel_tests();

auto main(int argc, char **argv) -> int {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: city_editor_tests GAME_EXE\n");
    return 2;
  }
  std::ifstream stream(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(stream),
                                       {}};
  if (file.size() < sizeof(IMAGE_DOS_HEADER))
    return 2;
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, file.data(), sizeof(dos));
  if (dos.e_lfanew < 0 ||
      static_cast<std::size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
          file.size())
    return 2;
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
  if (nt.OptionalHeader.SizeOfImage != 0x09dfd000)
    return 2;
  std::vector<std::uint8_t> image(nt.OptionalHeader.SizeOfImage);
  if (nt.OptionalHeader.SizeOfHeaders > file.size())
    return 2;
  std::memcpy(image.data(), file.data(), nt.OptionalHeader.SizeOfHeaders);
  const auto sections = static_cast<std::size_t>(dos.e_lfanew) +
                        sizeof(std::uint32_t) + sizeof(IMAGE_FILE_HEADER) +
                        nt.FileHeader.SizeOfOptionalHeader;
  for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER section{};
    if (sections + (i + 1) * sizeof(section) > file.size())
      return 2;
    std::memcpy(&section, file.data() + sections + i * sizeof(section),
                sizeof(section));
    if (static_cast<std::size_t>(section.PointerToRawData) +
                section.SizeOfRawData >
            file.size() ||
        static_cast<std::size_t>(section.VirtualAddress) +
                section.SizeOfRawData >
            image.size())
      return 2;
    std::memcpy(image.data() + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }
  int failures{}, checks{};
  const auto expect = [&](bool success, const char *label) {
    ++checks;
    if (!success) {
      ++failures;
      std::printf("FAIL profile %s\n", label);
    }
  };
  expect(city_validate_image(image.data(), image.size()),
         "supported 0.8.117 image");
  expect(!city_validate_image(nullptr, image.size()), "null image rejected");
  expect(!city_validate_image(image.data(), image.size() - 1),
         "wrong image extent rejected");
  for (const auto &check : city_fingerprints()) {
    for (std::size_t i = 0; i < check.bytes.size() / 2; ++i) {
      image[check.rva + i] ^= 1;
      expect(!city_validate_image(image.data(), image.size()),
             "every fingerprint byte is required");
      image[check.rva + i] ^= 1;
    }
  }
  const auto header_offset = static_cast<std::size_t>(dos.e_lfanew);
  auto legacy = nt;
  legacy.FileHeader.TimeDateStamp = 0xb47dcd3f;
  legacy.OptionalHeader.SizeOfImage = 0x09deb000;
  legacy.OptionalHeader.CheckSum = 0x09a1d1f2;
  std::memcpy(image.data() + header_offset, &legacy, sizeof(legacy));
  expect(!city_validate_image(image.data(), image.size()),
         "old 0.8.111 executable identity is rejected");
  std::memcpy(image.data() + header_offset, &nt, sizeof(nt));
  // The other installed mods may load first. Their audited patch bytes must
  // not overlap CityEditor's version evidence or make it depend on load order.
  const std::pair<std::size_t, std::size_t> coinstalled_sites[]{
      {0x4b68dfa, 13}, {0x4b687ad, 5},  {0x4b68806, 5}, {0x4b68cf0, 5},
      {0x4b68d59, 5},  {0x4b68db8, 5},  {0x4b6a194, 5}, {0x4b69032, 5},
      {0x4b90930, 5},  {0x4b90b12, 5},  {0x4b90bff, 5}, {0x4b909ad, 5},
      {0x4d56619, 5},  {0x4c2e9d0, 5},  {0x4d1ca47, 5}, {0x4cff83f, 5},
      {0x4c84b94, 7},  {0x4b69eb9, 5},  {0x4abb29e, 5}, {0x4be9206, 5},
      {0x4c99952, 7},  {0x4d0ca5b, 9},  {0x4cbc9ba, 5}, {0x4d2eb31, 8},
      {0x4d09e0b, 7},  {0x4cf9825, 5},  {0x4d0460f, 5}, {0x4d05b1f, 5},
      {0x4b353c5, 5},  {0x4c9301e, 14}, {0x4c931c7, 9}, {0x4c9340a, 7},
      {0x4d038e1, 5},  {0x4d1ecfe, 5},  {0x4d07433, 7}, {0x4b35394, 5},
      {0x4b4c7c5, 5},  {0x4b4cc2e, 5},  {0x4b785b0, 5}, {0x4b9d3a1, 7},
      {0x4b9d86e, 7},  {0x4bc79dd, 7},  {0x4bc7f39, 7}, {0x4b7863c, 5},
      {0x4b78651, 5},  {0x4cd5b89, 5},  {0x4b583c0, 5}, {0x4b91fd2, 5},
      {0x4b922a4, 5},  {0x4b92454, 5},  {0x4b925f6, 5}, {0x4b93c32, 5},
      {0x4b8b01f, 5},  {0x4b8b1b5, 5},  {0x4b8b598, 5}, {0x4b8b714, 5},
      {0x4b8ab74, 5},  {0x4dc8cde, 7},  {0x4dc8e4d, 7},
  };
  std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> originals;
  for (const auto &[site, length] : coinstalled_sites) {
    originals.emplace_back(
        site, std::vector<std::uint8_t>(image.begin() + site,
                                        image.begin() + site + length));
    std::memset(image.data() + site, 0xcc, length);
    expect(city_validate_image(image.data(), image.size()),
           "other mod patch sites are independent of CityEditor validation");
  }
  for (const auto &[site, bytes] : originals)
    std::memcpy(image.data() + site, bytes.data(), bytes.size());
  expect(city_validate_image(image.data(), image.size()),
         "original detached image restored after co-install validation");
  const auto saved_image = city_image;
  city_image = reinterpret_cast<std::uintptr_t>(image.data());
  const auto patches = city_make_patches();
  expect(patches.size() == 23, "all 23 targeted hooks present");
  std::size_t stub_size{}, entries{};
  for (const auto &patch : patches) {
    expect(patch.rva + patch.expected.size() <= image.size(), "patch in image");
    expect(std::memcmp(image.data() + patch.rva, patch.expected.data(),
                       patch.expected.size()) == 0,
           "patch original bytes match");
    stub_size += (patch.stub.size() + 15) & ~std::size_t{15};
    if (patch.trampoline) {
      ++entries;
      expect(patch.trampoline_offset + patch.expected.size() + 14 ==
                 patch.stub.size(),
             "entry has complete relocation-free replay plus tail jump");
      expect(std::memcmp(patch.stub.data() + patch.trampoline_offset,
                         patch.expected.data(), patch.expected.size()) == 0,
             "entry replay equals original instructions");
    }
  }
  expect(entries == 14, "fourteen original entry trampolines");
  expect(stub_size <= 0x1000, "all bridges fit validated RX stub page");
  city_image = saved_image;
  std::printf("profile: %d checks, %d failures; %zu hooks, %zu stub bytes\n",
              checks, failures, patches.size(), stub_size);
  failures += run_construction_tests();
  failures += run_demolition_tests();
  failures += run_upgrade_tests();
  failures += run_patch_tests();
  failures += run_bridge_tests();
  failures += run_retinue_tests();
  failures += run_eligibility_tests();
  failures += run_maturity_tests();
  failures += run_windmill_tests();
  failures += run_extension_cancel_tests();
  std::printf("TOTAL FAILURES: %d\n", failures);
  return failures == 0 ? 0 : 1;
}

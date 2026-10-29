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

#include "ManorLoadNative.hpp"

static auto native_profile_tests(const char *path) -> int {
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
      std::fprintf(stderr, "profile: %s\n", message);
    }
  };
  expect(manor_load_validate_image(image.data()),
         "current executable accepted");
  expect(!manor_load_validate_image(nullptr), "null image rejected");
  expect(!manor_load_validate_image(reinterpret_cast<const std::uint8_t *>(1)),
         "unreadable image rejected");
  for (const auto &fingerprint : manor_load_fingerprints()) {
    expect(!fingerprint.bytes.empty(), "nonempty profile signature");
    for (std::size_t i{}; i < fingerprint.bytes.size(); ++i) {
      image[fingerprint.rva + i] ^= 1;
      expect(!manor_load_validate_image(image.data()),
             "any altered signature byte fails closed");
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
    expect(!manor_load_validate_image(image.data()),
           "wrong executable identity rejected");
    image[offset] ^= 1;
  }
  const auto callback =
      reinterpret_cast<const void *>(&manor_load_validate_image);
  const std::array<std::pair<std::uintptr_t, std::size_t>, 3> patches{
      {{0x4c9301e, 14}, {0x4c931c7, 9}, {0x4c9340a, 7}}};
  ManorLoadHook hook;
  expect(hook.restore(), "empty restore succeeds");
  expect(!hook.install(image.data(), nullptr, callback, callback),
         "null route callback rejected");
  expect(!hook.install(image.data(), callback, nullptr, callback),
         "null begin callback rejected");
  expect(!hook.install(image.data(), callback, callback, nullptr),
         "null end callback rejected");
  image[0x4c931c7] ^= 1;
  expect(!hook.install(image.data(), callback, callback, callback),
         "altered load site is not overwritten");
  image[0x4c931c7] ^= 1;
  const auto snapshot = image;
  const bool installed =
      hook.install(image.data(), callback, callback, callback);
  expect(installed && hook.installed(),
         "three load hooks install in detached image");
  if (installed) {
    expect(!hook.install(image.data(), callback, callback, callback),
           "double install rejected");
    auto expected = snapshot;
    for (const auto &[rva, size] : patches) {
      expect(image[rva] == 0xe9, "near jump installed at each audited site");
      std::memcpy(expected.data() + rva, image.data() + rva, size);
    }
    expect(image == expected,
           "only the three audited instruction ranges change");
    for (const auto &[rva, size] : patches) {
      (void)size;
      image[rva] = 0xcc;
      auto foreign = expected;
      foreign[rva] = 0xcc;
      expect(
          !hook.restore() && hook.installed() && image == foreign,
          "foreign owner on any site prevents partial loop-hook restoration");
      image[rva] = expected[rva];
    }
    expect(hook.restore() && !hook.installed(),
           "uninstall restores all three native instruction ranges");
    expect(image == snapshot && manor_load_validate_image(image.data()),
           "uninstall restores every byte");
    expect(hook.restore(), "repeat uninstall succeeds");
    expect(hook.install(image.data(), callback, callback, callback) &&
               hook.restore(),
           "reinstall and restore succeed");
  }
  std::printf("manor load profile/installer: %d checks, %d failures\n", checks,
              failures);
  return failures;
}

auto run_manor_load_bridge_tests() -> int;
auto run_manor_load_runtime_tests() -> int;

auto main(int argc, char **argv) -> int {
  int failures = run_manor_load_runtime_tests() + run_manor_load_bridge_tests();
  if (argc != 2) {
    std::fprintf(stderr,
                 "Usage: manor_load_tests <game executable, read-only>\n");
    return 1;
  }
  failures += native_profile_tests(argv[1]);
  return failures == 0 ? 0 : 1;
}

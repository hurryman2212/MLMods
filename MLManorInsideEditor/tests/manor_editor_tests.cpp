#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/MLManorInsideEditor.cpp"

// External test groups are separate translation units.
int run_geometry_tests();
int run_stub_tests();

static auto image_digest(const std::uint8_t *bytes, std::size_t size)
    -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

int main(int argc, char **argv) {
  std::size_t checks{};
  const auto require = [&checks](bool condition,
                                 const std::string &description) {
    ++checks;
    if (!condition) {
      throw std::runtime_error(description);
    }
  };
  try {
    require(argc == 2, "Pass the read-only game EXE path as argument one");
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    require(input.good(), "Open current game EXE for reading");
    const auto length = input.tellg();
    require(length > 0 && length < 1024ll * 1024 * 1024,
            "Bound game EXE input size");
    std::vector<std::uint8_t> disk(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(disk.data()), length);
    require(input.good(), "Read whole game EXE without modifying it");
    require(disk.size() >= sizeof(IMAGE_DOS_HEADER), "DOS header exists");
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(disk.data());
    require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
                static_cast<std::size_t>(dos->e_lfanew) +
                        sizeof(IMAGE_NT_HEADERS64) <=
                    disk.size(),
            "Bound NT header location");
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        disk.data() + dos->e_lfanew);
    require(nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->OptionalHeader.SizeOfImage > 0 &&
                nt->OptionalHeader.SizeOfImage < 1024u * 1024u * 1024u,
            "Bound local image allocation");
    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    auto release = [](std::uint8_t *address) {
      if (address != nullptr) {
        VirtualFree(address, 0, MEM_RELEASE);
      }
    };
    std::unique_ptr<std::uint8_t, decltype(release)> allocation(
        static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)),
        release);
    require(allocation != nullptr, "Allocate isolated image in test process");
    auto *image = allocation.get();
    require(nt->OptionalHeader.SizeOfHeaders <= disk.size() &&
                nt->OptionalHeader.SizeOfHeaders <= image_size,
            "Bound image headers");
    std::memcpy(image, disk.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    const auto section_offset =
        reinterpret_cast<const std::uint8_t *>(sections) - disk.data();
    require(section_offset + nt->FileHeader.NumberOfSections *
                                 sizeof(IMAGE_SECTION_HEADER) <=
                disk.size(),
            "Bound section table");
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
      const auto &section = sections[index];
      require(static_cast<std::size_t>(section.PointerToRawData) +
                          section.SizeOfRawData <=
                      disk.size() &&
                  static_cast<std::size_t>(section.VirtualAddress) +
                          section.SizeOfRawData <=
                      image_size,
              "Bound section payload");
      std::memcpy(image + section.VirtualAddress,
                  disk.data() + section.PointerToRawData,
                  section.SizeOfRawData);
    }
    const auto patches = make_patches(image);
    require(patches.size() == 22,
            "Manor hook plan owns exactly 22 building and plot sites");
    constexpr std::array<std::uintptr_t, 20> road_sites{
        0x4b68dfa, 0x4b687ad, 0x4b68806, 0x4b68cf0, 0x4b68d59,
        0x4b68db8, 0x4b6a194, 0x4d56619, 0x4c2e9d0, 0x4d1ca47,
        0x4cff83f, 0x4c84b94, 0x4b69eb9, 0x4abb29e, 0x4be9206,
        0x4b69032, 0x4b90930, 0x4b90b12, 0x4b90bff, 0x4b909ad};
    for (const auto &patch : patches) {
      require(std::find(road_sites.begin(), road_sites.end(), patch.rva) ==
                  road_sites.end(),
              "Manor hook plan excludes every RoadEditor hook site");
    }
    require(validate_image(image, patches), "Exact installed EXE matches plan");
    for (const auto site : road_sites)
      image[site] ^= 1;
    require(
        validate_image(image, patches),
        "RoadEditor hook modifications do not invalidate Manor fingerprints");
    for (const auto site : road_sites)
      image[site] ^= 1;
    require(!validate_image(nullptr, patches), "Null image rejected");
    auto *local_nt =
        reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    const auto timestamp = local_nt->FileHeader.TimeDateStamp;
    local_nt->FileHeader.TimeDateStamp ^= 1;
    require(!validate_image(image, patches), "Wrong game timestamp rejected");
    local_nt->FileHeader.TimeDateStamp = timestamp;
    for (const auto &patch : patches) {
      require(std::memcmp(image + patch.rva, patch.expected.data(),
                          patch.expected.size()) == 0,
              "Hook expected bytes match read-only executable");
      image[patch.rva] ^= 1;
      require(!validate_image(image, patches),
              "Each individually changed hook site rejected");
      image[patch.rva] ^= 1;
    }
    require(validate_image(image, patches), "Restored local image revalidates");
    image[0x4b8ab70] ^= 1;
    require(!validate_image(image, patches),
            "Modified snap batch work-pointer adjustment rejected");
    image[0x4b8ab70] ^= 1;
    require(!snap_graph_profile_matches(nullptr),
            "Null snap graph context image rejected");
    for (const auto &[rva, size] :
         std::array<std::pair<std::uintptr_t, std::size_t>, 7>{
             {{0x4b8abb7, 22},
              {0x4b8adb1, 7},
              {0x4b8ae30, 15},
              {0x4b8b38a, 7},
              {0x4b8b3c7, 7},
              {0x4b8b429, 16},
              {0x4b8b5aa, 16}}}) {
      for (std::size_t i{}; i < size; ++i) {
        image[rva + i] ^= 1;
        require(!validate_image(image, patches),
                "Changed snap graph endpoint register context rejected");
        image[rva + i] ^= 1;
      }
    }
    std::vector<std::uint8_t> expected_image(image, image + image_size);
    const auto pristine_hash = image_digest(image, image_size);
    PatchSet installed;
    require(installed.install(image, patches),
            "Install hook bytes into isolated test image only");
    require(installed.installed(), "Installed state reported");
    require(!installed.install(image, patches), "Duplicate install rejected");
    for (const auto &patch : patches) {
      require(image[patch.rva] == (patch.call ? 0xe8 : 0xe9),
              "Hook uses intended CALL or JMP opcode");
      for (std::size_t index = 5; index < patch.expected.size(); ++index) {
        require(image[patch.rva + index] == 0x90,
                "Remaining displaced bytes padded with NOP");
      }
      std::memcpy(expected_image.data() + patch.rva, image + patch.rva,
                  patch.expected.size());
    }
    require(image_digest(image, image_size) ==
                image_digest(expected_image.data(), expected_image.size()),
            "No image bytes outside whitelisted hook sites changed");
    require(installed.restore(), "Restore every isolated-image hook");
    require(!installed.installed(), "Restored state reported");
    require(image_digest(image, image_size) == pristine_hash,
            "Entire isolated image byte-identical after restoration");
    require(validate_image(image, patches), "Restored executable revalidates");
    require(installed.restore(), "Repeated restoration safely succeeds");
    require(run_geometry_tests() == 0, "Geometry test group passes");
    require(run_stub_tests() == 0, "Machine-code bridge test group passes");
    std::printf("Image validation and restoration: %zu checks passed; %zu "
                "hook sites verified. Game files/process untouched.\n",
                checks, patches.size());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAILED after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}

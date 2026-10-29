#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/MLRoadEditor.cpp"

// External test groups are separate translation units.
int run_geometry_tests();
int run_stub_tests();
int run_connection_bridge_tests(std::uint8_t *image);

static void *finish_test_pawn{};
static int finish_test_calls{};
static void *finish_test_native(void *pawn) {
  finish_test_pawn = pawn;
  ++finish_test_calls;
  return reinterpret_cast<void *>(0x1234567887654321ULL);
}

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
    require(!patches.empty(), "Generated hook plan is nonempty");
    require(patches.size() == 20,
            "Only the road editor's twenty hook sites are owned");
    const std::array<std::uintptr_t, 22> manor_sites{
        0x4b35394, 0x4b4c7c5, 0x4b4cc2e, 0x4b785b0, 0x4b9d3a1, 0x4b9d86e,
        0x4bc79dd, 0x4bc7f39, 0x4b7863c, 0x4b78651, 0x4cd5b89, 0x4b583c0,
        0x4b91fd2, 0x4b922a4, 0x4b92454, 0x4b925f6, 0x4b93c32, 0x4b8b01f,
        0x4b8b1b5, 0x4b8b598, 0x4b8b714, 0x4b8ab74};
    for (const auto &patch : patches)
      for (const auto site : manor_sites)
        require(patch.rva != site,
                "Road and Manor editors never own the same hook");
    for (const auto site : manor_sites)
      image[site] ^= 1;
    for (const auto &[site, size] :
         std::array<std::pair<std::uintptr_t, std::size_t>, 2>{
             {{0x4b68be6, 3}, {0x4b69e53, 9}}}) {
      for (std::size_t i{}; i < size; ++i) {
        image[site + i] ^= 1;
        require(!validate_image(image, patches),
                "Native zero register and independent gate branch must match");
        image[site + i] ^= 1;
      }
    }
    require(
        validate_image(image, patches),
        "Manor hook modifications do not invalidate RoadEditor fingerprints");
    for (const auto site : manor_sites)
      image[site] ^= 1;
    require(validate_image(image, patches), "Exact installed EXE matches plan");
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
    for (const auto &[rva, size] :
         std::array<std::pair<std::uintptr_t, std::size_t>, 3>{
             {{0x4b69eab, 17}, {0x4cd6430, 21}, {0x4b69e68, 18}}}) {
      for (std::size_t i{}; i < size; ++i) {
        image[rva + i] ^= 1;
        require(!validate_image(image, patches),
                "Changed Castle preview context rejected");
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
    require(run_connection_bridge_tests(image) == 0,
            "Connection bridge and executable context tests pass");
    native_finish_road = &finish_test_native;
    void *finish_result = finish_castle_road(reinterpret_cast<void *>(0x13579));
    require(finish_test_calls == 1 &&
                finish_test_pawn == reinterpret_cast<void *>(0x13579),
            "Final road wrapper forwards pawn to native exactly once");
    require(finish_result == reinterpret_cast<void *>(0x1234567887654321ULL),
            "Final road wrapper preserves full 64-bit native road result");
    native_finish_road = nullptr;
    std::printf("Image validation and restoration: %zu checks passed; %zu "
                "hook sites verified. Game files/process untouched.\n",
                checks, patches.size());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAILED after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}

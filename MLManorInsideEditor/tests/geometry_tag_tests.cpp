#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "../src/Geometry.cpp"

int main() {
  unsigned checks{};
  const auto require = [&checks](bool condition, const char *message) {
    ++checks;
    if (!condition)
      throw std::runtime_error(message);
  };
  try {
    for (const auto &[tag, expected] :
         std::array<std::pair<std::wstring_view, int>, 12>{
             {{L"bType_98", 98},
              {L"bType_57", 57},
              {L"bType_472", 472},
              {L"BTYPE_98", 98},
              {L"btype_57", 57},
              {L"bTypeX472", 472},
              {L"bType_ +98tail", 98},
              {L"bType_-1", 0},
              {L"bType_0", 0},
              {L"xbType_98", 0},
              {L"bType_", 0},
              {L"unrelated", 0}}}) {
      require(module_type_from_tag(tag) == expected,
              "native contains/RightChop(6)/positive _wtoi semantics");
    }
    auto *image = static_cast<std::byte *>(
        VirtualAlloc(nullptr, 0x9200000, MEM_RESERVE, PAGE_NOACCESS));
    require(image != nullptr, "reserve isolated fake image");
    require(VirtualAlloc(image + 0x91d4000, 0x1000, MEM_COMMIT,
                         PAGE_READWRITE) != nullptr,
            "commit only fake NamePool metadata page");
    std::array<std::byte, 0x300> actor{};
    std::array<std::byte, 512> block{};
    struct Name {
      std::uint32_t index, number;
    };
    std::array<Name, 2> names{{{0, 99}, {64, 0}}};
    NativeArray<Name> header{names.data(), 1, 2};
    std::memcpy(actor.data() + 0x1e0, &header, sizeof(header));
    std::uint32_t current_block = 0, cursor = 512;
    const auto block_pointer = reinterpret_cast<std::uintptr_t>(block.data());
    std::memcpy(image + 0x91d49c8, &current_block, 4);
    std::memcpy(image + 0x91d49cc, &cursor, 4);
    std::memcpy(image + 0x91d49d0, &block_pointer, 8);
    std::uint16_t entry_header = 5 << 6;
    std::memcpy(block.data(), &entry_header, 2);
    std::memcpy(block.data() + 2, "bType", 5);
    const auto image_address = reinterpret_cast<std::uintptr_t>(image);
    const auto actor_address = reinterpret_cast<std::uintptr_t>(actor.data());
    const auto snapshot = [&] {
      return snapshot_module_type(actor_address, image_address);
    };
    const auto original_actor = actor;
    const auto original_names = names;
    const auto original_block = block;
    require(snapshot() == 98,
            "numbered ANSI FName bType plus internal number99 decodes98");
    names[0].number = 58;
    require(snapshot() == 57, "numbered ANSI FName reproduces live module57");
    names[0].number = 473;
    require(snapshot() == 472, "numbered ANSI FName reproduces live module472");
    names[0].number = 0;
    entry_header = 9 << 6;
    std::memcpy(block.data(), &entry_header, 2);
    std::memcpy(block.data() + 2, "bType_472", 9);
    require(snapshot() == 472, "unnumbered full ANSI FName also decodes");
    entry_header = (5 << 6) | 1;
    std::memcpy(block.data(), &entry_header, 2);
    std::memcpy(block.data() + 2, L"bType", 10);
    names[0].number = 99;
    require(snapshot() == 98,
            "wide FName payload is copied into local UTF16 storage");
    names = original_names;
    block = original_block;
    std::uint16_t other_header = 9 << 6;
    std::memcpy(block.data() + 128, &other_header, 2);
    std::memcpy(block.data() + 130, "unrelated", 9);
    names = {{{64, 0}, {0, 99}}};
    header.count = 2;
    std::memcpy(actor.data() + 0x1e0, &header, sizeof(header));
    require(snapshot() == 98,
            "unrelated tags preceding the native module tag are skipped");
    names = {{{0, 58}, {0, 99}}};
    require(snapshot() == 57,
            "first positive module tag wins like native code");
    names = original_names;
    header.count = 1;
    std::memcpy(actor.data() + 0x1e0, &header, sizeof(header));
    block = original_block;
    current_block = 8192;
    std::memcpy(image + 0x91d49c8, &current_block, 4);
    require(snapshot() == 0, "invalid pool block count fails closed");
    current_block = 0;
    std::memcpy(image + 0x91d49c8, &current_block, 4);
    names[0].index = 0x10000;
    require(snapshot() == 0, "unpublished name block is never read");
    names = original_names;
    cursor = 3;
    std::memcpy(image + 0x91d49cc, &cursor, 4);
    require(snapshot() == 0, "entry beyond current block cursor fails closed");
    cursor = 512;
    std::memcpy(image + 0x91d49cc, &cursor, 4);
    entry_header = 0;
    std::memcpy(block.data(), &entry_header, 2);
    require(snapshot() == 0, "zero-length name entry fails closed");
    block = original_block;
    header.count = -1;
    std::memcpy(actor.data() + 0x1e0, &header, sizeof(header));
    require(snapshot() == 0, "negative actor tag count fails closed");
    header.count = maximum_actor_tag_count + 1;
    header.capacity = header.count;
    std::memcpy(actor.data() + 0x1e0, &header, sizeof(header));
    require(snapshot() == 0, "excessive actor tag count fails closed");
    header = {reinterpret_cast<Name *>(1), 1, 1};
    std::memcpy(actor.data() + 0x1e0, &header, sizeof(header));
    require(snapshot() == 0,
            "unreadable tag array fails closed without dereference");
    actor = original_actor;
    require(snapshot_module_type(0, image_address) == 0 &&
                snapshot_module_type(1, image_address) == 0,
            "null and unreadable actor cannot call a native tag getter");
    const std::uint32_t destroyed = 0x8000;
    std::memcpy(actor.data() + 8, &destroyed, 4);
    require(snapshot() == 0, "destroying actor cannot supply module geometry");
    actor = original_actor;
    require(snapshot() == 98 && actor == original_actor &&
                block == original_block &&
                std::memcmp(names.data(), original_names.data(),
                            sizeof(names)) == 0,
            "successful snapshot leaves actor, tags and immutable name bytes "
            "untouched");
    require(VirtualFree(image, 0, MEM_RELEASE) != 0,
            "release isolated NamePool fixture");
    std::printf("Geometry tag tests passed: %u\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Geometry tag test failed after %u: %s\n", checks,
                 error.what());
    return 1;
  }
}

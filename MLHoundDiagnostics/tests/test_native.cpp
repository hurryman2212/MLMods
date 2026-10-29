#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/MLHoundDiagnostics.cpp"

struct TestHeader {
  std::uintptr_t data{};
  std::int32_t count{}, capacity{};
};

template <typename T, std::size_t N>
static void put(std::array<std::byte, N> &buffer, std::size_t offset,
                const T &value) {
  if (offset + sizeof(value) > N)
    throw std::runtime_error("fixture write outside its own allocation");
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

struct HoundFixture {
  std::array<std::byte, 0xb20> hound{}, hunter{};
  std::array<std::byte, 0x500> camp{};
  std::array<std::byte, 0x400> region{};
  std::array<std::byte, 0xd8> hound_task{}, hunter_task{};
  std::array<std::int32_t, 12> inventory{330, 4, 0, 0, 0, 0, 9, 3, 0, 0, 0, 0};

  HoundFixture() {
    put(hound, 0xc, std::uint32_t{42});
    put(hound, 0x18, std::uint32_t{123});
    put(hound, 0x1c, std::uint32_t{456});
    put(hound, 0x20, std::uintptr_t{0xabcdef});
    put(hound, 0xab0, std::uint8_t{14});
    put(hound, 0xab1, std::uint8_t{14});
    put(hound, 0x338, reinterpret_cast<std::uintptr_t>(camp.data()));
    put(hound, 0xac8, reinterpret_cast<std::uintptr_t>(hunter.data()));
    put(hound, 0x2f0,
        TestHeader{reinterpret_cast<std::uintptr_t>(L"Hanns"), 6, 6});
    put(hunter, 0x2f0,
        TestHeader{reinterpret_cast<std::uintptr_t>(L"Peter"), 6, 6});
    put(hunter, 0x5a0, std::uint8_t{1});
    put(region, 0x2b8,
        TestHeader{reinterpret_cast<std::uintptr_t>(L"Kesseleck"), 10, 10});
    put(camp, 0x2c8, reinterpret_cast<std::uintptr_t>(region.data()));
    put(camp, 0x448,
        TestHeader{reinterpret_cast<std::uintptr_t>(inventory.data()), 2, 2});
    put(hound_task, 0, std::uint8_t{41});
    put(hound_task, 0xa8, reinterpret_cast<std::uintptr_t>(hunter.data()));
    put(hunter_task, 0, std::uint8_t{4});
    put(hunter_task, 0x90, std::int32_t{44});
    put(hound, 0x898,
        TestHeader{reinterpret_cast<std::uintptr_t>(hound_task.data()), 1, 1});
    put(hunter, 0x898,
        TestHeader{reinterpret_cast<std::uintptr_t>(hunter_task.data()), 1, 1});
  }
};

static HoundFixture *active_fixture{};
static int original_calls{};
static void *original_unit{}, *original_camp{};
static const void *original_task{};
static bool original_result{};
static DWORD original_incoming_error{};
static constexpr DWORD native_error = 0x1945;

static bool mock_validation(void *unit, void *camp) {
  ++original_calls;
  original_unit = unit;
  original_camp = camp;
  original_incoming_error = GetLastError();
  SetLastError(native_error);
  return original_result;
}

static void mock_add_task(void *unit, const void *task) {
  ++original_calls;
  original_unit = unit;
  original_task = task;
  original_incoming_error = GetLastError();
  if (active_fixture && unit == active_fixture->hound.data() && task) {
    std::memmove(active_fixture->hound_task.data(), task, 0xd8);
    put(active_fixture->hound, 0x8a0, std::int32_t{1});
  }
  SetLastError(native_error);
}

static void mock_finish_task(void *unit) {
  ++original_calls;
  original_unit = unit;
  original_incoming_error = GetLastError();
  if (active_fixture && unit == active_fixture->hound.data())
    put(active_fixture->hound, 0x8a0, std::int32_t{0});
  SetLastError(native_error);
}

int main(int argc, char **argv) {
  std::size_t checks{};
  const auto require = [&checks](bool condition, const char *message) {
    ++checks;
    if (!condition)
      throw std::runtime_error(message);
  };
  try {
    native_owner_validation = mock_validation;
    native_add_task = mock_add_task;
    native_finish_task = mock_finish_task;
    game_image = 0;
    HoundFixture fixture;
    active_fixture = &fixture;
    const auto hound_address =
        reinterpret_cast<std::uintptr_t>(fixture.hound.data());
    require(read_name(hound_address + 0x2f0) == L"Hanns",
            "unit displayName FString decoded from verified offset");
    require(read_name(0) == L"<unknown>" && read_name(1) == L"<unknown>",
            "null and unreadable string headers are safe");
    TestHeader text{};
    const auto text_address = reinterpret_cast<std::uintptr_t>(&text);
    require(read_name(text_address) == L"<unknown>",
            "null string data rejected");
    text = {reinterpret_cast<std::uintptr_t>(L""), 1, 1};
    require(read_name(text_address) == L"<unnamed>",
            "valid empty name handled");
    text = {reinterpret_cast<std::uintptr_t>(L"X"), -1, 2};
    require(read_name(text_address) == L"<unknown>", "negative string count");
    text.count = maximum_string_length + 1;
    text.capacity = text.count;
    require(read_name(text_address) == L"<unknown>", "excessive string count");
    text.count = 2;
    text.capacity = 1;
    require(read_name(text_address) == L"<unknown>",
            "string count exceeds capacity");
    text = {reinterpret_cast<std::uintptr_t>(L"XY"), 2, 3};
    require(read_name(text_address) == L"<unknown>",
            "missing terminal NUL rejected");
    const std::array<wchar_t, 7> controls{L'A',   L'\n', L'\0', L'B',
                                          0x2028, 0x7f,  L'\0'};
    text = {reinterpret_cast<std::uintptr_t>(controls.data()), 7, 7};
    require(read_name(text_address) == L"A??B??",
            "embedded NUL and line controls cannot forge extra log records");
    text = {1, 2, 2};
    require(read_name(text_address) == L"<unknown>",
            "unreadable string data rejected");
    auto *guard =
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    require(guard != nullptr, "allocate isolated unreadable fixture");
    text = {reinterpret_cast<std::uintptr_t>(guard), 2, 2};
    require(read_name(text_address) == L"<unknown>",
            "PAGE_NOACCESS string is safe");
    require(!capture_record(guard, DiagnosticEvent::State),
            "PAGE_NOACCESS unit is ignored safely");
    require(VirtualFree(guard, 0, MEM_RELEASE) != 0,
            "release unreadable fixture");
    require(read_task(0).type == 255, "unreadable task is unknown, not idle");
    const auto original_hound = fixture.hound;
    put(fixture.hound, 0x898, TestHeader{1, -1, 1});
    require(read_task(hound_address).type == 255,
            "negative task count rejected");
    put(fixture.hound, 0x898, TestHeader{1, 1, 0});
    require(read_task(hound_address).type == 255,
            "task count exceeds capacity");
    put(fixture.hound, 0x898, TestHeader{1, 1, 1});
    require(read_task(hound_address).type == 255,
            "unreadable task data rejected");
    put(fixture.hound, 0x898, TestHeader{});
    require(read_task(hound_address).type == 0,
            "empty task array is ordinary idle");
    fixture.hound = original_hound;
    const auto camp_before = fixture.camp;
    const auto region_before = fixture.region;
    const auto hunter_before = fixture.hunter;
    const auto inventory_before = fixture.inventory;
    histories.clear();
    SetLastError(0xfeed);
    auto record = capture_record(fixture.hound.data(), DiagnosticEvent::State);
    require(GetLastError() == 0xfeed, "snapshot preserves caller LastError");
    require(record && record->name == L"Hanns" && record->hunter == L"Peter" &&
                record->region == L"Kesseleck",
            "native-layout fixture names and region captured");
    require(record->hound_task.type == 41 && record->owner_task.type == 4 &&
                record->owner_task.resource == 44 &&
                record->close_to_final == 1,
            "follow and carcass-return task context captured");
    require(record->meat == 4 && record->pelts == 3 &&
                record->decision.bonuses == 0,
            "initial stock is observational, not a claimed bonus");
    require(fixture.hound == original_hound && fixture.camp == camp_before &&
                fixture.region == region_before &&
                fixture.hunter == hunter_before &&
                fixture.inventory == inventory_before,
            "snapshot does not mutate any fake game object");
    require(!capture_record(fixture.hound.data(), DiagnosticEvent::State),
            "identical production snapshot is rate limited");
    put(fixture.hound, 0x1c, std::uint32_t{457});
    require(capture_record(fixture.hound.data(), DiagnosticEvent::State)
                    .has_value() &&
                histories.size() == 2,
            "new FName instance gets a distinct diagnostic history");
    put(fixture.hound, 0x20, std::uintptr_t{0xfedcba});
    require(capture_record(fixture.hound.data(), DiagnosticEvent::State)
                    .has_value() &&
                histories.size() == 3,
            "different outer level gets a distinct diagnostic history");
    fixture.hound = original_hound;
    for (const std::uint32_t flags : {0x8000U, 0x10000U, 0x40000000U}) {
      put(fixture.hound, 8, flags);
      require(!capture_record(fixture.hound.data(), DiagnosticEvent::State),
              "destroying or garbage hound is ignored");
    }
    fixture.hound = original_hound;
    put(fixture.hound, 0xab1, std::uint8_t{13});
    const auto nonhound = fixture.hound;
    std::array<void *, 3> invalid_units{nullptr, reinterpret_cast<void *>(1),
                                        fixture.hound.data()};
    for (void *unit : invalid_units) {
      active_fixture = nullptr;
      histories.clear();
      original_calls = 0;
      original_result = false;
      SetLastError(0xcafe);
      require(!owner_validation_hook(unit, fixture.camp.data()) &&
                  original_calls == 1 && original_unit == unit &&
                  original_camp == fixture.camp.data() &&
                  original_incoming_error == 0xcafe &&
                  GetLastError() == native_error,
              "invalid/nonhound validation preserves false result, args, "
              "LastError and one call");
      original_result = true;
      require(owner_validation_hook(unit, fixture.camp.data()) &&
                  original_calls == 2,
              "invalid/nonhound validation also preserves true result");
      const auto borrowed = fixture.hound_task;
      follow_queued_hook(unit, fixture.hound_task.data());
      require(original_calls == 3 && original_unit == unit &&
                  original_task == fixture.hound_task.data() &&
                  borrowed == fixture.hound_task,
              "invalid/nonhound queue still invokes original once without "
              "changing borrowed task");
      bonus_committed_hook(unit);
      require(original_calls == 4 && original_unit == unit &&
                  GetLastError() == native_error,
              "invalid/nonhound bonus wrapper still finishes native task once");
      follow_stopped_hook(unit);
      require(original_calls == 5 && original_unit == unit &&
                  histories.empty() && fixture.hound == nonhound,
              "invalid/nonhound stop wrapper preserves native call, emits no "
              "hound event");
    }
    active_fixture = &fixture;
    fixture.hound = original_hound;
    histories.clear();
    original_calls = 0;
    original_result = true;
    require(
        owner_validation_hook(fixture.hound.data(), fixture.camp.data()) &&
            original_calls == 1 && GetLastError() == native_error,
        "valid hound status observation preserves original bool and LastError");
    fixture.inventory[1] += 1;
    fixture.inventory[7] += 1;
    owner_validation_hook(fixture.hound.data(), fixture.camp.data());
    require(histories.begin()->second.bonuses == 0,
            "stock +1/+1 alone does not count a bonus");
    const auto increased_stock = fixture.inventory;
    const auto before_bonus = fixture.hound;
    auto expected_after = before_bonus;
    put(expected_after, 0x8a0, std::int32_t{0});
    SetLastError(0xbaba);
    bonus_committed_hook(fixture.hound.data());
    require(
        original_calls == 3 && original_incoming_error == 0xbaba &&
            GetLastError() == native_error,
        "committed bonus wrapper preserves native LastError input and output");
    require(histories.begin()->second.bonuses == 1 &&
                fixture.hound == expected_after &&
                fixture.inventory == increased_stock &&
                fixture.camp == camp_before &&
                fixture.region == region_before &&
                fixture.hunter == hunter_before,
            "native bonus event is counted but only mock-native task clearing "
            "mutates fixture");
    fixture.hound = before_bonus;
    bonus_committed_hook(fixture.hound.data());
    require(
        histories.begin()->second.bonuses == 2 &&
            fixture.inventory == increased_stock,
        "two committed hook calls count twice without needing a stock delta");
    fixture.hound = before_bonus;
    histories.clear();
    original_calls = 0;
    const auto borrowed = fixture.hound_task;
    follow_queued_hook(fixture.hound.data(), fixture.hound_task.data());
    require(original_calls == 1 && fixture.hound_task == borrowed &&
                histories.begin()->second.events[1].seen,
            "actual native queue is observed after original returns");
    follow_stopped_hook(fixture.hound.data());
    require(original_calls == 2 && histories.begin()->second.events[2].seen &&
                histories.begin()->second.bonuses == 0 &&
                fixture.inventory == increased_stock,
            "ordinary follow termination is not recorded as a bonus");
    active_fixture = nullptr;

    require(argc == 2, "Pass read-only game EXE path as argument one");
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    require(input.good(), "open current game EXE only for reading");
    const auto length = input.tellg();
    require(length > 0 && length < 1024ll * 1024 * 1024,
            "bound disk image size");
    std::vector<std::uint8_t> disk(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(disk.data()), length);
    require(input.good() && disk.size() >= sizeof(IMAGE_DOS_HEADER),
            "read full image");
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(disk.data());
    require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
                static_cast<std::size_t>(dos->e_lfanew) +
                        sizeof(IMAGE_NT_HEADERS64) <=
                    disk.size(),
            "bound PE headers");
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        disk.data() + dos->e_lfanew);
    require(nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->OptionalHeader.SizeOfImage > 0 &&
                nt->OptionalHeader.SizeOfImage < 1024u * 1024u * 1024u,
            "bound isolated image size");
    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    const auto release = [](std::uint8_t *address) {
      if (address)
        VirtualFree(address, 0, MEM_RELEASE);
    };
    std::unique_ptr<std::uint8_t, decltype(release)> allocation(
        static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)),
        release);
    require(allocation != nullptr, "allocate isolated image in test process");
    auto *image = allocation.get();
    require(nt->OptionalHeader.SizeOfHeaders <= disk.size() &&
                nt->OptionalHeader.SizeOfHeaders <= image_size,
            "bound header copy");
    std::memcpy(image, disk.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    const auto section_offset =
        reinterpret_cast<const std::uint8_t *>(sections) - disk.data();
    require(section_offset + nt->FileHeader.NumberOfSections *
                                 sizeof(IMAGE_SECTION_HEADER) <=
                disk.size(),
            "bound section table");
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      const auto &section = sections[i];
      require(static_cast<std::size_t>(section.PointerToRawData) +
                          section.SizeOfRawData <=
                      disk.size() &&
                  static_cast<std::size_t>(section.VirtualAddress) +
                          section.SizeOfRawData <=
                      image_size,
              "bound section contents");
      std::memcpy(image + section.VirtualAddress,
                  disk.data() + section.PointerToRawData,
                  section.SizeOfRawData);
    }
    require(validate_image(image),
            "all four native call contexts match actual current EXE");
    require(!validate_image(nullptr), "null image rejected");
    auto *local_nt =
        reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    const auto timestamp = local_nt->FileHeader.TimeDateStamp;
    local_nt->FileHeader.TimeDateStamp ^= 1;
    require(!validate_image(image), "different game version fails closed");
    local_nt->FileHeader.TimeDateStamp = timestamp;
    for (const auto &site : hook_sites)
      for (std::size_t byte = 0; byte < site.context.size(); ++byte) {
        image[site.context_rva + byte] ^= 1;
        require(!validate_image(image),
                "each of 148 context bytes is validated");
        image[site.context_rva + byte] ^= 1;
      }
    require(validate_image(image), "restored contexts pass");
    const std::vector<std::uint8_t> pristine(image, image + image_size);
    auto expected = pristine;
    const auto patches = make_patches();
    require(patches.size() == 4,
            "exactly four observational call patches generated");
    PatchSet installed;
    require(installed.install(image, patches) && installed.installed(),
            "install only into isolated copy in test process");
    require(!installed.install(image, patches),
            "duplicate installation rejected");
    for (const auto &patch : patches) {
      require(patch.call && image[patch.rva] == 0xe8 &&
                  patch.expected.size() == 5,
              "replacement retains five-byte native CALL contract");
      std::memcpy(expected.data() + patch.rva, image + patch.rva,
                  patch.expected.size());
    }
    require(std::memcmp(image, expected.data(), image_size) == 0,
            "entire isolated image differs only at four whitelisted calls");
    require(installed.restore() && !installed.installed(),
            "restore all isolated hooks");
    require(std::memcmp(image, pristine.data(), image_size) == 0 &&
                validate_image(image),
            "entire image byte-exact after restoration");
    require(installed.restore(), "repeated restoration is harmless");
    std::printf("Production diagnostics/native checks passed: %zu\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAILED after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}

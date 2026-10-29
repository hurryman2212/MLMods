#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "../src/MLCrashFix.cpp"

static std::size_t checks{};
static void require(bool condition, const char *message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

template <typename T, std::size_t N>
static void put(std::array<std::uint8_t, N> &buffer, std::size_t offset,
                const T &value) {
  require(offset + sizeof(value) <= N, "fixture write bounds");
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

struct Fixture {
  alignas(16) std::array<std::uint8_t, 0xd08> unit{};
  alignas(16) std::array<std::uint8_t, 0x30> unit_class{};
  alignas(16) std::array<std::uintptr_t, 4> stack{};
  CONTEXT context{};
  EXCEPTION_RECORD record{};
  EXCEPTION_POINTERS exception{&record, &context};

  explicit Fixture(std::uint8_t *image) {
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    put(unit, 0, base + 0x1000);
    put(unit, 0xc, std::int32_t{42});
    put(unit, 0x10, reinterpret_cast<std::uintptr_t>(unit_class.data()));
    put(unit, 0x944, std::int32_t{157});
    put(unit_class, 0, base + 0x2000);
    stack[1] = base + BANNER_CALLER_RVA;
    // Different bytes expose accidental writes to any register/XMM/control
    // field.
    std::memset(&context, 0xa5, sizeof(context));
    context.Rip = base + BANNER_FAULT_RVA;
    context.Rsp = reinterpret_cast<std::uintptr_t>(&stack[1]);
    context.Rsi = reinterpret_cast<std::uintptr_t>(unit.data());
    context.Rcx = 0;
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = image + BANNER_FAULT_RVA;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;
    record.ExceptionInformation[1] = 0x12e8;
  }

  void check(bool accepted) {
    const auto before = context;
    const auto old_record = record;
    const auto old_unit = unit;
    const auto old_class = unit_class;
    const auto old_stack = stack;
    const auto count = banner_capture.total_count;
    const auto old_captured_unit = banner_capture.unit;
    const LONG result = handle_banner_exception(&exception);
    require(result == (accepted ? EXCEPTION_CONTINUE_EXECUTION
                                : EXCEPTION_CONTINUE_SEARCH),
            "dispatcher decision");
    auto expected = before;
    if (accepted) {
      expected.Rsp += 8;
      expected.Rip = reinterpret_cast<std::uintptr_t>(banner_image_begin) +
                     BANNER_RESUME_RVA;
    }
    require(std::memcmp(&expected, &context, sizeof(context)) == 0,
            "all CONTEXT bytes preserved except accepted RIP/RSP");
    require(unit == old_unit && unit_class == old_class && stack == old_stack &&
                std::memcmp(&record, &old_record, sizeof(record)) == 0,
            "unit, class, stack and exception record remain immutable");
    require(banner_capture.total_count == count + (accepted ? 1 : 0) &&
                banner_capture.unit == (accepted
                                            ? static_cast<LONG64>(before.Rsi)
                                            : old_captured_unit),
            "only accepted recovery updates diagnostic counters");
  }
};

static bool hardware_context_preserved{};
static std::size_t hardware_faults{};
static LONG CALLBACK observed_handler(EXCEPTION_POINTERS *exception) {
  const auto before = *exception->ContextRecord;
  const LONG result = handle_banner_exception(exception);
  if (result == EXCEPTION_CONTINUE_EXECUTION) {
    ++hardware_faults;
    auto expected = before;
    expected.Rsp += 8;
    expected.Rip = reinterpret_cast<std::uintptr_t>(banner_image_begin) +
                   BANNER_RESUME_RVA;
    hardware_context_preserved =
        std::memcmp(&expected, exception->ContextRecord, sizeof(expected)) == 0;
  }
  return result;
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "actual current game EXE argument required");
    std::ifstream stream(argv[1], std::ios::binary | std::ios::ate);
    require(stream.good(), "open EXE read only");
    const auto length = static_cast<std::size_t>(stream.tellg());
    std::vector<std::uint8_t> file(length);
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(file.data()), file.size());
    require(stream.good(), "read complete EXE");
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
    require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
                static_cast<std::size_t>(dos->e_lfanew) +
                        sizeof(IMAGE_NT_HEADERS64) <
                    file.size(),
            "valid input PE header bounds");
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
        file.data() + dos->e_lfanew);
    auto *image = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    require(image != nullptr, "allocate private offline PE mapping");
    std::memcpy(image, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      const auto &section = sections[i];
      require(static_cast<std::size_t>(section.PointerToRawData) +
                          section.SizeOfRawData <=
                      file.size() &&
                  static_cast<std::size_t>(section.VirtualAddress) +
                          section.SizeOfRawData <=
                      nt->OptionalHeader.SizeOfImage,
              "section copy remains in file and private allocation");
      std::memcpy(image + section.VirtualAddress,
                  file.data() + section.PointerToRawData,
                  section.SizeOfRawData);
    }
    require(has_banner_image_fingerprint(image), "actual 0.8.111 PE profile");
    require(has_banner_code(image), "all actual native instructions match");
    require(!has_banner_image_fingerprint(nullptr) && !has_banner_code(nullptr),
            "null image rejected");
    require(!has_expected_image_fingerprint(image),
            "0.8.111 never qualifies for legacy 0.8.110 hooks");
    auto *mapped_nt =
        reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    const auto test_header_field = [&](auto &field) {
      const auto original = field;
      field ^= 1;
      require(!has_banner_image_fingerprint(image),
              "PE identity mismatch rejected");
      field = original;
    };
    test_header_field(mapped_nt->Signature);
    test_header_field(mapped_nt->FileHeader.Machine);
    test_header_field(mapped_nt->FileHeader.TimeDateStamp);
    test_header_field(mapped_nt->OptionalHeader.Magic);
    test_header_field(mapped_nt->OptionalHeader.SizeOfImage);
    test_header_field(mapped_nt->OptionalHeader.CheckSum);
    for (const auto [rva, size] :
         {std::pair{BANNER_FAULT_RVA, BANNER_FAULT_BYTES.size()},
          std::pair{BANNER_CALL_SEQUENCE_RVA,
                    BANNER_CALL_SEQUENCE_BYTES.size()},
          std::pair{BANNER_RESUME_RVA, BANNER_RESUME_BYTES.size()}}) {
      for (std::size_t i = 0; i < size; ++i) {
        image[rva + i] ^= 1;
        require(!has_banner_code(image), "every signature byte is enforced");
        image[rva + i] ^= 1;
      }
    }
    require(has_banner_code(image), "native signature restored exactly");
    banner_image_begin = image;
    banner_capture = {};
    Fixture(image).check(true);
    Fixture dead(image);
    put(dead.unit, 0x30c, std::uint8_t{1});
    dead.check(true);
    require(handle_banner_exception(nullptr) == EXCEPTION_CONTINUE_SEARCH,
            "null exception safely rejected");
    EXCEPTION_POINTERS missing{};
    require(handle_banner_exception(&missing) == EXCEPTION_CONTINUE_SEARCH,
            "missing records safely rejected");
    const auto reject = [&](auto mutate) {
      Fixture fixture(image);
      mutate(fixture);
      fixture.check(false);
    };
    reject([](auto &f) {
      f.record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
    });
    reject([](auto &f) { f.record.ExceptionFlags = EXCEPTION_NONCONTINUABLE; });
    reject([](auto &f) { f.record.NumberParameters = 1; });
    reject([](auto &f) { f.record.ExceptionInformation[0] = 1; });
    reject([](auto &f) { f.record.ExceptionInformation[0] = 8; });
    reject([](auto &f) { f.record.ExceptionInformation[1] = 0x12e9; });
    reject([](auto &f) { f.record.ExceptionAddress = nullptr; });
    reject([](auto &f) { ++f.context.Rip; });
    reject([](auto &f) { f.context.Rcx = 1; });
    reject([](auto &f) { f.context.Rsi = 0; });
    reject([](auto &f) { ++f.context.Rsi; });
    reject([](auto &f) { f.context.Rsi = 0x1000; });
    reject([](auto &f) { f.context.Rsp = 1; });
    reject([](auto &f) { f.context.Rsp += 8; });
    reject([](auto &f) { ++f.stack[1]; });
    reject([](auto &f) { put(f.unit, 0, std::uintptr_t{1}); });
    reject([&](auto &f) {
      put(f.unit, 0,
          reinterpret_cast<std::uintptr_t>(image) + BANNER_EXPECTED_IMAGE_SIZE);
    });
    for (auto flag : {0x40000000U, 0x10000U, 0x8000U})
      reject([flag](auto &f) { put(f.unit, 8, flag); });
    reject([](auto &f) { put(f.unit, 0xc, std::int32_t{0}); });
    reject([](auto &f) { put(f.unit, 0xc, std::int32_t{-1}); });
    reject([](auto &f) { put(f.unit, 0x10, std::uintptr_t{0}); });
    reject([](auto &f) { put(f.unit, 0x10, std::uintptr_t{1}); });
    reject([](auto &f) { put(f.unit_class, 0, std::uintptr_t{1}); });
    reject([](auto &f) { put(f.unit, 0x320, std::uintptr_t{0x12345678}); });
    reject([](auto &f) { put(f.unit, 0x944, std::int32_t{0}); });
    reject([](auto &f) { put(f.unit, 0x944, std::int32_t{205}); });
    auto *inaccessible =
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    require(inaccessible != nullptr, "allocate no-access fixture page");
    reject([&](auto &f) {
      f.context.Rsi = reinterpret_cast<std::uintptr_t>(inaccessible);
    });
    reject([&](auto &f) {
      f.context.Rsp = reinterpret_cast<std::uintptr_t>(inaccessible) + 8;
    });
    reject([&](auto &f) {
      put(f.unit, 0x10, reinterpret_cast<std::uintptr_t>(inaccessible));
    });
    require(!has_banner_image_fingerprint(
                static_cast<std::uint8_t *>(inaccessible)),
            "inaccessible PE header is safe");
    require(VirtualFree(inaccessible, 0, MEM_RELEASE) != 0,
            "free fixture page");
    banner_image_begin = nullptr;
    Fixture(image).check(false);
    banner_image_begin = image;

    // Only this private mapped copy is altered. The getter and its caller's
    // owner load / CALL are the actual verified game bytes, at their real RVAs.
    // The surrounding harness replaces unrelated Unreal rendering calls and
    // models the subsequent saved dead-state restoration after resuming.
    constexpr std::uintptr_t entry_rva = BANNER_CALL_SEQUENCE_RVA - 0x100;
    const std::array<std::uint8_t, 28> entry{
        0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x89, 0x54,
        0x24, 0x20, 0x48, 0x89, 0x22, 0x48, 0x8b, 0xf1, 0x48, 0xbb,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    std::memcpy(image + entry_rva, entry.data(), entry.size());
    const auto jump = [&](std::uintptr_t from, std::uintptr_t to) {
      image[from] = 0xe9;
      const auto relative = static_cast<std::int32_t>(to - from - 5);
      std::memcpy(image + from + 1, &relative, sizeof(relative));
    };
    jump(entry_rva + entry.size(), BANNER_CALL_SEQUENCE_RVA);
    jump(BANNER_CALLER_RVA, BANNER_RESUME_RVA);
    const std::array<std::uint8_t, 44> resume{
        0x48, 0x8b, 0x54, 0x24, 0x20, 0x48, 0x89, 0x62, 0x08, 0x89, 0x42,
        0x10, 0x48, 0x89, 0x5a, 0x18, 0xc6, 0x86, 0x0c, 0x03, 0x00, 0x00,
        0x01, 0x48, 0xc7, 0x42, 0x20, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83,
        0xc4, 0x30, 0x5f, 0x5e, 0x5b, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(image + BANNER_RESUME_RVA, resume.data(), resume.size());
    DWORD old_protection{};
    require(VirtualProtect(image, nt->OptionalHeader.SizeOfImage,
                           PAGE_EXECUTE_READ, &old_protection) != 0,
            "make private replica executable and nonwritable");
    require(FlushInstructionCache(GetCurrentProcess(), image,
                                  nt->OptionalHeader.SizeOfImage) != 0,
            "publish isolated fixture instructions");
    void *handler = AddVectoredExceptionHandler(1, observed_handler);
    require(handler != nullptr, "register production handler observer");
    using RunReplica = int (*)(void *, std::uint64_t *);
    const auto run = reinterpret_cast<RunReplica>(image + entry_rva);
    Fixture actual(image);
    const auto unit_before = actual.unit;
    std::array<std::uint64_t, 5> result{};
    const auto count_before = banner_capture.total_count;
    require(run(actual.unit.data(), result.data()) == 1,
            "actual CPU NULL read safely resumes and returns");
    require(hardware_faults == 1 && hardware_context_preserved &&
                banner_capture.total_count == count_before + 1,
            "actual VEH changes only RIP/RSP and records one recovery");
    require(result[0] == result[1] && result[3] == 0x1122334455667788ULL &&
                result[4] == 1,
            "stack balanced, RBX preserved, post-banner continuation reached");
    auto expected_unit = unit_before;
    expected_unit[0x30c] = 1;
    require(actual.unit == expected_unit,
            "native continuation alone restores saved dead; owner/equipment "
            "unchanged");
    alignas(16) std::array<std::uint8_t, 0x1300> owner{};
    put(owner, 0x12e8, std::int32_t{37});
    put(actual.unit, 0x320, reinterpret_cast<std::uintptr_t>(owner.data()));
    put(actual.unit, 0x30c, std::uint8_t{0});
    const auto owned_unit_before = actual.unit;
    result = {};
    require(run(actual.unit.data(), result.data()) == 1 && result[2] == 37,
            "valid owner executes original getter and keeps actual pawn index");
    require(hardware_faults == 1 &&
                banner_capture.total_count == count_before + 1,
            "normal owner path never invokes recovery");
    expected_unit = owned_unit_before;
    expected_unit[0x30c] = 1;
    require(
        actual.unit == expected_unit && result[0] == result[1],
        "normal caller remains balanced and does not change owner/equipment");
    require(RemoveVectoredExceptionHandler(handler) != 0, "remove fixture VEH");
    banner_image_begin = nullptr;
    require(VirtualFree(image, 0, MEM_RELEASE) != 0, "free isolated PE copy");
    std::printf("PASS %zu checks; actual hardware AV recovered once; native "
                "owned path unchanged\n",
                checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}

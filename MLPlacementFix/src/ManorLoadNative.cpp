#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

#include "ManorLoadNative.hpp"
#include "ManorLoadPatch.hpp"

static constexpr std::array<std::uint8_t, 9> original_branch{
    0x4c, 0x39, 0x33, 0x0f, 0x84, 0xc8, 0x00, 0x00, 0x00};
static constexpr std::array<std::uint8_t, 14> original_begin{
    0x4d, 0x8b, 0xae, 0x98, 0x03, 0, 0, 0x49, 0x63, 0x86, 0xa0, 0x03, 0, 0};
static constexpr std::array<std::uint8_t, 7> original_end{
    0x48, 0x8d, 0x95, 0x48, 0x02, 0, 0};

auto manor_load_read(std::uintptr_t address, void *output,
                     std::size_t size) noexcept -> bool {
  SIZE_T copied{};
  return address != 0 && output != nullptr && size != 0 &&
         size <= std::numeric_limits<std::uintptr_t>::max() - address &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) != 0 &&
         copied == size;
}

auto manor_load_fingerprints() -> std::span<const ManorLoadFingerprint> {
  static const auto fingerprints = [] {
    constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 8> raw{{
        {0x4c9301e, "4d8bae98030000496386a00300004c69e0b00000004d03e54c896580"},
        {0x4c9303d, "0f84c70300004d8dbda80000004533f6"},
        {0x4c931b7, "418b4d00e800030300488d98800100004c39330f84c8000000"},
        {0x4c93298, "418b4d00e86f361200410f1047c8498d57d88bd8"},
        {0x4c93354, "c644243801488bce44887424304889442428488d8548100000"},
        {0x4c93372, "0f1185900e0000e8328c0500453977f07e59"},
        {0x4c9340a, "488d9548020000488bcfe877a90a0033c0448bf88bd88b8550020000"},
        {0x4c93435, "453bbe280400007d41488b85480200004d8b8620040000"},
    }};
    std::array<ManorLoadFingerprint, raw.size()> result;
    for (std::size_t i{}; i < raw.size(); ++i) {
      result[i].rva = raw[i].first;
      for (std::size_t j{}; j < raw[i].second.size(); j += 2) {
        const auto digit = [](char c) {
          return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        result[i].bytes.push_back(static_cast<std::uint8_t>(
            digit(raw[i].second[j]) * 16 + digit(raw[i].second[j + 1])));
      }
    }
    return result;
  }();
  return fingerprints;
}

auto manor_load_validate_image(const std::uint8_t *image) -> bool {
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!manor_load_read(base, &dos, sizeof(dos)) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x1000 ||
      !manor_load_read(base + dos.e_lfanew, &nt, sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.TimeDateStamp != 0x6836c1ba ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09dfd000 ||
      nt.OptionalHeader.CheckSum != 0x09a2ba15)
    return false;
  // Audited Steam 0.8.117 (build 26185246) SHA256:
  // 560451df38cd91134026c25b69539449c570f3163e33bc8c45b4dc20b6952cee.
  for (const auto &fingerprint : manor_load_fingerprints()) {
    std::array<std::uint8_t, 32> bytes{};
    if (fingerprint.bytes.size() > bytes.size() ||
        fingerprint.rva + fingerprint.bytes.size() >
            nt.OptionalHeader.SizeOfImage ||
        !manor_load_read(base + fingerprint.rva, bytes.data(),
                         fingerprint.bytes.size()) ||
        std::memcmp(bytes.data(), fingerprint.bytes.data(),
                    fingerprint.bytes.size()) != 0)
      return false;
  }
  return true;
}

static auto write_manor_branch(std::uint8_t *site,
                               std::span<const std::uint8_t> bytes) -> bool {
  DWORD original{};
  if (!VirtualProtect(site, bytes.size(), PAGE_EXECUTE_READWRITE, &original))
    return false;
  std::memcpy(site, bytes.data(), bytes.size());
  const bool flushed =
      FlushInstructionCache(GetCurrentProcess(), site, bytes.size()) != 0;
  DWORD ignored{};
  const bool restored =
      VirtualProtect(site, bytes.size(), original, &ignored) != 0;
  return flushed && restored;
}

ManorLoadHook::~ManorLoadHook() { restore(); }

auto ManorLoadHook::restore() -> bool {
  // Check every site before restoring any. A foreign owner must not leave a
  // begin/end pair partially removed while its temporary records are in use.
  for (const auto &site : sites_) {
    if (site.address == nullptr)
      continue;
    std::vector<std::uint8_t> current(site.original.size());
    if (!manor_load_read(reinterpret_cast<std::uintptr_t>(site.address),
                         current.data(), current.size()) ||
        (current != site.original && current != site.replacement))
      return false;
  }
  for (auto &site : sites_) {
    if (site.address != nullptr &&
        !write_manor_branch(site.address, site.original))
      return false;
  }
  sites_ = {};
  if (stub_ != nullptr) {
    if (!VirtualFree(stub_, 0, MEM_RELEASE))
      return false;
    stub_ = nullptr;
  }
  return true;
}

auto ManorLoadHook::install(std::uint8_t *image, const void *route_callback,
                            const void *begin_callback,
                            const void *end_callback) -> bool {
  if (installed() || stub_ != nullptr || route_callback == nullptr ||
      begin_callback == nullptr || end_callback == nullptr ||
      !manor_load_validate_image(image))
    return false;
  const std::array<std::uintptr_t, 3> rvas{0x4c9301e, 0x4c931c7, 0x4c9340a};
  const std::array<std::span<const std::uint8_t>, 3> originals{
      original_begin, original_branch, original_end};
  const std::array<std::vector<std::uint8_t>, 3> bridges{
      manor_load_begin_bridge(begin_callback, image + 0x4c9302c),
      manor_load_bridge(route_callback, image + 0x4c93298, image + 0x4c931d0),
      manor_load_end_bridge(end_callback, image + 0x4c93411)};
  std::array<std::size_t, 3> offsets{};
  std::size_t required{};
  for (std::size_t i{}; i < bridges.size(); ++i) {
    if (bridges[i].empty() || bridges[i].size() > 0x1000 - required)
      return false;
    offsets[i] = required;
    required += (bridges[i].size() + 15) & ~std::size_t{15};
    if (required > 0x1000)
      return false;
  }
  const auto site_address = reinterpret_cast<std::uintptr_t>(image + rvas[0]);
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const std::uintptr_t granularity = info.dwAllocationGranularity;
  if (granularity == 0 || (granularity & (granularity - 1)) != 0)
    return false;
  const auto origin = site_address & ~(granularity - 1);
  const auto minimum =
      reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
  const auto maximum =
      reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
  for (std::uintptr_t distance{}; distance < 0x70000000 && stub_ == nullptr;
       distance += granularity) {
    for (const bool above : {true, false}) {
      if ((!above && distance > origin) ||
          (above && distance > maximum - origin))
        continue;
      const auto address = above ? origin + distance : origin - distance;
      if (address < minimum || address > maximum || maximum - address < 0x1000)
        continue;
      stub_ = static_cast<std::uint8_t *>(
          VirtualAlloc(reinterpret_cast<void *>(address), 0x1000,
                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
      if (stub_ != nullptr)
        break;
    }
  }
  if (stub_ == nullptr)
    return false;
  for (std::size_t i{}; i < bridges.size(); ++i)
    std::memcpy(stub_ + offsets[i], bridges[i].data(), bridges[i].size());
  DWORD previous{};
  if (!VirtualProtect(stub_, 0x1000, PAGE_EXECUTE_READ, &previous) ||
      !FlushInstructionCache(GetCurrentProcess(), stub_, required)) {
    restore();
    return false;
  }
  std::array<Site, 3> prepared;
  for (std::size_t i{}; i < prepared.size(); ++i) {
    auto &site = prepared[i];
    site.address = image + rvas[i];
    site.original.assign(originals[i].begin(), originals[i].end());
    site.replacement.assign(site.original.size(), 0x90);
    const auto relative =
        static_cast<std::int64_t>(
            reinterpret_cast<std::uintptr_t>(stub_ + offsets[i])) -
        static_cast<std::int64_t>(
            reinterpret_cast<std::uintptr_t>(site.address) + 5);
    std::vector<std::uint8_t> current(site.original.size());
    if (relative < INT32_MIN || relative > INT32_MAX ||
        !manor_load_read(reinterpret_cast<std::uintptr_t>(site.address),
                         current.data(), current.size()) ||
        current != site.original) {
      restore();
      return false;
    }
    site.replacement[0] = 0xe9;
    const auto displacement = static_cast<std::int32_t>(relative);
    std::memcpy(site.replacement.data() + 1, &displacement,
                sizeof(displacement));
  }
  sites_ = std::move(prepared);
  // Publish end/route first and begin last: no new plan can start until all
  // callbacks and continuations are installed.
  for (std::size_t i = sites_.size(); i-- > 0;) {
    if (!write_manor_branch(sites_[i].address, sites_[i].replacement)) {
      restore();
      return false;
    }
  }
  return true;
}

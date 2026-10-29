#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <polyhook2/Detour/x64Detour.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>

#include "Config.hpp"
#include "UE4SSABI.hpp"

#pragma intrinsic(_ReturnAddress)

namespace {
using RC::Unreal::UObject;

using GetPropertyValueFunction = void *(*)(UObject *, const wchar_t *);
using GetWorldFunction = UObject *(*)(const UObject *);
using SendLogFunction = void (*)(std::wstring_view);

constexpr std::string_view GET_PROPERTY_VALUE_EXPORT =
    "?GetValuePtrByPropertyNameInChain@UObject@Unreal@RC@@QEAAPEAXPEB_W@Z";
constexpr std::string_view GET_WORLD_EXPORT =
    "?GetWorld@UObject@Unreal@RC@@QEBAPEAVUWorld@23@XZ";
constexpr std::string_view SEND_LOG_EXPORT =
    "?send@Output@RC@@YAXV?$basic_string_view@_WU?$char_traits@_W@std@@@std@@@"
    "Z";

constexpr std::string_view GENERATE_RESOURCES_PATTERN =
    "4C 8B DC 55 49 8D AB 18 FE FF FF 48 81 EC E0 02 00 00 48 8B 05 ?? ?? ?? "
    "?? 48 33 C4 48 89 85 50 01 00 00";
constexpr std::string_view PREPARE_REGION_CALL_PATTERN =
    "4C 8B 21 49 8B CC 4C 89 65 80 E8 ?? ?? ?? ?? 8B 45 9C 4C 89 7C 24 70";
constexpr std::string_view PICK_RICH_CALL_PATTERN =
    "4C 8D 85 00 01 00 00 49 8B CD 48 8D 55 C0 E8 ?? ?? ?? ?? 48 63 5C 24 78 "
    "8B F0";
constexpr std::string_view SORT_RESOURCES_CALL_PATTERN =
    "8B 54 24 40 48 8D 44 24 70 48 8B 4C 24 38 4C 8D 4D 80 44 0F B6 C7 48 89 "
    "45 80 E8 ?? ?? ?? ?? 48 8B 5C 24 38";
constexpr std::string_view GET_RESOURCE_PROPERTIES_PATTERN =
    "48 89 5C 24 20 57 48 83 EC 20 48 8B 59 68 48 8B FA 48 63 41 70 48 C1 E0 "
    "07 48 03 C3 48 3B D8";
constexpr std::size_t PREPARE_REGION_CALL_OFFSET = 10;
constexpr std::size_t PICK_RICH_CALL_OFFSET = 14;
constexpr std::size_t SORT_RESOURCES_CALL_OFFSET = 26;

struct NativeArray {
  std::int32_t *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct NativeWideString {
  wchar_t *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct NativeUrlPrefix {
  NativeWideString protocol{};
  NativeWideString host{};
  std::int32_t port{};
  std::int32_t valid{};
  NativeWideString map{};
};

static_assert(offsetof(NativeUrlPrefix, map) == 0x28);

struct NativeResourceNodePropertiesPrefix {
  std::uint8_t is_limited_resource{};
  std::uint8_t underground_deposits_when_rich{};
  std::uint8_t padding[2]{};
  std::int32_t resource_type_id{};
  std::int32_t resource_count{};
  std::int32_t minimum_resource_amount{};
  std::int32_t maximum_resource_amount{};
  std::int32_t minimum_rich_resource_amount{};
  std::int32_t maximum_rich_resource_amount{};
};

static_assert(offsetof(NativeResourceNodePropertiesPrefix,
                       minimum_resource_amount) == 0xC);
static_assert(offsetof(NativeResourceNodePropertiesPrefix,
                       maximum_rich_resource_amount) == 0x18);

struct MemoryRange {
  std::uint8_t *data{};
  std::size_t size{};
};

struct GenerationState {
  bool active{};
  bool current_failed{};
  bool current_applied{};
  bool any_failed{};
  bool main_player_found{};
  UObject *current_region{};
  const MLSetRegionResource::Preset *preset{};
  std::array<std::int32_t, 6> effective_candidates{};
  std::size_t rich_pick_index{};
  std::size_t selected_count{};
  std::size_t applied_count{};
};

thread_local GenerationState generation_state{};
HMODULE mod_module{};
GetPropertyValueFunction get_property_value{};
GetWorldFunction get_world{};
SendLogFunction send_log{};

auto mark_current_region_failed() -> void {
  generation_state.current_failed = true;
  generation_state.any_failed = true;
}

auto widen_ascii(const std::string_view value) -> std::wstring {
  return {value.begin(), value.end()};
}

auto read_ascii_string(const NativeWideString *value)
    -> std::optional<std::string> {
  if (value == nullptr || value->data == nullptr || value->count < 2 ||
      value->capacity < value->count || value->count > 1024 ||
      value->data[value->count - 1] != L'\0') {
    return std::nullopt;
  }

  std::string result{};
  result.reserve(static_cast<std::size_t>(value->count - 1));
  for (std::int32_t index{}; index < value->count - 1; ++index) {
    const wchar_t character = value->data[index];
    if (character < 0 || character > 0x7f) {
      return std::nullopt;
    }
    result.push_back(static_cast<char>(character));
  }
  return result;
}

auto describe_preset(const MLSetRegionResource::Preset &preset)
    -> std::wstring {
  std::wostringstream description{};
  description << L"forced=";
  bool has_forced_resource{};
  const auto append =
      [&](const std::wstring_view slot,
          const std::optional<MLSetRegionResource::NodeType> type) {
        if (!type.has_value()) {
          return;
        }
        if (has_forced_resource) {
          description << L',';
        }
        description << slot << L':'
                    << widen_ascii(MLSetRegionResource::node_type_name(*type));
        has_forced_resource = true;
      };
  append(L"Gatherable", preset.gatherable);
  append(L"WildlifeOrWater", preset.wildlife_or_water);
  append(L"PrimaryDeposit", preset.primary_deposit);
  if (!has_forced_resource) {
    description << L"none";
  }
  description
      << L" rich="
      << widen_ascii(MLSetRegionResource::node_type_name(preset.rich[0]))
      << L','
      << widen_ascii(MLSetRegionResource::node_type_name(preset.rich[1]));
  description << L" max=";
  if (preset.maximum_resources.empty()) {
    description << L"none";
  } else {
    for (std::size_t index{}; index < preset.maximum_resources.size();
         ++index) {
      if (index != 0) {
        description << L',';
      }
      description << widen_ascii(
          MLSetRegionResource::node_type_name(preset.maximum_resources[index]));
    }
  }
  return description.str();
}

auto log_message(const std::wstring_view message) -> void {
  std::wstring line{L"[MLSetRegionResource] "};
  line.append(message);
  line.push_back(L'\n');
  if (send_log != nullptr) {
    send_log(line);
  } else {
    OutputDebugStringW(line.c_str());
  }
}

template <typename Function>
auto resolve_export(HMODULE module, const std::string_view name) -> Function {
  const std::string null_terminated_name{name};
  return reinterpret_cast<Function>(
      GetProcAddress(module, null_terminated_name.c_str()));
}

auto resolve_ue4ss_api() -> bool {
  HMODULE ue4ss = GetModuleHandleW(L"UE4SS.dll");
  if (ue4ss == nullptr) {
    return false;
  }
  get_property_value = resolve_export<GetPropertyValueFunction>(
      ue4ss, GET_PROPERTY_VALUE_EXPORT);
  get_world = resolve_export<GetWorldFunction>(ue4ss, GET_WORLD_EXPORT);
  send_log = resolve_export<SendLogFunction>(ue4ss, SEND_LOG_EXPORT);
  return get_property_value != nullptr && get_world != nullptr;
}

auto config_path() -> std::optional<std::filesystem::path> {
  if (mod_module == nullptr) {
    return std::nullopt;
  }

  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(mod_module, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return std::nullopt;
  }
  buffer.resize(length);
  return std::filesystem::path{buffer}.parent_path().parent_path() /
         L"config.ini";
}

auto hex_value(const char character) -> std::optional<std::uint8_t> {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint8_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint8_t>(character - 'a' + 10);
  }
  if (character >= 'A' && character <= 'F') {
    return static_cast<std::uint8_t>(character - 'A' + 10);
  }
  return std::nullopt;
}

auto parse_pattern(const std::string_view pattern)
    -> std::optional<std::vector<std::int16_t>> {
  std::vector<std::int16_t> bytes{};
  std::size_t position{};
  while (position < pattern.size()) {
    while (position < pattern.size() &&
           std::isspace(static_cast<unsigned char>(pattern[position])) != 0) {
      ++position;
    }
    if (position == pattern.size()) {
      break;
    }

    if (pattern[position] == '?') {
      ++position;
      if (position < pattern.size() && pattern[position] == '?') {
        ++position;
      }
      bytes.emplace_back(-1);
      continue;
    }

    if (position + 1 >= pattern.size()) {
      return std::nullopt;
    }
    const std::optional<std::uint8_t> high = hex_value(pattern[position]);
    const std::optional<std::uint8_t> low = hex_value(pattern[position + 1]);
    if (!high.has_value() || !low.has_value()) {
      return std::nullopt;
    }
    bytes.emplace_back(static_cast<std::int16_t>((*high << 4) | *low));
    position += 2;
  }
  return bytes;
}

auto find_text_section() -> std::optional<MemoryRange> {
  auto *module = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
  if (module == nullptr) {
    return std::nullopt;
  }

  const auto *dos_header = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
    return std::nullopt;
  }
  const auto *nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      module + dos_header->e_lfanew);
  if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
    return std::nullopt;
  }

  const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt_headers);
  for (std::uint16_t index{}; index < nt_headers->FileHeader.NumberOfSections;
       ++index, ++section) {
    if (std::memcmp(section->Name, ".text", 5) == 0) {
      return MemoryRange{module + section->VirtualAddress,
                         section->Misc.VirtualSize};
    }
  }
  return std::nullopt;
}

auto find_pattern_matches(const MemoryRange range,
                          const std::string_view pattern_text)
    -> std::optional<std::vector<std::uint8_t *>> {
  const std::optional<std::vector<std::int16_t>> pattern =
      parse_pattern(pattern_text);
  if (!pattern.has_value() || pattern->empty() ||
      pattern->size() > range.size) {
    return std::nullopt;
  }

  std::vector<std::uint8_t *> matches{};
  for (std::size_t offset{}; offset <= range.size - pattern->size(); ++offset) {
    bool matched = true;
    for (std::size_t pattern_index{}; pattern_index < pattern->size();
         ++pattern_index) {
      if ((*pattern)[pattern_index] >= 0 &&
          range.data[offset + pattern_index] != (*pattern)[pattern_index]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      matches.emplace_back(range.data + offset);
    }
  }
  return matches;
}

auto resolve_relative_call(std::uint8_t *call_site)
    -> std::optional<std::uint8_t *> {
  if (call_site == nullptr || *call_site != 0xE8) {
    return std::nullopt;
  }

  std::int32_t displacement{};
  std::memcpy(&displacement, call_site + 1, sizeof(displacement));
  return call_site + 5 + displacement;
}

auto contains_original_resource_slots(const NativeArray &candidates) -> bool {
  if (candidates.data == nullptr || candidates.count != 6 ||
      candidates.capacity < 6) {
    return false;
  }

  const std::span<const std::int32_t> values{
      candidates.data, static_cast<std::size_t>(candidates.count)};
  const auto has = [&](const MLSetRegionResource::NodeType type) {
    return std::ranges::find(values, static_cast<std::int32_t>(type)) !=
           values.end();
  };
  return (has(MLSetRegionResource::NodeType::Fish) !=
          has(MLSetRegionResource::NodeType::SmallGame)) &&
         (has(MLSetRegionResource::NodeType::Salt) !=
          has(MLSetRegionResource::NodeType::Iron)) &&
         (has(MLSetRegionResource::NodeType::Berries) !=
          has(MLSetRegionResource::NodeType::Mushrooms)) &&
         has(MLSetRegionResource::NodeType::Clay) &&
         has(MLSetRegionResource::NodeType::Stone) &&
         has(MLSetRegionResource::NodeType::Fertility);
}

class MLSetRegionResourceMod final : public RC::CppUserModBase {
public:
  MLSetRegionResourceMod() {
    instance = this;
    ModName = L"MLSetRegionResource";
    ModVersion = L"0.1.0";
    ModDescription = L"Fixes configured regional resources and rich traits";
    ModAuthors = L"jihong_min and Codex";
  }

  ~MLSetRegionResourceMod() override {
    uninstall_hooks();
    instance = nullptr;
  }

  auto on_unreal_init() -> void override {
    if (!resolve_ue4ss_api()) {
      log_message(L"Disabled: the required UE4SS 3.0.1 reflection export was "
                  L"not found.");
      return;
    }
    const std::optional<std::filesystem::path> path = config_path();
    if (!path.has_value()) {
      log_message(L"Disabled: could not locate the mod directory.");
      return;
    }

    MLSetRegionResource::ConfigResult parsed_config =
        MLSetRegionResource::load_config(*path);
    if (!parsed_config.config.has_value()) {
      log_message(L"Disabled: " + widen_ascii(parsed_config.error));
      return;
    }
    config = std::move(*parsed_config.config);

    if (!install_hooks()) {
      uninstall_hooks();
      log_message(L"Native signatures did not match exactly once. No game "
                  L"data will be changed.");
      return;
    }
    log_message(L"Ready: loaded " +
                std::to_wstring(config.scoped_presets.size()) +
                L" scoped preset(s)" +
                (config.fallback.has_value() ? L" plus [User]." : L"."));
  }

private:
  using GenerateResourcesFunction = void (*)(void *engine);
  using PrepareRegionFunction = void (*)(void *region);
  using PickRichFunction = std::int32_t (*)(void *engine,
                                            NativeArray *candidates,
                                            void *usage_counts);
  using SortResourcesFunction = void (*)(std::int32_t *candidates,
                                         std::int32_t count,
                                         std::uint8_t random_seed,
                                         void *rich_context);
  using GetResourcePropertiesFunction = NativeResourceNodePropertiesPrefix
      *(*)(void *settings, NativeResourceNodePropertiesPrefix *output,
           std::int32_t node_type);

  static inline MLSetRegionResourceMod *instance{};

  MLSetRegionResource::Config config{};
  std::unique_ptr<PLH::x64Detour> generate_resources_hook{};
  std::unique_ptr<PLH::x64Detour> prepare_region_hook{};
  std::unique_ptr<PLH::x64Detour> pick_rich_hook{};
  std::unique_ptr<PLH::x64Detour> sort_resources_hook{};
  std::unique_ptr<PLH::x64Detour> get_resource_properties_hook{};
  std::uint64_t generate_resources_trampoline{};
  std::uint64_t prepare_region_trampoline{};
  std::uint64_t pick_rich_trampoline{};
  std::uint64_t sort_resources_trampoline{};
  std::uint64_t get_resource_properties_trampoline{};
  std::uint8_t *prepare_region_return{};
  std::uint8_t *pick_rich_return{};
  std::uint8_t *sort_resources_return{};
  bool property_error_reported{};

  auto scan_unique(const MemoryRange range, const std::string_view name,
                   const std::string_view pattern) -> std::uint8_t * {
    const std::optional<std::vector<std::uint8_t *>> matches =
        find_pattern_matches(range, pattern);
    if (!matches.has_value() || matches->size() != 1) {
      const std::size_t count = matches.has_value() ? matches->size() : 0;
      log_message(L"Signature " + widen_ascii(name) + L" matched " +
                  std::to_wstring(count) + L" locations; expected 1.");
      return nullptr;
    }
    return matches->front();
  }

  auto install_hook(std::unique_ptr<PLH::x64Detour> &hook, std::uint8_t *target,
                    const std::uint64_t callback, std::uint64_t &trampoline)
      -> bool {
    hook = std::make_unique<PLH::x64Detour>(
        std::bit_cast<std::uint64_t>(target), callback, &trampoline);
    return hook->hook();
  }

  auto install_hooks() -> bool {
    const std::optional<MemoryRange> text = find_text_section();
    if (!text.has_value()) {
      log_message(L"Could not locate the executable .text section.");
      return false;
    }

    std::uint8_t *generate_resources =
        scan_unique(*text, "generate resources", GENERATE_RESOURCES_PATTERN);
    std::uint8_t *prepare_region_pattern =
        scan_unique(*text, "prepare region call", PREPARE_REGION_CALL_PATTERN);
    std::uint8_t *pick_rich_pattern =
        scan_unique(*text, "pick rich call", PICK_RICH_CALL_PATTERN);
    std::uint8_t *sort_resources_pattern =
        scan_unique(*text, "sort resources call", SORT_RESOURCES_CALL_PATTERN);
    std::uint8_t *get_resource_properties = scan_unique(
        *text, "get resource properties", GET_RESOURCE_PROPERTIES_PATTERN);
    if (generate_resources == nullptr || prepare_region_pattern == nullptr ||
        pick_rich_pattern == nullptr || sort_resources_pattern == nullptr ||
        get_resource_properties == nullptr) {
      return false;
    }

    std::uint8_t *prepare_region_call =
        prepare_region_pattern + PREPARE_REGION_CALL_OFFSET;
    std::uint8_t *pick_rich_call = pick_rich_pattern + PICK_RICH_CALL_OFFSET;
    std::uint8_t *sort_resources_call =
        sort_resources_pattern + SORT_RESOURCES_CALL_OFFSET;
    const std::optional<std::uint8_t *> prepare_region =
        resolve_relative_call(prepare_region_call);
    const std::optional<std::uint8_t *> pick_rich =
        resolve_relative_call(pick_rich_call);
    const std::optional<std::uint8_t *> sort_resources =
        resolve_relative_call(sort_resources_call);
    if (!prepare_region.has_value() || !pick_rich.has_value() ||
        !sort_resources.has_value()) {
      return false;
    }

    const auto belongs_to_generator = [generate_resources](
                                          const std::uint8_t *call) {
      return call > generate_resources && call < generate_resources + 0x1000;
    };
    if (!belongs_to_generator(prepare_region_call) ||
        !belongs_to_generator(pick_rich_call) ||
        !belongs_to_generator(sort_resources_call)) {
      return false;
    }

    prepare_region_return = prepare_region_call + 5;
    pick_rich_return = pick_rich_call + 5;
    sort_resources_return = sort_resources_call + 5;

    if (!install_hook(prepare_region_hook, *prepare_region,
                      std::bit_cast<std::uint64_t>(
                          &MLSetRegionResourceMod::hooked_prepare_region),
                      prepare_region_trampoline) ||
        !install_hook(pick_rich_hook, *pick_rich,
                      std::bit_cast<std::uint64_t>(
                          &MLSetRegionResourceMod::hooked_pick_rich),
                      pick_rich_trampoline) ||
        !install_hook(sort_resources_hook, *sort_resources,
                      std::bit_cast<std::uint64_t>(
                          &MLSetRegionResourceMod::hooked_sort_resources),
                      sort_resources_trampoline) ||
        !install_hook(
            get_resource_properties_hook, get_resource_properties,
            std::bit_cast<std::uint64_t>(
                &MLSetRegionResourceMod::hooked_get_resource_properties),
            get_resource_properties_trampoline) ||
        !install_hook(generate_resources_hook, generate_resources,
                      std::bit_cast<std::uint64_t>(
                          &MLSetRegionResourceMod::hooked_generate_resources),
                      generate_resources_trampoline)) {
      return false;
    }
    return true;
  }

  auto uninstall_hooks() -> void {
    if (generate_resources_hook != nullptr) {
      generate_resources_hook->unHook();
      generate_resources_hook.reset();
    }
    if (sort_resources_hook != nullptr) {
      sort_resources_hook->unHook();
      sort_resources_hook.reset();
    }
    if (get_resource_properties_hook != nullptr) {
      get_resource_properties_hook->unHook();
      get_resource_properties_hook.reset();
    }
    if (pick_rich_hook != nullptr) {
      pick_rich_hook->unHook();
      pick_rich_hook.reset();
    }
    if (prepare_region_hook != nullptr) {
      prepare_region_hook->unHook();
      prepare_region_hook.reset();
    }
  }

  auto is_main_player_region(UObject *region) -> bool {
    if (region == nullptr) {
      return false;
    }
    auto **owner =
        static_cast<UObject **>(get_property_value(region, L"ownerPawn"));
    if (owner == nullptr) {
      if (!property_error_reported) {
        log_message(L"ARegion.ownerPawn was not found; preset skipped.");
        property_error_reported = true;
      }
      return false;
    }
    if (*owner == nullptr) {
      return false;
    }

    auto *is_main_player = static_cast<std::uint8_t *>(
        get_property_value(*owner, L"isMainPlayer"));
    if (is_main_player == nullptr) {
      if (!property_error_reported) {
        log_message(L"APawnCPP.isMainPlayer was not found; preset skipped.");
        property_error_reported = true;
      }
      return false;
    }
    return *is_main_player != 0;
  }

  auto on_region_prepared(UObject *region) -> void {
    generation_state.current_region = region;
    generation_state.preset = nullptr;
    generation_state.rich_pick_index = 0;
    generation_state.current_failed = false;
    generation_state.current_applied = false;

    const bool main_player_region = is_main_player_region(region);
    if (main_player_region) {
      generation_state.main_player_found = true;
    }

    auto *region_name_value = static_cast<NativeWideString *>(
        get_property_value(region, L"regionName"));
    UObject *world = get_world(region);
    const std::optional<std::string> region_key =
        read_ascii_string(region_name_value);
    if (!region_key.has_value()) {
      mark_current_region_failed();
      log_message(L"Could not read ARegion.regionName; region preset "
                  L"skipped.");
      return;
    }
    if (world == nullptr) {
      mark_current_region_failed();
      log_message(L"Could not find the region's UWorld; region preset "
                  L"skipped.");
      return;
    }

    // UE4SS 3.0.1 reports UWorld::URL at 0x5A0 for Manor Lords 0.8.100,
    // but the native FURL member is not available through UProperty lookup.
    auto *world_url = reinterpret_cast<NativeUrlPrefix *>(
        reinterpret_cast<std::uint8_t *>(world) + 0x5A0);
    std::optional<std::string> map_key = read_ascii_string(&world_url->map);
    if (!map_key.has_value()) {
      mark_current_region_failed();
      log_message(L"Could not read UWorld.URL.Map; region preset skipped.");
      return;
    }

    const std::size_t path_separator = map_key->find_last_of("/\\");
    if (path_separator != std::string::npos) {
      map_key->erase(0, path_separator + 1);
    }
    const std::size_t object_separator = map_key->find('.');
    if (object_separator != std::string::npos) {
      map_key->erase(object_separator);
    }
    if (map_key->empty()) {
      mark_current_region_failed();
      log_message(L"The map URL did not contain an asset name; region preset "
                  L"skipped.");
      return;
    }

    generation_state.preset =
        config.select(*map_key, *region_key, main_player_region);
    const std::wstring map_name = widen_ascii(*map_key);
    const std::wstring region_name = widen_ascii(*region_key);
    if (generation_state.preset == nullptr) {
      if (main_player_region) {
        log_message(L"No preset matched " + map_name + L"/" + region_name +
                    L"; vanilla resources retained.");
      }
      return;
    }

    ++generation_state.selected_count;
    log_message(L"Selected " + map_name + L"/" + region_name + L": " +
                describe_preset(*generation_state.preset));
  }

  auto force_rich_pick(void *engine, NativeArray *candidates,
                       void *usage_counts) -> std::int32_t {
    auto original = reinterpret_cast<PickRichFunction>(pick_rich_trampoline);
    const MLSetRegionResource::Preset *preset = generation_state.preset;
    const std::size_t pick_index = generation_state.rich_pick_index;
    if (preset == nullptr || pick_index >= preset->rich.size() ||
        candidates == nullptr || candidates->data == nullptr ||
        candidates->capacity < 6) {
      mark_current_region_failed();
      return original(engine, candidates, usage_counts);
    }
    if (pick_index == 0 && !contains_original_resource_slots(*candidates)) {
      mark_current_region_failed();
      log_message(L"The original six-slot resource layout changed; preset "
                  L"skipped for safety.");
      return original(engine, candidates, usage_counts);
    }

    if (pick_index == 0) {
      std::ranges::copy_n(candidates->data,
                          generation_state.effective_candidates.size(),
                          generation_state.effective_candidates.begin());
      const auto replace_slot =
          [](std::array<std::int32_t, 6> &values,
             const MLSetRegionResource::NodeType first,
             const MLSetRegionResource::NodeType second,
             const std::optional<MLSetRegionResource::NodeType> forced) {
            if (!forced.has_value()) {
              return;
            }
            const auto slot =
                std::ranges::find_if(values, [&](const auto value) {
                  return value == static_cast<std::int32_t>(first) ||
                         value == static_cast<std::int32_t>(second);
                });
            *slot = static_cast<std::int32_t>(*forced);
          };
      replace_slot(generation_state.effective_candidates,
                   MLSetRegionResource::NodeType::Berries,
                   MLSetRegionResource::NodeType::Mushrooms,
                   preset->gatherable);
      replace_slot(generation_state.effective_candidates,
                   MLSetRegionResource::NodeType::Fish,
                   MLSetRegionResource::NodeType::SmallGame,
                   preset->wildlife_or_water);
      replace_slot(generation_state.effective_candidates,
                   MLSetRegionResource::NodeType::Salt,
                   MLSetRegionResource::NodeType::Iron,
                   preset->primary_deposit);
    }

    const std::int32_t chosen_type =
        static_cast<std::int32_t>(preset->rich[pick_index]);
    if (std::ranges::find(generation_state.effective_candidates, chosen_type) ==
        generation_state.effective_candidates.end()) {
      mark_current_region_failed();
      return original(engine, candidates, usage_counts);
    }
    candidates->data[0] = chosen_type;
    candidates->count = 1;
    const std::int32_t original_result =
        original(engine, candidates, usage_counts);
    if (original_result != chosen_type) {
      mark_current_region_failed();
      return original_result;
    }

    ++generation_state.rich_pick_index;
    if (pick_index == 0) {
      std::size_t output_index{};
      for (const std::int32_t resource :
           generation_state.effective_candidates) {
        if (resource != chosen_type) {
          candidates->data[output_index++] = resource;
        }
      }
      if (output_index != 5) {
        mark_current_region_failed();
        return chosen_type;
      }
      candidates->count = static_cast<std::int32_t>(output_index);
    }
    return chosen_type;
  }

  auto replace_final_arrays(std::int32_t *candidates, const std::int32_t count,
                            void *rich_context) -> void {
    const MLSetRegionResource::Preset *preset = generation_state.preset;
    if (preset == nullptr ||
        generation_state.rich_pick_index != preset->rich.size() ||
        candidates == nullptr || count != 6 || rich_context == nullptr) {
      mark_current_region_failed();
      return;
    }

    auto **rich_array_pointer = static_cast<NativeArray **>(rich_context);
    NativeArray *rich_array =
        rich_array_pointer == nullptr ? nullptr : *rich_array_pointer;
    if (rich_array == nullptr || rich_array->data == nullptr ||
        rich_array->count != 2 || rich_array->capacity < 2) {
      mark_current_region_failed();
      return;
    }

    std::ranges::copy(generation_state.effective_candidates, candidates);
    for (std::size_t index{}; index < preset->rich.size(); ++index) {
      rich_array->data[index] = static_cast<std::int32_t>(preset->rich[index]);
    }
    if (!generation_state.current_applied) {
      generation_state.current_applied = true;
      ++generation_state.applied_count;
    }
  }

  static auto hooked_generate_resources(void *engine) -> void {
    MLSetRegionResourceMod *mod = instance;
    if (mod == nullptr) {
      return;
    }

    const GenerationState previous_state = generation_state;
    generation_state = {.active = true};
    reinterpret_cast<GenerateResourcesFunction>(
        mod->generate_resources_trampoline)(engine);

    if (generation_state.applied_count != 0) {
      log_message(L"Preset applied to " +
                  std::to_wstring(generation_state.applied_count) +
                  L" configured region(s).");
      if (generation_state.any_failed) {
        log_message(L"At least one other region preset failed runtime "
                    L"validation.");
      }
    } else if (generation_state.any_failed) {
      log_message(L"Preset was not applied because a runtime validation "
                  L"failed.");
    } else if (generation_state.main_player_found) {
      log_message(L"No matching preset; generation remained fully vanilla.");
    } else {
      log_message(L"No main-player starting region was found during "
                  L"generation.");
    }
    generation_state = previous_state;
  }

  static auto hooked_prepare_region(void *region) -> void {
    MLSetRegionResourceMod *mod = instance;
    if (mod == nullptr) {
      return;
    }
    void *return_address = _ReturnAddress();
    reinterpret_cast<PrepareRegionFunction>(mod->prepare_region_trampoline)(
        region);
    if (generation_state.active &&
        return_address == mod->prepare_region_return) {
      mod->on_region_prepared(static_cast<UObject *>(region));
    }
  }

  static auto hooked_pick_rich(void *engine, NativeArray *candidates,
                               void *usage_counts) -> std::int32_t {
    MLSetRegionResourceMod *mod = instance;
    if (mod == nullptr) {
      return 0;
    }
    void *return_address = _ReturnAddress();
    const bool is_target = generation_state.active &&
                           !generation_state.current_failed &&
                           generation_state.preset != nullptr &&
                           return_address == mod->pick_rich_return &&
                           generation_state.current_region != nullptr;
    if (is_target) {
      return mod->force_rich_pick(engine, candidates, usage_counts);
    }
    return reinterpret_cast<PickRichFunction>(mod->pick_rich_trampoline)(
        engine, candidates, usage_counts);
  }

  static auto hooked_sort_resources(std::int32_t *candidates,
                                    const std::int32_t count,
                                    const std::uint8_t random_seed,
                                    void *rich_context) -> void {
    MLSetRegionResourceMod *mod = instance;
    if (mod == nullptr) {
      return;
    }
    void *return_address = _ReturnAddress();
    const bool is_target = generation_state.active &&
                           !generation_state.current_failed &&
                           generation_state.preset != nullptr &&
                           return_address == mod->sort_resources_return &&
                           generation_state.current_region != nullptr;
    if (is_target) {
      mod->replace_final_arrays(candidates, count, rich_context);
    }
    reinterpret_cast<SortResourcesFunction>(mod->sort_resources_trampoline)(
        candidates, count, random_seed, rich_context);
  }

  static auto hooked_get_resource_properties(
      void *settings, NativeResourceNodePropertiesPrefix *output,
      const std::int32_t node_type) -> NativeResourceNodePropertiesPrefix * {
    MLSetRegionResourceMod *mod = instance;
    if (mod == nullptr) {
      return output;
    }

    NativeResourceNodePropertiesPrefix *result =
        reinterpret_cast<GetResourcePropertiesFunction>(
            mod->get_resource_properties_trampoline)(settings, output,
                                                     node_type);
    const MLSetRegionResource::Preset *preset = generation_state.preset;
    if (!generation_state.active || generation_state.current_failed ||
        preset == nullptr || result == nullptr ||
        generation_state.current_region == nullptr) {
      return result;
    }

    const auto configured = std::ranges::find(
        preset->maximum_resources,
        static_cast<MLSetRegionResource::NodeType>(node_type));
    if (configured == preset->maximum_resources.end()) {
      return result;
    }

    result->minimum_resource_amount = result->maximum_resource_amount;
    result->minimum_rich_resource_amount = result->maximum_rich_resource_amount;
    return result;
  }
};
} // namespace

#define ML_STARTING_RESOURCES_API __declspec(dllexport)
BOOL APIENTRY DllMain(HMODULE module, const DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    mod_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}

extern "C" {
ML_STARTING_RESOURCES_API RC::CppUserModBase *start_mod() {
  return new MLSetRegionResourceMod();
}

ML_STARTING_RESOURCES_API void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
}

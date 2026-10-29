#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Config.hpp"

namespace MLSetRegionResource {
namespace {
struct RawSection {
  std::string name{};
  std::unordered_map<std::string, std::string> values{};
};

struct ScopeResult {
  bool fallback{};
  std::string map{};
  std::optional<std::string> region{};
  std::string error{};
};

struct PresetResult {
  std::optional<Preset> preset{};
  bool always{true};
  std::string error{};
};

auto trim(std::string_view value) -> std::string_view {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

auto lowercase(std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(),
                         [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                         });
  return result;
}

auto parse_node_type(std::string_view value) -> std::optional<NodeType> {
  static const std::unordered_map<std::string, NodeType> node_types{
      {"salt", NodeType::Salt},           {"iron", NodeType::Iron},
      {"clay", NodeType::Clay},           {"deer", NodeType::Deer},
      {"fish", NodeType::Fish},           {"berries", NodeType::Berries},
      {"stone", NodeType::Stone},         {"mushrooms", NodeType::Mushrooms},
      {"smallgame", NodeType::SmallGame}, {"eel", NodeType::Eel},
      {"fertility", NodeType::Fertility},
  };

  const auto node_type = node_types.find(lowercase(trim(value)));
  if (node_type == node_types.end()) {
    return std::nullopt;
  }
  return node_type->second;
}

auto parse_rich_types(std::string_view value, std::array<NodeType, 2> &output)
    -> std::string {
  std::vector<NodeType> parsed_types{};
  std::size_t start{};
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t end =
        comma == std::string_view::npos ? value.size() : comma;
    const std::string_view token = trim(value.substr(start, end - start));
    if (token.empty()) {
      return "Rich contains an empty item";
    }

    const std::optional<NodeType> node_type = parse_node_type(token);
    if (!node_type.has_value()) {
      return "Rich contains an unsupported type: " + std::string{token};
    }
    parsed_types.emplace_back(*node_type);

    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }

  if (parsed_types.size() != output.size()) {
    return "Rich must contain exactly two types";
  }
  if (parsed_types[0] == parsed_types[1]) {
    return "Rich types must be different";
  }

  std::ranges::copy(parsed_types, output.begin());
  return {};
}

auto read_sections(const std::filesystem::path &path)
    -> std::pair<std::vector<RawSection>, std::string> {
  std::ifstream input{path};
  if (!input) {
    return {{}, "Cannot open config file: " + path.string()};
  }

  std::vector<RawSection> sections{};
  std::unordered_set<std::string> section_names{};
  std::optional<std::size_t> current_section{};
  std::string line{};
  std::size_t line_number{};
  while (std::getline(input, line)) {
    ++line_number;
    std::string_view current = trim(line);
    if (current.empty() || current.front() == ';' || current.front() == '#') {
      continue;
    }

    if (current.front() == '[') {
      if (current.size() < 3 || current.back() != ']') {
        return {{}, "Malformed section at line " + std::to_string(line_number)};
      }
      const std::string name =
          lowercase(trim(current.substr(1, current.size() - 2)));
      if (name.empty()) {
        return {{}, "Empty section at line " + std::to_string(line_number)};
      }
      if (!section_names.emplace(name).second) {
        return {{},
                "Duplicate section at line " + std::to_string(line_number) +
                    ": " + name};
      }
      sections.push_back({.name = name});
      current_section = sections.size() - 1;
      continue;
    }

    if (!current_section.has_value()) {
      return {{},
              "Expected a section before line " + std::to_string(line_number)};
    }
    const std::size_t equals = current.find('=');
    if (equals == std::string_view::npos) {
      return {{}, "Expected key=value at line " + std::to_string(line_number)};
    }

    const std::string key = lowercase(trim(current.substr(0, equals)));
    std::string_view raw_value = trim(current.substr(equals + 1));
    const std::size_t comment = raw_value.find_first_of(";#");
    if (comment != std::string_view::npos) {
      raw_value = trim(raw_value.substr(0, comment));
    }
    if (key.empty() || raw_value.empty()) {
      return {{}, "Empty key or value at line " + std::to_string(line_number)};
    }
    auto &values = sections[*current_section].values;
    if (!values.emplace(key, raw_value).second) {
      return {{},
              "Duplicate key at line " + std::to_string(line_number) + ": " +
                  key};
    }
  }
  return {std::move(sections), {}};
}

auto parse_scope(const std::string_view section) -> ScopeResult {
  if (section == "user") {
    return {.fallback = true};
  }

  constexpr std::string_view map_prefix{"map."};
  if (!section.starts_with(map_prefix)) {
    return {.error = "Unsupported section: " + std::string{section}};
  }
  const std::string_view remainder = section.substr(map_prefix.size());
  constexpr std::string_view region_separator{".region."};
  const std::size_t separator = remainder.find(region_separator);
  if (separator == std::string_view::npos) {
    if (remainder.empty()) {
      return {.error = "Map name is empty in section: " + std::string{section}};
    }
    return {.map = std::string{remainder}};
  }

  const std::string_view map = remainder.substr(0, separator);
  const std::string_view region =
      remainder.substr(separator + region_separator.size());
  if (map.empty() || region.empty() ||
      region.find(region_separator) != std::string_view::npos) {
    return {.error =
                "Expected [Map.<name>.Region.<name>]: " + std::string{section}};
  }
  return {.map = std::string{map}, .region = std::string{region}};
}

auto parse_preset(const RawSection &section, const bool is_region)
    -> PresetResult {
  static constexpr std::array<std::string_view, 4> allowed_keys{
      "gatherable", "wildlifeorwater", "primarydeposit", "rich"};
  Preset preset{};
  for (const auto &[key, value] : section.values) {
    if (std::ranges::find(allowed_keys, key) != allowed_keys.end() ||
        (is_region && key == "always")) {
      continue;
    }

    constexpr std::string_view max_prefix{"max."};
    if (!key.starts_with(max_prefix)) {
      return {.error = "Unknown key in [" + section.name + "]: " + key};
    }

    const std::optional<NodeType> node_type =
        parse_node_type(std::string_view{key}.substr(max_prefix.size()));
    if (!node_type.has_value() || *node_type == NodeType::Fertility) {
      return {.error = "Unsupported Max resource in [" + section.name +
                       "]: " + key.substr(max_prefix.size())};
    }

    const std::string normalized = lowercase(trim(value));
    if (normalized == "yes") {
      preset.maximum_resources.push_back(*node_type);
    } else if (normalized != "no") {
      return {.error = key + " must be yes or no in [" + section.name + "]"};
    }
  }

  bool always = true;
  if (const auto value = section.values.find("always");
      value != section.values.end()) {
    const std::string normalized = lowercase(trim(value->second));
    if (normalized == "yes") {
      always = true;
    } else if (normalized == "no") {
      always = false;
    } else {
      return {.error = "Always must be yes or no in [" + section.name + "]"};
    }
  }

  if (const auto gatherable = section.values.find("gatherable");
      gatherable != section.values.end()) {
    preset.gatherable = parse_node_type(gatherable->second);
    if (!preset.gatherable.has_value() ||
        (*preset.gatherable != NodeType::Berries &&
         *preset.gatherable != NodeType::Mushrooms)) {
      return {.error = "Gatherable must be Berries or Mushrooms in [" +
                       section.name + "]"};
    }
  }

  if (const auto wildlife = section.values.find("wildlifeorwater");
      wildlife != section.values.end()) {
    preset.wildlife_or_water = parse_node_type(wildlife->second);
    if (!preset.wildlife_or_water.has_value() ||
        (*preset.wildlife_or_water != NodeType::Fish &&
         *preset.wildlife_or_water != NodeType::SmallGame)) {
      return {.error = "WildlifeOrWater must be Fish or SmallGame in [" +
                       section.name + "]"};
    }
  }

  if (const auto deposit = section.values.find("primarydeposit");
      deposit != section.values.end()) {
    preset.primary_deposit = parse_node_type(deposit->second);
    if (!preset.primary_deposit.has_value() ||
        (*preset.primary_deposit != NodeType::Salt &&
         *preset.primary_deposit != NodeType::Iron)) {
      return {.error = "PrimaryDeposit must be Salt or Iron in [" +
                       section.name + "]"};
    }
  }

  const auto rich = section.values.find("rich");
  if (rich == section.values.end()) {
    return {.error = "Every preset requires Rich in [" + section.name + "]"};
  }
  if (std::string rich_error = parse_rich_types(rich->second, preset.rich);
      !rich_error.empty()) {
    return {.error = std::move(rich_error) + " in [" + section.name + "]"};
  }

  const auto is_guaranteed = [&preset](const NodeType type) {
    switch (type) {
    case NodeType::Clay:
    case NodeType::Stone:
    case NodeType::Fertility:
      return true;
    case NodeType::Deer:
    case NodeType::Eel:
      return false;
    case NodeType::Salt:
    case NodeType::Iron:
      return preset.primary_deposit == type;
    case NodeType::Fish:
    case NodeType::SmallGame:
      return preset.wildlife_or_water == type;
    case NodeType::Berries:
    case NodeType::Mushrooms:
      return preset.gatherable == type;
    }
    return false;
  };
  for (const NodeType rich_type : preset.rich) {
    if (!is_guaranteed(rich_type)) {
      return {.error = "Rich type is not guaranteed by this partial preset: " +
                       std::string{node_type_name(rich_type)} + " in [" +
                       section.name + "]"};
    }
  }
  std::ranges::sort(preset.maximum_resources);
  return {.preset = preset, .always = always};
}
} // namespace

auto node_type_name(const NodeType node_type) -> std::string_view {
  switch (node_type) {
  case NodeType::Salt:
    return "Salt";
  case NodeType::Iron:
    return "Iron";
  case NodeType::Clay:
    return "Clay";
  case NodeType::Deer:
    return "Deer";
  case NodeType::Fish:
    return "Fish";
  case NodeType::Berries:
    return "Berries";
  case NodeType::Stone:
    return "Stone";
  case NodeType::Mushrooms:
    return "Mushrooms";
  case NodeType::SmallGame:
    return "SmallGame";
  case NodeType::Eel:
    return "Eel";
  case NodeType::Fertility:
    return "Fertility";
  }
  return "Unknown";
}

auto Config::select(const std::string_view map, const std::string_view region,
                    const bool is_main_player_region) const -> const Preset * {
  const std::string normalized_map = lowercase(trim(map));
  const std::string normalized_region = lowercase(trim(region));
  for (const ScopedPreset &scoped : scoped_presets) {
    if (scoped.region.has_value() && scoped.map == normalized_map &&
        *scoped.region == normalized_region) {
      return scoped.always || is_main_player_region ? &scoped.preset : nullptr;
    }
  }
  if (!is_main_player_region) {
    return nullptr;
  }
  for (const ScopedPreset &scoped : scoped_presets) {
    if (!scoped.region.has_value() && scoped.map == normalized_map) {
      return &scoped.preset;
    }
  }
  return fallback.has_value() ? &*fallback : nullptr;
}

auto load_config(const std::filesystem::path &path) -> ConfigResult {
  auto [sections, read_error] = read_sections(path);
  if (!read_error.empty()) {
    return {.error = std::move(read_error)};
  }
  if (sections.empty()) {
    return {.error = "Config contains no preset sections"};
  }

  Config config{};
  for (const RawSection &section : sections) {
    ScopeResult scope = parse_scope(section.name);
    if (!scope.error.empty()) {
      return {.error = std::move(scope.error)};
    }
    PresetResult parsed = parse_preset(section, scope.region.has_value());
    if (!parsed.preset.has_value()) {
      return {.error = std::move(parsed.error)};
    }

    if (scope.fallback) {
      config.fallback = std::move(*parsed.preset);
    } else {
      config.scoped_presets.push_back({.map = std::move(scope.map),
                                       .region = std::move(scope.region),
                                       .preset = std::move(*parsed.preset),
                                       .always = parsed.always});
    }
  }
  return {.config = std::move(config)};
}
} // namespace MLSetRegionResource

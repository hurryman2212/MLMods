#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MLSetRegionResource {
enum class NodeType : std::int32_t {
  Salt = 1,
  Iron = 2,
  Clay = 3,
  Deer = 4,
  Fish = 5,
  Berries = 6,
  Stone = 7,
  Mushrooms = 9,
  SmallGame = 10,
  Eel = 11,
  Fertility = 12,
};

struct Preset {
  std::optional<NodeType> gatherable{};
  std::optional<NodeType> wildlife_or_water{};
  std::optional<NodeType> primary_deposit{};
  std::array<NodeType, 2> rich{};
  std::vector<NodeType> maximum_resources{};
};

struct ScopedPreset {
  std::string map{};
  std::optional<std::string> region{};
  Preset preset{};
  bool always{true};
};

struct Config {
  std::optional<Preset> fallback{};
  std::vector<ScopedPreset> scoped_presets{};

  [[nodiscard]] auto select(std::string_view map, std::string_view region,
                            bool is_main_player_region) const -> const Preset *;
};

struct ConfigResult {
  std::optional<Config> config{};
  std::string error{};
};

auto load_config(const std::filesystem::path &path) -> ConfigResult;
auto node_type_name(NodeType node_type) -> std::string_view;
} // namespace MLSetRegionResource

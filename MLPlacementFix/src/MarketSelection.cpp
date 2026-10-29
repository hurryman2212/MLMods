#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "MarketSelection.hpp"

static constexpr std::int32_t maximum_selection_cells = 1048576;
static constexpr std::int32_t maximum_cell_buildings = 100000;
static constexpr std::int32_t maximum_selection_vertices = 65536;

static std::uintptr_t selection_image{};
static MarketSelectionApi selection_api{};

struct MarketSelectionIdentity {
  std::uintptr_t address{};
  std::array<std::uint8_t, 24> header{};
};

template <class T>
static bool selection_read(std::uintptr_t address, std::size_t offset,
                           T &value) noexcept {
  return selection_api.read_memory && address != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - address &&
         sizeof(T) <=
             std::numeric_limits<std::uintptr_t>::max() - address - offset &&
         selection_api.read_memory(address + offset, &value, sizeof(value));
}

static bool selection_identity(std::uintptr_t address,
                               MarketSelectionIdentity &identity) noexcept {
  identity.address = address;
  if (address % alignof(void *) != 0 ||
      !selection_read(address, 0, identity.header))
    return false;
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::memcpy(&vtable, identity.header.data(), sizeof(vtable));
  std::memcpy(&flags, identity.header.data() + 8, sizeof(flags));
  std::memcpy(&index, identity.header.data() + 12, sizeof(index));
  std::memcpy(&object_class, identity.header.data() + 16, sizeof(object_class));
  return vtable >= selection_image && vtable - selection_image < 0x09dfd000 &&
         (flags & 0x40018010U) == 0 && index > 0 &&
         object_class % alignof(void *) == 0 &&
         selection_read(object_class, 0, class_vtable) &&
         class_vtable >= selection_image &&
         class_vtable - selection_image < 0x09dfd000;
}

static bool selection_array(const MarketSelectionArray &array,
                            std::int32_t maximum, std::size_t stride) noexcept {
  return array.count >= 0 && array.capacity >= array.count &&
         array.capacity <= maximum &&
         (array.count == 0 ||
          (array.data != 0 && array.data % alignof(void *) == 0 &&
           static_cast<std::size_t>(array.count) <=
               (std::numeric_limits<std::uintptr_t>::max() - array.data) /
                   stride));
}

void configure_market_selection(std::uintptr_t image,
                                const MarketSelectionApi &api) noexcept {
  selection_image = image;
  selection_api = api;
}

void *market_selection_target(void *pawn_pointer, void *accepted) noexcept {
  if (!selection_image ||
      selection_image >
          std::numeric_limits<std::uintptr_t>::max() - 0x09dfd000 ||
      !selection_api.read_memory || !selection_api.contains ||
      !selection_api.cell_index || !pawn_pointer || !accepted)
    return accepted;
  try {
    const auto pawn = reinterpret_cast<std::uintptr_t>(pawn_pointer);
    const auto market = reinterpret_cast<std::uintptr_t>(accepted);
    std::array<MarketSelectionIdentity, 6> identities;
    if (!selection_identity(pawn, identities[0]) ||
        !selection_identity(market, identities[1]))
      return accepted;
    std::int32_t market_type{};
    std::uint8_t market_function{};
    if (!selection_read(market, 0x3b8, market_type) || market_type != 13 ||
        !selection_read(market, 0x3cc, market_function) ||
        market_function != 15)
      return accepted;

    constexpr std::array<std::size_t, 5> mode_offsets{0x560, 0x7a0, 0xe79,
                                                      0xa59, 0xb20};
    for (const auto offset : mode_offsets) {
      std::uint8_t mode{};
      if (!selection_read(pawn, offset, mode) || mode != 0)
        return accepted;
    }
    std::uintptr_t castle{}, master{}, market_master{}, owner{}, region{},
        grid{}, family_assignment{};
    MarketSelectionVec3 cursor{};
    if (!selection_read(pawn, 0x840, castle) || castle != 0 ||
        !selection_read(pawn, 0x340, master) ||
        !selection_identity(master, identities[2]) ||
        !selection_read(market, 0x2e8, market_master) ||
        market_master != master || !selection_read(market, 0x2e0, owner) ||
        owner != pawn || !selection_read(market, 0x2c8, region) ||
        !selection_identity(region, identities[3]) ||
        !selection_read(master, 0x2c0, grid) ||
        !selection_identity(grid, identities[4]) ||
        !selection_read(pawn, 0x1188, family_assignment) ||
        !selection_read(pawn, 0x4e8, cursor) || !std::isfinite(cursor.x) ||
        !std::isfinite(cursor.y) || !std::isfinite(cursor.z))
      return accepted;

    MarketSelectionArray cells{};
    if (!selection_read(grid, 0x438, cells) ||
        !selection_array(cells, maximum_selection_cells, 0x48))
      return accepted;
    const auto cell_index =
        selection_api.cell_index(reinterpret_cast<void *>(grid), &cursor);
    if (cell_index < 0 || cell_index >= cells.count)
      return accepted;
    const auto cell = cells.data + static_cast<std::size_t>(cell_index) * 0x48;
    MarketSelectionArray candidates{};
    if (!selection_read(cell, 0x10, candidates) ||
        !selection_array(candidates, maximum_cell_buildings,
                         sizeof(std::uintptr_t)))
      return accepted;
    std::vector<std::uintptr_t> buildings(
        static_cast<std::size_t>(candidates.count));
    if (!buildings.empty() &&
        !selection_api.read_memory(candidates.data, buildings.data(),
                                   buildings.size() * sizeof(std::uintptr_t)))
      return accepted;
    if (std::find(buildings.begin(), buildings.end(), market) ==
        buildings.end())
      return accepted;

    // Preserve the game's reverse ordering between actual stalls. Only the
    // broad marketplace area loses priority to one of its own eligible stalls.
    for (auto it = buildings.rbegin(); it != buildings.rend(); ++it) {
      const auto stall = *it;
      if (stall == market)
        continue;
      std::uint8_t function{};
      if (!selection_identity(stall, identities[5]) ||
          !selection_read(stall, 0x3cc, function))
        return accepted;
      if (function != 3)
        continue;
      std::int32_t stall_type{};
      if (!selection_read(stall, 0x3b8, stall_type))
        return accepted;
      // Both pinned 0.8.117 food/common-goods definitions have category 3,
      // not the native category 9/13 candidates restricted to cheat mode.
      if (stall_type != 91 && stall_type != 93)
        continue;
      std::uintptr_t area_master{}, stall_owner{}, stall_master{},
          stall_region{};
      if (!selection_read(stall, 0xca8, area_master) ||
          !selection_read(stall, 0x2e0, stall_owner) ||
          !selection_read(stall, 0x2e8, stall_master) ||
          !selection_read(stall, 0x2c8, stall_region))
        return accepted;
      if (area_master != market || stall_owner != owner ||
          stall_master != master || stall_region != region)
        continue;
      if (family_assignment != 0) {
        std::int32_t occupants{};
        if (!selection_api.max_workers ||
            !selection_read(stall, 0x1030, occupants) || occupants < 0)
          return accepted;
        if (occupants >=
            selection_api.max_workers(reinterpret_cast<void *>(stall)))
          continue;
      }
      std::array<double, 4> bounds{};
      MarketSelectionArray triangles{};
      if (!selection_read(stall, 0x6c8, bounds) ||
          !std::all_of(bounds.begin(), bounds.end(),
                       [](double value) { return std::isfinite(value); }) ||
          !selection_read(stall, 0x678, triangles) ||
          !selection_array(triangles, maximum_selection_vertices,
                           sizeof(MarketSelectionVec3)) ||
          triangles.count < 3 || triangles.count % 3 != 0)
        return accepted;
      // Exact native strict bounds; touching the edge is not inside.
      if (!(bounds[0] < cursor.x && cursor.x < bounds[2] &&
            bounds[1] < cursor.y && cursor.y < bounds[3]))
        continue;
      std::vector<MarketSelectionVec3> vertices(
          static_cast<std::size_t>(triangles.count));
      if (!selection_api.read_memory(triangles.data, vertices.data(),
                                     vertices.size() * sizeof(vertices[0])) ||
          !std::all_of(
              vertices.begin(), vertices.end(), [](const auto &vertex) {
                return std::isfinite(vertex.x) && std::isfinite(vertex.y) &&
                       std::isfinite(vertex.z);
              }))
        return accepted;
      // Pass a validated local snapshot to the original geometric predicate.
      const MarketSelectionArray local_triangles{
          reinterpret_cast<std::uintptr_t>(vertices.data()), triangles.count,
          triangles.count};
      if (!selection_api.contains(&cursor, &local_triangles))
        continue;
      for (const auto &identity : identities) {
        MarketSelectionIdentity current{};
        if (!selection_identity(identity.address, current) ||
            current.header != identity.header)
          return accepted;
      }
      MarketSelectionVec3 current_cursor{};
      MarketSelectionArray current_cells{}, current_candidates{},
          current_triangles{};
      std::uintptr_t current_area{};
      if (!selection_read(pawn, 0x4e8, current_cursor) ||
          current_cursor.x != cursor.x || current_cursor.y != cursor.y ||
          current_cursor.z != cursor.z ||
          !selection_read(grid, 0x438, current_cells) ||
          current_cells.data != cells.data ||
          current_cells.count != cells.count ||
          current_cells.capacity != cells.capacity ||
          !selection_read(cell, 0x10, current_candidates) ||
          current_candidates.data != candidates.data ||
          current_candidates.count != candidates.count ||
          current_candidates.capacity != candidates.capacity ||
          !selection_read(stall, 0x678, current_triangles) ||
          current_triangles.data != triangles.data ||
          current_triangles.count != triangles.count ||
          current_triangles.capacity != triangles.capacity ||
          !selection_read(stall, 0xca8, current_area) || current_area != market)
        return accepted;
      return reinterpret_cast<void *>(stall);
    }
  } catch (...) {
    // Allocation failures and native validation failures preserve selection.
  }
  return accepted;
}

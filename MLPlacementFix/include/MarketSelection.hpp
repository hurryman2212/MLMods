#pragma once

#include <cstddef>
#include <cstdint>

struct MarketSelectionVec3 {
  double x{}, y{}, z{};
};

struct MarketSelectionArray {
  std::uintptr_t data{};
  std::int32_t count{}, capacity{};
};

struct MarketSelectionApi {
  bool (*read_memory)(std::uintptr_t, void *, std::size_t) noexcept {};
  bool (*contains)(const MarketSelectionVec3 *, const MarketSelectionArray *){};
  std::int32_t (*cell_index)(void *, const MarketSelectionVec3 *){};
  std::int32_t (*max_workers)(void *){};
};

void configure_market_selection(std::uintptr_t image,
                                const MarketSelectionApi &api) noexcept;

// Chooses only among the accepted marketplace's own stalls in the original
// hover cell. Never changes selection flags, arrays, geometry or saved data.
void *market_selection_target(void *pawn, void *accepted) noexcept;

#pragma once

#include <cstddef>
#include <cstdint>

using CastlePreviewNative = void (*)(void *, std::uint8_t);
using CastlePreviewRead = bool (*)(std::uintptr_t, void *, std::size_t);
using CastlePreviewClock = std::uint64_t (*)();
using CastlePreviewButtons = bool (*)();
using CastlePreviewAssignWeak = void (*)(std::uint64_t *, void *);

struct CastlePreviewMetrics {
  std::uint64_t calls{};
  std::uint64_t reused{};
  std::uint64_t native{};
  std::uint64_t flushed{};
  std::uint64_t ineligible{};
  std::uint64_t changed{};
  std::uint64_t expired{};
  std::uint64_t overlap{};
  std::uint64_t buttons{};
  std::uint64_t periodic{};
  std::uint64_t deferred{};
  std::uint64_t max_periodic_per_pass{};
};

void configure_castle_preview(std::uintptr_t image) noexcept;
void configure_castle_preview_functions(
    CastlePreviewNative native, CastlePreviewClock clock = nullptr,
    CastlePreviewButtons buttons = nullptr,
    CastlePreviewAssignWeak weak = nullptr,
    CastlePreviewRead read = nullptr) noexcept;

// The verified sibling-loop callsite supplies the pawn and current/end array
// iterators. After a full native pass, periodic refreshes are spread across
// sibling passes; each sweep completes before another refresh window starts.
void castle_preview_regenerate(void *road, std::uint8_t force, void *pawn,
                               std::uintptr_t iterator, std::uintptr_t end);

// Invalidates every thread's cache, including after a load/session transition.
void castle_preview_reset() noexcept;
// Called immediately before accepted placement, while the active preview still
// exists. Rebuilds valid siblings with reused or pending native work.
void castle_preview_flush(void *pawn) noexcept;
CastlePreviewMetrics castle_preview_metrics() noexcept;

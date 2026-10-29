#pragma once

#include <cstddef>
#include <cstdint>

struct EraserVector {
  double x{};
  double y{};
  double z{};
};

struct EraserArray {
  void *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct EraserApi {
  void (*queue_tree_removal)(void *, const void *){};
};

static_assert(sizeof(EraserVector) == 24);
static_assert(sizeof(EraserArray) == 16);

extern std::uintptr_t eraser_image;

auto configure_eraser_api(const EraserApi &api) noexcept -> void;
auto eraser_allow_tree(void *pawn, void *component) noexcept -> bool;
auto eraser_cleanup(void *pawn, void *component, const EraserArray *removed,
                    bool success) noexcept -> void;

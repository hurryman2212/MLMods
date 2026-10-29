#pragma once

#include <cstddef>
#include <cstdint>

struct TreeVector {
  double x{};
  double y{};
  double z{};
};

struct TreeArray {
  void *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

struct TreeApi {
  void *(*region_at)(void *, const TreeVector *){};
};

static_assert(sizeof(TreeVector) == 24);
static_assert(sizeof(TreeArray) == 16);

extern std::uintptr_t tree_image;

auto configure_tree_api(const TreeApi &api) noexcept -> void;
auto tree_select_region(void *unit, void *original) noexcept -> void *;
auto tree_component_region(void *unit, void *component, void *original) noexcept
    -> void *;

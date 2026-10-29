#pragma once

#include <cstddef>
#include <cstdint>

struct ManorLoadApi {
  bool (*read_memory)(std::uintptr_t, void *, std::size_t) noexcept {};
};

struct ManorLoadArray {
  void *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

static_assert(sizeof(ManorLoadArray) == 16);

extern std::uintptr_t manor_load_image;

auto configure_manor_load_api(const ManorLoadApi &api) noexcept -> void;
auto manor_load_begin(void *master, void *manor, void *saved_building) noexcept
    -> const ManorLoadArray *;
auto manor_load_end(void *manor) noexcept -> void;
auto manor_load_plan_active() noexcept -> bool;
auto manor_load_restore_saved(void *record, void *manor,
                              void *definition) noexcept -> bool;

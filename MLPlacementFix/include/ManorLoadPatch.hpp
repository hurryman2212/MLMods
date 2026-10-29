#pragma once

#include <cstdint>
#include <vector>

auto manor_load_bridge(const void *callback, const void *direct,
                       const void *assembly) -> std::vector<std::uint8_t>;
auto manor_load_begin_bridge(const void *callback, const void *continuation)
    -> std::vector<std::uint8_t>;
auto manor_load_end_bridge(const void *callback, const void *continuation)
    -> std::vector<std::uint8_t>;

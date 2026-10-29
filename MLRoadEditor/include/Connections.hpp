#pragma once

#include <cstdint>

#include "Geometry.hpp"

using ConnectionNavPoint = std::uint8_t (*)(void *, const Vec3 *, float);
using ConnectionCachePoint = std::uint8_t (*)(void *, const Vec3 *,
                                              std::int32_t, std::uint64_t);
using ConnectionCircle = std::uint8_t (*)(const Vec3 *, float,
                                          const NativeArray<Vec3> *);
using ConnectionAssignWeak = void (*)(std::uint64_t *, void *);

void configure_connections(std::uintptr_t image) noexcept;
void configure_connection_functions(ConnectionNavPoint nav,
                                    ConnectionCachePoint cache,
                                    ConnectionCircle circle,
                                    ConnectionAssignWeak weak) noexcept;
void *resolve_connection_manor(void *building, void *nav = nullptr,
                               void *cache = nullptr) noexcept;
std::uint8_t connections_initial_point(void *nav, const Vec3 *point,
                                       float radius, void *building);
std::uint8_t connections_restore_point(void *cache, const Vec3 *point,
                                       std::int32_t radius,
                                       std::uint64_t ignored, void *building);
std::uint8_t connections_mask_circle(const Vec3 *point, float radius,
                                     const NativeArray<Vec3> *triangles,
                                     void *masking_building);
std::uint8_t connections_nav_circle(const Vec3 *point, float radius,
                                    const NativeArray<Vec3> *triangles);

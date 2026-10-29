#pragma once

#include <cstdint>

#include "Geometry.hpp"

void configure_road_placement(std::uintptr_t image) noexcept;
bool road_use_normal_obstacle_path(void *pawn) noexcept;
std::uint64_t road_normal_path_count() noexcept;

// RoadTick's four-argument path search appends the acting pawn as argument
// five. Its nested native probes use the same road-tool policy through a scoped
// context; other callers retain the original behavior.
NativeArray<Vec3> *road_path_query(void *cache, NativeArray<Vec3> *output,
                                   const Vec3 *start, const Vec3 *end,
                                   void *pawn);
std::uint8_t road_path_point_collision(void *cache, const Vec3 *point,
                                       std::int32_t radius,
                                       std::uint64_t ignored_weak_actor);
std::uint8_t road_path_segment_collision(void *cache, const double *start_xy,
                                         const double *end_xy, float half_width,
                                         void *ignored_actor,
                                         std::uint8_t check_flag);

// Only the six verified APawnCPP road-preview callsites use these bridges.
// The bridge supplies RDI (the pawn) as the final Windows x64 argument.
std::uint8_t road_endpoint_collision(void *game, const Vec3 *point,
                                     float radius, void *ignored_actor,
                                     void *pawn);
std::uint8_t road_point_collision(void *cache, const Vec3 *point,
                                  std::int32_t radius,
                                  std::uint64_t ignored_weak_actor, void *pawn);
std::uint8_t road_segment_collision(void *cache, const double *start_xy,
                                    const double *end_xy, float half_width,
                                    void *ignored_actor,
                                    std::uint8_t check_flag, void *pawn);

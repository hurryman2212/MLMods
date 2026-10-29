#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "Geometry.hpp"

// Owned only by one immutable snapshot's synchronous query/worker scope.
// Keys come from the same validated coordinate copies used for evaluation.
using PlacementSegmentMemo =
    std::map<std::array<std::uint64_t, 4>, GeometryVerdict>;

// Resolve only the prospective region whose native spatial cache is being used.
// Castle-planning mode is deliberately excluded from courtyard editing.
auto resolve_placement_manor(void *cache, void *pawn) noexcept -> void *;

// These callbacks run only inside the two native spatial-cache loops. Outside a
// scoped player placement query they never suppress any record.
auto placement_skip_record(void *cache, std::uint32_t actor_index) noexcept
    -> bool;

// The original consumes and frees the temporary ignored-actors array. This
// wrapper must call it exactly once, including when module geometry blocks.
auto placement_collision(void *cache, NativeArray<Vec3> *triangles,
                         const double *bounds, NativeArray<void *> *ignored,
                         std::uint8_t skip_segments, void *pawn) noexcept
    -> bool;

// The polygon tool tests its first point and first edge before triangulation.
auto placement_segment_collision(void *cache, const double *start,
                                 const double *end, void *pawn) noexcept
    -> bool;
// Shared by player previews and the native derived snap-graph builder. Callers
// must resolve an eligible Manor; nullptr retains the unmodified native query.
auto placement_segment_for_manor(
    void *cache, const double *start, const double *end, void *manor,
    const ManorGeometry *snapshot = nullptr,
    PlacementSegmentMemo *segment_memo = nullptr) noexcept -> bool;
auto placement_initial_point(void *game, const Vec3 *point, void *pawn) noexcept
    -> bool;
auto placement_initial_point_bounds(void *building, const Vec3 *point) noexcept
    -> bool;

#pragma once

#include <cstdint>

#include "Geometry.hpp"

using SnapPathQuery = NativeArray<Vec3> *(*)(void *, NativeArray<Vec3> *,
                                             void *, float, std::uint8_t);
using SnapSegmentQuery = bool (*)(void *, const double *, const double *);
using SnapGraphBuild = std::uintptr_t (*)(void *);
using SnapWeakCacheGet = void *(*)(const std::uint64_t *);

// Configure before installing the verified callsite hooks. The function-pointer
// form permits offline ABI/scope tests without calling any game code.
void configure_snap_query_functions(SnapPathQuery path,
                                    SnapSegmentQuery segment) noexcept;
void configure_snap_graph_functions(SnapGraphBuild build,
                                    SnapWeakCacheGet weak_cache) noexcept;
void configure_snap_queries(std::uintptr_t image) noexcept;

// The verified projectField callsite supplies RDI (the acting pawn) as arg6.
// All native arguments, output storage, and the original return are preserved.
NativeArray<Vec3> *snap_project_path(void *cache, NativeArray<Vec3> *output,
                                     void *plot_points, float threshold,
                                     std::uint8_t mode, void *pawn);

// Five native path-search segment calls use this identical three-argument ABI.
// Only a scoped, ordinary player plot query may use Manor-aware collision.
bool snap_segment_collision(void *cache, const double *start,
                            const double *end);

// Only four verified segment checks in the native snap-graph builder use this
// wrapper. The native builder owns/rebuilds its unsaved derived graph; this mod
// never edits the Manor's persistent boundary or the graph's suppression flags.
// Bridges supply both value-node pointers; two scoped Manor creators preserve
// the native boundary query, while other valid candidates use module geometry.
bool snap_graph_segment_collision(void *cache, const double *start,
                                  const double *end, const void *node_a,
                                  const void *node_b);

// The verified worker thunk has already adjusted RCX to its work payload.
// One immutable geometry snapshot is owned only for this batch invocation.
std::uintptr_t snap_build_graph(void *work);

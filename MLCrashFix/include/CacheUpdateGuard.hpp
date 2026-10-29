#pragma once

#include <cstddef>
#include <cstdint>

struct CacheUpdateGuardCallbacks {
  bool (*busy)(void *);
  void (*drain)(void *);
  int (*count)(void *);
};

struct CacheUpdateGuardCounters {
  std::uint64_t deferred;
  std::uint64_t executed;
  std::uint64_t graph_deferred;
};

// Test setup only: callers must be quiescent. Resets counters, rejects missing
// callbacks, and is permanently disabled after a native hook is published.
auto configure_cache_update_guard_for_testing(
    CacheUpdateGuardCallbacks callbacks) -> bool;

auto guarded_cache_update(void *cache) -> void;
auto guarded_cache_graph_count(void *cache) -> int;
auto cache_update_guard_counters() -> CacheUpdateGuardCounters;

// Installation and removal run serially on the mod lifecycle thread. Published
// code and callback targets remain valid for calls already in flight on
// removal.
auto install_cache_update_guard(std::uint8_t *image, std::size_t size) -> bool;
auto uninstall_cache_update_guard() -> bool;

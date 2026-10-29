#pragma once

#include <cstdint>

struct ConnectionReloadApi {
  void (*restore_blocked)(void *){};
  void (*register_road)(void *){};
};

struct ConnectionReloadStats {
  std::uint64_t regions{};
  std::uint64_t buildings{};
  std::uint64_t recovered_points{};
  std::uint64_t roads{};
  // Unique eligible caches receiving queued recovery, never ticks performed.
  std::uint64_t caches{};
  std::uint64_t skipped{};
};

void configure_connection_reload(std::uintptr_t image) noexcept;
void configure_connection_reload_functions(
    const ConnectionReloadApi &api) noexcept;
// Restore entrances and queue native road work only. The outer loader must
// finish its obstacle queue before the ordinary engine tick starts graph work.
ConnectionReloadStats connections_after_load(void *master) noexcept;

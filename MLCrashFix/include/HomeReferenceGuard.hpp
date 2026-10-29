#pragma once

#include <cstddef>
#include <cstdint>

struct HomeReferenceGuardCounters {
  std::uint64_t repaired;
  std::uint64_t skipped;
};

// Testing setup requires quiescent callers and is disabled after publication.
auto configure_home_reference_guard_for_testing(std::uint8_t *image,
                                                std::size_t size) -> bool;
auto guarded_home_reference(void *unit) -> void *;
auto home_reference_guard_counters() -> HomeReferenceGuardCounters;

// Install before gameplay and remove only after gameplay has stopped.
// Published stubs and their callback module remain pinned for outstanding
// calls.
auto install_home_reference_guard(std::uint8_t *image, std::size_t size)
    -> bool;
auto remove_home_reference_guard() -> bool;

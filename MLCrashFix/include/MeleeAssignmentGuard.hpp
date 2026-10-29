#pragma once

#include <cstddef>
#include <cstdint>

struct MeleeAssignmentArray {
  void *data;
  std::int32_t count;
  std::int32_t capacity;
};

struct MeleeAssignmentPoint {
  double x;
  double y;
  double z;
};

struct MeleeAssignmentGuardCallbacks {
  MeleeAssignmentArray *(*match)(MeleeAssignmentArray *,
                                 const MeleeAssignmentArray *,
                                 const MeleeAssignmentArray *, bool);
  void (*reserve_ints)(MeleeAssignmentArray *, std::int32_t);
  void (*free_memory)(void *);
};

struct MeleeAssignmentGuardCounters {
  std::uint64_t passed_through;
  std::uint64_t transposed;
  std::uint64_t reassigned;
  std::uint64_t rejected;
};

// Testing setup requires quiescent callers and is disabled after publication.
auto configure_melee_assignment_guard_for_testing(
    MeleeAssignmentGuardCallbacks callbacks) -> bool;
auto guarded_melee_assignment(MeleeAssignmentArray *output,
                              const MeleeAssignmentArray *rows,
                              const MeleeAssignmentArray *columns, bool bias)
    -> MeleeAssignmentArray *;
auto melee_assignment_guard_counters() -> MeleeAssignmentGuardCounters;

// Install before gameplay and remove only after gameplay has stopped. The
// native E8 displacement is unaligned, so publication is not an atomic patch.
// Published stubs and callback targets are retained for outstanding calls.
auto install_melee_assignment_guard(std::uint8_t *image, std::size_t size)
    -> bool;
auto remove_melee_assignment_guard() -> bool;

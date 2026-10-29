#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class DiagnosticEvent {
  State,
  FollowQueued,
  FollowStopped,
  BonusCommitted
};

struct EventHistory {
  bool seen{};
  std::uint64_t signature{};
  std::uint64_t emitted_at{};
  std::uint64_t suppressed{};
};

struct HoundHistory {
  std::array<EventHistory, 4> events{};
  std::uint64_t bonuses{};
};

struct LogDecision {
  bool emit{};
  std::uint64_t suppressed{};
  std::uint64_t bonuses{};
};

inline auto decide_log(HoundHistory &history, DiagnosticEvent event,
                       std::uint64_t signature, std::uint64_t now_ms)
    -> LogDecision {
  if (event == DiagnosticEvent::BonusCommitted) {
    // Identical hound/camp/owner combinations can earn consecutive real
    // bonuses.
    return {true, 0, ++history.bonuses};
  }
  auto &previous = history.events[static_cast<std::size_t>(event)];
  // Neutral status heartbeat: 60 s. Identical start/stop events: coalesce 5 s.
  // Neither interval is a timeout or a claim that a paused hound is stuck.
  const std::uint64_t interval = event == DiagnosticEvent::State ? 60000 : 5000;
  if (previous.seen && previous.signature == signature &&
      now_ms >= previous.emitted_at &&
      now_ms - previous.emitted_at < interval) {
    ++previous.suppressed;
    return {false, previous.suppressed, history.bonuses};
  }
  const auto suppressed = previous.suppressed;
  previous = {true, signature, now_ms, 0};
  return {true, suppressed, history.bonuses};
}

#include "phasegap/stats/timer.hpp"

namespace phasegap::stats {

std::uint64_t MonotonicMicrosNow() {
  const auto now = SteadyClock::now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

double DurationMicros(TimePoint start, TimePoint end) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

void IterationTimer::BeginIteration() {
  timing_ = {};
  iter_start_ = SteadyClock::now();
}

void IterationTimer::EndIteration() { timing_.t_iter_us = DurationMicros(iter_start_, SteadyClock::now()); }

void IterationTimer::BeginCommWindow() { comm_window_start_ = SteadyClock::now(); }

void IterationTimer::EndCommWindow() {
  timing_.t_comm_window_us = DurationMicros(comm_window_start_, SteadyClock::now());
}

void IterationTimer::BeginPost() { phase_start_ = SteadyClock::now(); }
void IterationTimer::EndPost() { timing_.t_post_us += DurationMicros(phase_start_, SteadyClock::now()); }

void IterationTimer::BeginInterior() { phase_start_ = SteadyClock::now(); }
void IterationTimer::EndInterior() {
  timing_.t_interior_us += DurationMicros(phase_start_, SteadyClock::now());
}

void IterationTimer::BeginWait() { phase_start_ = SteadyClock::now(); }
void IterationTimer::EndWait() { timing_.t_wait_us += DurationMicros(phase_start_, SteadyClock::now()); }

void IterationTimer::BeginBoundary() { phase_start_ = SteadyClock::now(); }
void IterationTimer::EndBoundary() {
  timing_.t_boundary_us += DurationMicros(phase_start_, SteadyClock::now());
}

void IterationTimer::BeginPoll() { phase_start_ = SteadyClock::now(); }
void IterationTimer::EndPoll() { timing_.t_poll_us += DurationMicros(phase_start_, SteadyClock::now()); }
void IterationTimer::SetInteriorMicros(double us) { timing_.t_interior_us = us; }
void IterationTimer::SetPollMicros(double us) { timing_.t_poll_us = us; }

const IterationTiming& IterationTimer::Current() const { return timing_; }

void IterationTimer::Reset() { timing_ = {}; }

}  // namespace phasegap::stats

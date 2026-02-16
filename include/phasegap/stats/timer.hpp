#pragma once

#include <chrono>
#include <cstdint>

namespace phasegap::stats {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

std::uint64_t MonotonicMicrosNow();
double DurationMicros(TimePoint start, TimePoint end);

struct IterationTiming {
  double t_post_us = 0.0;
  double t_interior_us = 0.0;
  double t_wait_us = 0.0;
  double t_boundary_us = 0.0;
  double t_poll_us = 0.0;
  double t_comm_window_us = 0.0;
  double t_iter_us = 0.0;
};

class IterationTimer {
 public:
  void BeginIteration();
  void EndIteration();

  void BeginCommWindow();
  void EndCommWindow();

  void BeginPost();
  void EndPost();
  void BeginInterior();
  void EndInterior();
  void BeginWait();
  void EndWait();
  void BeginBoundary();
  void EndBoundary();
  void BeginPoll();
  void EndPoll();
  void SetInteriorMicros(double us);
  void SetPollMicros(double us);

  const IterationTiming& Current() const;
  void Reset();

 private:
  TimePoint iter_start_{};
  TimePoint comm_window_start_{};
  TimePoint phase_start_{};
  IterationTiming timing_{};
};

}  // namespace phasegap::stats

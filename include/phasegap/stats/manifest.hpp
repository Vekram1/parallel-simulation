#pragma once

#include <cstdint>
#include <string>

#include "phasegap/cli.hpp"
#include "phasegap/stats/timer.hpp"

namespace phasegap::stats {

struct RuntimeSummary {
  int ranks = 0;
  int omp_threads = 0;
  int measured_iters = 0;
  std::uint64_t checksum64 = 0;
  double t_iter_us = 0.0;
  double t_post_us = 0.0;
  double t_interior_us = 0.0;
  double t_wait_us = 0.0;
  double t_boundary_us = 0.0;
  double t_poll_us = 0.0;
  double t_comm_window_us = 0.0;
  double wait_frac = 0.0;
  double wait_skew = 0.0;
  double overlap_ratio = 0.0;
  double mpi_test_calls = 0.0;
  double mpi_wait_calls = 0.0;
};

// Writes a run manifest at "<cfg.out_dir>/manifest.json". Returns false on failure.
bool WriteManifest(const cli::Config& cfg, const RuntimeSummary& summary,
                   int mpi_thread_provided, const std::string& error_prefix);

}  // namespace phasegap::stats

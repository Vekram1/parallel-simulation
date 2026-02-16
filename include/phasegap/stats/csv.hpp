#pragma once

#include <cstddef>
#include <cstdint>

#include "phasegap/cli.hpp"

namespace phasegap::stats {

struct CsvSummary {
  int ranks = 0;
  int omp_threads = 0;
  int mpi_thread_provided = 0;
  int measured_iters = 0;

  std::uint64_t checksum64 = 0;
  std::size_t msg_bytes = 0;
  std::size_t bytes_total = 0;

  double t_iter_us = 0.0;
  double t_post_us = 0.0;
  double t_interior_us = 0.0;
  double t_wait_us = 0.0;
  double t_boundary_us = 0.0;
  double t_poll_us = 0.0;
  double t_comm_window_us = 0.0;
  double t_iter_mean_max_us = 0.0;
  double t_post_mean_max_us = 0.0;
  double t_interior_mean_max_us = 0.0;
  double t_wait_mean_max_us = 0.0;
  double t_boundary_mean_max_us = 0.0;
  double t_poll_mean_max_us = 0.0;
  double t_comm_window_mean_max_us = 0.0;
  double t_iter_p50_us = 0.0;
  double t_iter_p95_us = 0.0;
  double t_post_p50_us = 0.0;
  double t_post_p95_us = 0.0;
  double t_interior_p50_us = 0.0;
  double t_interior_p95_us = 0.0;
  double t_wait_p50_us = 0.0;
  double t_wait_p95_us = 0.0;
  double t_boundary_p50_us = 0.0;
  double t_boundary_p95_us = 0.0;
  double t_poll_p50_us = 0.0;
  double t_poll_p95_us = 0.0;
  double t_comm_window_p50_us = 0.0;
  double t_comm_window_p95_us = 0.0;
  double wait_frac = 0.0;
  double wait_skew = 0.0;
  double overlap_ratio = 0.0;
  double bw_effective_bytes_per_us = 0.0;
  double mpi_test_calls = 0.0;
  double mpi_wait_calls = 0.0;
  double polls_to_complete_mean = 0.0;
  double polls_to_complete_p95 = 0.0;
};

// Writes one schema-versioned row to cfg.csv and returns true on success.
bool WriteCsvRow(const cli::Config& cfg, const CsvSummary& summary, const char* error_prefix);

}  // namespace phasegap::stats

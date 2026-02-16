#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "phasegap/cli.hpp"

namespace phasegap::trace {

struct TraceSummary {
  int ranks = 0;
  int omp_threads = 0;
  int measured_iters = 0;
  int trace_iters = 0;
  int mpi_thread_provided = 0;

  double t_post_us = 0.0;
  double t_interior_us = 0.0;
  double t_wait_us = 0.0;
  double t_boundary_us = 0.0;
  double t_iter_us = 0.0;
  double wait_frac = 0.0;
  double overlap_ratio = 0.0;

  int trace_window_start_iter = 0;
  std::uint64_t bytes_total = 0;

  struct RankIteration {
    double t_post_us = 0.0;
    double t_interior_us = 0.0;
    double t_wait_us = 0.0;
    double t_boundary_us = 0.0;
    double t_iter_us = 0.0;
    double wait_frac = 0.0;
    double mpi_test_calls = 0.0;
    std::uint64_t bytes_total = 0;
  };

  // Rank-major flattened vector: [rank0_iter0..rank0_iterN-1, rank1_iter0..]
  std::vector<RankIteration> rank_iterations;

  // Optional per-thread traces for trace_detail=thread.
  // Layout: [rank][iter][thread], flattened rank-major.
  std::vector<double> thread_interior_us;
  std::vector<double> thread_boundary_us;
};

// Writes a Chrome trace JSON file to "<out_dir>/trace.json".
bool WriteTrace(const cli::Config& cfg, const TraceSummary& summary, const char* error_prefix);

}  // namespace phasegap::trace

#pragma once

#include <string>

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
};

// Writes a Chrome trace JSON file to "<out_dir>/trace.json".
bool WriteTrace(const cli::Config& cfg, const TraceSummary& summary, const char* error_prefix);

}  // namespace phasegap::trace

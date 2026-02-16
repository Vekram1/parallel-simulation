#pragma once

#include <cstddef>
#include <vector>

namespace phasegap::stats {

double WaitFraction(double t_wait_mean_us, double t_iter_mean_us);
double WaitSkew(double t_wait_mean_max_us, double t_wait_mean_avg_us, double eps = 1e-9);
double OverlapRatio(double t_comm_window_us, double t_interior_us, double t_wait_us);

struct BandwidthMetrics {
  std::size_t msg_bytes = 0;
  std::size_t bytes_total = 0;
  double bw_effective_bytes_per_us = 0.0;
};

BandwidthMetrics ComputeBandwidthMetrics(int halo_elems, std::size_t element_size_bytes,
                                         double t_comm_window_mean_us);
double Percentile(std::vector<double> values, double q);

}  // namespace phasegap::stats

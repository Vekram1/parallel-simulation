#include "phasegap/stats/metrics.hpp"

#include <algorithm>

namespace phasegap::stats {

double WaitFraction(double t_wait_mean_us, double t_iter_mean_us) {
  if (t_iter_mean_us <= 0.0) {
    return 0.0;
  }
  return t_wait_mean_us / t_iter_mean_us;
}

double WaitSkew(double t_wait_mean_max_us, double t_wait_mean_avg_us, double eps) {
  const double denom = std::max(t_wait_mean_avg_us, eps);
  return t_wait_mean_max_us / denom;
}

double OverlapRatio(double t_comm_window_us, double t_interior_us, double t_wait_us) {
  const double t_ideal_hide = std::min(t_comm_window_us, t_interior_us);
  if (t_ideal_hide <= 0.0) {
    return 0.0;
  }
  const double t_hidden = std::clamp(t_comm_window_us - t_wait_us, 0.0, t_ideal_hide);
  return t_hidden / t_ideal_hide;
}

BandwidthMetrics ComputeBandwidthMetrics(int halo_elems, std::size_t element_size_bytes,
                                         double t_comm_window_mean_us) {
  BandwidthMetrics metrics{};
  if (halo_elems <= 0) {
    return metrics;
  }
  metrics.msg_bytes = static_cast<std::size_t>(halo_elems) * element_size_bytes;
  metrics.bytes_total = metrics.msg_bytes * 2;
  if (t_comm_window_mean_us > 0.0) {
    metrics.bw_effective_bytes_per_us =
        static_cast<double>(metrics.bytes_total) / t_comm_window_mean_us;
  }
  return metrics;
}

}  // namespace phasegap::stats


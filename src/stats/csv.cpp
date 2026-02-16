#include "phasegap/stats/csv.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace phasegap::stats {
namespace {

std::string CurrentTimestampUtc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string CsvEscape(const std::string& value) {
  const bool needs_quotes = value.find_first_of(",\"\n\r") != std::string::npos;
  if (!needs_quotes) {
    return value;
  }
  std::string escaped = "\"";
  for (const char c : value) {
    if (c == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(c);
    }
  }
  escaped += '"';
  return escaped;
}

bool ShouldWriteHeader(const cli::Config& cfg, const std::filesystem::path& path) {
  if (cfg.csv_mode == cli::CsvMode::kWrite) {
    return true;
  }
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    return true;
  }
  if (!exists) {
    return true;
  }
  return std::filesystem::is_empty(path, ec) || ec;
}

}  // namespace

bool WriteCsvRow(const cli::Config& cfg, const CsvSummary& summary, const char* error_prefix) {
  namespace fs = std::filesystem;
  const fs::path csv_path(cfg.csv);
  std::error_code ec;
  const fs::path parent = csv_path.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
    if (ec) {
      std::cerr << error_prefix << " failed to create csv directory '" << parent.string()
                << "': " << ec.message() << '\n';
      return false;
    }
  }

  const bool write_header = ShouldWriteHeader(cfg, csv_path);
  std::ios_base::openmode mode = std::ios::out;
  mode |= (cfg.csv_mode == cli::CsvMode::kAppend && !write_header) ? std::ios::app : std::ios::trunc;
  std::ofstream out(csv_path, mode);
  if (!out.good()) {
    std::cerr << error_prefix << " failed to open csv '" << csv_path.string() << "'\n";
    return false;
  }

  if (write_header) {
    out << "schema_version,timestamp,run_id,mode,kernel,transport_requested,transport_effective"
        << ",mpi_thread_requested,mpi_thread_provided"
        << ",ranks,omp_threads,measured_iters,N,H,radius,timesteps,B,iters,warmup,poll_every"
        << ",t_iter_us,t_post_us,t_interior_us,t_wait_us,t_boundary_us,t_poll_us,t_comm_window_us"
        << ",t_iter_mean_max_us,t_post_mean_max_us,t_interior_mean_max_us,t_wait_mean_max_us"
        << ",t_boundary_mean_max_us,t_poll_mean_max_us,t_comm_window_mean_max_us"
        << ",t_iter_p50_us,t_iter_p95_us,t_post_p50_us,t_post_p95_us,t_interior_p50_us"
        << ",t_interior_p95_us,t_wait_p50_us,t_wait_p95_us,t_boundary_p50_us,t_boundary_p95_us"
        << ",t_poll_p50_us,t_poll_p95_us,t_comm_window_p50_us,t_comm_window_p95_us"
        << ",wait_frac,wait_skew,overlap_ratio,checksum64,msg_bytes,bytes_total"
        << ",bw_effective_bytes_per_us,mpi_test_calls,mpi_wait_calls,polls_to_complete_mean"
        << ",polls_to_complete_p95\n";
  }

  const std::vector<std::string> fields = {
      "2",
      CurrentTimestampUtc(),
      cfg.run_id,
      cli::ToString(cfg.mode),
      cli::ToString(cfg.kernel),
      cli::ToString(cfg.transport),
      summary.transport_effective,
      cli::ToString(cfg.mpi_thread),
      std::to_string(summary.mpi_thread_provided),
      std::to_string(summary.ranks),
      std::to_string(summary.omp_threads),
      std::to_string(summary.measured_iters),
      std::to_string(cfg.n_local),
      std::to_string(cfg.halo),
      std::to_string(cfg.radius),
      std::to_string(cfg.timesteps),
      std::to_string(cfg.boundary_width),
      std::to_string(cfg.iters),
      std::to_string(cfg.warmup),
      std::to_string(cfg.poll_every),
      std::to_string(summary.t_iter_us),
      std::to_string(summary.t_post_us),
      std::to_string(summary.t_interior_us),
      std::to_string(summary.t_wait_us),
      std::to_string(summary.t_boundary_us),
      std::to_string(summary.t_poll_us),
      std::to_string(summary.t_comm_window_us),
      std::to_string(summary.t_iter_mean_max_us),
      std::to_string(summary.t_post_mean_max_us),
      std::to_string(summary.t_interior_mean_max_us),
      std::to_string(summary.t_wait_mean_max_us),
      std::to_string(summary.t_boundary_mean_max_us),
      std::to_string(summary.t_poll_mean_max_us),
      std::to_string(summary.t_comm_window_mean_max_us),
      std::to_string(summary.t_iter_p50_us),
      std::to_string(summary.t_iter_p95_us),
      std::to_string(summary.t_post_p50_us),
      std::to_string(summary.t_post_p95_us),
      std::to_string(summary.t_interior_p50_us),
      std::to_string(summary.t_interior_p95_us),
      std::to_string(summary.t_wait_p50_us),
      std::to_string(summary.t_wait_p95_us),
      std::to_string(summary.t_boundary_p50_us),
      std::to_string(summary.t_boundary_p95_us),
      std::to_string(summary.t_poll_p50_us),
      std::to_string(summary.t_poll_p95_us),
      std::to_string(summary.t_comm_window_p50_us),
      std::to_string(summary.t_comm_window_p95_us),
      std::to_string(summary.wait_frac),
      std::to_string(summary.wait_skew),
      std::to_string(summary.overlap_ratio),
      std::to_string(summary.checksum64),
      std::to_string(summary.msg_bytes),
      std::to_string(summary.bytes_total),
      std::to_string(summary.bw_effective_bytes_per_us),
      std::to_string(summary.mpi_test_calls),
      std::to_string(summary.mpi_wait_calls),
      std::to_string(summary.polls_to_complete_mean),
      std::to_string(summary.polls_to_complete_p95),
  };

  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << CsvEscape(fields[i]);
  }
  out << '\n';

  if (!out.good()) {
    std::cerr << error_prefix << " failed while writing csv row to '" << csv_path.string() << "'\n";
    return false;
  }
  return true;
}

}  // namespace phasegap::stats

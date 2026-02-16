#include "phasegap/stats/manifest.hpp"

#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace phasegap::stats {
namespace {

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (const char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

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

std::string GetEnv(const char* key) {
  const char* val = std::getenv(key);
  return val == nullptr ? "" : std::string(val);
}

std::string BuildCompilerString() {
#if defined(__clang__)
  return std::string("clang-") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc-") + __VERSION__;
#else
  return "unknown";
#endif
}

std::string UnameSummary() {
  struct utsname info {};
  if (uname(&info) != 0) {
    return "unknown";
  }
  std::ostringstream oss;
  oss << info.sysname << " " << info.release << " " << info.machine;
  return oss.str();
}

std::string Hostname() {
  char name[256];
  name[0] = '\0';
  if (gethostname(name, sizeof(name)) != 0) {
    return "unknown";
  }
  name[sizeof(name) - 1] = '\0';
  return std::string(name);
}

std::string GitSha() {
#ifdef PHASEGAP_GIT_SHA
  const std::string from_build = PHASEGAP_GIT_SHA;
  if (!from_build.empty()) {
    return from_build;
  }
#endif
  const std::string sha = GetEnv("PHASEGAP_GIT_SHA");
  if (!sha.empty()) {
    return sha;
  }
  return "unknown";
}

std::string ToJsonBool(bool value) { return value ? "true" : "false"; }

}  // namespace

bool WriteManifest(const cli::Config& cfg, const RuntimeSummary& summary,
                   int mpi_thread_provided, const std::string& progress_effective,
                   const std::string& transport_effective,
                   const std::string& error_prefix) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(cfg.out_dir, ec);
  if (ec) {
    std::cerr << error_prefix << " failed to create output directory '" << cfg.out_dir
              << "': " << ec.message() << '\n';
    return false;
  }

  const fs::path manifest_path = fs::path(cfg.out_dir) / "manifest.json";
  std::ofstream out(manifest_path);
  if (!out.good()) {
    std::cerr << error_prefix << " failed to open '" << manifest_path.string() << "' for writing\n";
    return false;
  }

  out << "{\n";
  out << "  \"schema_version\": 3,\n";
  out << "  \"timestamp\": \"" << JsonEscape(CurrentTimestampUtc()) << "\",\n";
  out << "  \"run_id\": \"" << JsonEscape(cfg.run_id) << "\",\n";
  out << "  \"git_sha\": \"" << JsonEscape(GitSha()) << "\",\n";
  out << "  \"hostname\": \"" << JsonEscape(Hostname()) << "\",\n";
  out << "  \"platform\": \"" << JsonEscape(UnameSummary()) << "\",\n";
  out << "  \"compiler\": \"" << JsonEscape(BuildCompilerString()) << "\",\n";

  out << "  \"config\": {\n";
  out << "    \"mode\": \"" << cli::ToString(cfg.mode) << "\",\n";
  out << "    \"kernel\": \"" << cli::ToString(cfg.kernel) << "\",\n";
  out << "    \"mpi_thread_requested\": \"" << cli::ToString(cfg.mpi_thread) << "\",\n";
  out << "    \"mpi_thread_provided\": " << mpi_thread_provided << ",\n";
  out << "    \"progress_requested\": \"" << cli::ToString(cfg.progress) << "\",\n";
  out << "    \"progress_effective\": \"" << JsonEscape(progress_effective) << "\",\n";
  out << "    \"omp_schedule\": \"" << cli::ToString(cfg.omp_schedule) << "\",\n";
  out << "    \"omp_places\": \"" << cli::ToString(cfg.omp_places) << "\",\n";
  out << "    \"sync\": \"" << cli::ToString(cfg.sync) << "\",\n";
  out << "    \"transport_requested\": \"" << cli::ToString(cfg.transport) << "\",\n";
  out << "    \"transport_effective\": \"" << JsonEscape(transport_effective) << "\",\n";
  out << "    \"trace_detail\": \"" << cli::ToString(cfg.trace_detail) << "\",\n";
  out << "    \"csv_mode\": \"" << cli::ToString(cfg.csv_mode) << "\",\n";
  out << "    \"ranks\": " << cfg.ranks << ",\n";
  out << "    \"threads\": " << cfg.threads << ",\n";
  out << "    \"N\": " << cfg.n_local << ",\n";
  out << "    \"halo\": " << cfg.halo << ",\n";
  out << "    \"radius\": " << cfg.radius << ",\n";
  out << "    \"timesteps\": " << cfg.timesteps << ",\n";
  out << "    \"B\": " << cfg.boundary_width << ",\n";
  out << "    \"poll_every\": " << cfg.poll_every << ",\n";
  out << "    \"iters\": " << cfg.iters << ",\n";
  out << "    \"warmup\": " << cfg.warmup << ",\n";
  out << "    \"trace\": " << ToJsonBool(cfg.trace) << ",\n";
  out << "    \"trace_iters\": " << cfg.trace_iters << ",\n";
  out << "    \"check\": " << ToJsonBool(cfg.check) << ",\n";
  out << "    \"check_every\": " << cfg.check_every << ",\n";
  out << "    \"poison_ghost\": " << ToJsonBool(cfg.poison_ghost) << ",\n";
  out << "    \"omp_bind\": " << ToJsonBool(cfg.omp_bind) << ",\n";
  out << "    \"omp_chunk\": " << cfg.omp_chunk << ",\n";
  out << "    \"flops_per_point\": " << cfg.flops_per_point << ",\n";
  out << "    \"bytes_per_point\": " << cfg.bytes_per_point << ",\n";
  out << "    \"out_dir\": \"" << JsonEscape(cfg.out_dir) << "\",\n";
  out << "    \"csv\": \"" << JsonEscape(cfg.csv) << "\"\n";
  out << "  },\n";

  out << "  \"environment\": {\n";
  out << "    \"OMP_NUM_THREADS\": \"" << JsonEscape(GetEnv("OMP_NUM_THREADS")) << "\",\n";
  out << "    \"OMP_PROC_BIND\": \"" << JsonEscape(GetEnv("OMP_PROC_BIND")) << "\",\n";
  out << "    \"OMP_PLACES\": \"" << JsonEscape(GetEnv("OMP_PLACES")) << "\",\n";
  out << "    \"OMP_SCHEDULE\": \"" << JsonEscape(GetEnv("OMP_SCHEDULE")) << "\",\n";
  out << "    \"OMP_DISPLAY_ENV\": \"" << JsonEscape(GetEnv("OMP_DISPLAY_ENV")) << "\",\n";
  out << "    \"OMPI_MCA_btl\": \"" << JsonEscape(GetEnv("OMPI_MCA_btl")) << "\",\n";
  out << "    \"OMPI_MCA_pml\": \"" << JsonEscape(GetEnv("OMPI_MCA_pml")) << "\"\n";
  out << "  },\n";

  out << "  \"runtime_summary\": {\n";
  out << "    \"ranks\": " << summary.ranks << ",\n";
  out << "    \"omp_threads\": " << summary.omp_threads << ",\n";
  out << "    \"measured_iters\": " << summary.measured_iters << ",\n";
  out << "    \"checksum64\": " << summary.checksum64 << ",\n";
  out << "    \"t_iter_us\": " << summary.t_iter_us << ",\n";
  out << "    \"t_post_us\": " << summary.t_post_us << ",\n";
  out << "    \"t_interior_us\": " << summary.t_interior_us << ",\n";
  out << "    \"t_wait_us\": " << summary.t_wait_us << ",\n";
  out << "    \"t_boundary_us\": " << summary.t_boundary_us << ",\n";
  out << "    \"t_poll_us\": " << summary.t_poll_us << ",\n";
  out << "    \"t_comm_window_us\": " << summary.t_comm_window_us << ",\n";
  out << "    \"t_iter_mean_max_us\": " << summary.t_iter_mean_max_us << ",\n";
  out << "    \"t_post_mean_max_us\": " << summary.t_post_mean_max_us << ",\n";
  out << "    \"t_interior_mean_max_us\": " << summary.t_interior_mean_max_us << ",\n";
  out << "    \"t_wait_mean_max_us\": " << summary.t_wait_mean_max_us << ",\n";
  out << "    \"t_boundary_mean_max_us\": " << summary.t_boundary_mean_max_us << ",\n";
  out << "    \"t_poll_mean_max_us\": " << summary.t_poll_mean_max_us << ",\n";
  out << "    \"t_comm_window_mean_max_us\": " << summary.t_comm_window_mean_max_us << ",\n";
  out << "    \"t_iter_p50_us\": " << summary.t_iter_p50_us << ",\n";
  out << "    \"t_iter_p95_us\": " << summary.t_iter_p95_us << ",\n";
  out << "    \"t_post_p50_us\": " << summary.t_post_p50_us << ",\n";
  out << "    \"t_post_p95_us\": " << summary.t_post_p95_us << ",\n";
  out << "    \"t_interior_p50_us\": " << summary.t_interior_p50_us << ",\n";
  out << "    \"t_interior_p95_us\": " << summary.t_interior_p95_us << ",\n";
  out << "    \"t_wait_p50_us\": " << summary.t_wait_p50_us << ",\n";
  out << "    \"t_wait_p95_us\": " << summary.t_wait_p95_us << ",\n";
  out << "    \"t_boundary_p50_us\": " << summary.t_boundary_p50_us << ",\n";
  out << "    \"t_boundary_p95_us\": " << summary.t_boundary_p95_us << ",\n";
  out << "    \"t_poll_p50_us\": " << summary.t_poll_p50_us << ",\n";
  out << "    \"t_poll_p95_us\": " << summary.t_poll_p95_us << ",\n";
  out << "    \"t_comm_window_p50_us\": " << summary.t_comm_window_p50_us << ",\n";
  out << "    \"t_comm_window_p95_us\": " << summary.t_comm_window_p95_us << ",\n";
  out << "    \"wait_frac\": " << summary.wait_frac << ",\n";
  out << "    \"wait_skew\": " << summary.wait_skew << ",\n";
  out << "    \"overlap_ratio\": " << summary.overlap_ratio << ",\n";
  out << "    \"mpi_test_calls\": " << summary.mpi_test_calls << ",\n";
  out << "    \"mpi_wait_calls\": " << summary.mpi_wait_calls << ",\n";
  out << "    \"polls_to_complete_mean\": " << summary.polls_to_complete_mean << ",\n";
  out << "    \"polls_to_complete_p95\": " << summary.polls_to_complete_p95 << "\n";
  out << "  }\n";
  out << "}\n";

  if (!out.good()) {
    std::cerr << error_prefix << " failed while writing '" << manifest_path.string() << "'\n";
    return false;
  }
  return true;
}

}  // namespace phasegap::stats

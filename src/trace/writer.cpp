#include "phasegap/trace/writer.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace phasegap::trace {
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

std::string Hostname() {
  char name[256];
  name[0] = '\0';
  if (gethostname(name, sizeof(name)) != 0) {
    return "unknown";
  }
  name[sizeof(name) - 1] = '\0';
  return std::string(name);
}

std::string PlatformSummary() {
  struct utsname info {};
  if (uname(&info) != 0) {
    return "unknown";
  }
  std::ostringstream oss;
  oss << info.sysname << " " << info.release << " " << info.machine;
  return oss.str();
}

std::string GetEnv(const char* key) {
  const char* val = std::getenv(key);
  return val == nullptr ? "" : std::string(val);
}

std::string GitSha() {
#ifdef PHASEGAP_GIT_SHA
  const std::string from_build = PHASEGAP_GIT_SHA;
  if (!from_build.empty()) {
    return from_build;
  }
#endif
  const std::string from_env = GetEnv("PHASEGAP_GIT_SHA");
  if (!from_env.empty()) {
    return from_env;
  }
  return "unknown";
}

}  // namespace

bool WriteTrace(const cli::Config& cfg, const TraceSummary& summary, const char* error_prefix) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(cfg.out_dir, ec);
  if (ec) {
    std::cerr << error_prefix << " failed to create output directory '" << cfg.out_dir
              << "': " << ec.message() << '\n';
    return false;
  }

  const fs::path trace_path = fs::path(cfg.out_dir) / "trace.json";
  std::ofstream out(trace_path);
  if (!out.good()) {
    std::cerr << error_prefix << " failed to open '" << trace_path.string() << "' for writing\n";
    return false;
  }

  // Minimal baseline trace envelope for portability and quick visual checks.
  const double t_post = std::max(0.0, summary.t_post_us);
  const double t_interior = std::max(0.0, summary.t_interior_us);
  const double t_wait = std::max(0.0, summary.t_wait_us);
  const double t_boundary = std::max(0.0, summary.t_boundary_us);

  out << "{\n";
  out << "  \"trace_schema_version\": 1,\n";
  out << "  \"timestamp\": \"" << JsonEscape(CurrentTimestampUtc()) << "\",\n";
  out << "  \"run_id\": \"" << JsonEscape(cfg.run_id) << "\",\n";
  out << "  \"git_sha\": \"" << JsonEscape(GitSha()) << "\",\n";
  out << "  \"hostname\": \"" << JsonEscape(Hostname()) << "\",\n";
  out << "  \"platform\": \"" << JsonEscape(PlatformSummary()) << "\",\n";
  out << "  \"config\": {\n";
  out << "    \"mode\": \"" << cli::ToString(cfg.mode) << "\",\n";
  out << "    \"kernel\": \"" << cli::ToString(cfg.kernel) << "\",\n";
  out << "    \"trace_detail\": \"" << cli::ToString(cfg.trace_detail) << "\",\n";
  out << "    \"ranks\": " << summary.ranks << ",\n";
  out << "    \"omp_threads\": " << summary.omp_threads << ",\n";
  out << "    \"trace_iters\": " << summary.trace_iters << ",\n";
  out << "    \"measured_iters\": " << summary.measured_iters << ",\n";
  out << "    \"mpi_thread_requested\": \"" << cli::ToString(cfg.mpi_thread) << "\",\n";
  out << "    \"mpi_thread_provided\": " << summary.mpi_thread_provided << "\n";
  out << "  },\n";
  out << "  \"summary\": {\n";
  out << "    \"t_iter_us\": " << summary.t_iter_us << ",\n";
  out << "    \"wait_frac\": " << summary.wait_frac << ",\n";
  out << "    \"overlap_ratio\": " << summary.overlap_ratio << "\n";
  out << "  },\n";
  out << "  \"environment\": {\n";
  out << "    \"OMP_NUM_THREADS\": \"" << JsonEscape(GetEnv("OMP_NUM_THREADS")) << "\",\n";
  out << "    \"OMP_PROC_BIND\": \"" << JsonEscape(GetEnv("OMP_PROC_BIND")) << "\",\n";
  out << "    \"OMP_PLACES\": \"" << JsonEscape(GetEnv("OMP_PLACES")) << "\",\n";
  out << "    \"OMP_SCHEDULE\": \"" << JsonEscape(GetEnv("OMP_SCHEDULE")) << "\",\n";
  out << "    \"OMPI_MCA_btl\": \"" << JsonEscape(GetEnv("OMPI_MCA_btl")) << "\"\n";
  out << "  },\n";
  out << "  \"traceEvents\": [\n";
  out << "    {\"name\":\"comm_post\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":0,\"tid\":0,\"ts\":0,"
      << "\"dur\":" << t_post << "},\n";
  out << "    {\"name\":\"interior_compute\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":0,\"tid\":0,\"ts\":"
      << t_post << ",\"dur\":" << t_interior << "},\n";
  out << "    {\"name\":\"waitall\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":0,\"tid\":0,\"ts\":"
      << (t_post + t_interior) << ",\"dur\":" << t_wait << "},\n";
  out << "    {\"name\":\"boundary_compute\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":0,\"tid\":0,\"ts\":"
      << (t_post + t_interior + t_wait) << ",\"dur\":" << t_boundary << "}\n";
  out << "  ]\n";
  out << "}\n";

  if (!out.good()) {
    std::cerr << error_prefix << " failed while writing '" << trace_path.string() << "'\n";
    return false;
  }
  return true;
}

}  // namespace phasegap::trace

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

  out << "{\n";
  out << "  \"trace_schema_version\": 2,\n";
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
  out << "    \"transport_requested\": \"" << cli::ToString(cfg.transport) << "\",\n";
  out << "    \"transport_effective\": \"" << JsonEscape(summary.transport_effective) << "\",\n";
  out << "    \"mpi_thread_requested\": \"" << cli::ToString(cfg.mpi_thread) << "\",\n";
  out << "    \"mpi_thread_provided\": " << summary.mpi_thread_provided << ",\n";
  out << "    \"trace_window_start_iter\": " << summary.trace_window_start_iter << ",\n";
  out << "    \"bytes_total\": " << summary.bytes_total << "\n";
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
  out << "    \"OMPI_MCA_btl\": \"" << JsonEscape(GetEnv("OMPI_MCA_btl")) << "\",\n";
  out << "    \"OMPI_MCA_pml\": \"" << JsonEscape(GetEnv("OMPI_MCA_pml")) << "\"\n";
  out << "  },\n";
  out << "  \"traceEvents\": [\n";
  bool first_event = true;
  const auto emit_event = [&](const std::string& json_line) {
    if (!first_event) {
      out << ",\n";
    }
    first_event = false;
    out << "    " << json_line;
  };

  const int trace_iters = std::max(0, summary.trace_iters);
  const int rank_count = std::max(0, summary.ranks);
  const int expected_rank_entries = rank_count * trace_iters;
  const bool have_rank_data =
      static_cast<int>(summary.rank_iterations.size()) >= expected_rank_entries;
  if (trace_iters > 0 && !have_rank_data) {
    std::cerr << error_prefix << " missing rank iteration trace data: expected at least "
              << expected_rank_entries << " rows, got " << summary.rank_iterations.size() << '\n';
    return false;
  }
  const int expected_thread_entries = rank_count * trace_iters * std::max(1, summary.omp_threads);
  const bool have_thread_data = static_cast<int>(summary.thread_interior_us.size()) >=
                                    expected_thread_entries &&
                                static_cast<int>(summary.thread_boundary_us.size()) >=
                                    expected_thread_entries;
  if (cfg.trace_detail == cli::TraceDetail::kThread && trace_iters > 0 && !have_thread_data) {
    std::cerr << error_prefix
              << " warning: trace_detail=thread requested but thread span payload is incomplete; "
                 "emitting rank-lane phase spans only\n";
  }

  if (trace_iters > 0 && have_rank_data) {
    for (int rank = 0; rank < rank_count; ++rank) {
      double rank_ts = 0.0;
      for (int iter = 0; iter < trace_iters; ++iter) {
        const int rank_iter_idx = rank * trace_iters + iter;
        const TraceSummary::RankIteration& row = summary.rank_iterations[rank_iter_idx];
        const double t_post = std::max(0.0, row.t_post_us);
        const double t_interior = std::max(0.0, row.t_interior_us);
        const double t_wait = std::max(0.0, row.t_wait_us);
        const double t_boundary = std::max(0.0, row.t_boundary_us);
        const double t_iter = std::max(std::max(0.0, row.t_iter_us), t_post + t_interior + t_wait + t_boundary);

        const int measured_iter = summary.trace_window_start_iter + iter;
        const double ts_post = rank_ts;
        const double ts_interior = ts_post + t_post;
        const double ts_wait = ts_interior + t_interior;
        const double ts_boundary = ts_wait + t_wait;
        const double ts_end = rank_ts + t_iter;

        {
          std::ostringstream e;
          e << "{\"name\":\"comm_post\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_post << ",\"dur\":" << t_post
            << ",\"args\":{\"iter\":" << measured_iter << "}}";
          emit_event(e.str());
        }
        {
          std::ostringstream e;
          e << "{\"name\":\"interior_compute\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_interior << ",\"dur\":" << t_interior
            << ",\"args\":{\"iter\":" << measured_iter << "}}";
          emit_event(e.str());
        }
        {
          std::ostringstream e;
          e << "{\"name\":\"waitall\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_wait << ",\"dur\":" << t_wait
            << ",\"args\":{\"iter\":" << measured_iter << "}}";
          emit_event(e.str());
        }
        {
          std::ostringstream e;
          e << "{\"name\":\"boundary_compute\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_boundary << ",\"dur\":" << t_boundary
            << ",\"args\":{\"iter\":" << measured_iter << "}}";
          emit_event(e.str());
        }

        {
          std::ostringstream e;
          e << "{\"name\":\"bytes_total\",\"cat\":\"counter\",\"ph\":\"C\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_end << ",\"args\":{\"value\":" << row.bytes_total << "}}";
          emit_event(e.str());
        }
        {
          std::ostringstream e;
          e << "{\"name\":\"mpi_test_calls\",\"cat\":\"counter\",\"ph\":\"C\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_end << ",\"args\":{\"value\":" << row.mpi_test_calls
            << "}}";
          emit_event(e.str());
        }
        {
          std::ostringstream e;
          e << "{\"name\":\"t_wait_us\",\"cat\":\"counter\",\"ph\":\"C\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_end << ",\"args\":{\"value\":" << row.t_wait_us << "}}";
          emit_event(e.str());
        }
        {
          std::ostringstream e;
          e << "{\"name\":\"wait_frac\",\"cat\":\"counter\",\"ph\":\"C\",\"pid\":" << rank
            << ",\"tid\":0,\"ts\":" << ts_end << ",\"args\":{\"value\":" << row.wait_frac << "}}";
          emit_event(e.str());
        }

        if (cfg.trace_detail == cli::TraceDetail::kThread && have_thread_data && summary.omp_threads > 0) {
          const int base = (rank * trace_iters + iter) * summary.omp_threads;
          for (int tid = 0; tid < summary.omp_threads; ++tid) {
            const double thread_interior = std::max(0.0, summary.thread_interior_us[base + tid]);
            const double thread_boundary = std::max(0.0, summary.thread_boundary_us[base + tid]);
            if (thread_interior > 0.0) {
              std::ostringstream e;
              e << "{\"name\":\"interior_compute\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":" << rank
                << ",\"tid\":" << (tid + 1) << ",\"ts\":" << ts_interior << ",\"dur\":"
                << thread_interior << ",\"args\":{\"iter\":" << measured_iter << "}}";
              emit_event(e.str());
            }
            if (thread_boundary > 0.0) {
              std::ostringstream e;
              e << "{\"name\":\"boundary_compute\",\"cat\":\"phase\",\"ph\":\"X\",\"pid\":" << rank
                << ",\"tid\":" << (tid + 1) << ",\"ts\":" << ts_boundary << ",\"dur\":"
                << thread_boundary << ",\"args\":{\"iter\":" << measured_iter << "}}";
              emit_event(e.str());
            }
          }
        }

        rank_ts += t_iter;
      }
    }
  }
  out << "\n  ]\n";
  out << "}\n";

  if (!out.good()) {
    std::cerr << error_prefix << " failed while writing '" << trace_path.string() << "'\n";
    return false;
  }
  return true;
}

}  // namespace phasegap::trace

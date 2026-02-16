#include "phasegap/cli.hpp"

#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace phasegap::cli {

namespace {

bool ParseInt(const std::string& text, int* out) {
  try {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed, 10);
    if (consumed != text.size()) {
      return false;
    }
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
      return false;
    }
    *out = static_cast<int>(value);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseBool(const std::string& text, bool* out) {
  if (text == "1" || text == "true") {
    *out = true;
    return true;
  }
  if (text == "0" || text == "false") {
    *out = false;
    return true;
  }
  return false;
}

template <typename T>
bool ParseEnum(const std::unordered_map<std::string, T>& table, const std::string& text, T* out) {
  const auto it = table.find(text);
  if (it == table.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

bool EnsurePositive(int value, const char* name, ParseResult* result) {
  if (value <= 0) {
    result->error = std::string(name) + " must be > 0";
    return false;
  }
  return true;
}

}  // namespace

std::string Usage() {
  return
      "Usage: phasegap --mode <phase_nb|phase_blk|nb_test|phase_persist|omp_tasks> "
      "--ranks <P> --threads <T> --N <local_n> --halo <H> --iters <K> --warmup <W> [options]\n"
      "Options:\n"
      "  --kernel <stencil3|stencil5|axpy>\n"
      "  --radius <R> --timesteps <S> --poll_every <q>\n"
      "  --mpi_thread <funneled|serialized|multiple>\n"
      "  --progress <inline_poll|progress_thread>\n"
      "  --omp_schedule <static|dynamic|guided> --omp_chunk <c>\n"
      "  --omp_bind <0|1|false|true> --omp_places <cores|threads>\n"
      "  --flops_per_point <f> --bytes_per_point <b>\n"
      "  --check <0|1> --check_every <E> --poison_ghost <0|1>\n"
      "  --sync <none|barrier_start|barrier_each>\n"
      "  --trace <0|1> --trace_iters <M> --trace_detail <rank|thread>\n"
      "  --transport <auto|shm|tcp>\n"
      "  --out_dir <dir> --run_id <id> --csv <file> --csv_mode <write|append> --manifest <0|1>\n";
}

ParseResult ParseArgs(int argc, char** argv) {
  ParseResult result;

  if (argc <= 1) {
    result.error = "missing required arguments\n" + Usage();
    return result;
  }

  std::unordered_map<std::string, std::string> kv;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--help" || key == "-h") {
      result.ok = true;
      result.should_exit = true;
      result.exit_code = 0;
      return result;
    }
    if (key.rfind("--", 0) != 0) {
      result.error = "unexpected argument: " + key;
      return result;
    }
    if (i + 1 >= argc) {
      result.error = "missing value for argument: " + key;
      return result;
    }
    kv[key] = argv[++i];
  }

  Config cfg{};
  bool has_mode = false;
  bool has_ranks = false;
  bool has_threads = false;
  bool has_n = false;
  bool has_halo = false;
  bool has_iters = false;
  bool has_warmup = false;

  const std::unordered_map<std::string, Mode> mode_map = {
      {"phase_nb", Mode::kPhaseNb},
      {"phase_blk", Mode::kPhaseBlk},
      {"nb_test", Mode::kNbTest},
      {"phase_persist", Mode::kPhasePersist},
      {"omp_tasks", Mode::kOmpTasks},
  };
  const std::unordered_map<std::string, MpiThread> mpi_thread_map = {
      {"funneled", MpiThread::kFunneled},
      {"serialized", MpiThread::kSerialized},
      {"multiple", MpiThread::kMultiple},
  };
  const std::unordered_map<std::string, Progress> progress_map = {
      {"inline_poll", Progress::kInlinePoll},
      {"progress_thread", Progress::kProgressThread},
  };
  const std::unordered_map<std::string, OmpSchedule> schedule_map = {
      {"static", OmpSchedule::kStatic},
      {"dynamic", OmpSchedule::kDynamic},
      {"guided", OmpSchedule::kGuided},
  };
  const std::unordered_map<std::string, OmpPlaces> places_map = {
      {"cores", OmpPlaces::kCores},
      {"threads", OmpPlaces::kThreads},
  };
  const std::unordered_map<std::string, SyncMode> sync_map = {
      {"none", SyncMode::kNone},
      {"barrier_start", SyncMode::kBarrierStart},
      {"barrier_each", SyncMode::kBarrierEach},
  };
  const std::unordered_map<std::string, TraceDetail> trace_detail_map = {
      {"rank", TraceDetail::kRank},
      {"thread", TraceDetail::kThread},
  };
  const std::unordered_map<std::string, Transport> transport_map = {
      {"auto", Transport::kAuto},
      {"shm", Transport::kShm},
      {"tcp", Transport::kTcp},
  };
  const std::unordered_map<std::string, CsvMode> csv_mode_map = {
      {"write", CsvMode::kWrite},
      {"append", CsvMode::kAppend},
  };

  for (const auto& [key, value] : kv) {
    if (key == "--mode") {
      if (!ParseEnum(mode_map, value, &cfg.mode)) {
        result.error = "invalid --mode: " + value;
        return result;
      }
      has_mode = true;
      continue;
    }
    if (key == "--ranks") {
      if (!ParseInt(value, &cfg.ranks)) {
        result.error = "invalid --ranks: " + value;
        return result;
      }
      has_ranks = true;
      continue;
    }
    if (key == "--threads") {
      if (!ParseInt(value, &cfg.threads)) {
        result.error = "invalid --threads: " + value;
        return result;
      }
      has_threads = true;
      continue;
    }
    if (key == "--N") {
      if (!ParseInt(value, &cfg.n_local)) {
        result.error = "invalid --N: " + value;
        return result;
      }
      has_n = true;
      continue;
    }
    if (key == "--halo") {
      if (!ParseInt(value, &cfg.halo)) {
        result.error = "invalid --halo: " + value;
        return result;
      }
      has_halo = true;
      continue;
    }
    if (key == "--kernel") {
      if (!kernels::ParseKernel(value, &cfg.kernel)) {
        result.error = "invalid --kernel: " + value;
        return result;
      }
      continue;
    }
    if (key == "--radius") {
      if (!ParseInt(value, &cfg.radius)) {
        result.error = "invalid --radius: " + value;
        return result;
      }
      cfg.radius_set = true;
      continue;
    }
    if (key == "--timesteps") {
      if (!ParseInt(value, &cfg.timesteps)) {
        result.error = "invalid --timesteps: " + value;
        return result;
      }
      continue;
    }
    if (key == "--poll_every") {
      if (!ParseInt(value, &cfg.poll_every)) {
        result.error = "invalid --poll_every: " + value;
        return result;
      }
      continue;
    }
    if (key == "--mpi_thread") {
      if (!ParseEnum(mpi_thread_map, value, &cfg.mpi_thread)) {
        result.error = "invalid --mpi_thread: " + value;
        return result;
      }
      continue;
    }
    if (key == "--progress") {
      if (!ParseEnum(progress_map, value, &cfg.progress)) {
        result.error = "invalid --progress: " + value;
        return result;
      }
      continue;
    }
    if (key == "--omp_schedule") {
      if (!ParseEnum(schedule_map, value, &cfg.omp_schedule)) {
        result.error = "invalid --omp_schedule: " + value;
        return result;
      }
      continue;
    }
    if (key == "--omp_chunk") {
      if (!ParseInt(value, &cfg.omp_chunk)) {
        result.error = "invalid --omp_chunk: " + value;
        return result;
      }
      continue;
    }
    if (key == "--omp_bind") {
      if (!ParseBool(value, &cfg.omp_bind)) {
        result.error = "invalid --omp_bind: " + value;
        return result;
      }
      continue;
    }
    if (key == "--omp_places") {
      if (!ParseEnum(places_map, value, &cfg.omp_places)) {
        result.error = "invalid --omp_places: " + value;
        return result;
      }
      continue;
    }
    if (key == "--flops_per_point") {
      if (!ParseInt(value, &cfg.flops_per_point)) {
        result.error = "invalid --flops_per_point: " + value;
        return result;
      }
      continue;
    }
    if (key == "--bytes_per_point") {
      if (!ParseInt(value, &cfg.bytes_per_point)) {
        result.error = "invalid --bytes_per_point: " + value;
        return result;
      }
      continue;
    }
    if (key == "--iters") {
      if (!ParseInt(value, &cfg.iters)) {
        result.error = "invalid --iters: " + value;
        return result;
      }
      has_iters = true;
      continue;
    }
    if (key == "--warmup") {
      if (!ParseInt(value, &cfg.warmup)) {
        result.error = "invalid --warmup: " + value;
        return result;
      }
      has_warmup = true;
      continue;
    }
    if (key == "--check") {
      if (!ParseBool(value, &cfg.check)) {
        result.error = "invalid --check: " + value;
        return result;
      }
      continue;
    }
    if (key == "--check_every") {
      if (!ParseInt(value, &cfg.check_every)) {
        result.error = "invalid --check_every: " + value;
        return result;
      }
      continue;
    }
    if (key == "--poison_ghost") {
      if (!ParseBool(value, &cfg.poison_ghost)) {
        result.error = "invalid --poison_ghost: " + value;
        return result;
      }
      continue;
    }
    if (key == "--sync") {
      if (!ParseEnum(sync_map, value, &cfg.sync)) {
        result.error = "invalid --sync: " + value;
        return result;
      }
      continue;
    }
    if (key == "--trace") {
      if (!ParseBool(value, &cfg.trace)) {
        result.error = "invalid --trace: " + value;
        return result;
      }
      continue;
    }
    if (key == "--trace_iters") {
      if (!ParseInt(value, &cfg.trace_iters)) {
        result.error = "invalid --trace_iters: " + value;
        return result;
      }
      continue;
    }
    if (key == "--trace_detail") {
      if (!ParseEnum(trace_detail_map, value, &cfg.trace_detail)) {
        result.error = "invalid --trace_detail: " + value;
        return result;
      }
      continue;
    }
    if (key == "--transport") {
      if (!ParseEnum(transport_map, value, &cfg.transport)) {
        result.error = "invalid --transport: " + value;
        return result;
      }
      continue;
    }
    if (key == "--out_dir") {
      cfg.out_dir = value;
      continue;
    }
    if (key == "--run_id") {
      cfg.run_id = value;
      continue;
    }
    if (key == "--csv") {
      cfg.csv = value;
      continue;
    }
    if (key == "--csv_mode") {
      if (!ParseEnum(csv_mode_map, value, &cfg.csv_mode)) {
        result.error = "invalid --csv_mode: " + value;
        return result;
      }
      continue;
    }
    if (key == "--manifest") {
      if (!ParseBool(value, &cfg.manifest)) {
        result.error = "invalid --manifest: " + value;
        return result;
      }
      continue;
    }

    result.error = "unknown argument: " + key;
    return result;
  }

  if (!has_mode || !has_ranks || !has_threads || !has_n || !has_halo || !has_iters ||
      !has_warmup) {
    result.error =
        "missing required args: --mode --ranks --threads --N --halo --iters --warmup\n" + Usage();
    return result;
  }
  if (!EnsurePositive(cfg.ranks, "--ranks", &result) ||
      !EnsurePositive(cfg.threads, "--threads", &result) ||
      !EnsurePositive(cfg.n_local, "--N", &result) || !EnsurePositive(cfg.halo, "--halo", &result) ||
      !EnsurePositive(cfg.iters, "--iters", &result) ||
      !EnsurePositive(cfg.timesteps, "--timesteps", &result) ||
      !EnsurePositive(cfg.poll_every, "--poll_every", &result) ||
      !EnsurePositive(cfg.check_every, "--check_every", &result) ||
      !EnsurePositive(cfg.trace_iters, "--trace_iters", &result)) {
    return result;
  }
  if (cfg.warmup < 0) {
    result.error = "--warmup must be >= 0";
    return result;
  }
  if (cfg.warmup >= cfg.iters) {
    result.error = "--warmup must be less than --iters";
    return result;
  }

  if (!cfg.radius_set) {
    cfg.radius = kernels::DefaultRadius(cfg.kernel);
  }
  if (!EnsurePositive(cfg.radius, "--radius", &result)) {
    return result;
  }

  const long long boundary = static_cast<long long>(cfg.radius) * static_cast<long long>(cfg.timesteps);
  if (boundary > std::numeric_limits<int>::max()) {
    result.error = "derived boundary width B=R*S overflowed int range";
    return result;
  }
  cfg.boundary_width = static_cast<int>(boundary);

  if (cfg.halo < cfg.boundary_width) {
    std::ostringstream oss;
    oss << "invalid halo configuration: require H >= B, got H=" << cfg.halo
        << " and B=R*S=" << cfg.boundary_width;
    result.error = oss.str();
    return result;
  }
  if (cfg.omp_chunk < 0) {
    result.error = "--omp_chunk must be >= 0";
    return result;
  }
  if (cfg.flops_per_point < 0 || cfg.bytes_per_point < 0) {
    result.error = "--flops_per_point and --bytes_per_point must be >= 0";
    return result;
  }

  if (cfg.out_dir.empty()) {
    cfg.out_dir = "runs/default";
  }
  if (cfg.csv.empty()) {
    cfg.csv = cfg.out_dir + "/results.csv";
  }

  result.ok = true;
  result.config = std::move(cfg);
  return result;
}

const char* ToString(Mode mode) {
  switch (mode) {
    case Mode::kPhaseNb:
      return "phase_nb";
    case Mode::kPhaseBlk:
      return "phase_blk";
    case Mode::kNbTest:
      return "nb_test";
    case Mode::kPhasePersist:
      return "phase_persist";
    case Mode::kOmpTasks:
      return "omp_tasks";
  }
  return "unknown";
}

const char* ToString(kernels::Kernel kernel) { return kernels::KernelName(kernel); }

const char* ToString(MpiThread level) {
  switch (level) {
    case MpiThread::kFunneled:
      return "funneled";
    case MpiThread::kSerialized:
      return "serialized";
    case MpiThread::kMultiple:
      return "multiple";
  }
  return "unknown";
}

const char* ToString(Progress progress) {
  switch (progress) {
    case Progress::kInlinePoll:
      return "inline_poll";
    case Progress::kProgressThread:
      return "progress_thread";
  }
  return "unknown";
}

const char* ToString(OmpSchedule schedule) {
  switch (schedule) {
    case OmpSchedule::kStatic:
      return "static";
    case OmpSchedule::kDynamic:
      return "dynamic";
    case OmpSchedule::kGuided:
      return "guided";
  }
  return "unknown";
}

const char* ToString(OmpPlaces places) {
  switch (places) {
    case OmpPlaces::kCores:
      return "cores";
    case OmpPlaces::kThreads:
      return "threads";
  }
  return "unknown";
}

const char* ToString(SyncMode sync_mode) {
  switch (sync_mode) {
    case SyncMode::kNone:
      return "none";
    case SyncMode::kBarrierStart:
      return "barrier_start";
    case SyncMode::kBarrierEach:
      return "barrier_each";
  }
  return "unknown";
}

const char* ToString(TraceDetail detail) {
  switch (detail) {
    case TraceDetail::kRank:
      return "rank";
    case TraceDetail::kThread:
      return "thread";
  }
  return "unknown";
}

const char* ToString(Transport transport) {
  switch (transport) {
    case Transport::kAuto:
      return "auto";
    case Transport::kShm:
      return "shm";
    case Transport::kTcp:
      return "tcp";
  }
  return "unknown";
}

const char* ToString(CsvMode mode) {
  switch (mode) {
    case CsvMode::kWrite:
      return "write";
    case CsvMode::kAppend:
      return "append";
  }
  return "unknown";
}

}  // namespace phasegap::cli

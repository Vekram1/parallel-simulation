#pragma once

#include <string>
#include <vector>

#include "phasegap/kernels/kernels.hpp"

namespace phasegap::cli {

enum class Mode { kPhaseNb, kPhaseBlk, kNbTest, kPhasePersist, kOmpTasks };
enum class MpiThread { kFunneled, kSerialized, kMultiple };
enum class Progress { kInlinePoll, kProgressThread };
enum class OmpSchedule { kStatic, kDynamic, kGuided };
enum class OmpPlaces { kCores, kThreads };
enum class SyncMode { kNone, kBarrierStart, kBarrierEach };
enum class TraceDetail { kRank, kThread };
enum class Transport { kAuto, kShm, kTcp };
enum class CsvMode { kWrite, kAppend };

struct Config {
  Mode mode;
  int ranks;
  int threads;
  int n_local;
  int halo;

  kernels::Kernel kernel = kernels::Kernel::kStencil3;
  int radius = -1;
  bool radius_set = false;
  int timesteps = 1;
  int boundary_width = 0;

  int poll_every = 1024;
  MpiThread mpi_thread = MpiThread::kFunneled;
  Progress progress = Progress::kInlinePoll;
  OmpSchedule omp_schedule = OmpSchedule::kStatic;
  int omp_chunk = 0;
  bool omp_bind = false;
  OmpPlaces omp_places = OmpPlaces::kCores;
  int flops_per_point = 0;
  int bytes_per_point = 0;

  int iters = 0;
  int warmup = 0;
  bool check = true;
  int check_every = 50;
  bool poison_ghost = false;
  SyncMode sync = SyncMode::kNone;
  bool trace = false;
  int trace_iters = 50;
  TraceDetail trace_detail = TraceDetail::kRank;
  Transport transport = Transport::kAuto;

  std::string out_dir;
  std::string run_id;
  std::string csv;
  CsvMode csv_mode = CsvMode::kWrite;
  bool manifest = true;
};

struct ParseResult {
  bool ok = false;
  bool should_exit = false;
  int exit_code = 0;
  Config config{};
  std::string error;
};

ParseResult ParseArgs(int argc, char** argv);
std::string Usage();

const char* ToString(Mode mode);
const char* ToString(kernels::Kernel kernel);
const char* ToString(MpiThread level);
const char* ToString(Progress progress);
const char* ToString(OmpSchedule schedule);
const char* ToString(OmpPlaces places);
const char* ToString(SyncMode sync_mode);
const char* ToString(TraceDetail detail);
const char* ToString(Transport transport);
const char* ToString(CsvMode mode);

}  // namespace phasegap::cli

#include <mpi.h>
#include <omp.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "phasegap/cli.hpp"
#include "phasegap/mpi/ring_halo.hpp"
#include "phasegap/stats/checksum.hpp"
#include "phasegap/stats/csv.hpp"
#include "phasegap/stats/manifest.hpp"
#include "phasegap/stats/metrics.hpp"
#include "phasegap/stats/timer.hpp"
#include "phasegap/trace/writer.hpp"

namespace {

int RequestedMpiThreadLevel(phasegap::cli::MpiThread level) {
  switch (level) {
    case phasegap::cli::MpiThread::kFunneled:
      return MPI_THREAD_FUNNELED;
    case phasegap::cli::MpiThread::kSerialized:
      return MPI_THREAD_SERIALIZED;
    case phasegap::cli::MpiThread::kMultiple:
      return MPI_THREAD_MULTIPLE;
  }
  return MPI_THREAD_FUNNELED;
}

const char* MpiThreadLevelName(int level) {
  switch (level) {
    case MPI_THREAD_SINGLE:
      return "single";
    case MPI_THREAD_FUNNELED:
      return "funneled";
    case MPI_THREAD_SERIALIZED:
      return "serialized";
    case MPI_THREAD_MULTIPLE:
      return "multiple";
  }
  return "unknown";
}

void FreePersistentRequests(MPI_Request (&reqs)[4]) {
  for (MPI_Request& req : reqs) {
    if (req != MPI_REQUEST_NULL) {
      MPI_Request_free(&req);
    }
  }
}

bool CheckMpiSuccess(int code, const char* op, int rank) {
  if (code == MPI_SUCCESS) {
    return true;
  }
  if (rank == 0) {
    char errstr[MPI_MAX_ERROR_STRING];
    int errlen = 0;
    MPI_Error_string(code, errstr, &errlen);
    std::cerr << op << " failed: " << std::string(errstr, static_cast<std::size_t>(errlen)) << '\n';
  }
  return false;
}

bool EnvIsUnsetOrEmpty(const char* key) {
  const char* val = std::getenv(key);
  return val == nullptr || val[0] == '\0';
}

void EmitRuntimeWarnings(const phasegap::cli::Config& cfg, int world_size, int rank) {
  if (rank != 0) {
    return;
  }

  if (!cfg.manifest) {
    std::cerr << "Warning: --manifest=0 disables reproducibility metadata capture for this run\n";
  }

  const int requested_workers = world_size * cfg.threads;
  const int available_workers = omp_get_num_procs();
  if (available_workers > 0 && requested_workers > available_workers) {
    std::cerr << "Warning: oversubscription detected (P*T=" << requested_workers
              << " > available_workers=" << available_workers
              << "); headline sweeps should prefer P*T <= C\n";
  }

  if (cfg.threads > 1 && EnvIsUnsetOrEmpty("OMP_PROC_BIND")) {
    std::cerr << "Warning: OMP_PROC_BIND is unset; thread placement may vary across runs\n";
  }
  if (cfg.threads > 1 && EnvIsUnsetOrEmpty("OMP_PLACES")) {
    std::cerr << "Warning: OMP_PLACES is unset; affinity/topology mapping may be unstable\n";
  }

  const char* omp_threads_env = std::getenv("OMP_NUM_THREADS");
  if (omp_threads_env != nullptr && omp_threads_env[0] != '\0') {
    char* end = nullptr;
    const long parsed = std::strtol(omp_threads_env, &end, 10);
    if (end != omp_threads_env && *end == '\0' && parsed > 0 && parsed != cfg.threads) {
      std::cerr << "Warning: OMP_NUM_THREADS=" << parsed << " differs from --threads="
                << cfg.threads << " (runtime applies --threads)\n";
    }
  }
}

omp_sched_t RequestedOmpSchedule(phasegap::cli::OmpSchedule schedule) {
  switch (schedule) {
    case phasegap::cli::OmpSchedule::kStatic:
      return omp_sched_static;
    case phasegap::cli::OmpSchedule::kDynamic:
      return omp_sched_dynamic;
    case phasegap::cli::OmpSchedule::kGuided:
      return omp_sched_guided;
  }
  return omp_sched_static;
}

double UpdatePoint(const phasegap::mpi::LocalBuffers& buffers, int i, phasegap::kernels::Kernel kernel,
                   int radius) {
  if (kernel == phasegap::kernels::Kernel::kAxpy) {
    return 1.5 * buffers.current[static_cast<std::size_t>(i)] + 0.25;
  }
  if (kernel == phasegap::kernels::Kernel::kStencil3) {
    return 0.5 * buffers.current[static_cast<std::size_t>(i)] +
           0.25 * (buffers.current[static_cast<std::size_t>(i - 1)] +
                   buffers.current[static_cast<std::size_t>(i + 1)]);
  }
  // Fallback generic radius-based stencil (covers stencil5 and explicit radius override).
  double sum = buffers.current[static_cast<std::size_t>(i)];
  for (int r = 1; r <= radius; ++r) {
    sum += buffers.current[static_cast<std::size_t>(i - r)];
    sum += buffers.current[static_cast<std::size_t>(i + r)];
  }
  return sum / static_cast<double>(2 * radius + 1);
}

}  // namespace

int main(int argc, char** argv) {
  const phasegap::cli::ParseResult parsed = phasegap::cli::ParseArgs(argc, argv);
  if (!parsed.ok) {
    std::cerr << parsed.error << '\n';
    return EXIT_FAILURE;
  }
  if (parsed.should_exit) {
    std::cout << phasegap::cli::Usage();
    return parsed.exit_code;
  }
  const phasegap::cli::Config& cfg = parsed.config;

  const int mpi_thread_requested = RequestedMpiThreadLevel(cfg.mpi_thread);
  int provided = MPI_THREAD_SINGLE;
  if (MPI_Init_thread(&argc, &argv, mpi_thread_requested, &provided) != MPI_SUCCESS) {
    std::cerr << "MPI_Init_thread failed\n";
    return EXIT_FAILURE;
  }

  int rank = 0;
  int world_size = 1;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  if (world_size != cfg.ranks) {
    if (rank == 0) {
      std::cerr << "Configured --ranks=" << cfg.ranks << " but launched with MPI world size "
                << world_size << '\n';
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  if (cfg.threads > 1 && provided < MPI_THREAD_FUNNELED) {
    if (rank == 0) {
      std::cerr << "MPI thread support mismatch: OpenMP requested --threads=" << cfg.threads
                << " but MPI provided " << MpiThreadLevelName(provided)
                << " (need at least funneled)\n";
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }
  if (provided < mpi_thread_requested && rank == 0) {
    std::cerr << "Warning: requested MPI thread level "
              << phasegap::cli::ToString(cfg.mpi_thread)
              << " but runtime provided " << MpiThreadLevelName(provided) << '\n';
  }

  if (cfg.mode == phasegap::cli::Mode::kOmpTasks) {
    if (rank == 0) {
      std::cerr << "Selected mode is declared in CLI but not implemented yet: "
                << phasegap::cli::ToString(cfg.mode) << '\n';
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }
  if (cfg.progress == phasegap::cli::Progress::kProgressThread) {
    if (rank == 0) {
      std::cerr << "--progress=progress_thread is not implemented yet\n";
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  omp_set_num_threads(cfg.threads);
  omp_set_schedule(RequestedOmpSchedule(cfg.omp_schedule), cfg.omp_chunk);
  EmitRuntimeWarnings(cfg, world_size, rank);

  const phasegap::mpi::RingNeighbors neighbors =
      phasegap::mpi::ComputeRingNeighbors(rank, world_size);
  phasegap::mpi::LocalBuffers buffers(cfg.n_local, cfg.halo);
  const bool use_persistent_requests = (cfg.mode == phasegap::cli::Mode::kPhasePersist);
  double halo_sum = 0.0;
  std::uint64_t checksum64_global = 0;
  phasegap::stats::IterationTiming timing_sum{};
  std::vector<double> t_post_samples;
  std::vector<double> t_interior_samples;
  std::vector<double> t_wait_samples;
  std::vector<double> t_boundary_samples;
  std::vector<double> t_poll_samples;
  std::vector<double> t_comm_samples;
  std::vector<double> t_iter_samples;
  std::vector<double> mpi_test_calls_samples;
  std::vector<double> polls_to_complete_samples;
  double overlap_ratio_sum_local = 0.0;
  double mpi_test_calls_sum_local = 0.0;
  double mpi_wait_calls_sum_local = 0.0;
  double polls_to_complete_sum_local = 0.0;
  int measured_iters = 0;

  if (cfg.sync == phasegap::cli::SyncMode::kBarrierStart) {
    MPI_Barrier(MPI_COMM_WORLD);
  }
  const int measured_capacity = std::max(0, cfg.iters - cfg.warmup);
  const int trace_window_iters = cfg.trace ? std::min(cfg.trace_iters, measured_capacity) : 0;
  const int trace_window_start = measured_capacity - trace_window_iters;
  t_post_samples.reserve(static_cast<std::size_t>(measured_capacity));
  t_interior_samples.reserve(static_cast<std::size_t>(measured_capacity));
  t_wait_samples.reserve(static_cast<std::size_t>(measured_capacity));
  t_boundary_samples.reserve(static_cast<std::size_t>(measured_capacity));
  t_poll_samples.reserve(static_cast<std::size_t>(measured_capacity));
  t_comm_samples.reserve(static_cast<std::size_t>(measured_capacity));
  t_iter_samples.reserve(static_cast<std::size_t>(measured_capacity));
  mpi_test_calls_samples.reserve(static_cast<std::size_t>(measured_capacity));
  polls_to_complete_samples.reserve(static_cast<std::size_t>(measured_capacity));
  std::vector<double> trace_thread_interior_local(
      static_cast<std::size_t>(trace_window_iters * cfg.threads), 0.0);
  std::vector<double> trace_thread_boundary_local(
      static_cast<std::size_t>(trace_window_iters * cfg.threads), 0.0);

  std::vector<double> send_left(static_cast<std::size_t>(cfg.halo), 0.0);
  std::vector<double> send_right(static_cast<std::size_t>(cfg.halo), 0.0);
  std::vector<double> recv_left(static_cast<std::size_t>(cfg.halo), 0.0);
  std::vector<double> recv_right(static_cast<std::size_t>(cfg.halo), 0.0);
  MPI_Request persistent_reqs[4] = {MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL,
                                    MPI_REQUEST_NULL};
  if (use_persistent_requests) {
    if (!CheckMpiSuccess(MPI_Recv_init(recv_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 101,
                                       MPI_COMM_WORLD, &persistent_reqs[0]),
                         "MPI_Recv_init(left)", rank) ||
        !CheckMpiSuccess(MPI_Recv_init(recv_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right,
                                       100, MPI_COMM_WORLD, &persistent_reqs[1]),
                         "MPI_Recv_init(right)", rank) ||
        !CheckMpiSuccess(MPI_Send_init(send_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 100,
                                       MPI_COMM_WORLD, &persistent_reqs[2]),
                         "MPI_Send_init(left)", rank) ||
        !CheckMpiSuccess(MPI_Send_init(send_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right,
                                       101, MPI_COMM_WORLD, &persistent_reqs[3]),
                         "MPI_Send_init(right)", rank)) {
      FreePersistentRequests(persistent_reqs);
      MPI_Finalize();
      return EXIT_FAILURE;
    }
  }

  for (int iter = 0; iter < cfg.iters; ++iter) {
    phasegap::stats::IterationTimer timer;
    int mpi_test_calls = 0;
    int mpi_wait_calls = 0;
    int polls_to_complete = 0;
    int trace_slot = -1;
    if (cfg.trace && iter >= cfg.warmup) {
      const int measured_idx = iter - cfg.warmup;
      if (measured_idx >= trace_window_start) {
        trace_slot = measured_idx - trace_window_start;
      }
    }
    if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread) {
      std::fill_n(&trace_thread_interior_local[static_cast<std::size_t>(trace_slot * cfg.threads)],
                  static_cast<std::size_t>(cfg.threads), 0.0);
      std::fill_n(&trace_thread_boundary_local[static_cast<std::size_t>(trace_slot * cfg.threads)],
                  static_cast<std::size_t>(cfg.threads), 0.0);
    }
    timer.BeginIteration();

    if (cfg.sync == phasegap::cli::SyncMode::kBarrierEach) {
      MPI_Barrier(MPI_COMM_WORLD);
    }
    for (int i = 0; i < buffers.n_local; ++i) {
      buffers.current[static_cast<std::size_t>(buffers.OwnedBegin() + i)] =
          static_cast<double>(rank * 1000000 + iter * 100 + i);
      buffers.next[static_cast<std::size_t>(buffers.OwnedBegin() + i)] = 0.0;
    }

    if (cfg.poison_ghost) {
      const double nan = std::numeric_limits<double>::quiet_NaN();
      for (int i = buffers.LeftGhostBegin(); i < buffers.LeftGhostEnd(); ++i) {
        buffers.current[static_cast<std::size_t>(i)] = nan;
      }
      for (int i = buffers.RightGhostBegin(); i < buffers.RightGhostEnd(); ++i) {
        buffers.current[static_cast<std::size_t>(i)] = nan;
      }
    }

    for (int i = 0; i < cfg.halo; ++i) {
      send_left[static_cast<std::size_t>(i)] =
          buffers.current[static_cast<std::size_t>(buffers.OwnedBegin() + i)];
      send_right[static_cast<std::size_t>(i)] =
          buffers.current[static_cast<std::size_t>(buffers.OwnedEnd() - cfg.halo + i)];
    }

    if (cfg.mode == phasegap::cli::Mode::kPhaseBlk) {
      timer.BeginCommWindow();
      timer.BeginWait();
      bool mpi_ok = true;
      mpi_ok = mpi_ok && CheckMpiSuccess(
                           MPI_Sendrecv(send_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 100,
                                        recv_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right, 100,
                                        MPI_COMM_WORLD, MPI_STATUS_IGNORE),
                           "MPI_Sendrecv(left->right)", rank);
      mpi_ok = mpi_ok && CheckMpiSuccess(
                           MPI_Sendrecv(send_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right,
                                        101, recv_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left,
                                        101, MPI_COMM_WORLD, MPI_STATUS_IGNORE),
                           "MPI_Sendrecv(right->left)", rank);
      timer.EndWait();
      timer.EndCommWindow();
      if (!mpi_ok) {
        if (use_persistent_requests) {
          FreePersistentRequests(persistent_reqs);
        }
        MPI_Finalize();
        return EXIT_FAILURE;
      }

      phasegap::mpi::UnpackLeftGhost(&buffers, recv_left);
      phasegap::mpi::UnpackRightGhost(&buffers, recv_right);

      const int interior_begin = buffers.OwnedBegin() + cfg.boundary_width;
      const int interior_end = buffers.OwnedEnd() - cfg.boundary_width;
      const int left_boundary_end = std::min(buffers.OwnedBegin() + cfg.boundary_width, buffers.OwnedEnd());
      const int right_boundary_begin =
          std::max(left_boundary_end, buffers.OwnedEnd() - cfg.boundary_width);
      double interior_work_us = 0.0;

#pragma omp parallel
      {
        const int tid = omp_get_thread_num();
        const phasegap::stats::TimePoint interior_start = phasegap::stats::SteadyClock::now();
#pragma omp for nowait schedule(runtime)
        for (int i = interior_begin; i < interior_end; ++i) {
          buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
        }
        const double thread_interior_us =
            phasegap::stats::DurationMicros(interior_start, phasegap::stats::SteadyClock::now());
        if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread &&
            tid < cfg.threads) {
          trace_thread_interior_local[static_cast<std::size_t>(trace_slot * cfg.threads + tid)] =
              thread_interior_us;
        }
#pragma omp critical
        {
          interior_work_us = std::max(interior_work_us, thread_interior_us);
        }
#pragma omp barrier
#pragma omp master
        timer.SetInteriorMicros(interior_work_us);

#pragma omp master
        timer.BeginBoundary();
        const phasegap::stats::TimePoint boundary_start = phasegap::stats::SteadyClock::now();
#pragma omp for schedule(runtime)
        for (int i = buffers.OwnedBegin(); i < left_boundary_end; ++i) {
          buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
        }
#pragma omp for schedule(runtime)
        for (int i = right_boundary_begin; i < buffers.OwnedEnd(); ++i) {
          buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
        }
        const double thread_boundary_us =
            phasegap::stats::DurationMicros(boundary_start, phasegap::stats::SteadyClock::now());
        if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread &&
            tid < cfg.threads) {
          trace_thread_boundary_local[static_cast<std::size_t>(trace_slot * cfg.threads + tid)] =
              thread_boundary_us;
        }
#pragma omp master
        timer.EndBoundary();
      }
    } else {
      MPI_Request reqs[4] = {MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL};
      MPI_Request* active_reqs = reqs;
      bool mpi_ok = true;
      timer.BeginCommWindow();
      timer.BeginPost();
      if (use_persistent_requests) {
        mpi_ok = CheckMpiSuccess(MPI_Startall(4, persistent_reqs), "MPI_Startall", rank);
        active_reqs = persistent_reqs;
      } else {
        mpi_ok = mpi_ok && CheckMpiSuccess(
                             MPI_Irecv(recv_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 101,
                                       MPI_COMM_WORLD, &reqs[0]),
                             "MPI_Irecv(left)", rank);
        mpi_ok = mpi_ok && CheckMpiSuccess(
                             MPI_Irecv(recv_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right,
                                       100, MPI_COMM_WORLD, &reqs[1]),
                             "MPI_Irecv(right)", rank);
        mpi_ok = mpi_ok && CheckMpiSuccess(
                             MPI_Isend(send_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 100,
                                       MPI_COMM_WORLD, &reqs[2]),
                             "MPI_Isend(left)", rank);
        mpi_ok = mpi_ok && CheckMpiSuccess(
                             MPI_Isend(send_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right,
                                       101, MPI_COMM_WORLD, &reqs[3]),
                             "MPI_Isend(right)", rank);
      }
      timer.EndPost();
      if (!mpi_ok) {
        if (use_persistent_requests) {
          FreePersistentRequests(persistent_reqs);
        }
        MPI_Finalize();
        return EXIT_FAILURE;
      }

      const int interior_begin = buffers.OwnedBegin() + cfg.boundary_width;
      const int interior_end = buffers.OwnedEnd() - cfg.boundary_width;
      const int left_boundary_end = std::min(buffers.OwnedBegin() + cfg.boundary_width, buffers.OwnedEnd());
      const int right_boundary_begin =
          std::max(left_boundary_end, buffers.OwnedEnd() - cfg.boundary_width);

      if (cfg.mode == phasegap::cli::Mode::kNbTest) {
        int completed = 0;
        double interior_work_us = 0.0;
#pragma omp parallel shared(completed, interior_work_us)
        {
          const int tid = omp_get_thread_num();
          // Measure each worker's interior compute span and use max to avoid
          // master-thread barrier/poll skew inflating interior duration.
          const phasegap::stats::TimePoint interior_start = phasegap::stats::SteadyClock::now();
#pragma omp for nowait schedule(runtime)
          for (int i = interior_begin; i < interior_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
          const double thread_interior_us =
              phasegap::stats::DurationMicros(interior_start, phasegap::stats::SteadyClock::now());
          if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread &&
              tid < cfg.threads) {
            trace_thread_interior_local[static_cast<std::size_t>(trace_slot * cfg.threads + tid)] =
                thread_interior_us;
          }
#pragma omp critical
          {
            interior_work_us = std::max(interior_work_us, thread_interior_us);
          }

#pragma omp master
          {
            while (!completed) {
              volatile std::uint64_t poll_budget = 0;
              for (int k = 0; k < cfg.poll_every; ++k) {
                poll_budget += static_cast<std::uint64_t>(k);
              }
              timer.BeginPoll();
              if (!CheckMpiSuccess(MPI_Testall(4, active_reqs, &completed, MPI_STATUSES_IGNORE),
                                   "MPI_Testall", rank)) {
                mpi_ok = false;
                completed = 1;
              }
              ++mpi_test_calls;
              ++polls_to_complete;
              timer.EndPoll();
            }
          }

#pragma omp barrier

#pragma omp master
          timer.SetInteriorMicros(interior_work_us);

#pragma omp master
          {
            timer.BeginWait();
            if (!completed && mpi_ok) {
              mpi_ok = CheckMpiSuccess(MPI_Waitall(4, active_reqs, MPI_STATUSES_IGNORE),
                                       "MPI_Waitall", rank);
              if (mpi_ok) {
                ++mpi_wait_calls;
              }
            }
            timer.EndWait();
            timer.EndCommWindow();
            phasegap::mpi::UnpackLeftGhost(&buffers, recv_left);
            phasegap::mpi::UnpackRightGhost(&buffers, recv_right);
          }

#pragma omp barrier

#pragma omp master
          timer.BeginBoundary();
          const phasegap::stats::TimePoint boundary_start = phasegap::stats::SteadyClock::now();
#pragma omp for schedule(runtime)
          for (int i = buffers.OwnedBegin(); i < left_boundary_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
#pragma omp for schedule(runtime)
          for (int i = right_boundary_begin; i < buffers.OwnedEnd(); ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
          const double thread_boundary_us =
              phasegap::stats::DurationMicros(boundary_start, phasegap::stats::SteadyClock::now());
          if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread &&
              tid < cfg.threads) {
            trace_thread_boundary_local[static_cast<std::size_t>(trace_slot * cfg.threads + tid)] =
                thread_boundary_us;
          }
#pragma omp master
          timer.EndBoundary();
        }
      } else {
        double interior_work_us = 0.0;
#pragma omp parallel
        {
          const int tid = omp_get_thread_num();
          // Interior compute should overlap communication in nonblocking modes.
          const phasegap::stats::TimePoint interior_start = phasegap::stats::SteadyClock::now();
#pragma omp for nowait schedule(runtime)
          for (int i = interior_begin; i < interior_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
          const double thread_interior_us =
              phasegap::stats::DurationMicros(interior_start, phasegap::stats::SteadyClock::now());
          if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread &&
              tid < cfg.threads) {
            trace_thread_interior_local[static_cast<std::size_t>(trace_slot * cfg.threads + tid)] =
                thread_interior_us;
          }
#pragma omp critical
          {
            interior_work_us = std::max(interior_work_us, thread_interior_us);
          }
#pragma omp barrier
#pragma omp master
          timer.SetInteriorMicros(interior_work_us);

#pragma omp master
          {
            timer.BeginWait();
            mpi_ok = CheckMpiSuccess(MPI_Waitall(4, active_reqs, MPI_STATUSES_IGNORE),
                                     "MPI_Waitall", rank);
            if (mpi_ok) {
              ++mpi_wait_calls;
            }
            timer.EndWait();
            timer.EndCommWindow();
            phasegap::mpi::UnpackLeftGhost(&buffers, recv_left);
            phasegap::mpi::UnpackRightGhost(&buffers, recv_right);
          }

#pragma omp barrier

#pragma omp master
          timer.BeginBoundary();
          const phasegap::stats::TimePoint boundary_start = phasegap::stats::SteadyClock::now();
#pragma omp for schedule(runtime)
          for (int i = buffers.OwnedBegin(); i < left_boundary_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
#pragma omp for schedule(runtime)
          for (int i = right_boundary_begin; i < buffers.OwnedEnd(); ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
          const double thread_boundary_us =
              phasegap::stats::DurationMicros(boundary_start, phasegap::stats::SteadyClock::now());
          if (trace_slot >= 0 && cfg.trace_detail == phasegap::cli::TraceDetail::kThread &&
              tid < cfg.threads) {
            trace_thread_boundary_local[static_cast<std::size_t>(trace_slot * cfg.threads + tid)] =
                thread_boundary_us;
          }
#pragma omp master
          timer.EndBoundary();
        }
      }
      if (!mpi_ok) {
        if (use_persistent_requests) {
          FreePersistentRequests(persistent_reqs);
        }
        MPI_Finalize();
        return EXIT_FAILURE;
      }
    }

    halo_sum = std::accumulate(send_left.begin(), send_left.end(), 0.0) +
               std::accumulate(send_right.begin(), send_right.end(), 0.0);

    buffers.current.swap(buffers.next);

    const bool should_check =
        cfg.check && (((iter + 1) % cfg.check_every == 0) || (iter + 1 == cfg.iters));
    if (should_check) {
      const std::uint64_t local = phasegap::stats::ChecksumOwnedRegion(buffers);
      std::uint64_t reduced = 0;
      MPI_Allreduce(&local, &reduced, 1, MPI_UINT64_T, MPI_BXOR, MPI_COMM_WORLD);
      checksum64_global = reduced;
    }

    timer.EndIteration();
    if (iter >= cfg.warmup) {
      const phasegap::stats::IterationTiming& t = timer.Current();
      timing_sum.t_post_us += t.t_post_us;
      timing_sum.t_interior_us += t.t_interior_us;
      timing_sum.t_wait_us += t.t_wait_us;
      timing_sum.t_boundary_us += t.t_boundary_us;
      timing_sum.t_poll_us += t.t_poll_us;
      timing_sum.t_comm_window_us += t.t_comm_window_us;
      timing_sum.t_iter_us += t.t_iter_us;
      t_post_samples.push_back(t.t_post_us);
      t_interior_samples.push_back(t.t_interior_us);
      t_wait_samples.push_back(t.t_wait_us);
      t_boundary_samples.push_back(t.t_boundary_us);
      t_poll_samples.push_back(t.t_poll_us);
      t_comm_samples.push_back(t.t_comm_window_us);
      t_iter_samples.push_back(t.t_iter_us);
      mpi_test_calls_samples.push_back(static_cast<double>(mpi_test_calls));
      overlap_ratio_sum_local +=
          phasegap::stats::OverlapRatio(t.t_comm_window_us, t.t_interior_us, t.t_wait_us);
      mpi_test_calls_sum_local += static_cast<double>(mpi_test_calls);
      mpi_wait_calls_sum_local += static_cast<double>(mpi_wait_calls);
      polls_to_complete_sum_local += static_cast<double>(polls_to_complete);
      polls_to_complete_samples.push_back(static_cast<double>(polls_to_complete));
      ++measured_iters;
    }
  }

  if (use_persistent_requests) {
    FreePersistentRequests(persistent_reqs);
  }

  if (measured_iters == 0) {
    if (rank == 0) {
      std::cerr << "No measured iterations available; ensure --iters > --warmup\n";
    }
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  int omp_threads = 0;
#pragma omp parallel
  {
#pragma omp master
    { omp_threads = omp_get_num_threads(); }
  }

  const double measured_iters_d = static_cast<double>(measured_iters);
  phasegap::stats::IterationTiming timing_mean_local{};
  timing_mean_local.t_post_us = timing_sum.t_post_us / measured_iters_d;
  timing_mean_local.t_interior_us = timing_sum.t_interior_us / measured_iters_d;
  timing_mean_local.t_wait_us = timing_sum.t_wait_us / measured_iters_d;
  timing_mean_local.t_boundary_us = timing_sum.t_boundary_us / measured_iters_d;
  timing_mean_local.t_poll_us = timing_sum.t_poll_us / measured_iters_d;
  timing_mean_local.t_comm_window_us = timing_sum.t_comm_window_us / measured_iters_d;
  timing_mean_local.t_iter_us = timing_sum.t_iter_us / measured_iters_d;
  const double overlap_local = overlap_ratio_sum_local / measured_iters_d;
  const double mpi_test_calls_mean_local = mpi_test_calls_sum_local / measured_iters_d;
  const double mpi_wait_calls_mean_local = mpi_wait_calls_sum_local / measured_iters_d;
  const double polls_to_complete_mean_local = polls_to_complete_sum_local / measured_iters_d;
  const double polls_to_complete_p95_local =
      phasegap::stats::Percentile(polls_to_complete_samples, 0.95);
  const double t_iter_p50_local = phasegap::stats::Percentile(t_iter_samples, 0.50);
  const double t_iter_p95_local = phasegap::stats::Percentile(t_iter_samples, 0.95);
  const double t_post_p50_local = phasegap::stats::Percentile(t_post_samples, 0.50);
  const double t_post_p95_local = phasegap::stats::Percentile(t_post_samples, 0.95);
  const double t_interior_p50_local = phasegap::stats::Percentile(t_interior_samples, 0.50);
  const double t_interior_p95_local = phasegap::stats::Percentile(t_interior_samples, 0.95);
  const double t_wait_p50_local = phasegap::stats::Percentile(t_wait_samples, 0.50);
  const double t_wait_p95_local = phasegap::stats::Percentile(t_wait_samples, 0.95);
  const double t_boundary_p50_local = phasegap::stats::Percentile(t_boundary_samples, 0.50);
  const double t_boundary_p95_local = phasegap::stats::Percentile(t_boundary_samples, 0.95);
  const double t_poll_p50_local = phasegap::stats::Percentile(t_poll_samples, 0.50);
  const double t_poll_p95_local = phasegap::stats::Percentile(t_poll_samples, 0.95);
  const double t_comm_p50_local = phasegap::stats::Percentile(t_comm_samples, 0.50);
  const double t_comm_p95_local = phasegap::stats::Percentile(t_comm_samples, 0.95);

  double t_post_sum = 0.0;
  double t_post_max = 0.0;
  double t_interior_sum = 0.0;
  double t_interior_max = 0.0;
  double t_wait_sum = 0.0;
  double t_wait_max = 0.0;
  double t_boundary_sum = 0.0;
  double t_boundary_max = 0.0;
  double t_poll_sum = 0.0;
  double t_poll_max = 0.0;
  double t_iter_sum = 0.0;
  double t_iter_max = 0.0;
  double t_comm_sum = 0.0;
  double t_comm_max = 0.0;
  double t_iter_p50_sum = 0.0;
  double t_iter_p95_sum = 0.0;
  double t_post_p50_sum = 0.0;
  double t_post_p95_sum = 0.0;
  double t_interior_p50_sum = 0.0;
  double t_interior_p95_sum = 0.0;
  double t_wait_p50_sum = 0.0;
  double t_wait_p95_sum = 0.0;
  double t_boundary_p50_sum = 0.0;
  double t_boundary_p95_sum = 0.0;
  double t_poll_p50_sum = 0.0;
  double t_poll_p95_sum = 0.0;
  double t_comm_p50_sum = 0.0;
  double t_comm_p95_sum = 0.0;
  double overlap_sum = 0.0;
  double mpi_test_calls_sum = 0.0;
  double mpi_wait_calls_sum = 0.0;
  double polls_to_complete_mean_sum = 0.0;
  double polls_to_complete_p95_sum = 0.0;
  MPI_Allreduce(&timing_mean_local.t_post_us, &t_post_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_post_us, &t_post_max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_interior_us, &t_interior_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_interior_us, &t_interior_max, 1, MPI_DOUBLE, MPI_MAX,
                MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_wait_us, &t_wait_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_wait_us, &t_wait_max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_boundary_us, &t_boundary_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_boundary_us, &t_boundary_max, 1, MPI_DOUBLE, MPI_MAX,
                MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_poll_us, &t_poll_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_poll_us, &t_poll_max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_iter_us, &t_iter_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_iter_us, &t_iter_max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_comm_window_us, &t_comm_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_comm_window_us, &t_comm_max, 1, MPI_DOUBLE, MPI_MAX,
                MPI_COMM_WORLD);
  MPI_Allreduce(&t_iter_p50_local, &t_iter_p50_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_iter_p95_local, &t_iter_p95_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_post_p50_local, &t_post_p50_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_post_p95_local, &t_post_p95_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_interior_p50_local, &t_interior_p50_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_interior_p95_local, &t_interior_p95_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&t_wait_p50_local, &t_wait_p50_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_wait_p95_local, &t_wait_p95_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_boundary_p50_local, &t_boundary_p50_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&t_boundary_p95_local, &t_boundary_p95_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&t_poll_p50_local, &t_poll_p50_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_poll_p95_local, &t_poll_p95_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_comm_p50_local, &t_comm_p50_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&t_comm_p95_local, &t_comm_p95_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&overlap_local, &overlap_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&mpi_test_calls_mean_local, &mpi_test_calls_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&mpi_wait_calls_mean_local, &mpi_wait_calls_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&polls_to_complete_mean_local, &polls_to_complete_mean_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&polls_to_complete_p95_local, &polls_to_complete_p95_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);

  const double t_post_mean_avg = t_post_sum / static_cast<double>(world_size);
  const double t_interior_mean_avg = t_interior_sum / static_cast<double>(world_size);
  const double t_wait_mean_avg = t_wait_sum / static_cast<double>(world_size);
  const double t_boundary_mean_avg = t_boundary_sum / static_cast<double>(world_size);
  const double t_poll_mean_avg = t_poll_sum / static_cast<double>(world_size);
  const double t_iter_mean_avg = t_iter_sum / static_cast<double>(world_size);
  const double t_comm_mean_avg = t_comm_sum / static_cast<double>(world_size);
  const double t_post_mean_max = t_post_max;
  const double t_interior_mean_max = t_interior_max;
  const double t_wait_mean_max = t_wait_max;
  const double t_boundary_mean_max = t_boundary_max;
  const double t_poll_mean_max = t_poll_max;
  const double t_iter_mean_max = t_iter_max;
  const double t_comm_mean_max = t_comm_max;
  const double t_iter_p50_avg = t_iter_p50_sum / static_cast<double>(world_size);
  const double t_iter_p95_avg = t_iter_p95_sum / static_cast<double>(world_size);
  const double t_post_p50_avg = t_post_p50_sum / static_cast<double>(world_size);
  const double t_post_p95_avg = t_post_p95_sum / static_cast<double>(world_size);
  const double t_interior_p50_avg = t_interior_p50_sum / static_cast<double>(world_size);
  const double t_interior_p95_avg = t_interior_p95_sum / static_cast<double>(world_size);
  const double t_wait_p50_avg = t_wait_p50_sum / static_cast<double>(world_size);
  const double t_wait_p95_avg = t_wait_p95_sum / static_cast<double>(world_size);
  const double t_boundary_p50_avg = t_boundary_p50_sum / static_cast<double>(world_size);
  const double t_boundary_p95_avg = t_boundary_p95_sum / static_cast<double>(world_size);
  const double t_poll_p50_avg = t_poll_p50_sum / static_cast<double>(world_size);
  const double t_poll_p95_avg = t_poll_p95_sum / static_cast<double>(world_size);
  const double t_comm_p50_avg = t_comm_p50_sum / static_cast<double>(world_size);
  const double t_comm_p95_avg = t_comm_p95_sum / static_cast<double>(world_size);
  const double overlap_ratio_avg = overlap_sum / static_cast<double>(world_size);
  const double mpi_test_calls_mean_avg = mpi_test_calls_sum / static_cast<double>(world_size);
  const double mpi_wait_calls_mean_avg = mpi_wait_calls_sum / static_cast<double>(world_size);
  const double polls_to_complete_mean_avg =
      polls_to_complete_mean_sum / static_cast<double>(world_size);
  const double polls_to_complete_p95_avg =
      polls_to_complete_p95_sum / static_cast<double>(world_size);

  std::vector<double> trace_post_global;
  std::vector<double> trace_interior_global;
  std::vector<double> trace_wait_global;
  std::vector<double> trace_boundary_global;
  std::vector<double> trace_iter_global;
  std::vector<double> trace_wait_frac_global;
  std::vector<double> trace_mpi_test_calls_global;
  std::vector<std::uint64_t> trace_bytes_total_global;
  std::vector<double> trace_thread_interior_global;
  std::vector<double> trace_thread_boundary_global;

  if (cfg.trace && trace_window_iters > 0) {
    std::vector<double> trace_post_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<double> trace_interior_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<double> trace_wait_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<double> trace_boundary_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<double> trace_iter_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<double> trace_wait_frac_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<double> trace_mpi_test_calls_local(static_cast<std::size_t>(trace_window_iters), 0.0);
    std::vector<std::uint64_t> trace_bytes_total_local(static_cast<std::size_t>(trace_window_iters),
                                                       static_cast<std::uint64_t>(2) *
                                                           static_cast<std::uint64_t>(cfg.halo) *
                                                           static_cast<std::uint64_t>(sizeof(double)));

    for (int i = 0; i < trace_window_iters; ++i) {
      const int idx = trace_window_start + i;
      trace_post_local[static_cast<std::size_t>(i)] = t_post_samples[static_cast<std::size_t>(idx)];
      trace_interior_local[static_cast<std::size_t>(i)] =
          t_interior_samples[static_cast<std::size_t>(idx)];
      trace_wait_local[static_cast<std::size_t>(i)] = t_wait_samples[static_cast<std::size_t>(idx)];
      trace_boundary_local[static_cast<std::size_t>(i)] =
          t_boundary_samples[static_cast<std::size_t>(idx)];
      trace_iter_local[static_cast<std::size_t>(i)] = t_iter_samples[static_cast<std::size_t>(idx)];
      trace_wait_frac_local[static_cast<std::size_t>(i)] = phasegap::stats::WaitFraction(
          trace_wait_local[static_cast<std::size_t>(i)], trace_iter_local[static_cast<std::size_t>(i)]);
      trace_mpi_test_calls_local[static_cast<std::size_t>(i)] =
          mpi_test_calls_samples[static_cast<std::size_t>(idx)];
    }

    const std::size_t gathered_size =
        static_cast<std::size_t>(world_size * trace_window_iters);
    if (rank == 0) {
      trace_post_global.resize(gathered_size);
      trace_interior_global.resize(gathered_size);
      trace_wait_global.resize(gathered_size);
      trace_boundary_global.resize(gathered_size);
      trace_iter_global.resize(gathered_size);
      trace_wait_frac_global.resize(gathered_size);
      trace_mpi_test_calls_global.resize(gathered_size);
      trace_bytes_total_global.resize(gathered_size);
    }

    MPI_Gather(trace_post_local.data(), trace_window_iters, MPI_DOUBLE, trace_post_global.data(),
               trace_window_iters, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(trace_interior_local.data(), trace_window_iters, MPI_DOUBLE,
               trace_interior_global.data(), trace_window_iters, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(trace_wait_local.data(), trace_window_iters, MPI_DOUBLE, trace_wait_global.data(),
               trace_window_iters, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(trace_boundary_local.data(), trace_window_iters, MPI_DOUBLE,
               trace_boundary_global.data(), trace_window_iters, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(trace_iter_local.data(), trace_window_iters, MPI_DOUBLE, trace_iter_global.data(),
               trace_window_iters, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(trace_wait_frac_local.data(), trace_window_iters, MPI_DOUBLE,
               trace_wait_frac_global.data(), trace_window_iters, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(trace_mpi_test_calls_local.data(), trace_window_iters, MPI_DOUBLE,
               trace_mpi_test_calls_global.data(), trace_window_iters, MPI_DOUBLE, 0,
               MPI_COMM_WORLD);
    MPI_Gather(trace_bytes_total_local.data(), trace_window_iters, MPI_UINT64_T,
               trace_bytes_total_global.data(), trace_window_iters, MPI_UINT64_T, 0,
               MPI_COMM_WORLD);

    if (cfg.trace_detail == phasegap::cli::TraceDetail::kThread) {
      const int thread_payload = trace_window_iters * cfg.threads;
      const std::size_t thread_gather_size =
          static_cast<std::size_t>(world_size) * static_cast<std::size_t>(thread_payload);
      if (rank == 0) {
        trace_thread_interior_global.resize(thread_gather_size);
        trace_thread_boundary_global.resize(thread_gather_size);
      }
      MPI_Gather(trace_thread_interior_local.data(), thread_payload, MPI_DOUBLE,
                 trace_thread_interior_global.data(), thread_payload, MPI_DOUBLE, 0,
                 MPI_COMM_WORLD);
      MPI_Gather(trace_thread_boundary_local.data(), thread_payload, MPI_DOUBLE,
                 trace_thread_boundary_global.data(), thread_payload, MPI_DOUBLE, 0,
                 MPI_COMM_WORLD);
    }
  }

  if (rank == 0) {
    const double wait_frac = phasegap::stats::WaitFraction(t_wait_mean_avg, t_iter_mean_avg);
    const double wait_skew = phasegap::stats::WaitSkew(t_wait_mean_max, t_wait_mean_avg);
    const phasegap::stats::BandwidthMetrics bw = phasegap::stats::ComputeBandwidthMetrics(
        cfg.halo, sizeof(double), t_comm_mean_avg);
    phasegap::stats::CsvSummary csv_summary{};
    csv_summary.ranks = world_size;
    csv_summary.omp_threads = omp_threads;
    csv_summary.mpi_thread_provided = provided;
    csv_summary.measured_iters = measured_iters;
    csv_summary.checksum64 = checksum64_global;
    csv_summary.msg_bytes = bw.msg_bytes;
    csv_summary.bytes_total = bw.bytes_total;
    csv_summary.t_iter_us = t_iter_mean_avg;
    csv_summary.t_post_us = t_post_mean_avg;
    csv_summary.t_interior_us = t_interior_mean_avg;
    csv_summary.t_wait_us = t_wait_mean_avg;
    csv_summary.t_boundary_us = t_boundary_mean_avg;
    csv_summary.t_poll_us = t_poll_mean_avg;
    csv_summary.t_comm_window_us = t_comm_mean_avg;
    csv_summary.t_iter_mean_max_us = t_iter_mean_max;
    csv_summary.t_post_mean_max_us = t_post_mean_max;
    csv_summary.t_interior_mean_max_us = t_interior_mean_max;
    csv_summary.t_wait_mean_max_us = t_wait_mean_max;
    csv_summary.t_boundary_mean_max_us = t_boundary_mean_max;
    csv_summary.t_poll_mean_max_us = t_poll_mean_max;
    csv_summary.t_comm_window_mean_max_us = t_comm_mean_max;
    csv_summary.t_iter_p50_us = t_iter_p50_avg;
    csv_summary.t_iter_p95_us = t_iter_p95_avg;
    csv_summary.t_post_p50_us = t_post_p50_avg;
    csv_summary.t_post_p95_us = t_post_p95_avg;
    csv_summary.t_interior_p50_us = t_interior_p50_avg;
    csv_summary.t_interior_p95_us = t_interior_p95_avg;
    csv_summary.t_wait_p50_us = t_wait_p50_avg;
    csv_summary.t_wait_p95_us = t_wait_p95_avg;
    csv_summary.t_boundary_p50_us = t_boundary_p50_avg;
    csv_summary.t_boundary_p95_us = t_boundary_p95_avg;
    csv_summary.t_poll_p50_us = t_poll_p50_avg;
    csv_summary.t_poll_p95_us = t_poll_p95_avg;
    csv_summary.t_comm_window_p50_us = t_comm_p50_avg;
    csv_summary.t_comm_window_p95_us = t_comm_p95_avg;
    csv_summary.wait_frac = wait_frac;
    csv_summary.wait_skew = wait_skew;
    csv_summary.overlap_ratio = overlap_ratio_avg;
    csv_summary.bw_effective_bytes_per_us = bw.bw_effective_bytes_per_us;
    csv_summary.mpi_test_calls = mpi_test_calls_mean_avg;
    csv_summary.mpi_wait_calls = mpi_wait_calls_mean_avg;
    csv_summary.polls_to_complete_mean = polls_to_complete_mean_avg;
    csv_summary.polls_to_complete_p95 = polls_to_complete_p95_avg;
    const bool csv_ok = phasegap::stats::WriteCsvRow(cfg, csv_summary, "csv");
    if (!csv_ok) {
      MPI_Finalize();
      return EXIT_FAILURE;
    }

    if (cfg.manifest) {
      phasegap::stats::RuntimeSummary summary{};
      summary.ranks = world_size;
      summary.omp_threads = omp_threads;
      summary.measured_iters = measured_iters;
      summary.checksum64 = checksum64_global;
      summary.t_iter_us = t_iter_mean_avg;
      summary.t_post_us = t_post_mean_avg;
      summary.t_interior_us = t_interior_mean_avg;
      summary.t_wait_us = t_wait_mean_avg;
      summary.t_boundary_us = t_boundary_mean_avg;
      summary.t_poll_us = t_poll_mean_avg;
      summary.t_comm_window_us = t_comm_mean_avg;
      summary.t_iter_mean_max_us = t_iter_mean_max;
      summary.t_post_mean_max_us = t_post_mean_max;
      summary.t_interior_mean_max_us = t_interior_mean_max;
      summary.t_wait_mean_max_us = t_wait_mean_max;
      summary.t_boundary_mean_max_us = t_boundary_mean_max;
      summary.t_poll_mean_max_us = t_poll_mean_max;
      summary.t_comm_window_mean_max_us = t_comm_mean_max;
      summary.t_iter_p50_us = t_iter_p50_avg;
      summary.t_iter_p95_us = t_iter_p95_avg;
      summary.t_post_p50_us = t_post_p50_avg;
      summary.t_post_p95_us = t_post_p95_avg;
      summary.t_interior_p50_us = t_interior_p50_avg;
      summary.t_interior_p95_us = t_interior_p95_avg;
      summary.t_wait_p50_us = t_wait_p50_avg;
      summary.t_wait_p95_us = t_wait_p95_avg;
      summary.t_boundary_p50_us = t_boundary_p50_avg;
      summary.t_boundary_p95_us = t_boundary_p95_avg;
      summary.t_poll_p50_us = t_poll_p50_avg;
      summary.t_poll_p95_us = t_poll_p95_avg;
      summary.t_comm_window_p50_us = t_comm_p50_avg;
      summary.t_comm_window_p95_us = t_comm_p95_avg;
      summary.wait_frac = wait_frac;
      summary.wait_skew = wait_skew;
      summary.overlap_ratio = overlap_ratio_avg;
      summary.mpi_test_calls = mpi_test_calls_mean_avg;
      summary.mpi_wait_calls = mpi_wait_calls_mean_avg;
      summary.polls_to_complete_mean = polls_to_complete_mean_avg;
      summary.polls_to_complete_p95 = polls_to_complete_p95_avg;
      const bool manifest_ok =
          phasegap::stats::WriteManifest(cfg, summary, provided, "manifest");
      if (!manifest_ok) {
        MPI_Finalize();
        return EXIT_FAILURE;
      }
    }

    if (cfg.trace) {
      phasegap::trace::TraceSummary trace_summary{};
      trace_summary.ranks = world_size;
      trace_summary.omp_threads = omp_threads;
      trace_summary.measured_iters = measured_iters;
      trace_summary.trace_iters = trace_window_iters;
      trace_summary.mpi_thread_provided = provided;
      trace_summary.t_post_us = t_post_mean_avg;
      trace_summary.t_interior_us = t_interior_mean_avg;
      trace_summary.t_wait_us = t_wait_mean_avg;
      trace_summary.t_boundary_us = t_boundary_mean_avg;
      trace_summary.t_iter_us = t_iter_mean_avg;
      trace_summary.wait_frac = wait_frac;
      trace_summary.overlap_ratio = overlap_ratio_avg;
      trace_summary.trace_window_start_iter = trace_window_start;
      trace_summary.bytes_total = bw.bytes_total;
      if (trace_window_iters > 0) {
        trace_summary.rank_iterations.resize(
            static_cast<std::size_t>(world_size * trace_window_iters));
        for (int r = 0; r < world_size; ++r) {
          for (int i = 0; i < trace_window_iters; ++i) {
            const int idx = r * trace_window_iters + i;
            phasegap::trace::TraceSummary::RankIteration row{};
            row.t_post_us = trace_post_global[static_cast<std::size_t>(idx)];
            row.t_interior_us = trace_interior_global[static_cast<std::size_t>(idx)];
            row.t_wait_us = trace_wait_global[static_cast<std::size_t>(idx)];
            row.t_boundary_us = trace_boundary_global[static_cast<std::size_t>(idx)];
            row.t_iter_us = trace_iter_global[static_cast<std::size_t>(idx)];
            row.wait_frac = trace_wait_frac_global[static_cast<std::size_t>(idx)];
            row.mpi_test_calls = trace_mpi_test_calls_global[static_cast<std::size_t>(idx)];
            row.bytes_total = trace_bytes_total_global[static_cast<std::size_t>(idx)];
            trace_summary.rank_iterations[static_cast<std::size_t>(idx)] = row;
          }
        }
        if (cfg.trace_detail == phasegap::cli::TraceDetail::kThread) {
          trace_summary.thread_interior_us = std::move(trace_thread_interior_global);
          trace_summary.thread_boundary_us = std::move(trace_thread_boundary_global);
        }
      }
      const bool trace_ok = phasegap::trace::WriteTrace(cfg, trace_summary, "trace");
      if (!trace_ok) {
        MPI_Finalize();
        return EXIT_FAILURE;
      }
    }

    std::cout << "phasegap skeleton ready"
              << " | ranks=" << world_size
              << " | omp_threads=" << omp_threads
              << " | mode=" << phasegap::cli::ToString(cfg.mode)
              << " | kernel=" << phasegap::cli::ToString(cfg.kernel)
              << " | halo=" << cfg.halo
              << " | radius=" << cfg.radius
              << " | timesteps=" << cfg.timesteps
              << " | B=" << cfg.boundary_width
              << " | left_nbr=" << neighbors.left
              << " | right_nbr=" << neighbors.right
              << " | halo_sum=" << halo_sum
              << " | sync=" << phasegap::cli::ToString(cfg.sync)
              << " | poison_ghost=" << (cfg.poison_ghost ? 1 : 0)
              << " | check=" << (cfg.check ? 1 : 0)
              << " | check_every=" << cfg.check_every
              << " | checksum64=" << checksum64_global
              << " | measured_iters=" << measured_iters
              << " | t_iter_us=" << t_iter_mean_avg
              << " | t_post_us=" << t_post_mean_avg
              << " | t_interior_us=" << t_interior_mean_avg
              << " | t_wait_us=" << t_wait_mean_avg
              << " | t_boundary_us=" << t_boundary_mean_avg
              << " | t_poll_us=" << t_poll_mean_avg
              << " | t_comm_window_us=" << t_comm_mean_avg
              << " | t_iter_p50_us=" << t_iter_p50_avg
              << " | t_iter_p95_us=" << t_iter_p95_avg
              << " | t_wait_mean_max_us=" << t_wait_mean_max
              << " | wait_frac=" << wait_frac
              << " | wait_skew=" << wait_skew
              << " | overlap_ratio=" << overlap_ratio_avg
              << " | msg_bytes=" << bw.msg_bytes
              << " | bytes_total=" << bw.bytes_total
              << " | bw_effective_bytes_per_us=" << bw.bw_effective_bytes_per_us
              << " | mpi_test_calls=" << mpi_test_calls_mean_avg
              << " | mpi_wait_calls=" << mpi_wait_calls_mean_avg
              << " | polls_to_complete_mean=" << polls_to_complete_mean_avg
              << " | polls_to_complete_p95=" << polls_to_complete_p95_avg
              << " | mpi_thread_requested=" << phasegap::cli::ToString(cfg.mpi_thread)
              << " | mpi_thread_provided=" << provided
              << '\n';
  }

  MPI_Finalize();
  return EXIT_SUCCESS;
}

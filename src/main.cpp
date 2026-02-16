#include <mpi.h>
#include <omp.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
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

  const phasegap::mpi::RingNeighbors neighbors =
      phasegap::mpi::ComputeRingNeighbors(rank, world_size);
  phasegap::mpi::LocalBuffers buffers(cfg.n_local, cfg.halo);
  const bool use_persistent_requests = (cfg.mode == phasegap::cli::Mode::kPhasePersist);
  double halo_sum = 0.0;
  std::uint64_t checksum64_global = 0;
  phasegap::stats::IterationTiming timing_sum{};
  double overlap_ratio_sum_local = 0.0;
  double mpi_test_calls_sum_local = 0.0;
  double mpi_wait_calls_sum_local = 0.0;
  int measured_iters = 0;

  if (cfg.sync == phasegap::cli::SyncMode::kBarrierStart) {
    MPI_Barrier(MPI_COMM_WORLD);
  }

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
      MPI_Sendrecv(send_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 100, recv_right.data(),
                   cfg.halo, MPI_DOUBLE, neighbors.right, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Sendrecv(send_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right, 101, recv_left.data(),
                   cfg.halo, MPI_DOUBLE, neighbors.left, 101, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      timer.EndWait();
      timer.EndCommWindow();

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
        const phasegap::stats::TimePoint interior_start = phasegap::stats::SteadyClock::now();
#pragma omp for nowait schedule(runtime)
        for (int i = interior_begin; i < interior_end; ++i) {
          buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
        }
        const double thread_interior_us =
            phasegap::stats::DurationMicros(interior_start, phasegap::stats::SteadyClock::now());
#pragma omp critical
        {
          interior_work_us = std::max(interior_work_us, thread_interior_us);
        }
#pragma omp barrier
#pragma omp master
        timer.SetInteriorMicros(interior_work_us);

#pragma omp master
        timer.BeginBoundary();
#pragma omp for schedule(runtime)
        for (int i = buffers.OwnedBegin(); i < left_boundary_end; ++i) {
          buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
        }
#pragma omp for schedule(runtime)
        for (int i = right_boundary_begin; i < buffers.OwnedEnd(); ++i) {
          buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
        }
#pragma omp master
        timer.EndBoundary();
      }
    } else {
      MPI_Request reqs[4] = {MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL};
      MPI_Request* active_reqs = reqs;
      timer.BeginCommWindow();
      timer.BeginPost();
      if (use_persistent_requests) {
        MPI_Startall(4, persistent_reqs);
        active_reqs = persistent_reqs;
      } else {
        MPI_Irecv(recv_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 101, MPI_COMM_WORLD,
                  &reqs[0]);
        MPI_Irecv(recv_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right, 100, MPI_COMM_WORLD,
                  &reqs[1]);
        MPI_Isend(send_left.data(), cfg.halo, MPI_DOUBLE, neighbors.left, 100, MPI_COMM_WORLD,
                  &reqs[2]);
        MPI_Isend(send_right.data(), cfg.halo, MPI_DOUBLE, neighbors.right, 101, MPI_COMM_WORLD,
                  &reqs[3]);
      }
      timer.EndPost();

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
          // Measure each worker's interior compute span and use max to avoid
          // master-thread barrier/poll skew inflating interior duration.
          const phasegap::stats::TimePoint interior_start = phasegap::stats::SteadyClock::now();
#pragma omp for nowait schedule(runtime)
          for (int i = interior_begin; i < interior_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
          const double thread_interior_us =
              phasegap::stats::DurationMicros(interior_start, phasegap::stats::SteadyClock::now());
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
              MPI_Testall(4, active_reqs, &completed, MPI_STATUSES_IGNORE);
              ++mpi_test_calls;
              timer.EndPoll();
            }
          }

#pragma omp barrier

#pragma omp master
          timer.SetInteriorMicros(interior_work_us);

#pragma omp master
          {
            timer.BeginWait();
            if (!completed) {
              MPI_Waitall(4, active_reqs, MPI_STATUSES_IGNORE);
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
#pragma omp for schedule(runtime)
          for (int i = buffers.OwnedBegin(); i < left_boundary_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
#pragma omp for schedule(runtime)
          for (int i = right_boundary_begin; i < buffers.OwnedEnd(); ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
#pragma omp master
          timer.EndBoundary();
        }
      } else {
        double interior_work_us = 0.0;
#pragma omp parallel
        {
          // Interior compute should overlap communication in nonblocking modes.
          const phasegap::stats::TimePoint interior_start = phasegap::stats::SteadyClock::now();
#pragma omp for nowait schedule(runtime)
          for (int i = interior_begin; i < interior_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
          const double thread_interior_us =
              phasegap::stats::DurationMicros(interior_start, phasegap::stats::SteadyClock::now());
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
            MPI_Waitall(4, active_reqs, MPI_STATUSES_IGNORE);
            ++mpi_wait_calls;
            timer.EndWait();
            timer.EndCommWindow();
            phasegap::mpi::UnpackLeftGhost(&buffers, recv_left);
            phasegap::mpi::UnpackRightGhost(&buffers, recv_right);
          }

#pragma omp barrier

#pragma omp master
          timer.BeginBoundary();
#pragma omp for schedule(runtime)
          for (int i = buffers.OwnedBegin(); i < left_boundary_end; ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
#pragma omp for schedule(runtime)
          for (int i = right_boundary_begin; i < buffers.OwnedEnd(); ++i) {
            buffers.next[static_cast<std::size_t>(i)] = UpdatePoint(buffers, i, cfg.kernel, cfg.radius);
          }
#pragma omp master
          timer.EndBoundary();
        }
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
      overlap_ratio_sum_local +=
          phasegap::stats::OverlapRatio(t.t_comm_window_us, t.t_interior_us, t.t_wait_us);
      mpi_test_calls_sum_local += static_cast<double>(mpi_test_calls);
      mpi_wait_calls_sum_local += static_cast<double>(mpi_wait_calls);
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

  double t_post_sum = 0.0;
  double t_interior_sum = 0.0;
  double t_wait_sum = 0.0;
  double t_wait_max = 0.0;
  double t_boundary_sum = 0.0;
  double t_poll_sum = 0.0;
  double t_iter_sum = 0.0;
  double t_comm_sum = 0.0;
  double overlap_sum = 0.0;
  double mpi_test_calls_sum = 0.0;
  double mpi_wait_calls_sum = 0.0;
  MPI_Allreduce(&timing_mean_local.t_post_us, &t_post_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_interior_us, &t_interior_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_wait_us, &t_wait_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_wait_us, &t_wait_max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_boundary_us, &t_boundary_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_poll_us, &t_poll_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_iter_us, &t_iter_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&timing_mean_local.t_comm_window_us, &t_comm_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&overlap_local, &overlap_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&mpi_test_calls_mean_local, &mpi_test_calls_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);
  MPI_Allreduce(&mpi_wait_calls_mean_local, &mpi_wait_calls_sum, 1, MPI_DOUBLE, MPI_SUM,
                MPI_COMM_WORLD);

  const double t_post_mean_avg = t_post_sum / static_cast<double>(world_size);
  const double t_interior_mean_avg = t_interior_sum / static_cast<double>(world_size);
  const double t_wait_mean_avg = t_wait_sum / static_cast<double>(world_size);
  const double t_boundary_mean_avg = t_boundary_sum / static_cast<double>(world_size);
  const double t_poll_mean_avg = t_poll_sum / static_cast<double>(world_size);
  const double t_iter_mean_avg = t_iter_sum / static_cast<double>(world_size);
  const double t_comm_mean_avg = t_comm_sum / static_cast<double>(world_size);
  const double overlap_ratio_avg = overlap_sum / static_cast<double>(world_size);
  const double mpi_test_calls_mean_avg = mpi_test_calls_sum / static_cast<double>(world_size);
  const double mpi_wait_calls_mean_avg = mpi_wait_calls_sum / static_cast<double>(world_size);

  if (rank == 0) {
    const double wait_frac = phasegap::stats::WaitFraction(t_wait_mean_avg, t_iter_mean_avg);
    const double wait_skew = phasegap::stats::WaitSkew(t_wait_max, t_wait_mean_avg);
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
    csv_summary.wait_frac = wait_frac;
    csv_summary.wait_skew = wait_skew;
    csv_summary.overlap_ratio = overlap_ratio_avg;
    csv_summary.bw_effective_bytes_per_us = bw.bw_effective_bytes_per_us;
    csv_summary.mpi_test_calls = mpi_test_calls_mean_avg;
    csv_summary.mpi_wait_calls = mpi_wait_calls_mean_avg;
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
      summary.wait_frac = wait_frac;
      summary.wait_skew = wait_skew;
      summary.overlap_ratio = overlap_ratio_avg;
      summary.mpi_test_calls = mpi_test_calls_mean_avg;
      summary.mpi_wait_calls = mpi_wait_calls_mean_avg;
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
      trace_summary.trace_iters = cfg.trace_iters;
      trace_summary.mpi_thread_provided = provided;
      trace_summary.t_post_us = t_post_mean_avg;
      trace_summary.t_interior_us = t_interior_mean_avg;
      trace_summary.t_wait_us = t_wait_mean_avg;
      trace_summary.t_boundary_us = t_boundary_mean_avg;
      trace_summary.t_iter_us = t_iter_mean_avg;
      trace_summary.wait_frac = wait_frac;
      trace_summary.overlap_ratio = overlap_ratio_avg;
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
              << " | wait_frac=" << wait_frac
              << " | wait_skew=" << wait_skew
              << " | overlap_ratio=" << overlap_ratio_avg
              << " | msg_bytes=" << bw.msg_bytes
              << " | bytes_total=" << bw.bytes_total
              << " | bw_effective_bytes_per_us=" << bw.bw_effective_bytes_per_us
              << " | mpi_test_calls=" << mpi_test_calls_mean_avg
              << " | mpi_wait_calls=" << mpi_wait_calls_mean_avg
              << " | mpi_thread_requested=" << phasegap::cli::ToString(cfg.mpi_thread)
              << " | mpi_thread_provided=" << provided
              << '\n';
  }

  MPI_Finalize();
  return EXIT_SUCCESS;
}

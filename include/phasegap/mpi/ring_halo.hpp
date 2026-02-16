#pragma once

#include <vector>

namespace phasegap::mpi {

struct RingNeighbors {
  int left = 0;
  int right = 0;
};

RingNeighbors ComputeRingNeighbors(int rank, int world_size);

struct LocalBuffers {
  int n_local = 0;
  int halo = 0;
  std::vector<double> current;
  std::vector<double> next;

  LocalBuffers(int n_local_in, int halo_in);

  int TotalLength() const;
  int OwnedBegin() const;
  int OwnedEnd() const;
  int LeftGhostBegin() const;
  int LeftGhostEnd() const;
  int RightGhostBegin() const;
  int RightGhostEnd() const;
};

std::vector<double> PackLeftHalo(const LocalBuffers& buffers);
std::vector<double> PackRightHalo(const LocalBuffers& buffers);
void UnpackLeftGhost(LocalBuffers* buffers, const std::vector<double>& packed);
void UnpackRightGhost(LocalBuffers* buffers, const std::vector<double>& packed);

}  // namespace phasegap::mpi


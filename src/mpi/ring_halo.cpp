#include "phasegap/mpi/ring_halo.hpp"

#include <stdexcept>

namespace phasegap::mpi {

RingNeighbors ComputeRingNeighbors(int rank, int world_size) {
  if (world_size <= 0) {
    throw std::invalid_argument("world_size must be > 0");
  }
  if (rank < 0 || rank >= world_size) {
    throw std::invalid_argument("rank must be in [0, world_size)");
  }

  RingNeighbors n{};
  n.left = (rank - 1 + world_size) % world_size;
  n.right = (rank + 1) % world_size;
  return n;
}

LocalBuffers::LocalBuffers(int n_local_in, int halo_in) : n_local(n_local_in), halo(halo_in) {
  if (n_local <= 0) {
    throw std::invalid_argument("n_local must be > 0");
  }
  if (halo <= 0) {
    throw std::invalid_argument("halo must be > 0");
  }
  const int total = n_local + 2 * halo;
  current.assign(static_cast<std::size_t>(total), 0.0);
  next.assign(static_cast<std::size_t>(total), 0.0);
}

int LocalBuffers::TotalLength() const { return n_local + 2 * halo; }
int LocalBuffers::OwnedBegin() const { return halo; }
int LocalBuffers::OwnedEnd() const { return halo + n_local; }
int LocalBuffers::LeftGhostBegin() const { return 0; }
int LocalBuffers::LeftGhostEnd() const { return halo; }
int LocalBuffers::RightGhostBegin() const { return halo + n_local; }
int LocalBuffers::RightGhostEnd() const { return halo + n_local + halo; }

std::vector<double> PackLeftHalo(const LocalBuffers& buffers) {
  std::vector<double> packed(static_cast<std::size_t>(buffers.halo));
  const int begin = buffers.OwnedBegin();
  for (int i = 0; i < buffers.halo; ++i) {
    packed[static_cast<std::size_t>(i)] = buffers.current[static_cast<std::size_t>(begin + i)];
  }
  return packed;
}

std::vector<double> PackRightHalo(const LocalBuffers& buffers) {
  std::vector<double> packed(static_cast<std::size_t>(buffers.halo));
  const int right_start = buffers.OwnedEnd() - buffers.halo;
  for (int i = 0; i < buffers.halo; ++i) {
    packed[static_cast<std::size_t>(i)] =
        buffers.current[static_cast<std::size_t>(right_start + i)];
  }
  return packed;
}

void UnpackLeftGhost(LocalBuffers* buffers, const std::vector<double>& packed) {
  if (buffers == nullptr) {
    throw std::invalid_argument("buffers pointer must not be null");
  }
  if (static_cast<int>(packed.size()) != buffers->halo) {
    throw std::invalid_argument("packed left halo size mismatch");
  }
  for (int i = 0; i < buffers->halo; ++i) {
    buffers->current[static_cast<std::size_t>(buffers->LeftGhostBegin() + i)] =
        packed[static_cast<std::size_t>(i)];
  }
}

void UnpackRightGhost(LocalBuffers* buffers, const std::vector<double>& packed) {
  if (buffers == nullptr) {
    throw std::invalid_argument("buffers pointer must not be null");
  }
  if (static_cast<int>(packed.size()) != buffers->halo) {
    throw std::invalid_argument("packed right halo size mismatch");
  }
  const int begin = buffers->RightGhostBegin();
  for (int i = 0; i < buffers->halo; ++i) {
    buffers->current[static_cast<std::size_t>(begin + i)] = packed[static_cast<std::size_t>(i)];
  }
}

}  // namespace phasegap::mpi


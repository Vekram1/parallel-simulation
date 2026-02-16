#pragma once

#include <cstdint>

#include "phasegap/mpi/ring_halo.hpp"

namespace phasegap::stats {

std::uint64_t ChecksumOwnedRegion(const mpi::LocalBuffers& buffers);

}  // namespace phasegap::stats


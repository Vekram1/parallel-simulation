#include "phasegap/stats/checksum.hpp"

#include <cstddef>
#include <cstring>

namespace phasegap::stats {

std::uint64_t ChecksumOwnedRegion(const mpi::LocalBuffers& buffers) {
  constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;

  std::uint64_t hash = kOffsetBasis;
  for (int i = buffers.OwnedBegin(); i < buffers.OwnedEnd(); ++i) {
    std::uint64_t bits = 0;
    const double value = buffers.current[static_cast<std::size_t>(i)];
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash ^= bits;
    hash *= kPrime;
  }
  return hash;
}

}  // namespace phasegap::stats

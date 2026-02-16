#include "phasegap/kernels/kernels.hpp"

#include <stdexcept>

namespace phasegap::kernels {

namespace {

const std::array<KernelSpec, 3> kSpecs = {{
    {Kernel::kStencil3, "stencil3", 1},
    {Kernel::kStencil5, "stencil5", 2},
    {Kernel::kAxpy, "axpy", 1},
}};

}  // namespace

const std::array<KernelSpec, 3>& AllSpecs() { return kSpecs; }

bool ParseKernel(std::string_view name, Kernel* out) {
  if (out == nullptr) {
    return false;
  }
  for (const KernelSpec& spec : kSpecs) {
    if (spec.name == name) {
      *out = spec.kernel;
      return true;
    }
  }
  return false;
}

const char* KernelName(Kernel kernel) {
  for (const KernelSpec& spec : kSpecs) {
    if (spec.kernel == kernel) {
      return spec.name.data();
    }
  }
  return "unknown";
}

int DefaultRadius(Kernel kernel) {
  for (const KernelSpec& spec : kSpecs) {
    if (spec.kernel == kernel) {
      return spec.default_radius;
    }
  }
  throw std::invalid_argument("unknown kernel");
}

}  // namespace phasegap::kernels


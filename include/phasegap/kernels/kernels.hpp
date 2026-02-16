#pragma once

#include <array>
#include <string_view>

namespace phasegap::kernels {

enum class Kernel { kStencil3, kStencil5, kAxpy };

struct KernelSpec {
  Kernel kernel;
  std::string_view name;
  int default_radius;
};

const std::array<KernelSpec, 3>& AllSpecs();
bool ParseKernel(std::string_view name, Kernel* out);
const char* KernelName(Kernel kernel);
int DefaultRadius(Kernel kernel);

}  // namespace phasegap::kernels


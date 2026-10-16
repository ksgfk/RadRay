#pragma once

#include "workload.h"

namespace radray::example {
std::optional<DescriptorReuseOptions> ParseDescriptorReuseOptions(int argc, char** argv);
}  // namespace radray::example

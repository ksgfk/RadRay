#pragma once

#include <string_view>

#include <radray/types.h>

namespace radray {

string GetEnv(std::string_view name);

}  // namespace radray

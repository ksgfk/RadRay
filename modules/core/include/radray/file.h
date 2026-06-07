#pragma once

#include <filesystem>
#include <optional>

#include <radray/types.h>

namespace radray {

std::optional<string> ReadTextFile(const std::filesystem::path& filepath) noexcept;

}  // namespace radray

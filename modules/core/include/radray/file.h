#pragma once

#include <filesystem>
#include <optional>

#include <radray/types.h>

namespace radray {

namespace file {

std::optional<string> ReadText(const std::filesystem::path& filepath) noexcept;

}

}  // namespace radray

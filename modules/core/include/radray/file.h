#pragma once

#include <filesystem>
#include <optional>

#include <radray/types.h>

namespace radray {

std::optional<string> ReadTextFile(const std::filesystem::path& filepath) noexcept;

/// 返回当前可执行文件所在目录。失败时返回空路径。
/// 用于以运行时目录为基准定位随程序部署的资源（例如 shaderlib include 根目录）。
std::filesystem::path GetExecutableDirectory() noexcept;

}  // namespace radray

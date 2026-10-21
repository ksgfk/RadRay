#pragma once

#include <filesystem>
#include <optional>
#include <span>

#include <radray/types.h>

namespace radray {

/// 读取整个文件为字符串。文件不存在或读取失败时返回 nullopt。
/// 【按二进制读，不做换行转换】返回内容与磁盘逐字一致，CRLF 保持原样。要逐字写回所读内容的
/// 调用方依赖这一点（如 asset manifest 的 settings 原文保真），文本模式会把 `\r\n` 折成 `\n`
/// 从而静默改写它们。
std::optional<string> ReadTextFile(const std::filesystem::path& filepath) noexcept;

/// 以二进制方式读取整个文件。文件不存在或读取失败时返回 nullopt。
std::optional<vector<byte>> ReadBinaryFile(const std::filesystem::path& filepath) noexcept;

/// 把文本写入文件 (覆盖已有内容)。会自动创建父目录。成功返回 true。
bool WriteTextFile(const std::filesystem::path& filepath, std::string_view content) noexcept;

/// 把二进制数据写入文件 (覆盖已有内容)。会自动创建父目录。成功返回 true。
bool WriteBinaryFile(const std::filesystem::path& filepath, std::span<const byte> data) noexcept;

/// 返回当前可执行文件所在目录。失败时返回空路径。
/// 用于以运行时目录为基准定位随程序部署的资源（例如 shaderlib include 根目录）。
std::filesystem::path GetExecutableDirectory() noexcept;

}  // namespace radray

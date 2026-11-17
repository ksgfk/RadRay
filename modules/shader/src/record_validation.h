#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <radray/types.h>

namespace radray::shader {
class ShaderArtifactView;
struct ShaderArtifactTypeRecord;
struct ShaderTypeRecord;
struct ShaderVertexAttributeSignatureDesc;
struct WireBlobRange;
}  // namespace radray::shader

namespace radray::shader::detail {

struct RecordIssue {
    size_t Index;
    std::string_view Reason;
};

bool IsArtifactName(const ShaderArtifactView& artifact, WireBlobRange range) noexcept;
std::optional<RecordIssue> ValidateTypes(const ShaderArtifactView& artifact);
std::optional<RecordIssue> ValidateTypes(std::span<const ShaderTypeRecord> types);
std::optional<RecordIssue> ValidateVertexInputs(const ShaderArtifactView& artifact);
std::optional<RecordIssue> ValidateVertexInputs(std::span<const ShaderVertexAttributeSignatureDesc> inputs);
bool IsRoot(std::span<const ShaderTypeRecord> types, std::optional<uint32_t> index) noexcept;
bool IsRoot(std::span<const ShaderArtifactTypeRecord> types, uint32_t index) noexcept;

}  // namespace radray::shader::detail

#pragma once

#include <optional>
#include <span>

#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/render/shader_layout.h>
#include <radray/shader/shader_artifact.h>

namespace radray::render {

// 后端共用的少量 layout 辅助类型。两个后端都直接消费 `ResolvedD3D12Layout` /
// `ResolvedVulkanLayout`，这里不再存在第二种 layout 描述。
struct ShaderBindingLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderBindingLocation&, const ShaderBindingLocation&) noexcept = default;
};

// A binding record is either a descriptor slot or a push/root-constant block. Both live in one
// table so a caller reaches either by the declaration name it wrote in the shader, and the kind is
// what keeps a push handle out of a parameter-set write and vice versa.
enum class BackendBindingRecordKind : uint32_t {
    Descriptor = 0,
    Push = 1,
};

struct BackendBindingName {
    string Name;
    ShaderBindingLocation Location{};
    uint32_t Namespace{0};
    BackendBindingRecordKind Kind{BackendBindingRecordKind::Descriptor};
};

// Backend-internal view of a BindingHandle's token. The handle is opaque to callers: it pairs the
// layout's binding generation with an index into that layout's record table, so one handle names
// exactly one declaration and neither the group nor the register class can be mistaken. The bit
// layout is not ABI and only the two backends may take it apart.
struct BindingHandleAccess {
    static constexpr BindingHandle Make(uint32_t recordIndex, uint32_t generation) noexcept {
        // generation is never 0 (see NextBackendBindingGeneration) and the stored index is biased by
        // one, so a valid token can never be the default-invalid value.
        return BindingHandle{
            (static_cast<uint64_t>(generation) << 32) | (static_cast<uint64_t>(recordIndex) + 1)};
    }

    static constexpr uint32_t RecordIndex(BindingHandle handle) noexcept {
        return static_cast<uint32_t>(handle._value & 0xffffffffull) - 1;
    }

    static constexpr uint32_t Generation(BindingHandle handle) noexcept {
        return static_cast<uint32_t>(handle._value >> 32);
    }
};

// Resolves a handle against the table it was minted from. A handle from another layout carries a
// different generation and is rejected here rather than silently resolving to whatever record
// happens to sit at that index.
Nullable<const BackendBindingName*> FindBackendBindingRecord(
    std::span<const BackendBindingName> records,
    uint32_t generation,
    BindingHandle handle) noexcept;

// Monotonic generation stamped into every BindingHandle a layout hands out, so a handle taken from
// one layout cannot be used against another. Never returns 0, which is the invalid-handle value.
uint32_t NextBackendBindingGeneration() noexcept;

bool ValidateVertexInputStateAgainstArtifact(
    const VertexInputState& state,
    const shader::ShaderArtifactView& artifact) noexcept;

}  // namespace radray::render

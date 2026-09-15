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
// layout's address with an index into that layout's record table, so one handle names
// exactly one declaration and neither the group nor the register class can be mistaken. The bit
// layout is not ABI and only the two backends may take it apart.
struct BindingHandleAccess {
    static BindingHandle Make(uint32_t recordIndex, uintptr_t generation) noexcept;

    static uint32_t RecordIndex(BindingHandle handle) noexcept;

    static uintptr_t Generation(BindingHandle handle) noexcept;
};

// Resolves a handle against its live layout's table. The generation is the layout address;
// handles must not outlive their layout.
Nullable<const BackendBindingName*> FindBackendBindingRecord(
    std::span<const BackendBindingName> records,
    uintptr_t generation,
    BindingHandle handle) noexcept;

bool ValidateVertexInputStateAgainstArtifact(
    const VertexInputState& state,
    const shader::ShaderArtifactView& artifact) noexcept;

}  // namespace radray::render

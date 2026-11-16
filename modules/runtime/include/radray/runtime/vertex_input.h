#pragma once

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/shader_layout.h>

namespace radray {

struct VertexInputAttribute {
    uint32_t BufferBinding{0};
    uint32_t Offset{0};
    string Semantic{};
    uint32_t SemanticIndex{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};
};

struct VertexInputStream {
    // 借用缓冲；每条已绑定的流都必须提供；Offset 与 Size 的单位为字节。
    Nullable<render::Buffer*> Buffer{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
    uint32_t Binding{0};
};

struct VertexInput {
    vector<render::VertexBufferLayout> Layouts;
    vector<VertexInputAttribute> Attributes;
    vector<VertexInputStream> Streams;
};

struct ResolvedVertexInputState {
    vector<render::VertexBufferLayout> Layouts;
    // Semantic 借用源 VertexInput；创建 PSO 完成前源对象不得销毁或搬移。
    vector<render::VertexAttribute> Attributes;
};

// 只验证几何布局；不读取 Streams，Location 由 shader 变体接口提供。
bool ValidateVertexInputLayout(const VertexInput& input) noexcept;
bool ResolveVertexInputState(const VertexInput& meshInput, std::span<const ShaderVertexAttributeSignatureDesc> shaderInputs, ResolvedVertexInputState& output);

}  // namespace radray

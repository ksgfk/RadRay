#pragma once

#include <optional>
#include <string_view>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/material_tag.h>

namespace radray {

// Material defaults; each Pass explicitly chooses which fields to use or override.
// Attachment formats, sampling and geometry topology are supplied by the Pass.
struct MaterialRenderState {
    render::CullMode Cull{render::CullMode::Back};
    std::optional<render::BlendState> Blend;
    render::ColorWrites ColorWriteMask{render::ColorWrite::All};

    render::CompareFunction DepthCompare{render::CompareFunction::LessEqual};
    render::DepthBiasState DepthBias{};
    std::optional<render::StencilState> Stencil;

    // Keep byte-sized fields together after the naturally aligned state fields.
    bool DepthTestEnable{true};
    bool DepthWriteEnable{true};
    uint8_t StencilReference{0};
};

struct MaterialProperties {
    MaterialTagSet Tags;
    MaterialRenderState RenderState;
};

// 借用 RHI 对象，拥有入口名称字符串；空 PS 表示仅顶点阶段的程序。
struct MaterialShaderProgram {
    string Name;
    Nullable<render::PipelineLayout*> Layout{nullptr};
    Nullable<render::Shader*> VertexShader{nullptr};
    string VertexEntry;
    Nullable<render::Shader*> PixelShader{nullptr};
    string PixelEntry;
};

// 材质所有权与 UE 对照见 docs/architecture/runtime.md。
// 使用前完成准备；被引用期间保持地址稳定、内容不变。
// 程序名称必须唯一；列表表达可用 shader，不决定 Pass 参与或持有 PSO。
struct MaterialResource {
    MaterialProperties Properties;
    vector<MaterialShaderProgram> Programs;

    // 准备阶段解析名称；Programs 不变时索引保持有效。
    std::optional<size_t> FindProgram(std::string_view name) const noexcept;
    Nullable<const MaterialShaderProgram*> GetProgram(size_t index) const noexcept;
};

}  // namespace radray

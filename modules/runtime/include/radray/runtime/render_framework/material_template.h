#pragma once

#include <radray/runtime/shader_program.h>
#include <radray/runtime/render_framework/material_input.h>
#include <radray/runtime/render_framework/instance_parameters.h>
#include <radray/runtime/render_framework/renderer_id.h>

namespace radray {
class RenderSystem;
/// CPU definition. Registration copies it; it never owns native plans or GPU storage.
struct MaterialTemplateDescriptor {
    string Name;
    ShaderProgramRequest Program;
    std::optional<ShaderProgramId> RegisteredProgram;
    vector<MaterialInput> Inputs;
    string ObjectIndexPushConstant;
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    std::optional<render::BlendState> Blend;
    render::ColorWrites WriteMask{render::ColorWrite::All};
    std::optional<render::DepthStencilState> DepthStencil;
    render::MultiSampleState Samples{render::MultiSampleState::Default()};
    string PushConstant;
    vector<InstanceParameterLayout> ParameterLayouts;
    MaterialInstanceAddressing InstanceAddressing{MaterialInstanceAddressing::DirectObject};
    bool AllowAutomaticInstancing{false};
    string SceneIndexField;
    vector<byte> ScenePushValues;
    bool PreserveOrder{false};
};
/// An immutable material version references registered templates and imported texture views.
struct RendererMaterialDescriptor {
    vector<TemplateId> Templates;
    vector<vector<byte>> Constants;
    vector<ResourceId> Textures;
    vector<render::SamplerDescriptor> Samplers;
};
struct CompiledMaterialTemplate {
    struct Input {
        MaterialInput Source;
        render::BindingHandle Handle;
        render::ShaderBindingInfo Binding;
        uint32_t BufferSize{0};
    };
    struct Group {
        uint32_t Index;
        MaterialBindingDomain Domain;
        vector<uint32_t> Inputs;
        /// Canonical element index inside the group compatibility, in the same expansion order a frame builds writes.
        vector<uint32_t> ElementIndices;
        /// Prebuilt dynamic offsets in expansion order; each names one dynamic input handle.
        vector<render::ShaderParameterDynamicOffset> DynamicOffsets;
    };
    MaterialTemplateDescriptor Descriptor;
    ShaderProgram* Program;
    vector<Input> Inputs;
    vector<Group> Groups;
    render::BindingHandle Push;
    uint32_t PushSize{0};
    std::optional<uint32_t> SceneIndexOffset;
    static std::optional<CompiledMaterialTemplate> Compile(render::Device* device, RenderSystem& services,
                                                           const MaterialTemplateDescriptor& descriptor, string& error);
    bool Equivalent(const CompiledMaterialTemplate& other) const noexcept;
};
}  // namespace radray

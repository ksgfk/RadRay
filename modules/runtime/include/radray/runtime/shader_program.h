#pragma once

#include <optional>

#include <radray/hash.h>
#include <radray/nullable.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/runtime/material_state.h>
#include <radray/runtime/render_framework/primitive_vertex_layout.h>
#include <radray/runtime/shader_parameters.h>
#include <radray/types.h>

namespace radray {

struct GraphicsPassState {
    GraphicsPassState(
        vector<render::TextureFormat> colorFormats,
        std::optional<render::TextureFormat> depthStencilFormat,
        uint32_t sampleCount,
        render::RenderPass* compatibleRenderPass) noexcept;

    bool IsValid() const noexcept;

    vector<render::TextureFormat> ColorFormats;
    std::optional<render::TextureFormat> DepthStencilFormat;
    uint32_t SampleCount;
    render::RenderPass* CompatibleRenderPass;

    friend bool operator==(const GraphicsPassState&, const GraphicsPassState&) = default;
};

class ShaderProgram {
public:
    // The artifact already carries the layout the recipe produced, so the recipe itself is not an
    // input here: the resolved layout is what everything downstream reads.
    static Nullable<unique_ptr<ShaderProgram>> Create(
        render::Device* device,
        render::BackendShaderArtifact artifact) noexcept;

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram& operator=(ShaderProgram&&) = delete;
    ~ShaderProgram() noexcept;

    Nullable<render::GraphicsPipelineState*> GetOrCreateGraphicsPipelineState(
        const MaterialPipelineState& materialState,
        const PrimitiveVertexLayout& vertexLayout,
        PrimitiveTopology topology,
        const GraphicsPassState& passState) noexcept;

    const render::BackendShaderArtifact& GetArtifact() const noexcept { return _artifact; }
    render::PipelineLayout* GetPipelineLayout() const noexcept { return _artifact.Layout.get(); }
    render::Device* GetDevice() const noexcept { return _device; }
    // True when the named buffer declaration takes its offset at bind time. Dynamic-ness is a
    // property of one declaration, not of a whole descriptor group.
    bool IsBufferDynamic(std::string_view declarationName) const noexcept;
    const ShaderParameterLayout& GetParameterLayout() const noexcept { return _parameterLayout; }
    size_t GetGraphicsPipelineStateCount() const noexcept { return _graphicsPipelineStates.size(); }

private:
    struct PsoKey {
        MaterialPipelineState MaterialState;
        PrimitiveVertexLayout VertexLayout;
        PrimitiveTopology Topology{PrimitiveTopology::TriangleList};
        GraphicsPassState PassState;

        friend bool operator==(const PsoKey&, const PsoKey&) = default;
    };

    // Borrowed view of a PsoKey used for cache lookups. The draw loop asks for a PSO
    // once per draw, and an owning PsoKey allocates for its format vector, its vertex
    // buffer/attribute vectors and every attribute semantic string. Looking up through
    // this view keeps the hit path allocation free; only a miss materializes a key.
    struct PsoKeyRef {
        const MaterialPipelineState* MaterialState;
        const PrimitiveVertexLayout* VertexLayout;
        PrimitiveTopology Topology;
        const GraphicsPassState* PassState;
    };

    struct PsoKeyHash {
        using is_transparent = void;
        size_t operator()(const PsoKey& value) const noexcept;
        size_t operator()(const PsoKeyRef& value) const noexcept;
    };

    struct PsoKeyEqual {
        using is_transparent = void;
        bool operator()(const PsoKey& lhs, const PsoKey& rhs) const noexcept;
        bool operator()(const PsoKeyRef& lhs, const PsoKey& rhs) const noexcept;
        bool operator()(const PsoKey& lhs, const PsoKeyRef& rhs) const noexcept;
    };

    ShaderProgram(
        render::Device* device,
        render::BackendShaderArtifact artifact,
        ShaderParameterLayout parameterLayout,
        unique_ptr<render::Shader> vertexShader,
        string vertexEntry,
        unique_ptr<render::Shader> pixelShader,
        string pixelEntry,
        unique_ptr<render::Shader> computeShader,
        string computeEntry) noexcept;

    render::Device* _device;
    render::BackendShaderArtifact _artifact;
    unique_ptr<render::Shader> _vertexShader;
    string _vertexEntry;
    unique_ptr<render::Shader> _pixelShader;
    string _pixelEntry;
    unique_ptr<render::Shader> _computeShader;
    string _computeEntry;
    ShaderParameterLayout _parameterLayout;
    unordered_map<PsoKey, unique_ptr<render::GraphicsPipelineState>, PsoKeyHash, PsoKeyEqual>
        _graphicsPipelineStates;
};

}  // namespace radray

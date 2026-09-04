#pragma once

#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/runtime/material_state.h>
#include <radray/runtime/render_framework/render_types.h>
#include <radray/runtime/shader_parameters.h>
#include <radray/runtime/texture_asset.h>
#include <radray/types.h>

namespace radray {

class ShaderProgram;

struct MaterialTextureFrameData {
    ShaderParameterInfo Parameter;
    TextureAsset* Texture;
    TextureSubViewDesc SubView;
    uint32_t Element{0};
};

struct MaterialSamplerFrameData {
    ShaderParameterInfo Parameter;
    render::SamplerDescriptor Sampler;
    uint32_t Element{0};
};

/// CPU value snapshot. Asset owners are retained separately on the game thread.
struct MaterialRenderData {
    Nullable<ShaderProgram*> Program{nullptr};
    uint32_t ParameterGroup{0};
    ShaderParameterStorage Parameters{};
    MaterialPipelineState PipelineState;
    RenderQueue Queue{RenderQueue::Geometry};
    vector<MaterialTextureFrameData> Textures;
    vector<MaterialSamplerFrameData> Samplers;
};

class Material {
public:
    static Nullable<unique_ptr<Material>> Create(
        ShaderProgram* program,
        std::string_view parameterGroupAnchor);

    Material(const Material&) = delete;
    Material(Material&&) = delete;
    Material& operator=(const Material&) = delete;
    Material& operator=(Material&&) = delete;
    ~Material() noexcept;

    ShaderProgram* GetProgram() const noexcept { return _program; }
    uint32_t GetParameterGroup() const noexcept { return _parameterGroup; }

    MaterialPipelineState& GetPipelineState() noexcept { return _pipelineState; }
    const MaterialPipelineState& GetPipelineState() const noexcept { return _pipelineState; }
    RenderQueue GetRenderQueue() const noexcept { return _renderQueue; }
    void SetRenderQueue(RenderQueue value) noexcept { _renderQueue = value; }

    const ShaderParameterStorage& GetParameterStorage() const noexcept { return _parameters; }

    bool SetFloat(std::string_view name, float value, uint32_t element = 0) noexcept;
    bool SetFloat2(std::string_view name, const Eigen::Vector2f& value, uint32_t element = 0) noexcept;
    bool SetFloat3(std::string_view name, const Eigen::Vector3f& value, uint32_t element = 0) noexcept;
    bool SetFloat4(std::string_view name, const Eigen::Vector4f& value, uint32_t element = 0) noexcept;
    bool SetInt(std::string_view name, int32_t value, uint32_t element = 0) noexcept;
    bool SetUInt(std::string_view name, uint32_t value, uint32_t element = 0) noexcept;
    bool SetMatrix4x4(std::string_view name, const Eigen::Matrix4f& value, uint32_t element = 0) noexcept;

    bool SetTexture(
        std::string_view name,
        StreamingAssetRef<TextureAsset> texture,
        const TextureSubViewDesc& subView = TextureSubViewDesc::Default(),
        uint32_t element = 0) noexcept;
    bool SetSampler(
        std::string_view name,
        const render::SamplerDescriptor& sampler,
        uint32_t element = 0) noexcept;

    /// Game thread only. Copies authoring values and retains ready texture owners without RHI calls.
    bool BuildRenderData(MaterialRenderData& out, vector<StreamingAssetRefAny>& retainedAssets) const;

private:
    struct ResourceState;

    Material(ShaderProgram* program, uint32_t parameterGroup);

    Nullable<const ShaderParameterInfo*> FindNumericParameter(
        std::string_view name,
        ShaderParameterKind kind) const noexcept;

    ShaderProgram* _program;
    uint32_t _parameterGroup;
    ShaderParameterStorage _parameters;
    MaterialPipelineState _pipelineState;
    RenderQueue _renderQueue{RenderQueue::Geometry};
    unique_ptr<ResourceState> _resources;
};

}  // namespace radray

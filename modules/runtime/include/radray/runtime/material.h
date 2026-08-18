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

struct MaterialBufferBinding {
    uint32_t BufferIndex{0};
    render::ShaderBufferBinding Value;
};

class Material {
public:
    static Nullable<unique_ptr<Material>> Create(
        ShaderProgram* program,
        BindingGroupPlan bindingGroups,
        uint32_t flightCount);

    Material(const Material&) = delete;
    Material(Material&&) = delete;
    Material& operator=(const Material&) = delete;
    Material& operator=(Material&&) = delete;
    ~Material() noexcept;

    ShaderProgram* GetProgram() const noexcept { return _program; }
    const BindingGroupPlan& GetBindingGroups() const noexcept { return _bindingGroups; }

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

    Nullable<render::ShaderParameterSet*> PrepareParameterSet(
        uint32_t flightIndex,
        std::span<const MaterialBufferBinding> bufferBindings) noexcept;
    Nullable<render::ShaderParameterSet*> GetResidentParameterSet(
        uint32_t flightIndex) const noexcept;
    uint64_t GetResourceVersion() const noexcept;
    uint64_t GetResidentResourceVersion(uint32_t flightIndex) const noexcept;

private:
    struct ResourceState;

    Material(
        ShaderProgram* program,
        BindingGroupPlan bindingGroups,
        uint32_t flightCount);

    const ShaderParameterInfo* FindNumericParameter(
        std::string_view name,
        ShaderParameterKind kind) const noexcept;

    ShaderProgram* _program;
    BindingGroupPlan _bindingGroups;
    ShaderParameterStorage _parameters;
    MaterialPipelineState _pipelineState;
    RenderQueue _renderQueue{RenderQueue::Geometry};
    unique_ptr<ResourceState> _resources;
};

}  // namespace radray

#pragma once

#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/runtime/material_state.h>
#include <radray/runtime/material_technique.h>
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
struct MaterialPassRenderData {
    string PassName{};
    Nullable<ShaderProgram*> Program{nullptr};
    uint32_t ProgramFrameId{0};
    std::optional<uint32_t> ParameterGroup;
    ShaderParameterStorage Parameters{};
    MaterialPipelineState PipelineState{};
    vector<MaterialTextureFrameData> Textures{};
    vector<MaterialSamplerFrameData> Samplers{};
    bool Valid{false};
};

struct MaterialRenderData {
    RenderQueue Queue{RenderQueue::Geometry};
    vector<MaterialPassRenderData> Passes;
    uint64_t Generation{0}, Revision{0};

    Nullable<const MaterialPassRenderData*> FindPass(std::string_view name) const noexcept;
    /// Call before changing authoring-derived snapshot fields outside BuildRenderData.
    /// ProgramFrameId is builder bookkeeping and does not require invalidation.
    void Invalidate() noexcept { Generation = Revision = 0; }
};

class Material {
public:
    static Nullable<unique_ptr<Material>> Create(const MaterialTechnique* technique);

    Material(const Material&) = delete;
    Material(Material&&) = delete;
    Material& operator=(const Material&) = delete;
    Material& operator=(Material&&) = delete;
    ~Material() noexcept;

    ShaderProgram* GetProgram() const noexcept { return _program; }
    uint32_t GetParameterGroup() const noexcept { return _parameterGroup; }
    uint64_t GetGeneration() const noexcept { return _generation; }
    /// Game thread only. Includes resource readiness and edits through GetPipelineState().
    uint64_t GetRevision() const noexcept;

    MaterialPipelineState& GetPipelineState() noexcept { return _pipelineStates[_technique->GetPrimaryPassIndex()]; }
    const MaterialPipelineState& GetPipelineState() const noexcept { return _pipelineStates[_technique->GetPrimaryPassIndex()]; }
    bool SetPassPipelineState(std::string_view pass, const MaterialPipelineState& state) noexcept;
    RenderQueue GetRenderQueue() const noexcept { return _renderQueue; }
    void SetRenderQueue(RenderQueue value) noexcept;

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

    /// Game thread only. Reuses matching generation/revision values and always retains ready owners.
    /// `out` belongs to a writable flight. Authoring-derived fields remain read-only between builds;
    /// external edits require out.Invalidate(). ProgramFrameId is independent builder bookkeeping.
    bool BuildRenderData(MaterialRenderData& out, vector<StreamingAssetRefAny>& retainedAssets,
                         Nullable<uint64_t*> bytesCopied = nullptr) const;

private:
    struct ResourceState;

    explicit Material(const MaterialTechnique* technique);
    string CanonicalName(std::string_view name) const;
    void MarkChanged() const noexcept;
    bool SetNumericBytes(std::string_view name, ShaderParameterKind kind,
                         std::span<const byte> value, uint32_t element) noexcept;

    Nullable<const ShaderParameterInfo*> FindNumericParameter(
        std::string_view name,
        ShaderParameterKind kind) const noexcept;

    const MaterialTechnique* _technique;
    ShaderProgram* _program;
    uint32_t _parameterGroup;
    ShaderParameterStorage _parameters;
    vector<MaterialPipelineState> _pipelineStates;
    mutable vector<MaterialPipelineState> _observedPipelineStates;
    uint64_t _generation{0};
    mutable uint64_t _revision{1};
    RenderQueue _renderQueue{RenderQueue::Geometry};
    unique_ptr<ResourceState> _resources;
};

}  // namespace radray

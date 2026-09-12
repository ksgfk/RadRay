#pragma once

#include <span>
#include <string_view>

#include <radray/enum_flags.h>
#include <radray/nullable.h>
#include <radray/runtime/material_state.h>
#include <radray/runtime/material_technique.h>
#include <radray/runtime/render_framework/cbuffer_view.h>
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
    uint64_t ProgramGeneration{0};
    uint32_t ProgramFrameId{0};
    std::optional<uint32_t> ParameterGroup;
    /// The anchored cbuffer's GPU bytes verbatim. Empty for passes without a material cbuffer.
    vector<byte> NumericBytes{};
    MaterialPipelineState PipelineState{};
    vector<MaterialTextureFrameData> Textures{};
    vector<MaterialSamplerFrameData> Samplers{};
    bool Valid{false};
};

struct MaterialRenderData {
    RenderQueue Queue{RenderQueue::Geometry};
    vector<MaterialPassRenderData> Passes;
    uint64_t Generation{0}, Revision{0};
    uint64_t StructureRevision{0}, ValuesRevision{0}, BindingsRevision{0};

    Nullable<const MaterialPassRenderData*> FindPass(std::string_view name) const noexcept;
    /// Call before changing authoring-derived snapshot fields outside BuildRenderData.
    /// ProgramFrameId is builder bookkeeping and does not require invalidation.
    void Invalidate() noexcept { Generation = Revision = StructureRevision = ValuesRevision = BindingsRevision = 0; }
};

struct MaterialRevisions {
    uint64_t Revision{1};
    uint64_t StructureRevision{1};
    uint64_t ValuesRevision{1};
    uint64_t BindingsRevision{1};
};

struct MaterialObservationStats {
    uint64_t LegacyMaterialsObserved{0};
    uint64_t LegacyBytesCompared{0};
    uint64_t PendingResourcesObserved{0};
    uint64_t TrackedChanges{0};
};

enum class MaterialDirtyKind : uint8_t {
    Values = 1,
    Bindings = 2,
    Structure = 4,
    Observe = 8,
    Removed = 16,
};
template <>
struct is_flags<MaterialDirtyKind> : std::true_type {};
using MaterialDirtyFlags = EnumFlags<MaterialDirtyKind>;
inline auto format_as(MaterialDirtyKind value) { return EnumFlagsName(value); }

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
    /// GT: observe legacy writes and pending resources at most once per global frame epoch.
    MaterialRevisions GetRevisions(uint64_t epoch) const noexcept;
    const MaterialObservationStats& GetObservationStats() const noexcept { return _observationStats; }
    bool HasEscapedWrites() const noexcept { return _numericEscaped || _pipelineStateEscaped; }
    bool HasPendingResources() const noexcept;
    using ChangeCallback = void (*)(void*, Material&, MaterialDirtyFlags) noexcept;
    /// GT subscriptions must be removed before context destruction. Removal is notified during destruction.
    void AddChangeListener(void* context, ChangeCallback callback);
    void RemoveChangeListener(void* context) noexcept;
    void RetainAssetReferences(vector<StreamingAssetRefAny>& out) const;

    MaterialPipelineState& GetPipelineState() noexcept {
        MarkEscaped(_pipelineStateEscaped);
        return _pipelineStates[_technique->GetPrimaryPassIndex()];
    }
    const MaterialPipelineState& GetPipelineState() const noexcept { return _pipelineStates[_technique->GetPrimaryPassIndex()]; }
    bool SetPassPipelineState(std::string_view pass, const MaterialPipelineState& state) noexcept;
    RenderQueue GetRenderQueue() const noexcept { return _renderQueue; }
    void SetRenderQueue(RenderQueue value) noexcept;

    const ShaderParameterStorage& GetParameterStorage() const noexcept { return _parameters; }
    std::span<byte> NumericBytes() noexcept {
        MarkEscaped(_numericEscaped);
        return _numericBytes;
    }
    std::span<const byte> NumericBytes() const noexcept { return _numericBytes; }

    /// The GPU-layout bytes seen as the POD generated for this material's cbuffer. The caller
    /// asserts the type matches the shader ABI; nothing is validated here. Writes are picked up by
    /// GetRevision comparing the bytes, so no explicit change notification is needed. Techniques
    /// without a material cbuffer (DepthOnly) hold no bytes and must not be viewed.
    template <class T>
    T* As() noexcept {
        return AsCBuffer<T>(NumericBytes());
    }
    template <class T>
    const T* As() const noexcept {
        return AsCBuffer<T>(NumericBytes());
    }

    /// Copies a complete GPU-layout value without exposing writable storage. Equal data is a no-op.
    bool SetNumericData(std::span<const byte> value) noexcept;
    template <class T>
    bool SetNumeric(const T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        return SetNumericData(std::as_bytes(std::span{&value, size_t{1}}));
    }

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
                         Nullable<uint64_t*> bytesCopied = nullptr,
                         std::optional<uint64_t> observationEpoch = std::nullopt) const;

private:
    struct ResourceState;

    explicit Material(const MaterialTechnique* technique);
    string CanonicalName(std::string_view name) const;
    enum class ChangeKind { Values,
                            Bindings,
                            Structure };
    void MarkChanged(ChangeKind kind, bool tracked = true) const noexcept;
    void ObserveChanges() const noexcept;
    void MarkEscaped(bool& flag) noexcept;
    void NotifyChanged(MaterialDirtyFlags flags) const noexcept;
    bool SetNumericBytes(std::string_view name, ShaderParameterKind kind,
                         std::span<const byte> value, uint32_t element) noexcept;

    Nullable<const ShaderParameterInfo*> FindNumericParameter(
        std::string_view name,
        ShaderParameterKind kind) const noexcept;

    const MaterialTechnique* _technique;
    ShaderProgram* _program;
    uint32_t _parameterGroup;
    vector<byte> _numericBytes;
    mutable vector<byte> _observedNumericBytes;
    ShaderParameterStorage _parameters;
    vector<MaterialPipelineState> _pipelineStates;
    mutable vector<MaterialPipelineState> _observedPipelineStates;
    uint64_t _generation{0};
    mutable MaterialRevisions _revisions;
    mutable MaterialRevisions _epochRevisions;
    mutable std::optional<uint64_t> _observedEpoch;
    mutable MaterialObservationStats _observationStats;
    bool _numericEscaped{false};
    bool _pipelineStateEscaped{false};
    struct Listener {
        void* Context;
        ChangeCallback Callback;
    };
    vector<Listener> _changeListeners;
    RenderQueue _renderQueue{RenderQueue::Geometry};
    unique_ptr<ResourceState> _resources;
};

}  // namespace radray

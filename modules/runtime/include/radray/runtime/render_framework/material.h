#pragma once
#include <radray/runtime/shader_program.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/render_framework/instance_parameters.h>
#include <radray/runtime/render_framework/material_gpu_state.h>

namespace radray {
class RenderSystem;
struct RasterMaterialPlan;
enum class MaterialInputSource : uint8_t { SceneObjects,
                                           ViewConstants,
                                           MaterialConstants,
                                           Texture,
                                           Sampler,
                                           FrameConstants,
                                           InstanceParameters,
                                           VisibleInstanceSlots };
enum class MaterialInstanceAddressing : uint8_t { DirectObject,
                                                  VisibleSlots };
struct MaterialInput {
    string Name;
    MaterialInputSource Source;
    uint32_t ValueIndex{0};
    friend bool operator==(const MaterialInput&, const MaterialInput&) = default;
};
struct MaterialPass {
    MaterialPass() = default;
    /// A published pass shares one immutable definition; defaults remain per Material.
    explicit MaterialPass(shared_ptr<const MaterialPass> definition);
    const MaterialPass& GetDefinition() const noexcept;
    string Name;
    ShaderProgramRequest Program;
    vector<MaterialInput> Inputs;
    string ObjectIndexPushConstant;
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    std::optional<render::BlendState> Blend;
    render::ColorWrites WriteMask{render::ColorWrite::All};
    /// Validated, device-scoped program registered through RenderSystem's artifact API.
    /// Its service generation must outlive this material and all prepared draws.
    Nullable<ShaderProgram*> RegisteredProgram{nullptr};
    std::optional<render::DepthStencilState> DepthStencil;
    render::MultiSampleState Samples{render::MultiSampleState::Default()};
    /// General reflected push block. Mutually exclusive with ObjectIndexPushConstant.
    string PushConstant;
    vector<InstanceParameterLayout> ParameterLayouts;
    MaterialInstanceAddressing InstanceAddressing{MaterialInstanceAddressing::DirectObject};
    bool AllowAutomaticInstancing{false};
    /// Reflected uint field inside PushConstant, written by SceneDraw as object slot or visible base.
    string SceneIndexField;
    vector<byte> ScenePushValues;
    /// Preserve scene order even when the pass has no blending (for example an order-sensitive stencil pass).
    bool PreserveOrder{false};
    Nullable<ShaderProgram*> ResolveProgram(RenderSystem& renderer) const;

private:
    friend class RasterDraw;
    shared_ptr<const MaterialPass> _definition;
    mutable shared_ptr<RasterMaterialPlan> _nativePlan;
};
struct MaterialRenderData {
    vector<MaterialPass> Passes;
    vector<vector<byte>> Constants;
    vector<render::TextureView*> Textures;
    vector<render::SamplerDescriptor> Samplers;
};
struct MaterialBindingView {
    const MaterialRenderData& Data;
    MaterialGpuState& Gpu;
};
/// Immutable after publication. Replacing parameters creates a new asset/version.
class Material final : public Asset {
public:
    Material(vector<MaterialPass> passes, vector<vector<byte>> constants,
             vector<StreamingAssetRef<TextureAsset>> textures = {}, vector<render::SamplerDescriptor> samplers = {});
    ~Material() noexcept override;
    void OnUnload(AssetManager& manager) override;
    bool IsValid() const noexcept { return _valid; }
    const MaterialRenderData& GetRenderData() const noexcept { return _data; }
    MaterialGpuState& GetGpuState() const noexcept { return _gpuState; }

private:
    vector<StreamingAssetRef<TextureAsset>> _textures;
    MaterialRenderData _data;
    mutable MaterialGpuState _gpuState;
    bool _valid{false};
};
struct MaterialBinding {
    AssetId Id;
    Nullable<const MaterialRenderData*> Data;
    Nullable<MaterialGpuState*> Gpu{nullptr};
    friend bool operator==(const MaterialBinding& a, const MaterialBinding& b) noexcept {
        return a.Id == b.Id && a.Data.Get() == b.Data.Get() && a.Gpu.Get() == b.Gpu.Get();
    }
};
/// Shared immutable section bindings; all copies borrow the same material versions.
class MaterialBindings {
public:
    MaterialBindings() = default;
    MaterialBindings(vector<MaterialBinding> values);
    MaterialBindings(std::initializer_list<MaterialBinding> values);
    size_t size() const noexcept { return _values ? _values->size() : 0; }
    bool empty() const noexcept { return size() == 0; }
    const MaterialBinding* data() const noexcept { return _values ? _values->data() : nullptr; }
    const MaterialBinding* begin() const noexcept { return data(); }
    const MaterialBinding* end() const noexcept { return _values ? _values->data() + _values->size() : nullptr; }
    const MaterialBinding& operator[](size_t index) const noexcept { return (*_values)[index]; }
    operator std::span<const MaterialBinding>() const noexcept { return {data(), size()}; }

private:
    shared_ptr<const vector<MaterialBinding>> _values;
};
template <>
struct RuntimeTypeTrait<Material> {
    static constexpr RuntimeTypeId value{0x9a054ef2, 0x72ca, 0x4a19, 0xb1, 0x84, 0x46, 0x31, 0x29, 0x05, 0x79, 0x39};
};
}  // namespace radray

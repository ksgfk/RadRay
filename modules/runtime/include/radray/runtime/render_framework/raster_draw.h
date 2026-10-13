#pragma once
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/vertex_layout.h>

namespace radray {
struct RasterFrameBindingCache;
/// Cold reflection result. Owned by RasterDraw, borrows a RenderSystem program generation.
struct RasterMaterialPlan {
    struct Group {
        uint32_t Index;
        bool Static;
        vector<size_t> Inputs;
    };
    shared_ptr<RenderServiceGeneration> Generation;
    ShaderProgram* Program;
    vector<MaterialInput> Inputs;
    vector<render::BindingHandle> Handles;
    vector<render::ShaderBindingInfo> Bindings;
    vector<uint32_t> BufferSizes;
    vector<InstanceParameterLayout> ParameterLayouts;
    string PushName;
    render::BindingHandle Push;
    uint32_t PushSize{0};
    bool LegacyObjectIndexPush{false};
    vector<Group> Groups;
    MaterialInstanceAddressing InstanceAddressing{MaterialInstanceAddressing::DirectObject};
    string SceneIndexField;
    std::optional<uint32_t> SceneIndexOffset;
};
/// Values are borrowed only through PrepareMaterial. Empty overrides use material defaults.
struct RasterMaterialValues {
    std::span<const std::span<const byte>> Constants{};
    std::span<render::TextureView* const> Textures{};
    std::span<const render::SamplerDescriptor> Samplers{};
    std::span<const std::span<const byte>> Views{};
    std::span<const std::span<const byte>> Frames{};
    std::optional<render::ShaderBufferBinding> Objects{};
    std::optional<render::ShaderBufferBinding> VisibleSlots{};
    std::span<const InstanceParameterBuffer> Parameters{};
    /// Already committed constant slices, borrowed through this flight's completion.
    /// A nonempty source overrides its corresponding CPU byte source.
    std::span<const render::ShaderBufferBinding> ConstantBuffers{}, ViewBuffers{}, FrameBuffers{};
    Nullable<RasterFrameBindingCache*> SharedGroups{nullptr};
};
struct PreparedRasterMaterial {
    struct Group {
        uint32_t Index;
        render::ShaderParameterSet* Parameters;
        vector<render::ShaderParameterDynamicOffset> Offsets;
    };
    vector<Group> Groups;
    render::BindingHandle Push;
    uint32_t PushSize{0};
};
/// Optional reuse domain for one Prepare scope. Only scene/view/frame buffer groups participate.
struct RasterFrameBindingCache {
    struct Entry {
        vector<render::ShaderBufferBinding> Values;
        PreparedRasterMaterial::Group Group;
    };
    unordered_map<const RasterMaterialPlan*, unordered_map<uint32_t, vector<Entry>>> Groups;
};
/// Execution slices only; no Scene or producer storage is retained.
struct PreparedRasterDraw {
    render::GraphicsPipelineState* Pipeline;
    vector<render::VertexBufferBinding> Vertices;
    std::optional<render::IndexBufferView> Indices;
    uint32_t Count{0}, First{0}, Instances{1}, FirstInstance{0};
    int32_t VertexOffset{0};
    radray::Viewport Viewport;
    radray::Rect Scissor;
    vector<byte> Push;
    void Record(render::GraphicsCommandEncoder* encoder, const PreparedRasterMaterial& material) const;
};
class RasterDraw {
public:
    Nullable<const RasterMaterialPlan*> ResolveMaterial(RenderSystem& renderer, const MaterialPass& pass);
    std::optional<PreparedRasterMaterial> PrepareMaterial(PipelineContext& context, const RasterMaterialPlan& plan,
                                                          const MaterialRenderData& material, const RasterMaterialValues& values = {}, Nullable<MaterialGpuState*> gpu = nullptr, Nullable<const MaterialPass*> source = nullptr);
    std::optional<PreparedRasterMaterial> PrepareMaterial(PipelineContext& context, const RasterMaterialPlan& plan,
                                                          MaterialBindingView binding, const RasterMaterialValues& values = {});
    Nullable<render::GraphicsPipelineState*> PreparePipeline(PipelineContext& context, const RasterMaterialPlan& plan,
                                                             const MaterialPass& material, const GeometryVertexLayout& geometry, render::PrimitiveTopology topology,
                                                             render::RenderPass* pass, const RenderOutputInfo& output, bool reverseCulling = false);

private:
    vector<shared_ptr<RasterMaterialPlan>> _plans;
};
struct VisibleInstanceRange {
    uint32_t First{0}, Count{0};
};
enum class RasterInstancePrepareStatus : uint8_t { Prepared,
                                                   NoWork,
                                                   Unsupported,
                                                   InvalidData,
                                                   AllocationFailure };
struct RasterFrameParameters {
    const InstanceParameterLayout& Layout;
    std::span<const byte> Values;
};
struct RasterInstanceData {
    RasterInstanceData() = default;
    RasterInstanceData(const RasterInstanceData& other);
    RasterInstanceData& operator=(const RasterInstanceData& other);
    RasterInstanceData(RasterInstanceData&& other) noexcept = default;
    RasterInstanceData& operator=(RasterInstanceData&& other) noexcept = default;
    render::ShaderBufferBinding Objects{}, VisibleSlots{};
    vector<InstanceParameterLayout> Layouts;
    vector<InstanceParameterBuffer> Parameters;
    VisibleInstanceRange Visible;
};
struct RasterInstancePreparation {
    RasterInstancePrepareStatus Status;
    std::optional<RasterInstanceData> Data;
};
/// CPU input is copied into flight storage; source assets/external buffers stay owned through GPU completion.
RasterInstancePreparation PrepareRasterInstances(PipelineContext& context, const RasterMaterialPlan& plan,
                                                 std::span<const Eigen::Matrix4f> matrices, std::span<const uint32_t> slots, std::span<const RasterFrameParameters> parameters = {});
RasterInstancePreparation PrepareRasterInstances(PipelineContext& context, const RasterMaterialPlan& plan,
                                                 render::ShaderBufferBinding objects, std::span<const uint32_t> slots, std::span<const InstanceParameterBuffer> parameters = {});
}  // namespace radray

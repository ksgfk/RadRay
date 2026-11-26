#pragma once

#include <filesystem>
#include <optional>
#include <radray/runtime/scene.h>
#include <radray/runtime/frame_slot.h>
#include <radray/shader/client.h>
#include <radray/uri.h>
#include <radray/image_data.h>
#include <span>
#include <string_view>

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/runtime/asset_handle.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/runtime/material_tag.h>
#include <radray/types.h>
#include <radray/runtime/gpu_env.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/vertex_input.h>
#include <radray/runtime/material.h>
#include <radray/runtime/pipeline_layout_registry.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/shader_asset.h>

namespace radray::examples::mesh_drawing {

struct PassConstants {
    array<float, 16> ViewProjection{};
    float LocalScale{1.0f};
    array<float, 3> Padding{};
};

struct InstanceData {
    array<float, 16> LocalToWorld{};
};

struct MaterialConstants {
    array<float, 4> Color{};
};

static_assert(sizeof(PassConstants) == 80);
static_assert(sizeof(InstanceData) == 64);
static_assert(sizeof(MaterialConstants) == 16);

// ID 只在这张应用拥有的表中有意义；Processor 不按字符串查标签。
struct MaterialTagFilter {
    const MaterialTagTable* Table;
    MaterialTag Required;
    std::string_view Description;
};

// 同一材质资源仍需结合顶点输入、Pass 附件和深度状态生成不同 PSO。
struct MaterialPipelines {
    AssetHandle<MaterialAsset> Material;
    shared_ptr<GpuPipelineLayout> Layout;
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> Depth{nullptr};
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> Color{nullptr};
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> Outline{nullptr};
    uint32_t AppliedShaderRevision{};
    uint32_t AppliedMaterialRevision{};
    uint32_t CompatibleShaderRevision{};
    uint32_t CompatibleMaterialRevision{};
};

// 资源按依赖顺序声明，析构时逆序释放；仅供本教学样例使用。
struct GpuResources {
    GpuResources() noexcept = default;
    GpuResources(const GpuResources&) = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    bool Initialize(gpu::GpuBackend backend, const std::filesystem::path& shaderDirectory, Nullable<const shader::Client*> compiler = nullptr);
    shader::DiagnosticResult<ShaderAsset> CompileCube(const shader::Client& compiler, const Uri& uri, bool swapColors);
    std::optional<MaterialPipelines> PreparePipelines(const VertexInput& input, AssetHandle<MaterialAsset> material);
    void SyncPipelines(const VertexInput& input, std::span<const AssetReloadEvent> events,
                       Scene& scene, std::span<const MaterialProxyHandle> proxies, FrameSlot& slot);
    // Creates Device buffers and retains CPU bytes for the first frame's graph upload.
    bool PrepareGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices);
    bool CreatePipelines(const VertexInput& input, AssetHandle<MaterialAsset> material);

    uint32_t Width{800};
    uint32_t Height{600};
    Nullable<unique_ptr<Environment>> Env{nullptr};
    Nullable<unique_ptr<DeviceContext>> Device{nullptr};
    Nullable<unique_ptr<gpu::Buffer>> VertexBuffer{nullptr};
    Nullable<unique_ptr<gpu::Buffer>> IndexBuffer{nullptr};
    vector<byte> PendingVertexBytes;
    vector<byte> PendingIndexBytes;
    MaterialTagTable MaterialTags;
    unique_ptr<PipelineLayoutRegistry> LayoutRegistry;
    unique_ptr<AssetManager> Assets;
    AssetRef<ShaderAsset> Shader;
    Nullable<shared_ptr<gpu::BindingGroupLayout>> PassGroupLayout{nullptr};
    MaterialTag OpaqueTag;
    MaterialTag TransparentTag;
    MaterialTag OutlineTag;
    AssetRef<MaterialAsset> OpaqueMaterial;
    AssetRef<MaterialAsset> TwoSidedMaterial;
    AssetRef<MaterialAsset> TransparentMaterial;
    vector<MaterialPipelines> Pipelines;
};

// Each slot owns distinct writable resources; release only after its allocator has reset.
struct GpuFrameResources {
    bool Initialize(const GpuResources& shared);
    // Requires completion of the latest frame recorded into this slot.
    std::optional<ImageData> ReadImage() const;

    uint32_t Width{0};
    uint32_t Height{0};
    uint64_t RowPitch{0};
    Nullable<unique_ptr<gpu::Buffer>> Readback{nullptr};
    shared_ptr<gpu::BindingGroupLayout> PassGroupLayout;
    Nullable<unique_ptr<gpu::Fence>> CompletionFence{nullptr};
    uint64_t CompletionValue{0};
};

bool SyncMaterialProxies(const AssetManager& assets, Scene& scene, std::span<const MaterialInstance> instances,
                         std::span<const MaterialProxyHandle> proxies, std::span<uint32_t> appliedRevisions);

bool PrepareOutputDirectory(const std::filesystem::path& output);

}  // namespace radray::examples::mesh_drawing

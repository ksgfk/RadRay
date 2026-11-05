#pragma once

#include <filesystem>
#include <span>
#include <string_view>

#include <Eigen/Core>

#include <radray/runtime/gpu_env.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/vertex_input.h>
#include <radray/runtime/material.h>

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
    const MaterialResource* Resource;
    Nullable<unique_ptr<render::GraphicsPipelineState>> Depth{nullptr};
    Nullable<unique_ptr<render::GraphicsPipelineState>> Color{nullptr};
    Nullable<unique_ptr<render::GraphicsPipelineState>> Outline{nullptr};
};

// 资源按依赖顺序声明，析构时逆序释放；仅供本教学样例使用。
struct GpuResources {
    GpuResources() noexcept = default;
    GpuResources(const GpuResources&) = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    bool Initialize(render::RenderBackend backend, const std::filesystem::path& shaderDirectory);
    // Creates Device buffers and retains CPU bytes for the first frame's graph upload.
    bool PrepareGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices);
    bool CreatePipelines(const VertexInput& input, const MaterialResource& resource);

    uint32_t Width{800};
    uint32_t Height{600};
    Nullable<unique_ptr<Environment>> Env{nullptr};
    Nullable<unique_ptr<DeviceContext>> Device{nullptr};
    Nullable<unique_ptr<render::Buffer>> VertexBuffer{nullptr};
    Nullable<unique_ptr<render::Buffer>> IndexBuffer{nullptr};
    vector<byte> PendingVertexBytes;
    vector<byte> PendingIndexBytes;
    Nullable<unique_ptr<render::Shader>> VertexShader{nullptr};
    Nullable<unique_ptr<render::Shader>> PixelShader{nullptr};
    Nullable<unique_ptr<render::BindingGroupLayout>> PassGroupLayout{nullptr};
    Nullable<unique_ptr<render::PipelineLayout>> Layout{nullptr};
    MaterialTagTable MaterialTags;
    MaterialTag OpaqueTag;
    MaterialTag TransparentTag;
    MaterialTag OutlineTag;
    MaterialResource OpaqueResource;
    MaterialResource TwoSidedResource;
    MaterialResource TransparentResource;
    vector<MaterialPipelines> Pipelines;
};

// Each slot owns distinct writable resources; release only after its allocator has reset.
struct GpuFrameResources {
    bool Initialize(const GpuResources& shared, uint32_t instanceCount);
    // Requires the corresponding completion fence; saves the latest frame recorded into this slot.
    bool SaveImage(const std::filesystem::path& output) const;

    uint32_t Width{0};
    uint32_t Height{0};
    uint64_t RowPitch{0};
    Nullable<unique_ptr<render::Buffer>> InstanceBuffer{nullptr};
    array<Nullable<unique_ptr<render::Buffer>>, 2> PassBuffers{};
    Nullable<unique_ptr<render::Buffer>> Readback{nullptr};
    array<Nullable<unique_ptr<render::ShaderParameterSet>>, 2> PassParameterSets{};
    Nullable<unique_ptr<render::Fence>> CompletionFence{nullptr};
    uint64_t CompletionValue{0};
};

bool PrepareOutputDirectory(const std::filesystem::path& output);

}  // namespace radray::examples::mesh_drawing
